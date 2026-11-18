#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WoTMeshPayloadLayout.generated.h"

class USkeletalMesh;

// Trusted, generated structure which connects a retail UE1 mesh payload to the
// cooked LOD-0 render vertices. Payload bytes never live here: topology, the
// post-Interchange render mapping, and the pinned cooked tangent basis do.
UCLASS(BlueprintType, DefaultToInstanced, EditInlineNew)
class UWoTMeshPayloadLayout : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FString PayloadKey;

	// Exact cache-v3 semantic object id for the sequence/notify table paired
	// with this mesh. Guarded runtime never infers it from an asset name.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FString AnimationLogicalId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FVector Origin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FVector Scale = FVector(0.1, 0.1, 0.2);

	// UE1 #exec rotation units: Pitch, Yaw, Roll, each in 1/256 turns.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FVector RotationUnits = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	bool bUnmirror = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	int32 FrameVertexCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	int32 AnimationFrameCount = 0;

	// Flat UE1 vertex-index triples. This includes invisible placeholder
	// triangles because the retail smooth-normal linkup includes them.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	TArray<int32> SmoothTriangles;

	// Indexed by the final stripped LOD-0 render vertex. Phase 1 maps each opaque
	// per-wedge lattice position to its TEXCOORD_1 UE1 identity; phase 2 transfers
	// that mapping by position after dropping the carrier.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	TArray<int32> RenderVertexToUE1Vertex;

	// The equivalence-selected cooked base TangentZ in final render-vertex
	// order. Injected normal deltas are always FrameNormal - this value.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	TArray<FVector> PinnedBaseTangentZ;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FString PinnedBaseTangentZSHA256;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	bool bRenderVertexMappingVerified = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	bool bStrippedBasePositions = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	bool bTangentRecomputeDisabled = false;

	bool ValidateDescriptor(FString& OutError) const;
	bool ValidatePinnedTangentHash(FString& OutError) const;
	static FString HashTangentBasis(TConstArrayView<FVector> Tangents);
	// Exact-order gate for the canonical COLOR_0 and independently decoded
	// TEXCOORD_1 render carriers.
	static bool ValidateRenderVertexMappingAgreement(
		TConstArrayView<int32> RenderMapping,
		TConstArrayView<int32> UVDecodedMapping,
		FString& OutError);
};

// Stripped-import-only bridge. Interchange is allowed to reorder and duplicate
// GLB corners, so Python cannot author RenderVertexToUE1Vertex from source
// order. This captures a per-wedge position-key mapping while TEXCOORD_1 is
// present, then removes the carrier and transfers the mapping by position in a
// second build.
UCLASS()
class UWoTMeshPayloadLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Generated meshes sampled by CPU vertex effects must opt in before cook;
	// setting this at runtime is too late once render buffers were stripped.
	UFUNCTION(BlueprintCallable, Category="WoT Payload")
	static bool SetLOD0AllowCPUAccess(
		USkeletalMesh* Mesh, bool bAllowCPUAccess = true);

	UFUNCTION(BlueprintCallable, Category="WoT Payload")
	static FString CaptureAndOptionallyStrip(
		USkeletalMesh* Mesh,
		const FString& PayloadKey,
		FVector Origin,
		FVector Scale,
		FVector RotationUnits,
		bool bUnmirror,
		int32 FrameVertexCount,
		int32 AnimationFrameCount,
		const TArray<int32>& SmoothTriangles,
		const TArray<int32>& ExpectedRenderTriangles,
		const TArray<FVector>& ExpectedBasePositions,
		const TArray<FVector>& ExpectedBaseNormals,
		bool bStripPayload,
		const FString& AnimationLogicalId = FString());
};
