#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/FrontEndWorld.h"
#if WITH_AUTOMATION_TESTS
#include "WoTFrontEndSceneController.h"
#endif

class AActor;
class APlayerController;
class FWoTCacheGenerationLease;
class FWoTFrontEndFontOwner;
class FWoTRuntimeWorldResource;
class UWoTPayloadSubsystem;
class UWorld;
struct FWoTBSPPayloadData;
struct FWoTNeutralResourceId;

struct FWoTFrontEndScenePreferences
{
	float AudioVolume = 1.0f;
	bool bReducedMotion = false;
};

enum class EWoTFrontEndSceneProgressStage : uint8
{
	WorldAndBSP,
	SurfacesAndLightmaps,
	WeatherAndAmbience,
	FontsAndLocalization,
	Commit
};

struct FWoTFrontEndSceneProgress
{
	EWoTFrontEndSceneProgressStage Stage =
		EWoTFrontEndSceneProgressStage::WorldAndBSP;
	uint32 CompletedUnits = 0;
	uint32 TotalUnits = 5;
};

struct FWoTFrontEndSceneStagedCensus
{
	uint32 BSPSurfaces = 0;
	uint32 Lights = 0;
	uint32 AmbientSounds = 0;
	uint32 SplashEmitters = 0;
	uint32 RainCylinders = 0;
	uint32 LightningActors = 0;
	uint32 RandomLightningControllers = 0;
	uint32 WaterSurfaces = 0;
};

struct FWoTFrontEndSceneObservation
{
	uint32 ObservableActors = 0;
	uint32 VisibleComponents = 0;
	uint32 TickingActors = 0;
	uint32 CollidingComponents = 0;
	uint32 AudibleComponents = 0;
	uint32 AnimatedWeatherComponents = 0;
	uint32 MusicComponents = 0;
	uint32 AuthenticatedBSPRenderers = 0;
	uint32 BoundBSPMaterialSections = 0;
	uint32 BoundWaterMaterials = 0;
	uint32 AuthoredSplashRenderers = 0;
	uint32 AuthenticatedRainRenderers = 0;
	uint32 RegisteredComponents = 0;
	uint32 PendingDestroyActors = 0;
	uint32 PendingDestroyComponents = 0;
	uint32 PlayingAmbientLoops = 0;
	float FirstAmbientVolume = 0.0f;
	uint32 LightningEndpointCount = 0;
	uint32 PreservedLightningEndpointProperties = 0;
	float MinLightningSeconds = 0.0f;
	float MaxLightningSeconds = 0.0f;
	float NextLightningSeconds = 0.0f;
	int32 NearThunderSoundIndex = INDEX_NONE;
	int32 FarThunderSoundIndex = INDEX_NONE;
	FString NearThunderSoundIdentity;
	FString FarThunderSoundIdentity;
	float FirstRainInitialYaw = 0.0f;
	float FirstRainFallRate = 0.0f;
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	float HorizontalFov = 0.0f;
	bool bMaintainHorizontalFov = false;
	bool bConstrainAspectRatio = true;
	FString SnapshotIdentity;
};

class FWoTFrontEndSceneMaterializer;
struct FWoTFrontEndSceneOwnerState;

class WOT_API FWoTFrontEndSceneStaging final
{
public:
	FWoTFrontEndSceneStaging(FWoTFrontEndSceneStaging&& Other);
	FWoTFrontEndSceneStaging& operator=(FWoTFrontEndSceneStaging&& Other);
	~FWoTFrontEndSceneStaging();
	FWoTFrontEndSceneStaging(const FWoTFrontEndSceneStaging&) = delete;
	FWoTFrontEndSceneStaging& operator=(const FWoTFrontEndSceneStaging&) = delete;

	FWoTBootstrapStatus Commit(APlayerController* ViewOwner = nullptr) &&;
	void Rollback();
#if WITH_AUTOMATION_TESTS
	FWoTFrontEndSceneStagedCensus StagedCensusForTesting() const;
#endif

private:
	FWoTFrontEndSceneStaging(
		TWeakPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> InOwner,
		uint64 InSerial);
	TWeakPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> Owner;
	uint64 Serial = 0;
	friend class FWoTFrontEndSceneMaterializer;
};

#if WITH_AUTOMATION_TESTS
struct FWoTFrontEndSceneTestFixture
{
	FString SnapshotIdentity;
	FString FontSnapshotIdentity;
	uint32 BSPSurfaceCount = 0;
	uint32 WaterSurfaceCount = 0;
	uint32 PositionalAmbienceCount = 0;
	bool bMalformedBSP = false;
	bool bMissingSurface = false;
	bool bCorruptSound = false;
	bool bCorruptFont = false;
	bool bCancelled = false;
};
#endif

