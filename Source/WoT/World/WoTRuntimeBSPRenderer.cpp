#include "WoTRuntimeBSPRenderer.h"
#include "../WoTDynamicLightSubsystem.h"

#include "../Assets/WoTRuntimeAssets.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../WoTFractalData.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTPayloadSubsystem.h"

#include "../WoTLightWave.h"
#include "../WoTPresentationPolicy.h"
#include "../WoTSkyBox.h"
#include "WoTRuntimeMapSky.h"
#include "WoTRetailReader/Generated/WorldTextures.generated.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "RHI.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/BodySetup.h"
#include "HAL/PlatformTime.h"

namespace
{
// Payload coordinates are float32: at 65536 units the spacing is 0.0078125.
// A 0.01-unit tolerance absorbs that encoding/projection rounding while
// retaining authored UV/material seams (only host attributes are interpolated).
constexpr double BSPJunctionEpsilon = 0.01;
constexpr double BSPJunctionNearCoincidentGap = 0.5;
constexpr double BSPJunctionNearCoincidentDirectionDot = 0.9999;
constexpr double BSPJunctionNearCoincidentNormalDot = 0.9999;
constexpr double BSPJunctionNearCoincidentMinLength = 4.0;
constexpr double BSPLightmapRibbonWidth = 0.5;
constexpr double BSPLightmapRibbonMinLength = 4.0;
constexpr double BSPLightmapRibbonNormalDot = 0.9999;
constexpr double BSPLightmapRibbonContrast = 4.0;
constexpr double BSPLightmapRibbonMinNeighborLuminance = 16.0;
// UE1's fixed fan can leave one long, locally thinnest triangle between
// otherwise healthy coplanar neighbours. At high resolution that individual
// face can disappear as a black wedge. Only index-diagonal exchange is used:
// the exact convex quad, affine attributes and source vertices stay unchanged.
constexpr double BSPFanSliverMaximumQuality = 0.22;
constexpr double BSPFanSliverMinimumSpan = 8.0;
constexpr double BSPFanSliverMaximumAltitude = 6.1;
constexpr double BSPFanSliverMaximumRimChord = 10.25;
constexpr double BSPFanSliverNeedleRimRatio = 0.02;
constexpr double BSPFanSliverNeedleRadialEdgeRatio = 0.99;
constexpr double BSPFanSliverMinimumChildQuality = 0.01;
constexpr double BSPFanSliverMinimumRetainedQualityRatio = 0.75;
constexpr float BSPFanSliverAttributeEpsilon = 0.0001f;
constexpr double BSPJunctionCellSize = 128.0;
constexpr uint64 BSPJunctionMaxInputVertices = 4000000;
constexpr uint64 BSPJunctionMaxOutputVertices = 8000000;
constexpr uint64 BSPJunctionMaxOutputIndices = 24000000;
constexpr uint64 BSPJunctionMaxCandidateChecks = 100000000;
constexpr uint64 BSPJunctionMaxCellVisits = 20000000;
constexpr int32 BSPJunctionMaxClosurePasses = 16;

FIntVector BSPJunctionCell(const FVector3d& Position)
{
	return FIntVector(FMath::FloorToInt(Position.X / BSPJunctionCellSize),
		FMath::FloorToInt(Position.Y / BSPJunctionCellSize),
		FMath::FloorToInt(Position.Z / BSPJunctionCellSize));
}

struct FBSPJunctionCandidate
{
	double Parameter = 0;
	int32 Vertex = 0;
	FVector3f SourcePosition = FVector3f::ZeroVector;
	bool bNearCoincident = false;
};

struct FBSPJunctionBoundaryPoint
{
	uint32 Vertex = 0;
	// Original source-triangle edge whose outgoing boundary segment follows.
	// INDEX_NONE marks a watertight/internal edge that closure must not probe.
	int32 SourceEdge = INDEX_NONE;

	FBSPJunctionBoundaryPoint() = default;
	FBSPJunctionBoundaryPoint(uint32 InVertex, int32 InSourceEdge)
		: Vertex(InVertex), SourceEdge(InSourceEdge) {}
	operator uint32() const { return Vertex; }
};

struct FBSPJunctionOpenEdge
{
	uint32 PointA = 0;
	uint32 PointB = 0;
	uint32 ThirdPoint = 0;
	uint32 Surface = MAX_uint32;
	FVector3d Normal = FVector3d::ZeroVector;
};

struct FBSPLightmapRibbonSurface
{
	FVector3d Normal = FVector3d::ZeroVector;
	double MaximumMinimumAltitude = 0.0;
	double MaximumEdgeLength = 0.0;
	uint32 TriangleCount = 0;
	bool bConsistentNormal = true;
	TArray<int32> TriangleOffsets;
	TArray<int32> SectionIndices;
};

struct FBSPLightmapRibbonEdge
{
	int32 Part = INDEX_NONE;
	uint32 VertexA = 0;
	uint32 VertexB = 0;
	uint32 Surface = 0;
	FVector3d Normal = FVector3d::ZeroVector;
};

struct FBSPFanSliverIncident
{
	int32 TriangleOffset = INDEX_NONE;
	int32 Edge = INDEX_NONE;
	int32 Section = INDEX_NONE;
};

struct FBSPFanSliverFlip
{
	int32 TargetOffset = INDEX_NONE;
	int32 NeighbourOffset = INDEX_NONE;
	uint32 Indices[6]{};
	double TargetQuality = 0;
	double NewMinimumQuality = 0;
	double NewDiagonalLength = 0;
};

uint64 BSPLightmapRibbonSurfaceKey(int32 Part, uint32 Surface)
{
	return (uint64(uint32(Part)) << 32) | Surface;
}

uint64 BSPJunctionEdgeKey(uint32 A, uint32 B)
{
	return (uint64(FMath::Min(A, B)) << 32) | FMath::Max(A, B);
}
}

