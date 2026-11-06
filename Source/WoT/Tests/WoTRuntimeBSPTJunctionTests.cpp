#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTMover.h"
#include "WoTTestWorld.h"
#include "WoTX6BrazierHallTestUtils.h"
#include "WoTPayloadActivationTestSupport.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTGameInstance.h"
#include "HAL/FileManager.h"
#include "WoTRetailReader/TextureSound.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/BodySetup.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
void AddTJunctionTriangle(FWoTBSPPayloadData& Payload,
	const FVector3f& A, const FVector3f& B, const FVector3f& C,
	uint32 Surface, bool bNewSection)
{
	const uint32 FirstVertex = static_cast<uint32>(Payload.Positions.Num());
	const uint32 FirstIndex = static_cast<uint32>(Payload.Indices.Num());
	for (const FVector3f& Position : { A, B, C })
	{
		Payload.Positions.Add(Position);
		Payload.Normals.Add(FVector3f::ZAxisVector);
		Payload.UV0.Add(FVector2f(20.0f, 30.0f));
		Payload.UV1.Add(FVector2f(40.0f, 50.0f));
		Payload.Indices.Add(static_cast<uint32>(Payload.Positions.Num() - 1));
	}
	Payload.TriangleSurfaces.Add(Surface);
	if (bNewSection)
	{
		FWoTBSPPayloadSection& Section = Payload.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = FString::Printf(TEXT("slot_%u"), Surface);
		Section.FirstIndex = FirstIndex;
		Section.MinVertex = FirstVertex;
		Section.bCollisionEnabled = true;
		Section.CanonicalPackage = TEXT("TestPackage");
		Section.CanonicalName = TEXT("TestTexture");
		Section.SourceTextureIdentity = TEXT("TestPackage.TestTexture");
		Section.SurfaceFlags = 0x1234;
		Section.PanZoneActorIdentity = TEXT("PanZone");
		Section.MacroCanonicalPackage = TEXT("MacroPackage");
		Section.MacroCanonicalName = TEXT("MacroTexture");
		Section.MacroSourceTextureIdentity = TEXT("MacroPackage.MacroTexture");
		Section.MacroRatio = 0.25f;
		Section.MacroOffsetU = 0.5f;
		Section.MacroOffsetV = 0.75f;
	}
	Payload.Sections.Last().IndexCount += 3;
	Payload.Sections.Last().MaxVertex = FirstVertex + 2;
}

void AddTJunctionDonor(FWoTBSPPayloadData& Payload,
	const FVector3f& Position, uint32 Surface, bool bNewSection)
{
	AddTJunctionTriangle(Payload, Position,
		Position + FVector3f(0, 0, 40),
		Position + FVector3f(0, 10, 40), Surface, bNewSection);
}

