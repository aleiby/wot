#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "../WoTVertexAnimComponent.h"

struct FWoTNeutralResourceId;
class FWoTRuntimeVerifiedObject;

enum class EWoTRuntimeVertexMeshStyle : uint8
{
	Normal,
	TwoSided,
	Masked,
	Translucent,
	Modulated,
};

struct WOT_API FWoTRuntimeVertexMeshTriangle
{
	uint32 SourceVertices[3] = {};
	uint32 RenderVertices[3] = {};
	int32 UVIndices[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
	int32 SectionIndex = INDEX_NONE;
	int32 TextureIndex = INDEX_NONE;
	uint32 PolyFlags = 0;
	bool bHasMaterial = false;
	bool bLodAttachment = false;
};

struct WOT_API FWoTRuntimeVertexMeshRenderVertex
{
	uint32 SourceVertex = 0;
	int32 UVIndex = INDEX_NONE;
	int32 SkinIndex = INDEX_NONE;
};

struct WOT_API FWoTRuntimeVertexMeshSlot
{
	FName Name;
	int32 TextureIndex = INDEX_NONE;
	FString TexturePackage;
	FString TextureName;
	FString TextureLogicalId;
	EWoTRuntimeVertexMeshStyle Style = EWoTRuntimeVertexMeshStyle::Normal;
	bool bUnlit = false;
	TArray<uint32> Indices;
};

struct WOT_API FWoTRuntimeVertexMeshPose
{
	TArray<FVector3f> SourcePositions;
	TArray<FVector3f> RenderPositions;
	TArray<FVector3f> RenderNormals;
	TArray<FVector3f> RenderTangents;
};

class FWoTRuntimeVertexMeshData;
using FWoTRuntimeVertexMeshDataPtr =
	TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>;

/** Immutable decoded WOTMSH3/WOTANI3 pair with lease-scoped lazy frame poses. */
class WOT_API FWoTRuntimeVertexMeshData final
{
public:
	const FString& GetMeshLogicalId() const { return MeshLogicalId; }
	const FString& GetAnimationLogicalId() const { return AnimationLogicalId; }
	FName GetMeshName() const { return MeshName; }
	FName GetMeshPackage() const { return MeshPackage; }
	int32 GetSourceVertexCount() const { return SourceVertexCount; }
	int32 GetPositionVertexCount() const
	{
		return SourceVertexCount + DerivedParents.Num();
	}
	int32 GetRenderVertexCount() const { return RenderVertices.Num(); }
	int32 GetFrameCount() const { return FrameCount; }
	int32 GetCurvyLevels() const { return CurvyLevels; }
	bool IsLodMesh() const { return bLodMesh; }
	const FBox& GetBounds() const { return Bounds; }
	const TArray<FWoTRuntimeVertexMeshTriangle>& GetTriangles() const
	{
		return Triangles;
	}
	const TArray<FWoTRuntimeVertexMeshRenderVertex>& GetRenderVertices() const
	{
		return RenderVertices;
	}
	const TArray<FVector2f>& GetRenderUVs() const { return RenderUVs; }
	const TArray<FWoTRuntimeVertexMeshSlot>& GetSlots() const { return Slots; }
	const TArray<FUintVector2>& GetDerivedParents() const { return DerivedParents; }
	const TArray<FWoTAnimSequence>& GetSequences() const { return Sequences; }

	/** FrameCount is the deliberate UE1 terminal sentinel and aliases frame zero. */
	const FWoTRuntimeVertexMeshPose* GetFramePose(int32 Frame) const;
	bool BlendPose(const TMap<int32, float>& Weights,
		FWoTRuntimeVertexMeshPose& OutPose) const;

private:
	FString MeshLogicalId;
	FString AnimationLogicalId;
	FName MeshName;
	FName MeshPackage;
	int32 SourceVertexCount = 0;
	int32 FrameCount = 0;
	int32 CurvyLevels = 0;
	bool bLodMesh = false;
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f Scale = FVector3f(1.0f);
	int32 RotationOrigin[3] = {};
	FBox Bounds = FBox(ForceInit);
	TArray<FWoTRuntimeVertexMeshTriangle> Triangles;
	TArray<FWoTRuntimeVertexMeshRenderVertex> RenderVertices;
	TArray<FVector2f> RenderUVs;
	TArray<FWoTRuntimeVertexMeshSlot> Slots;
	TArray<FUintVector2> DerivedParents;
	TArray<FWoTAnimSequence> Sequences;
	TArray<uint32> PackedFrames;

	mutable FCriticalSection PoseGuard;
	mutable TArray<TUniquePtr<FWoTRuntimeVertexMeshPose>> FramePoses;

	friend class FWoTRuntimeVertexMesh;
};

/** Registry resolver, strict decoder, and generation-bound shared cache. */
class WOT_API FWoTRuntimeVertexMesh final
{
public:
	static bool ResolveLogicalIds(FStringView MeshName, FName PackageHint,
		TConstArrayView<FWoTNeutralResourceId> Resources,
		FString& OutMeshLogicalId, FString& OutAnimationLogicalId,
		FString* OutFailure = nullptr);

	static FWoTRuntimeVertexMeshDataPtr Load(FStringView MeshLogicalId,
		FStringView AnimationLogicalId);
	static FWoTRuntimeVertexMeshDataPtr ResolveAndLoad(FStringView MeshName,
		FName PackageHint = NAME_None);
	static FWoTRuntimeVertexMeshDataPtr MakeCurvy(
		const FWoTRuntimeVertexMeshDataPtr& Source, int32 Levels);
	static void ResetRuntimeCache();

#if WITH_AUTOMATION_TESTS
	static FWoTRuntimeVertexMeshDataPtr MakeSyntheticForTesting(
		TConstArrayView<FIntVector> SourcePositions,
		TConstArrayView<FIntVector> SourceTriangles,
		TConstArrayView<uint32> RenderSourceVertices);
	static void EvaluateCurvyMidpointForTesting(
		const FVector3d& PositionA, const FVector3d& PositionB,
		const FVector3d& NormalA, const FVector3d& NormalB,
		FVector3d& OutPosition, FVector3d& OutNormal);
	static TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>
		DecodeObjectBytesForTesting(TArray<uint8> MeshObject,
			TArray<uint8> AnimationObject, FStringView MeshLogicalId,
			FStringView AnimationLogicalId);
#endif

private:
	static TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr> Decode(
		FWoTRuntimeVerifiedObject&& MeshObject,
		FWoTRuntimeVerifiedObject&& AnimationObject,
		FStringView MeshLogicalId, FStringView AnimationLogicalId);
};