bool WoTRuntimeBSPRepairRenderTJunctions(
	TArray<FWoTBSPPayloadData>& RenderParts,
	FWoTBSPTJunctionStats& OutStats, FString& OutError)
{
	OutStats = {};
	OutError.Reset();
	const double Started = FPlatformTime::Seconds();
	const auto FailRepair = [&OutError, &OutStats, Started](const FString& Reason)
	{
		OutError = TEXT("runtime BSP render T-junction repair: ") + Reason;
		// The transaction never publishes WorkingParts on failure. Preserve
		// attempted-work diagnostics, but do not report discarded ribbon UVs as
		// applied repairs in the materialization receipt or heartbeat.
		OutStats.RepairedLightmapRibbonSurfaces = 0;
		OutStats.RepairedLightmapRibbonTriangles = 0;
		OutStats.RepairedLightmapRibbonVertices = 0;
		OutStats.MaxRepairedLightmapRibbonWidth = 0;
		OutStats.RepairedNearCoincidentEdges = 0;
		OutStats.RetessellatedFanSliverPairs = 0;
		OutStats.SkippedNearCoincidentTriangles = 0;
		OutStats.MaxRepairedNearCoincidentGap = 0;
		OutStats.RepairMilliseconds = (FPlatformTime::Seconds() - Started) * 1000;
		return false;
	};
	if (RenderParts.Num() > 4096)
	{
		return FailRepair(TEXT("part count exceeds 4096; partition the render request"));
	}
	uint64 TotalVertices = 0;
	uint64 TotalIndices = 0;
	for (int32 PartIndex = 0; PartIndex < RenderParts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = RenderParts[PartIndex];
		TotalVertices += Part.Positions.Num();
		TotalIndices += Part.Indices.Num();
		if (TotalVertices > BSPJunctionMaxInputVertices
			|| TotalIndices > BSPJunctionMaxOutputIndices)
		{
			return FailRepair(TEXT("input exceeds 4000000 vertices or 24000000 indices; partition the render request"));
		}
		if (Part.Normals.Num() != Part.Positions.Num()
			|| Part.UV0.Num() != Part.Positions.Num()
			|| Part.UV1.Num() != Part.Positions.Num()
			|| Part.Indices.Num() % 3 != 0
			|| (!Part.TriangleSurfaces.IsEmpty()
				&& Part.TriangleSurfaces.Num() != Part.Indices.Num() / 3))
		{
			return FailRepair(FString::Printf(TEXT("part %d has incomplete vertex attributes, triangles, or surface provenance"), PartIndex));
		}
		for (int32 Vertex = 0; Vertex < Part.Positions.Num(); ++Vertex)
		{
			const FVector3f& Position = Part.Positions[Vertex];
			const FVector3f& Normal = Part.Normals[Vertex];
			const FVector2f& TextureUV = Part.UV0[Vertex];
			const FVector2f& LightmapUV = Part.UV1[Vertex];
			if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
				|| !FMath::IsFinite(Position.Z) || Position.GetAbsMax() > 1000000000.0f
				|| !FMath::IsFinite(Normal.X) || !FMath::IsFinite(Normal.Y)
				|| !FMath::IsFinite(Normal.Z)
				|| !FMath::IsFinite(TextureUV.X) || !FMath::IsFinite(TextureUV.Y)
				|| !FMath::IsFinite(LightmapUV.X) || !FMath::IsFinite(LightmapUV.Y))
			{
				return FailRepair(FString::Printf(TEXT("part %d vertex %d has nonfinite attributes or coordinates outside +/-1000000000"), PartIndex, Vertex));
			}
		}
		uint64 CoveredIndices = 0;
		for (const FWoTBSPPayloadSection& Section : Part.Sections)
		{
			if (Section.FirstIndex != CoveredIndices || Section.IndexCount % 3 != 0
				|| CoveredIndices + Section.IndexCount > static_cast<uint64>(Part.Indices.Num()))
			{
				return FailRepair(FString::Printf(TEXT("part %d sections do not form contiguous complete triangle ranges"), PartIndex));
			}
			CoveredIndices += Section.IndexCount;
		}
		if (CoveredIndices != static_cast<uint64>(Part.Indices.Num()))
		{
			return FailRepair(FString::Printf(TEXT("part %d sections do not cover all indices"), PartIndex));
		}
		for (uint32 Vertex : Part.Indices)
		{
			if (Vertex >= static_cast<uint32>(Part.Positions.Num()))
			{
				return FailRepair(FString::Printf(TEXT("part %d has an out-of-range vertex index"), PartIndex));
			}
		}
		OutStats.InputTriangles += Part.Indices.Num() / 3;
	}
	// All repairs are transactional. The authenticated input remains untouched
	// on success as well as failure; callers retain it for collision.
	TArray<FWoTBSPPayloadData> WorkingParts = RenderParts;

	const auto RetessellateFanSlivers = [&](TArray<FWoTBSPPayloadData>& Parts)
	{
		// Retail bsp2obj triangulates every BSP node as a fixed fan. Exact
		// adjacent nodes can therefore surround a long, locally thinnest fan
		// face even when every edge is incidence two and no T-junction donor
		// exists. Exchange that face's best shared diagonal only inside one
		// part/section/surface. The two source triangles must form a convex
		// coplanar quad and their normals/UVs must be one affine mapping, so
		// this changes only its render tessellation.
		for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
		{
			FWoTBSPPayloadData& Part = Parts[PartIndex];
			if (Part.TriangleSurfaces.IsEmpty()) continue;
			TMap<FVector3f, uint32> PointIds;
			for (uint32 Vertex : Part.Indices)
				if (!PointIds.Contains(Part.Positions[Vertex]))
					PointIds.Add(Part.Positions[Vertex], PointIds.Num());
			TMap<uint64, TArray<FBSPFanSliverIncident>> Incidents;
			for (int32 SectionIndex = 0; SectionIndex < Part.Sections.Num(); ++SectionIndex)
			{
				const FWoTBSPPayloadSection& Section = Part.Sections[SectionIndex];
				const int32 End = int32(Section.FirstIndex + Section.IndexCount);
				for (int32 At = int32(Section.FirstIndex); At < End; At += 3)
				{
					const FVector3d P0(Part.Positions[Part.Indices[At]]);
					const FVector3d P1(Part.Positions[Part.Indices[At + 1]]);
					const FVector3d P2(Part.Positions[Part.Indices[At + 2]]);
					if (FVector3d::CrossProduct(P1 - P0, P2 - P0).SizeSquared() == 0) continue;
					for (int32 Edge = 0; Edge < 3; ++Edge)
					{
						const uint32 A =
							PointIds.FindChecked(Part.Positions[Part.Indices[At + Edge]]);
						const uint32 B =
							PointIds.FindChecked(Part.Positions[Part.Indices[At + (Edge + 1) % 3]]);
						Incidents.FindOrAdd(BSPJunctionEdgeKey(A, B)).Add({At, Edge, SectionIndex});
					}
				}
			}
			const auto TriangleQuality = [&Part](uint32 A, uint32 B, uint32 C,
												 double* OutLongest = nullptr,
												 double* OutShortest = nullptr)
			{
				const FVector3d PA(Part.Positions[A]);
				const FVector3d PB(Part.Positions[B]);
				const FVector3d PC(Part.Positions[C]);
				const double AB = (PB - PA).Size();
				const double BC = (PC - PB).Size();
				const double CA = (PA - PC).Size();
				const double Longest = FMath::Max3(AB, BC, CA);
				if (OutLongest != nullptr) *OutLongest = Longest;
				if (OutShortest != nullptr) *OutShortest = FMath::Min3(AB, BC, CA);
				return Longest == 0 ? 0.0
									: FVector3d::CrossProduct(PB - PA, PC - PA).Size() /
										  FMath::Square(Longest);
			};
			const auto AttributesMatch = [&Part](uint32 A, uint32 B)
			{
				return Part.Normals[A].Equals(Part.Normals[B], BSPFanSliverAttributeEpsilon) &&
					   Part.UV0[A].Equals(Part.UV0[B], BSPFanSliverAttributeEpsilon) &&
					   Part.UV1[A].Equals(Part.UV1[B], BSPFanSliverAttributeEpsilon);
			};
			const auto AffineAttribute =
				[&Part](uint32 A, uint32 B, uint32 C, uint32 D, bool bLightmap)
			{
				const FVector3d PA(Part.Positions[A]);
				const FVector3d V0 = FVector3d(Part.Positions[B]) - PA;
				const FVector3d V1 = FVector3d(Part.Positions[C]) - PA;
				const FVector3d V2 = FVector3d(Part.Positions[D]) - PA;
				const double D00 = FVector3d::DotProduct(V0, V0);
				const double D01 = FVector3d::DotProduct(V0, V1);
				const double D11 = FVector3d::DotProduct(V1, V1);
				const double D20 = FVector3d::DotProduct(V2, V0);
				const double D21 = FVector3d::DotProduct(V2, V1);
				const double Denominator = D00 * D11 - D01 * D01;
				if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER) return false;
				const double WeightB = (D11 * D20 - D01 * D21) / Denominator;
				const double WeightC = (D00 * D21 - D01 * D20) / Denominator;
				const double WeightA = 1.0 - WeightB - WeightC;
				const FVector2d ValueA(bLightmap ? Part.UV1[A] : Part.UV0[A]);
				const FVector2d ValueB(bLightmap ? Part.UV1[B] : Part.UV0[B]);
				const FVector2d ValueC(bLightmap ? Part.UV1[C] : Part.UV0[C]);
				const FVector2d ValueD(bLightmap ? Part.UV1[D] : Part.UV0[D]);
				return (WeightA * ValueA + WeightB * ValueB + WeightC * ValueC)
					.Equals(ValueD, BSPFanSliverAttributeEpsilon);
			};
			TArray<FBSPFanSliverFlip> BestFlips;
			for (int32 SectionIndex = 0; SectionIndex < Part.Sections.Num(); ++SectionIndex)
			{
				const FWoTBSPPayloadSection& Section = Part.Sections[SectionIndex];
				const int32 End = int32(Section.FirstIndex + Section.IndexCount);
				for (int32 At = int32(Section.FirstIndex); At < End; At += 3)
				{
					double TargetLongest = 0;
					double TargetShortest = 0;
					const double TargetQuality =
						TriangleQuality(Part.Indices[At], Part.Indices[At + 1],
										Part.Indices[At + 2], &TargetLongest, &TargetShortest);
					if (TargetQuality <= 0 || TargetQuality >= BSPFanSliverMaximumQuality ||
						TargetLongest < BSPFanSliverMinimumSpan ||
						TargetQuality * TargetLongest > BSPFanSliverMaximumAltitude ||
						TargetShortest > BSPFanSliverMaximumRimChord)
						continue;
					FBSPFanSliverFlip Best;
					for (int32 Edge = 0; Edge < 3; ++Edge)
					{
						if (++OutStats.CandidateChecks > BSPJunctionMaxCandidateChecks)
							return FailRepair(TEXT("fan-sliver search exceeds 100000000 checks; "
												   "partition the render request"));
						const uint32 TargetA = Part.Indices[At + Edge];
						const uint32 TargetB = Part.Indices[At + (Edge + 1) % 3];
						const uint32 TargetOpposite = Part.Indices[At + (Edge + 2) % 3];
						const uint64 EdgeKey =
							BSPJunctionEdgeKey(PointIds.FindChecked(Part.Positions[TargetA]),
											   PointIds.FindChecked(Part.Positions[TargetB]));
						const TArray<FBSPFanSliverIncident>* EdgeIncidents =
							Incidents.Find(EdgeKey);
						if (EdgeIncidents == nullptr || EdgeIncidents->Num() != 2) continue;
						const FBSPFanSliverIncident& Neighbour =
							(*EdgeIncidents)[(*EdgeIncidents)[0].TriangleOffset == At ? 1 : 0];
						if (Neighbour.TriangleOffset == At || Neighbour.Section != SectionIndex ||
							Part.TriangleSurfaces[At / 3] !=
								Part.TriangleSurfaces[Neighbour.TriangleOffset / 3])
							continue;
						const int32 NeighbourAt = Neighbour.TriangleOffset;
						uint32 NeighbourA = Part.Indices[NeighbourAt + Neighbour.Edge];
						uint32 NeighbourB = Part.Indices[NeighbourAt + (Neighbour.Edge + 1) % 3];
						const uint32 NeighbourOpposite =
							Part.Indices[NeighbourAt + (Neighbour.Edge + 2) % 3];
						if (Part.Positions[NeighbourA] == Part.Positions[TargetB] &&
							Part.Positions[NeighbourB] == Part.Positions[TargetA])
							Swap(NeighbourA, NeighbourB);
						if (Part.Positions[NeighbourA] != Part.Positions[TargetA] ||
							Part.Positions[NeighbourB] != Part.Positions[TargetB] ||
							!AttributesMatch(TargetA, NeighbourA) ||
							!AttributesMatch(TargetB, NeighbourB))
							continue;
						const FVector3d A(Part.Positions[TargetA]);
						const FVector3d B(Part.Positions[TargetB]);
						const FVector3d C(Part.Positions[TargetOpposite]);
						const FVector3d D(Part.Positions[NeighbourOpposite]);
						const double OldDiagonalLength = (B - A).Size();
						const bool bNeedleRadialEdge =
							TargetShortest < TargetLongest * BSPFanSliverNeedleRimRatio &&
							OldDiagonalLength >= TargetLongest * BSPFanSliverNeedleRadialEdgeRatio;
						if (OldDiagonalLength + BSPJunctionEpsilon < TargetLongest &&
							!bNeedleRadialEdge)
							continue;
						if (A == B || A == C || A == D || B == C || B == D || C == D) continue;
						const FVector3d TargetCross = FVector3d::CrossProduct(B - A, C - A);
						const FVector3d NeighbourCross = FVector3d::CrossProduct(
							FVector3d(Part.Positions[Part.Indices[NeighbourAt + 1]]) -
								FVector3d(Part.Positions[Part.Indices[NeighbourAt]]),
							FVector3d(Part.Positions[Part.Indices[NeighbourAt + 2]]) -
								FVector3d(Part.Positions[Part.Indices[NeighbourAt]]));
						const FVector3d Normal = TargetCross.GetSafeNormal();
						if (Normal.IsNearlyZero() ||
							FVector3d::DotProduct(Normal, NeighbourCross.GetSafeNormal()) <
								BSPJunctionNearCoincidentNormalDot ||
							FMath::Abs(FVector3d::DotProduct(D - A, Normal)) > BSPJunctionEpsilon)
							continue;
						const double SideC =
							FVector3d::DotProduct(FVector3d::CrossProduct(B - A, C - A), Normal);
						const double SideD =
							FVector3d::DotProduct(FVector3d::CrossProduct(B - A, D - A), Normal);
						const double SideA =
							FVector3d::DotProduct(FVector3d::CrossProduct(D - C, A - C), Normal);
						const double SideB =
							FVector3d::DotProduct(FVector3d::CrossProduct(D - C, B - C), Normal);
						if (SideC * SideD >= 0 || SideA * SideB >= 0) continue;
						const double NeighbourQuality = TriangleQuality(
							Part.Indices[NeighbourAt], Part.Indices[NeighbourAt + 1],
							Part.Indices[NeighbourAt + 2]);
						if (NeighbourQuality <= TargetQuality) continue;
						if (!Part.Normals[TargetA].Equals(Part.Normals[TargetB],
														  BSPFanSliverAttributeEpsilon) ||
							!Part.Normals[TargetA].Equals(Part.Normals[TargetOpposite],
														  BSPFanSliverAttributeEpsilon) ||
							!Part.Normals[TargetA].Equals(Part.Normals[NeighbourOpposite],
														  BSPFanSliverAttributeEpsilon) ||
							!AffineAttribute(TargetA, TargetB, TargetOpposite, NeighbourOpposite,
											 false) ||
							!AffineAttribute(TargetA, TargetB, TargetOpposite, NeighbourOpposite,
											 true))
							continue;
						uint32 NewIndices[6] = {TargetOpposite,	   NeighbourOpposite, TargetA,
												NeighbourOpposite, TargetOpposite,	  TargetB};
						for (int32 Triangle = 0; Triangle < 2; ++Triangle)
						{
							const int32 Base = Triangle * 3;
							const FVector3d NewCross = FVector3d::CrossProduct(
								FVector3d(Part.Positions[NewIndices[Base + 1]]) -
									FVector3d(Part.Positions[NewIndices[Base]]),
								FVector3d(Part.Positions[NewIndices[Base + 2]]) -
									FVector3d(Part.Positions[NewIndices[Base]]));
							if (FVector3d::DotProduct(NewCross, TargetCross) < 0)
								Swap(NewIndices[Base + 1], NewIndices[Base + 2]);
						}
						const double NewMinimumQuality = FMath::Min(
							TriangleQuality(NewIndices[0], NewIndices[1], NewIndices[2]),
							TriangleQuality(NewIndices[3], NewIndices[4], NewIndices[5]));
						const double NewDiagonalLength = (D - C).Size();
						if (NewMinimumQuality < BSPFanSliverMinimumChildQuality ||
							(NewDiagonalLength + BSPJunctionEpsilon >= OldDiagonalLength &&
							 NewMinimumQuality <
								 TargetQuality * BSPFanSliverMinimumRetainedQualityRatio))
							continue;
						if (Best.TargetOffset != INDEX_NONE &&
							(NewMinimumQuality < Best.NewMinimumQuality ||
							 (NewMinimumQuality == Best.NewMinimumQuality &&
							  NewDiagonalLength >= Best.NewDiagonalLength)))
							continue;
						Best.TargetOffset = At;
						Best.NeighbourOffset = NeighbourAt;
						for (int32 Index = 0; Index < 6; ++Index)
							Best.Indices[Index] = NewIndices[Index];
						Best.TargetQuality = TargetQuality;
						Best.NewMinimumQuality = NewMinimumQuality;
						Best.NewDiagonalLength = NewDiagonalLength;
					}
					if (Best.TargetOffset != INDEX_NONE) BestFlips.Add(Best);
				}
			}
			BestFlips.Sort(
				[](const FBSPFanSliverFlip& A, const FBSPFanSliverFlip& B)
				{
					if (A.TargetQuality != B.TargetQuality)
						return A.TargetQuality < B.TargetQuality;
					return A.TargetOffset < B.TargetOffset;
				});
			TSet<int32> RetessellatedTriangles;
			for (const FBSPFanSliverFlip& Flip : BestFlips)
			{
				if (RetessellatedTriangles.Contains(Flip.TargetOffset) ||
					RetessellatedTriangles.Contains(Flip.NeighbourOffset))
					continue;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					Part.Indices[Flip.TargetOffset + Corner] = Flip.Indices[Corner];
					Part.Indices[Flip.NeighbourOffset + Corner] = Flip.Indices[3 + Corner];
				}
				RetessellatedTriangles.Add(Flip.TargetOffset);
				RetessellatedTriangles.Add(Flip.NeighbourOffset);
				++OutStats.RetessellatedFanSliverPairs;
			}
			for (FWoTBSPPayloadSection& Section : Part.Sections)
			{
				Section.MinVertex = MAX_uint32;
				Section.MaxVertex = 0;
				const uint32 End = Section.FirstIndex + Section.IndexCount;
				for (uint32 At = Section.FirstIndex; At < End; ++At)
				{
					Section.MinVertex = FMath::Min(Section.MinVertex, Part.Indices[At]);
					Section.MaxVertex = FMath::Max(Section.MaxVertex, Part.Indices[At]);
				}
				if (Section.IndexCount == 0) Section.MinVertex = 0;
			}
		}
		return true;
	};

	// Hash only indexed source positions, once per exact float position.
	// The same exact ids also census undirected edge incidence across every
	// part: only a nondegenerate edge with one incident triangle is an
	// exposed repair boundary. Never hash new repaired vertices or mix
	// attributes across sections/parts; candidates supply positions alone.
	// Stable source order is the tie break.
	TArray<FVector3d> SourcePositions;
	TMap<FVector3f, uint32> SourcePointIds;
	TMap<FIntVector, TArray<int32>> SpatialHash;
	for (const FWoTBSPPayloadData& Part : WorkingParts)
	{
		TBitArray<> Indexed(false, Part.Positions.Num());
		for (uint32 Vertex : Part.Indices) Indexed[Vertex] = true;
		for (int32 Vertex = 0; Vertex < Part.Positions.Num(); ++Vertex)
		{
			if (!Indexed[Vertex]) continue;
			if (SourcePointIds.Contains(Part.Positions[Vertex])) continue;
			const FVector3d Position(Part.Positions[Vertex]);
			const int32 SourceVertex = SourcePositions.Add(Position);
			SourcePointIds.Add(Part.Positions[Vertex], uint32(SourceVertex));
			SpatialHash.FindOrAdd(BSPJunctionCell(Position)).Add(SourceVertex);
		}
	}
	TMap<uint64, uint32> NonzeroEdgeIncidences;
	for (const FWoTBSPPayloadData& Part : WorkingParts)
	{
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const FVector3f A = Part.Positions[Part.Indices[At]];
			const FVector3f B = Part.Positions[Part.Indices[At + 1]];
			const FVector3f C = Part.Positions[Part.Indices[At + 2]];
			if (FVector3d::CrossProduct(FVector3d(B) - FVector3d(A),
				FVector3d(C) - FVector3d(A)).SizeSquared() == 0) continue;
			const uint32 IdA = SourcePointIds.FindChecked(A);
			const uint32 IdB = SourcePointIds.FindChecked(B);
			const uint32 IdC = SourcePointIds.FindChecked(C);
			++NonzeroEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdA, IdB));
			++NonzeroEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdB, IdC));
			++NonzeroEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdC, IdA));
		}
	}

	// A second crack class has two open, same-facing coplanar boundaries that
	// share one exact anchor but fork by a sub-unit amount at their free ends.
	// Keep the general donor tolerance at 0.01. Pre-authorize only the inner
	// endpoint of a long, near-collinear anchored pair with opposite interiors,
	// then let transactional triangulation share that exact source position.
	TArray<FBSPJunctionOpenEdge> OpenEdges;
	TMap<uint32, TArray<int32>> OpenEdgesByPoint;
	for (const FWoTBSPPayloadData& Part : WorkingParts)
	{
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const FVector3f Positions[3] = {
				Part.Positions[Part.Indices[At]],
				Part.Positions[Part.Indices[At + 1]],
				Part.Positions[Part.Indices[At + 2]]};
			const FVector3d Cross = FVector3d::CrossProduct(
				FVector3d(Positions[1]) - FVector3d(Positions[0]),
				FVector3d(Positions[2]) - FVector3d(Positions[0]));
			if (Cross.SizeSquared() == 0) continue;
			const FVector3d Normal = Cross.GetSafeNormal();
			const uint32 Surface = Part.TriangleSurfaces.IsValidIndex(At / 3)
				? Part.TriangleSurfaces[At / 3] : MAX_uint32;
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const uint32 PointA = SourcePointIds.FindChecked(Positions[Edge]);
				const uint32 PointB = SourcePointIds.FindChecked(Positions[(Edge + 1) % 3]);
				if (NonzeroEdgeIncidences.FindRef(
					BSPJunctionEdgeKey(PointA, PointB)) != 1) continue;
				const uint32 ThirdPoint = SourcePointIds.FindChecked(Positions[(Edge + 2) % 3]);
				const int32 OpenEdge = OpenEdges.Add({PointA, PointB, ThirdPoint, Surface, Normal});
				OpenEdgesByPoint.FindOrAdd(PointA).Add(OpenEdge);
				OpenEdgesByPoint.FindOrAdd(PointB).Add(OpenEdge);
			}
		}
	}
	TMap<uint64, TArray<int32>> NearCoincidentCandidates;
	TMap<uint64, double> NearCoincidentGaps;
	for (const TPair<uint32, TArray<int32>>& Pair : OpenEdgesByPoint)
	{
		const uint32 Anchor = Pair.Key;
		const FVector3d& AnchorPosition = SourcePositions[Anchor];
		for (int32 LeftAt = 0; LeftAt < Pair.Value.Num(); ++LeftAt)
		for (int32 RightAt = LeftAt + 1; RightAt < Pair.Value.Num(); ++RightAt)
		{
			if (++OutStats.CandidateChecks > BSPJunctionMaxCandidateChecks)
				return FailRepair(TEXT("near-coincident edge search exceeds 100000000 checks; partition the render request"));
			const FBSPJunctionOpenEdge& Left = OpenEdges[Pair.Value[LeftAt]];
			const FBSPJunctionOpenEdge& Right = OpenEdges[Pair.Value[RightAt]];
			const uint32 LeftFree = Left.PointA == Anchor ? Left.PointB : Left.PointA;
			const uint32 RightFree = Right.PointA == Anchor ? Right.PointB : Right.PointA;
			if (LeftFree == RightFree || Left.Surface == MAX_uint32
				|| Left.Surface != Right.Surface
				|| FVector3d::DotProduct(Left.Normal, Right.Normal)
					< BSPJunctionNearCoincidentNormalDot) continue;
			const FVector3d LeftDelta = SourcePositions[LeftFree] - AnchorPosition;
			const FVector3d RightDelta = SourcePositions[RightFree] - AnchorPosition;
			const double LeftLength = LeftDelta.Size();
			const double RightLength = RightDelta.Size();
			if (LeftLength < BSPJunctionNearCoincidentMinLength
				|| RightLength < BSPJunctionNearCoincidentMinLength
				|| FMath::Abs(LeftLength - RightLength) <= BSPJunctionEpsilon
				|| FVector3d::DotProduct(LeftDelta / LeftLength, RightDelta / RightLength)
					< BSPJunctionNearCoincidentDirectionDot) continue;
			// Same-facing triangles may still overlap on the same side. Compare
			// their interiors against one common anchored direction; an ambiguous
			// side or equal-radius fork must preserve authenticated geometry.
			const FVector3d CommonDirection = LeftDelta / LeftLength + RightDelta / RightLength;
			const double LeftSide = FVector3d::DotProduct(FVector3d::CrossProduct(
				CommonDirection, SourcePositions[Left.ThirdPoint] - AnchorPosition), Left.Normal);
			const double RightSide = FVector3d::DotProduct(FVector3d::CrossProduct(
				CommonDirection, SourcePositions[Right.ThirdPoint] - AnchorPosition), Left.Normal);
			if (LeftSide * RightSide >= 0) continue;
			const double Gap = (SourcePositions[LeftFree]
				- SourcePositions[RightFree]).Size();
			if (Gap <= BSPJunctionEpsilon || Gap > BSPJunctionNearCoincidentGap) continue;
			const uint32 HostFree = LeftLength > RightLength ? LeftFree : RightFree;
			const uint32 CandidateFree = LeftLength > RightLength ? RightFree : LeftFree;
			const FVector3d Delta = SourcePositions[HostFree] - AnchorPosition;
			const double Parameter = FVector3d::DotProduct(
				SourcePositions[CandidateFree] - AnchorPosition, Delta) / Delta.SizeSquared();
			const double Distance = (SourcePositions[CandidateFree]
				- (AnchorPosition + Parameter * Delta)).Size();
			if (Parameter <= 0 || Parameter >= 1
				|| Distance > BSPJunctionNearCoincidentGap) continue;
			const uint64 HostKey = BSPJunctionEdgeKey(Anchor, HostFree);
			TArray<int32>& Donors = NearCoincidentCandidates.FindOrAdd(HostKey);
			// Two distinct donors permanently mark an ambiguous host. Saturating
			// here keeps membership work constant per charged anchor pair, even
			// for a dense anchor; no competing pair reaches spatial search.
			if (Donors.Num() < 2 && !Donors.Contains(int32(CandidateFree)))
				Donors.Add(int32(CandidateFree));
			double& MaximumGap = NearCoincidentGaps.FindOrAdd(HostKey);
			MaximumGap = FMath::Max(MaximumGap, Gap);
		}
	}

	// UE1 can leave a separately lit, sub-unit ribbon between otherwise exact
	// coplanar wall edges. At modern resolution that authenticated polygon
	// becomes a one-pixel black line even though there is no geometric gap. A
	// narrow surface is eligible only when every source triangle is narrow, an
	// exact edge has a same-facing coplanar neighbour, every ribbon vertex lies
	// within 0.5 units transversely of one neighbour edge, and the neighbour
	// light is at least four times brighter. Duplicate render vertices preserve
	// every base/material seam and keep the authenticated/collision arrays inert.
	const FWoTBSPPayloadData* LightmapCarrier = WorkingParts.FindByPredicate(
		[](const FWoTBSPPayloadData& Part)
		{
			return Part.LightmapWidth > 0 && Part.LightmapHeight > 0
				&& uint64(Part.LightmapWidth) * Part.LightmapHeight * 4
					== uint64(Part.LightmapBGRA.Num());
		});
	if (LightmapCarrier != nullptr)
	{
		const auto LightmapLuminance = [LightmapCarrier](const FVector2f& UV)
		{
			// The material uses clamp addressing. Clamp in double before FloorToInt
			// so any finite untrusted UV remains inside the integer domain too.
			const double TexelX = FMath::Clamp(
				double(UV.X) * LightmapCarrier->LightmapWidth - 0.5,
				-1.0, double(LightmapCarrier->LightmapWidth));
			const double TexelY = FMath::Clamp(
				double(UV.Y) * LightmapCarrier->LightmapHeight - 0.5,
				-1.0, double(LightmapCarrier->LightmapHeight));
			const int32 X = FMath::FloorToInt(TexelX);
			const int32 Y = FMath::FloorToInt(TexelY);
			const double FractionX = TexelX - X;
			const double FractionY = TexelY - Y;
			const auto Pixel = [LightmapCarrier](int32 PX, int32 PY)
			{
				PX = FMath::Clamp(PX, 0, int32(LightmapCarrier->LightmapWidth) - 1);
				PY = FMath::Clamp(PY, 0, int32(LightmapCarrier->LightmapHeight) - 1);
				const int32 Offset = (PY * LightmapCarrier->LightmapWidth + PX) * 4;
				return (double(LightmapCarrier->LightmapBGRA[Offset])
					+ LightmapCarrier->LightmapBGRA[Offset + 1]
					+ LightmapCarrier->LightmapBGRA[Offset + 2]) / 3.0;
			};
			return FMath::Lerp(
				FMath::Lerp(Pixel(X, Y), Pixel(X + 1, Y), FractionX),
				FMath::Lerp(Pixel(X, Y + 1), Pixel(X + 1, Y + 1), FractionX),
				FractionY);
		};
		TMap<uint64, FBSPLightmapRibbonSurface> SurfaceStats;
		for (int32 PartIndex = 0; PartIndex < WorkingParts.Num(); ++PartIndex)
		{
			const FWoTBSPPayloadData& Part = WorkingParts[PartIndex];
			if (Part.TriangleSurfaces.IsEmpty()) continue;
			int32 SectionIndex = 0;
			for (int32 At = 0; At < Part.Indices.Num(); At += 3)
			{
				++OutStats.LightmapRibbonSourceTriangleVisits;
				while (SectionIndex + 1 < Part.Sections.Num()
					&& At >= int32(Part.Sections[SectionIndex].FirstIndex
						+ Part.Sections[SectionIndex].IndexCount)) ++SectionIndex;
				const FVector3d P[3] = {
					FVector3d(Part.Positions[Part.Indices[At]]),
					FVector3d(Part.Positions[Part.Indices[At + 1]]),
					FVector3d(Part.Positions[Part.Indices[At + 2]])};
				const FVector3d Cross = FVector3d::CrossProduct(P[1] - P[0], P[2] - P[0]);
				const double CrossSize = Cross.Size();
				if (CrossSize == 0) continue;
				const double Longest = FMath::Max3((P[1] - P[0]).Size(),
					(P[2] - P[1]).Size(), (P[0] - P[2]).Size());
				FBSPLightmapRibbonSurface& Stats = SurfaceStats.FindOrAdd(
					BSPLightmapRibbonSurfaceKey(PartIndex, Part.TriangleSurfaces[At / 3]));
				const FVector3d Normal = (FVector3d(Part.Normals[Part.Indices[At]])
					+ FVector3d(Part.Normals[Part.Indices[At + 1]])
					+ FVector3d(Part.Normals[Part.Indices[At + 2]])).GetSafeNormal();
				if (Normal.IsNearlyZero()) continue;
				if (Stats.TriangleCount == 0) Stats.Normal = Normal;
				else if (FVector3d::DotProduct(Stats.Normal, Normal) < BSPLightmapRibbonNormalDot)
					Stats.bConsistentNormal = false;
				Stats.MaximumMinimumAltitude = FMath::Max(
					Stats.MaximumMinimumAltitude, CrossSize / Longest);
				Stats.MaximumEdgeLength = FMath::Max(Stats.MaximumEdgeLength, Longest);
				Stats.TriangleOffsets.Add(At);
				Stats.SectionIndices.Add(SectionIndex);
				++Stats.TriangleCount;
			}
		}
		TArray<uint64> CandidateSurfaces;
		for (const TPair<uint64, FBSPLightmapRibbonSurface>& Pair : SurfaceStats)
		{
			const FBSPLightmapRibbonSurface& Stats = Pair.Value;
			if (Stats.bConsistentNormal && Stats.TriangleCount >= 2
				&& Stats.MaximumMinimumAltitude > BSPJunctionEpsilon
				&& Stats.MaximumMinimumAltitude <= BSPLightmapRibbonWidth
				&& Stats.MaximumEdgeLength >= BSPLightmapRibbonMinLength)
				CandidateSurfaces.Add(Pair.Key);
		}
		CandidateSurfaces.Sort();
		TMap<uint64, TArray<FBSPLightmapRibbonEdge>> CandidateEdges;
		for (uint64 CandidateKey : CandidateSurfaces)
		{
			const int32 PartIndex = int32(CandidateKey >> 32);
			const uint32 Surface = uint32(CandidateKey);
			const FWoTBSPPayloadData& Part = WorkingParts[PartIndex];
			const FBSPLightmapRibbonSurface& Ribbon = SurfaceStats.FindChecked(CandidateKey);
			for (int32 At : Ribbon.TriangleOffsets)
			{
				++OutStats.LightmapRibbonSourceTriangleVisits;
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const uint32 VertexA = Part.Indices[At + Edge];
					const uint32 VertexB = Part.Indices[At + (Edge + 1) % 3];
					CandidateEdges.FindOrAdd(BSPJunctionEdgeKey(
						SourcePointIds.FindChecked(Part.Positions[VertexA]),
						SourcePointIds.FindChecked(Part.Positions[VertexB])))
						.Add({PartIndex, VertexA, VertexB, Surface, Ribbon.Normal});
				}
			}
		}
		TMap<uint64, TArray<FBSPLightmapRibbonEdge>> NeighbourEdges;
		for (int32 PartIndex = 0; PartIndex < WorkingParts.Num(); ++PartIndex)
		{
			const FWoTBSPPayloadData& Part = WorkingParts[PartIndex];
			if (Part.TriangleSurfaces.IsEmpty()) continue;
			for (int32 At = 0; At < Part.Indices.Num(); At += 3)
			{
				++OutStats.LightmapRibbonSourceTriangleVisits;
				const uint32 Surface = Part.TriangleSurfaces[At / 3];
				const FVector3d Normal = (FVector3d(Part.Normals[Part.Indices[At]])
					+ FVector3d(Part.Normals[Part.Indices[At + 1]])
					+ FVector3d(Part.Normals[Part.Indices[At + 2]])).GetSafeNormal();
				if (Normal.IsNearlyZero()) continue;
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const uint32 VertexA = Part.Indices[At + Edge];
					const uint32 VertexB = Part.Indices[At + (Edge + 1) % 3];
					const TArray<FBSPLightmapRibbonEdge>* Candidates = CandidateEdges.Find(
						BSPJunctionEdgeKey(SourcePointIds.FindChecked(Part.Positions[VertexA]),
							SourcePointIds.FindChecked(Part.Positions[VertexB])));
					if (Candidates == nullptr) continue;
					for (const FBSPLightmapRibbonEdge& Candidate : *Candidates)
					{
						if (Candidate.Part == PartIndex && Candidate.Surface == Surface) continue;
						if (++OutStats.LightmapRibbonChecks > BSPJunctionMaxCandidateChecks)
							return FailRepair(TEXT("lightmap ribbon adjacency exceeds 100000000 checks; partition the render request"));
						if (FVector3d::DotProduct(Candidate.Normal, Normal)
							< BSPLightmapRibbonNormalDot) continue;
						NeighbourEdges.FindOrAdd(BSPLightmapRibbonSurfaceKey(
							Candidate.Part, Candidate.Surface)).Add(
								{PartIndex, VertexA, VertexB, Surface, Normal});
					}
				}
			}
		}
		for (uint64 CandidateKey : CandidateSurfaces)
		{
			const TArray<FBSPLightmapRibbonEdge>* Neighbours = NeighbourEdges.Find(CandidateKey);
			if (Neighbours == nullptr || Neighbours->IsEmpty()) continue;
			const int32 PartIndex = int32(CandidateKey >> 32);
			FWoTBSPPayloadData& Part = WorkingParts[PartIndex];
			const FBSPLightmapRibbonSurface& Ribbon = SurfaceStats.FindChecked(CandidateKey);
			TSet<uint32> VertexSet;
			for (int32 At : Ribbon.TriangleOffsets)
			{
				++OutStats.LightmapRibbonSourceTriangleVisits;
				VertexSet.Add(Part.Indices[At]);
				VertexSet.Add(Part.Indices[At + 1]);
				VertexSet.Add(Part.Indices[At + 2]);
			}
			TArray<uint32> Vertices = VertexSet.Array();
			Vertices.Sort();
			TMap<uint32, FVector2f> MappedUVs;
			double MaximumWidth = 0;
			double SourceLuminance = 0;
			for (uint32 Vertex : Vertices)
				SourceLuminance += LightmapLuminance(Part.UV1[Vertex]);
			if (Vertices.IsEmpty()) continue;
			SourceLuminance /= Vertices.Num();
			double NeighbourLuminance = -1.0;
			// Use one exact coplanar neighbour edge for the whole ribbon. Mixing
			// atlas patches per corner makes interpolation cross unrelated atlas
			// space. Distance is transverse to the shared edge's infinite line;
			// longitudinal projection clamps to that neighbour's dilated endpoint.
			for (const FBSPLightmapRibbonEdge& Edge : *Neighbours)
			{
				const FWoTBSPPayloadData& Neighbour = WorkingParts[Edge.Part];
				const FVector3d A(Neighbour.Positions[Edge.VertexA]);
				const FVector3d B(Neighbour.Positions[Edge.VertexB]);
				const FVector3d Delta = B - A;
				const double LengthSquared = Delta.SizeSquared();
				if (LengthSquared == 0) continue;
				const double Length = FMath::Sqrt(LengthSquared);
				TMap<uint32, FVector2f> CandidateUVs;
				double CandidateWidth = 0;
				double CandidateLuminance = 0;
				bool bCoversRibbon = true;
				for (uint32 Vertex : Vertices)
				{
					if (++OutStats.LightmapRibbonChecks > BSPJunctionMaxCandidateChecks)
						return FailRepair(TEXT("lightmap ribbon projection exceeds 100000000 checks; partition the render request"));
					const FVector3d Position(Part.Positions[Vertex]);
					const double Distance = FVector3d::CrossProduct(
						Position - A, Delta).Size() / Length;
					if (Distance > BSPLightmapRibbonWidth)
					{ bCoversRibbon = false; break; }
					const double T = FMath::Clamp(FVector3d::DotProduct(
						Position - A, Delta) / LengthSquared, 0.0, 1.0);
					const FVector2f CandidateUV = FMath::Lerp(
						Neighbour.UV1[Edge.VertexA], Neighbour.UV1[Edge.VertexB], float(T));
					CandidateUVs.Add(Vertex, CandidateUV);
					CandidateWidth = FMath::Max(CandidateWidth, Distance);
					CandidateLuminance += LightmapLuminance(CandidateUV);
				}
				if (!bCoversRibbon) continue;
				CandidateLuminance /= Vertices.Num();
				if (CandidateLuminance > NeighbourLuminance)
				{
					NeighbourLuminance = CandidateLuminance;
					MaximumWidth = CandidateWidth;
					MappedUVs = MoveTemp(CandidateUVs);
				}
			}
			if (NeighbourLuminance < BSPLightmapRibbonMinNeighborLuminance
				|| NeighbourLuminance < SourceLuminance * BSPLightmapRibbonContrast) continue;
			for (int32 TriangleIndex = 0;
				TriangleIndex < Ribbon.TriangleOffsets.Num(); ++TriangleIndex)
			{
				const int32 At = Ribbon.TriangleOffsets[TriangleIndex];
				const int32 SectionIndex = Ribbon.SectionIndices[TriangleIndex];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 SourceVertex = Part.Indices[At + Corner];
					const FVector3f Position = Part.Positions[SourceVertex];
					const FVector3f Normal = Part.Normals[SourceVertex];
					const FVector2f TextureUV = Part.UV0[SourceVertex];
					const FVector2f LightmapUV = MappedUVs.FindChecked(SourceVertex);
					if (++TotalVertices > BSPJunctionMaxOutputVertices)
						return FailRepair(TEXT("lightmap ribbon output exceeds 8000000 vertices; partition the render request"));
					const uint32 Added = Part.Positions.Num();
					Part.Positions.Add(Position);
					Part.Normals.Add(Normal);
					Part.UV0.Add(TextureUV);
					Part.UV1.Add(LightmapUV);
					Part.Indices[At + Corner] = Added;
					Part.Sections[SectionIndex].MinVertex = FMath::Min(
						Part.Sections[SectionIndex].MinVertex, Added);
					Part.Sections[SectionIndex].MaxVertex = FMath::Max(
						Part.Sections[SectionIndex].MaxVertex, Added);
					++OutStats.RepairedLightmapRibbonVertices;
				}
				++OutStats.RepairedLightmapRibbonTriangles;
			}
			++OutStats.RepairedLightmapRibbonSurfaces;
			OutStats.MaxRepairedLightmapRibbonWidth = FMath::Max(
				OutStats.MaxRepairedLightmapRibbonWidth, MaximumWidth);
		}
	}
	uint64 TriangulationChecks = 0;
	const auto FindEdgeCandidates = [&SourcePositions, &SpatialHash, &OutStats,
		&TriangulationChecks, &FailRepair](const FVector3d& A, const FVector3d& B,
		TConstArrayView<FVector3f> Represented,
		TConstArrayView<int32> AuthorizedNearCandidates,
		TArray<FBSPJunctionCandidate>& Candidates) -> bool
	{
		const FVector3d Delta = B - A;
		const double LengthSquared = Delta.SizeSquared();
		if (LengthSquared <= 0) return true;
		TSet<int32> VisitedCandidates;
		FIntVector Cell = BSPJunctionCell(A);
		const FIntVector EndCell = BSPJunctionCell(B);
		int32 Step[3];
		double NextCrossing[3];
		double CrossingDelta[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Step[Axis] = Delta[Axis] > 0 ? 1 : Delta[Axis] < 0 ? -1 : 0;
			CrossingDelta[Axis] = Step[Axis] == 0
				? TNumericLimits<double>::Max()
				: BSPJunctionCellSize / FMath::Abs(Delta[Axis]);
			const double Boundary = (static_cast<double>(Cell[Axis])
				+ (Step[Axis] > 0 ? 1 : 0)) * BSPJunctionCellSize;
			NextCrossing[Axis] = Step[Axis] == 0 || Cell[Axis] == EndCell[Axis]
				? TNumericLimits<double>::Max() : (Boundary - A[Axis]) / Delta[Axis];
		}
		for (;;)
		{
			// A 3D DDA visits edge cells, with a one-cell halo to cover
			// epsilon-close points across faces, corners and negative cells. The
			// work budget counts central DDA progress, not 27 hash lookups.
			if (++OutStats.CellVisits > BSPJunctionMaxCellVisits)
			{
				return FailRepair(TEXT("spatial traversal exceeds 20000000 cell visits; use unrepaired render geometry"));
			}
			for (int32 X = -1; X <= 1; ++X)
			for (int32 Y = -1; Y <= 1; ++Y)
			for (int32 Z = -1; Z <= 1; ++Z)
			{
				const TArray<int32>* Bucket = SpatialHash.Find(Cell + FIntVector(X, Y, Z));
				if (Bucket == nullptr) continue;
				for (int32 CandidateVertex : *Bucket)
				{
					bool bAlreadyVisited = false;
					VisitedCandidates.Add(CandidateVertex, &bAlreadyVisited);
					if (bAlreadyVisited) continue;
					if (++OutStats.CandidateChecks > BSPJunctionMaxCandidateChecks)
					{
						return FailRepair(TEXT("candidate work exceeds 100000000 segment checks; partition the render request"));
					}
					const FVector3d& Candidate = SourcePositions[CandidateVertex];
					const double DistanceSquared =
						(Candidate - (A + FVector3d::DotProduct(Candidate - A, Delta)
							/ LengthSquared * Delta)).SizeSquared();
					const bool bAuthorizedNear = AuthorizedNearCandidates.Contains(CandidateVertex)
						&& DistanceSquared > BSPJunctionEpsilon * BSPJunctionEpsilon;
					const double Parameter = FVector3d::DotProduct(Candidate - A, Delta) / LengthSquared;
					if (Parameter <= 0 || Parameter >= 1
						|| (Candidate - A).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon
						|| (Candidate - B).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon) continue;
					if (DistanceSquared
						> (bAuthorizedNear
							? BSPJunctionNearCoincidentGap * BSPJunctionNearCoincidentGap
							: BSPJunctionEpsilon * BSPJunctionEpsilon)) continue;
					bool bRepresented = false;
					for (const FVector3f& Known : Represented)
					{
						if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
							return FailRepair(TEXT("representation exceeds 100000000 checks; partition the render request"));
						if ((Candidate - FVector3d(Known)).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon)
						{
							bRepresented = true;
							break;
						}
					}
					if (bRepresented) continue;
					// The accepted donor is already a payload float position.
					// Share it exactly; only host attributes use projected T.
					Candidates.Add({ Parameter, CandidateVertex,
						FVector3f(Candidate), bAuthorizedNear });
				}
			}
			if (Cell == EndCell) break;
			const double Next = FMath::Min3(NextCrossing[0], NextCrossing[1], NextCrossing[2]);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (NextCrossing[Axis] <= Next)
				{
					Cell[Axis] += Step[Axis];
					NextCrossing[Axis] = Cell[Axis] == EndCell[Axis]
						? TNumericLimits<double>::Max()
						: NextCrossing[Axis] + CrossingDelta[Axis];
				}
			}
		}
		Candidates.Sort([](const FBSPJunctionCandidate& Left, const FBSPJunctionCandidate& Right)
		{
			return Left.Parameter != Right.Parameter ? Left.Parameter < Right.Parameter : Left.Vertex < Right.Vertex;
		});
		int32 UniqueCount = 0;
		for (const FBSPJunctionCandidate& Candidate : Candidates)
		{
			// Distinct donor positions retain separate boundary vertices
			// even when their projection parameters are nearly equal.
			if (UniqueCount == 0
				|| Candidate.SourcePosition != Candidates[UniqueCount - 1].SourcePosition)
			{
				Candidates[UniqueCount++] = Candidate;
			}
		}
		Candidates.SetNum(UniqueCount, EAllowShrinking::No);
		int32 NearCoincidentCount = 0;
		bool bHasOrdinaryCandidate = false;
		for (const FBSPJunctionCandidate& Candidate : Candidates)
		{
			NearCoincidentCount += Candidate.bNearCoincident ? 1 : 0;
			bHasOrdinaryCandidate |= !Candidate.bNearCoincident;
		}
		if (NearCoincidentCount > 1 || (NearCoincidentCount > 0 && bHasOrdinaryCandidate))
			Candidates.RemoveAll([](const FBSPJunctionCandidate& Candidate)
				{ return Candidate.bNearCoincident; });
		return true;
	};
	TArray<FWoTBSPPayloadData> RepairedParts = WorkingParts;
	for (int32 PartIndex = 0; PartIndex < WorkingParts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Original = WorkingParts[PartIndex];
		FWoTBSPPayloadData& Repaired = RepairedParts[PartIndex];
		Repaired.Indices.Reset();
		Repaired.TriangleSurfaces.Reset();
		for (int32 SectionIndex = 0; SectionIndex < Original.Sections.Num(); ++SectionIndex)
		{
			const FWoTBSPPayloadSection& SourceSection = Original.Sections[SectionIndex];
			FWoTBSPPayloadSection& DestSection = Repaired.Sections[SectionIndex];
			DestSection.FirstIndex = static_cast<uint32>(Repaired.Indices.Num());
			DestSection.MinVertex = MAX_uint32;
			DestSection.MaxVertex = 0;
			const auto AppendTriangle = [&Repaired, &Original, &DestSection](
				uint32 A, uint32 B, uint32 C, uint32 SourceTriangle)
			{
				for (uint32 Vertex : { A, B, C })
				{
					Repaired.Indices.Add(Vertex);
					DestSection.MinVertex = FMath::Min(DestSection.MinVertex, Vertex);
					DestSection.MaxVertex = FMath::Max(DestSection.MaxVertex, Vertex);
				}
				if (!Original.TriangleSurfaces.IsEmpty())
				{
					Repaired.TriangleSurfaces.Add(Original.TriangleSurfaces[SourceTriangle]);
				}
			};
			const uint32 SectionEnd = SourceSection.FirstIndex + SourceSection.IndexCount;
			for (uint32 At = SourceSection.FirstIndex; At < SectionEnd; At += 3)
			{
				const FVector3f HostCorners[3] = { Original.Positions[Original.Indices[At]],
					Original.Positions[Original.Indices[At + 1]], Original.Positions[Original.Indices[At + 2]] };
				TArray<FBSPJunctionCandidate> EdgeCandidates[3];
				bool bRepairedSourceEdge[3] = {false, false, false};
				bool bNearCoincidentSourceEdge[3] = {false, false, false};
				int32 RepairSourceEdges[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
				uint64 RepairSourceEdgeKeys[3] = {0, 0, 0};
				int32 InsertionCount = 0;
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const FVector3f SourceA = Original.Positions[Original.Indices[At + Edge]];
					const FVector3f SourceB = Original.Positions[Original.Indices[At + (Edge + 1) % 3]];
					const uint64 EdgeKey = BSPJunctionEdgeKey(
						SourcePointIds.FindChecked(SourceA), SourcePointIds.FindChecked(SourceB));
					if (NonzeroEdgeIncidences.FindRef(EdgeKey) != 1) continue;
					RepairSourceEdges[Edge] = Edge;
					RepairSourceEdgeKeys[Edge] = EdgeKey;
					const FVector3d A(SourceA);
					const FVector3d B(SourceB);
					const TArray<int32>* AuthorizedNear = NearCoincidentCandidates.Find(EdgeKey);
					if (!FindEdgeCandidates(A, B, HostCorners,
						AuthorizedNear && AuthorizedNear->Num() == 1
							? TConstArrayView<int32>(*AuthorizedNear) : TConstArrayView<int32>(),
						EdgeCandidates[Edge])) return false;
					for (const FBSPJunctionCandidate& Candidate : EdgeCandidates[Edge])
						bNearCoincidentSourceEdge[Edge] |= Candidate.bNearCoincident;
					InsertionCount += EdgeCandidates[Edge].Num();
					bRepairedSourceEdge[Edge] = !EdgeCandidates[Edge].IsEmpty();
				}
				int32 TriangleNearCandidateCount = 0;
				int32 TriangleOrdinaryCandidateCount = 0;
				for (const TArray<FBSPJunctionCandidate>& Candidates : EdgeCandidates)
					for (const FBSPJunctionCandidate& Candidate : Candidates)
						(Candidate.bNearCoincident
							? TriangleNearCandidateCount : TriangleOrdinaryCandidateCount)++;
				// The optional wide candidate must be the triangle's only repair.
				// Mixing it with an ordinary donor or another fork can turn an
				// otherwise safe boundary into an ambiguous/crossing loop. In that
				// case retain the exact pre-existing 0.01 repair candidates.
				if (TriangleNearCandidateCount > 0
					&& (TriangleNearCandidateCount != 1
						|| TriangleOrdinaryCandidateCount != 0))
				{
					InsertionCount = 0;
					for (int32 Edge = 0; Edge < 3; ++Edge)
					{
						EdgeCandidates[Edge].RemoveAll(
							[](const FBSPJunctionCandidate& Candidate)
							{ return Candidate.bNearCoincident; });
						bNearCoincidentSourceEdge[Edge] = false;
						bRepairedSourceEdge[Edge] = !EdgeCandidates[Edge].IsEmpty();
						InsertionCount += EdgeCandidates[Edge].Num();
					}
				}
				const bool bOptionalNearCoincidentTriangle =
					TriangleNearCandidateCount == 1
					&& TriangleOrdinaryCandidateCount == 0;
				if (InsertionCount == 0)
				{
					AppendTriangle(Original.Indices[At], Original.Indices[At + 1], Original.Indices[At + 2], At / 3);
					continue;
				}
				const FVector3d P0(Original.Positions[Original.Indices[At]]);
				const FVector3d P1(Original.Positions[Original.Indices[At + 1]]);
				const FVector3d P2(Original.Positions[Original.Indices[At + 2]]);
				const FVector3d FaceNormal = FVector3d::CrossProduct(P1 - P0, P2 - P0);
				if (FaceNormal.SizeSquared() == 0)
				{
					++OutStats.DroppedDegenerateTriangles;
					continue;
				}
				const int32 FirstRepairVertex = Repaired.Positions.Num();
				const uint64 TotalVerticesBeforeRepair = TotalVertices;
				const uint64 TotalIndicesBeforeRepair = TotalIndices;
				const auto AppendRepairedTriangle = [&Repaired, &AppendTriangle,
					&FaceNormal, At](uint32 A, uint32 B, uint32 C)
				{
					const FVector3d Cross = FVector3d::CrossProduct(
						FVector3d(Repaired.Positions[B]) - FVector3d(Repaired.Positions[A]),
						FVector3d(Repaired.Positions[C]) - FVector3d(Repaired.Positions[A]));
					if (FVector3d::DotProduct(Cross, FaceNormal) < 0) Swap(B, C);
					AppendTriangle(A, B, C, At / 3);
				};
				TotalVertices += static_cast<uint64>(InsertionCount);
				TotalIndices += static_cast<uint64>(InsertionCount) * 3;
				if (TotalVertices > BSPJunctionMaxOutputVertices || TotalIndices > BSPJunctionMaxOutputIndices)
				{
					return FailRepair(TEXT("repair output exceeds 8000000 vertices or 24000000 indices; partition the render request"));
				}
				const auto AppendVertex = [&Repaired, &FaceNormal](const FVector3d& Position,
					const FVector3d& Normal, const FVector2d& TextureUV, const FVector2d& LightmapUV)
				{
					const uint32 Added = static_cast<uint32>(Repaired.Positions.Num());
					Repaired.Positions.Add(FVector3f(Position));
					Repaired.Normals.Add(FVector3f(Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FaceNormal.GetSafeNormal(0))));
					Repaired.UV0.Add(FVector2f(TextureUV));
					Repaired.UV1.Add(FVector2f(LightmapUV));
					return Added;
				};
				TArray<FBSPJunctionBoundaryPoint> Boundary;
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const uint32 VertexA = Original.Indices[At + Edge];
					const uint32 VertexB = Original.Indices[At + (Edge + 1) % 3];
					Boundary.Emplace(VertexA, RepairSourceEdges[Edge]);
					for (const FBSPJunctionCandidate& Candidate : EdgeCandidates[Edge])
					{
						const double T = Candidate.Parameter;
						Boundary.Emplace(AppendVertex(
							FVector3d(Candidate.SourcePosition),
							FMath::Lerp(FVector3d(Original.Normals[VertexA]), FVector3d(Original.Normals[VertexB]), T),
							FMath::Lerp(FVector2d(Original.UV0[VertexA]), FVector2d(Original.UV0[VertexB]), T),
							FMath::Lerp(FVector2d(Original.UV1[VertexA]), FVector2d(Original.UV1[VertexB]), T)),
							RepairSourceEdges[Edge]);
					}
				}
				TArray<TArray<FBSPJunctionBoundaryPoint>> PendingBoundaries;
				PendingBoundaries.Add(MoveTemp(Boundary));
				const int32 FirstChildIndex = Repaired.Indices.Num();
				const int32 FirstChildSurface = Repaired.TriangleSurfaces.Num();
				const uint32 DestMinBeforeRepair = DestSection.MinVertex;
				const uint32 DestMaxBeforeRepair = DestSection.MaxVertex;
				bool bTriangleFellBack = false;
				const auto FallBackToSourceTriangle = [&](const FString& Reason)
				{
					Repaired.Positions.SetNum(FirstRepairVertex, EAllowShrinking::No);
					Repaired.Normals.SetNum(FirstRepairVertex, EAllowShrinking::No);
					Repaired.UV0.SetNum(FirstRepairVertex, EAllowShrinking::No);
					Repaired.UV1.SetNum(FirstRepairVertex, EAllowShrinking::No);
					Repaired.Indices.SetNum(FirstChildIndex, EAllowShrinking::No);
					Repaired.TriangleSurfaces.SetNum(FirstChildSurface, EAllowShrinking::No);
					DestSection.MinVertex = DestMinBeforeRepair;
					DestSection.MaxVertex = DestMaxBeforeRepair;
					TotalVertices = TotalVerticesBeforeRepair;
					TotalIndices = TotalIndicesBeforeRepair;
					AppendTriangle(Original.Indices[At], Original.Indices[At + 1],
						Original.Indices[At + 2], At / 3);
					if (bOptionalNearCoincidentTriangle)
					{
						// This triangle had no pre-existing 0.01 repair. Rejecting
						// the optional wider closure exactly preserves prior output;
						// report it separately from an ordinary local fallback.
						++OutStats.SkippedNearCoincidentTriangles;
					}
					else
					{
						++OutStats.FallbackTriangles;
						OutStats.FallbackTriangleCorners.Add(HostCorners[0]);
						OutStats.FallbackTriangleCorners.Add(HostCorners[1]);
						OutStats.FallbackTriangleCorners.Add(HostCorners[2]);
						OutStats.bFellBackToUnrepaired = true;
						if (OutStats.FallbackReason.IsEmpty())
							OutStats.FallbackReason = FString::Printf(
								TEXT("part %d triangle %u: %s"), PartIndex, At / 3, *Reason);
					}
					bTriangleFellBack = true;
				};
				for (int32 BoundaryIndex = 0; BoundaryIndex < PendingBoundaries.Num(); ++BoundaryIndex)
				{
					Boundary = MoveTemp(PendingBoundaries[BoundaryIndex]);
					const auto CanonicalizeBoundary = [&Boundary, &Repaired, &TriangulationChecks,
						&FailRepair, &PendingBoundaries, &OutStats]() -> bool
					{
						// Opposing source split chains can meet at exactly the same point.
						// Retain its first occurrence/attributes and discard only a closed
						// excursion whose double fan cross products are all zero in 3D.
						TMap<FVector3f, int32> FirstBoundaryPoint;
						for (int32 Point = 0; Point < Boundary.Num();)
						{
							if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
							{
								return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
							}
							const FVector3f Position = Repaired.Positions[Boundary[Point]];
							const int32* RepeatedAt = FirstBoundaryPoint.Find(Position);
							if (RepeatedAt == nullptr)
							{
								FirstBoundaryPoint.Add(Position, Point++);
								continue;
							}
							const int32 LoopStart = *RepeatedAt;
							double LoopArea3D = 0;
							for (int32 Segment = LoopStart + 1; Segment < Point; ++Segment)
							{
								if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
								{
									return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
								}
								// Nonnegative magnitudes cannot cancel or vanish merely
								// because the loop is perpendicular to the host normal.
								LoopArea3D += FVector3d::CrossProduct(
									FVector3d(Repaired.Positions[Boundary[Segment]]) - FVector3d(Position),
									FVector3d(Repaired.Positions[Boundary[Segment + 1]]) - FVector3d(Position)).SizeSquared();
							}
							if (LoopArea3D != 0)
							{
								TArray<FBSPJunctionBoundaryPoint> ClosedLoop;
								for (int32 Segment = LoopStart; Segment < Point; ++Segment)
								{
									if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
										return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
									ClosedLoop.Add(Boundary[Segment]);
								}
								PendingBoundaries.Add(MoveTemp(ClosedLoop));
							}
							else ++OutStats.DroppedDegenerateLoops;
							const int32 ContinuationEdge = Boundary[Point].SourceEdge;
							Boundary.RemoveAt(LoopStart + 1, Point - LoopStart, EAllowShrinking::No);
							Boundary[LoopStart].SourceEdge = ContinuationEdge;
							FirstBoundaryPoint.Reset();
							Point = 0;
						}
						return true;
					};
					if (!CanonicalizeBoundary()) return false;
					// Re-test the current subedges: exact-source insertion can bend
					// an edge enough to make another original source newly eligible.
					bool bClosed = Boundary.Num() < 3;
					for (int32 ClosurePass = 0; !bClosed && ClosurePass < BSPJunctionMaxClosurePasses; ++ClosurePass)
					{
						TArray<FVector3f> Represented;
						for (uint32 Vertex : Boundary) Represented.Add(Repaired.Positions[Vertex]);
						TArray<FBSPJunctionBoundaryPoint> NextBoundary;
						bool bAdded = false;
						for (int32 Segment = 0; Segment < Boundary.Num(); ++Segment)
						{
							const uint32 VertexA = Boundary[Segment];
							const uint32 VertexB = Boundary[(Segment + 1) % Boundary.Num()];
							NextBoundary.Add(Boundary[Segment]);
							const int32 SourceEdge = Boundary[Segment].SourceEdge;
							if (SourceEdge == INDEX_NONE) continue;
							TArray<FBSPJunctionCandidate> Candidates;
							if (!FindEdgeCandidates(FVector3d(Repaired.Positions[VertexA]),
								FVector3d(Repaired.Positions[VertexB]), Represented,
								TConstArrayView<int32>(), Candidates)) return false;
							for (const FBSPJunctionCandidate& Candidate : Candidates)
							{
								if (++TotalVertices > BSPJunctionMaxOutputVertices
									|| (TotalIndices += 3) > BSPJunctionMaxOutputIndices)
									return FailRepair(TEXT("closure output exceeds 8000000 vertices or 24000000 indices; partition the render request"));
								const double T = Candidate.Parameter;
								NextBoundary.Emplace(AppendVertex(FVector3d(Candidate.SourcePosition),
									FMath::Lerp(FVector3d(Repaired.Normals[VertexA]), FVector3d(Repaired.Normals[VertexB]), T),
									FMath::Lerp(FVector2d(Repaired.UV0[VertexA]), FVector2d(Repaired.UV0[VertexB]), T),
									FMath::Lerp(FVector2d(Repaired.UV1[VertexA]), FVector2d(Repaired.UV1[VertexB]), T)),
									SourceEdge);
								++InsertionCount;
								bRepairedSourceEdge[SourceEdge] = true;
								bAdded = true;
							}
						}
						Boundary = MoveTemp(NextBoundary);
						if (!CanonicalizeBoundary()) return false;
						if (!bAdded || Boundary.Num() < 3) { bClosed = true; break; }
					}
					if (!bClosed)
					{
						FallBackToSourceTriangle(FString::Printf(
							TEXT("closure exceeds %d passes"), BSPJunctionMaxClosurePasses));
						break;
					}
					double PolygonArea = 0;
					double PolygonArea3D = 0;
					FVector3d AreaVector = FVector3d::ZeroVector;
					FVector3d LargestFanNormal = FVector3d::ZeroVector;
					TArray<FVector3d> FanNormals;
					double LargestFanAreaSquared = 0;
					for (int32 Point = 1; Point + 1 < Boundary.Num(); ++Point)
					{
						if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
							return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
						const FVector3d FanNormal = FVector3d::CrossProduct(
							FVector3d(Repaired.Positions[Boundary[Point]]) - FVector3d(Repaired.Positions[Boundary[0]]),
							FVector3d(Repaired.Positions[Boundary[Point + 1]]) - FVector3d(Repaired.Positions[Boundary[0]]));
						FanNormals.Add(FanNormal);
						PolygonArea += FVector3d::DotProduct(FanNormal, FaceNormal);
						const double FanAreaSquared = FanNormal.SizeSquared();
						PolygonArea3D += FanAreaSquared;
						AreaVector += FanNormal;
						if (FanAreaSquared > LargestFanAreaSquared)
						{
							LargestFanAreaSquared = FanAreaSquared;
							LargestFanNormal = FanNormal;
						}
					}
					if (PolygonArea3D == 0)
					{
						// A fully collapsed render boundary has no area to rasterize.
						// Its original collision payload is owned by the caller.
						++OutStats.DroppedDegenerateLoops;
						continue;
					}
					FVector3d TriangulationNormal = FaceNormal;
					const auto SignedArea = [&TriangulationNormal](const FVector3d& A,
						const FVector3d& B, const FVector3d& C)
					{
						return FVector3d::DotProduct(FVector3d::CrossProduct(B - A, C - A), TriangulationNormal);
					};
					// A noncoplanar loop may cross in one projection but remain simple in
					// another. Try the authored face first, then deterministic local
					// planes: largest fan, Newell area vector, and each source-order fan.
					// Every local normal is oriented to the authored face, never a world
					// axis, so mirrored one-sided geometry retains its facing.
					TArray<FVector3d> ProjectionNormals;
					if (PolygonArea != 0) ProjectionNormals.Add(FaceNormal);
					ProjectionNormals.Add(LargestFanNormal);
					if (AreaVector.SizeSquared() != 0) ProjectionNormals.Add(AreaVector);
					ProjectionNormals.Append(FanNormals);
					bool bFoundProjection = false;
					for (int32 Projection = 0; Projection < ProjectionNormals.Num(); ++Projection)
					{
						TriangulationNormal = ProjectionNormals[Projection];
						if (FVector3d::DotProduct(TriangulationNormal, FaceNormal) < 0)
							TriangulationNormal = -TriangulationNormal;
						const double ProjectedArea = FVector3d::DotProduct(
							AreaVector, TriangulationNormal);
						if (ProjectedArea == 0) continue;
						bool bCrossing = false;
						for (int32 EdgeA = 0; EdgeA < Boundary.Num() && !bCrossing; ++EdgeA)
							for (int32 EdgeB = EdgeA + 2; EdgeB < Boundary.Num(); ++EdgeB)
							{
								if (EdgeA == 0 && EdgeB + 1 == Boundary.Num()) continue;
								if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
									return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
								const FVector3d A(Repaired.Positions[Boundary[EdgeA]]);
								const FVector3d B(Repaired.Positions[Boundary[(EdgeA + 1) % Boundary.Num()]]);
								const FVector3d C(Repaired.Positions[Boundary[EdgeB]]);
								const FVector3d D(Repaired.Positions[Boundary[(EdgeB + 1) % Boundary.Num()]]);
								const double AC = SignedArea(A, B, C);
								const double AD = SignedArea(A, B, D);
								const double CA = SignedArea(C, D, A);
								const double CB = SignedArea(C, D, B);
								if (((AC > 0 && AD < 0) || (AC < 0 && AD > 0))
									&& ((CA > 0 && CB < 0) || (CA < 0 && CB > 0)))
								{
									bCrossing = true;
									break;
								}
							}
						if (!bCrossing)
						{
							PolygonArea = ProjectedArea;
							bFoundProjection = true;
							if (Projection > 0 || ProjectionNormals[0] != FaceNormal)
								++OutStats.LocallyOrientedLoops;
							break;
						}
					}
					if (!bFoundProjection)
					{
						FallBackToSourceTriangle(
							TEXT("self-intersecting closed boundary loop in every consistent local projection"));
						break;
					}
					if (PolygonArea < 0)
						for (int32 Point = 0; Point < Boundary.Num() / 2; ++Point)
							Swap(Boundary[Point], Boundary[Boundary.Num() - Point - 1]);
					// Ear clipping adds no geometric positions and emits N-2 children.
					// An inclusive containment test preserves collinear split vertices:
					// an ear cannot skip a remaining point on its new diagonal. A sibling
					// sliver near that shared diagonal is part of the same filled polygon.
					TArray<int32> Remaining;
					for (int32 Point = 0; Point < Boundary.Num(); ++Point) Remaining.Add(Point);
					while (Remaining.Num() > 3)
					{
						bool bClipped = false;
						for (int32 Ear = 0; Ear < Remaining.Num(); ++Ear)
						{
							if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
							{
								return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
							}
							const int32 Prev = Remaining[(Ear + Remaining.Num() - 1) % Remaining.Num()];
							const int32 Tip = Remaining[Ear];
							const int32 Next = Remaining[(Ear + 1) % Remaining.Num()];
							const FVector3d A(Repaired.Positions[Boundary[Prev]]);
							const FVector3d B(Repaired.Positions[Boundary[Tip]]);
							const FVector3d C(Repaired.Positions[Boundary[Next]]);
							if (SignedArea(A, B, C) <= 0) continue;
							bool bBlocked = false;
							for (int32 Point : Remaining)
							{
								if (Point == Prev || Point == Tip || Point == Next) continue;
								if (++TriangulationChecks > BSPJunctionMaxCandidateChecks)
								{
									return FailRepair(TEXT("triangulation exceeds 100000000 checks; partition the render request"));
								}
								const FVector3d P(Repaired.Positions[Boundary[Point]]);
								if (SignedArea(A, B, P) >= 0 && SignedArea(B, C, P) >= 0 && SignedArea(C, A, P) >= 0)
								{
									bBlocked = true;
									break;
								}
							}
							if (bBlocked) continue;
							AppendRepairedTriangle(Boundary[Prev], Boundary[Tip], Boundary[Next]);
							Remaining.RemoveAt(Ear, 1, EAllowShrinking::No);
							bClipped = true;
							break;
						}
						if (!bClipped)
						{
							FallBackToSourceTriangle(TEXT("cannot triangulate repaired boundary without losing area/winding or skipping a boundary point"));
							break;
						}
					}
					if (bTriangleFellBack) break;
					if (SignedArea(FVector3d(Repaired.Positions[Boundary[Remaining[0]]]),
						FVector3d(Repaired.Positions[Boundary[Remaining[1]]]),
						FVector3d(Repaired.Positions[Boundary[Remaining[2]]])) <= 0)
					{
						FallBackToSourceTriangle(
							TEXT("repair loses area/winding at float payload precision"));
						break;
					}
					AppendRepairedTriangle(Boundary[Remaining[0]], Boundary[Remaining[1]], Boundary[Remaining[2]]);
				}
				if (bTriangleFellBack) continue;
				// Retriangulating a near-collapsed source triangle can expose a child
				// edge that passes through one of the triangle's own authored corners.
				// Those corners were intentionally excluded from donor discovery. Count
				// the resulting tolerance-only ambiguity so the final-buffer audit can
				// distinguish it from an unexplained missed source donor. Replacing this
				// single edge perturbs several overlapping retail slivers and creates
				// more residuals than retaining their authenticated triangulation.
				{
					TMap<FVector3f, uint32> ChildPointIds;
					TMap<uint64, uint32> ChildEdgeIncidences;
					const auto ChildPointId = [&ChildPointIds](const FVector3f& Position)
					{
						if (const uint32* Found = ChildPointIds.Find(Position)) return *Found;
						const uint32 Added = ChildPointIds.Num();
						ChildPointIds.Add(Position, Added);
						return Added;
					};
					for (int32 ChildAt = FirstChildIndex; ChildAt < Repaired.Indices.Num(); ChildAt += 3)
					{
						const FVector3f A = Repaired.Positions[Repaired.Indices[ChildAt]];
						const FVector3f B = Repaired.Positions[Repaired.Indices[ChildAt + 1]];
						const FVector3f C = Repaired.Positions[Repaired.Indices[ChildAt + 2]];
						if (FVector3d::CrossProduct(FVector3d(B) - FVector3d(A),
							FVector3d(C) - FVector3d(A)).SizeSquared() == 0) continue;
						const uint32 IdA = ChildPointId(A), IdB = ChildPointId(B), IdC = ChildPointId(C);
						++ChildEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdA, IdB));
						++ChildEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdB, IdC));
						++ChildEdgeIncidences.FindOrAdd(BSPJunctionEdgeKey(IdC, IdA));
					}
					int32 SplitChildAt = INDEX_NONE;
					for (int32 ChildAt = FirstChildIndex;
						ChildAt < Repaired.Indices.Num() && SplitChildAt == INDEX_NONE; ChildAt += 3)
					{
						const FVector3d Child[3] = {
							FVector3d(Repaired.Positions[Repaired.Indices[ChildAt]]),
							FVector3d(Repaired.Positions[Repaired.Indices[ChildAt + 1]]),
							FVector3d(Repaired.Positions[Repaired.Indices[ChildAt + 2]])};
						if (FVector3d::CrossProduct(Child[1] - Child[0],
							Child[2] - Child[0]).SizeSquared() == 0) continue;
						for (int32 Edge = 0; Edge < 3 && SplitChildAt == INDEX_NONE; ++Edge)
						{
							const FVector3d& A = Child[Edge];
							const FVector3d& B = Child[(Edge + 1) % 3];
							const FVector3d& C = Child[(Edge + 2) % 3];
							if (ChildEdgeIncidences.FindRef(BSPJunctionEdgeKey(
								ChildPointIds.FindChecked(FVector3f(A)),
								ChildPointIds.FindChecked(FVector3f(B)))) != 1) continue;
							const FVector3d Delta = B - A;
							const double LengthSquared = Delta.SizeSquared();
							if (LengthSquared == 0) continue;
							for (int32 Corner = 0; Corner < 3; ++Corner)
							{
								const FVector3d Point(HostCorners[Corner]);
								if ((Point - A).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon
									|| (Point - B).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon
									|| (Point - C).SizeSquared() <= BSPJunctionEpsilon * BSPJunctionEpsilon) continue;
								const double Parameter = FVector3d::DotProduct(Point - A, Delta) / LengthSquared;
								if (Parameter > 0 && Parameter < 1
									&& (Point - (A + Parameter * Delta)).SizeSquared()
										<= BSPJunctionEpsilon * BSPJunctionEpsilon)
								{
									SplitChildAt = ChildAt;
									OutStats.NearCollapsedEdgeWitnesses.Add(FVector3f(A));
									OutStats.NearCollapsedEdgeWitnesses.Add(FVector3f(B));
									OutStats.NearCollapsedEdgeWitnesses.Add(FVector3f(Point));
									break;
								}
							}
						}
					}
					if (SplitChildAt != INDEX_NONE) ++OutStats.NearCollapsedChildEdges;
				}
				if (Repaired.Indices.Num() == FirstChildIndex) ++OutStats.DroppedDegenerateTriangles;
				else ++OutStats.SplitTriangles;
				for (bool bRepaired : bRepairedSourceEdge)
					if (bRepaired) ++OutStats.RepairedEdges;
				for (int32 Edge = 0; Edge < 3; ++Edge)
					if (bRepairedSourceEdge[Edge] && bNearCoincidentSourceEdge[Edge])
					{
						++OutStats.RepairedNearCoincidentEdges;
						OutStats.MaxRepairedNearCoincidentGap = FMath::Max(
							OutStats.MaxRepairedNearCoincidentGap,
							NearCoincidentGaps.FindRef(RepairSourceEdgeKeys[Edge]));
					}
				OutStats.InsertedEdgeVertices += InsertionCount;
			}
			DestSection.IndexCount = static_cast<uint32>(Repaired.Indices.Num()) - DestSection.FirstIndex;
			if (DestSection.IndexCount == 0) DestSection.MinVertex = 0;
		}
		OutStats.OutputTriangles += Repaired.Indices.Num() / 3;
	}
	// Keep fan diagonal exchange after edge closure so it cannot perturb donor
	// discovery, fallback classification, or the established closure counters.
	if (!RetessellateFanSlivers(RepairedParts)) return false;
	RenderParts = MoveTemp(RepairedParts);
	OutStats.RepairMilliseconds = (FPlatformTime::Seconds() - Started) * 1000;
	return true;
}