// Independent geometric audit: every indexed vertex in every part against
// every triangle edge. Deliberately brute force for these tiny fixtures.
int32 CountTJunctions(const TArray<FWoTBSPPayloadData>& Parts)
{
	int32 Count = 0;
	for (const FWoTBSPPayloadData& Host : Parts)
	{
		for (int32 At = 0; At < Host.Indices.Num(); At += 3)
		{
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const FVector3d A(Host.Positions[Host.Indices[At + Edge]]);
				const FVector3d B(Host.Positions[Host.Indices[At + (Edge + 1) % 3]]);
				const FVector3d Third(Host.Positions[Host.Indices[At + (Edge + 2) % 3]]);
				const FVector3d Delta = B - A;
				// A nonzero sibling triangle A-B-P fills the sliver beside
				// this exact internal diagonal; P is not an unmatched junction.
				TSet<FVector3f> ConnectedThirds;
				for (const FWoTBSPPayloadData& Other : Parts)
					for (int32 OtherAt = 0; OtherAt < Other.Indices.Num(); OtherAt += 3)
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							const FVector3d X(Other.Positions[Other.Indices[OtherAt + Corner]]);
							const FVector3d Y(Other.Positions[Other.Indices[OtherAt + (Corner + 1) % 3]]);
							const FVector3f Z = Other.Positions[Other.Indices[OtherAt + (Corner + 2) % 3]];
							if (((X == A && Y == B) || (X == B && Y == A))
								&& FVector3d::CrossProduct(Y - X, FVector3d(Z) - X).SizeSquared() > 0)
								ConnectedThirds.Add(Z);
						}
				for (const FWoTBSPPayloadData& Donor : Parts)
				{
					for (uint32 Vertex : Donor.Indices)
					{
						const FVector3d P(Donor.Positions[Vertex]);
						if (ConnectedThirds.Contains(Donor.Positions[Vertex])) continue;
						// A triangle's own geometric corners do not form
						// cross-polygon T-junctions against that triangle.
						if ((P - Third).SizeSquared() <= 0.0001) continue;
						if ((P - A).SizeSquared() <= 0.0001
							|| (P - B).SizeSquared() <= 0.0001) continue;
						const double T = FVector3d::DotProduct(P - A, Delta)
							/ Delta.SizeSquared();
						if (T > 0 && T < 1
							&& (P - (A + T * Delta)).SizeSquared() <= 0.0001)
						{
							++Count;
						}
					}
				}
			}
		}
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPTJunctionRepairSpec,
	"WoT.World.RuntimeBSP.TJunctionRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPTJunctionRepairSpec::RunTest(const FString&)
{
	// Removing the joint repair, dropping either anchor-adjacent edge, using
	// donor UVs, or losing child provenance must fail these geometric checks.
	TArray<FWoTBSPPayloadData> Parts;
	Parts.SetNum(2);
	AddTJunctionTriangle(Parts[0], FVector3f(-256, -256, 0),
		FVector3f(256, -256, 0), FVector3f(-256, 256, 0), 17, true);
	Parts[0].UV0[0] = FVector2f(0, 0);
	Parts[0].UV0[1] = FVector2f(1, 0);
	Parts[0].UV0[2] = FVector2f(0, 1);
	Parts[0].UV1[0] = FVector2f(2, 3);
	Parts[0].UV1[1] = FVector2f(4, 3);
	Parts[0].UV1[2] = FVector2f(2, 7);
	Parts[0].Normals[1] = FVector3f(0, 0.6f, 0.8f);
	Parts[0].Normals[2] = FVector3f(0.6f, 0, 0.8f);
	AddTJunctionDonor(Parts[0], FVector3f(0, -256, 0), 23, true);
	AddTJunctionDonor(Parts[1], FVector3f(-128, -256, 0), 29, true);
	AddTJunctionDonor(Parts[1], FVector3f(-256, 0, 0), 31, false);
	AddTJunctionDonor(Parts[1], FVector3f(0, 0, 0), 37, false);
	AddTJunctionDonor(Parts[1], FVector3f(128, -256, 0.01f), 41, false);
	AddTJunctionDonor(Parts[1], FVector3f(192, -256, 0.011f), 43, false);
	// Duplicate position has different UVs/identity and must not add a sliver.
	AddTJunctionDonor(Parts[1], FVector3f(0, -256, 0), 47, false);
	Parts[0].FormatVersion = 10;
	Parts[0].bRootOutside = true;
	Parts[0].BoundsMin = FVector3f(-256, -256, 0);
	Parts[0].BoundsMax = FVector3f(256, 256, 40);
	Parts[0].LightmapWidth = Parts[0].LightmapHeight = 1;
	Parts[0].LightmapBGRA = { 1, 2, 3, 4 };
	Parts[0].EntrySurfaces.AddDefaulted();
	Parts[0].EntryLights.AddDefaulted();
	Parts[0].EntryIllumination.AddDefaulted();
	Parts[0].AnimationSurfaces.AddDefaulted();
	Parts[0].AnimationGroups.AddDefaulted();
	Parts[0].AnimationTerms.AddDefaulted();
	Parts[0].SkyRest.bPresent = true;
	const TArray<FWoTBSPPayloadData> Original = Parts;
	TestTrue(TEXT("fixture has proper edge-interior vertices"), CountTJunctions(Parts) > 0);
	FWoTBSPTJunctionStats Stats;
	FString Error;
	if (!TestTrue(TEXT("render repair succeeds"),
		WoTRuntimeBSPRepairRenderTJunctions(Parts, Stats, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("joint repair leaves zero proper T-junctions"), CountTJunctions(Parts), 0);
	TestTrue(TEXT("cross-section host boundary is split"), Parts[0].Sections[0].IndexCount > 3);
	TestEqual(TEXT("only affected triangle is retriangulated"), Stats.SplitTriangles, uint64(1));
	TestEqual(TEXT("closure adds sixth host edge insertion"), Stats.InsertedEdgeVertices, uint64(6));
	TestEqual(TEXT("input triangle census"), Stats.InputTriangles, uint64(8));
	TestEqual(TEXT("output triangle census"), Stats.OutputTriangles, uint64(14));
	const FWoTBSPPayloadData& Repaired = Parts[0];
	const FWoTBSPPayloadSection& HostSection = Repaired.Sections[0];
	TestEqual(TEXT("nine boundary vertices become seven children"), HostSection.IndexCount, 21u);
	TestEqual(TEXT("repair creates only inserted edge positions"), Repaired.Positions.Num(), Original[0].Positions.Num() + 6);
	double TwiceArea = 0;
	for (uint32 At = 0; At < HostSection.IndexCount; At += 3)
	{
		const FVector3d A(Repaired.Positions[Repaired.Indices[At]]);
		const FVector3d B(Repaired.Positions[Repaired.Indices[At + 1]]);
		const FVector3d C(Repaired.Positions[Repaired.Indices[At + 2]]);
		const FVector3d Cross = FVector3d::CrossProduct(B - A, C - A);
		TestTrue(TEXT("child preserves winding and nonzero area"), Cross.Z > 0);
		TwiceArea += Cross.Z;
		TestEqual(TEXT("child inherits source surface"), Repaired.TriangleSurfaces[At / 3], 17u);
	}
	TestTrue(TEXT("triangulation preserves host area"), FMath::Abs(TwiceArea - 262144.0) < 0.01);
	for (const FVector3f& Expected : { FVector3f(-128, -256, 0),
		FVector3f(0, -256, 0), FVector3f(128, -256, 0.01f),
		FVector3f(192, -256, 0.011f),
		FVector3f(-256, 0, 0), FVector3f(0, 0, 0) })
	{
		bool bFound = false;
		for (int32 Vertex = Original[0].Positions.Num(); Vertex < Repaired.Positions.Num(); ++Vertex)
		{
			if (!Repaired.Positions[Vertex].Equals(Expected, 0.0001f)) continue;
			bFound = true;
			const float U = (Expected.X + 256) / 512;
			const float V = (Expected.Y + 256) / 512;
			TestTrue(TEXT("host edge UV0 interpolated"), Repaired.UV0[Vertex].Equals(FVector2f(U, V), 0.0001f));
			TestTrue(TEXT("host edge UV1 interpolated"), Repaired.UV1[Vertex].Equals(FVector2f(2 + 2 * U, 3 + 4 * V), 0.0001f));
			const FVector3f ExpectedNormal = Expected.X == 192
				? FVector3f(0, 0.5355849f, 0.8444814f)
				: FVector3f(0.6f * V, 0.6f * U, 1 - 0.2f * U - 0.2f * V).GetSafeNormal();
			TestTrue(TEXT("host normal interpolated and normalized"), Repaired.Normals[Vertex].Equals(ExpectedNormal, 0.0001f));
		}
		TestTrue(TEXT("exact donor position shared including epsilon limit"), bFound);
	}
	TestFalse(TEXT("outside-epsilon point is not projected"), Repaired.Positions.Contains(FVector3f(192, -256, 0)));
	const TArray<FVector3f> ExpectedBoundary = {
		FVector3f(-256, -256, 0), FVector3f(-128, -256, 0),
		FVector3f(0, -256, 0), FVector3f(128, -256, 0.01f),
		FVector3f(192, -256, 0.011f),
		FVector3f(256, -256, 0), FVector3f(0, 0, 0),
		FVector3f(-256, 256, 0), FVector3f(-256, 0, 0) };
	for (int32 Segment = 0; Segment < ExpectedBoundary.Num(); ++Segment)
	{
		bool bFound = false;
		for (uint32 At = 0; At < HostSection.IndexCount; At += 3)
			for (uint32 Edge = 0; Edge < 3; ++Edge)
				bFound |= Repaired.Positions[Repaired.Indices[At + Edge]] == ExpectedBoundary[Segment]
					&& Repaired.Positions[Repaired.Indices[At + (Edge + 1) % 3]] == ExpectedBoundary[(Segment + 1) % ExpectedBoundary.Num()];
		TestTrue(TEXT("every inserted boundary subedge survives triangulation"), bFound);
	}
	TestEqual(TEXT("unaffected part retains original topology"), Parts[1].Indices, Original[1].Indices);
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Actual = Parts[PartIndex];
		const FWoTBSPPayloadData& Before = Original[PartIndex];
		TestEqual(TEXT("section count retained"), Actual.Sections.Num(), Before.Sections.Num());
		uint32 NextIndex = 0;
		for (int32 SectionIndex = 0; SectionIndex < Actual.Sections.Num(); ++SectionIndex)
		{
			const FWoTBSPPayloadSection& Section = Actual.Sections[SectionIndex];
			const FWoTBSPPayloadSection& Prior = Before.Sections[SectionIndex];
			TestEqual(TEXT("section ranges contiguous"), Section.FirstIndex, NextIndex);
			NextIndex += Section.IndexCount;
			TestEqual(TEXT("material preserved"), Section.MaterialSlot, Prior.MaterialSlot);
			TestEqual(TEXT("macro identity preserved"), Section.MacroSourceTextureIdentity, Prior.MacroSourceTextureIdentity);
			TestEqual(TEXT("macro transform preserved"), Section.MacroOffsetV, Prior.MacroOffsetV);
			TestEqual(TEXT("flags preserved"), Section.SurfaceFlags, Prior.SurfaceFlags);
			TestEqual(TEXT("pan zone preserved"), Section.PanZoneActorIdentity, Prior.PanZoneActorIdentity);
			for (uint32 At = Section.FirstIndex; At < NextIndex; ++At)
			{
				TestTrue(TEXT("section vertex bounds rebuilt"), Actual.Indices[At] >= Section.MinVertex && Actual.Indices[At] <= Section.MaxVertex);
			}
		}
		TestEqual(TEXT("section coverage complete"), NextIndex, static_cast<uint32>(Actual.Indices.Num()));
		for (int32 Vertex = 0; Vertex < Before.Positions.Num(); ++Vertex)
		{
			TestTrue(TEXT("original position retained"), Actual.Positions[Vertex] == Before.Positions[Vertex]);
			TestTrue(TEXT("original UV retained"), Actual.UV0[Vertex] == Before.UV0[Vertex]);
		}
	}
	TestEqual(TEXT("lightmap bytes preserved"), Repaired.LightmapBGRA, Original[0].LightmapBGRA);
	TestEqual(TEXT("format preserved"), Repaired.FormatVersion, uint16(10));
	TestTrue(TEXT("root flag preserved"), Repaired.bRootOutside);
	TestTrue(TEXT("bounds preserved"), Repaired.BoundsMin == Original[0].BoundsMin && Repaired.BoundsMax == Original[0].BoundsMax);
	TestTrue(TEXT("trailers preserved"), Repaired.EntrySurfaces.Num() == 1
		&& Repaired.EntryLights.Num() == 1 && Repaired.EntryIllumination.Num() == 1
		&& Repaired.AnimationSurfaces.Num() == 1 && Repaired.AnimationGroups.Num() == 1
		&& Repaired.AnimationTerms.Num() == 1 && Repaired.SkyRest.bPresent);
	TArray<FWoTBSPPayloadData> MissingProvenance = Original;
	for (FWoTBSPPayloadData& Part : MissingProvenance) Part.TriangleSurfaces.Reset();
	TestTrue(TEXT("absent provenance accepted"), WoTRuntimeBSPRepairRenderTJunctions(MissingProvenance, Stats, Error));
	TestTrue(TEXT("absent provenance remains absent"), MissingProvenance[0].TriangleSurfaces.IsEmpty());
	TArray<FWoTBSPPayloadData> Invalid = Original;
	Invalid[1].TriangleSurfaces.Pop();
	TestFalse(TEXT("partial provenance fails closed"), WoTRuntimeBSPRepairRenderTJunctions(Invalid, Stats, Error));
	TestTrue(TEXT("failure gives actionable error"), !Error.IsEmpty());
	TestEqual(TEXT("failure does not partially modify earlier parts"), Invalid[0].Indices, Original[0].Indices);
	Invalid = Original;
	Invalid[1].UV1.Pop();
	TestFalse(TEXT("incomplete lightmap attributes fail closed"), WoTRuntimeBSPRepairRenderTJunctions(Invalid, Stats, Error));
	Invalid = Original;
	Invalid[1].Indices[0] = MAX_uint32;
	TestFalse(TEXT("invalid index fails closed"), WoTRuntimeBSPRepairRenderTJunctions(Invalid, Stats, Error));
	Invalid = Original;
	Invalid[1].Sections[0].FirstIndex = 3;
	TestFalse(TEXT("uncovered section range fails closed"), WoTRuntimeBSPRepairRenderTJunctions(Invalid, Stats, Error));
	TArray<FWoTBSPPayloadData> Repeated = Original;
	TestTrue(TEXT("same input repairs repeatedly"), WoTRuntimeBSPRepairRenderTJunctions(Repeated, Stats, Error));
	TestEqual(TEXT("repair topology deterministic"), Repeated[0].Indices, Parts[0].Indices);
	TestEqual(TEXT("repair positions deterministic"), Repeated[0].Positions, Parts[0].Positions);
	TArray<FWoTBSPPayloadData> LateFailure = Original;
	AddTJunctionTriangle(LateFailure[1], FVector3f(1024, 0, 0),
		FVector3f(1024.018f, 0, 0), FVector3f(1024, 1, 0), 59, false);
	AddTJunctionDonor(LateFailure[1], FVector3f(1024.000597528531f, 0.838426292f, 0), 61, false);
	AddTJunctionDonor(LateFailure[1], FVector3f(1024.00926849805f, 0.838473916f, 0), 63, false);
	AddTJunctionDonor(LateFailure[1], FVector3f(1023.99005003367f, 0.851055741f, 0), 65, false);
	TestTrue(TEXT("self-intersecting loop in a later part falls back locally"),
		WoTRuntimeBSPRepairRenderTJunctions(LateFailure, Stats, Error));
	TestTrue(TEXT("late crossing reports a source fallback"), Stats.FallbackTriangles > 0);
	TestEqual(TEXT("late fallback retains earlier repaired positions"),
		LateFailure[0].Positions, Parts[0].Positions);
	TestEqual(TEXT("late fallback retains earlier repaired indices"),
		LateFailure[0].Indices, Parts[0].Indices);
	TArray<FWoTBSPPayloadData> QuantizedParts = Original;
	QuantizedParts.SetNum(3);
	AddTJunctionTriangle(QuantizedParts[1], FVector3f(65536, 65536, 0),
		FVector3f(65538, 65539, 0), FVector3f(65536, 65636, 0), 91, true);
	// Both sources project to the same rounded point, over 0.01 from the
	// second source. Emitting exact source positions avoids quantization
	// error and must preserve these distinct shared boundary vertices.
	AddTJunctionDonor(QuantizedParts[2], FVector3f(65536.0234375f, 65536.03125f, 0), 97, true);
	AddTJunctionDonor(QuantizedParts[2], FVector3f(65536.015625f, 65536.0390625f, 0), 101, false);
	const TArray<FWoTBSPPayloadData> BeforeQuantization = QuantizedParts;
	TestTrue(TEXT("oblique join uses exact source positions despite projected rounding"),
		WoTRuntimeBSPRepairRenderTJunctions(QuantizedParts, Stats, Error));
	TestTrue(TEXT("oblique join shares first source position exactly"),
		QuantizedParts[1].Positions.Contains(FVector3f(65536.0234375f, 65536.03125f, 0)));
	TestTrue(TEXT("oblique join shares second source position exactly"),
		QuantizedParts[1].Positions.Contains(FVector3f(65536.015625f, 65536.0390625f, 0)));
	for (int32 PartIndex = 0; PartIndex < QuantizedParts.Num(); ++PartIndex)
	{
		for (int32 Vertex = 0; Vertex < BeforeQuantization[PartIndex].Positions.Num(); ++Vertex)
			TestTrue(TEXT("source sharing retains all original positions"), QuantizedParts[PartIndex].Positions[Vertex] == BeforeQuantization[PartIndex].Positions[Vertex]);
	}
	TArray<FWoTBSPPayloadData> CloseParts;
	CloseParts.SetNum(2);
	AddTJunctionTriangle(CloseParts[0], FVector3f(0, 0, 0),
		FVector3f(16, 0, 0), FVector3f(0, 16, 0), 61, true);
	AddTJunctionDonor(CloseParts[1], FVector3f(0.009f, 0, 0.009f), 67, true);
	AddTJunctionDonor(CloseParts[1], FVector3f(8, 0, 0.009f), 71, false);
	AddTJunctionDonor(CloseParts[1], FVector3f(8.009f, 0, 0.009f), 73, false);
	TestTrue(TEXT("near-endpoint and nearby distinct candidates repair"),
		WoTRuntimeBSPRepairRenderTJunctions(CloseParts, Stats, Error));
	TestEqual(TEXT("nearby candidates leave zero proper junctions"), CountTJunctions(CloseParts), 0);
	TestEqual(TEXT("distinct close projections are retained"), Stats.InsertedEdgeVertices, uint64(3));
	TArray<FWoTBSPPayloadData> ShortParts;
	ShortParts.SetNum(2);
	AddTJunctionTriangle(ShortParts[0], FVector3f(0, 0, 0),
		FVector3f(0.018f, 0, 0), FVector3f(0, 1, 0), 79, true);
	AddTJunctionDonor(ShortParts[1], FVector3f(0.009f, 0, 0.009f), 83, true);
	TestTrue(TEXT("short edge with proper interior candidate repairs"),
		WoTRuntimeBSPRepairRenderTJunctions(ShortParts, Stats, Error));
	TestEqual(TEXT("thin triangle leaves zero proper junctions"), CountTJunctions(ShortParts), 0);
	TestEqual(TEXT("thin repair introduces only its boundary insertion"), ShortParts[0].Positions.Num(), 4);
	TestEqual(TEXT("short nonzero edge accepts original candidate"), Stats.InsertedEdgeVertices, uint64(1));
	TestTrue(TEXT("short edge shares original donor"), ShortParts[0].Positions.Contains(FVector3f(0.009f, 0, 0.009f)));
	// Captured from authenticated Battle2_02 part 0 triangle 80. These
	// opposite boundary chains are closer than epsilon, so a rounded new
	// centroid can fall outside the original sliver and reverse child faces.
	TArray<FWoTBSPPayloadData> RetailSliver;
	RetailSliver.SetNum(2);
	AddTJunctionTriangle(RetailSliver[0], FVector3f(-17470.9863f, -8951.99707f, 21030.0312f),
		FVector3f(-17470.9883f, -8952, 21023.3281f),
		FVector3f(-17470.9863f, -8952.00293f, 21003.2246f), 103, true);
	AddTJunctionDonor(RetailSliver[1], FVector3f(-17470.9902f, -8952, 21016.627f), 107, true);
	AddTJunctionDonor(RetailSliver[1], FVector3f(-17470.9902f, -8951.99902f, 21009.9277f), 109, false);
	const bool bSliverRepaired = WoTRuntimeBSPRepairRenderTJunctions(RetailSliver, Stats, Error);
	TestTrue(TEXT("real triangle 80 boundary triangulates without centroid rounding"), bSliverRepaired);
	if (!bSliverRepaired) AddError(Error);
	TestEqual(TEXT("real triangle 80 adds only four external boundary positions"), RetailSliver[0].Positions.Num(), 7);
	TestEqual(TEXT("real triangle 80 repaired geometry has zero junctions"), CountTJunctions(RetailSliver), 0);
	TArray<FWoTBSPPayloadData> SelfSliver;
	SelfSliver.SetNum(1);
	AddTJunctionTriangle(SelfSliver[0], FVector3f(17859.0508f, 15720.8301f, -730.545471f),
		FVector3f(17691.3164f, 15338.8066f, -730.546936f),
		FVector3f(17625.8555f, 15189.7129f, -730.547485f), 113, true);
	const FWoTBSPPayloadData BeforeSelfSliver = SelfSliver[0];
	TestTrue(TEXT("real triangle 173 excludes its own vertices as edge candidates"),
		WoTRuntimeBSPRepairRenderTJunctions(SelfSliver, Stats, Error));
	TestEqual(TEXT("self vertex does not retrace the boundary"), Stats.InsertedEdgeVertices, uint64(0));
	TestEqual(TEXT("self-only sliver keeps original topology"), SelfSliver[0].Indices, BeforeSelfSliver.Indices);
	TestEqual(TEXT("self-only sliver keeps original positions"), SelfSliver[0].Positions, BeforeSelfSliver.Positions);
	SelfSliver.SetNum(2);
	AddTJunctionDonor(SelfSliver[1], BeforeSelfSliver.Positions[1], 127, true);
	TestTrue(TEXT("coincident external source cannot insert a second host corner"),
		WoTRuntimeBSPRepairRenderTJunctions(SelfSliver, Stats, Error));
	TestEqual(TEXT("coincident external corner keeps simple host topology"), SelfSliver[0].Indices, BeforeSelfSliver.Indices);
	TestEqual(TEXT("coincident corner needs no duplicate boundary vertex"), Stats.InsertedEdgeVertices, uint64(0));
	TArray<FWoTBSPPayloadData> CollapsedSpike;
	CollapsedSpike.SetNum(2);
	AddTJunctionTriangle(CollapsedSpike[0], FVector3f(13875.4336f, 11279.9717f, -731.86908f),
		FVector3f(13878.2178f, 11275.2949f, -735.044434f),
		FVector3f(13952.0947f, 11151.1406f, -819.332153f), 131, true);
	AddTJunctionDonor(CollapsedSpike[1], FVector3f(13951.5273f, 11152.0928f, -818.685547f), 137, true);
	TestTrue(TEXT("real triangle 261 removes only its zero-area repeated-Q excursion"),
		WoTRuntimeBSPRepairRenderTJunctions(CollapsedSpike, Stats, Error));
	TestTrue(TEXT("triangle 261 keeps shared boundary position indexed"),
		CollapsedSpike[0].Indices.Contains(3));
	TestEqual(TEXT("triangle 261 emits valid residual triangle"), CollapsedSpike[0].Indices.Num(), 3);
	TestEqual(TEXT("triangle 261 has zero proper cross-polygon junctions"), CountTJunctions(CollapsedSpike), 0);
	TArray<FWoTBSPPayloadData> NonzeroLoop;
	NonzeroLoop.SetNum(2);
	AddTJunctionTriangle(NonzeroLoop[0], FVector3f(0, 0, 0),
		FVector3f(0.018f, 0, 0), FVector3f(0, 1, 0), 139, true);
	AddTJunctionDonor(NonzeroLoop[1], FVector3f(0.0045f, 0.5f, 0), 149, true);
	AddTJunctionDonor(NonzeroLoop[1], FVector3f(0.012f, 0.75f, 0), 151, false);
	const FWoTBSPPayloadData BeforeNonzeroLoop = NonzeroLoop[0];
	TestTrue(TEXT("nonzero-area repeated-position excursion is retained as its own loop"),
		WoTRuntimeBSPRepairRenderTJunctions(NonzeroLoop, Stats, Error));
	TestEqual(TEXT("both nonzero loops emit positive children"), NonzeroLoop[0].Indices.Num(), 6);
	TestEqual(TEXT("nonzero loop preservation retains source provenance"), NonzeroLoop[0].TriangleSurfaces.Num(), 2);
	TestTrue(TEXT("nonzero loops preserve original positions"), NonzeroLoop[0].Positions[0] == BeforeNonzeroLoop.Positions[0]);
	TArray<FWoTBSPPayloadData> ProjectedZeroLoop = Original;
	ProjectedZeroLoop.SetNum(4);
	AddTJunctionTriangle(ProjectedZeroLoop[2], FVector3f(0, 0, 0),
		FVector3f(0.015625f, 0, 0), FVector3f(0, 1, 0), 153, true);
	AddTJunctionDonor(ProjectedZeroLoop[3], FVector3f(0.0078125f, 0.5f, 0), 155, true);
	AddTJunctionDonor(ProjectedZeroLoop[3], FVector3f(0.00390625f, 0.75f, 0.0095f), 156, false);
	const TArray<FWoTBSPPayloadData> BeforeProjectedZero = ProjectedZeroLoop;
	const FVector3d LoopQ(0.0078125, 0.5, 0);
	const FVector3d LoopR(0.00390625, 0.75, double(0.0095f));
	const FVector3d LoopC(0, 1, 0);
	const FVector3d LoopCross = FVector3d::CrossProduct(LoopR - LoopQ, LoopC - LoopQ);
	TestEqual(TEXT("regression loop has exactly zero host-plane projection"), LoopCross.Z, 0.0);
	TestTrue(TEXT("regression loop retains nonzero true 3D area"),
		FMath::Abs(LoopCross.Size() * 0.5 - 0.00237529) < 0.00000001);
	TestTrue(TEXT("zero projected area preserves its nonzero 3D loop with a local normal"),
		WoTRuntimeBSPRepairRenderTJunctions(ProjectedZeroLoop, Stats, Error));
	TestEqual(TEXT("nonzero 3D loop and residual both emit triangles"), ProjectedZeroLoop[2].Indices.Num(), 6);
	TestTrue(TEXT("local noncoplanar orientation is explicitly counted"), Stats.LocallyOrientedLoops > 0);
	bool bRetained3DLoop = false;
	const FWoTBSPPayloadData& Retained3D = ProjectedZeroLoop[2];
	for (int32 At = 0; At < Retained3D.Indices.Num(); At += 3)
	{
		const FVector3d A(Retained3D.Positions[Retained3D.Indices[At]]);
		const FVector3d B(Retained3D.Positions[Retained3D.Indices[At + 1]]);
		const FVector3d C(Retained3D.Positions[Retained3D.Indices[At + 2]]);
		if ((A == LoopQ || B == LoopQ || C == LoopQ)
			&& (A == LoopR || B == LoopR || C == LoopR)
			&& (A == LoopC || B == LoopC || C == LoopC))
		{
			bRetained3DLoop = true;
			TestTrue(TEXT("emitted noncoplanar child preserves true 3D area"),
				FMath::Abs(FVector3d::CrossProduct(B - A, C - A).Size() * 0.5 - 0.00237529) < 0.00000001);
			TestEqual(TEXT("local orientation preserves child source provenance"), Retained3D.TriangleSurfaces[At / 3], 153u);
		}
	}
	TestTrue(TEXT("nonzero Q-R-C geometry survives output"), bRetained3DLoop);
	TArray<FWoTBSPPayloadData> RepeatedLocal = BeforeProjectedZero;
	TestTrue(TEXT("local-normal repair is repeatable"), WoTRuntimeBSPRepairRenderTJunctions(RepeatedLocal, Stats, Error));
	TestEqual(TEXT("local-normal topology is deterministic"), RepeatedLocal[2].Indices, Retained3D.Indices);
	TestEqual(TEXT("local-normal positions are deterministic"), RepeatedLocal[2].Positions, Retained3D.Positions);
	TArray<FWoTBSPPayloadData> Invalid3DLoop = BeforeProjectedZero;
	Invalid3DLoop[3] = {};
	AddTJunctionDonor(Invalid3DLoop[3], FVector3f(0.0078125f, 0.5f, 0), 155, true);
	AddTJunctionDonor(Invalid3DLoop[3], FVector3f(0.005859375f, 0.625f, 0.0099f), 156, false);
	AddTJunctionDonor(Invalid3DLoop[3], FVector3f(0.00390625f, 0.75f, -0.0099f), 157, false);
	const TArray<FWoTBSPPayloadData> BeforeInvalid3D = Invalid3DLoop;
	TestTrue(TEXT("self-crossing nonzero 3D loop falls back to its source triangle"),
		WoTRuntimeBSPRepairRenderTJunctions(Invalid3DLoop, Stats, Error));
	TestEqual(TEXT("invalid 3D loop reports one source fallback"),
		Stats.FallbackTriangles, uint64(1));
	TestTrue(TEXT("invalid 3D loop fallback identifies source triangle and crossing"),
		Stats.FallbackReason.Contains(TEXT("part 2 triangle 0"))
		&& Stats.FallbackReason.Contains(TEXT("self-intersect")));
	TestEqual(TEXT("3D loop fallback retains source positions"),
		Invalid3DLoop[2].Positions, BeforeInvalid3D[2].Positions);
	TestEqual(TEXT("3D loop fallback retains source indices"),
		Invalid3DLoop[2].Indices, BeforeInvalid3D[2].Indices);
	TArray<FWoTBSPPayloadData> RetailLocal;
	RetailLocal.SetNum(2);
	AddTJunctionTriangle(RetailLocal[0], FVector3f(19064.084f, -16810.9141f, -25879.1465f),
		FVector3f(19064.084f, -16810.9141f, -25861.1562f),
		FVector3f(19064.084f, -16810.916f, -25690.2461f), 159, true);
	AddTJunctionDonor(RetailLocal[1], FVector3f(19064.0859f, -16810.9141f, -25825.1738f), 161, true);
	TestTrue(TEXT("real triangle40263 retains its noncoplanar residual"),
		WoTRuntimeBSPRepairRenderTJunctions(RetailLocal, Stats, Error));
	TestEqual(TEXT("real triangle40263 emits one preserved child"), RetailLocal[0].Indices.Num(), 3);
	TestEqual(TEXT("real triangle40263 records local orientation"), Stats.LocallyOrientedLoops, uint64(1));
	if (RetailLocal[0].Indices.Num() == 3)
	{
		const FVector3d A(RetailLocal[0].Positions[RetailLocal[0].Indices[0]]);
		const FVector3d B(RetailLocal[0].Positions[RetailLocal[0].Indices[1]]);
		const FVector3d C(RetailLocal[0].Positions[RetailLocal[0].Indices[2]]);
		TestTrue(TEXT("real triangle40263 preserves measured true 3D area"),
			FMath::Abs(FVector3d::CrossProduct(B - A, C - A).Size() * 0.5 - 0.017568588256835938) < 0.000000001);
		TestEqual(TEXT("real local-normal child inherits provenance"), RetailLocal[0].TriangleSurfaces[0], 159u);
	}
	TArray<FWoTBSPPayloadData> SourceSliver;
	SourceSliver.SetNum(2);
	AddTJunctionTriangle(SourceSliver[0], FVector3f(14146.2773f, 10824.9766f, -810.021606f),
		FVector3f(14197.5342f, 10738.9365f, -733.494019f),
		FVector3f(14211.4727f, 10715.5381f, -712.682861f), 157, true);
	const FVector3f ExactSource(14197.9551f, 10738.2275f, -732.86438f);
	AddTJunctionDonor(SourceSliver[1], ExactSource, 163, true);
	TestTrue(TEXT("real triangle 271 shares exact source and retains winding"),
		WoTRuntimeBSPRepairRenderTJunctions(SourceSliver, Stats, Error));
	TestTrue(TEXT("real triangle 271 emits exact donor position"), SourceSliver[0].Positions.Contains(ExactSource));
	TestEqual(TEXT("real triangle 271 has zero proper cross-polygon junctions"), CountTJunctions(SourceSliver), 0);
	TArray<FWoTBSPPayloadData> ReversedSliver;
	ReversedSliver.SetNum(2);
	AddTJunctionTriangle(ReversedSliver[0], FVector3f(17961.7598f, 14051.4893f, 241.608246f),
		FVector3f(17955.0918f, 14066.71f, 204.637939f),
		FVector3f(17924.1777f, 14137.2725f, 33.2390404f), 167, true);
	const FVector3d ReversedNormal = FVector3d::CrossProduct(
		FVector3d(ReversedSliver[0].Positions[1] - ReversedSliver[0].Positions[0]),
		FVector3d(ReversedSliver[0].Positions[2] - ReversedSliver[0].Positions[0]));
	AddTJunctionDonor(ReversedSliver[1], FVector3f(17953.2402f, 14070.9365f, 194.374039f), 173, true);
	TestTrue(TEXT("real triangle 226 reorients boundary to original winding"),
		WoTRuntimeBSPRepairRenderTJunctions(ReversedSliver, Stats, Error));
	const FWoTBSPPayloadData& Oriented = ReversedSliver[0];
	for (int32 At = 0; At < Oriented.Indices.Num(); At += 3)
	{
		const FVector3d A(Oriented.Positions[Oriented.Indices[At]]);
		const FVector3d B(Oriented.Positions[Oriented.Indices[At + 1]]);
		const FVector3d C(Oriented.Positions[Oriented.Indices[At + 2]]);
		TestTrue(TEXT("reoriented child has positive host winding"),
			FVector3d::DotProduct(FVector3d::CrossProduct(B - A, C - A), ReversedNormal) > 0);
	}
	// z=.011 is outside the original host edge, but becomes eligible after
	// z=.01 bends its subedge. A complete closure pass must insert it too.
	TestTrue(TEXT("second pass shares newly eligible outside-original-epsilon source"),
		Parts[0].Positions.Contains(FVector3f(192, -256, 0.011f)));
	TArray<FWoTBSPPayloadData> PassLimited = Original;
	PassLimited.SetNum(4);
	AddTJunctionTriangle(PassLimited[2], FVector3f(0, 0, 0),
		FVector3f(1000000, 0, 0), FVector3f(0, 1000000, 0), 179, true);
	float ChainHeight = 0;
	for (int32 Point = 1; Point <= 20; ++Point)
	{
		ChainHeight = ChainHeight * 0.5f + 0.009f;
		AddTJunctionDonor(PassLimited[3], FVector3f(
			1000000.0f * (1 - FMath::Pow(0.5f, Point)), 0, ChainHeight),
			200 + Point, Point == 1);
	}
	TestTrue(TEXT("closure pass cap falls back only its source triangle"),
		WoTRuntimeBSPRepairRenderTJunctions(PassLimited, Stats, Error));
	TestTrue(TEXT("closure cap reports its context"),
		Stats.FallbackReason.Contains(TEXT("closure"))
		&& Stats.FallbackReason.Contains(TEXT("16")));
	TestEqual(TEXT("closure fallback keeps previously repaired topology"),
		PassLimited[0].Indices, Parts[0].Indices);
	TestEqual(TEXT("closure fallback keeps prior repaired positions"),
		PassLimited[0].Positions, Parts[0].Positions);
	TArray<FWoTBSPPayloadData> FullyCollapsed;
	FullyCollapsed.SetNum(2);
	AddTJunctionTriangle(FullyCollapsed[0], FVector3f(-16508, -5902, 20934.9766f),
		FVector3f(-16508, -5902.00049f, 21358), FVector3f(-16508, -5902.00049f, 21450), 181, true);
	AddTJunctionDonor(FullyCollapsed[1], FVector3f(-16508, -5902.00049f, 21061.8848f), 191, true);
	AddTJunctionDonor(FullyCollapsed[1], FVector3f(-16508, -5902.00049f, 21382), 193, false);
	AddTJunctionDonor(FullyCollapsed[1], FVector3f(-16508, -5902.00049f, 21418.7168f), 197, false);
	TestTrue(TEXT("real triangle4571 publishes no face for fully zero-area repaired boundary"),
		WoTRuntimeBSPRepairRenderTJunctions(FullyCollapsed, Stats, Error));
	TestEqual(TEXT("fully collapsed render sliver is explicitly counted"), Stats.DroppedDegenerateTriangles, uint64(1));
	TestTrue(TEXT("zero-area excursions are explicitly counted"), Stats.DroppedDegenerateLoops > 0);
	TestTrue(TEXT("fully collapsed source has zero render children"), FullyCollapsed[0].Indices.IsEmpty());
	TestTrue(TEXT("zero children produce zero provenance entries"), FullyCollapsed[0].TriangleSurfaces.IsEmpty());
	TestEqual(TEXT("empty source section remains valid"), FullyCollapsed[0].Sections[0].IndexCount, 0u);
	TArray<FWoTBSPPayloadData> OriginalCollinear;
	OriginalCollinear.SetNum(2);
	AddTJunctionTriangle(OriginalCollinear[0], FVector3f(-18044, -6926, 21358),
		FVector3f(-18044, -6926, 21333.9902f), FVector3f(-18044, -6926, 21022.3848f), 211, true);
	AddTJunctionDonor(OriginalCollinear[1], FVector3f(-18044, -6926, 21200), 223, true);
	const TArray<uint32> OriginalCollinearIndices = OriginalCollinear[0].Indices;
	TestTrue(TEXT("real triangle5068 ignores its exact-zero render face for incidence"),
		WoTRuntimeBSPRepairRenderTJunctions(OriginalCollinear, Stats, Error));
	TestEqual(TEXT("ignored zero-area face is not counted as repaired"),
		Stats.DroppedDegenerateTriangles, uint64(0));
	TestEqual(TEXT("ignored zero-area source topology is untouched"),
		OriginalCollinear[0].Indices, OriginalCollinearIndices);
	TArray<FWoTBSPPayloadData> TinyNonzero;
	TinyNonzero.SetNum(2);
	AddTJunctionTriangle(TinyNonzero[0], FVector3f(0, 0, 0),
		FVector3f(0.00001f, 0, 0), FVector3f(0, 1, 0), 227, true);
	AddTJunctionDonor(TinyNonzero[1], FVector3f(0.000005f, 0.5f, 0), 229, true);
	TestTrue(TEXT("arbitrarily small nonzero original area is retained"),
		WoTRuntimeBSPRepairRenderTJunctions(TinyNonzero, Stats, Error));
	TestEqual(TEXT("tiny nonzero source is not dropped"), Stats.DroppedDegenerateTriangles, uint64(0));
	TestTrue(TEXT("tiny nonzero source still has visible render area"), !TinyNonzero[0].Indices.IsEmpty());
	TArray<FWoTBSPPayloadData> ThinQuad;
	ThinQuad.SetNum(2);
	AddTJunctionTriangle(ThinQuad[0], FVector3f(-19121, -6382, 21092.2793f),
		FVector3f(-19004, -6381.91016f, 21092.2793f),
		FVector3f(-19240.2871f, -6381.91016f, 21092.2793f), 233, true);
	AddTJunctionDonor(ThinQuad[1], FVector3f(-19125, -6382, 21092.2793f), 239, true);
	TestTrue(TEXT("unmatched external point is detected before quad repair"), CountTJunctions(ThinQuad) > 0);
	TestTrue(TEXT("real triangle6332 uses valid ordinary quad triangulation"),
		WoTRuntimeBSPRepairRenderTJunctions(ThinQuad, Stats, Error));
	TestEqual(TEXT("real triangle6332 retains both nonzero children"), ThinQuad[0].Indices.Num(), 6);
	TestEqual(TEXT("sibling sliver closes its near-diagonal point"), CountTJunctions(ThinQuad), 0);
	TArray<FWoTBSPPayloadData> RetailLoops;
	RetailLoops.SetNum(2);
	AddTJunctionTriangle(RetailLoops[0], FVector3f(19064.0859f, -17674.4609f, -25411.3906f),
		FVector3f(19064.0859f, -17674.459f, -25237.0391f),
		FVector3f(19064.084f, -17674.4609f, -25201.3125f), 241, true);
	AddTJunctionDonor(RetailLoops[1], FVector3f(19064.0879f, -17674.459f, -25240.4824f), 251, true);
	AddTJunctionDonor(RetailLoops[1], FVector3f(19064.0859f, -17674.4648f, -25204.502f), 253, false);
	TestTrue(TEXT("real triangle41482 retains its nonzero closed loop"),
		WoTRuntimeBSPRepairRenderTJunctions(RetailLoops, Stats, Error));
	TestEqual(TEXT("real triangle41482 emits its nonzero loop triangle"), RetailLoops[0].Indices.Num(), 3);
	TestEqual(TEXT("retained retail loop inherits its source surface"), RetailLoops[0].TriangleSurfaces.Num(), 1);
	// Arena_12 part 0 triangle 16477: this noncoplanar Q-R-S-T-U loop
	// crosses only in the authored plane. Rejecting that projection, or
	// dropping/flipping its nonzero children, loses real render geometry.
	TArray<FWoTBSPPayloadData> ArenaLoop;
	ArenaLoop.SetNum(2);
	AddTJunctionTriangle(ArenaLoop[0], FVector3f(1715.4674072265625f, -897.8675537109375f, -3.9205477237701416f),
		FVector3f(1715.46728515625f, -897.86737060546875f, 211.96627807617188f),
		FVector3f(1715.467041015625f, -897.84619140625f, 571.77703857421875f), 281, true);
	ArenaLoop[0].UV0 = { FVector2f(-1, -2), FVector2f(0, 0), FVector2f(1, 2) };
	ArenaLoop[0].UV1 = { FVector2f(0, -1), FVector2f(2, 3), FVector2f(4, 7) };
	ArenaLoop[0].Normals = { FVector3f(0.6f, 0, 0.8f), FVector3f(0, 0, 1), FVector3f(0, 0.6f, 0.8f) };
	const FVector3f ArenaDonors[] = {
		{ 1715.46728515625f, -897.86737060546875f, 238.95211791992188f },
		{ 1715.4666748046875f, -897.86737060546875f, 274.93292236328125f },
		{ 1715.466064453125f, -897.86767578125f, 292.92361450195312f },
		{ 1715.466064453125f, -897.86767578125f, 328.90469360351562f },
		{ 1715.4676513671875f, -897.86737060546875f, 382.87643432617188f } };
	for (int32 Donor = 0; Donor < 5; ++Donor)
		AddTJunctionTriangle(ArenaLoop[1], ArenaDonors[Donor],
			ArenaDonors[Donor] + FVector3f(40, 0, 0), ArenaDonors[Donor] + FVector3f(40, 40, 0),
			283 + Donor, Donor == 0);
	const TArray<FWoTBSPPayloadData> BeforeArenaLoop = ArenaLoop;
	const bool bArenaRepaired = WoTRuntimeBSPRepairRenderTJunctions(ArenaLoop, Stats, Error);
	TestTrue(TEXT("Arena triangle16477 uses a simple local projection for its noncoplanar loop"), bArenaRepaired);
	if (!bArenaRepaired) AddError(Error);
	if (bArenaRepaired)
	{
		const FWoTBSPPayloadData& Arena = ArenaLoop[0];
		TestEqual(TEXT("Arena retains its residual triangle and three nonzero loop children"), Arena.Indices.Num(), 12);
		TestEqual(TEXT("Arena projection fallback is counted once"), Stats.LocallyOrientedLoops, uint64(1));
		TestEqual(TEXT("Arena source triangle is not discarded"), Stats.DroppedDegenerateTriangles, uint64(0));
		FVector3d LoopAreaVector = FVector3d::ZeroVector;
		const FVector3d SourceNormal = FVector3d::CrossProduct(
			FVector3d(BeforeArenaLoop[0].Positions[1]) - FVector3d(BeforeArenaLoop[0].Positions[0]),
			FVector3d(BeforeArenaLoop[0].Positions[2]) - FVector3d(BeforeArenaLoop[0].Positions[0]));
		for (int32 At = 0; At < Arena.Indices.Num(); At += 3)
		{
			const FVector3d A(Arena.Positions[Arena.Indices[At]]);
			const FVector3d B(Arena.Positions[Arena.Indices[At + 1]]);
			const FVector3d C(Arena.Positions[Arena.Indices[At + 2]]);
			const FVector3d Cross = FVector3d::CrossProduct(B - A, C - A);
			TestTrue(TEXT("Arena retains nonzero 3D child area"), Cross.SizeSquared() > 0);
			TestEqual(TEXT("Arena children inherit source surface"), Arena.TriangleSurfaces[At / 3], 281u);
			if (Arena.Indices[At] >= 3 && Arena.Indices[At + 1] >= 3 && Arena.Indices[At + 2] >= 3)
			{
				TestTrue(TEXT("Arena loop children retain source-facing winding"),
					FVector3d::DotProduct(Cross, SourceNormal) >= 0);
				LoopAreaVector += Cross;
			}
		}
		TestTrue(TEXT("Arena complete loop remains source-facing"),
			FVector3d::DotProduct(LoopAreaVector, SourceNormal) >= 0);
		// Literal independently projected host attributes. U is added during
		// closure on T-C and interpolates the already normalized T normal.
		const float HostParameters[] = { 0.0750000909f, 0.174999336f, 0.224999764f, 0.324999772f, 0.475000123f };
		const FVector3f HostNormals[] = { { 0, 0.045637734f, 0.998958056f },
			{ 0, 0.108169429f, 0.994132473f }, { 0, 0.139969516f, 0.990155813f },
			{ 0, 0.204163152f, 0.978936876f }, { 0, 0.297010842f, 0.954874107f } };
		for (int32 Donor = 0; Donor < 5; ++Donor)
		{
			bool bShared = false;
			bool bBoundaryEdge = false;
			for (int32 At = 0; At < Arena.Indices.Num(); ++At)
			{
				const uint32 Vertex = Arena.Indices[At];
				if (Arena.Positions[Vertex] != ArenaDonors[Donor]) continue;
				bShared = true;
				const float T = HostParameters[Donor];
				TestTrue(TEXT("Arena exact donor uses host UV0"), Arena.UV0[Vertex].Equals(FVector2f(T, 2 * T), 1e-6f));
				TestTrue(TEXT("Arena exact donor uses host UV1"), Arena.UV1[Vertex].Equals(FVector2f(2 + 2 * T, 3 + 4 * T), 1e-6f));
				TestTrue(TEXT("Arena exact donor uses interpolated host normal"), Arena.Normals[Vertex].Equals(HostNormals[Donor], 1e-6f));
				const int32 TriangleStart = At - At % 3;
				for (int32 Corner = 0; Corner < 3; ++Corner)
					bBoundaryEdge |= Arena.Positions[Arena.Indices[TriangleStart + Corner]] == ArenaDonors[(Donor + 1) % 5];
			}
			TestTrue(TEXT("Arena retains each exact shared donor in indexed geometry"), bShared);
			TestTrue(TEXT("Arena retains every edge of the nonzero loop"), bBoundaryEdge);
		}
		TestEqual(TEXT("Arena section covers every emitted child"), Arena.Sections[0].IndexCount, uint32(Arena.Indices.Num()));
		TestEqual(TEXT("Arena section material stays intact"), Arena.Sections[0].MaterialSlot, BeforeArenaLoop[0].Sections[0].MaterialSlot);
		TestEqual(TEXT("Arena donor topology stays intact"), ArenaLoop[1].Indices, BeforeArenaLoop[1].Indices);
		TArray<FWoTBSPPayloadData> RepeatedArena = BeforeArenaLoop;
		TestTrue(TEXT("Arena local projection repairs repeatedly"), WoTRuntimeBSPRepairRenderTJunctions(RepeatedArena, Stats, Error));
		TestEqual(TEXT("Arena local projection topology is deterministic"), RepeatedArena[0].Indices, Arena.Indices);
		TestEqual(TEXT("Arena local projection positions are deterministic"), RepeatedArena[0].Positions, Arena.Positions);
		TestEqual(TEXT("Arena local projection normals are deterministic"), RepeatedArena[0].Normals, Arena.Normals);
		TestEqual(TEXT("Arena local projection UV0 is deterministic"), RepeatedArena[0].UV0, Arena.UV0);
		TestEqual(TEXT("Arena local projection UV1 is deterministic"), RepeatedArena[0].UV1, Arena.UV1);
	}
	TArray<FWoTBSPPayloadData> CrossingLoop;
	CrossingLoop.SetNum(2);
	AddTJunctionTriangle(CrossingLoop[0], FVector3f(0, 0, 0),
		FVector3f(0.018f, 0, 0), FVector3f(0, 1, 0), 257, true);
	AddTJunctionDonor(CrossingLoop[1], FVector3f(0.000597528531f, 0.838426292f, 0), 263, true);
	AddTJunctionDonor(CrossingLoop[1], FVector3f(0.00926849805f, 0.838473916f, 0), 269, false);
	AddTJunctionDonor(CrossingLoop[1], FVector3f(-0.00994996633f, 0.851055741f, 0), 271, false);
	TestTrue(TEXT("properly crossing nonzero loop falls back to source geometry"),
		WoTRuntimeBSPRepairRenderTJunctions(CrossingLoop, Stats, Error));
	TestEqual(TEXT("ambiguous loop reports one fallback triangle"),
		Stats.FallbackTriangles, uint64(1));
	TestTrue(TEXT("ambiguous loop fallback identifies self intersection"),
		Stats.FallbackReason.Contains(TEXT("self-intersect")));
	TestEqual(TEXT("ambiguous loop leaves source triangle untouched"), CrossingLoop[0].Indices.Num(), 3);
	TArray<FWoTBSPPayloadData> ExcessiveWork;
	ExcessiveWork.SetNum(1);
	for (int32 Triangle = 0; Triangle < 40; ++Triangle)
	{
		const float Z = Triangle * 100.0f;
		AddTJunctionTriangle(ExcessiveWork[0], FVector3f(-9000000, -9000000, Z),
			FVector3f(9000000, 8500000, Z + 17),
			FVector3f(-8500000, 9000000, Z + 43), 89, Triangle == 0);
	}
	TestFalse(TEXT("excessively long edge fails within traversal budget"),
		WoTRuntimeBSPRepairRenderTJunctions(ExcessiveWork, Stats, Error));
	TestTrue(TEXT("work limit gives actionable error"), Error.Contains(TEXT("cell visits")));
	TestEqual(TEXT("work limit leaves original topology intact"), ExcessiveWork[0].Indices.Num(), 120);
	TArray<FWoTBSPPayloadData> DarkRibbon;
	DarkRibbon.SetNum(1);
	AddTJunctionTriangle(DarkRibbon[0], FVector3f(0, 0, 0),
		FVector3f(0.25f, 0, 0), FVector3f(0.25f, 10, 0), 101, true);
	AddTJunctionTriangle(DarkRibbon[0], FVector3f(0, 0, 0),
		FVector3f(0.25f, 10, 0), FVector3f(0, 10, 0), 101, false);
	AddTJunctionTriangle(DarkRibbon[0], FVector3f(0.25f, 0, 0),
		FVector3f(10, 0, 0), FVector3f(10, 10, 0), 202, false);
	AddTJunctionTriangle(DarkRibbon[0], FVector3f(0.25f, 0, 0),
		FVector3f(10, 10, 0), FVector3f(0.25f, 10, 0), 202, false);
	for (int32 Vertex = 0; Vertex < DarkRibbon[0].UV1.Num(); ++Vertex)
		DarkRibbon[0].UV1[Vertex] = Vertex < 6
			? FVector2f(0.0625f, 0.5f) : FVector2f(0.8125f, 0.5f);
	DarkRibbon[0].LightmapWidth = 8;
	DarkRibbon[0].LightmapHeight = 1;
	DarkRibbon[0].LightmapBGRA.Init(16, 8 * 4);
	for (int32 Channel = 0; Channel < 3; ++Channel)
		DarkRibbon[0].LightmapBGRA[6 * 4 + Channel] = 128;
	TestTrue(TEXT("isolated dark coplanar ribbon repairs"),
		WoTRuntimeBSPRepairRenderTJunctions(DarkRibbon, Stats, Error));
	bool bRibbonUsesNeighborLight = true;
	for (int32 Triangle = 0; Triangle < DarkRibbon[0].TriangleSurfaces.Num(); ++Triangle)
		if (DarkRibbon[0].TriangleSurfaces[Triangle] == 101)
			for (int32 Corner = 0; Corner < 3; ++Corner)
				bRibbonUsesNeighborLight &= DarkRibbon[0].UV1[
					DarkRibbon[0].Indices[Triangle * 3 + Corner]].X > 0.5f;
	TestTrue(TEXT("sub-unit dark ribbon inherits its coplanar neighbour lightmap edge"),
		bRibbonUsesNeighborLight);
	TArray<FWoTBSPPayloadData> ManyRibbons;
	ManyRibbons.SetNum(1);
	for (int32 Ribbon = 0; Ribbon < 64; ++Ribbon)
	{
		const float X = Ribbon * 20.0f;
		const int32 FirstVertex = ManyRibbons[0].Positions.Num();
		AddTJunctionTriangle(ManyRibbons[0], FVector3f(X, 0, 0),
			FVector3f(X + 0.25f, 0, 0), FVector3f(X + 0.25f, 10, 0),
			1000 + Ribbon * 2, Ribbon == 0);
		AddTJunctionTriangle(ManyRibbons[0], FVector3f(X, 0, 0),
			FVector3f(X + 0.25f, 10, 0), FVector3f(X, 10, 0),
			1000 + Ribbon * 2, false);
		AddTJunctionTriangle(ManyRibbons[0], FVector3f(X + 0.25f, 0, 0),
			FVector3f(X + 10, 0, 0), FVector3f(X + 10, 10, 0),
			1001 + Ribbon * 2, false);
		AddTJunctionTriangle(ManyRibbons[0], FVector3f(X + 0.25f, 0, 0),
			FVector3f(X + 10, 10, 0), FVector3f(X + 0.25f, 10, 0),
			1001 + Ribbon * 2, false);
		for (int32 Vertex = FirstVertex; Vertex < ManyRibbons[0].UV1.Num(); ++Vertex)
			ManyRibbons[0].UV1[Vertex] = Vertex < FirstVertex + 6
				? FVector2f(0.0625f, 0.5f) : FVector2f(0.8125f, 0.5f);
	}
	ManyRibbons[0].LightmapWidth = 8;
	ManyRibbons[0].LightmapHeight = 1;
	ManyRibbons[0].LightmapBGRA.Init(2, 8 * 4);
	for (int32 Channel = 0; Channel < 3; ++Channel)
		ManyRibbons[0].LightmapBGRA[6 * 4 + Channel] = 128;
	TestTrue(TEXT("many isolated ribbons repair within the work census"),
		WoTRuntimeBSPRepairRenderTJunctions(ManyRibbons, Stats, Error));
	TestTrue(TEXT("ribbon work census counts source triangle visits"),
		Stats.LightmapRibbonSourceTriangleVisits > 0);
	TestTrue(TEXT("ribbon source triangle visits remain linear"),
		Stats.LightmapRibbonSourceTriangleVisits <= Stats.InputTriangles * 4);
	TArray<FWoTBSPPayloadData> ForkedBoundary;
	ForkedBoundary.SetNum(2);
	const FVector3f ForkAnchor(0, 0, 0);
	const FVector3f ForkUpperEnd(100, 0.3f, 0);
	const FVector3f ForkLowerEnd(99.9f, 0, 0);
	AddTJunctionTriangle(ForkedBoundary[0], ForkAnchor, ForkUpperEnd,
		FVector3f(0, 100, 0), 2001, true);
	AddTJunctionTriangle(ForkedBoundary[1], ForkLowerEnd, ForkAnchor,
		FVector3f(0, -100, 0), 2001, true);
	const TArray<FWoTBSPPayloadData> ForkCollisionAuthority = ForkedBoundary;
	TestEqual(TEXT("forked fixture starts with an incidence-one lower edge"),
		WoTX6Test::EdgeIncidence(ForkedBoundary, ForkAnchor, ForkLowerEnd), 1u);
	TestTrue(TEXT("forked incidence-one render boundaries repair"),
		WoTRuntimeBSPRepairRenderTJunctions(ForkedBoundary, Stats, Error));
	TestTrue(TEXT("forked render boundary shares the lower edge exactly"),
		WoTX6Test::EdgeIncidence(ForkedBoundary, ForkAnchor, ForkLowerEnd) >= 2);
	TestEqual(TEXT("saved source copy retains original positions outside the render repair input"),
		ForkCollisionAuthority[0].Positions,
		TArray<FVector3f>({ForkAnchor, ForkUpperEnd, FVector3f(0, 100, 0)}));
	TestEqual(TEXT("saved source copy retains original indices outside the render repair input"),
		ForkCollisionAuthority[0].Indices, TArray<uint32>({0, 1, 2}));
	// Removing the strict inner-endpoint, opposite-interior, unique-donor,
	// surface, facing, or gap guard must change these exact source buffers.
	const auto CheckRejectedFork = [this](const TCHAR* Label,
		TArray<FWoTBSPPayloadData> Rejected)
	{
		const TArray<FWoTBSPPayloadData> Before = Rejected;
		FWoTBSPTJunctionStats RejectedStats;
		FString RejectedError;
		TestTrue(FString(Label) + TEXT(" repair succeeds without optional closure"),
			WoTRuntimeBSPRepairRenderTJunctions(Rejected, RejectedStats, RejectedError));
		TestEqual(FString(Label) + TEXT(" applies no near-coincident repairs"),
			RejectedStats.RepairedNearCoincidentEdges, uint64(0));
		for (int32 Part = 0; Part < Rejected.Num(); ++Part)
		{
			TestEqual(FString::Printf(TEXT("%s part %d exact positions unchanged"), Label, Part),
				Rejected[Part].Positions, Before[Part].Positions);
			TestEqual(FString::Printf(TEXT("%s part %d exact indices unchanged"), Label, Part),
				Rejected[Part].Indices, Before[Part].Indices);
		}
	};
	TArray<FWoTBSPPayloadData> EqualRadius;
	EqualRadius.SetNum(2);
	AddTJunctionTriangle(EqualRadius[0], ForkAnchor, FVector3f(100, 0.15f, 0),
		FVector3f(0, 100, 0), 2001, true);
	AddTJunctionTriangle(EqualRadius[1], FVector3f(100, -0.15f, 0), ForkAnchor,
		FVector3f(0, -100, 0), 2001, true);
	CheckRejectedFork(TEXT("equal-radius reciprocal ambiguity"), EqualRadius);
	TArray<FWoTBSPPayloadData> WithinLengthEpsilon;
	WithinLengthEpsilon.SetNum(2);
	AddTJunctionTriangle(WithinLengthEpsilon[0], ForkAnchor, ForkUpperEnd,
		FVector3f(0, 100, 0), 2001, true);
	AddTJunctionTriangle(WithinLengthEpsilon[1], FVector3f(100, 0, 0), ForkAnchor,
		FVector3f(0, -100, 0), 2001, true);
	CheckRejectedFork(TEXT("radial difference within ordinary epsilon"), WithinLengthEpsilon);
	TArray<FWoTBSPPayloadData> SameSide;
	SameSide.SetNum(2);
	AddTJunctionTriangle(SameSide[0], ForkAnchor, ForkUpperEnd,
		FVector3f(0, 100, 0), 2001, true);
	AddTJunctionTriangle(SameSide[1], ForkAnchor, ForkLowerEnd,
		FVector3f(10, 90, 0), 2001, true);
	CheckRejectedFork(TEXT("same-side overlapping triangle interiors"), SameSide);
	TArray<FWoTBSPPayloadData> CompetingDonors;
	CompetingDonors.SetNum(2);
	AddTJunctionTriangle(CompetingDonors[0], ForkAnchor, FVector3f(100, 0.15f, 0),
		FVector3f(0, 100, 0), 2001, true);
	for (int32 Donor = 0; Donor < 20; ++Donor)
	{
		AddTJunctionTriangle(CompetingDonors[1],
			FVector3f(99.8f, -0.02f - 0.014f * Donor, 0), ForkAnchor,
			FVector3f(-10000, -100 - 30 * Donor, 0), 2001, Donor == 0);
	}
	CheckRejectedFork(TEXT("dense anchor competing donors"), CompetingDonors);
	TArray<FWoTBSPPayloadData> DifferentSurface = WithinLengthEpsilon;
	DifferentSurface[1].Positions[0] = ForkLowerEnd;
	DifferentSurface[1].TriangleSurfaces[0] = 2002;
	CheckRejectedFork(TEXT("different source surface"), DifferentSurface);
	TArray<FWoTBSPPayloadData> FacingMismatch = WithinLengthEpsilon;
	FacingMismatch[1].Positions[0] = ForkLowerEnd;
	Swap(FacingMismatch[1].Indices[0], FacingMismatch[1].Indices[1]);
	CheckRejectedFork(TEXT("opposite source facing"), FacingMismatch);
	TArray<FWoTBSPPayloadData> BeyondGap = WithinLengthEpsilon;
	BeyondGap[0].Positions[1] = FVector3f(100, 0.4901f, 0);
	BeyondGap[1].Positions[0] = ForkLowerEnd;
	// sqrt(0.1^2 + 0.4901^2) = 0.500198, just beyond the 0.5 bound.
	CheckRejectedFork(TEXT("endpoint gap just beyond half a unit"), BeyondGap);
	TArray<FWoTBSPPayloadData> FanSliver;
	FanSliver.SetNum(1);
	const FVector3f FanAnchor(256, 158.129f, 0);
	const FVector3f FanPrevious(303.104f, 156.257f, 0);
	const FVector3f FanBaseA(315.152f, 165.865f, 0);
	const FVector3f FanBaseB(315.440f, 166.828f, 0);
	AddTJunctionTriangle(FanSliver[0], FanBaseB, FanBaseA, FanAnchor, 3001, true);
	AddTJunctionTriangle(FanSliver[0], FanBaseA, FanPrevious, FanAnchor, 3001, false);
	const TArray<FWoTBSPPayloadData> FanSliverSource = FanSliver;
	TestTrue(TEXT("local-minimum fan sliver retessellates"),
		WoTRuntimeBSPRepairRenderTJunctions(FanSliver, Stats, Error));
	TestEqual(TEXT("one fan-sliver quad exchanges its diagonal"),
		Stats.RetessellatedFanSliverPairs, uint64(1));
	TestEqual(TEXT("fan-sliver exchange adds no vertices"),
		FanSliver[0].Positions, FanSliverSource[0].Positions);
	TestEqual(TEXT("fan-sliver exchange adds no indices"),
		FanSliver[0].Indices.Num(), FanSliverSource[0].Indices.Num());
	bool bSourceFanTriangleSurvives = false;
	for (int32 At = 0; At < FanSliver[0].Indices.Num(); At += 3)
	{
		int32 Matches = 0;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector3f Position = FanSliver[0].Positions[
				FanSliver[0].Indices[At + Corner]];
			Matches += Position == FanAnchor || Position == FanBaseA
				|| Position == FanBaseB ? 1 : 0;
		}
		bSourceFanTriangleSurvives |= Matches == 3;
	}
	TestFalse(TEXT("long fan face no longer survives as one render triangle"),
		bSourceFanTriangleSurvives);
	TArray<FWoTBSPPayloadData> NonAffineFan = FanSliverSource;
	NonAffineFan[0].UV0[4] = FVector2f(999, 999);
	const TArray<uint32> NonAffineIndices = NonAffineFan[0].Indices;
	TestTrue(TEXT("non-affine fan remains a valid unrepaired payload"),
		WoTRuntimeBSPRepairRenderTJunctions(NonAffineFan, Stats, Error));
	TestEqual(TEXT("non-affine fan is not retessellated"),
		Stats.RetessellatedFanSliverPairs, uint64(0));
	TestEqual(TEXT("non-affine fan topology stays exact"),
		NonAffineFan[0].Indices, NonAffineIndices);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPWatertightEdgesUnchangedSpec,
	"WoT.World.RuntimeBSP.WatertightEdgesUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPWatertightEdgesUnchangedSpec::RunTest(const FString&)
{
	TArray<FWoTBSPPayloadData> Parts;
	Parts.SetNum(1);
	const FVector3f A(0, 0, 0), B(100, 0, 0), C(0, 100, 0);
	const FVector3f NearbySibling(50, 0, 0.009f);
	AddTJunctionTriangle(Parts[0], A, B, C, 301, true);
	AddTJunctionTriangle(Parts[0], B, A, NearbySibling, 301, false);
	const FWoTBSPPayloadData Before = Parts[0];
	FWoTBSPTJunctionStats Stats;
	FString Error;
	if (!TestTrue(TEXT("watertight pair is accepted"),
		WoTRuntimeBSPRepairRenderTJunctions(Parts, Stats, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("watertight indices stay byte-identical"), Parts[0].Indices, Before.Indices);
	TestEqual(TEXT("watertight positions stay byte-identical"), Parts[0].Positions, Before.Positions);
	TestEqual(TEXT("watertight normals stay byte-identical"), Parts[0].Normals, Before.Normals);
	TestEqual(TEXT("watertight UV0 stays byte-identical"), Parts[0].UV0, Before.UV0);
	TestEqual(TEXT("watertight UV1 stays byte-identical"), Parts[0].UV1, Before.UV1);
	TestEqual(TEXT("watertight provenance stays byte-identical"),
		Parts[0].TriangleSurfaces, Before.TriangleSurfaces);
	TestEqual(TEXT("watertight section index count stays byte-identical"),
		Parts[0].Sections[0].IndexCount, Before.Sections[0].IndexCount);
	TestEqual(TEXT("watertight section vertex range stays byte-identical"),
		Parts[0].Sections[0].MaxVertex, Before.Sections[0].MaxVertex);
	TestEqual(TEXT("watertight pair needs no inserted vertices"),
		Stats.InsertedEdgeVertices, uint64(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPProtectedEdgePairedDonorSpec,
	"WoT.World.RuntimeBSP.ProtectedEdgePairedDonorFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPProtectedEdgePairedDonorSpec::RunTest(const FString&)
{
	TArray<FWoTBSPPayloadData> Parts;
	Parts.SetNum(2);
	const FVector3f A(0, 0, 0), B(1, 0, 0), C(0, 0.02f, 0);
	AddTJunctionTriangle(Parts[0], A, B, C, 303, true);
	// Protect B-C and C-A with exact nonzero incidence two. P and Q are
	// distinct in 3D but coincide in the authored face projection while all
	// three donors split the sole eligible edge A-B.
	AddTJunctionTriangle(Parts[0], B, C, FVector3f(1, 1, 0), 303, false);
	AddTJunctionTriangle(Parts[0], C, A, FVector3f(-1, 1, 0), 303, false);
	const FVector3f P(0.3f, 0.009f, 0.001f);
	const FVector3f Q(0.3f, 0.009f, -0.001f);
	const FVector3f R(0.31f, -0.009f, 0);
	AddTJunctionTriangle(Parts[1], P, P + FVector3f(0, 0, 40),
		P + FVector3f(0, 40, 40), 307, true);
	AddTJunctionTriangle(Parts[1], Q, Q + FVector3f(0, 0, -40),
		Q + FVector3f(0, 40, -40), 309, false);
	AddTJunctionTriangle(Parts[1], R, R + FVector3f(0, 0, 40),
		R + FVector3f(0, 40, 40), 311, false);
	const TArray<FWoTBSPPayloadData> Before = Parts;
	FWoTBSPTJunctionStats Stats;
	FString Error;
	if (!TestTrue(TEXT("paired projected donor repair returns safely"),
		WoTRuntimeBSPRepairRenderTJunctions(Parts, Stats, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("unsafe emergency fan falls back exactly one source triangle"),
		Stats.FallbackTriangles, uint64(1));
	TestEqual(TEXT("fallback preserves protected-edge host topology"),
		Parts[0].Indices, Before[0].Indices);
	TestEqual(TEXT("fallback preserves protected-edge host positions"),
		Parts[0].Positions, Before[0].Positions);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPMirroredFallbackWindingSpec,
	"WoT.World.RuntimeBSP.MirroredFallbackWinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPMirroredFallbackWindingSpec::RunTest(const FString&)
{
	for (const float MirrorX : {1.0f, -1.0f})
	{
		TArray<FWoTBSPPayloadData> Parts;
		Parts.SetNum(2);
		auto Mirror = [MirrorX](FVector3f Point)
		{
			Point.X *= MirrorX;
			return Point;
		};
		AddTJunctionTriangle(Parts[0],
			Mirror(FVector3f(1715.4674072265625f, -897.8675537109375f, -3.9205477237701416f)),
			Mirror(FVector3f(1715.46728515625f, -897.86737060546875f, 211.96627807617188f)),
			Mirror(FVector3f(1715.467041015625f, -897.84619140625f, 571.77703857421875f)),
			311, true);
		const FVector3f Donors[] = {
			{1715.46728515625f, -897.86737060546875f, 238.95211791992188f},
			{1715.4666748046875f, -897.86737060546875f, 274.93292236328125f},
			{1715.466064453125f, -897.86767578125f, 292.92361450195312f},
			{1715.466064453125f, -897.86767578125f, 328.90469360351562f},
			{1715.4676513671875f, -897.86737060546875f, 382.87643432617188f}};
		for (int32 Donor = 0; Donor < UE_ARRAY_COUNT(Donors); ++Donor)
		{
			const FVector3f Point = Mirror(Donors[Donor]);
			AddTJunctionTriangle(Parts[1], Point,
				Point + FVector3f(40 * MirrorX, 0, 0),
				Point + FVector3f(40 * MirrorX, 40, 0),
				313 + Donor, Donor == 0);
		}
		const FVector3d SourceA(Parts[0].Positions[0]);
		const FVector3d SourceB(Parts[0].Positions[1]);
		const FVector3d SourceC(Parts[0].Positions[2]);
		const FVector3d SourceNormal = FVector3d::CrossProduct(
			SourceB - SourceA, SourceC - SourceA);
		FWoTBSPTJunctionStats Stats;
		FString Error;
		const FString Label = MirrorX < 0 ? TEXT("mirrored") : TEXT("source");
		if (!TestTrue(Label + TEXT(" fallback repairs"),
			WoTRuntimeBSPRepairRenderTJunctions(Parts, Stats, Error)))
		{
			AddError(Label + TEXT(": ") + Error);
			continue;
		}
		TestEqual(Label + TEXT(" takes one local-projection fallback"),
			Stats.LocallyOrientedLoops, uint64(1));
		for (int32 At = 0; At < Parts[0].Indices.Num(); At += 3)
		{
			const FVector3d A(Parts[0].Positions[Parts[0].Indices[At]]);
			const FVector3d B(Parts[0].Positions[Parts[0].Indices[At + 1]]);
			const FVector3d C(Parts[0].Positions[Parts[0].Indices[At + 2]]);
			TestTrue(Label + TEXT(" emitted child keeps source-facing winding"),
				FVector3d::DotProduct(FVector3d::CrossProduct(B - A, C - A),
					SourceNormal) >= 0);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRepairBudgetFallbackSpec,
	"WoT.World.RuntimeBSP.RepairBudgetFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPRepairBudgetFallbackSpec::RunTest(const FString&)
{
	TArray<FWoTBSPPayloadData> Parts;
	Parts.SetNum(1);
	AddTJunctionTriangle(Parts[0], FVector3f(0, 0, 0),
		FVector3f(0.25f, 0, 0), FVector3f(0.25f, 10, 0), 101, true);
	AddTJunctionTriangle(Parts[0], FVector3f(0, 0, 0),
		FVector3f(0.25f, 10, 0), FVector3f(0, 10, 0), 101, false);
	AddTJunctionTriangle(Parts[0], FVector3f(0.25f, 0, 0),
		FVector3f(10, 0, 0), FVector3f(10, 10, 0), 202, false);
	AddTJunctionTriangle(Parts[0], FVector3f(0.25f, 0, 0),
		FVector3f(10, 10, 0), FVector3f(0.25f, 10, 0), 202, false);
	for (int32 Vertex = 0; Vertex < Parts[0].UV1.Num(); ++Vertex)
		Parts[0].UV1[Vertex] = Vertex < 6
			? FVector2f(0.0625f, 0.5f) : FVector2f(0.8125f, 0.5f);
	Parts[0].LightmapWidth = 8;
	Parts[0].LightmapHeight = 1;
	Parts[0].LightmapBGRA.Init(16, 8 * 4);
	for (int32 Channel = 0; Channel < 3; ++Channel)
		Parts[0].LightmapBGRA[6 * 4 + Channel] = 128;
	{
		TArray<FWoTBSPPayloadData> RibbonOnly = Parts;
		FWoTBSPTJunctionStats RibbonStats;
		FString RibbonError;
		TestTrue(TEXT("budget fallback fixture starts with an eligible ribbon"),
			WoTRuntimeBSPRepairRenderTJunctions(
				RibbonOnly, RibbonStats, RibbonError));
		TestTrue(TEXT("budget fallback fixture repairs its ribbon in isolation"),
			RibbonStats.RepairedLightmapRibbonSurfaces >= 1);
	}
	for (int32 Triangle = 0; Triangle < 40; ++Triangle)
	{
		const float Z = Triangle * 100.0f;
		AddTJunctionTriangle(Parts[0], FVector3f(-9000000, -9000000, Z),
			FVector3f(9000000, 8500000, Z + 17),
			FVector3f(-8500000, 9000000, Z + 43), 317, Triangle == 0);
	}
	const FWoTBSPPayloadData Before = Parts[0];
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticRepairBudgetFallback");
	FWoTBSPPartDescriptor& PartDescriptor = Descriptor.BSPParts.AddDefaulted_GetRef();
	PartDescriptor.PartIdentity = Descriptor.MapIdentity;
	PartDescriptor.Role = EWoTBSPPartRole::Main;
	WoTTest::FSpecWorldOptions Options;
	Options.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTRuntimeBSPWorld* Runtime = nullptr;
	uint32 FallbackLightmaps = 0;
	FWoTBSPTJunctionStats Repair;
	FString Error;
	if (!TestTrue(TEXT("over-budget repair still materializes the world"),
		WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Runtime,
			FallbackLightmaps, Error, {}, nullptr, nullptr, nullptr, nullptr,
			EWoTRuntimeSkyStageFault::None, nullptr, {}, nullptr, &Repair)))
	{
		AddError(Error);
		return false;
	}
	TestNotNull(TEXT("fallback materialization creates a runtime world"), Runtime);
	TestTrue(TEXT("materializer reports unrepaired render fallback"),
		Repair.bFellBackToUnrepaired);
	TestTrue(TEXT("materializer distinguishes whole-payload fallback"),
		Repair.bWholePayloadFallback);
	TestTrue(TEXT("fallback reason identifies the work budget"),
		Repair.FallbackReason.Contains(TEXT("cell visits")));
	TestEqual(TEXT("whole-payload fallback reports no applied ribbon surfaces"),
		Repair.RepairedLightmapRibbonSurfaces, uint64(0));
	TestEqual(TEXT("whole-payload fallback reports no applied ribbon triangles"),
		Repair.RepairedLightmapRibbonTriangles, uint64(0));
	TestEqual(TEXT("fallback keeps source positions"), Parts[0].Positions, Before.Positions);
	TestEqual(TEXT("fallback keeps source topology"), Parts[0].Indices, Before.Indices);
	TestEqual(TEXT("fallback render keeps every unrepaired triangle"),
		Runtime->GeometryComponents[0]->GetStaticMesh()->GetRenderData()
			->LODResources[0].IndexBuffer.GetNumIndices(), 132);
	return !HasAnyErrors();
}


namespace
{
struct FTJunctionAuditWitness
{
	FVector3f A = FVector3f::ZeroVector;
	FVector3f B = FVector3f::ZeroVector;
	FVector3f C = FVector3f::ZeroVector;
	FVector3f P = FVector3f::ZeroVector;
};

// Read the final render buffers, including generated vertices and radial edges.
// This audit shares neither spatial hash nor edge enumeration with the repair.
bool ReadTJunctionRenderMesh(UStaticMesh* Mesh, FWoTBSPPayloadData& Out)
{
	if (!Mesh || !Mesh->bAllowCPUAccess || !Mesh->GetRenderData()
		|| Mesh->GetRenderData()->LODResources.IsEmpty()) return false;
	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	for (uint32 At = 0; At < LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices(); ++At)
	{
		Out.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(At));
		Out.Normals.Add(FVector3f(
			LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(At)));
		Out.UV0.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0
			? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(At, 0)
			: FVector2f::ZeroVector);
		Out.UV1.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 1
			? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(At, 1)
			: FVector2f::ZeroVector);
	}
	for (int32 At = 0; At < LOD.IndexBuffer.GetNumIndices(); ++At)
		Out.Indices.Add(LOD.IndexBuffer.GetIndex(At));
	return !Out.Indices.IsEmpty();
}

uint64 AuditTJunctionRenderParts(const TArray<FWoTBSPPayloadData>& Parts,
	TArray<FString>& Examples, TArray<FTJunctionAuditWitness>* OutWitnesses = nullptr)
{
	TSet<FVector3f> Unique;
	for (const FWoTBSPPayloadData& Part : Parts)
		for (uint32 Index : Part.Indices) Unique.Add(Part.Positions[Index]);
	TArray<FVector3f> Points = Unique.Array();
	Points.Sort([](const FVector3f& A, const FVector3f& B)
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	});
	TMap<FVector3f, uint32> PointIds;
	for (int32 At = 0; At < Points.Num(); ++At) PointIds.Add(Points[At], uint32(At));
	auto EdgeKey = [](uint32 A, uint32 B)
	{ return (uint64(FMath::Min(A, B)) << 32) | FMath::Max(A, B); };
	// A geometric edge shared by two nonzero built triangles is matched;
	// a T-junction's unmatched long side has only one incident triangle.
	// Build adjacency solely from output buffers: source ownership and
	// helper repair counters play no role.
	TMap<uint64, uint32> NonzeroEdgeIncidences;
	for (const FWoTBSPPayloadData& Part : Parts)
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const FVector3f A = Part.Positions[Part.Indices[At]];
			const FVector3f B = Part.Positions[Part.Indices[At + 1]];
			const FVector3f C = Part.Positions[Part.Indices[At + 2]];
			if (FVector3d::CrossProduct(FVector3d(B) - FVector3d(A),
				FVector3d(C) - FVector3d(A)).SizeSquared() == 0) continue;
			const uint32 IdA = PointIds.FindChecked(A), IdB = PointIds.FindChecked(B), IdC = PointIds.FindChecked(C);
			++NonzeroEdgeIncidences.FindOrAdd(EdgeKey(IdA, IdB));
			++NonzeroEdgeIncidences.FindOrAdd(EdgeKey(IdB, IdC));
			++NonzeroEdgeIncidences.FindOrAdd(EdgeKey(IdC, IdA));
		}
	uint64 Count = 0;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const FVector3d HostA(Part.Positions[Part.Indices[At]]);
			const FVector3d HostB(Part.Positions[Part.Indices[At + 1]]);
			const FVector3d HostC(Part.Positions[Part.Indices[At + 2]]);
			if (FVector3d::CrossProduct(HostB - HostA, HostC - HostA).SizeSquared() == 0)
				continue;
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const FVector3d A(Part.Positions[Part.Indices[At + Edge]]);
				const FVector3d B(Part.Positions[Part.Indices[At + (Edge + 1) % 3]]);
				const FVector3d C(Part.Positions[Part.Indices[At + (Edge + 2) % 3]]);
				const uint32 IncidenceCount = NonzeroEdgeIncidences.FindRef(EdgeKey(
					PointIds.FindChecked(FVector3f(A)), PointIds.FindChecked(FVector3f(B))));
				if (IncidenceCount != 1) continue;
				const FVector3d Delta = B - A;
				const double LengthSquared = Delta.SizeSquared();
				if (LengthSquared == 0) continue;
				const FVector3d Min = A.ComponentMin(B) - FVector3d(0.01);
				const FVector3d Max = A.ComponentMax(B) + FVector3d(0.01);
				int32 Low = 0, High = Points.Num();
				while (Low < High)
				{
					const int32 Mid = Low + (High - Low) / 2;
					if (Points[Mid].X < Min.X) Low = Mid + 1;
					else High = Mid;
				}
				for (int32 PointIndex = Low; PointIndex < Points.Num()
					&& Points[PointIndex].X <= Max.X; ++PointIndex)
				{
					const FVector3d P(Points[PointIndex]);
					if (P.Y < Min.Y || P.Y > Max.Y || P.Z < Min.Z || P.Z > Max.Z
						|| (P - A).SizeSquared() <= 0.0001
						|| (P - B).SizeSquared() <= 0.0001
						|| (P - C).SizeSquared() <= 0.0001) continue;
					const double T = FVector3d::DotProduct(P - A, Delta) / LengthSquared;
					if (T > 0 && T < 1 && (P - (A + T * Delta)).SizeSquared() <= 0.0001)
					{
						++Count;
						if (OutWitnesses != nullptr)
						{
							FTJunctionAuditWitness& Witness = OutWitnesses->AddDefaulted_GetRef();
							Witness.A = FVector3f(A);
							Witness.B = FVector3f(B);
							Witness.C = FVector3f(C);
							Witness.P = FVector3f(P);
						}
						if (Examples.Num() < 12) Examples.Add(FString::Printf(
							TEXT("part=%d tri=%d edge=%d A=%s B=%s C=%s P=%s distance=%.9f nonzero_incidents=%u"),
							PartIndex, At / 3, Edge, *A.ToString(), *B.ToString(), *C.ToString(), *P.ToString(),
							(P - (A + T * Delta)).Size(), IncidenceCount));
					}
				}
			}
		}
	}
	return Count;
}

// Compare the actual cooked collision vertices and index topology against an
// independently built unrepaired collision mesh, including fallback streams.
FString TJunctionCollisionSignature(UStaticMesh& Mesh)
{
	FString Result;
	if (!Mesh.GetBodySetup()) return Result;
	for (const auto& TriangleMesh : Mesh.GetBodySetup()->TriMeshGeometries)
	{
		const auto& Vertices = TriangleMesh->Particles();
		for (uint32 At = 0; At < Vertices.Size(); ++At)
		{
			const auto& P = Vertices.GetX(At);
			Result += FString::Printf(TEXT("%.9g,%.9g,%.9g;"), double(P.X), double(P.Y), double(P.Z));
		}
		auto AppendIndices = [&Result](const auto& Indices)
		{
			for (const auto& Triangle : Indices)
				Result += FString::Printf(TEXT("%d,%d,%d;"), int32(Triangle[0]), int32(Triangle[1]), int32(Triangle[2]));
		};
		if (TriangleMesh->Elements().RequiresLargeIndices())
			AppendIndices(TriangleMesh->Elements().GetLargeIndexBuffer());
		else AppendIndices(TriangleMesh->Elements().GetSmallIndexBuffer());
	}
	return Result;
}

class FTJunctionRetailWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (uint64(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("T-junction retail writer overflow")));
		static const TCHAR* GateMaps[] = {
			TEXT("Arena_01"), TEXT("Arena_02"), TEXT("Arena_03"),
			TEXT("Arena_04"), TEXT("Arena_05"), TEXT("Arena_06"),
			TEXT("Arena_07"), TEXT("Arena_08"), TEXT("Arena_09"),
			TEXT("Arena_11"), TEXT("Arena_12"), TEXT("Battle2_01"),
			TEXT("Battle2_02"), TEXT("Battle2_03"), TEXT("Battle2_04"),
			TEXT("Battle2_05"), TEXT("Battle3_01"), TEXT("Battle3_02"),
			TEXT("Battle4_01")};
		for (const TCHAR* Map : GateMaps)
		{
			if (LogicalId == FString::Printf(TEXT("cache/world/%s"), Map)
				|| LogicalId.StartsWith(FString::Printf(TEXT("cache/bsp/%s/"), Map)))
			{
				Objects.Add(FString(LogicalId)).Append(Bytes.GetData(), Bytes.Num());
				break;
			}
		}
		if (LogicalId == TEXT("cache/texture/Engine__DefaultTexture"))
		{
			auto& Object = TextureObjects.AddDefaulted_GetRef();
			Object.LogicalId = FString(LogicalId);
			Object.Schema = FString(Schema);
			Object.Bytes.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}
	TArray<FWoTPayloadActivationTestObject> TextureObjects;
	TMap<FString, TArray<uint8>> Objects;
};
class FTJunctionRetailProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{ return FWoTBootstrapStatus::Success(); }
	bool IsCancellationRequested() const override { return false; }
};

struct FX6CameraPose
{
	FVector3d Location = FVector3d::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

struct FX6RayHit
{
	bool bHit = false;
	double Distance = TNumericLimits<double>::Max();
	int32 Part = INDEX_NONE;
	int32 Triangle = INDEX_NONE;
	int32 Surface = INDEX_NONE;
	int32 Section = INDEX_NONE;
	int32 Node = INDEX_NONE;
	FVector3d Position = FVector3d::ZeroVector;
	FVector3d Normal = FVector3d::ZeroVector;
	FVector2d UV0 = FVector2d::ZeroVector;
	FVector2d UV1 = FVector2d::ZeroVector;
};

FX6CameraPose X6CameraAtVideoTime(double VideoSeconds)
{
	// The capture begins at timeline 0.15. The director's two-key free-camera
	// path uses cubic in/out time followed by its endpoint-clamped cubic spline.
	const double TimelineSeconds = VideoSeconds + 0.15;
	const double Raw = FMath::Clamp(TimelineSeconds / 4.3, 0.0, 1.0);
	const double Alpha = Raw < 0.5
		? 4.0 * Raw * Raw * Raw
		: 1.0 - FMath::Pow(-2.0 * Raw + 2.0, 3.0) / 2.0;
	const FVector3d A(-17350.0, -8390.0, 21061.0);
	const FVector3d B(-17342.0, -8449.0, 21061.0);
	const FVector3d Tangent = 0.5 * (B - A);
	FX6CameraPose Result;
	Result.Location = FMath::CubicInterp(A, Tangent, B, Tangent, Alpha);
	Result.Rotation = FRotator(
		FMath::Lerp(21.0, 26.0, Alpha), 278.0, 0.0);
	return Result;
}

FX6CameraPose A2CameraAtVideoTime(double VideoSeconds)
{
	// Shot A2 captures from timeline 0.75. Its two-key free camera uses the
	// director's cubic in/out time and endpoint-clamped cubic interpolation.
	const double TimelineSeconds = VideoSeconds + 0.75;
	const double Raw = FMath::Clamp(TimelineSeconds / 5.6, 0.0, 1.0);
	const double Alpha = Raw < 0.5
		? 4.0 * Raw * Raw * Raw
		: 1.0 - FMath::Pow(-2.0 * Raw + 2.0, 3.0) / 2.0;
	const FVector3d A(322.0, -652.0, 82.0);
	const FVector3d B(328.0, -620.0, 80.0);
	const FVector3d Tangent = 0.5 * (B - A);
	FX6CameraPose Result;
	Result.Location = FMath::CubicInterp(A, Tangent, B, Tangent, Alpha);
	Result.Rotation = FRotator(
		FMath::Lerp(6.0, 6.5, Alpha), 86.5, 0.0);
	return Result;
}

double A2FOVAtVideoTime(double VideoSeconds)
{
	const double Raw = FMath::Clamp((VideoSeconds + 0.75) / 5.6, 0.0, 1.0);
	const double Alpha = Raw < 0.5
		? 4.0 * Raw * Raw * Raw
		: 1.0 - FMath::Pow(-2.0 * Raw + 2.0, 3.0) / 2.0;
	return FMath::Lerp(66.0, 61.0, Alpha);
}

FX6CameraPose A5RV3CameraAtVideoTime(double VideoSeconds)
{
	// The scout capture begins at timeline 1.5. Its two-key free camera uses
	// the director's cubic in/out time and endpoint-clamped cubic spline.
	const double TimelineSeconds = VideoSeconds + 1.5;
	const double Raw = FMath::Clamp(TimelineSeconds / 5.8, 0.0, 1.0);
	const double Alpha = Raw < 0.5
		? 4.0 * Raw * Raw * Raw
		: 1.0 - FMath::Pow(-2.0 * Raw + 2.0, 3.0) / 2.0;
	const FVector3d A(-680.0, 0.0, -245.0);
	const FVector3d B(-700.0, -10.0, -247.0);
	const FVector3d Tangent = 0.5 * (B - A);
	FX6CameraPose Result;
	Result.Location = FMath::CubicInterp(A, Tangent, B, Tangent, Alpha);
	Result.Rotation = FRotator(FMath::Lerp(-3.0, -2.0, Alpha), 180.0, 0.0);
	return Result;
}

void A5RV3PixelRay(double VideoSeconds, int32 PixelX, int32 PixelY,
	FVector3d& OutOrigin, FVector3d& OutDirection)
{
	const FX6CameraPose Pose = A5RV3CameraAtVideoTime(VideoSeconds);
	const FRotationMatrix Axes(Pose.Rotation);
	const double ScreenX = (double(PixelX) + 0.5) / 960.0 * 2.0 - 1.0;
	const double ScreenY = 1.0 - (double(PixelY) + 0.5) / 540.0 * 2.0;
	const double TanHalfHorizontalFOV = FMath::Tan(
		FMath::DegreesToRadians(80.0) * 0.5);
	OutOrigin = Pose.Location;
	OutDirection = (FVector3d(Axes.GetUnitAxis(EAxis::X))
		+ FVector3d(Axes.GetUnitAxis(EAxis::Y)) * ScreenX * TanHalfHorizontalFOV
		+ FVector3d(Axes.GetUnitAxis(EAxis::Z)) * ScreenY
			* TanHalfHorizontalFOV / (16.0 / 9.0)).GetSafeNormal();
}

void A2PixelRay(double VideoSeconds, int32 PixelX, int32 PixelY,
	FVector3d& OutOrigin, FVector3d& OutDirection)
{
	const FX6CameraPose Pose = A2CameraAtVideoTime(VideoSeconds);
	const FRotationMatrix Axes(Pose.Rotation);
	const double ScreenX = (double(PixelX) + 0.5) / 3840.0 * 2.0 - 1.0;
	const double ScreenY = 1.0 - (double(PixelY) + 0.5) / 2160.0 * 2.0;
	const double TanHalfHorizontalFOV = FMath::Tan(
		FMath::DegreesToRadians(A2FOVAtVideoTime(VideoSeconds)) * 0.5);
	OutOrigin = Pose.Location;
	OutDirection = (FVector3d(Axes.GetUnitAxis(EAxis::X))
		+ FVector3d(Axes.GetUnitAxis(EAxis::Y)) * ScreenX * TanHalfHorizontalFOV
		+ FVector3d(Axes.GetUnitAxis(EAxis::Z)) * ScreenY
			* TanHalfHorizontalFOV / (16.0 / 9.0)).GetSafeNormal();
}

void X6PixelRay(double VideoSeconds, int32 PixelX, int32 PixelY,
	FVector3d& OutOrigin, FVector3d& OutDirection)
{
	const FX6CameraPose Pose = X6CameraAtVideoTime(VideoSeconds);
	const FRotationMatrix Axes(Pose.Rotation);
	const double ScreenX = (double(PixelX) + 0.5) / 3840.0 * 2.0 - 1.0;
	const double ScreenY = 1.0 - (double(PixelY) + 0.5) / 2160.0 * 2.0;
	const double TanHalfHorizontalFOV = FMath::Tan(FMath::DegreesToRadians(75.0) * 0.5);
	OutOrigin = Pose.Location;
	OutDirection = (FVector3d(Axes.GetUnitAxis(EAxis::X))
		+ FVector3d(Axes.GetUnitAxis(EAxis::Y)) * ScreenX * TanHalfHorizontalFOV
		+ FVector3d(Axes.GetUnitAxis(EAxis::Z)) * ScreenY
			* TanHalfHorizontalFOV / (16.0 / 9.0)).GetSafeNormal();
}

bool X6IntersectTriangle(const FVector3d& Origin, const FVector3d& Direction,
	const FVector3d& A, const FVector3d& B, const FVector3d& C,
	double& OutDistance, FVector3d& OutBarycentric)
{
	const FVector3d Edge1 = B - A;
	const FVector3d Edge2 = C - A;
	const FVector3d P = FVector3d::CrossProduct(Direction, Edge2);
	const double Determinant = FVector3d::DotProduct(Edge1, P);
	if (FMath::Abs(Determinant) <= 1.0e-10) return false;
	const double Inverse = 1.0 / Determinant;
	const FVector3d T = Origin - A;
	const double U = FVector3d::DotProduct(T, P) * Inverse;
	if (U < -1.0e-8 || U > 1.0 + 1.0e-8) return false;
	const FVector3d Q = FVector3d::CrossProduct(T, Edge1);
	const double V = FVector3d::DotProduct(Direction, Q) * Inverse;
	if (V < -1.0e-8 || U + V > 1.0 + 1.0e-8) return false;
	const double Distance = FVector3d::DotProduct(Edge2, Q) * Inverse;
	if (Distance <= 0) return false;
	OutDistance = Distance;
	OutBarycentric = FVector3d(1.0 - U - V, U, V);
	return true;
}

FX6RayHit X6HitPayload(const TArray<FWoTBSPPayloadData>& Parts,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	X6PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FX6RayHit Result;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const uint32 I0 = Part.Indices[At];
			const uint32 I1 = Part.Indices[At + 1];
			const uint32 I2 = Part.Indices[At + 2];
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction,
				FVector3d(Part.Positions[I0]), FVector3d(Part.Positions[I1]),
				FVector3d(Part.Positions[I2]), Distance, Barycentric)
				|| Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Part = PartIndex;
			Result.Triangle = At / 3;
			Result.Surface = Part.TriangleSurfaces.IsValidIndex(At / 3)
				? int32(Part.TriangleSurfaces[At / 3]) : INDEX_NONE;
			Result.Section = INDEX_NONE;
			for (int32 Section = 0; Section < Part.Sections.Num(); ++Section)
				if (At >= int32(Part.Sections[Section].FirstIndex)
					&& At < int32(Part.Sections[Section].FirstIndex
						+ Part.Sections[Section].IndexCount))
				{ Result.Section = Section; break; }
			Result.Position = Origin + Distance * Direction;
			if (Part.Normals.IsValidIndex(I0) && Part.Normals.IsValidIndex(I1)
				&& Part.Normals.IsValidIndex(I2))
				Result.Normal = (Barycentric.X * FVector3d(Part.Normals[I0])
					+ Barycentric.Y * FVector3d(Part.Normals[I1])
					+ Barycentric.Z * FVector3d(Part.Normals[I2])).GetSafeNormal();
			Result.UV0 = Barycentric.X * FVector2d(Part.UV0[I0])
				+ Barycentric.Y * FVector2d(Part.UV0[I1])
				+ Barycentric.Z * FVector2d(Part.UV0[I2]);
			Result.UV1 = Barycentric.X * FVector2d(Part.UV1[I0])
				+ Barycentric.Y * FVector2d(Part.UV1[I1])
				+ Barycentric.Z * FVector2d(Part.UV1[I2]);
		}
	}
	return Result;
}

