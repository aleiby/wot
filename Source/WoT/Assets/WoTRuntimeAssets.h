#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ObjectKey.h"
#include "WoTRuntimeAssets.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class USoundBase;
class UTexture2D;
class UWoTPayloadSoundWave;
class UWoTPayloadSubsystem;
#if WITH_AUTOMATION_TESTS
class FWoTRuntimeAssetReleaseLifetimeSpec;
#endif
struct FWoTNeutralResourceId;
struct FWoTTexturePayloadData;

enum class EWoTRuntimeAssetKind : uint8
{
	Sound,
	Texture,
};

/** Rendering choices are part of the cache identity of a transient texture. */
struct WOT_API FWoTRuntimeTextureOptions
{
	bool bLoadMips = false;
	bool bSRGB = true;
	TextureFilter Filter = TF_Nearest;
	int32 LODBias = 0;
	TextureAddress AddressX = TA_Wrap;
	TextureAddress AddressY = TA_Wrap;
	/** Mesh STY_Masked always keys palette index zero, independent of Texture.bMasked. */
	bool bForcePaletteIndexZeroTransparent = false;

	bool operator==(const FWoTRuntimeTextureOptions& Other) const
	{
		return bLoadMips == Other.bLoadMips && bSRGB == Other.bSRGB
			&& Filter == Other.Filter && LODBias == Other.LODBias
			&& AddressX == Other.AddressX
			&& AddressY == Other.AddressY
			&& bForcePaletteIndexZeroTransparent
				== Other.bForcePaletteIndexZeroTransparent;
	}
};

/** Header-shaped P8 payload materialization shared by gameplay consumers/specs. */
WOT_API UTexture2D* WoTRuntimeMaterializeTexture(
	UObject* Outer, const FWoTTexturePayloadData& Payload,
	const FWoTRuntimeTextureOptions& Options);

/** Separate outer lets both (id, loop) waves retain the exact retail FName. */
UCLASS(Transient)
class UWoTRuntimeAssetContainer final : public UObject
{
	GENERATED_BODY()
};

/**
 * Cache-backed gameplay presentation owned by the game instance. It remains
 * dormant until the authenticated guarded payload bootstrap is active; normal
 * developer builds continue to load their cooked soft-object paths.
 */
UCLASS()
class WOT_API UWoTRuntimeAssets final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UWoTPayloadSoundWave* ResolveSound(
		const FSoftObjectPath& Path, bool bLoop);
	UTexture2D* ResolveTexture(
		const FSoftObjectPath& Path,
		const FWoTRuntimeTextureOptions& Options = {});
	/** A texture identity served from the procedural domain: the placeholder
	    still frame the fractal subsystem later replaces with the live sim. */
	UTexture2D* ResolveProceduralPlaceholder(const FString& Requested);
	UMaterialInterface* ResolveDerivedMaterial(
		const FSoftObjectPath& Path);

	bool GetPalette(const UTexture2D* Texture, TArray<FColor>& OutPalette) const;
	bool GetKeyColor(const UTexture2D* Texture, FColor& OutKeyColor) const;

	/** Dev/cooked fallback plus guarded routing for context-free gameplay code. */
	static USoundBase* ResolveSoundPath(
		const FSoftObjectPath& Path, bool bLoop = false);
	static UTexture2D* ResolveTexturePath(
		const FSoftObjectPath& Path,
		const FWoTRuntimeTextureOptions& Options = {});
	static UMaterialInterface* ResolveMaterialPath(const FSoftObjectPath& Path);
	/** Direct STY_Modulated sprite binding: authenticated raw texture on the
	    neutral, closure-pinned FB=2*Tex*Dst master. */
	static UMaterialInstanceDynamic* CreateModulatedSpriteMaterial(
		UObject* Outer, const FSoftObjectPath& TexturePath,
		FName RuntimeIdentity = NAME_None);
	/** Palette[0] for a transient cache texture; false for ordinary cooked assets. */
	static bool ResolveKeyColor(const UTexture2D* Texture, FColor& OutKeyColor);
	/** Texture.bMasked recorded while materializing an authenticated payload. */
	static bool ResolveTextureMasked(const UTexture2D* Texture);
	/** Importer-compatible raw palette bytes; cache mesh textures are non-sRGB. */
	static FLinearColor PaletteColorToMaterialColor(const FColor& Color);
	/** Rebinds context-free resolution to the authenticated payload owner. */
	static void RefreshProviderBinding();

	/** Exact path rule plus case-insensitive, ambiguity-rejecting registry match. */
	static bool ResolveCacheId(
		FStringView Path, EWoTRuntimeAssetKind Kind,
		TConstArrayView<FWoTNeutralResourceId> Resources,
		FString& OutLogicalId, FString* OutFailure = nullptr);
	/** Cache identities split at the first separator; retail names may contain it. */
	static bool SplitCacheIdentity(
		FStringView LogicalId, EWoTRuntimeAssetKind Kind,
		FString& OutPackage, FString& OutName);

	/** Called by the payload owner before it revokes/releases its cache lease. */
	void ShutdownForPayloadDeactivation();

