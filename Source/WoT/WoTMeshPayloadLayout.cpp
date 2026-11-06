#include "WoTMeshPayloadLayout.h"

#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "WoTSha256.h"

#if WITH_EDITOR
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#endif

namespace
{
	constexpr float WoTMeshLayout_PositionTolerance = 0.02f;
	// Every description vertex receives its own packed point. The positions are
	// opaque transfer keys: UE1 identity is never decoded from this lattice.
	// One-UU spacing keeps all description vertices above the weld threshold.
	// Bounds are restored after the rebuild, so the ~1023-UU extent is inert.
	constexpr float WoTMeshLayout_LatticeStep = 1.0f;
	constexpr float WoTMeshLayout_LatticeTolerance = 1.e-4f;
	constexpr float WoTMeshLayout_LatticeMaxIndex = 1023.0f;
	constexpr float WoTMeshLayout_NormalTolerance = 0.01f;
	// Imported pawn normals are not perfectly unit (AesSedai len^2 1.0104);
	// a base normal cancels in reconstruction (base + (frame - base) =
	// frame), so accept the real imported length here — the raw captured
	// value must match the shipped render buffer bit-for-bit.
	constexpr float WoTMeshLayout_BaseLengthTolerance = 0.06f;
	constexpr float WoTMeshLayout_MappingUVTolerance = 0.01f;
	constexpr uint32 WoTMeshLayout_MappingUVChannel = 1;

	FVector3f WoTMeshLayout_LatticePositionFromVertex(const int32 VertexIndex)
	{
		return FVector3f(
			(VertexIndex & 1023) * WoTMeshLayout_LatticeStep,
			((VertexIndex >> 10) & 1023) * WoTMeshLayout_LatticeStep,
			((VertexIndex >> 20) & 1023) * WoTMeshLayout_LatticeStep);
	}

	bool WoTMeshLayout_IsLatticeComponent(const double Component)
	{
		if (!FMath::IsFinite(Component))
		{
			return false;
		}
		const float Value = static_cast<float>(Component);
		const float LatticeIndex = FMath::RoundToFloat(
			Value / WoTMeshLayout_LatticeStep);
		return LatticeIndex >= 0.0f
			&& LatticeIndex <= WoTMeshLayout_LatticeMaxIndex
			&& FMath::IsNearlyEqual(Value,
				LatticeIndex * WoTMeshLayout_LatticeStep,
				WoTMeshLayout_LatticeTolerance);
	}

	bool WoTMeshLayout_IsLatticePosition(const FVector& Position)
	{
		return WoTMeshLayout_IsLatticeComponent(Position.X)
			&& WoTMeshLayout_IsLatticeComponent(Position.Y)
			&& WoTMeshLayout_IsLatticeComponent(Position.Z);
	}

	FIntVector WoTMeshLayout_PositionKey(const FVector3f& Position)
	{
		return FIntVector(
			FMath::RoundToInt(Position.X / WoTMeshLayout_LatticeStep),
			FMath::RoundToInt(Position.Y / WoTMeshLayout_LatticeStep),
			FMath::RoundToInt(Position.Z / WoTMeshLayout_LatticeStep));
	}

	bool WoTMeshLayout_TryExactPositionKey(
		const FVector3f& Position, FIntVector& OutKey)
	{
		if (!WoTMeshLayout_IsLatticePosition(FVector(Position)))
		{
			return false;
		}
		OutKey = WoTMeshLayout_PositionKey(Position);
		return Position == FVector3f(
			static_cast<float>(OutKey.X),
			static_cast<float>(OutKey.Y),
			static_cast<float>(OutKey.Z)) * WoTMeshLayout_LatticeStep;
	}