FX6RayHit A2HitPayload(const TArray<FWoTBSPPayloadData>& Parts,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	A2PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FX6RayHit Result;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const uint32 I0 = Part.Indices[At];
			const uint32 I1 = Part.Indices[At + 1];
			const uint32 I2 = Part.Indices[At + 2];
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction,
				FVector3d(Part.Positions[I0]), FVector3d(Part.Positions[I1]),
				FVector3d(Part.Positions[I2]), Distance, Barycentric)
				|| Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Part = PartIndex;
			Result.Triangle = At / 3;
			Result.Surface = Part.TriangleSurfaces.IsValidIndex(At / 3)
				? int32(Part.TriangleSurfaces[At / 3]) : INDEX_NONE;
			Result.Section = INDEX_NONE;
			for (int32 Section = 0; Section < Part.Sections.Num(); ++Section)
				if (At >= int32(Part.Sections[Section].FirstIndex)
					&& At < int32(Part.Sections[Section].FirstIndex
						+ Part.Sections[Section].IndexCount))
				{ Result.Section = Section; break; }
			Result.Position = Origin + Distance * Direction;
			if (Part.Normals.IsValidIndex(I0) && Part.Normals.IsValidIndex(I1)
				&& Part.Normals.IsValidIndex(I2))
				Result.Normal = (Barycentric.X * FVector3d(Part.Normals[I0])
					+ Barycentric.Y * FVector3d(Part.Normals[I1])
					+ Barycentric.Z * FVector3d(Part.Normals[I2])).GetSafeNormal();
			Result.UV0 = Barycentric.X * FVector2d(Part.UV0[I0])
				+ Barycentric.Y * FVector2d(Part.UV0[I1])
				+ Barycentric.Z * FVector2d(Part.UV0[I2]);
			Result.UV1 = Barycentric.X * FVector2d(Part.UV1[I0])
				+ Barycentric.Y * FVector2d(Part.UV1[I1])
				+ Barycentric.Z * FVector2d(Part.UV1[I2]);
		}
	}
	return Result;
}

