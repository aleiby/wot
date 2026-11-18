#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WoTBSPPayload.generated.h"

class UStaticMesh;

// One contiguous material section in both the authenticated payload and the
// generated LOD0.  Ranges are kept explicit so no untrusted input can make the
// renderer or collision cooker infer past an allocation.
struct FWoTBSPPayloadSection
{
	FString MaterialSlot;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	uint32 MinVertex = 0;
	uint32 MaxVertex = 0;
	bool bCollisionEnabled = false;
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	uint32 SurfaceFlags = 0;
	FString PanZoneActorIdentity;
	// Version 10: canonical macro-texture identity and exact authored UV transform.
	FString MacroCanonicalPackage;
	FString MacroCanonicalName;
	FString MacroSourceTextureIdentity;
	float MacroRatio = 0.0f;
	float MacroOffsetU = 0.0f;
	float MacroOffsetV = 0.0f;
};

struct FWoTEntrySurfaceRecord
{
	uint32 SurfaceIndex = 0;
	uint32 X = 0;
	uint32 Y = 0;
	uint32 W = 0;
	uint32 H = 0;
	FVector2f PanUV = FVector2f::ZeroVector;
};

struct FWoTEntryLightRecord
{
	FString Identity;
	uint8 Brightness = 64;
	uint8 Hue = 0;
	uint8 Saturation = 255;
	float LevelBrightness = 1.0f;
};

struct FWoTEntryIlluminationRecord
{
	uint32 LightIndex = 0;
	uint32 SurfaceIndex = 0;
	TArray<uint8> Illumination;
};

/** Atlas placement of one model surface (interior origin/size; zero when the
    surface has no light map). Version-5+ animation trailer. */
struct FWoTBSPAnimationSurface
{
	uint32 X = 0;
	uint32 Y = 0;
	uint32 W = 0;
	uint32 H = 0;
};

/** One waveform group: UE1 LightType (2 Pulse, 3 Blink, 7 SubtlePulse), LightPeriod,
    LightPhase, in the material Wave0..Wave3 order. */
struct FWoTBSPAnimationGroup
{
	uint8 Type = 0;
	uint8 Period = 0;
	uint8 Phase = 0;
	uint32 LightCount = 0;
};

/** Unsigned scale-2 RGB centered response of one lit surface. */
struct FWoTBSPAnimationRect
{
	uint32 SurfaceIndex = 0;
	TArray<uint8> RGB;
};

/** One centered-response term: kind 0 waveform (Group), 1 shimmer effect
    (Effect 1 TorchWaver, 2 FireWaver, 3 WateryShimmer), 2 intersection. */
struct FWoTBSPAnimationTerm
{
	uint8 Kind = 0;
	uint8 Group = 255;
	uint8 Effect = 0;
	uint32 LightCount = 0;
	TArray<FWoTBSPAnimationRect> Rects;
};

/** Version-6/8 sky-rest census row: skybake.py bake_rest ray counts. */
struct FWoTBSPSkyRestSurface
{
	uint32 Surface = 0;
	uint32 VisibleRays = 0;
	uint32 BlockerRays = 0;
	uint32 FakeBackdropRays = 0;
};

enum class EWoTBSPSkyRestRole : uint8
{
	Visible = 1,
	Blocker = 2,
	FakeBackdrop = 3,
};

/** One skygeo.py rest group: "SkyRest__<Slot>" (visible, drawn with the world
 * slot's material), "SkyRestBlock" (flipped opaque black blockers) or
 * "SkyRestFB" (hidden nested sky windows). Three vertices per triangle, world
 * space, the world encoder's UV0/UV1. */
struct FWoTBSPSkyRestGroup
{
	EWoTBSPSkyRestRole Role = EWoTBSPSkyRestRole::Visible;
	FString Slot;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UV0;
	TArray<FVector2f> UV1;
};