	void WoTMeshLayout_AppendFloatLE(float Value, TArray<uint8>& Bytes)
	{
		uint32 Bits = 0;
		static_assert(sizeof(Bits) == sizeof(Value));
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		for (int32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
		{
			Bytes.Add(static_cast<uint8>((Bits >> (ByteIndex * 8)) & 0xff));
		}
	}

	struct FWoTMeshLayout_RenderCapture
	{
		TArray<int32> Mapping;
		TArray<FVector> Positions;
		TArray<FVector> Tangents;
	};

	bool WoTMeshLayout_DecodeMappingUV(
		const FStaticMeshVertexBuffer& StaticBuffer,
		uint32 RenderVertexCount, int32 FrameVertexCount,
		TArray<int32>& OutMapping, FString& OutError)
	{
		OutMapping.Reset();
		if (StaticBuffer.GetNumVertices() != RenderVertexCount
			|| !StaticBuffer.GetUseFullPrecisionUVs()
			|| StaticBuffer.GetNumTexCoords() <= WoTMeshLayout_MappingUVChannel)
		{
			OutError = TEXT("LOD-0 TEXCOORD_1 mapping carrier is incomplete or is not full precision");
			return false;
		}
		OutMapping.Reserve(static_cast<int32>(RenderVertexCount));
		for (uint32 RenderVertex = 0; RenderVertex < RenderVertexCount; ++RenderVertex)
		{
			const float EncodedFloat = StaticBuffer.GetVertexUV(
				RenderVertex, WoTMeshLayout_MappingUVChannel).X;
			if (!FMath::IsFinite(EncodedFloat)
				|| EncodedFloat < 1.0f
				|| EncodedFloat > static_cast<float>(FrameVertexCount))
			{
				OutError = FString::Printf(
					TEXT("render vertex %u has an invalid TEXCOORD_1 mapping carrier"),
					RenderVertex);
				return false;
			}
			const int32 Encoded = FMath::RoundToInt(EncodedFloat);
			if (!FMath::IsNearlyEqual(
					EncodedFloat, static_cast<float>(Encoded),
					WoTMeshLayout_MappingUVTolerance))
			{
				OutError = FString::Printf(
					TEXT("render vertex %u has a non-integral TEXCOORD_1 mapping carrier"),
					RenderVertex);
				return false;
			}
			OutMapping.Add(Encoded - 1);
		}
		return true;
	}

	bool WoTMeshLayout_ValidateCarrierFreeSingleUV(
		USkeletalMesh* Mesh, const TCHAR* Context, FString& OutError)
	{
		const FSkeletalMeshRenderData* RenderData = Mesh
			? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
		{
			OutError = FString::Printf(TEXT("%s skeletal mesh has no LOD-0 render data"), Context);
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
		const FStaticMeshVertexBuffer& StaticBuffer =
			LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		const FColorVertexBuffer& ColorBuffer =
			LOD.StaticVertexBuffers.ColorVertexBuffer;
		if (Mesh->GetHasVertexColors() || ColorBuffer.GetNumVertices() != 0)
		{
			OutError = FString::Printf(TEXT("%s mesh retained the COLOR_0 mapping carrier"), Context);
			return false;
		}
		if (StaticBuffer.GetNumTexCoords() != 1)
		{
			OutError = FString::Printf(
				TEXT("%s mesh has %u UV channels; expected exactly TEXCOORD_0"),
				Context, StaticBuffer.GetNumTexCoords());
			return false;
		}
		return true;
	}

	FString WoTMeshLayout_TriangleKey(int32 A, int32 B, int32 C)
	{
		if (A > B) Swap(A, B);
		if (B > C) Swap(B, C);
		if (A > B) Swap(A, B);
		return FString::Printf(TEXT("%d:%d:%d"), A, B, C);
	}

	bool WoTMeshLayout_VerifyTriangleMapping(
		const FSkeletalMeshLODRenderData& LOD, int32 FrameVertexCount,
		TConstArrayView<int32> Mapping,
		const TArray<int32>& ExpectedRenderTriangles, FString& OutError)
	{
		if (ExpectedRenderTriangles.Num() % 3 != 0)
		{
			OutError = TEXT("expected render triangle list is not a triple array");
			return false;
		}
		TArray<uint32> Indices;
		LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
		TMap<FString, int32> ExpectedCounts;
		for (int32 Offset = 0; Offset < ExpectedRenderTriangles.Num(); Offset += 3)
		{
			const int32 A = ExpectedRenderTriangles[Offset];
			const int32 B = ExpectedRenderTriangles[Offset + 1];
			const int32 C = ExpectedRenderTriangles[Offset + 2];
			if (A < 0 || B < 0 || C < 0
				|| A >= FrameVertexCount || B >= FrameVertexCount || C >= FrameVertexCount)
			{
				OutError = TEXT("expected render triangle references an invalid UE1 vertex");
				return false;
			}
			++ExpectedCounts.FindOrAdd(WoTMeshLayout_TriangleKey(A, B, C));
		}
		// A UE1-degenerate expected triangle (a repeated UE1 vertex, A==B etc.)
		// has zero visual contribution and is un-reproducible carrier-free:
		// its two identical-attribute corner instances were kept distinct only
		// by the carrier, so dropping the carrier legitimately merges them and
		// the triangle vanishes. Track which expected keys are degenerate so
		// missing ones can be excused below.
		TSet<FString> ExpectedDegenerateKeys;
		for (int32 Offset = 0; Offset < ExpectedRenderTriangles.Num(); Offset += 3)
		{
			const int32 A = ExpectedRenderTriangles[Offset];
			const int32 B = ExpectedRenderTriangles[Offset + 1];
			const int32 C = ExpectedRenderTriangles[Offset + 2];
			if (A == B || B == C || A == C)
			{
				ExpectedDegenerateKeys.Add(WoTMeshLayout_TriangleKey(A, B, C));
			}
		}
		TMap<FString, int32> ActualCounts;
		int32 ActualTriangleCount = 0;
		for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		{
			for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
			{
				const uint64 Base = static_cast<uint64>(Section.BaseIndex)
					+ static_cast<uint64>(Triangle) * 3ull;
				if (Base + 2 >= static_cast<uint64>(Indices.Num()))
				{
					OutError = TEXT("render section index range is invalid");
					return false;
				}
				const uint32 IA = Indices[static_cast<int32>(Base)];
				const uint32 IB = Indices[static_cast<int32>(Base + 1)];
				const uint32 IC = Indices[static_cast<int32>(Base + 2)];
				if (!Mapping.IsValidIndex(static_cast<int32>(IA))
					|| !Mapping.IsValidIndex(static_cast<int32>(IB))
					|| !Mapping.IsValidIndex(static_cast<int32>(IC)))
				{
					OutError = TEXT("render triangle references a vertex outside the mapping");
					return false;
				}
				++ActualCounts.FindOrAdd(WoTMeshLayout_TriangleKey(
					Mapping[static_cast<int32>(IA)],
					Mapping[static_cast<int32>(IB)],
					Mapping[static_cast<int32>(IC)]));
				++ActualTriangleCount;
			}
		}
		// The stripped mesh must reproduce every NON-DEGENERATE expected
		// triangle exactly, add none of its own, and may only be SHORT on
		// UE1-degenerate expected triangles (which render nothing and cannot
		// survive carrier removal). Any surplus/wrong actual key, or a missing
		// NON-degenerate expected key, is a real failure.
		int32 ActualSurplus = 0, MissingNonDegenerate = 0, MissingDegenerate = 0;
		for (const TPair<FString, int32>& Pair : ActualCounts)
		{
			if (Pair.Value > ExpectedCounts.FindRef(Pair.Key)) ++ActualSurplus;
		}
		for (const TPair<FString, int32>& Pair : ExpectedCounts)
		{
			if (ActualCounts.FindRef(Pair.Key) < Pair.Value)
			{
				if (ExpectedDegenerateKeys.Contains(Pair.Key)) ++MissingDegenerate;
				else ++MissingNonDegenerate;
			}
		}
		if (ActualSurplus != 0 || MissingNonDegenerate != 0)
		{
			OutError = FString::Printf(
				TEXT("post-import mapping does not reproduce the rendered "
					 "triangle multiset (actual tris %d expected %d; surplus %d "
					 "missing non-degenerate %d missing degenerate %d)"),
				ActualTriangleCount, ExpectedRenderTriangles.Num() / 3,
				ActualSurplus, MissingNonDegenerate, MissingDegenerate);
			return false;
		}
		return true;
	}

	bool WoTMeshLayout_CaptureRenderData(USkeletalMesh* Mesh,
		int32 FrameVertexCount, const TArray<int32>& ExpectedRenderTriangles,
		FWoTMeshLayout_RenderCapture& Out, FString& OutError)
	{
		Out = FWoTMeshLayout_RenderCapture();
		const FSkeletalMeshRenderData* RenderData = Mesh
			? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
		{
			OutError = TEXT("skeletal mesh has no LOD-0 render data");
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
		const FPositionVertexBuffer& PositionBuffer =
			LOD.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& StaticBuffer =
			LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		const FColorVertexBuffer& ColorBuffer =
			LOD.StaticVertexBuffers.ColorVertexBuffer;
		const uint32 RenderVertexCount = PositionBuffer.GetNumVertices();
		if (RenderVertexCount == 0 || RenderVertexCount > MAX_int32
			|| StaticBuffer.GetNumVertices() != RenderVertexCount
			|| ColorBuffer.GetNumVertices() != RenderVertexCount
			|| !StaticBuffer.GetUseFullPrecisionUVs()
			|| StaticBuffer.GetNumTexCoords() <= WoTMeshLayout_MappingUVChannel)
		{
			OutError = TEXT("LOD-0 mapping carriers are incomplete or TEXCOORD_1 is not full precision");
			return false;
		}

		Out.Mapping.Reserve(static_cast<int32>(RenderVertexCount));
		TArray<int32> UVMapping;
		if (!WoTMeshLayout_DecodeMappingUV(StaticBuffer, RenderVertexCount,
				FrameVertexCount, UVMapping, OutError))
		{
			return false;
		}
		Out.Positions.Reserve(static_cast<int32>(RenderVertexCount));
		Out.Tangents.Reserve(static_cast<int32>(RenderVertexCount));
		for (uint32 RenderVertex = 0; RenderVertex < RenderVertexCount; ++RenderVertex)
		{
			const FColor Carrier = ColorBuffer.VertexColor(RenderVertex);
			if (Carrier.A != 255)
			{
				OutError = FString::Printf(
					TEXT("render vertex %u lost the mapping carrier alpha"), RenderVertex);
				return false;
			}
			const int32 Encoded = static_cast<int32>(Carrier.R)
				| (static_cast<int32>(Carrier.G) << 8)
				| (static_cast<int32>(Carrier.B) << 16);
			const int32 UE1Vertex = Encoded - 1;
			if (UE1Vertex < 0 || UE1Vertex >= FrameVertexCount)
			{
				OutError = FString::Printf(
					TEXT("render vertex %u has invalid UE1 carrier %d"),
					RenderVertex, UE1Vertex);
				return false;
			}
			Out.Mapping.Add(UE1Vertex);
			Out.Positions.Add(FVector(PositionBuffer.VertexPosition(RenderVertex)));
			const FVector4f Tangent = StaticBuffer.VertexTangentZ(RenderVertex);
			Out.Tangents.Add(FVector(Tangent.X, Tangent.Y, Tangent.Z));
		}

		if (!UWoTMeshPayloadLayout::ValidateRenderVertexMappingAgreement(
				Out.Mapping, UVMapping, OutError))
		{
			return false;
		}
		return WoTMeshLayout_VerifyTriangleMapping(LOD, FrameVertexCount,
			Out.Mapping, ExpectedRenderTriangles, OutError);
	}

	bool WoTMeshLayout_CaptureCarrierBearingStrippedRenderData(
		USkeletalMesh* Mesh, int32 FrameVertexCount,
		const TArray<int32>& ExpectedRenderTriangles,
		FWoTMeshLayout_RenderCapture& Out,
		TMap<FIntVector, int32>& OutPositionKeyToUE1,
		FString& OutError)
	{
		Out = FWoTMeshLayout_RenderCapture();
		OutPositionKeyToUE1.Reset();
		const FSkeletalMeshRenderData* RenderData = Mesh
			? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
		{
			OutError = TEXT("stripped skeletal mesh has no LOD-0 render data");
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
		const FPositionVertexBuffer& PositionBuffer =
			LOD.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& StaticBuffer =
			LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		const uint32 RenderVertexCount = PositionBuffer.GetNumVertices();
		if (RenderVertexCount == 0 || RenderVertexCount > MAX_int32
			|| StaticBuffer.GetNumVertices() != RenderVertexCount
			|| StaticBuffer.GetNumTexCoords() != 2)
		{
			OutError = TEXT("carrier-bearing stripped rebuild has invalid LOD-0 vertex buffers");
			return false;
		}

		TArray<int32> UVMapping;
		if (!WoTMeshLayout_DecodeMappingUV(StaticBuffer, RenderVertexCount,
				FrameVertexCount, UVMapping, OutError))
		{
			return false;
		}

		// TEXCOORD_1 is authoritative in phase 1. The per-wedge position is only
		// an opaque key used to transfer that mapping after the carrier is dropped.
		Out.Mapping = UVMapping;
		Out.Positions.Reserve(static_cast<int32>(RenderVertexCount));
		Out.Tangents.Reserve(static_cast<int32>(RenderVertexCount));
		for (uint32 RenderVertex = 0; RenderVertex < RenderVertexCount; ++RenderVertex)
		{
			const FVector3f Position = PositionBuffer.VertexPosition(RenderVertex);
			FIntVector PositionKey;
			if (!WoTMeshLayout_TryExactPositionKey(Position, PositionKey))
			{
				OutError = FString::Printf(
					TEXT("phase-1 render vertex off the per-wedge lattice (render vertex %u)"),
					RenderVertex);
				return false;
			}
			const int32 UE1Vertex = UVMapping[static_cast<int32>(RenderVertex)];
			if (const int32* ExistingUE1 = OutPositionKeyToUE1.Find(PositionKey))
			{
				if (*ExistingUE1 != UE1Vertex)
				{
					OutError = FString::Printf(
						TEXT("phase-1 position key collision at render vertex %u"),
						RenderVertex);
					return false;
				}
			}
			else
			{
				OutPositionKeyToUE1.Add(PositionKey, UE1Vertex);
			}
			Out.Positions.Add(FVector(Position));
			const FVector4f Tangent = StaticBuffer.VertexTangentZ(RenderVertex);
			Out.Tangents.Add(FVector(Tangent.X, Tangent.Y, Tangent.Z));
		}
		return WoTMeshLayout_VerifyTriangleMapping(LOD, FrameVertexCount,
			Out.Mapping, ExpectedRenderTriangles, OutError);
	}

	bool WoTMeshLayout_CaptureCarrierFreeStrippedRenderData(
		USkeletalMesh* Mesh, int32 FrameVertexCount,
		const TArray<int32>& StrippedMapping,
		const TMap<FIntVector, int32>& PositionKeyToUE1,
		const TArray<int32>& ExpectedRenderTriangles,
		FWoTMeshLayout_RenderCapture& Out, FString& OutError)
	{
		Out = FWoTMeshLayout_RenderCapture();
		const FSkeletalMeshRenderData* RenderData = Mesh
			? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
		{
			OutError = TEXT("carrier-free stripped skeletal mesh has no LOD-0 render data");
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
		const FPositionVertexBuffer& PositionBuffer =
			LOD.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& StaticBuffer =
			LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		const uint32 RenderVertexCount = PositionBuffer.GetNumVertices();
		if (RenderVertexCount == 0 || RenderVertexCount > MAX_int32
			|| StaticBuffer.GetNumVertices() != RenderVertexCount)
		{
			OutError = TEXT("carrier-free stripped rebuild has invalid LOD-0 vertex buffers");
			return false;
		}
		if (!WoTMeshLayout_ValidateCarrierFreeSingleUV(
				Mesh, TEXT("stripped"), OutError))
		{
			return false;
		}

		// Phase 2 recovers its own render order by looking up the identical
		// per-wedge positions captured in phase 1. Positions are never decoded.
		Out.Mapping.Reserve(static_cast<int32>(RenderVertexCount));
		for (uint32 RenderVertex = 0; RenderVertex < RenderVertexCount; ++RenderVertex)
		{
			const FVector3f Position = PositionBuffer.VertexPosition(RenderVertex);
			FIntVector PositionKey;
			if (!WoTMeshLayout_TryExactPositionKey(Position, PositionKey))
			{
				OutError = FString::Printf(
					TEXT("carrier-free render vertex %u is off the per-wedge lattice"),
					RenderVertex);
				return false;
			}
			const int32* UE1Vertex = PositionKeyToUE1.Find(PositionKey);
			if (!UE1Vertex)
			{
				OutError = FString::Printf(
					TEXT("carrier-free render vertex position has no phase-1 UE1 mapping (render vertex %u)"),
					RenderVertex);
				return false;
			}
			Out.Mapping.Add(*UE1Vertex);
		}
		{
			TArray<int32> SortedPhase1 = StrippedMapping;
			TArray<int32> SortedPhase2 = Out.Mapping;
			SortedPhase1.Sort();
			SortedPhase2.Sort();
			if (SortedPhase1 != SortedPhase2)
			{
				OutError = TEXT("carrier drop changed the stripped UE1-vertex multiset");
				return false;
			}
		}
		Out.Positions.Reserve(static_cast<int32>(RenderVertexCount));
		Out.Tangents.Reserve(static_cast<int32>(RenderVertexCount));
		for (uint32 RenderVertex = 0; RenderVertex < RenderVertexCount; ++RenderVertex)
		{
			Out.Positions.Add(FVector(PositionBuffer.VertexPosition(RenderVertex)));
			const FVector4f Tangent = StaticBuffer.VertexTangentZ(RenderVertex);
			Out.Tangents.Add(FVector(Tangent.X, Tangent.Y, Tangent.Z));
		}
		return WoTMeshLayout_VerifyTriangleMapping(LOD, FrameVertexCount,
			Out.Mapping, ExpectedRenderTriangles, OutError);
	}

	bool WoTMeshLayout_VerifyExpectedBase(
		const FWoTMeshLayout_RenderCapture& Capture,
		const TArray<FVector>& ExpectedPositions,
		const TArray<FVector>& ExpectedNormals,
		FString& OutError)
	{
		if (ExpectedPositions.Num() != ExpectedNormals.Num())
		{
			OutError = TEXT("expected base position/normal counts disagree");
			return false;
		}
		for (int32 RenderVertex = 0; RenderVertex < Capture.Mapping.Num(); ++RenderVertex)
		{
			const int32 UE1Vertex = Capture.Mapping[RenderVertex];
			if (!ExpectedPositions.IsValidIndex(UE1Vertex))
			{
				OutError = FString::Printf(
					TEXT("render vertex %d carrier maps to out-of-range UE1 vertex %d"),
					RenderVertex, UE1Vertex);
				return false;
			}
			const double PositionDelta = (Capture.Positions[RenderVertex]
				- ExpectedPositions[UE1Vertex]).Size();
			// A near-zero expected base normal is degenerate: mesh2glb's retail
			// smooth normal (unweighted unit-face-normal average) cancels to zero
			// when a UE1 vertex is touched only by opposite-wound faces — a
			// zero-thickness double-sided sliver tip (e.g. Sister UE1 453). There
			// is no meaningful retail normal to compare against, and the importer
			// substitutes an arbitrary unit normal for the zero, so skip the normal
			// term. The imported unit normal is what gets pinned; the per-frame
			// injection deltas (retail frame normals, also zero here) reconstruct
			// the retail value, so parity is preserved. Position (matched to
			// tolerance) still confirms the vertex identity.
			const bool bDegenerateExpectedNormal =
				ExpectedNormals[UE1Vertex].SizeSquared()
					< WoTMeshLayout_NormalTolerance;
			const double NormalDelta = bDegenerateExpectedNormal ? 0.0
				: (Capture.Tangents[RenderVertex]
					- ExpectedNormals[UE1Vertex]).Size();
			if (PositionDelta > WoTMeshLayout_PositionTolerance
				|| NormalDelta > WoTMeshLayout_NormalTolerance)
			{
				OutError = FString::Printf(
					TEXT("render vertex %d (UE1 %d) carrier disagrees with transformed UE1 base: ")
					TEXT("position delta %.5f (tol %.5f), normal delta %.5f (tol %.5f)"),
					RenderVertex, UE1Vertex,
					PositionDelta, WoTMeshLayout_PositionTolerance,
					NormalDelta, WoTMeshLayout_NormalTolerance);
				return false;
			}
		}
		return true;
	}
}

FString UWoTMeshPayloadLayout::HashTangentBasis(
	TConstArrayView<FVector> Tangents)
{
	TArray<uint8> Bytes;
	if (Tangents.Num() > MAX_int32 / 12)
	{
		return FString();
	}
	Bytes.Reserve(Tangents.Num() * 12);
	for (const FVector& Tangent : Tangents)
	{
		WoTMeshLayout_AppendFloatLE(static_cast<float>(Tangent.X), Bytes);
		WoTMeshLayout_AppendFloatLE(static_cast<float>(Tangent.Y), Bytes);
		WoTMeshLayout_AppendFloatLE(static_cast<float>(Tangent.Z), Bytes);
	}
	return WoTSha256::HashBuffer(Bytes.GetData(), Bytes.Num());
}

bool UWoTMeshPayloadLayout::ValidatePinnedTangentHash(FString& OutError) const
{
	if (PinnedBaseTangentZSHA256.Len() != 64
		|| HashTangentBasis(PinnedBaseTangentZ) != PinnedBaseTangentZSHA256)
	{
		OutError = TEXT("pinned base TangentZ SHA-256 mismatch");
		return false;
	}
	return true;
}

bool UWoTMeshPayloadLayout::ValidateRenderVertexMappingAgreement(
	TConstArrayView<int32> RenderMapping,
	TConstArrayView<int32> UVDecodedMapping,
	FString& OutError)
{
	if (RenderMapping.Num() != UVDecodedMapping.Num())
	{
		OutError = FString::Printf(
			TEXT("render/TEXCOORD_1 mapping counts disagree: %d vs %d"),
			RenderMapping.Num(), UVDecodedMapping.Num());
		return false;
	}
	for (int32 RenderVertex = 0; RenderVertex < RenderMapping.Num(); ++RenderVertex)
	{
		if (RenderMapping[RenderVertex] != UVDecodedMapping[RenderVertex])
		{
			OutError = FString::Printf(
				TEXT("render/TEXCOORD_1 mapping disagrees at vertex %d: render=%d uv=%d"),
				RenderVertex, RenderMapping[RenderVertex],
				UVDecodedMapping[RenderVertex]);
			return false;
		}
	}
	return true;
}

bool UWoTMeshPayloadLayout::ValidateDescriptor(FString& OutError) const
{
	if (!PayloadKey.StartsWith(TEXT("mesh:"), ESearchCase::CaseSensitive)
		|| !AnimationLogicalId.StartsWith(
			TEXT("cache/animation/"), ESearchCase::CaseSensitive)
		|| FrameVertexCount <= 0 || AnimationFrameCount <= 0
		|| SmoothTriangles.IsEmpty() || SmoothTriangles.Num() % 3 != 0
		|| RenderVertexToUE1Vertex.IsEmpty()
		|| RenderVertexToUE1Vertex.Num() != PinnedBaseTangentZ.Num()
		|| !bRenderVertexMappingVerified || !bStrippedBasePositions
		|| !bTangentRecomputeDisabled)
	{
		OutError = TEXT("mesh payload layout is incomplete or is not a verified stripped layout");
		return false;
	}
	for (int32 UE1Vertex : SmoothTriangles)
	{
		if (UE1Vertex < 0 || UE1Vertex >= FrameVertexCount)
		{
			OutError = TEXT("smooth-normal topology references an invalid UE1 vertex");
			return false;
		}
	}
	for (int32 UE1Vertex : RenderVertexToUE1Vertex)
	{
		if (UE1Vertex < 0 || UE1Vertex >= FrameVertexCount)
		{
			OutError = TEXT("render mapping references an invalid UE1 vertex");
			return false;
		}
	}
	if (Origin.ContainsNaN() || RotationUnits.ContainsNaN() || Scale.ContainsNaN()
		|| !FMath::IsFinite(Origin.X) || !FMath::IsFinite(Origin.Y)
		|| !FMath::IsFinite(Origin.Z) || !FMath::IsFinite(RotationUnits.X)
		|| !FMath::IsFinite(RotationUnits.Y) || !FMath::IsFinite(RotationUnits.Z)
		|| !FMath::IsFinite(Scale.X) || !FMath::IsFinite(Scale.Y)
		|| !FMath::IsFinite(Scale.Z) || FMath::IsNearlyZero(Scale.X)
		|| FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z))
	{
		OutError = TEXT("mesh transform descriptor is invalid");
		return false;
	}
	for (const FVector& Tangent : PinnedBaseTangentZ)
	{
		// A ZERO base normal is legitimate for a degenerate co-UE1 render
		// vertex (canonical welds it away; the carrier-derived stripped mesh
		// keeps it). Runtime reconstruction is base(0) + delta = frame
		// normal, so the delta carries the full basis — same zero-basis
		// ruling as the BSP proof. Reject only NaN and PARTIAL vectors (a
		// corrupt half-length normal), accept unit OR zero.
		const double SquaredLength = Tangent.SquaredLength();
		const bool bUnit = FMath::IsNearlyEqual(
			SquaredLength, 1.0, WoTMeshLayout_BaseLengthTolerance);
		const bool bZero = SquaredLength <= WoTMeshLayout_NormalTolerance;
		if (Tangent.ContainsNaN() || !(bUnit || bZero))
		{
			OutError = FString::Printf(
				TEXT("pinned base TangentZ contains a partial vector (len^2 %f)"),
				SquaredLength);
			return false;
		}
	}
	return ValidatePinnedTangentHash(OutError);
}

bool UWoTMeshPayloadLibrary::SetLOD0AllowCPUAccess(
	USkeletalMesh* Mesh, const bool bAllowCPUAccess)
{
#if WITH_EDITOR
	if (Mesh)
	{
		if (FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(0))
		{
			Mesh->Modify();
			LODInfo->bAllowCPUAccess = bAllowCPUAccess;
			Mesh->MarkPackageDirty();
			return true;
		}
	}
#endif
	return false;
}

FString UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
	USkeletalMesh* Mesh, const FString& PayloadKey, FVector Origin,
	FVector Scale, FVector RotationUnits, bool bUnmirror,
	int32 FrameVertexCount, int32 AnimationFrameCount,
	const TArray<int32>& SmoothTriangles,
	const TArray<int32>& ExpectedRenderTriangles,
	const TArray<FVector>& ExpectedBasePositions,
	const TArray<FVector>& ExpectedBaseNormals, bool bStripPayload,
	const FString& AnimationLogicalId)
{
#if WITH_EDITOR
	FString EffectiveAnimationLogicalId = AnimationLogicalId;
	if (EffectiveAnimationLogicalId.IsEmpty()
		&& PayloadKey.StartsWith(TEXT("mesh:"), ESearchCase::CaseSensitive))
	{
		EffectiveAnimationLogicalId = TEXT("cache/animation/")
			+ PayloadKey.RightChop(5);
	}
	if (!Mesh || Mesh->HasAnyFlags(RF_ClassDefaultObject)
		|| !PayloadKey.StartsWith(TEXT("mesh:"), ESearchCase::CaseSensitive)
		|| (bStripPayload && !EffectiveAnimationLogicalId.StartsWith(
			TEXT("cache/animation/"), ESearchCase::CaseSensitive))
		|| FrameVertexCount <= 0 || AnimationFrameCount <= 0
		|| ExpectedBasePositions.Num() != FrameVertexCount
		|| ExpectedBaseNormals.Num() != FrameVertexCount
		|| SmoothTriangles.IsEmpty() || SmoothTriangles.Num() % 3 != 0)
	{
		return TEXT("invalid mesh payload capture arguments");
	}

	FWoTMeshLayout_RenderCapture CanonicalCapture;
	FString Error;
	FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});
	if (!WoTMeshLayout_CaptureRenderData(Mesh, FrameVertexCount,
			ExpectedRenderTriangles, CanonicalCapture, Error)
		|| !WoTMeshLayout_VerifyExpectedBase(CanonicalCapture,
			ExpectedBasePositions, ExpectedBaseNormals, Error))
	{
		return Error;
	}

	// A UE1 vertex whose retail smooth normal cancels to zero — a double-sided
	// sliver tip touched only by opposite-wound faces (e.g. Sister UE1 453) — is
	// exempt from the per-UE1 canonical-normal invariants below. Its render
	// duplicates legitimately carry OPPOSITE unit normals, and the runtime
	// reconstructs the retail (zero) normal per render vertex from the pinned
	// baseline plus the per-frame delta regardless of the baseline's direction,
	// so the pinned duplicates need not agree.
	TBitArray<> DegenerateExpectedNormal(false, FrameVertexCount);
	for (int32 UE1Vertex = 0; UE1Vertex < FrameVertexCount; ++UE1Vertex)
	{
		DegenerateExpectedNormal[UE1Vertex] =
			ExpectedBaseNormals[UE1Vertex].SizeSquared()
				< WoTMeshLayout_NormalTolerance;
	}

	// Every non-degenerate render duplicate for one UE1 vertex receives the same
	// authored smooth normal. Pin that canonical cooked value before any
	// stripping build.
	TArray<FVector> CanonicalTangentByUE1;
	TBitArray<> HasCanonicalTangent(false, FrameVertexCount);
	CanonicalTangentByUE1.SetNumZeroed(FrameVertexCount);
	for (int32 RenderVertex = 0; RenderVertex < CanonicalCapture.Mapping.Num(); ++RenderVertex)
	{
		const int32 UE1Vertex = CanonicalCapture.Mapping[RenderVertex];
		const FVector Tangent = CanonicalCapture.Tangents[RenderVertex];
		if (HasCanonicalTangent[UE1Vertex]
			&& !DegenerateExpectedNormal[UE1Vertex]
			&& !CanonicalTangentByUE1[UE1Vertex].Equals(
				Tangent, WoTMeshLayout_NormalTolerance))
		{
			return FString::Printf(TEXT("canonical render duplicates for UE1 vertex %d disagree on TangentZ"), UE1Vertex);
		}
		CanonicalTangentByUE1[UE1Vertex] = Tangent;
		HasCanonicalTangent[UE1Vertex] = true;
	}

	FWoTMeshLayout_RenderCapture FinalCapture = CanonicalCapture;
	if (bStripPayload)
	{
		const FBoxSphereBounds ImportedBounds = Mesh->GetImportedBounds();
		TArray<FName> MorphNames;
		MorphNames.Reserve(Mesh->GetMorphTargets().Num());
		for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
		{
			if (MorphTarget)
			{
				MorphNames.Add(MorphTarget->GetFName());
			}
		}
		if (!MorphNames.IsEmpty())
		{
			Mesh->RemoveMorphTargets(MorphNames, /*bInRebuildRenderMesh*/ false);
		}
		FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(0);
		FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!LODInfo || !Description)
		{
			return TEXT("cannot strip mesh without LOD-0 build settings and MeshDescription");
		}
		// Normals/tangents MUST NOT be recomputed: the positions become a
		// meaningless lattice, so a cook-time recompute would derive garbage
		// normals from lattice geometry. Preserve the imported basis (the
		// per-render-vertex canonical TangentZ the injector pins) and keep
		// UV0 at full precision after the carrier drop.
		LODInfo->BuildSettings.bRecomputeNormals = false;
		LODInfo->BuildSettings.bRecomputeTangents = false;
		LODInfo->BuildSettings.bUseFullPrecisionUVs = true;
		// Degenerate removal is POSITION-dependent: on the meaningless lattice
		// it collapses a DIFFERENT set of triangles than the real-position
		// import did, so the rebuilt UE1-triangle multiset stops matching the
		// ground-truth ExpectedRenderTriangles (AngrealGuardianPickup). Off =
		// the rebuild keeps every description triangle, reproducing the
		// as-imported set exactly. The carrier-derived mapping absorbs the
		// extra render vertices this keeps.
		LODInfo->BuildSettings.bRemoveDegenerates = false;
		LODInfo->bAllowCPUAccess = true;
		Mesh->ModifyMeshDescription(0);
		{
			FSkeletalMeshAttributes Attributes(*Description);
			TVertexAttributesRef<FVector3f> Positions =
				Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector4f> Colors =
				Attributes.GetVertexInstanceColors();
			TVertexInstanceAttributesRef<FVector2f> UVs =
				Attributes.GetVertexInstanceUVs();
			if (UVs.GetNumChannels() != 2)
			{
				return TEXT("cannot strip mesh without exactly TEXCOORD_0 and the verified TEXCOORD_1 carrier");
			}
			// Unregister every morph target attribute from the source description.
			// RemoveMorphTargets above drops only the materialized UMorphTargets;
			// the MeshDescription retains the interchange-authored morph delta
			// attributes, and each Mesh->Build() below re-materializes them —
			// including zero-delta frames the initial import had deduped (e.g.
			// TaintExpGeoB's F1-F4). Unregistering here makes the rebuilds produce
			// no morphs, which the stripped payload requires (deltas ride the
			// runtime injection, not native morph targets).
			for (const FName MorphTargetName : Attributes.GetMorphTargetNames())
			{
				Attributes.UnregisterMorphTargetAttribute(MorphTargetName);
			}
			// Give every description vertex one deterministic, weld-safe point.
			// The phase-1 carrier supplies UE1 identity; the position is only an
			// opaque key and is never decoded as a UE1 vertex.
			for (const FVertexID VertexID : Description->Vertices().GetElementIDs())
			{
				const int32 VertexIndex = VertexID.GetValue();
				if (VertexIndex < 0 || VertexIndex > 0x3fffffff)
				{
					return TEXT("stripped mesh description vertex exceeds the per-wedge lattice range");
				}
				Positions[VertexID] =
					WoTMeshLayout_LatticePositionFromVertex(VertexIndex);
			}
			for (const FVertexInstanceID VertexInstanceID
				: Description->VertexInstances().GetElementIDs())
			{
				Colors[VertexInstanceID] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			}
		}
		USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
		// Rebuild morph targets from the (now morph-free, see the unregister above)
		// committed description so the deduped zero-delta attributes cannot be
		// re-materialized by the builds. With bUpdateMorphTargets=false the commit
		// preserved the prior morph data and the rebuilds resurrected it.
		CommitParams.bUpdateMorphTargets = true;
		CommitParams.bForceUpdate = true;
		if (!Mesh->CommitMeshDescription(0, CommitParams))
		{
			return TEXT("failed to commit the carrier-bearing stripped LOD-0 MeshDescription");
		}
		Mesh->Build();
		FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});

		FWoTMeshLayout_RenderCapture CarrierBearingStrippedCapture;
		TMap<FIntVector, int32> PositionKeyToUE1;
		if (!WoTMeshLayout_CaptureCarrierBearingStrippedRenderData(
				Mesh, FrameVertexCount, ExpectedRenderTriangles,
				CarrierBearingStrippedCapture, PositionKeyToUE1, Error))
		{
			return FString::Printf(
				TEXT("carrier-bearing stripped rebuild mapping verification failed: %s"),
				*Error);
		}
		for (int32 RenderVertex = 0;
			RenderVertex < CarrierBearingStrippedCapture.Mapping.Num(); ++RenderVertex)
		{
			const int32 UE1Vertex =
				CarrierBearingStrippedCapture.Mapping[RenderVertex];
			// Position identity is deliberately independent of UE1 identity.
			// Capture already checked the exact position-key round trip; retain a
			// membership guard here alongside the canonical tangent check.
			// A degenerate co-UE1 render vertex carries a ZERO stripped normal
			// (canonical welded it away); its basis rides the runtime delta,
			// so skip the equality there — same zero-basis ruling as the
			// pinned-base validation and the BSP proof.
			const FVector StrippedTangent =
				CarrierBearingStrippedCapture.Tangents[RenderVertex];
			const bool bDegenerateBasis =
				StrippedTangent.SquaredLength() <= WoTMeshLayout_NormalTolerance;
			if (!WoTMeshLayout_IsLatticePosition(
					CarrierBearingStrippedCapture.Positions[RenderVertex])
				|| !HasCanonicalTangent[UE1Vertex]
				|| !(bDegenerateBasis || DegenerateExpectedNormal[UE1Vertex]
					|| StrippedTangent.Equals(
						CanonicalTangentByUE1[UE1Vertex], WoTMeshLayout_NormalTolerance)))
			{
				return FString::Printf(
					TEXT("stripped render vertex %d did not retain lattice position and canonical TangentZ"),
					RenderVertex);
			}
		}

		// Drop TEXCOORD_1 only after phase 1 has transferred its authoritative
		// mapping into the opaque position-key table.
		Description = Mesh->GetMeshDescription(0);
		if (!Description)
		{
			return TEXT("cannot drop the stripped TEXCOORD_1 carrier without LOD-0 MeshDescription");
		}
		Mesh->ModifyMeshDescription(0);
		{
			FSkeletalMeshAttributes Attributes(*Description);
			TVertexInstanceAttributesRef<FVector2f> UVs =
				Attributes.GetVertexInstanceUVs();
			if (UVs.GetNumChannels() != 2)
			{
				return TEXT("carrier-bearing stripped MeshDescription lost TEXCOORD_1 before capture");
			}
			UVs.SetNumChannels(1);
		}
		if (!Mesh->CommitMeshDescription(0, CommitParams))
		{
			return TEXT("failed to commit the carrier-free stripped LOD-0 MeshDescription");
		}
		Mesh->Build();
		FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});
		Mesh->SetImportedBounds(ImportedBounds);

		FWoTMeshLayout_RenderCapture CarrierFreeStrippedCapture;
		if (!WoTMeshLayout_CaptureCarrierFreeStrippedRenderData(
				Mesh, FrameVertexCount, CarrierBearingStrippedCapture.Mapping,
				PositionKeyToUE1, ExpectedRenderTriangles,
				CarrierFreeStrippedCapture, Error))
		{
			return FString::Printf(
				TEXT("carrier-free stripped rebuild verification failed: %s"),
				*Error);
		}
		for (int32 RenderVertex = 0;
			RenderVertex < CarrierFreeStrippedCapture.Mapping.Num(); ++RenderVertex)
		{
			const int32 UE1Vertex =
				CarrierFreeStrippedCapture.Mapping[RenderVertex];
			const FVector StrippedTangent =
				CarrierFreeStrippedCapture.Tangents[RenderVertex];
			const bool bDegenerateBasis =
				StrippedTangent.SquaredLength() <= WoTMeshLayout_NormalTolerance;
			if (!WoTMeshLayout_IsLatticePosition(
					CarrierFreeStrippedCapture.Positions[RenderVertex])
				|| !HasCanonicalTangent[UE1Vertex]
				|| !(bDegenerateBasis || DegenerateExpectedNormal[UE1Vertex]
					|| StrippedTangent.Equals(
						CanonicalTangentByUE1[UE1Vertex],
						WoTMeshLayout_NormalTolerance)))
			{
				return FString::Printf(
					TEXT("carrier-free stripped render vertex %d did not retain per-wedge lattice membership and canonical TangentZ"),
					RenderVertex);
			}
		}
		FinalCapture = MoveTemp(CarrierFreeStrippedCapture);
		if (!Mesh->GetMorphTargets().IsEmpty())
		{
			return TEXT("stripped skeletal mesh still contains native morph targets");
		}
	}

	UWoTMeshPayloadLayout* Layout = NewObject<UWoTMeshPayloadLayout>(Mesh);
	Layout->PayloadKey = PayloadKey;
	Layout->AnimationLogicalId = EffectiveAnimationLogicalId;
	Layout->Origin = Origin;
	Layout->Scale = Scale;
	Layout->RotationUnits = RotationUnits;
	Layout->bUnmirror = bUnmirror;
	Layout->FrameVertexCount = FrameVertexCount;
	Layout->AnimationFrameCount = AnimationFrameCount;
	Layout->SmoothTriangles = SmoothTriangles;
	Layout->RenderVertexToUE1Vertex = FinalCapture.Mapping;
	Layout->PinnedBaseTangentZ = FinalCapture.Tangents;
	Layout->PinnedBaseTangentZSHA256 =
		UWoTMeshPayloadLayout::HashTangentBasis(Layout->PinnedBaseTangentZ);
	Layout->bRenderVertexMappingVerified = true;
	Layout->bStrippedBasePositions = bStripPayload;
	Layout->bTangentRecomputeDisabled = bStripPayload;
	if (bStripPayload && !Layout->ValidateDescriptor(Error))
	{
		return FString::Printf(
			TEXT("generated stripped mesh layout is invalid: %s"), *Error);
	}
	Mesh->RemoveUserDataOfClass(UWoTMeshPayloadLayout::StaticClass());
	Mesh->AddAssetUserData(Layout);
	Mesh->MarkPackageDirty();
	return FString();
#else
	return TEXT("mesh payload capture is editor-only");
#endif
}