FX6RayHit A5RV3HitPayload(const TArray<FWoTBSPPayloadData>& Parts,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	A5RV3PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FX6RayHit Result;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			const uint32 I0 = Part.Indices[At];
			const uint32 I1 = Part.Indices[At + 1];
			const uint32 I2 = Part.Indices[At + 2];
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction,
				FVector3d(Part.Positions[I0]), FVector3d(Part.Positions[I1]),
				FVector3d(Part.Positions[I2]), Distance, Barycentric)
				|| Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Part = PartIndex;
			Result.Triangle = At / 3;
			Result.Surface = Part.TriangleSurfaces.IsValidIndex(At / 3)
				? int32(Part.TriangleSurfaces[At / 3]) : INDEX_NONE;
			Result.Section = INDEX_NONE;
			for (int32 Section = 0; Section < Part.Sections.Num(); ++Section)
				if (At >= int32(Part.Sections[Section].FirstIndex)
					&& At < int32(Part.Sections[Section].FirstIndex
						+ Part.Sections[Section].IndexCount))
				{ Result.Section = Section; break; }
			Result.Position = Origin + Distance * Direction;
			if (Part.Normals.IsValidIndex(I0) && Part.Normals.IsValidIndex(I1)
				&& Part.Normals.IsValidIndex(I2))
				Result.Normal = (Barycentric.X * FVector3d(Part.Normals[I0])
					+ Barycentric.Y * FVector3d(Part.Normals[I1])
					+ Barycentric.Z * FVector3d(Part.Normals[I2])).GetSafeNormal();
			Result.UV0 = Barycentric.X * FVector2d(Part.UV0[I0])
				+ Barycentric.Y * FVector2d(Part.UV0[I1])
				+ Barycentric.Z * FVector2d(Part.UV0[I2]);
			Result.UV1 = Barycentric.X * FVector2d(Part.UV1[I0])
				+ Barycentric.Y * FVector2d(Part.UV1[I1])
				+ Barycentric.Z * FVector2d(Part.UV1[I2]);
		}
	}
	return Result;
}