namespace
{
UTexture2D* WoTRuntimeBSPMaterializeBGRA(
	int32 Width, int32 Height, TConstArrayView<uint8> BGRA,
	TextureFilter Filter, TextureAddress Address)
{
	const int64 ByteCount = static_cast<int64>(Width) * Height * 4;
	if (Width <= 0 || Height <= 0 || ByteCount <= 0
		|| ByteCount != BGRA.Num())
	{
		return nullptr;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(
		Width, Height, PF_B8G8R8A8);
	if (Texture == nullptr || Texture->GetPlatformData() == nullptr
		|| Texture->GetPlatformData()->Mips.Num() != 1)
	{
		return nullptr;
	}
	void* Destination = Texture->GetPlatformData()
		->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	if (Destination == nullptr)
	{
		Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
		return nullptr;
	}
	FMemory::Memcpy(Destination, BGRA.GetData(), static_cast<SIZE_T>(ByteCount));
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->SRGB = false;
	Texture->Filter = Filter;
	Texture->AddressX = Address;
	Texture->AddressY = Address;
	Texture->NeverStream = true;
#if WITH_EDITORONLY_DATA
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif
	Texture->UpdateResource();
	return Texture;
}
}

UTexture2D* WoTRuntimeBSPMaterializeTexture(
	const FWoTTexturePayloadData& Payload, bool bRawSurfaceTexture,
	bool bNoSmooth)
{
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = !bRawSurfaceTexture;
	// Smooth surfaces sample through TEXTUREGROUP_World (TF_Default: anisotropic,
	// blended mips) instead of retail's isotropic bilinear, so oblique floors and
	// ceilings keep their detail (PORT_NOTES "World surfaces filter anisotropically").
	Options.Filter = bNoSmooth ? TF_Nearest : TF_Default;
	// Retail Glide authors DetailBias=-1.5. UTexture exposes only an integer
	// asset bias; -1 is the closest conservative value (the equally near -2
	// would sharpen past the authored midpoint).
	Options.LODBias = -1;
	Options.AddressX = TA_Wrap;
	Options.AddressY = TA_Wrap;
	return WoTRuntimeMaterializeTexture(
		GetTransientPackage(), Payload, Options);
}

UTexture2D* WoTRuntimeProceduralPlaceholderTexture(
	const FWoTFractalTextureRecord& Record)
{
	if (Record.Width <= 0 || Record.Height <= 0
		|| Record.PaletteRGBA.Num() != 256 * 4
		|| static_cast<int64>(Record.Width) * Record.Height
			!= static_cast<int64>(Record.InitialBitmap.Num()))
	{
		return nullptr;
	}
	TArray<uint8> BGRA;
	BGRA.SetNumUninitialized(Record.InitialBitmap.Num() * 4);
	for (int32 Pixel = 0; Pixel < Record.InitialBitmap.Num(); ++Pixel)
	{
		const int32 Offset = static_cast<int32>(Record.InitialBitmap[Pixel]) * 4;
		BGRA[Pixel * 4 + 0] = Record.PaletteRGBA[Offset + 2];
		BGRA[Pixel * 4 + 1] = Record.PaletteRGBA[Offset + 1];
		BGRA[Pixel * 4 + 2] = Record.PaletteRGBA[Offset + 0];
		BGRA[Pixel * 4 + 3] = Record.PaletteRGBA[Offset + 3];
	}
	const FName BaseName = UWoTFractalSubsystem::RuntimePlaceholderName(
		UWoTFractalSubsystem::RecordIdentityKey(Record));
	UTexture2D* Texture = UTexture2D::CreateTransient(
		Record.Width, Record.Height, PF_B8G8R8A8,
		MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), BaseName),
		TConstArrayView64<uint8>(BGRA.GetData(), BGRA.Num()));
	if (Texture == nullptr) return nullptr;
	Texture->SRGB = false;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Wrap;
	Texture->AddressY = TA_Wrap;
	Texture->NeverStream = true;