/** The version-6/8 sky-rest trailer (flag 32) of a map's main part. */
struct FWoTBSPSkyRest
{
	bool bPresent = false;
	uint32 SkyZone = 0;
	uint32 RayCount = 0;
	uint32 CoveredRays = 0;
	TArray<FWoTBSPSkyRestSurface> Surfaces;
	TArray<FWoTBSPSkyRestGroup> Groups;
};

struct FWoTBSPPayloadData
{
	uint16 FormatVersion = 0;
	bool bPortal = false;
	bool bRootOutside = false;
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UV0;
	TArray<FVector2f> UV1;
	TArray<uint32> Indices;
	TArray<FWoTBSPPayloadSection> Sections;
	uint32 LightmapWidth = 0;
	uint32 LightmapHeight = 0;
	TArray<uint8> LightmapBGRA;
	TArray<uint32> TriangleSurfaces;
	TArray<FWoTEntrySurfaceRecord> EntrySurfaces;
	TArray<FWoTEntryLightRecord> EntryLights;
	TArray<FWoTEntryIlluminationRecord> EntryIllumination;
	// Version-5+ animated light-map responses (empty without flag 16).
	TArray<FWoTBSPAnimationSurface> AnimationSurfaces;
	TArray<FWoTBSPAnimationGroup> AnimationGroups;
	TArray<FWoTBSPAnimationTerm> AnimationTerms;
	// Version-6 sky-rest census and rest geometry (empty without flag 32).
	FWoTBSPSkyRest SkyRest;

	bool HasLightmapAnimation() const { return !AnimationTerms.IsEmpty(); }
};

/** Pure render half shared by authenticated scene materializers. It consumes
 * decoded payload memory only, builds no collision/navigation, and performs no
 * file access or cache lookup. */
WOT_API bool WoTBuildBSPRenderMeshOnly(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError);

// Trusted metadata on the tiny synthetic carrier.  It contains counts, real
// bounds and slot/range policy, but never retail vertex or index streams.
UCLASS(BlueprintType, DefaultToInstanced, EditInlineNew)
class UWoTBSPPayloadAssetUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	FString PayloadKey;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	FString Role;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	int32 PayloadFormatVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	FString TangentAlgorithm = TEXT("ue-mikk-preserve-normals-v1");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	int32 VertexCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	int32 IndexCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	FVector BoundsMax = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<FString> MaterialSlots;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<int32> SectionFirstIndices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<int32> SectionIndexCounts;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<int32> SectionMinVertices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<int32> SectionMaxVertices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	TArray<bool> SectionCollisionEnabled;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT BSP Payload")
	bool bSyntheticCarrier = true;

	bool ValidateDescriptor(const UStaticMesh* Mesh, FString& OutError) const;
};

UCLASS()
class UWoTBSPPayloadLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Import-commandlet bridge. Empty means success; a nonempty string is fatal.
	UFUNCTION(BlueprintCallable, Category="WoT BSP Payload")
	static FString StampCarrier(
		UStaticMesh* Mesh,
		const FString& PayloadKey,
		const FString& Role,
		int32 VertexCount,
		int32 IndexCount,
		FVector BoundsMin,
		FVector BoundsMax,
		const FString& TangentAlgorithm,
		const TArray<FString>& MaterialSlots,
		const TArray<int32>& SectionFirstIndices,
		const TArray<int32>& SectionIndexCounts,
		const TArray<int32>& SectionMinVertices,
		const TArray<int32>& SectionMaxVertices,
		const TArray<bool>& SectionCollisionEnabled);

	// Pure key seam used by headless automation. The runtime passes its full
	// engine build identity and platform name; every field is framed before SHA.
	static FString BuildDerivedCacheKeyForInputs(
		const FString& PayloadSHA256,
		const FString& EngineBuildIdentity,
		const FString& PlatformName,
		int32 DerivedFormatVersion = 1,
		const FString& TangentAlgorithm = TEXT("ue-mikk-preserve-normals-v1"));

	static bool DecodePayload(TConstArrayView<uint8> Bytes,
		FWoTBSPPayloadData& OutData, FString& OutError);
};