FString A2HitStack(const TArray<FWoTBSPPayloadData>& Parts,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	A2PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	struct FEntry { double Distance; int32 Part; int32 Triangle; uint32 Surface; };
	TArray<FEntry> Entries;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction,
				FVector3d(Part.Positions[Part.Indices[At]]),
				FVector3d(Part.Positions[Part.Indices[At + 1]]),
				FVector3d(Part.Positions[Part.Indices[At + 2]]),
				Distance, Barycentric)) continue;
			Entries.Add({Distance, PartIndex, At / 3,
				Part.TriangleSurfaces.IsValidIndex(At / 3)
					? Part.TriangleSurfaces[At / 3] : MAX_uint32});
		}
	}
	Entries.Sort([](const FEntry& A, const FEntry& B)
	{
		if (A.Distance != B.Distance) return A.Distance < B.Distance;
		if (A.Part != B.Part) return A.Part < B.Part;
		return A.Triangle < B.Triangle;
	});
	FString Result;
	const double MaximumDistance = Entries.IsEmpty()
		? 0.0 : Entries[0].Distance + 1.0;
	for (const FEntry& Entry : Entries)
	{
		if (Entry.Distance > MaximumDistance || Result.Len() > 1000) break;
		if (!Result.IsEmpty()) Result += TEXT(" | ");
		Result += FString::Printf(TEXT("p%d:t%d:s%u:d%.9f"), Entry.Part,
			Entry.Triangle, Entry.Surface, Entry.Distance);
	}
	return Result;
}

FX6RayHit X6HitWorldNodes(const FWoTRuntimeWorldResource& Resource,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	X6PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FX6RayHit Result;
	for (int32 NodeIndex = 0; NodeIndex < Resource.Nodes().Num(); ++NodeIndex)
	{
		const FWoTRuntimeWorldNode& Node = Resource.Nodes()[NodeIndex];
		if (Node.VertexCount < 3 || Node.VertexPool < 0
			|| Node.VertexPool + Node.VertexCount > Resource.Vertices().Num()) continue;
		const int32 FirstPoint = Resource.Vertices()[Node.VertexPool].Point;
		if (!Resource.Points().IsValidIndex(FirstPoint)) continue;
		const FVector3d A(Resource.Points()[FirstPoint]);
		for (int32 Corner = 1; Corner + 1 < Node.VertexCount; ++Corner)
		{
			const int32 PointB = Resource.Vertices()[Node.VertexPool + Corner].Point;
			const int32 PointC = Resource.Vertices()[Node.VertexPool + Corner + 1].Point;
			if (!Resource.Points().IsValidIndex(PointB)
				|| !Resource.Points().IsValidIndex(PointC)) continue;
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction, A,
				FVector3d(Resource.Points()[PointB]), FVector3d(Resource.Points()[PointC]),
				Distance, Barycentric) || Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Node = NodeIndex;
			Result.Surface = Node.Surface;
			Result.Position = Origin + Distance * Direction;
		}
	}
	return Result;
}