#if WITH_AUTOMATION_TESTS
	int32 SoundCacheSizeForTesting() const { return SoundCache.Num(); }
	int32 TextureCacheSizeForTesting() const { return TextureCache.Num(); }
	void ResetTravelStatsForTesting();
	void RecordResolutionForTesting(bool bSound, bool bSuccess, FStringView Path)
	{
		RecordResolution(bSound, bSuccess, Path);
	}
	FString TravelSummaryForTesting() const;
	static void ResetProvidersForTesting();
	static void RegisterProviderForTesting(UWoTRuntimeAssets* Provider);
	static void RemoveProviderForTesting(UWoTRuntimeAssets* Provider);
	static void RefreshProviderBindingForTesting(
		UWoTPayloadSubsystem* AuthenticatedOwner);
	static UWoTRuntimeAssets* ProviderForTesting();
	/** Opt an editor automation scope into packaged texture/material routing. */
	static void SetPayloadTextureResolutionForTesting(bool bEnabled);
#endif

private:
#if WITH_AUTOMATION_TESTS
	friend class FWoTRuntimeAssetReleaseLifetimeSpec;
#endif
	void HandlePostWorldInitialization(
		UWorld* World, const UWorld::InitializationValues IVS);
	void HandleWorldTickStart(
		UWorld* World, enum ELevelTick TickType, float DeltaSeconds);
	void HandleWorldCleanup(
		UWorld* World, bool bSessionEnded, bool bCleanupResources);
	bool FinalizeTravelSummary(UWorld* ExpectedWorld);
	bool AdvanceTravelSummaryClock(bool bWorldHasBegunPlay, double RealTimeSeconds);
	void RecordResolution(bool bSound, bool bSuccess, FStringView Path);
	void RecordMaterialFailure(FStringView Path);
	FString TravelSummary() const;
	void ReleaseRuntimeObjects();
	static void RefreshProviderBindingWithOwner(
		UWoTPayloadSubsystem* AuthenticatedOwner);

	TMap<FString, TObjectPtr<UWoTPayloadSoundWave>> SoundCache;
	TMap<FString, TObjectPtr<UTexture2D>> TextureCache;
	TMap<FString, TObjectPtr<UMaterialInterface>> MaterialCache;
	TMap<FObjectKey, TArray<FColor>> TexturePalettes;
	TMap<FObjectKey, bool> TextureMasked;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UWoTRuntimeAssetContainer>> RuntimeOuters;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWoTPayloadSoundWave>> RuntimeSounds;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> RuntimeTextures;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> RuntimeMaterials;

	FDelegateHandle PostWorldInitializationHandle;
	FDelegateHandle WorldTickStartHandle;
	FDelegateHandle WorldCleanupHandle;
	TWeakObjectPtr<UWorld> TravelWorld;
	bool bTravelSummaryPending = false;
	bool bTravelSummaryClockStarted = false;
	double TravelSummaryStartRealSeconds = 0.0;
	bool bShutdown = false;
	int32 ResolvedSounds = 0;
	int32 FailedSounds = 0;
	int32 ResolvedTextures = 0;
	int32 FailedTextures = 0;
	int32 FailedMaterials = 0;
	TArray<FString> FirstFailures;
#if WITH_AUTOMATION_TESTS
	int32 FinalTravelSummaryEmissionsForTesting = 0;
#endif
};
