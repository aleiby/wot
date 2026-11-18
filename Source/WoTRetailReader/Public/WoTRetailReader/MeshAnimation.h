// Bounded cache-v3 import of UE1 vertex meshes and their frame animations.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

struct FWoTVertexMeshUV
{
	uint8 U = 0;
	uint8 V = 0;
};

struct FWoTVertexMeshTriangle
{
	uint32 VertexIndices[3] = {};
	TOptional<uint32> UVIndices[3];
	uint32 RenderVertexIndices[3] = {};
	uint32 SectionIndex = 0;
	TOptional<uint32> PolyFlags;
	TOptional<int32> TextureIndex;
	bool bLodAttachment = false;
};

struct FWoTVertexMeshSection
{
	uint32 FirstTriangle = 0;
	uint32 TriangleCount = 0;
	TOptional<uint32> PolyFlags;
	TOptional<uint32> SkinIndex;
	bool bLodAttachment = false;
};

struct FWoTSemanticSkinId
{
	TOptional<FString> Package;
	TOptional<FString> Name;
	TOptional<FString> LogicalId;
};

struct FWoTVertexMeshRenderVertex
{
	uint32 SourceVertex = 0;
	TOptional<uint32> UVIndex;
	TOptional<uint32> SkinIndex;
};

struct FWoTVertexMeshBounds
{
	FVector3f Min = FVector3f::ZeroVector;
	FVector3f Max = FVector3f::ZeroVector;
};

struct FWoTVertexMeshNotify
{
	float Time = 0.0f;
	FString Function;
};

struct FWoTVertexMeshSequence
{
	FString Name;
	uint32 FirstFrame = 0;
	uint32 NumFrames = 0;
	float Rate = 0.0f;
	FString Group;
	TArray<FWoTVertexMeshNotify> Notifies;
};

struct FWoTVertexMeshObjectHeader
{
	uint32 VertexCount = 0;
	uint32 TriangleCount = 0;
	uint32 FrameCount = 0;
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f Scale = FVector3f(1.0f, 1.0f, 1.0f);
	// Serialized UE1 order/units: Pitch, Yaw, Roll in 1/65536 turns.
	int32 RotationOrigin[3] = {};
	TArray<FWoTVertexMeshTriangle> Triangles;
	TArray<FWoTVertexMeshUV> UVs;
	TArray<FWoTVertexMeshSection> Sections;
	TArray<FWoTSemanticSkinId> Skins;

	FString ClassName;
	TArray<FWoTVertexMeshRenderVertex> RenderVertices;
	TArray<uint32> CollisionTriangles;
	FWoTVertexMeshBounds Bounds;
	TArray<FWoTVertexMeshSequence> Sequences;
	TOptional<uint32> LodModelVertices;
	TOptional<uint32> LodSpecialVertices;
	TOptional<uint32> LodOldFrameVertices;
	TOptional<bool> bLodRemapPresent;
};

#if !WOT_GUARDED_BOOTSTRAP
/** Parsed development package used for lazy, export-scoped mesh sequences. */
class WOTRETAILREADER_API FWoTDevelopmentMeshPackage final
{
public:
	static TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentMeshPackage>> Parse(
		TConstArrayView<uint8> SourceBytes, FStringView SourcePath);
	~FWoTDevelopmentMeshPackage();

	bool FindMeshSequences(FStringView SourceExport,
		TArray<FWoTVertexMeshSequence>& Out) const;
	/** Logical vertices omitted by triangle-based asset import, in mesh-local units. */
	bool FindUnreferencedMeshFrames(FStringView SourceExport,
		TArray<TArray<FVector>>& Out) const;

private:
	struct FImpl;
	explicit FWoTDevelopmentMeshPackage(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
};
#endif

WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportMeshAnimation(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

#if WITH_AUTOMATION_TESTS
/** The real-retail pin uses the same importer core without wrapping a .u in Inno. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportMeshAnimationPackageBytesForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress);
#endif