FX6RayHit A2HitWorldNodes(const FWoTRuntimeWorldResource& Resource,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	A2PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FX6RayHit Result;
	for (int32 NodeIndex = 0; NodeIndex < Resource.Nodes().Num(); ++NodeIndex)
	{
		const FWoTRuntimeWorldNode& Node = Resource.Nodes()[NodeIndex];
		if (Node.VertexCount < 3 || Node.VertexPool < 0
			|| Node.VertexPool + Node.VertexCount > Resource.Vertices().Num()) continue;
		const int32 FirstPoint = Resource.Vertices()[Node.VertexPool].Point;
		if (!Resource.Points().IsValidIndex(FirstPoint)) continue;
		const FVector3d A(Resource.Points()[FirstPoint]);
		for (int32 Corner = 1; Corner + 1 < Node.VertexCount; ++Corner)
		{
			const int32 PointB = Resource.Vertices()[Node.VertexPool + Corner].Point;
			const int32 PointC = Resource.Vertices()[Node.VertexPool + Corner + 1].Point;
			if (!Resource.Points().IsValidIndex(PointB)
				|| !Resource.Points().IsValidIndex(PointC)) continue;
			double Distance = 0;
			FVector3d Barycentric;
			if (!X6IntersectTriangle(Origin, Direction, A,
				FVector3d(Resource.Points()[PointB]), FVector3d(Resource.Points()[PointC]),
				Distance, Barycentric) || Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Node = NodeIndex;
			Result.Surface = Node.Surface;
			Result.Position = Origin + Distance * Direction;
		}
	}
	return Result;
}

double X6PointSegmentDistance(const FVector3d& Point,
	const FVector3d& A, const FVector3d& B)
{
	const FVector3d Delta = B - A;
	const double LengthSquared = Delta.SizeSquared();
	const double T = LengthSquared == 0 ? 0.0 : FMath::Clamp(
		FVector3d::DotProduct(Point - A, Delta) / LengthSquared, 0.0, 1.0);
	return (Point - (A + T * Delta)).Size();
}

double X6NodeEdgeGap(const FWoTRuntimeWorldResource& Resource,
	int32 NodeA, int32 NodeB)
{
	if (!Resource.Nodes().IsValidIndex(NodeA) || !Resource.Nodes().IsValidIndex(NodeB))
		return TNumericLimits<double>::Max();
	const FWoTRuntimeWorldNode& A = Resource.Nodes()[NodeA];
	const FWoTRuntimeWorldNode& B = Resource.Nodes()[NodeB];
	double Result = TNumericLimits<double>::Max();
	for (int32 EdgeA = 0; EdgeA < A.VertexCount; ++EdgeA)
	{
		const FVector3d A0(Resource.Points()[Resource.Vertices()[A.VertexPool + EdgeA].Point]);
		const FVector3d A1(Resource.Points()[Resource.Vertices()[A.VertexPool
			+ (EdgeA + 1) % A.VertexCount].Point]);
		for (int32 EdgeB = 0; EdgeB < B.VertexCount; ++EdgeB)
		{
			const FVector3d B0(Resource.Points()[Resource.Vertices()[B.VertexPool + EdgeB].Point]);
			const FVector3d B1(Resource.Points()[Resource.Vertices()[B.VertexPool
				+ (EdgeB + 1) % B.VertexCount].Point]);
			Result = FMath::Min(Result, X6PointSegmentDistance(A0, B0, B1));
			Result = FMath::Min(Result, X6PointSegmentDistance(A1, B0, B1));
			Result = FMath::Min(Result, X6PointSegmentDistance(B0, A0, A1));
			Result = FMath::Min(Result, X6PointSegmentDistance(B1, A0, A1));
		}
	}
	return Result;
}

double X6NodeRibbonWidth(const FWoTRuntimeWorldResource& Resource,
	int32 RibbonNode, int32 NeighborNode)
{
	if (!Resource.Nodes().IsValidIndex(RibbonNode)
		|| !Resource.Nodes().IsValidIndex(NeighborNode))
		return TNumericLimits<double>::Max();
	const FWoTRuntimeWorldNode& Ribbon = Resource.Nodes()[RibbonNode];
	const FWoTRuntimeWorldNode& Neighbor = Resource.Nodes()[NeighborNode];
	for (int32 RibbonEdge = 0; RibbonEdge < Ribbon.VertexCount; ++RibbonEdge)
	{
		const FVector3d A(Resource.Points()[Resource.Vertices()[Ribbon.VertexPool + RibbonEdge].Point]);
		const FVector3d B(Resource.Points()[Resource.Vertices()[Ribbon.VertexPool
			+ (RibbonEdge + 1) % Ribbon.VertexCount].Point]);
		for (int32 NeighborEdge = 0; NeighborEdge < Neighbor.VertexCount; ++NeighborEdge)
		{
			const FVector3d C(Resource.Points()[Resource.Vertices()[Neighbor.VertexPool + NeighborEdge].Point]);
			const FVector3d D(Resource.Points()[Resource.Vertices()[Neighbor.VertexPool
				+ (NeighborEdge + 1) % Neighbor.VertexCount].Point]);
			if (!((A == C && B == D) || (A == D && B == C))) continue;
			double Width = 0;
			const double EdgeLength = (B - A).Size();
			for (int32 Corner = 0; Corner < Ribbon.VertexCount; ++Corner)
			{
				const FVector3d Point(Resource.Points()[Resource.Vertices()[
					Ribbon.VertexPool + Corner].Point]);
				Width = FMath::Max(Width, EdgeLength == 0 ? 0.0
					: FVector3d::CrossProduct(Point - A, B - A).Size() / EdgeLength);
			}
			return Width;
		}
	}
	return TNumericLimits<double>::Max();
}

FString X6NodeVertices(const FWoTRuntimeWorldResource& Resource, int32 NodeIndex)
{
	if (!Resource.Nodes().IsValidIndex(NodeIndex)) return TEXT("invalid");
	const FWoTRuntimeWorldNode& Node = Resource.Nodes()[NodeIndex];
	FString Result;
	for (int32 Corner = 0; Corner < Node.VertexCount; ++Corner)
	{
		const int32 Point = Resource.Vertices()[Node.VertexPool + Corner].Point;
		if (Corner > 0) Result += TEXT(" | ");
		Result += Resource.Points().IsValidIndex(Point)
			? Resource.Points()[Point].ToString() : TEXT("invalid");
	}
	return Result;
}

FString X6AtlasEvidence(const TArray<FWoTBSPPayloadData>& Parts,
	const FX6RayHit& Hit)
{
	const FWoTBSPPayloadData* Atlas = Parts.FindByPredicate(
		[](const FWoTBSPPayloadData& Part) { return Part.LightmapWidth > 0; });
	if (!Atlas || !Hit.bHit) return TEXT("no-atlas");
	const double TexelX = Hit.UV1.X * Atlas->LightmapWidth - 0.5;
	const double TexelY = Hit.UV1.Y * Atlas->LightmapHeight - 0.5;
	const int32 X = FMath::Clamp(FMath::FloorToInt(TexelX), 0, int32(Atlas->LightmapWidth) - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(TexelY), 0, int32(Atlas->LightmapHeight) - 1);
	auto Pixel = [Atlas](int32 PX, int32 PY)
	{
		PX = FMath::Clamp(PX, 0, int32(Atlas->LightmapWidth) - 1);
		PY = FMath::Clamp(PY, 0, int32(Atlas->LightmapHeight) - 1);
		const int32 Offset = (PY * Atlas->LightmapWidth + PX) * 4;
		return FString::Printf(TEXT("(%u,%u,%u,%u)"),
			Atlas->LightmapBGRA[Offset], Atlas->LightmapBGRA[Offset + 1],
			Atlas->LightmapBGRA[Offset + 2], Atlas->LightmapBGRA[Offset + 3]);
	};
	FString Placement = TEXT("none");
	if (Atlas->AnimationSurfaces.IsValidIndex(Hit.Surface))
	{
		const FWoTBSPAnimationSurface& Surface = Atlas->AnimationSurfaces[Hit.Surface];
		Placement = FString::Printf(TEXT("x=%u y=%u w=%u h=%u"),
			Surface.X, Surface.Y, Surface.W, Surface.H);
	}
	return FString::Printf(TEXT("uv=(%.9f,%.9f) texel=(%.6f,%.6f) floor=(%d,%d) samples=%s,%s,%s,%s placement=%s"),
		Hit.UV1.X, Hit.UV1.Y, TexelX, TexelY, X, Y,
		*Pixel(X, Y), *Pixel(X + 1, Y), *Pixel(X, Y + 1), *Pixel(X + 1, Y + 1),
		*Placement);
}

double X6AtlasLuminance(const TArray<FWoTBSPPayloadData>& Parts,
	const FX6RayHit& Hit)
{
	const FWoTBSPPayloadData* Atlas = Parts.FindByPredicate(
		[](const FWoTBSPPayloadData& Part) { return Part.LightmapWidth > 0; });
	if (!Atlas || !Hit.bHit || Atlas->LightmapBGRA.IsEmpty()) return -1;
	const double TexelX = Hit.UV1.X * Atlas->LightmapWidth - 0.5;
	const double TexelY = Hit.UV1.Y * Atlas->LightmapHeight - 0.5;
	const int32 X = FMath::FloorToInt(TexelX);
	const int32 Y = FMath::FloorToInt(TexelY);
	const double FractionX = TexelX - X;
	const double FractionY = TexelY - Y;
	auto Pixel = [Atlas](int32 PX, int32 PY)
	{
		PX = FMath::Clamp(PX, 0, int32(Atlas->LightmapWidth) - 1);
		PY = FMath::Clamp(PY, 0, int32(Atlas->LightmapHeight) - 1);
		const int32 Offset = (PY * Atlas->LightmapWidth + PX) * 4;
		return (double(Atlas->LightmapBGRA[Offset])
			+ Atlas->LightmapBGRA[Offset + 1] + Atlas->LightmapBGRA[Offset + 2]) / 3.0;
	};
	return FMath::Lerp(
		FMath::Lerp(Pixel(X, Y), Pixel(X + 1, Y), FractionX),
		FMath::Lerp(Pixel(X, Y + 1), Pixel(X + 1, Y + 1), FractionX),
		FractionY);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPX6BrazierHallSeamSpec,
	"WoT.World.RuntimeBSP.X6BrazierHallSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPX6BrazierHallSeamSpec::RunTest(const FString&)
{
	TestFalse(TEXT("uniform black final UV1 cannot satisfy the lit-neighbour acceptance"),
		WoTX6Test::FinalCenterUsesLitNeighbour(56.0, 51.0, 0.0, 0.0, 0.0));
	if (!WoTReferenceTreeExists())
	{ AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	auto Loaded = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("authenticated retail contract loads"), Loaded.IsError())) return false;
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : Loaded.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World && Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase)) continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		auto& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(TEXT("authenticated package bytes load"), FFileHelper::LoadFileToArray(
			Source.Bytes, *(WoTReferencePath(TEXT("original")) / Fact.SourcePath)))) return false;
	}
	FTJunctionRetailWriter Writer;
	FTJunctionRetailProgress Progress;
	auto Imported = ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("authenticated production world set imports"), Imported.IsError()))
	{ AddError(Imported.GetError().ToString()); return false; }
	const FWoTWorldDescriptor* Descriptor = Imported.GetValue().Worlds.FindByPredicate(
		[](const FWoTWorldDescriptor& Item) { return Item.MapIdentity == TEXT("Battle2_02"); });
	if (!TestNotNull(TEXT("Battle2_02 authenticated descriptor"), Descriptor)) return false;
	TArray<FWoTBSPPayloadData> Parts;
	FString Error;
	for (const FWoTBSPPartDescriptor& Part : Descriptor->BSPParts)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(Part.LogicalId);
		if (!TestNotNull(TEXT("Battle2_02 authenticated BSP object"), Bytes)) return false;
		auto Decoded = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			MoveTemp(*Bytes), TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
		if (!TestFalse(TEXT("Battle2_02 BSP object frame decodes"), Decoded.IsError())
			|| !TestTrue(TEXT("Battle2_02 BSP payload decodes"),
				UWoTBSPPayloadLibrary::DecodePayload(Decoded.GetValue().Payload(),
					Parts.AddDefaulted_GetRef(), Error))) return false;
	}
	TArray<uint8>* WorldBytes = Writer.Objects.Find(TEXT("cache/world/Battle2_02"));
	if (!TestNotNull(TEXT("Battle2_02 authenticated world object"), WorldBytes)) return false;
	auto DecodedWorld = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
		MoveTemp(*WorldBytes), TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!TestFalse(TEXT("Battle2_02 world object frame decodes"), DecodedWorld.IsError())) return false;
	FWoTRuntimeWorldResource Resource;
	const FWoTBootstrapStatus WorldLoaded = Resource.LoadObjectForTesting(
		MoveTemp(DecodedWorld).GetValue());
	if (!TestFalse(TEXT("Battle2_02 world records decode"), WorldLoaded.IsError()))
	{ AddError(WorldLoaded.GetError().ToString()); return false; }

	WoTTest::FSpecWorldOptions Options;
	Options.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTRuntimeBSPWorld* Runtime = nullptr;
	uint32 FallbackLightmaps = 0;
	FWoTBSPTJunctionStats Repair;
	double BSPMilliseconds = 0;
	if (!TestTrue(TEXT("Battle2_02 production world render build"),
		WoTWorldBuildRuntimeBSPForTesting(*World, *Descriptor, Parts, Runtime,
			FallbackLightmaps, Error, {}, nullptr, nullptr, nullptr, nullptr,
			EWoTRuntimeSkyStageFault::None, nullptr, {}, nullptr, &Repair,
			&BSPMilliseconds)))
	{ AddError(Error); return false; }
	TArray<FWoTBSPPayloadData> FinalRender;
	for (UStaticMeshComponent* Component : Runtime->GeometryComponents)
	{
		if (!Component || !Component->IsVisible()) continue;
		if (!TestTrue(TEXT("Battle2_02 built visible mesh has CPU render buffers"),
			ReadTJunctionRenderMesh(Component->GetStaticMesh(),
				FinalRender.AddDefaulted_GetRef()))) return false;
	}
	for (const FWoTBSPPayloadData& Part : Parts)
	{
		int32 TriangleCount = 0;
		double MaximumMinimumAltitude = 0;
		double MaximumEdgeLength = 0;
		double MinimumNormalDot = 1.0;
		FVector3d FirstNormal = FVector3d::ZeroVector;
		TSet<FVector3f> Positions;
		for (int32 At = 0; At < Part.Indices.Num(); At += 3)
		{
			if (!Part.TriangleSurfaces.IsValidIndex(At / 3)
				|| Part.TriangleSurfaces[At / 3] != 2467) continue;
			const FVector3d A(Part.Positions[Part.Indices[At]]);
			const FVector3d B(Part.Positions[Part.Indices[At + 1]]);
			const FVector3d C(Part.Positions[Part.Indices[At + 2]]);
			const double Longest = FMath::Max3((B - A).Size(),
				(C - B).Size(), (A - C).Size());
			const double Cross = FVector3d::CrossProduct(B - A, C - A).Size();
			const FVector3d Normal = FVector3d::CrossProduct(B - A, C - A)
				.GetSafeNormal();
			if (TriangleCount == 0) FirstNormal = Normal;
			else MinimumNormalDot = FMath::Min(MinimumNormalDot,
				FVector3d::DotProduct(FirstNormal, Normal));
			MaximumMinimumAltitude = FMath::Max(MaximumMinimumAltitude,
				Longest > 0 ? Cross / Longest : 0);
			MaximumEdgeLength = FMath::Max(MaximumEdgeLength, Longest);
			Positions.Add(FVector3f(A));
			Positions.Add(FVector3f(B));
			Positions.Add(FVector3f(C));
			++TriangleCount;
		}
		if (TriangleCount > 0)
			AddInfo(FString::Printf(TEXT("X6Surface2467 triangles=%d unique_positions=%d max_min_altitude=%.9f max_edge=%.9f min_normal_dot=%.9f normal=%s"),
				TriangleCount, Positions.Num(), MaximumMinimumAltitude,
				MaximumEdgeLength, MinimumNormalDot, *FirstNormal.ToString()));
	}

	struct FSample { double Time; int32 X; int32 Y; };
	const FSample Samples[] = {{1.4, 1302, 1360}, {1.6, 1298, 1360},
		{1.8, 1294, 1360}, {1.6, 1304, 1300}};
	double LargestGap = 0;
	for (const FSample& Sample : Samples)
	{
		int32 DarkestX = Sample.X;
		double DarkestLuminance = TNumericLimits<double>::Max();
		for (int32 X = Sample.X - 20; X <= Sample.X + 20; ++X)
		{
			const double Luminance = X6AtlasLuminance(Parts,
				X6HitPayload(Parts, Sample.Time, X, Sample.Y));
			if (Luminance >= 0 && Luminance < DarkestLuminance)
			{ DarkestLuminance = Luminance; DarkestX = X; }
		}
		FX6RayHit Previous;
		for (int32 X = Sample.X - 16; X <= Sample.X + 16; ++X)
		{
			const FX6RayHit NodeHit = X6HitWorldNodes(Resource, Sample.Time, X, Sample.Y);
			if (Previous.bHit && NodeHit.bHit
				&& (Previous.Node != NodeHit.Node || Previous.Surface != NodeHit.Surface))
			{
				const double Gap = X6NodeEdgeGap(Resource, Previous.Node, NodeHit.Node);
				LargestGap = FMath::Max(LargestGap, Gap);
				AddInfo(FString::Printf(TEXT("X6LineTransition video_t=%.1f pixels=%d/%d y=%d nodes=%d/%d surfaces=%d/%d edge_gap=%.9f positions=%s/%s"),
					Sample.Time, X - 1, X, Sample.Y, Previous.Node, NodeHit.Node,
					Previous.Surface, NodeHit.Surface, Gap,
					*Previous.Position.ToString(), *NodeHit.Position.ToString()));
			}
			Previous = NodeHit;
		}
		for (int32 Offset : {-1, 0, 1})
		{
			const int32 PixelX = DarkestX + Offset;
			const FX6RayHit NodeHit = X6HitWorldNodes(Resource,
				Sample.Time, PixelX, Sample.Y);
			const FX6RayHit PayloadHit = X6HitPayload(Parts,
				Sample.Time, PixelX, Sample.Y);
			const FWoTRuntimeWorldSurface* Surface = Resource.Surfaces().IsValidIndex(NodeHit.Surface)
				? &Resource.Surfaces()[NodeHit.Surface] : nullptr;
			const FVector3f Normal = Surface && Resource.Vectors().IsValidIndex(Surface->Normal)
				? Resource.Vectors()[Surface->Normal] : FVector3f::ZeroVector;
			AddInfo(FString::Printf(TEXT("X6LineSample video_t=%.1f pixel=(%d,%d) node=%d surface=%d payload_part=%d triangle=%d payload_surface=%d texture=%s lightmap=%d normal=%s luminance=%.6f node_vertices=%s atlas=%s"),
				Sample.Time, PixelX, Sample.Y, NodeHit.Node, NodeHit.Surface,
				PayloadHit.Part, PayloadHit.Triangle, PayloadHit.Surface,
				Surface ? *Surface->TextureIdentity : TEXT("invalid"),
				Surface ? Surface->LightMap : INDEX_NONE,
				*Normal.ToString(), X6AtlasLuminance(Parts, PayloadHit),
				*X6NodeVertices(Resource, NodeHit.Node),
				*X6AtlasEvidence(Parts, PayloadHit)));
		}
		const FX6RayHit LeftNode = X6HitWorldNodes(Resource,
			Sample.Time, DarkestX - 1, Sample.Y);
		const FX6RayHit CenterNode = X6HitWorldNodes(Resource,
			Sample.Time, DarkestX, Sample.Y);
		const FX6RayHit RightNode = X6HitWorldNodes(Resource,
			Sample.Time, DarkestX + 1, Sample.Y);
		const double RibbonWidth = FMath::Min(
			X6NodeRibbonWidth(Resource, CenterNode.Node, LeftNode.Node),
			X6NodeRibbonWidth(Resource, CenterNode.Node, RightNode.Node));
		const double SourceLeft = X6AtlasLuminance(Parts,
			X6HitPayload(Parts, Sample.Time, DarkestX - 1, Sample.Y));
		const double SourceCenter = X6AtlasLuminance(Parts,
			X6HitPayload(Parts, Sample.Time, DarkestX, Sample.Y));
		const double SourceRight = X6AtlasLuminance(Parts,
			X6HitPayload(Parts, Sample.Time, DarkestX + 1, Sample.Y));
		const double FinalLeft = X6AtlasLuminance(Parts,
			X6HitPayload(FinalRender, Sample.Time, DarkestX - 1, Sample.Y));
		const double FinalCenter = X6AtlasLuminance(Parts,
			X6HitPayload(FinalRender, Sample.Time, DarkestX, Sample.Y));
		const double FinalRight = X6AtlasLuminance(Parts,
			X6HitPayload(FinalRender, Sample.Time, DarkestX + 1, Sample.Y));
		AddInfo(FString::Printf(TEXT("X6LineRibbon video_t=%.1f pixels=%d/%d/%d nodes=%d/%d/%d surfaces=%d/%d/%d width=%.9f source_luma=%.6f/%.6f/%.6f final_luma=%.6f/%.6f/%.6f"),
			Sample.Time, DarkestX - 1, DarkestX, DarkestX + 1,
			LeftNode.Node, CenterNode.Node, RightNode.Node,
			LeftNode.Surface, CenterNode.Surface, RightNode.Surface,
			RibbonWidth, SourceLeft, SourceCenter, SourceRight,
			FinalLeft, FinalCenter, FinalRight));
		TestTrue(FString::Printf(TEXT("X6 %.1f source witness is a sub-unit ribbon"), Sample.Time),
			RibbonWidth > 0.01 && RibbonWidth < 0.5);
		TestTrue(FString::Printf(TEXT("X6 %.1f source ribbon is the isolated dark line"), Sample.Time),
			SourceCenter * 4.0 < FMath::Min(SourceLeft, SourceRight));
		TestTrue(FString::Printf(TEXT("X6 %.1f final render does not preserve an isolated dark ribbon"), Sample.Time),
			FinalCenter * 2.0 >= FMath::Min(FinalLeft, FinalRight));
	}
	AddInfo(FString::Printf(TEXT("X6LineSummary largest_transition_gap=%.9f repair_tolerance=0.010000000 ribbon_surfaces=%llu ribbon_triangles=%llu ribbon_vertices=%llu ribbon_width=%.9f ribbon_triangle_visits=%llu ribbon_checks=%llu repair_edges=%llu inserted=%llu near_edges=%llu near_gap=%.9f bsp_ms=%.3f"),
		LargestGap, Repair.RepairedLightmapRibbonSurfaces,
		Repair.RepairedLightmapRibbonTriangles,
		Repair.RepairedLightmapRibbonVertices,
		Repair.MaxRepairedLightmapRibbonWidth,
		Repair.LightmapRibbonSourceTriangleVisits, Repair.LightmapRibbonChecks,
		Repair.RepairedEdges, Repair.InsertedEdgeVertices,
		Repair.RepairedNearCoincidentEdges,
		Repair.MaxRepairedNearCoincidentGap, BSPMilliseconds));
	TestTrue(TEXT("X6 wall polygons have exact shared edges rather than a gap"),
		LargestGap <= UE_DOUBLE_SMALL_NUMBER);
	TestTrue(TEXT("Battle2_02 production render repairs at least the X6 dark ribbon"),
		Repair.RepairedLightmapRibbonSurfaces >= 1);
	TestTrue(TEXT("Battle2_02 repaired ribbon stays inside the 0.5-unit safety bound"),
		Repair.MaxRepairedLightmapRibbonWidth <= 0.5);

	for (int32 X : {1025, 1075, 1125, 1175, 1225, 1275, 1322})
	{
		const int32 CenterY = FMath::RoundToInt(994.0
			+ double(X - 1025) * 112.0 / 297.0);
		for (int32 Offset = -2; Offset <= 2; ++Offset)
		{
			const int32 Y = CenterY + Offset;
			const FX6RayHit NodeHit = X6HitWorldNodes(Resource, 1.6, X, Y);
			const FX6RayHit SourceHit = X6HitPayload(Parts, 1.6, X, Y);
			const FX6RayHit FinalHit = X6HitPayload(FinalRender, 1.6, X, Y);
			AddInfo(FString::Printf(TEXT("X6CDiag pixel=(%d,%d) offset=%d node=%d surface=%d source_part=%d source_tri=%d source_surface=%d source_section=%d final_part=%d final_tri=%d node_pos=%s source_pos=%s final_pos=%s"),
				X, Y, Offset, NodeHit.Node, NodeHit.Surface,
				SourceHit.Part, SourceHit.Triangle, SourceHit.Surface,
				SourceHit.Section, FinalHit.Part, FinalHit.Triangle,
				*NodeHit.Position.ToString(), *SourceHit.Position.ToString(),
				*FinalHit.Position.ToString()));
		}
	}
	for (int32 Y : {1200, 1225, 1250, 1275, 1300, 1325})
	{
		const int32 CenterX = FMath::RoundToInt(1316.0
			- double(Y - 1200) * 15.0 / 125.0);
		for (int32 Offset = -2; Offset <= 2; ++Offset)
		{
			const int32 X = CenterX + Offset;
			const FX6RayHit NodeHit = X6HitWorldNodes(Resource, 1.6, X, Y);
			const FX6RayHit SourceHit = X6HitPayload(Parts, 1.6, X, Y);
			const FX6RayHit FinalHit = X6HitPayload(FinalRender, 1.6, X, Y);
			AddInfo(FString::Printf(TEXT("X6CVertical pixel=(%d,%d) offset=%d node=%d surface=%d source_part=%d source_tri=%d source_surface=%d source_section=%d final_part=%d final_tri=%d node_pos=%s source_pos=%s final_pos=%s"),
				X, Y, Offset, NodeHit.Node, NodeHit.Surface,
				SourceHit.Part, SourceHit.Triangle, SourceHit.Surface,
				SourceHit.Section, FinalHit.Part, FinalHit.Triangle,
				*NodeHit.Position.ToString(), *SourceHit.Position.ToString(),
				*FinalHit.Position.ToString()));
		}
		FX6RayHit PreviousNode;
		FX6RayHit PreviousSource;
		FX6RayHit PreviousFinal;
		for (int32 X = CenterX - 16; X <= CenterX + 4; ++X)
		{
			const FX6RayHit NodeHit = X6HitWorldNodes(Resource, 1.6, X, Y);
			const FX6RayHit SourceHit = X6HitPayload(Parts, 1.6, X, Y);
			const FX6RayHit FinalHit = X6HitPayload(FinalRender, 1.6, X, Y);
			if (PreviousNode.bHit && NodeHit.bHit
				&& (PreviousNode.Node != NodeHit.Node
					|| PreviousNode.Surface != NodeHit.Surface))
			{
				const FWoTRuntimeWorldSurface* LeftSurface =
					Resource.Surfaces().IsValidIndex(PreviousNode.Surface)
					? &Resource.Surfaces()[PreviousNode.Surface] : nullptr;
				const FWoTRuntimeWorldSurface* RightSurface =
					Resource.Surfaces().IsValidIndex(NodeHit.Surface)
					? &Resource.Surfaces()[NodeHit.Surface] : nullptr;
				const FVector3f LeftNormal = LeftSurface
					&& Resource.Vectors().IsValidIndex(LeftSurface->Normal)
					? Resource.Vectors()[LeftSurface->Normal] : FVector3f::ZeroVector;
				const FVector3f RightNormal = RightSurface
					&& Resource.Vectors().IsValidIndex(RightSurface->Normal)
					? Resource.Vectors()[RightSurface->Normal] : FVector3f::ZeroVector;
				AddInfo(FString::Printf(TEXT("X6CVerticalTransition y=%d pixels=%d/%d nodes=%d/%d surfaces=%d/%d edge_gap=%.9f normal_dot=%.9f source_luma=%.6f/%.6f final_luma=%.6f/%.6f"),
					Y, X - 1, X, PreviousNode.Node, NodeHit.Node,
					PreviousNode.Surface, NodeHit.Surface,
					X6NodeEdgeGap(Resource, PreviousNode.Node, NodeHit.Node),
					FVector3d::DotProduct(FVector3d(LeftNormal), FVector3d(RightNormal)),
					X6AtlasLuminance(Parts, PreviousSource),
					X6AtlasLuminance(Parts, SourceHit),
					X6AtlasLuminance(Parts, PreviousFinal),
					X6AtlasLuminance(Parts, FinalHit)));
			}
			PreviousNode = NodeHit;
			PreviousSource = SourceHit;
			PreviousFinal = FinalHit;
		}
	}
	const auto TriangleEvidence = [](const TArray<FWoTBSPPayloadData>& EvidenceParts,
		int32 PartIndex, int32 TriangleIndex)
	{
		if (!EvidenceParts.IsValidIndex(PartIndex)
			|| TriangleIndex < 0) return FString(TEXT("invalid"));
		const FWoTBSPPayloadData& Part = EvidenceParts[PartIndex];
		const int32 At = TriangleIndex * 3;
		if (At + 2 >= Part.Indices.Num()) return FString(TEXT("invalid"));
		return FString::Printf(TEXT("indices=%u/%u/%u positions=%s | %s | %s"),
			Part.Indices[At], Part.Indices[At + 1], Part.Indices[At + 2],
			*Part.Positions[Part.Indices[At]].ToString(),
			*Part.Positions[Part.Indices[At + 1]].ToString(),
			*Part.Positions[Part.Indices[At + 2]].ToString());
	};
	for (int32 Node : {14709, 14707, 14702, 14698, 14699})
	{
		if (!Resource.Nodes().IsValidIndex(Node)) continue;
		const FWoTRuntimeWorldNode& Item = Resource.Nodes()[Node];
		const FWoTRuntimeWorldSurface* Surface = Resource.Surfaces().IsValidIndex(Item.Surface)
			? &Resource.Surfaces()[Item.Surface] : nullptr;
		const FVector3f Normal = Surface && Resource.Vectors().IsValidIndex(Surface->Normal)
			? Resource.Vectors()[Surface->Normal] : FVector3f::ZeroVector;
		AddInfo(FString::Printf(TEXT("X6CNode node=%d surface=%d texture=%s lightmap=%d normal=%s vertices=%s"),
			Node, Item.Surface,
			Surface ? *Surface->TextureIdentity : TEXT("invalid"),
			Surface ? Surface->LightMap : INDEX_NONE, *Normal.ToString(),
			*X6NodeVertices(Resource, Node)));
	}
	for (const TPair<int32, int32>& Triangle : {
		TPair<int32, int32>(0, 3930), TPair<int32, int32>(0, 3925)})
	{
		AddInfo(FString::Printf(TEXT("X6CSourceTriangle part=%d triangle=%d %s"),
			Triangle.Key, Triangle.Value,
			*TriangleEvidence(Parts, Triangle.Key, Triangle.Value)));
	}
	for (const TPair<int32, int32>& Triangle : {
		TPair<int32, int32>(0, 4106), TPair<int32, int32>(0, 4100)})
	{
		AddInfo(FString::Printf(TEXT("X6CFinalTriangle part=%d triangle=%d %s"),
			Triangle.Key, Triangle.Value,
			*TriangleEvidence(FinalRender, Triangle.Key, Triangle.Value)));
	}
	if (Parts.IsValidIndex(0) && Parts[0].Sections.IsValidIndex(21))
	{
		const FWoTBSPPayloadSection& Section = Parts[0].Sections[21];
		AddInfo(FString::Printf(TEXT("X6CSection part=0 section=21 slot=%s texture=%s first=%u count=%u min=%u max=%u"),
			*Section.MaterialSlot, *Section.SourceTextureIdentity,
			Section.FirstIndex, Section.IndexCount, Section.MinVertex, Section.MaxVertex));
	}
	const FWoTBSPPayloadData& X6Part = Parts[0];
	const int32 X6UpperAt = 3930 * 3;
	const int32 X6LowerAt = 3925 * 3;
	const FVector3f X6Anchor = X6Part.Positions[X6Part.Indices[X6UpperAt + 2]];
	const FVector3f X6UpperEnd = X6Part.Positions[X6Part.Indices[X6UpperAt]];
	const FVector3f X6LowerEnd = X6Part.Positions[X6Part.Indices[X6LowerAt + 1]];
	const double X6EndpointGap = FVector3d(X6UpperEnd - X6LowerEnd).Size();
	const double X6LowerToUpperEdge = WoTX6Test::PointSegmentDistance(
		X6LowerEnd, X6Anchor, X6UpperEnd);
	const uint32 X6SourceUpperIncidence = WoTX6Test::EdgeIncidence(
		Parts, X6Anchor, X6UpperEnd);
	const uint32 X6SourceLowerIncidence = WoTX6Test::EdgeIncidence(
		Parts, X6Anchor, X6LowerEnd);
	const uint32 X6FinalLowerIncidence = WoTX6Test::EdgeIncidence(
		FinalRender, X6Anchor, X6LowerEnd);
	AddInfo(FString::Printf(TEXT("X6CDiagonalSummary anchor=%s upper_end=%s lower_end=%s endpoint_gap=%.9f lower_to_upper_edge=%.9f source_incidence=%u/%u final_shared_lower_incidence=%u"),
		*X6Anchor.ToString(), *X6UpperEnd.ToString(), *X6LowerEnd.ToString(),
		X6EndpointGap, X6LowerToUpperEdge, X6SourceUpperIncidence,
		X6SourceLowerIncidence, X6FinalLowerIncidence));
	TestTrue(TEXT("X6 diagonal source is the pinned 0.326-unit forked boundary"),
		FMath::IsNearlyEqual(X6EndpointGap, 0.326177722, 0.0001)
		&& X6SourceUpperIncidence == 1 && X6SourceLowerIncidence == 1);
	TestTrue(TEXT("X6 diagonal final render shares the lower boundary edge exactly"),
		X6FinalLowerIncidence >= 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPA2ArchWatertightSpec,
	"WoT.World.RuntimeBSP.A2ArchWatertight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPA2ArchWatertightSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{ AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	auto Loaded = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("authenticated retail contract loads"), Loaded.IsError())) return false;
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : Loaded.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World && Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase)) continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		auto& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(TEXT("authenticated package bytes load"), FFileHelper::LoadFileToArray(
			Source.Bytes, *(WoTReferencePath(TEXT("original")) / Fact.SourcePath)))) return false;
	}
	FTJunctionRetailWriter Writer;
	FTJunctionRetailProgress Progress;
	auto Imported = ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("authenticated production world set imports"), Imported.IsError()))
	{ AddError(Imported.GetError().ToString()); return false; }
	const FWoTWorldDescriptor* Descriptor = Imported.GetValue().Worlds.FindByPredicate(
		[](const FWoTWorldDescriptor& Item) { return Item.MapIdentity == TEXT("Arena_02"); });
	if (!TestNotNull(TEXT("Arena_02 authenticated descriptor"), Descriptor)) return false;
	TArray<FWoTBSPPayloadData> Parts;
	FString Error;
	for (const FWoTBSPPartDescriptor& Part : Descriptor->BSPParts)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(Part.LogicalId);
		if (!TestNotNull(TEXT("Arena_02 authenticated BSP object"), Bytes)) return false;
		auto Decoded = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			MoveTemp(*Bytes), TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
		if (!TestFalse(TEXT("Arena_02 BSP object frame decodes"), Decoded.IsError())
			|| !TestTrue(TEXT("Arena_02 BSP payload decodes"),
				UWoTBSPPayloadLibrary::DecodePayload(Decoded.GetValue().Payload(),
					Parts.AddDefaulted_GetRef(), Error))) return false;
	}
	TArray<uint8>* WorldBytes = Writer.Objects.Find(TEXT("cache/world/Arena_02"));
	if (!TestNotNull(TEXT("Arena_02 authenticated world object"), WorldBytes)) return false;
	auto DecodedWorld = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
		MoveTemp(*WorldBytes), TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!TestFalse(TEXT("Arena_02 world object frame decodes"), DecodedWorld.IsError())) return false;
	FWoTRuntimeWorldResource Resource;
	const FWoTBootstrapStatus WorldLoaded = Resource.LoadObjectForTesting(
		MoveTemp(DecodedWorld).GetValue());
	if (!TestFalse(TEXT("Arena_02 world records decode"), WorldLoaded.IsError()))
	{ AddError(WorldLoaded.GetError().ToString()); return false; }

	WoTTest::FSpecWorldOptions Options;
	Options.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTRuntimeBSPWorld* Runtime = nullptr;
	uint32 FallbackLightmaps = 0;
	FWoTBSPTJunctionStats Repair;
	double BSPMilliseconds = 0;
	if (!TestTrue(TEXT("Arena_02 production world render build"),
		WoTWorldBuildRuntimeBSPForTesting(*World, *Descriptor, Parts, Runtime,
			FallbackLightmaps, Error, {}, nullptr, nullptr, nullptr, nullptr,
			EWoTRuntimeSkyStageFault::None, nullptr, {}, nullptr, &Repair,
			&BSPMilliseconds)))
	{ AddError(Error); return false; }
	TArray<FWoTBSPPayloadData> FinalRender;
	for (UStaticMeshComponent* Component : Runtime->GeometryComponents)
	{
		if (!Component || !Component->IsVisible()) continue;
		UStaticMesh* BuiltMesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* BuiltRender = BuiltMesh
			? BuiltMesh->GetRenderData() : nullptr;
		TestTrue(TEXT("Arena_02 shipped BSP uses full-precision lightmap UVs"),
			BuiltRender && BuiltRender->LODResources.IsValidIndex(0)
			&& BuiltRender->LODResources[0].VertexBuffers.StaticMeshVertexBuffer
				.GetUseFullPrecisionUVs());
		if (!TestTrue(TEXT("Arena_02 built visible mesh has CPU render buffers"),
			ReadTJunctionRenderMesh(BuiltMesh,
				FinalRender.AddDefaulted_GetRef()))) return false;
	}

	// A5 reverse-angle scout v3, frame 155. The dynamic-on/off image pair
	// locates the missing-light ribbon at this transition: the light gain falls
	// from 40-70 RGB levels beside it to zero on it. Keep the real payload edge
	// and both independently-authored lightmap charts pinned here.
	constexpr double A5RVideoSeconds = 155.0 / 60.0;
	const FX6RayHit A5RUpper = A5RV3HitPayload(
		Parts, A5RVideoSeconds, 400, 437);
	const FX6RayHit A5RRibbon = A5RV3HitPayload(
		Parts, A5RVideoSeconds, 400, 438);
	const FX6RayHit A5RLower = A5RV3HitPayload(
		Parts, A5RVideoSeconds, 400, 442);
	TestTrue(TEXT("A5 V3 upper side is grass surface 1787 triangle 753"),
		A5RUpper.bHit && A5RUpper.Part == 0 && A5RUpper.Triangle == 753
		&& A5RUpper.Surface == 1787 && A5RUpper.Section == 5);
	TestTrue(TEXT("A5 V3 ribbon is grass surface 1790 skinny triangle 807"),
		A5RRibbon.bHit && A5RRibbon.Part == 0 && A5RRibbon.Triangle == 807
		&& A5RRibbon.Surface == 1790 && A5RRibbon.Section == 5);
	TestTrue(TEXT("A5 V3 lower side remains surface 1790"),
		A5RLower.bHit && A5RLower.Part == 0 && A5RLower.Surface == 1790
		&& A5RLower.Section == 5);
	const FWoTBSPPayloadData& A5RPart = Parts[0];
	TestTrue(TEXT("A5 V3 grass material slot is authenticated"),
		A5RPart.Sections.IsValidIndex(5)
		&& A5RPart.Sections[5].MaterialSlot == TEXT("NatureT__GrssGrss1150")
		&& A5RPart.Sections[5].SourceTextureIdentity
			== TEXT("NatureT.Floor.GrssGrss1150"));
	const int32 UpperAt = 753 * 3;
	const int32 RibbonAt = 807 * 3;
	const FVector3f A5REdgeA(-911.5f, 192.0f, -323.804f);
	const FVector3f A5REdgeB(-911.5f, -135.529f, -312.5f);
	TestTrue(TEXT("A5 V3 upper triangle owns the measured shared edge"),
		A5RPart.Positions[A5RPart.Indices[UpperAt + 1]].Equals(A5REdgeA, 0.001f)
		&& A5RPart.Positions[A5RPart.Indices[UpperAt + 2]].Equals(A5REdgeB, 0.001f));
	TestTrue(TEXT("A5 V3 skinny triangle owns the measured shared edge"),
		A5RPart.Positions[A5RPart.Indices[RibbonAt]].Equals(A5REdgeA, 0.001f)
		&& A5RPart.Positions[A5RPart.Indices[RibbonAt + 2]].Equals(A5REdgeB, 0.001f));
	TestTrue(TEXT("A5 V3 edge normals are finite unit vectors"),
		A5RUpper.Normal.IsUnit(0.001) && A5RRibbon.Normal.IsUnit(0.001));
	const FWoTRuntimeWorldSurface& UpperSurface = Resource.Surfaces()[1787];
	const FWoTRuntimeWorldSurface& RibbonSurface = Resource.Surfaces()[1790];
	TestEqual(TEXT("A5 V3 upper chart lightmap"), UpperSurface.LightMap, 417);
	TestEqual(TEXT("A5 V3 ribbon chart lightmap"), RibbonSurface.LightMap, 406);
	const FWoTBSPPayloadData* AtlasCarrier = nullptr;
	for (const FWoTBSPPayloadData& Candidate : Parts)
	{
		if (Candidate.LightmapWidth > 0) AtlasCarrier = &Candidate;
	}
	TestTrue(TEXT("A5 V3 atlas is authenticated"),
		AtlasCarrier && AtlasCarrier->LightmapWidth == 1024
		&& AtlasCarrier->LightmapHeight == 1024);
	FVector2d UpperAtlasMin(TNumericLimits<double>::Max());
	FVector2d UpperAtlasMax(TNumericLimits<double>::Lowest());
	FVector2d RibbonAtlasMin(TNumericLimits<double>::Max());
	FVector2d RibbonAtlasMax(TNumericLimits<double>::Lowest());
	for (int32 Triangle = 0; Triangle < A5RPart.TriangleSurfaces.Num(); ++Triangle)
	{
		const uint32 Surface = A5RPart.TriangleSurfaces[Triangle];
		if (Surface != 1787 && Surface != 1790) continue;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector2d Q = FVector2d(A5RPart.UV1[
				A5RPart.Indices[Triangle * 3 + Corner]]) * 1024.0 - 0.5;
			FVector2d& Minimum = Surface == 1787 ? UpperAtlasMin : RibbonAtlasMin;
			FVector2d& Maximum = Surface == 1787 ? UpperAtlasMax : RibbonAtlasMax;
			Minimum.X = FMath::Min(Minimum.X, Q.X);
			Minimum.Y = FMath::Min(Minimum.Y, Q.Y);
			Maximum.X = FMath::Max(Maximum.X, Q.X);
			Maximum.Y = FMath::Max(Maximum.Y, Q.Y);
		}
	}
	TestTrue(TEXT("A5 V3 surfaces occupy distinct finite atlas charts"),
		UpperAtlasMin.X < UpperAtlasMax.X && UpperAtlasMin.Y < UpperAtlasMax.Y
		&& RibbonAtlasMin.X < RibbonAtlasMax.X
		&& RibbonAtlasMin.Y < RibbonAtlasMax.Y
		&& UpperAtlasMin.X > RibbonAtlasMax.X);
	AddInfo(FString::Printf(TEXT("A5RV3Edge surfaces=1787/1790 triangles=753/807 edge=%s|%s atlas=1024x1024 chart_q_bounds=%s..%s/%s..%s upper_uv0=%s upper_uv1=%s ribbon_uv0=%s ribbon_uv1=%s normals=%s/%s"),
		*A5REdgeA.ToString(), *A5REdgeB.ToString(), *UpperAtlasMin.ToString(),
		*UpperAtlasMax.ToString(), *RibbonAtlasMin.ToString(),
		*RibbonAtlasMax.ToString(), *A5RUpper.UV0.ToString(),
		*A5RUpper.UV1.ToString(), *A5RRibbon.UV0.ToString(),
		*A5RRibbon.UV1.ToString(), *A5RUpper.Normal.ToString(),
		*A5RRibbon.Normal.ToString()));

	const FX6CameraPose Pose = A2CameraAtVideoTime(2.20);
	AddInfo(FString::Printf(TEXT("A2Camera video_t=2.20 timeline_t=2.95 location=%s rotation=%s fov=%.9f bsp_ms=%.3f fan_pairs=%llu repair_edges=%llu inserted=%llu near_edges=%llu fallback=%llu"),
		*Pose.Location.ToString(), *Pose.Rotation.ToString(), A2FOVAtVideoTime(2.20),
		BSPMilliseconds, Repair.RetessellatedFanSliverPairs,
		Repair.RepairedEdges, Repair.InsertedEdgeVertices,
		Repair.RepairedNearCoincidentEdges, Repair.FallbackTriangles));
	for (int32 At = 0; At + 2 < Repair.NearCollapsedEdgeWitnesses.Num(); At += 3)
		AddInfo(FString::Printf(TEXT("A2NearCollapsed edge=%s -> %s source_corner=%s"),
			*Repair.NearCollapsedEdgeWitnesses[At].ToString(),
			*Repair.NearCollapsedEdgeWitnesses[At + 1].ToString(),
			*Repair.NearCollapsedEdgeWitnesses[At + 2].ToString()));

	struct FA2Sliver
	{
		const TCHAR* Name;
		FVector2d Start;
		FVector2d End;
	};
	// Independently measured from the BT.601-decoded 3840x2160 frame at t=2.20.
	const FA2Sliver Slivers[] = {
		{TEXT("upper_left_steep"), {1261, 637}, {1135, 1039}},
		{TEXT("upper_left_diagonal"), {1659, 540}, {1171, 891}},
		{TEXT("top_right_long"), {2185, 689}, {2677, 749}},
		{TEXT("right_vertical"), {2731, 468}, {2714, 743}},
		{TEXT("lower_right_vertical"), {2531, 1275}, {2567, 1910}}};
	struct FA2FanWitness { const TCHAR* Name; int32 X; int32 Y; };
	const FA2FanWitness FanWitnesses[] = {
		{TEXT("upper_left_steep"), 1198, 838},
		{TEXT("upper_left_diagonal"), 1614, 580},
		{TEXT("top_right_long"), 2431, 719},
		{TEXT("right_vertical"), 2724, 606},
		{TEXT("lower_right_vertical"), 2549, 1593}};
	int32 UnretessellatedFanSlivers = 0;
	for (const FA2FanWitness& Witness : FanWitnesses)
	{
		const FX6RayHit SourceHit = A2HitPayload(Parts, 2.20, Witness.X, Witness.Y);
		const FX6RayHit FinalHit = A2HitPayload(FinalRender, 2.20, Witness.X, Witness.Y);
		bool bSameTriangle = SourceHit.bHit && FinalHit.bHit
			&& Parts.IsValidIndex(SourceHit.Part)
			&& FinalRender.IsValidIndex(FinalHit.Part);
		if (bSameTriangle)
		{
			const FWoTBSPPayloadData& SourcePart = Parts[SourceHit.Part];
			const FWoTBSPPayloadData& FinalPart = FinalRender[FinalHit.Part];
			const int32 SourceAt = SourceHit.Triangle * 3;
			const int32 FinalAt = FinalHit.Triangle * 3;
			for (int32 SourceCorner = 0; SourceCorner < 3 && bSameTriangle; ++SourceCorner)
			{
				const FVector3f Position = SourcePart.Positions[
					SourcePart.Indices[SourceAt + SourceCorner]];
				bool bFound = false;
				for (int32 FinalCorner = 0; FinalCorner < 3; ++FinalCorner)
					bFound |= Position == FinalPart.Positions[
						FinalPart.Indices[FinalAt + FinalCorner]];
				bSameTriangle &= bFound;
			}
		}
		UnretessellatedFanSlivers += bSameTriangle ? 1 : 0;
		FString SourceCorners = TEXT("none");
		if (SourceHit.bHit && Parts.IsValidIndex(SourceHit.Part))
		{
			const FWoTBSPPayloadData& SourcePart = Parts[SourceHit.Part];
			const int32 SourceAt = SourceHit.Triangle * 3;
			SourceCorners = FString::Printf(TEXT("%s|%s|%s"),
				*SourcePart.Positions[SourcePart.Indices[SourceAt]].ToString(),
				*SourcePart.Positions[SourcePart.Indices[SourceAt + 1]].ToString(),
				*SourcePart.Positions[SourcePart.Indices[SourceAt + 2]].ToString());
		}
		FString FinalCorners = TEXT("none");
		if (FinalHit.bHit && FinalRender.IsValidIndex(FinalHit.Part))
		{
			const FWoTBSPPayloadData& FinalPart = FinalRender[FinalHit.Part];
			const int32 FinalAt = FinalHit.Triangle * 3;
			FinalCorners = FString::Printf(TEXT("%s|%s|%s"),
				*FinalPart.Positions[FinalPart.Indices[FinalAt]].ToString(),
				*FinalPart.Positions[FinalPart.Indices[FinalAt + 1]].ToString(),
				*FinalPart.Positions[FinalPart.Indices[FinalAt + 2]].ToString());
		}
		AddInfo(FString::Printf(TEXT("A2FanWitness class=%s pixel=(%d,%d) source_part=%d source_triangle=%d final_part=%d final_triangle=%d same_triangle=%d source_distance=%.9f final_distance=%.9f source_corners=%s final_corners=%s"),
			Witness.Name, Witness.X, Witness.Y, SourceHit.Part, SourceHit.Triangle,
			FinalHit.Part, FinalHit.Triangle, bSameTriangle,
			SourceHit.Distance, FinalHit.Distance, *SourceCorners, *FinalCorners));
	}
	TSet<int32> SeenNodes;
	TSet<uint64> SeenSourceTriangles;
	int32 ForegroundMisses = 0;
	int32 StrongLuminanceTroughs = 0;
	int32 SampleCount = 0;
	for (const FA2Sliver& Sliver : Slivers)
	{
		const FVector2d Delta = Sliver.End - Sliver.Start;
		const FVector2d Perpendicular = FVector2d(-Delta.Y, Delta.X).GetSafeNormal();
		for (double Along : {0.1, 0.3, 0.5, 0.7, 0.9})
		{
			const FVector2d Center = FMath::Lerp(Sliver.Start, Sliver.End, Along);
			FX6RayHit SourceOffsets[13];
			FX6RayHit FinalOffsets[13];
			for (int32 Offset = -6; Offset <= 6; ++Offset)
			{
				const FVector2d Pixel = Center + Perpendicular * Offset;
				const int32 X = FMath::RoundToInt(Pixel.X);
				const int32 Y = FMath::RoundToInt(Pixel.Y);
				const FX6RayHit NodeHit = A2HitWorldNodes(Resource, 2.20, X, Y);
				const FX6RayHit SourceHit = A2HitPayload(Parts, 2.20, X, Y);
				const FX6RayHit FinalHit = A2HitPayload(FinalRender, 2.20, X, Y);
				SourceOffsets[Offset + 6] = SourceHit;
				FinalOffsets[Offset + 6] = FinalHit;
				if (NodeHit.bHit) SeenNodes.Add(NodeHit.Node);
				if (SourceHit.bHit)
					SeenSourceTriangles.Add((uint64(uint32(SourceHit.Part)) << 32)
						| uint32(SourceHit.Triangle));
				AddInfo(FString::Printf(TEXT("A2Ray class=%s along=%.1f offset=%d pixel=(%d,%d) node=%d node_surface=%d node_distance=%.6f source_part=%d source_tri=%d source_surface=%d source_section=%d source_distance=%.6f source_luminance=%.6f source_uv1=%s source_normal=%s final_part=%d final_tri=%d final_distance=%.6f final_luminance=%.6f final_uv1=%s final_normal=%s node_pos=%s source_pos=%s final_pos=%s"),
					Sliver.Name, Along, Offset, X, Y, NodeHit.Node, NodeHit.Surface,
					NodeHit.Distance, SourceHit.Part, SourceHit.Triangle,
					SourceHit.Surface, SourceHit.Section, SourceHit.Distance,
					X6AtlasLuminance(Parts, SourceHit), *SourceHit.UV1.ToString(),
					*SourceHit.Normal.ToString(),
					FinalHit.Part, FinalHit.Triangle, FinalHit.Distance,
					X6AtlasLuminance(Parts, FinalHit), *FinalHit.UV1.ToString(),
					*FinalHit.Normal.ToString(),
					*NodeHit.Position.ToString(), *SourceHit.Position.ToString(),
					*FinalHit.Position.ToString()));
			}
			double MinimumLuminance = TNumericLimits<double>::Max();
			double MaximumLuminance = 0;
			for (const FX6RayHit& Hit : FinalOffsets)
			{
				const double Luminance = X6AtlasLuminance(Parts, Hit);
				if (Luminance < 0) continue;
				MinimumLuminance = FMath::Min(MinimumLuminance, Luminance);
				MaximumLuminance = FMath::Max(MaximumLuminance, Luminance);
			}
			const bool bStrongLuminanceTrough = MaximumLuminance >= 16.0
				&& MinimumLuminance * 4.0 < MaximumLuminance;
			if (bStrongLuminanceTrough) ++StrongLuminanceTroughs;
			const double NeighbourDistance = FMath::Min(
				FinalOffsets[0].Distance, FinalOffsets[12].Distance);
			bool bMiss = false;
			for (int32 Offset = 4; Offset <= 8; ++Offset)
			{
				const FX6RayHit& Hit = FinalOffsets[Offset];
				bMiss |= !Hit.bHit || (NeighbourDistance < TNumericLimits<double>::Max()
					&& Hit.Distance > NeighbourDistance + 10.0);
			}
			if (bMiss) ++ForegroundMisses;
			++SampleCount;
			AddInfo(FString::Printf(TEXT("A2RaySummary class=%s along=%.1f neighbour_distance=%.6f center_distance=%.6f foreground_miss=%d minimum_luminance=%.6f maximum_luminance=%.6f strong_luminance_trough=%d source_stack=%s final_stack=%s"),
				Sliver.Name, Along, NeighbourDistance, FinalOffsets[6].Distance, bMiss,
				MinimumLuminance, MaximumLuminance, bStrongLuminanceTrough,
				*A2HitStack(Parts, 2.20, FMath::RoundToInt(Center.X),
					FMath::RoundToInt(Center.Y)),
				*A2HitStack(FinalRender, 2.20, FMath::RoundToInt(Center.X),
					FMath::RoundToInt(Center.Y))));
		}
	}
	TArray<int32> SortedNodes = SeenNodes.Array();
	SortedNodes.Sort();
	for (int32 NodeIndex : SortedNodes)
	{
		if (!Resource.Nodes().IsValidIndex(NodeIndex)) continue;
		const FWoTRuntimeWorldNode& Node = Resource.Nodes()[NodeIndex];
		const FWoTRuntimeWorldSurface* Surface = Resource.Surfaces().IsValidIndex(Node.Surface)
			? &Resource.Surfaces()[Node.Surface] : nullptr;
		const FVector3f Normal = Surface && Resource.Vectors().IsValidIndex(Surface->Normal)
			? Resource.Vectors()[Surface->Normal] : FVector3f::ZeroVector;
		AddInfo(FString::Printf(TEXT("A2Node node=%d surface=%d vertex_count=%u texture=%s lightmap=%d flags=%u normal=%s vertices=%s"),
			NodeIndex, Node.Surface, Node.VertexCount,
			Surface ? *Surface->TextureIdentity : TEXT("invalid"),
			Surface ? Surface->LightMap : INDEX_NONE,
			Surface ? Surface->Flags : 0, *Normal.ToString(),
			*X6NodeVertices(Resource, NodeIndex)));
	}
	TArray<uint64> SortedSourceTriangles = SeenSourceTriangles.Array();
	SortedSourceTriangles.Sort();
	TSet<FVector3f> A2UniquePointSet;
	for (const FWoTBSPPayloadData& CandidatePart : Parts)
		for (uint32 Index : CandidatePart.Indices)
			A2UniquePointSet.Add(CandidatePart.Positions[Index]);
	const TArray<FVector3f> A2UniquePoints = A2UniquePointSet.Array();
	for (uint64 Key : SortedSourceTriangles)
	{
		const int32 PartIndex = int32(Key >> 32);
		const int32 TriangleIndex = int32(uint32(Key));
		if (!Parts.IsValidIndex(PartIndex)) continue;
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		const int32 At = TriangleIndex * 3;
		if (At + 2 >= Part.Indices.Num()) continue;
		const FVector3f A = Part.Positions[Part.Indices[At]];
		const FVector3f B = Part.Positions[Part.Indices[At + 1]];
		const FVector3f C = Part.Positions[Part.Indices[At + 2]];
		const FVector3d Cross = FVector3d::CrossProduct(
			FVector3d(B) - FVector3d(A), FVector3d(C) - FVector3d(A));
		const FVector3f VertexNormal = Part.Normals[Part.Indices[At]];
		int32 SectionIndex = INDEX_NONE;
		for (int32 Candidate = 0; Candidate < Part.Sections.Num(); ++Candidate)
			if (At >= int32(Part.Sections[Candidate].FirstIndex)
				&& At < int32(Part.Sections[Candidate].FirstIndex
					+ Part.Sections[Candidate].IndexCount))
			{ SectionIndex = Candidate; break; }
		AddInfo(FString::Printf(TEXT("A2SourceTriangle part=%d triangle=%d surface=%u section=%d positions=%s | %s | %s normals=%s | %s | %s uv0=(%.9f,%.9f)|(%.9f,%.9f)|(%.9f,%.9f) uv1=(%.9f,%.9f)|(%.9f,%.9f)|(%.9f,%.9f) edge_lengths=%.9f/%.9f/%.9f facing_dot=%.9f"),
			PartIndex, TriangleIndex,
			Part.TriangleSurfaces.IsValidIndex(TriangleIndex)
				? Part.TriangleSurfaces[TriangleIndex] : MAX_uint32,
			SectionIndex,
			*A.ToString(), *B.ToString(), *C.ToString(),
			*Part.Normals[Part.Indices[At]].ToString(),
			*Part.Normals[Part.Indices[At + 1]].ToString(),
			*Part.Normals[Part.Indices[At + 2]].ToString(),
			Part.UV0[Part.Indices[At]].X, Part.UV0[Part.Indices[At]].Y,
			Part.UV0[Part.Indices[At + 1]].X, Part.UV0[Part.Indices[At + 1]].Y,
			Part.UV0[Part.Indices[At + 2]].X, Part.UV0[Part.Indices[At + 2]].Y,
			Part.UV1[Part.Indices[At]].X, Part.UV1[Part.Indices[At]].Y,
			Part.UV1[Part.Indices[At + 1]].X, Part.UV1[Part.Indices[At + 1]].Y,
			Part.UV1[Part.Indices[At + 2]].X, Part.UV1[Part.Indices[At + 2]].Y,
			FVector3d(B - A).Size(), FVector3d(C - B).Size(),
			FVector3d(A - C).Size(),
			FVector3d::DotProduct(Cross.GetSafeNormal(), FVector3d(VertexNormal))));
		const FVector3f Corners[] = {A, B, C};
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const FVector3d EdgeA(Corners[Edge]);
			const FVector3d EdgeB(Corners[(Edge + 1) % 3]);
			const FVector3d Delta = EdgeB - EdgeA;
			const double LengthSquared = Delta.SizeSquared();
			if (LengthSquared == 0) continue;
			uint32 Incidence = 0;
			FString IncidentTriangles;
			for (int32 IncidencePartIndex = 0;
				IncidencePartIndex < Parts.Num(); ++IncidencePartIndex)
			{
				const FWoTBSPPayloadData& IncidencePart = Parts[IncidencePartIndex];
				for (int32 IncidenceAt = 0; IncidenceAt < IncidencePart.Indices.Num();
					IncidenceAt += 3)
				{
					const FVector3f IncidenceCorners[] = {
						IncidencePart.Positions[IncidencePart.Indices[IncidenceAt]],
						IncidencePart.Positions[IncidencePart.Indices[IncidenceAt + 1]],
						IncidencePart.Positions[IncidencePart.Indices[IncidenceAt + 2]]};
					for (int32 CandidateEdge = 0; CandidateEdge < 3; ++CandidateEdge)
					{
						const FVector3f& X = IncidenceCorners[CandidateEdge];
						const FVector3f& Y = IncidenceCorners[(CandidateEdge + 1) % 3];
						if ((X == Corners[Edge] && Y == Corners[(Edge + 1) % 3])
							|| (Y == Corners[Edge] && X == Corners[(Edge + 1) % 3]))
						{
							++Incidence;
							if (!IncidentTriangles.IsEmpty()) IncidentTriangles += TEXT("|");
							IncidentTriangles += FString::Printf(TEXT("p%d:t%d:s%u"),
								IncidencePartIndex, IncidenceAt / 3,
								IncidencePart.TriangleSurfaces.IsValidIndex(IncidenceAt / 3)
									? IncidencePart.TriangleSurfaces[IncidenceAt / 3]
									: MAX_uint32);
						}
					}
				}
			}
			FString Donors;
			for (const FVector3f& CandidatePoint : A2UniquePoints)
			{
				if (CandidatePoint == Corners[Edge]
					|| CandidatePoint == Corners[(Edge + 1) % 3]) continue;
				const FVector3d Point(CandidatePoint);
				const double Parameter = FVector3d::DotProduct(Point - EdgeA, Delta)
					/ LengthSquared;
				if (Parameter <= 0 || Parameter >= 1) continue;
				const double Distance = (Point - (EdgeA + Parameter * Delta)).Size();
				if (Distance > 0.5) continue;
				if (!Donors.IsEmpty()) Donors += TEXT(" | ");
				Donors += FString::Printf(TEXT("%s:t%.9f:d%.9f"),
					*CandidatePoint.ToString(), Parameter, Distance);
			}
			AddInfo(FString::Printf(TEXT("A2SourceEdge part=%d triangle=%d edge=%d a=%s b=%s length=%.9f incidence=%u incident_triangles=%s donors=%s"),
				PartIndex, TriangleIndex, Edge, *EdgeA.ToString(), *EdgeB.ToString(),
				FMath::Sqrt(LengthSquared), Incidence, *IncidentTriangles,
				Donors.IsEmpty() ? TEXT("none") : *Donors));
		}
	}
	AddInfo(FString::Printf(TEXT("A2WatertightSummary classes=%d samples=%d foreground_misses=%d strong_luminance_troughs=%d unretessellated_fan_slivers=%d nodes=%d source_triangles=%d"),
		UE_ARRAY_COUNT(Slivers), SampleCount, ForegroundMisses, StrongLuminanceTroughs,
		UnretessellatedFanSlivers,
		SeenNodes.Num(), SeenSourceTriangles.Num()));
	TestEqual(TEXT("A2 arch final render has no foreground misses across measured slivers"),
		ForegroundMisses, 0);
	TestEqual(TEXT("A2 arch final render has no strong black lightmap troughs across measured slivers"),
		StrongLuminanceTroughs, 0);
	TestEqual(TEXT("A2 arch final render retessellates each measured fan sliver"),
		UnretessellatedFanSlivers, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPTJunctionAuditSpec,
	"WoT.World.RuntimeBSP.TJunctionAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPTJunctionAuditSpec::RunTest(const FString&)
{
	TArray<FWoTBSPPayloadData> Parts;
	Parts.SetNum(2);
	const FVector3f A(0, 0, 0), B(100, 0, 0), C(0, 100, 0);
	const FVector3f P(50, 0, 0.009f);
	AddTJunctionTriangle(Parts[0], A, B, C, 1, true);
	AddTJunctionDonor(Parts[1], P, 1, true);
	TArray<FString> Examples;
	TestEqual(TEXT("external unmatched point is a junction even with matching surface identity"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(1));
	AddTJunctionTriangle(Parts[0], B, A, FVector3f(0, -100, 0), 1, false);
	TestEqual(TEXT("two nonzero triangles sharing A-B form a matched edge despite an external near-edge point"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(0));
	Parts[0].Indices.SetNum(3);
	AddTJunctionTriangle(Parts[0], A, B, P, 1, false);
	TestEqual(TEXT("nonzero sibling triangle fills the sliver along its shared edge"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(0));
	Parts[0].Indices.SetNum(3);
	AddTJunctionTriangle(Parts[0], A, B, FVector3f(50, 0, 0), 1, false);
	TestTrue(TEXT("a zero-area sibling does not fill the sliver"),
		AuditTJunctionRenderParts(Parts, Examples) > 0);
	Parts.SetNum(1);
	Parts[0] = FWoTBSPPayloadData();
	AddTJunctionTriangle(Parts[0], A, B, FVector3f(50, 0, 0.009f), 1, true);
	TestEqual(TEXT("a near-degenerate triangle's third corner is already represented"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(0));
	Parts.SetNum(2);
	Parts[0] = FWoTBSPPayloadData();
	AddTJunctionTriangle(Parts[0], A, B, FVector3f(200, 0, 0), 1, true);
	AddTJunctionDonor(Parts[1], P, 1, true);
	TestEqual(TEXT("an exact-zero-area-only host cannot have an unmatched render edge"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(0));
	AddTJunctionTriangle(Parts[0], A, B, C, 1, false);
	TestEqual(TEXT("zero-area host sharing a singly incident nonzero edge is not audited twice"),
		AuditTJunctionRenderParts(Parts, Examples), uint64(1));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPMoverTJunctionSpec,
	"WoT.World.RuntimeBSP.MoverTJunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPMoverTJunctionSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{ AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	auto TextureContract = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("default texture retail contract loads"), TextureContract.IsError())) return false;
	TextureContract.GetValue().Facts.RemoveAll([](const FWoTRequiredFact& Fact)
	{ return Fact.Domain != EWoTFactDomain::Texture || Fact.SourceExport != TEXT("Engine.DefaultTexture"); });
	if (!TestEqual(TEXT("one default texture fact"), TextureContract.GetValue().Facts.Num(), 1)) return false;
	const FString TextureSource = TextureContract.GetValue().Facts[0].SourcePath;
	TArray<uint8> TextureBytes;
	if (!TestTrue(TEXT("default texture package loads"), FFileHelper::LoadFileToArray(TextureBytes,
		*(WoTReferencePath(TEXT("original")) / TextureSource)))) return false;
	FTJunctionRetailWriter TextureWriter;
	FTJunctionRetailProgress TextureProgress;
	auto TextureImport = ImportTexturePackageBytesForTesting(TextureBytes, TextureSource,
		TextureContract.GetValue(), TextureWriter, TextureProgress);
	if (!TestFalse(TEXT("default texture imports"), TextureImport.IsError())) return false;
	const FString CacheRoot = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()
		/ (TEXT("MoverTJunction_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)) / TEXT("ContentCache"));
	FString ActivationError;
	if (!TestTrue(TEXT("default texture generation publishes"), WoTPayloadActivationTest_PublishObjects(
		CacheRoot, TextureWriter.TextureObjects, ActivationError))) { AddError(ActivationError); return false; }
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	ON_SCOPE_EXIT
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(CacheRoot), false, true);
	};
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bActivatePayload = false;
	Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* Instance)
	{ CastChecked<UWoTGameInstance>(Instance)->SetPayloadCacheDir(CacheRoot, false); };
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	Options.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestTrue(TEXT("default texture generation activates"), WoTPayloadActivationTest_ActivatePublished(
		World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>(), CacheRoot, ActivationError)))
	{ AddError(ActivationError); return false; }
	FWoTBSPPayloadData Original;
	AddTJunctionTriangle(Original, FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0), 0, true);
	AddTJunctionDonor(Original, FVector3f(50, 0, 0), 0, false);
	for (bool bDedicated : { false, true })
	{
		FWoTRuntimeMoverModel Model;
		Model.Positions = Original.Positions;
		Model.Normals = Original.Normals;
		Model.UV0 = Original.UV0;
		Model.Indices = Original.Indices;
		FWoTRuntimeMoverSection& Section = Model.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = TEXT("None");
		Section.IndexCount = Model.Indices.Num();
		Section.MaxVertex = Model.Positions.Num() - 1;
		Section.bCollisionEnabled = true;
		FWoTBSPPayloadData ExpectedCollision = Original;
		if (bDedicated)
		{
			Model.CollisionPositions = { FVector3f(0, 0, -10), FVector3f(100, 0, -10), FVector3f(0, 100, -10) };
			Model.CollisionIndices = { 0, 1, 2 };
			ExpectedCollision.Positions = Model.CollisionPositions;
			ExpectedCollision.Indices = Model.CollisionIndices;
			ExpectedCollision.Normals.SetNum(3);
			ExpectedCollision.UV0.SetNum(3);
			ExpectedCollision.UV1.SetNum(3);
			ExpectedCollision.Sections[0].IndexCount = 3;
			ExpectedCollision.Sections[0].MaxVertex = 2;
		}
		AWoTMover* Mover = World->SpawnActor<AWoTMover>();
		FString Error;
		if (!TestNotNull(TEXT("mover spawns"), Mover)
			|| !TestTrue(TEXT("real projector mover mesh builds"), WoTBuildMoverMeshForTesting(*Mover, Model, Error)))
		{ AddError(Error); return false; }
		TArray<FWoTBSPPayloadData> Render;
		TestTrue(TEXT("mover built render data is CPU accessible"), ReadTJunctionRenderMesh(Mover->RuntimeMesh, Render.AddDefaulted_GetRef()));
		TArray<FString> Examples;
		TestEqual(TEXT("mover built render has zero proper T-junctions"), AuditTJunctionRenderParts(Render, Examples), uint64(0));
		TestTrue(TEXT("mover host was split"), Render[0].Indices.Num() > Original.Indices.Num());
		UStaticMesh* Reference = NewObject<UStaticMesh>(World);
		if (!TestTrue(TEXT("unrepaired collision reference builds"), WoTRuntimeBSPBuildCollision(*Reference, ExpectedCollision, Error)))
		{ AddError(Error); return false; }
		const FString Expected = TJunctionCollisionSignature(*Reference);
		TestFalse(TEXT("reference has cooked collision topology"), Expected.IsEmpty());
		TestEqual(bDedicated ? TEXT("dedicated collision topology unchanged") : TEXT("fallback collision topology unchanged"),
			TJunctionCollisionSignature(*Mover->RuntimeMesh), Expected);
		TestEqual(TEXT("source mover positions untouched"), Model.Positions, Original.Positions);
		TestEqual(TEXT("source mover indices untouched"), Model.Indices, Original.Indices);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPMultiplayerMapTJunctionSpec,
	"WoT.World.RuntimeBSP.MultiplayerMapTJunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPMultiplayerMapTJunctionSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{ AddWarning(WoTReferenceMissingWarning(TEXT("original"))); return true; }
	auto Loaded = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("authenticated retail contract loads"), Loaded.IsError())) return false;
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : Loaded.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World && Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase)) continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		auto& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(TEXT("authenticated package bytes load"), FFileHelper::LoadFileToArray(Source.Bytes,
			*(WoTReferencePath(TEXT("original")) / Fact.SourcePath)))) return false;
	}
	FTJunctionRetailWriter Writer;
	FTJunctionRetailProgress Progress;
	auto Imported = ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("authenticated production world set imports"), Imported.IsError()))
	{ AddError(Imported.GetError().ToString()); return false; }
	const TCHAR* GateMaps[] = {
		TEXT("Arena_01"), TEXT("Arena_02"), TEXT("Arena_03"),
		TEXT("Arena_04"), TEXT("Arena_05"), TEXT("Arena_06"),
		TEXT("Arena_07"), TEXT("Arena_08"), TEXT("Arena_09"),
		TEXT("Arena_11"), TEXT("Arena_12"), TEXT("Battle2_01"),
		TEXT("Battle2_02"), TEXT("Battle2_03"), TEXT("Battle2_04"),
		TEXT("Battle2_05"), TEXT("Battle3_01"), TEXT("Battle3_02"),
		TEXT("Battle4_01")};
	for (const TCHAR* Map : GateMaps)
	{
		const FString MapName(Map);
		const FWoTWorldDescriptor* Descriptor = Imported.GetValue().Worlds.FindByPredicate(
			[Map](const FWoTWorldDescriptor& Item) { return Item.MapIdentity == Map; });
		if (!TestNotNull(MapName + TEXT(" authenticated descriptor"), Descriptor)) continue;
		TArray<FWoTBSPPayloadData> Parts;
		FString Error;
		bool bDecoded = true;
		for (const FWoTBSPPartDescriptor& Part : Descriptor->BSPParts)
		{
			TArray<uint8>* Bytes = Writer.Objects.Find(Part.LogicalId);
			if (!TestNotNull(MapName + TEXT(" authenticated BSP object"), Bytes))
			{ bDecoded = false; break; }
			auto Decoded = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
				MoveTemp(*Bytes), TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
			if (!TestFalse(MapName + TEXT(" BSP object frame decodes"), Decoded.IsError())
				|| !TestTrue(MapName + TEXT(" BSP payload decodes"),
					UWoTBSPPayloadLibrary::DecodePayload(Decoded.GetValue().Payload(),
						Parts.AddDefaulted_GetRef(), Error)))
			{ bDecoded = false; break; }
		}
		if (!bDecoded) continue;
		const TArray<FWoTBSPPayloadData> Original = Parts;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTRuntimeBSPWorld* Runtime = nullptr;
		uint32 FallbackLightmaps = 0;
		FWoTBSPTJunctionStats Repair;
		double BSPMilliseconds = 0;
		// No sky view: retain every drawable section in shared world coordinates
		// so the built-buffer audit includes joins across parts/materials.
		if (!TestTrue(MapName + TEXT(" production world render build"),
			WoTWorldBuildRuntimeBSPForTesting(*World, *Descriptor, Parts, Runtime,
				FallbackLightmaps, Error, {}, nullptr, nullptr, nullptr, nullptr,
				EWoTRuntimeSkyStageFault::None, nullptr, {}, nullptr, &Repair,
				&BSPMilliseconds)))
		{
			AddError(MapName + TEXT(": ") + Error);
			continue;
		}
		TArray<FWoTBSPPayloadData> Render;
		uint64 RenderTriangles = 0;
		bool bReadBuffers = true;
		for (UStaticMeshComponent* Component : Runtime->GeometryComponents)
		{
			if (!Component || !Component->IsVisible()) continue;
			if (!TestTrue(MapName + TEXT(" built visible mesh has CPU render buffers"),
				ReadTJunctionRenderMesh(Component->GetStaticMesh(), Render.AddDefaulted_GetRef())))
			{ bReadBuffers = false; break; }
			RenderTriangles += Render.Last().Indices.Num() / 3;
		}
		if (!bReadBuffers) continue;
		TestTrue(MapName + TEXT(" audit collects visible render parts"), !Render.IsEmpty());
		TestTrue(MapName + TEXT(" audit collects render triangles"), RenderTriangles > 0);
		TArray<FString> Examples;
		TArray<FTJunctionAuditWitness> Witnesses;
		const double AuditStart = FPlatformTime::Seconds();
		const uint64 Junctions = AuditTJunctionRenderParts(Render, Examples, &Witnesses);
		const double AuditMilliseconds = (FPlatformTime::Seconds() - AuditStart) * 1000;
		uint64 FallbackJunctions = 0;
		uint64 NearCollapsedJunctions = 0;
		uint64 PostFilterProtectedJunctions = 0;
		uint64 UnexplainedJunctions = 0;
		const bool bWholePayloadFallback = Repair.bWholePayloadFallback;
		auto OriginalNonzeroEdgeIncidence = [&Original](const FVector3f& EdgeA,
			const FVector3f& EdgeB)
		{
			uint32 Incidence = 0;
			for (const FWoTBSPPayloadData& Part : Original)
				for (int32 At = 0; At < Part.Indices.Num(); At += 3)
				{
					const FVector3f A = Part.Positions[Part.Indices[At]];
					const FVector3f B = Part.Positions[Part.Indices[At + 1]];
					const FVector3f C = Part.Positions[Part.Indices[At + 2]];
					if (FVector3d::CrossProduct(FVector3d(B) - FVector3d(A),
						FVector3d(C) - FVector3d(A)).SizeSquared() == 0) continue;
					for (int32 Edge = 0; Edge < 3; ++Edge)
					{
						const FVector3f X = Part.Positions[Part.Indices[At + Edge]];
						const FVector3f Y = Part.Positions[Part.Indices[At + (Edge + 1) % 3]];
						if ((X == EdgeA && Y == EdgeB) || (X == EdgeB && Y == EdgeA))
							++Incidence;
					}
				}
			return Incidence;
		};
		for (const FTJunctionAuditWitness& Witness : Witnesses)
		{
			bool bExplained = bWholePayloadFallback;
			for (int32 At = 0; !bExplained && At + 2 < Repair.FallbackTriangleCorners.Num(); At += 3)
			{
				auto ContainsCorner = [&Repair, At](const FVector3f& Point)
				{
					return Point == Repair.FallbackTriangleCorners[At]
						|| Point == Repair.FallbackTriangleCorners[At + 1]
						|| Point == Repair.FallbackTriangleCorners[At + 2];
				};
				bExplained = ContainsCorner(Witness.A)
					&& ContainsCorner(Witness.B) && ContainsCorner(Witness.C);
			}
			if (bExplained)
			{
				++FallbackJunctions;
				continue;
			}
			for (int32 At = 0; !bExplained && At + 2 < Repair.NearCollapsedEdgeWitnesses.Num(); At += 3)
			{
				const bool bSameEdge = (Witness.A == Repair.NearCollapsedEdgeWitnesses[At]
					&& Witness.B == Repair.NearCollapsedEdgeWitnesses[At + 1])
					|| (Witness.B == Repair.NearCollapsedEdgeWitnesses[At]
						&& Witness.A == Repair.NearCollapsedEdgeWitnesses[At + 1]);
				bExplained = bSameEdge
					&& Witness.P == Repair.NearCollapsedEdgeWitnesses[At + 2];
			}
			if (bExplained)
			{
				++NearCollapsedJunctions;
				continue;
			}
			// Repair must leave an exact incidence-two source edge untouched. If
			// final visibility filtering removes its sibling face, that same edge
			// is incidence one in the visible CPU buffers. Correlate the exact
			// output endpoints back to the independent source payload here.
			const uint32 OriginalIncidence = OriginalNonzeroEdgeIncidence(
				Witness.A, Witness.B);
			if (OriginalIncidence > 1)
			{
				++PostFilterProtectedJunctions;
				continue;
			}
			else
			{
				if (UnexplainedJunctions < 12)
					AddInfo(FString::Printf(TEXT("%s unexplained A=%s B=%s C=%s P=%s original_incidence=%u"),
						Map, *Witness.A.ToString(), *Witness.B.ToString(),
						*Witness.C.ToString(), *Witness.P.ToString(),
						OriginalIncidence));
				++UnexplainedJunctions;
			}
		}
		AddInfo(FString::Printf(TEXT("MPMapTJunction map=%s triangles=%llu input=%llu fan_pairs=%llu edges=%llu inserted=%llu near_edges=%llu near_skipped=%llu near_gap=%.9f ribbon_surfaces=%llu ribbon_triangles=%llu ribbon_vertices=%llu ribbon_width=%.9f ribbon_triangle_visits=%llu ribbon_checks=%llu unmatched=%llu fallback_unmatched=%llu near_unmatched=%llu protected_unmatched=%llu unexplained=%llu near_collapsed=%llu fallback=%d fallback_triangles=%llu split=%llu cells=%llu checks=%llu repair_ms=%.3f bsp_ms=%.3f audit_ms=%.3f"),
			Map, RenderTriangles, Repair.InputTriangles,
			Repair.RetessellatedFanSliverPairs, Repair.RepairedEdges,
			Repair.InsertedEdgeVertices,
			Repair.RepairedNearCoincidentEdges,
			Repair.SkippedNearCoincidentTriangles,
			Repair.MaxRepairedNearCoincidentGap,
			Repair.RepairedLightmapRibbonSurfaces,
			Repair.RepairedLightmapRibbonTriangles,
			Repair.RepairedLightmapRibbonVertices,
			Repair.MaxRepairedLightmapRibbonWidth,
			Repair.LightmapRibbonSourceTriangleVisits,
			Repair.LightmapRibbonChecks,
			Junctions, FallbackJunctions,
			NearCollapsedJunctions, PostFilterProtectedJunctions,
			UnexplainedJunctions,
			Repair.NearCollapsedChildEdges,
			Repair.bFellBackToUnrepaired ? 1 : 0, Repair.FallbackTriangles,
			Repair.SplitTriangles, Repair.CellVisits, Repair.CandidateChecks,
			Repair.RepairMilliseconds, BSPMilliseconds, AuditMilliseconds));
		if (NearCollapsedJunctions > 0)
			AddInfo(MapName + TEXT(" unmatched explanation=exact near-collapsed source-corner/child-edge witness within the 0.01 repair tolerance"));
		if (PostFilterProtectedJunctions > 0)
			AddInfo(MapName + TEXT(" unmatched explanation=exact incidence-two source edge became incidence one only after final render visibility filtering"));
		if (Repair.bFellBackToUnrepaired)
			AddInfo(MapName + TEXT(" fallback_reason=") + Repair.FallbackReason);
		for (const FString& Example : Examples)
			AddInfo(MapName + TEXT(" ") + Example);
		TestEqual(MapName + TEXT(" every unmatched junction has exact fallback, near-collapsed, or protected-edge provenance"),
			UnexplainedJunctions, uint64(0));
		TestTrue(MapName + TEXT(" BSP materialization stays below 30 seconds"),
			BSPMilliseconds < 30000.0);
		TestTrue(MapName + TEXT(" lightmap ribbon repair stays inside its 0.5-unit bound"),
			Repair.MaxRepairedLightmapRibbonWidth <= 0.5);
		TestTrue(MapName + TEXT(" near-coincident repair stays inside its 0.5-unit bound"),
			Repair.MaxRepairedNearCoincidentGap <= 0.5);
		if (MapName == TEXT("Battle2_02"))
			TestTrue(TEXT("Battle2_02 19-map build repairs the X6 dark ribbon"),
				Repair.RepairedLightmapRibbonSurfaces >= 1);
		for (int32 At = 0; At < Parts.Num(); ++At)
		{
			TestEqual(MapName + TEXT(" static collision source positions unchanged"),
				Parts[At].Positions, Original[At].Positions);
			TestEqual(MapName + TEXT(" static collision source indices unchanged"),
				Parts[At].Indices, Original[At].Indices);
		}
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