#if WITH_EDITORONLY_DATA
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif
	Texture->UpdateResource();
	return Texture;
}

UTexture2D* WoTRuntimeBSPMaterializeProceduralIndices(
	const FWoTRuntimeProceduralMip& Mip)
{
	if (Mip.Width == 0 || Mip.Height == 0 || Mip.Width > MAX_int32
		|| Mip.Height > MAX_int32
		|| static_cast<uint64>(Mip.Width) * Mip.Height
			!= static_cast<uint64>(Mip.Indices.Num()))
	{
		return nullptr;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(
		static_cast<int32>(Mip.Width), static_cast<int32>(Mip.Height),
		PF_B8G8R8A8);
	if (Texture == nullptr || Texture->GetPlatformData() == nullptr
		|| Texture->GetPlatformData()->Mips.IsEmpty())
	{
		return nullptr;
	}
	FColor* Destination = static_cast<FColor*>(Texture->GetPlatformData()
		->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
	if (Destination == nullptr) return nullptr;
	for (int32 Index = 0; Index < Mip.Indices.Num(); ++Index)
	{
		const uint8 AuthoredIndex = Mip.Indices[Index];
		Destination[Index] = FColor(
			AuthoredIndex, AuthoredIndex, AuthoredIndex, 255);
	}
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->SRGB = false;
	Texture->Filter = TF_Bilinear;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	return Texture;
}

UTexture2D* WoTRuntimeBSPMaterializeLightmap(
	const FWoTBSPPayloadData& Payload)
{
	return WoTRuntimeBSPMaterializeLightmap(Payload.LightmapWidth,
		Payload.LightmapHeight, Payload.LightmapBGRA);
}

UTexture2D* WoTRuntimeBSPMaterializeLightmap(
	uint32 Width, uint32 Height, TConstArrayView<uint8> BGRA)
{
	uint64 PixelCount = 0;
	uint64 ByteCount = 0;
	if (Width == 0 || Height == 0
		|| Width > MAX_int32 || Height > MAX_int32
		|| Width > MAX_uint64 / Height
		|| (PixelCount = static_cast<uint64>(Width) * Height) > MAX_uint64 / 4
		|| (ByteCount = PixelCount * 4)
			!= static_cast<uint64>(BGRA.Num()))
	{
		return nullptr;
	}
	return WoTRuntimeBSPMaterializeBGRA(
		static_cast<int32>(Width), static_cast<int32>(Height), BGRA,
		TF_Bilinear, TA_Clamp);
}

UTexture2D* WoTRuntimeBSPMaterializeNeutralLightmap()
{
	const uint8 Pixel[] = {128, 128, 128, 255};
	return WoTRuntimeBSPMaterializeBGRA(1, 1, MakeArrayView(Pixel),
		TF_Bilinear, TA_Clamp);
}

UTexture2D* WoTRuntimeBSPMaterializeNeutralGreyTexture()
{
	const uint8 Pixel[] = {128, 128, 128, 255};
	return WoTRuntimeBSPMaterializeBGRA(1, 1, MakeArrayView(Pixel),
		TF_Nearest, TA_Wrap);
}

UTexture2D* WoTRuntimeBSPMaterializeSolidBlackTexture()
{
	const uint8 Pixel[] = {0, 0, 0, 255};
	return WoTRuntimeBSPMaterializeBGRA(1, 1, MakeArrayView(Pixel),
		TF_Nearest, TA_Wrap);
}

FString WoTRuntimeBSPConverterSlotBase(FStringView SourceExport)
{
	const FString Source(SourceExport);
	int32 FirstDot = INDEX_NONE;
	int32 LastDot = INDEX_NONE;
	if (!Source.FindChar(TEXT('.'), FirstDot)
		|| !Source.FindLastChar(TEXT('.'), LastDot)
		|| FirstDot <= 0 || LastDot + 1 >= Source.Len()
		|| Source.Contains(TEXT("..")))
	{
		return {};
	}
	const FString Package = Source.Left(FirstDot);
	const FString Leaf = Source.Mid(LastDot + 1);
	if (Package.Contains(TEXT("__")) || Leaf.Contains(TEXT("__"))) return {};
	return Package + TEXT("__") + Leaf;
}

namespace
{
bool WoTRuntimeBSPConverterSlotMatchesImpl(
	FStringView MaterialSlot, FStringView Base, ESearchCase::Type BaseSearchCase)
{
	static constexpr const TCHAR* OrderedSuffixes[] = {
		TEXT("_M"), TEXT("_T"), TEXT("_Mod"), TEXT("_2S"),
		TEXT("_U"), TEXT("_Sky"), TEXT("_SL")};
	if (Base.IsEmpty()
		|| !MaterialSlot.StartsWith(Base, BaseSearchCase)) return false;
	FStringView Remaining = MaterialSlot.RightChop(Base.Len());
	for (const TCHAR* Suffix : OrderedSuffixes)
	{
		const FStringView Token(Suffix);
		if (Remaining.StartsWith(Token, ESearchCase::CaseSensitive)
			&& (Remaining.Len() == Token.Len()
				|| Remaining[Token.Len()] == TCHAR('_')))
		{
			Remaining = Remaining.RightChop(Token.Len());
		}
	}
	return Remaining.IsEmpty()
		|| Remaining.Equals(TEXT("__SkyZone"), ESearchCase::CaseSensitive);
}
}

bool WoTRuntimeBSPConverterSlotMatches(
	FStringView MaterialSlot, FStringView Base)
{
	return WoTRuntimeBSPConverterSlotMatchesImpl(
		MaterialSlot, Base, ESearchCase::CaseSensitive);
}

bool WoTRuntimeBSPConverterSlotMatchesFolded(
	FStringView MaterialSlot, FStringView Base)
{
	return WoTRuntimeBSPConverterSlotMatchesImpl(
		MaterialSlot, Base, ESearchCase::IgnoreCase);
}

int32 WoTRuntimeBSPResolveAuthenticatedConverterSlot(
	FStringView MaterialSlot,
	TConstArrayView<FWoTRuntimeBSPConverterCandidate> Candidates)
{
	int32 Match = INDEX_NONE;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FWoTRuntimeBSPConverterCandidate& Candidate = Candidates[Index];
		const FString SourceBase =
			WoTRuntimeBSPConverterSlotBase(Candidate.SourceExport);
		if (SourceBase.IsEmpty()
			|| !Candidate.LogicalBase.Equals(
				SourceBase, ESearchCase::IgnoreCase)
			|| !WoTRuntimeBSPConverterSlotMatchesFolded(
				MaterialSlot, SourceBase))
		{
			continue;
		}
		if (Match != INDEX_NONE) return INDEX_NONE;
		Match = Index;
	}
	return Match;
}

int32 WoTRuntimeBSPResolveConverterSlot(FStringView MaterialSlot,
	TConstArrayView<FString> AuthenticatedSourceExports)
{
	int32 Match = INDEX_NONE;
	int32 MatchLength = INDEX_NONE;
	TSet<FString> SeenBases;
	for (int32 Index = 0; Index < AuthenticatedSourceExports.Num(); ++Index)
	{
		const FString Base = WoTRuntimeBSPConverterSlotBase(
			AuthenticatedSourceExports[Index]);
		if (Base.IsEmpty()) return INDEX_NONE;
		const FString FoldedBase = Base.ToLower();
		if (SeenBases.Contains(FoldedBase)) return INDEX_NONE;
		SeenBases.Add(FoldedBase);
		if (!WoTRuntimeBSPConverterSlotMatches(MaterialSlot, Base)) continue;
		if (Base.Len() > MatchLength)
		{
			Match = Index;
			MatchLength = Base.Len();
		}
		else if (Base.Len() == MatchLength)
		{
			return INDEX_NONE;
		}
	}
	return Match;
}

bool WoTRuntimeBSPSectionStyleForBase(FStringView MaterialSlot,
	FStringView ConverterSlotBase, FWoTRuntimeBSPSectionStyle& OutStyle)
{
	OutStyle = FWoTRuntimeBSPSectionStyle();
	if (ConverterSlotBase.IsEmpty()
		|| !WoTRuntimeBSPConverterSlotMatchesFolded(
			MaterialSlot, ConverterSlotBase))
		return false;
	OutStyle.ConverterSlotBase = FString(ConverterSlotBase);
	FStringView Remaining = MaterialSlot.RightChop(ConverterSlotBase.Len());
	static constexpr const TCHAR* OrderedSuffixes[] = {
		TEXT("_M"), TEXT("_T"), TEXT("_Mod"), TEXT("_2S"),
		TEXT("_U"), TEXT("_Sky"), TEXT("_SL")};
	for (const TCHAR* Suffix : OrderedSuffixes)
	{
		const FStringView Token(Suffix);
		if (!Remaining.StartsWith(Token, ESearchCase::CaseSensitive)
			|| (Remaining.Len() != Token.Len()
				&& Remaining[Token.Len()] != TCHAR('_'))) continue;
		if (Token.Equals(TEXT("_T"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Translucent;
		else if (Token.Equals(TEXT("_Mod"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Modulated;
		else if (Token.Equals(TEXT("_M"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Masked;
		else if (Token.Equals(TEXT("_2S"), ESearchCase::CaseSensitive))
			OutStyle.bTwoSided = true;
		else if (Token.Equals(TEXT("_U"), ESearchCase::CaseSensitive))
			OutStyle.bUnlit = true;
		else if (Token.Equals(TEXT("_Sky"), ESearchCase::CaseSensitive))
			OutStyle.bSky = true;
		else if (Token.Equals(TEXT("_SL"), ESearchCase::CaseSensitive))
			OutStyle.bSpecialLit = true;
		Remaining = Remaining.RightChop(Token.Len());
	}
	if (Remaining.Equals(TEXT("__SkyZone"), ESearchCase::CaseSensitive))
	{
		OutStyle.bSky = true;
		Remaining = Remaining.RightChop(Remaining.Len());
	}
	return Remaining.IsEmpty();
}

bool WoTRuntimeBSPSectionStyle(
	FStringView MaterialSlot, FWoTRuntimeBSPSectionStyle& OutStyle)
{
	OutStyle = FWoTRuntimeBSPSectionStyle();
	const FString Slot(MaterialSlot);
	const int32 Separator = Slot.Find(TEXT("__"), ESearchCase::CaseSensitive);
	// Without an authenticated source.export base, only suffixes at the end can
	// be converter flags. Embedded fragments such as WATR_M08 and Sky_440 are
	// ordinary texture-name bytes. Invalid/unqualified slots stay conservative;
	// callers bind neutral grey rather than rejecting an otherwise valid world.
	if (Separator <= 0 || Separator + 2 >= Slot.Len()) return true;
	static constexpr const TCHAR* OrderedSuffixes[] = {
		TEXT("_M"), TEXT("_T"), TEXT("_Mod"), TEXT("_2S"),
		TEXT("_U"), TEXT("_Sky"), TEXT("_SL")};
	int32 BaseEnd = Slot.Len();
	const FStringView SkyZone(TEXT("__SkyZone"));
	if (FStringView(Slot).Left(BaseEnd).EndsWith(
		SkyZone, ESearchCase::CaseSensitive))
	{
		OutStyle.bSky = true;
		BaseEnd -= SkyZone.Len();
	}
	for (int32 Index = UE_ARRAY_COUNT(OrderedSuffixes) - 1; Index >= 0; --Index)
	{
		const FStringView Token(OrderedSuffixes[Index]);
		if (!FStringView(Slot).Left(BaseEnd).EndsWith(
			Token, ESearchCase::CaseSensitive)
			|| BaseEnd - Token.Len() <= Separator + 2) continue;
		if (Token.Equals(TEXT("_T"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Translucent;
		else if (Token.Equals(TEXT("_Mod"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Modulated;
		else if (Token.Equals(TEXT("_M"), ESearchCase::CaseSensitive))
			OutStyle.Blend = EWoTRuntimeBSPBlend::Masked;
		else if (Token.Equals(TEXT("_2S"), ESearchCase::CaseSensitive))
			OutStyle.bTwoSided = true;
		else if (Token.Equals(TEXT("_U"), ESearchCase::CaseSensitive))
			OutStyle.bUnlit = true;
		else if (Token.Equals(TEXT("_Sky"), ESearchCase::CaseSensitive))
			OutStyle.bSky = true;
		else if (Token.Equals(TEXT("_SL"), ESearchCase::CaseSensitive))
			OutStyle.bSpecialLit = true;
		BaseEnd -= Token.Len();
	}
	if (BaseEnd <= Separator + 2)
	{
		OutStyle = FWoTRuntimeBSPSectionStyle();
		return true;
	}
	OutStyle.ConverterSlotBase = Slot.Left(BaseEnd);
	return true;
}

bool WoTRuntimeBSPFindDetailTexture(
	FStringView CanonicalPackage, FStringView CanonicalName,
	FWoTRuntimeBSPDetailTexture& OutDetail)
{
	OutDetail = FWoTRuntimeBSPDetailTexture();
	if (CanonicalPackage.IsEmpty() || CanonicalName.IsEmpty()) return false;
	for (const WoT::Retail::Generated::FWorldTexture& Texture
		: WoT::Retail::Generated::WorldTextures)
	{
		// Every source row of one canonical texture carries that texture's
		// canonical facts (generate_world_texture_registry.collect).
		if (!CanonicalPackage.Equals(FStringView(Texture.CanonicalPackage),
				ESearchCase::IgnoreCase)
			|| !CanonicalName.Equals(FStringView(Texture.CanonicalName),
				ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Texture.DetailWidth == 0 || Texture.DetailHeight == 0
			|| Texture.Width == 0 || Texture.Height == 0
			|| !(Texture.DrawScale > 0.0) || !(Texture.DetailDrawScale > 0.0))
		{
			return false;
		}
		const double RatioU = static_cast<double>(Texture.Width) * Texture.DrawScale
			/ (static_cast<double>(Texture.DetailWidth) * Texture.DetailDrawScale);
		const double RatioV = static_cast<double>(Texture.Height) * Texture.DrawScale
			/ (static_cast<double>(Texture.DetailHeight) * Texture.DetailDrawScale);
		if (!FMath::IsFinite(RatioU) || !FMath::IsFinite(RatioV)) return false;
		OutDetail.CanonicalPackage = Texture.DetailCanonicalPackage;
		OutDetail.CanonicalName = Texture.DetailCanonicalName;
		OutDetail.SourceTextureIdentity = Texture.DetailSourceTextureIdentity;
		OutDetail.RatioU = static_cast<float>(RatioU);
		OutDetail.RatioV = static_cast<float>(RatioV);
		return !OutDetail.CanonicalPackage.IsEmpty()
			&& !OutDetail.CanonicalName.IsEmpty()
			&& !OutDetail.SourceTextureIdentity.IsEmpty();
	}
	return false;
}

bool WoTRuntimeBSPSectionStyleForIdentity(
	FStringView CanonicalPackage, FStringView CanonicalName,
	uint32 SurfaceFlags, FWoTRuntimeBSPSectionStyle& OutStyle)
{
	OutStyle = FWoTRuntimeBSPSectionStyle();
	if (CanonicalPackage.IsEmpty() || CanonicalName.IsEmpty()
		|| CanonicalPackage.Contains(TEXT("__"))
		|| CanonicalName.Contains(TEXT("__"))) return false;
	OutStyle.ConverterSlotBase =
		FString(CanonicalPackage) + TEXT("__") + FString(CanonicalName);
	constexpr uint32 PFMasked = 0x00000002u;
	constexpr uint32 PFTranslucent = 0x00000004u;
	constexpr uint32 PFModulated = 0x00000040u;
	constexpr uint32 PFFakeBackdrop = 0x00000080u;
	constexpr uint32 PFTwoSided = 0x00000100u;
	constexpr uint32 PFNoSmooth = 0x00000800u;
	constexpr uint32 PFSpecialLit = 0x00100000u;
	constexpr uint32 PFUnlit = 0x00400000u;
	if ((SurfaceFlags & PFTranslucent) != 0)
		OutStyle.Blend = EWoTRuntimeBSPBlend::Translucent;
	else if ((SurfaceFlags & PFModulated) != 0)
		OutStyle.Blend = EWoTRuntimeBSPBlend::Modulated;
	else if ((SurfaceFlags & PFMasked) != 0)
		OutStyle.Blend = EWoTRuntimeBSPBlend::Masked;
	OutStyle.bTwoSided = (SurfaceFlags & PFTwoSided) != 0;
	OutStyle.bNoSmooth = (SurfaceFlags & PFNoSmooth) != 0;
	OutStyle.bUnlit = (SurfaceFlags & (PFUnlit | PFFakeBackdrop)) != 0;
	OutStyle.bSky = (SurfaceFlags & PFFakeBackdrop) != 0;
	OutStyle.bSpecialLit = (SurfaceFlags & PFSpecialLit) != 0;
	return true;
}

void WoTRuntimeBSPApplyTextureFlags(
	FWoTRuntimeBSPSectionStyle& Style, bool bTextureMasked)
{
	// Retail folds the texture flags into the BSP polygon flags before choosing
	// the blend.
	if (bTextureMasked && Style.Blend == EWoTRuntimeBSPBlend::Opaque)
	{
		Style.Blend = EWoTRuntimeBSPBlend::Masked;
	}
}

bool WoTRuntimeBSPBuildRenderMesh(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError)
{
	return WoTBuildBSPRenderMeshOnly(Mesh, Payload, OutError);
}

bool WoTRuntimeBSPBuildCollision(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError,
	TConstArrayView<uint32> SurfaceFlags)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("runtime BSP collision construction requires the game thread");
		return false;
	}
	if (Payload.bPortal)
	{
		Mesh.SetBodySetup(nullptr);
		return true;
	}
	const FWoTBSPPayloadData* CollisionPayload = &Payload;
	FWoTBSPPayloadData SolidPayload;
	// Version-10 multiplayer payloads always carry exact per-triangle source
	// surfaces. Older synthetic in-memory fixtures may omit them, but a partial
	// array remains corrupt rather than silently falling back.
	if (Payload.FormatVersion == 10 && Payload.TriangleSurfaces.IsEmpty())
	{
		OutError = TEXT("version-10 runtime BSP collision is missing surface provenance");
		return false;
	}
	if (!SurfaceFlags.IsEmpty() && !Payload.TriangleSurfaces.IsEmpty())
	{
		if (Payload.Indices.Num() % 3 != 0
			|| Payload.TriangleSurfaces.Num() != Payload.Indices.Num() / 3)
		{
			OutError = TEXT("runtime BSP collision surface provenance is malformed");
			return false;
		}
		SolidPayload = Payload;
		SolidPayload.Indices.Reset();
		SolidPayload.Sections.Reset();
		SolidPayload.TriangleSurfaces.Reset();
		constexpr uint32 PFNotSolid = 0x00000008u;
		for (const FWoTBSPPayloadSection& SourceSection : Payload.Sections)
		{
			const uint64 End = static_cast<uint64>(SourceSection.FirstIndex)
				+ SourceSection.IndexCount;
			if (SourceSection.FirstIndex % 3 != 0 || SourceSection.IndexCount % 3 != 0
				|| End > static_cast<uint64>(Payload.Indices.Num()))
			{
				OutError = TEXT("runtime BSP collision section range is malformed");
				return false;
			}
			FWoTBSPPayloadSection Added = SourceSection;
			Added.FirstIndex = static_cast<uint32>(SolidPayload.Indices.Num());
			Added.MinVertex = MAX_uint32;
			Added.MaxVertex = 0;
			for (uint32 At = SourceSection.FirstIndex; At < End; At += 3)
			{
				const uint32 Triangle = At / 3;
				const uint32 Surface = Payload.TriangleSurfaces[Triangle];
				if (!SurfaceFlags.IsValidIndex(static_cast<int32>(Surface)))
				{
					OutError = TEXT("runtime BSP collision surface index is out of range");
					return false;
				}
				if ((SurfaceFlags[Surface] & PFNotSolid) != 0) continue;
				for (uint32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 Vertex = Payload.Indices[At + Corner];
					SolidPayload.Indices.Add(Vertex);
					Added.MinVertex = FMath::Min(Added.MinVertex, Vertex);
					Added.MaxVertex = FMath::Max(Added.MaxVertex, Vertex);
				}
				SolidPayload.TriangleSurfaces.Add(Surface);
			}
			Added.IndexCount = static_cast<uint32>(SolidPayload.Indices.Num())
				- Added.FirstIndex;
			if (Added.IndexCount != 0) SolidPayload.Sections.Add(MoveTemp(Added));
		}
		if (SolidPayload.Indices.IsEmpty())
		{
			Mesh.SetBodySetup(nullptr);
			return true;
		}
		CollisionPayload = &SolidPayload;
	}
	UStaticMesh* CollisionSource = NewObject<UStaticMesh>(
		&Mesh, NAME_None, RF_Transient);
	if (CollisionSource == nullptr
		|| !WoTRuntimeBSPBuildRenderMesh(*CollisionSource, *CollisionPayload, OutError))
	{
		if (OutError.IsEmpty())
			OutError = TEXT("runtime BSP collision source allocation failed");
		return false;
	}
	CollisionSource->bAllowCPUAccess = true;
	if (!CollisionSource->ContainsPhysicsTriMeshData(false))
	{
		OutError = TEXT("runtime BSP has no collision-enabled triangle data");
		return false;
	}
	UBodySetup* Body = NewObject<UBodySetup>(
		CollisionSource, NAME_None, RF_Transient);
	if (Body == nullptr)
	{
		OutError = TEXT("runtime BSP body setup allocation failed");
		return false;
	}
	Body->AggGeom.EmptyElements();
	Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Body->bMeshCollideAll = false;
	Body->bNeverNeedsCookedCollisionData = false;
	Body->InvalidatePhysicsData();
	Body->CreatePhysicsMeshes();
	if (!Body->bCreatedPhysicsMeshes || Body->bFailedToCreatePhysicsMeshes)
	{
		OutError = TEXT("runtime BSP synchronous complex collision cook failed");
		return false;
	}
	if (!Body->Rename(nullptr, &Mesh,
		REN_DontCreateRedirectors | REN_DoNotDirty))
	{
		OutError = TEXT("runtime BSP cooked body ownership transfer failed");
		return false;
	}
	Mesh.SetBodySetup(Body);
	return true;
}

AWoTRuntimeBSPWorld::AWoTRuntimeBSPWorld()
{
	// Ticks only while it has animated light-map surfaces or scene-time
	// instances, never on a dedicated server (RefreshTickEnabled).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("RuntimeBSPRoot")));
}

bool AWoTRuntimeBSPWorld::ResolveInvisibleSolidCollisionFace(
	const UPrimitiveComponent* Component, int32 FaceIndex,
	uint32& OutSurface, FString& OutTextureIdentity) const
{
	OutSurface = MAX_uint32;
	OutTextureIdentity.Reset();
	if (Component == nullptr || Component != InvisibleSolidCollisionComponent.Get()
		|| FaceIndex < 0
		|| !InvisibleSolidCollisionTriangleSurfaces.IsValidIndex(FaceIndex)
		|| !InvisibleSolidCollisionTriangleTextureIdentities.IsValidIndex(FaceIndex))
	{
		return false;
	}
	OutSurface = InvisibleSolidCollisionTriangleSurfaces[FaceIndex];
	OutTextureIdentity =
		InvisibleSolidCollisionTriangleTextureIdentities[FaceIndex];
	return true;
}

void WoTLightmapComposeSurface(
	TConstArrayView<uint8> BaseBGRA, const uint32 AtlasWidth,
	const FWoTBSPAnimationSurface& Surface,
	TConstArrayView<const FWoTBSPAnimationTerm*> Terms,
	TConstArrayView<const FWoTBSPAnimationRect*> Rects,
	const FWoTLightmapAnimationFrame& Frame, TArray<uint8>& Out)
{
	const int32 TexelCount = static_cast<int32>(Surface.W * Surface.H);
	Out.SetNumUninitialized(TexelCount * 4);
	for (uint32 LocalY = 0; LocalY < Surface.H; ++LocalY)
	{
		for (uint32 LocalX = 0; LocalX < Surface.W; ++LocalX)
		{
			const uint32 TexelX = Surface.X + LocalX;
			const uint32 TexelY = Surface.Y + LocalY;
			const int32 BaseOffset = static_cast<int32>((TexelY * AtlasWidth + TexelX) * 4);
			const int32 Local = static_cast<int32>(LocalY * Surface.W + LocalX);
			float Channel[3] = {
				static_cast<float>(BaseBGRA[BaseOffset + 2]),
				static_cast<float>(BaseBGRA[BaseOffset + 1]),
				static_cast<float>(BaseBGRA[BaseOffset + 0])};
			if (!Frame.bFrozen)
			{
				for (int32 Index = 0; Index < Terms.Num(); ++Index)
				{
					const FWoTBSPAnimationTerm& Term = *Terms[Index];
					const FWoTBSPAnimationRect& Rect = *Rects[Index];
					float Coefficient = 1.0f;
					if (Term.Kind != 1)
					{
						Coefficient *= Term.Group < 4 ? Frame.Wave[Term.Group] : 0.0f;
					}
					if (Term.Kind != 0)
					{
						// UE1's FLightManager::Merge:
						// Torch 0.95+0.05*RandomBase(Key++), Fire 0.80+0.20*
						// RandomBase(Key++), WateryShimmer 0.60+0.40*Random(Key++)
						// over the surface-local light-map key. The mean is in
						// the base atlas and the response is the half range, so
						// the centered coefficient is 2*rand-1.
						if (Frame.Randoms == nullptr) continue;
						const float Rand = Term.Effect == 3
							? Frame.Randoms->Random(Local)
							: Frame.Randoms->RandomBase(Local);
						Coefficient *= 2.0f * Rand - 1.0f;
					}
					if (Coefficient == 0.0f) continue;
					const uint8* Response = Rect.RGB.GetData() + Local * 3;
					Channel[0] += Coefficient * static_cast<float>(Response[0]);
					Channel[1] += Coefficient * static_cast<float>(Response[1]);
					Channel[2] += Coefficient * static_cast<float>(Response[2]);
				}
			}
			uint8* Dest = Out.GetData() + Local * 4;
			// Texture-range clamp (the material floors at 0 and ceilings at
			// 254/255) then the byte quantization the atlas sampler reads.
			Dest[2] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Channel[0]), 0, 254));
			Dest[1] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Channel[1]), 0, 254));
			Dest[0] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Channel[2]), 0, 254));
			Dest[3] = 255;
		}
	}
}

void AWoTRuntimeBSPWorld::SetLightmapAnimation(
	const FWoTBSPPayloadData& Payload, UTexture2D* SharedLightmap)
{
	AnimatedLightmap = nullptr;
	AffectedSurfaces.Reset();
	AnimationSurfaces.Reset();
	AnimationGroups.Reset();
	AnimationTerms.Reset();
	for (TSharedPtr<FUploadSlot, ESPMode::ThreadSafe>& Slot : UploadSlots) Slot.Reset();
	LastUploadBatchCount = 0;
	LastUploadRegionCount = 0;
	bHasComposedFrame = false;
	// A dedicated server has nothing to draw: it never adopts the trailer
	// (no base copy, no staging, no tick).
	if (!SharedLightmap || !Payload.HasLightmapAnimation()
		|| !WoTPresentationAnimationEnabled(GetWorld())
		|| Payload.LightmapWidth == 0 || Payload.LightmapHeight == 0
		|| static_cast<uint64>(Payload.LightmapWidth) * Payload.LightmapHeight * 4
			!= static_cast<uint64>(Payload.LightmapBGRA.Num()))
	{
		RefreshTickEnabled();
		return;
	}
	AnimatedLightmap = SharedLightmap;
	BaseLightmapBGRA = Payload.LightmapBGRA;
	AtlasWidth = Payload.LightmapWidth;
	AtlasHeight = Payload.LightmapHeight;
	AnimationSurfaces = Payload.AnimationSurfaces;
	AnimationGroups = Payload.AnimationGroups;
	AnimationTerms = Payload.AnimationTerms;
	TMap<uint32, int32> AffectedByIndex;
	for (const FWoTBSPAnimationTerm& Term : AnimationTerms)
	{
		for (const FWoTBSPAnimationRect& Rect : Term.Rects)
		{
			int32* Existing = AffectedByIndex.Find(Rect.SurfaceIndex);
			if (Existing == nullptr)
			{
				FAffectedSurface& Added = AffectedSurfaces.AddDefaulted_GetRef();
				Added.Surface = Rect.SurfaceIndex;
				Existing = &AffectedByIndex.Add(Rect.SurfaceIndex, AffectedSurfaces.Num() - 1);
			}
			AffectedSurfaces[*Existing].Terms.Add(&Term);
			AffectedSurfaces[*Existing].Rects.Add(&Rect);
		}
	}
	ShimmerRandoms.Reset(WoTLightmapShimmerSeed);
	RefreshTickEnabled();
}

TSharedPtr<AWoTRuntimeBSPWorld::FUploadSlot, ESPMode::ThreadSafe>
AWoTRuntimeBSPWorld::AcquireUploadSlot()
{
	for (TSharedPtr<FUploadSlot, ESPMode::ThreadSafe>& Slot : UploadSlots)
	{
		if (!Slot.IsValid())
		{
			// Allocated once per animated world; the decoder bounds the atlas.
			Slot = MakeShared<FUploadSlot, ESPMode::ThreadSafe>();
			Slot->Page.SetNumUninitialized(
				static_cast<int32>(static_cast<uint64>(AtlasWidth) * AtlasHeight * 4));
			Slot->Regions.SetNum(AffectedSurfaces.Num());
			return Slot;
		}
		if (!Slot->bInFlight.load(std::memory_order_acquire))
		{
			return Slot;
		}
	}
	return nullptr;
}

uint64 AWoTRuntimeBSPWorld::GetUploadStagingBytes() const
{
	uint64 Bytes = 0;
	for (const TSharedPtr<FUploadSlot, ESPMode::ThreadSafe>& Slot : UploadSlots)
	{
		if (Slot.IsValid())
		{
			Bytes += static_cast<uint64>(Slot->Page.Num())
				+ static_cast<uint64>(Slot->Regions.Num()) * sizeof(FUpdateTextureRegion2D);
		}
	}
	return Bytes;
}

void AWoTRuntimeBSPWorld::RecomposeLightmap(const float TimeSeconds, const bool bFrozen)
{
	LastUploadBatchCount = 0;
	LastUploadRegionCount = 0;
	if (!AnimatedLightmap || AffectedSurfaces.IsEmpty()) return;
	FWoTLightmapAnimationFrame Frame;
	Frame.bFrozen = bFrozen;
	Frame.NoiseFrame = LastFrame.NoiseFrame;
	if (!bFrozen)
	{
		for (int32 Index = 0; Index < AnimationGroups.Num() && Index < 4; ++Index)
		{
			const FWoTBSPAnimationGroup& Group = AnimationGroups[Index];
			const float Mean =
				UWoTDynamicLightSubsystem::RetailWaveMean(Group.Type);
			const float Amplitude =
				UWoTDynamicLightSubsystem::RetailWaveAmplitude(Group.Type);
			const float Contribution =
				UWoTDynamicLightSubsystem::EvaluateRetailWaveform(
					Group.Type, TimeSeconds, Group.Period, Group.Phase, 0, 0);
			Frame.Wave[Index] = Amplitude > 0.0f
				? (Contribution - Mean) / Amplitude : 0.0f;
		}
		// GRandoms->Tick runs once per rendered frame with the level clock
		// (the original renderer): the bases regenerate now, the
		// continuous table advances by the elapsed 35 Hz server ticks.
		ShimmerRandoms.Tick(TimeSeconds);
		Frame.Randoms = &ShimmerRandoms;
		Frame.NoiseFrame = (LastFrame.NoiseFrame + 1u) & AWoTLightWave::NoiseFrameMask;
	}
	// A frozen frame uploads the literal base once and then stays put.
	if (bFrozen && bHasComposedFrame && LastFrame.bFrozen) return;
	// Every staging slot still in flight: drop this frame's upload and retry
	// next tick rather than write under the render thread's copy.
	TSharedPtr<FUploadSlot, ESPMode::ThreadSafe> Slot = AcquireUploadSlot();
	if (!Slot.IsValid())
	{
		++SkippedUploadFrames;
		return;
	}
	LastFrame = Frame;
	bHasComposedFrame = true;
	// One padded block per surface (interior plus the replicated one-texel
	// gutter, matching lightmap_atlas.py edge padding) written at its own
	// atlas position in the staging page, so every region shares the page
	// pitch and reads Data + SrcY*Pitch + SrcX*4 (UTexture2D::UpdateTextureRegions).
	const uint32 Pitch = AtlasWidth * 4;
	uint8* Page = Slot->Page.GetData();
	for (int32 Index = 0; Index < AffectedSurfaces.Num(); ++Index)
	{
		const FAffectedSurface& Affected = AffectedSurfaces[Index];
		const FWoTBSPAnimationSurface& Surface = AnimationSurfaces[Affected.Surface];
		WoTLightmapComposeSurface(BaseLightmapBGRA, AtlasWidth, Surface,
			Affected.Terms, Affected.Rects, Frame, ComposeScratch);
		const uint32 PaddedW = Surface.W + 2;
		const uint32 PaddedH = Surface.H + 2;
		const uint32 OriginX = Surface.X - 1;
		const uint32 OriginY = Surface.Y - 1;
		for (uint32 PaddedY = 0; PaddedY < PaddedH; ++PaddedY)
		{
			const uint32 SourceY = FMath::Clamp<int32>(static_cast<int32>(PaddedY) - 1,
				0, static_cast<int32>(Surface.H) - 1);
			uint8* Row = Page + (static_cast<uint64>(OriginY + PaddedY) * Pitch)
				+ static_cast<uint64>(OriginX) * 4;
			for (uint32 PaddedX = 0; PaddedX < PaddedW; ++PaddedX)
			{
				const uint32 SourceX = FMath::Clamp<int32>(static_cast<int32>(PaddedX) - 1,
					0, static_cast<int32>(Surface.W) - 1);
				FMemory::Memcpy(Row + PaddedX * 4,
					ComposeScratch.GetData() + (SourceY * Surface.W + SourceX) * 4, 4);
			}
		}
		Slot->Regions[Index] = FUpdateTextureRegion2D(
			OriginX, OriginY, OriginX, OriginY, PaddedW, PaddedH);
	}
	// One batch: every region in one UpdateTextureRegions call sharing the
	// page pitch. Without an RHI resource (headless) the batch is staged but
	// nothing is enqueued.
	LastUploadBatchCount = 1;
	LastUploadRegionCount = Slot->Regions.Num();
	if (!AnimatedLightmap->GetResource()) return;
	Slot->bInFlight.store(true, std::memory_order_release);
	TSharedPtr<FUploadSlot, ESPMode::ThreadSafe> Retained = Slot;
	AnimatedLightmap->UpdateTextureRegions(0, static_cast<uint32>(Slot->Regions.Num()),
		Slot->Regions.GetData(), Pitch, 4, Page,
		[Retained](uint8*, const FUpdateTextureRegion2D*)
		{
			Retained->bInFlight.store(false, std::memory_order_release);
		});
	++TotalUploadCallCount;
}

bool AWoTRuntimeBSPWorld::ShouldTick() const
{
	// One predicate for both per-frame duties: the animated light-map
	// recomposition and the WoTSceneTime pan driver.
	// Neither has anything to draw on a dedicated server.
	return (!AffectedSurfaces.IsEmpty() || !SceneTimeMaterials.IsEmpty()
		|| SkyBoxes.Num() > 1)
		&& WoTPresentationAnimationEnabled(GetWorld());
}

void AWoTRuntimeBSPWorld::RefreshTickEnabled()
{
	const bool bTick = ShouldTick();
	// Before BeginPlay the start flag decides registration; afterwards the
	// live tick function does. Keep both in step whichever side changes.
	PrimaryActorTick.bStartWithTickEnabled = bTick;
	SetActorTickEnabled(bTick);
}

void AWoTRuntimeBSPWorld::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const UWorld* OwningWorld = GetWorld();
	if (!OwningWorld) return;
	// The world clock drives both, so a light wave and a WoTSceneTime-panned
	// layer stay in phase with the engine Time node of the opaque/masked
	// world masters (paused with it).
	const float Now = static_cast<float>(OwningWorld->GetTimeSeconds());
	if (HasLightmapAnimation())
		RecomposeLightmap(Now, AWoTLightWave::IsFreezeEnabled());
	ApplySceneTime(Now);
	if (SkyBoxes.Num() > 1)
	{
		for (FConstPlayerControllerIterator It = OwningWorld->GetPlayerControllerIterator();
			It; ++It)
		{
			APlayerController* Controller = It->Get();
			if (Controller == nullptr || !Controller->IsLocalController()) continue;
			FVector ViewPoint;
			FRotator ViewRotation;
			Controller->GetPlayerViewPoint(ViewPoint, ViewRotation);
			ApplySkyForViewPoint(ViewPoint);
			break;
		}
	}
}

void AWoTRuntimeBSPWorld::ApplySkyForViewPoint(const FVector& ViewPoint)
{
	const int32 Zone = WoTRuntimeMapPointZone(SkyZoneNodes, ViewPoint);
	const int32 ZonePlan = SkyPlanByZone.IsValidIndex(Zone)
		? SkyPlanByZone[Zone] : INDEX_NONE;
	BindSkyWindowsToPlan(SkyBoxes.IsValidIndex(ZonePlan) ? ZonePlan : 0);
}

void AWoTRuntimeBSPWorld::BindSkyWindowsToPlan(const int32 PlanIndex)

{
	if (!SkyBoxes.IsValidIndex(PlanIndex)) return;
	AWoTSkyBox* Box = SkyBoxes[PlanIndex].Get();
	UTextureRenderTargetCube* Cube = Box != nullptr
		? Box->GetSkyCaptureTarget() : nullptr;
	if (Cube == nullptr) return;
	for (UMaterialInstanceDynamic* Material : SkyWindowMaterials)
	{
		if (IsValid(Material))
			Material->SetTextureParameterValue(WoTRuntimeSkyWindowCubeParameter, Cube);
	}
	// One live cube per frame: the bound plan captures, the others idle.
	for (int32 Index = 0; Index < SkyBoxes.Num(); ++Index)
	{
		if (AWoTSkyBox* Plan = SkyBoxes[Index].Get())
			Plan->SetSkyWindowCaptureActive(Index == PlanIndex);
	}
	ActiveSkyPlan = PlanIndex;
}

void AWoTRuntimeBSPWorld::ApplySceneTime(const float Seconds)
{
	for (UMaterialInstanceDynamic* Material : SceneTimeMaterials)
	{
		if (IsValid(Material))
			Material->SetScalarParameterValue(TEXT("WoTSceneTime"), Seconds);
	}
}
