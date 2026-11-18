#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "../WoTBSPPayload.h"

namespace WoTX6Test
{
struct FHit
{
	bool bHit = false;
	double Distance = TNumericLimits<double>::Max();
	int32 Part = INDEX_NONE;
	int32 Triangle = INDEX_NONE;
	int32 Surface = INDEX_NONE;
	FVector2d UV1 = FVector2d::ZeroVector;
};

inline void PixelRay(double VideoSeconds, int32 PixelX, int32 PixelY,
	FVector3d& OutOrigin, FVector3d& OutDirection)
{
	const double Raw = FMath::Clamp((VideoSeconds + 0.15) / 4.3, 0.0, 1.0);
	const double Alpha = Raw < 0.5
		? 4.0 * Raw * Raw * Raw
		: 1.0 - FMath::Pow(-2.0 * Raw + 2.0, 3.0) / 2.0;
	const FVector3d A(-17350.0, -8390.0, 21061.0);
	const FVector3d B(-17342.0, -8449.0, 21061.0);
	const FVector3d Tangent = 0.5 * (B - A);
	const FVector3d Location = FMath::CubicInterp(A, Tangent, B, Tangent, Alpha);
	const FRotator Rotation(FMath::Lerp(21.0, 26.0, Alpha), 278.0, 0.0);
	const FRotationMatrix Axes(Rotation);
	const double ScreenX = (double(PixelX) + 0.5) / 3840.0 * 2.0 - 1.0;
	const double ScreenY = 1.0 - (double(PixelY) + 0.5) / 2160.0 * 2.0;
	const double TanHalfHorizontalFOV =
		FMath::Tan(FMath::DegreesToRadians(75.0) * 0.5);
	OutOrigin = Location;
	OutDirection = (FVector3d(Axes.GetUnitAxis(EAxis::X))
		+ FVector3d(Axes.GetUnitAxis(EAxis::Y)) * ScreenX * TanHalfHorizontalFOV
		+ FVector3d(Axes.GetUnitAxis(EAxis::Z)) * ScreenY
			* TanHalfHorizontalFOV / (16.0 / 9.0)).GetSafeNormal();
}

inline bool IntersectTriangle(const FVector3d& Origin,
	const FVector3d& Direction, const FVector3d& A, const FVector3d& B,
	const FVector3d& C, double& OutDistance, FVector3d& OutBarycentric)
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

inline FHit HitPayload(const TArray<FWoTBSPPayloadData>& Parts,
	double VideoSeconds, int32 PixelX, int32 PixelY)
{
	FVector3d Origin, Direction;
	PixelRay(VideoSeconds, PixelX, PixelY, Origin, Direction);
	FHit Result;
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
			if (!IntersectTriangle(Origin, Direction,
				FVector3d(Part.Positions[I0]), FVector3d(Part.Positions[I1]),
				FVector3d(Part.Positions[I2]), Distance, Barycentric)
				|| Distance >= Result.Distance) continue;
			Result.bHit = true;
			Result.Distance = Distance;
			Result.Part = PartIndex;
			Result.Triangle = At / 3;
			Result.Surface = Part.TriangleSurfaces.IsValidIndex(At / 3)
				? int32(Part.TriangleSurfaces[At / 3]) : INDEX_NONE;
			Result.UV1 = Barycentric.X * FVector2d(Part.UV1[I0])
				+ Barycentric.Y * FVector2d(Part.UV1[I1])
				+ Barycentric.Z * FVector2d(Part.UV1[I2]);
		}
	}
	return Result;
}

inline double AtlasLuminance(const TArray<FWoTBSPPayloadData>& Parts,
	const FHit& Hit)
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
	const auto Pixel = [Atlas](int32 PX, int32 PY)
	{
		PX = FMath::Clamp(PX, 0, int32(Atlas->LightmapWidth) - 1);
		PY = FMath::Clamp(PY, 0, int32(Atlas->LightmapHeight) - 1);
		const int32 Offset = (PY * Atlas->LightmapWidth + PX) * 4;
		return (double(Atlas->LightmapBGRA[Offset])
			+ Atlas->LightmapBGRA[Offset + 1]
			+ Atlas->LightmapBGRA[Offset + 2]) / 3.0;
	};
	return FMath::Lerp(
		FMath::Lerp(Pixel(X, Y), Pixel(X + 1, Y), FractionX),
		FMath::Lerp(Pixel(X, Y + 1), Pixel(X + 1, Y + 1), FractionX),
		FractionY);
}

inline bool FinalCenterUsesLitNeighbour(double SourceLeft, double SourceRight,
	double FinalLeft, double FinalCenter, double FinalRight)
{
	if (SourceLeft < 0 || SourceRight < 0 || FinalLeft < 0
		|| FinalCenter < 0 || FinalRight < 0) return false;
	const double SourceNeighbour = FMath::Min(SourceLeft, SourceRight);
	return SourceNeighbour >= 16.0 && FinalCenter >= 16.0
		&& FinalCenter * 2.0 >= SourceNeighbour
		&& FinalCenter * 2.0 >= FMath::Min(FinalLeft, FinalRight);
}

inline bool ReadRenderMesh(UStaticMesh* Mesh, FWoTBSPPayloadData& Out)
{
	if (!Mesh || !Mesh->bAllowCPUAccess || !Mesh->GetRenderData()
		|| Mesh->GetRenderData()->LODResources.IsEmpty()) return false;
	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	if (LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() < 2)
		return false;
	for (uint32 At = 0;
		At < LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices(); ++At)
	{
		Out.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(At));
		Out.UV0.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(At, 0));
		Out.UV1.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(At, 1));
	}
	for (int32 At = 0; At < LOD.IndexBuffer.GetNumIndices(); ++At)
		Out.Indices.Add(LOD.IndexBuffer.GetIndex(At));
	return !Out.Indices.IsEmpty();
}

inline uint32 EdgeIncidence(const TArray<FWoTBSPPayloadData>& Parts,
	const FVector3f& EdgeA, const FVector3f& EdgeB)
{
	uint32 Result = 0;
	for (const FWoTBSPPayloadData& Part : Parts)
	{
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
					++Result;
			}
		}
	}
	return Result;
}

inline double PointSegmentDistance(const FVector3f& Point,
	const FVector3f& EdgeA, const FVector3f& EdgeB)
{
	const FVector3d A(EdgeA);
	const FVector3d B(EdgeB);
	const FVector3d Delta = B - A;
	const double LengthSquared = Delta.SizeSquared();
	const double Parameter = LengthSquared == 0 ? 0 : FMath::Clamp(
		FVector3d::DotProduct(FVector3d(Point) - A, Delta) / LengthSquared,
		0.0, 1.0);
	return (FVector3d(Point) - (A + Parameter * Delta)).Size();
}
}