/** Transactional owner for the payload-built retail entry scene. Stage never
 * publishes an actor/component/delegate/audio source. Commit is the sole
 * game-thread publication point; every error path calls Rollback. */
class WOT_API FWoTFrontEndSceneMaterializer final
{
public:
	FWoTFrontEndSceneMaterializer();
	~FWoTFrontEndSceneMaterializer();
	FWoTFrontEndSceneMaterializer(const FWoTFrontEndSceneMaterializer&) = delete;
	FWoTFrontEndSceneMaterializer& operator=(
		const FWoTFrontEndSceneMaterializer&) = delete;

	TWoTBootstrapResult<FWoTFrontEndSceneStaging> Stage(
		UWorld& World, const FWoTFrontEndDescriptor& Descriptor,
		const FWoTCacheGenerationLease& WorldLease,
		UWoTPayloadSubsystem& Payload, const FWoTFrontEndFontOwner& FontOwner,
		const FWoTFrontEndScenePreferences& Preferences,
		TFunction<bool()> IsCancelled,
		TFunction<void(const FWoTFrontEndSceneProgress&)> Progress = {});
	void Rollback();
	void SuspendBeforeTravel();
	FWoTBootstrapStatus Rematerialize(UWorld& World,
		APlayerController* ViewOwner = nullptr);
	void Shutdown();
	bool IsCommitted() const;
	bool IsSuspendedForWorld(const UWorld& World) const;

#if WITH_AUTOMATION_TESTS
	TWoTBootstrapResult<FWoTFrontEndSceneStaging> StageForTesting(
		UWorld& World, const FWoTFrontEndDescriptor& Descriptor,
		const FWoTFrontEndSceneTestFixture& Fixture,
		const FWoTFrontEndScenePreferences& Preferences);
	FWoTFrontEndSceneObservation ObserveForTesting() const;
	TSharedPtr<FWoTFrontEndSceneController> ControllerForTesting() const;
	UTexture2D* GenericTextureForTesting(FStringView SemanticIdentity) const;
	void SetAtlasUploaderForTesting(FWoTEntryAtlasUploader Uploader);
	FWoTBootstrapStatus RematerializeForTesting(UWorld& World);
	static void FailNextCommitForTesting();
	static void FailAfterHiddenConstructionForTesting();
	static void FailAfterControllerPreparationForTesting();
	static void CancelNextCommitForTesting();
	static void RevokeAfterHiddenConstructionForTesting();
	static void RevokeAfterControllerPreparationForTesting();
	static void RevokeDuringPublicationForTesting();
	static void SetPublicationBoundaryObserverForTesting(
		TFunction<void(const TCHAR*, const FWoTFrontEndSceneObservation&)> Observer);
	static void SetResourceStageObserverForTesting(
		TFunction<void(EWoTFactDomain, int32)> Observer);
	static bool IsPublicationGuardActiveForTesting();
	static int32 ResolveConverterSurfaceSlotForTesting(FStringView MaterialSlot,
		TConstArrayView<FString> AuthenticatedSourceExports);
	void RevokeAuthorityForTesting();
	static void RevokeLeaseForTesting(const FWoTCacheGenerationLease& Lease);
#endif

private:
	struct FPlan;
	FWoTBootstrapStatus CommitStaged(uint64 Serial, APlayerController* ViewOwner);
	void RollbackStaged(uint64 Serial);
	void HandleLeaseRevoked(uint64 AuthoritySerial);
	void RestoreViewTarget();
	void HidePublishedScene();
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	FWoTFrontEndSceneStagedCensus StagedCensus(uint64 Serial) const;
	TUniquePtr<FPlan> Plan;
	TWeakObjectPtr<AActor> ActiveRoot;
	TWeakObjectPtr<UWorld> ObservationWorld;
	TArray<TWeakObjectPtr<AActor>> ActiveWeatherActors;
	uint64 NextSerial = 1;
	uint64 StagedSerial = 0;
	TSharedRef<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> OwnerState;
	FDelegateHandle WorldCleanupHandle;
#if WITH_AUTOMATION_TESTS
	FWoTEntryAtlasUploader TestAtlasUploader;
#endif
	friend class FWoTFrontEndSceneStaging;
};

/** The entry world decode that staging runs first: the descriptor's world
 * object, which must carry the descriptor's map identity. */
WOT_API FWoTBootstrapStatus WoTFrontEndSceneLoadWorld(
	const FWoTFrontEndDescriptor& Descriptor,
	FWoTRuntimeWorldResource& WorldResource);

/** The entry BSP decode that staging runs for the descriptor's BSP object. */
WOT_API FWoTBootstrapStatus WoTFrontEndSceneDecodeBSP(
	const FWoTNeutralResourceId& Id, FWoTBSPPayloadData& OutBSP);
