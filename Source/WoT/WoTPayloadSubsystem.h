#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ObjectKey.h"
#include "Payload/WoTCacheLease.h"
#include "Payload/WoTFrontEndFontOwner.h"
#include "Payload/WoTRuntimeRegistry.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "WoTPayloadSubsystem.generated.h"

class UTexture2D;
class UPackage;
class USkeletalMesh;
class UStaticMesh;
class UBodySetup;
class UWorld;
class UWoTMeshPayloadLayout;
class UWoTBSPPayloadAssetUserData;
class UWoTPayloadSoundWave;
class UWoTVertexAnimComponent;
struct FExternalMorphSet;
struct FWoTBSPPayloadData;
struct FWoTRetailTextureFacts;

struct FWoTBSPProofCounters
{
	int32 RenderBuilds = 0;
	int32 CollisionCooks = 0;
	int32 DerivedLoads = 0;
	int32 DerivedWrites = 0;
};

enum class EWoTPayloadType : uint8
{
	Texture,
	Sound,
	Mesh,
	BSP,
};

struct FWoTTexturePayloadMip
{
	uint32 Width = 0;
	uint32 Height = 0;
	uint32 StoredByteLength = 0;
	uint64 ExpandedByteLength = 0;
	uint64 DataOffset = 0;
};

struct FWoTTexturePayloadEntry
{
	FString CacheKey;
	FString PackageName;
	FString AssetName;
	FString AssetPath;
	FString BlobPath;
	FString BlobSHA256;
	uint64 BlobSize = 0;
	bool bMasked = false;
	FString AnimNext;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	uint8 PrimeCount = 0;
	TArray<FWoTTexturePayloadMip> Mips;
};

// Authenticated texture data retained by the subsystem after first use.
// Activated cache-v3 generations keep the immutable verified object alive so
// mip offsets continue to address the exact bytes that passed verification.
// Non-guarded developer targets may instead retain the legacy oracle buffer.
struct FWoTTexturePayloadData
{
	bool bMasked = false;
	uint8 Format = 0;
	FString AnimNext;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	uint8 PrimeCount = 0;
	FString SourcePackage;
	FString SourceExport;
	FWoTSha256 SourceSha256;
	TArray<FColor> Palette;
	TArray<FWoTTexturePayloadMip> Mips;
	TOptional<FWoTRuntimeVerifiedObject> VerifiedObject;
	TConstArrayView<uint8> PayloadBytes() const
	{
		if (VerifiedObject.IsSet())
		{
			return VerifiedObject->Payload();
		}
#if !WOT_GUARDED_BOOTSTRAP
		return BlobBytes;
#else
		return TConstArrayView<uint8>();
#endif
	}
#if !WOT_GUARDED_BOOTSTRAP
	TArray<uint8> BlobBytes;
#endif
};

// An exact runtime texture consumer authorized by an authenticated cache
// object. These fields bind both the destination identity and the complete
// source/header shape before any transient UObject is published.
struct FWoTTextureConsumerBinding
{
	FString AssetPath;
	FString LogicalId;
	FString SourcePackage;
	FString SourceExport;
	FString SourceSha256;
	int32 Width = 0;
	int32 Height = 0;
	int32 MipCount = 0;
	bool bMasked = false;
};

struct FWoTSoundPayloadEntry
{
	FString CacheKey;
	FString PackageName;
	FString AssetName;
	FString AssetPath;
	FString BlobPath;
	FString BlobSHA256;
	uint64 BlobSize = 0;
	uint16 PCMFormatTag = 0;
	uint16 Channels = 0;
	uint32 SampleRate = 0;
	uint16 BitsPerSample = 0;
	uint64 FrameCount = 0;
	uint64 DataByteLength = 0;
};

// Authenticated, immutable sample data retained after the cached WAV has been
// parsed once. PCMBytes contains only the interleaved data chunk, not RIFF.
struct FWoTSoundPayloadData
{
	struct FLoop
	{
		uint64 StartFrame = 0;
		uint64 EndFrameExclusive = 0;
		uint32 PlayCount = 0;
	};

	uint16 Channels = 0;
	uint32 SampleRate = 0;
	uint16 BitsPerSample = 0;
	uint64 FrameCount = 0;
	TArray<FLoop> Loops;
	TArray<uint8> PCMBytes;
};

/**
 * The texture decode of an activated cache generation, which
 * UWoTPayloadSubsystem::LoadTexturePayload runs: the exact logical object read
 * through FWoTRuntimeRegistry, its canonical header, then the blob structure.
 * Null, with the rejection reason, on any failure.
 */
WOT_API TSharedPtr<FWoTTexturePayloadData> WoTDecodeActivatedTexturePayload(
	const FString& CacheKey, FString& OutRejectReason);

/** The sound decode of an activated generation (LoadSoundPayload). */
WOT_API TSharedPtr<FWoTSoundPayloadData> WoTDecodeActivatedSoundPayload(
	const FString& CacheKey, FString& OutRejectReason);

struct FWoTMeshPayloadEntry
{
	FString CacheKey;
	FString PackageName;
	FString AssetName;
	FString AssetPath;
	FString BlobPath;
	FString BlobSHA256;
	uint64 BlobSize = 0;
	FString ClassName;
	uint32 FrameVertexCount = 0;
	uint32 AnimationFrameCount = 0;
	uint32 PackedVertexCount = 0;
	uint32 FrameByteLength = 0;
};

struct FWoTMeshPayloadData
{
	uint32 FrameVertexCount = 0;
	uint32 AnimationFrameCount = 0;
	TArray<uint32> PackedVertices;
};

struct FWoTBSPPayloadEntry
{
	FString CacheKey;
	FString PackageName;
	FString AssetName;
	FString AssetPath;
	FString BlobPath;
	FString BlobSHA256;
	FString Role;
	FString TangentAlgorithm;
	uint64 BlobSize = 0;
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	uint32 SectionCount = 0;
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
	TArray<FString> MaterialSlots;
	TArray<uint32> SectionFirstIndices;
	TArray<uint32> SectionIndexCounts;
	TArray<uint32> SectionMinVertices;
	TArray<uint32> SectionMaxVertices;
	TArray<bool> SectionCollisionEnabled;
};

// CPU-side contract which remains testable under -nullrhi. Each frame has one
// dense delta per final render vertex. Positions are deltas from the stripped
// lattice base; TangentZ is FrameNormal - the explicitly pinned cooked basis.
struct FWoTMeshPayloadDelta
{
	FVector3f PositionDelta = FVector3f::ZeroVector;
	FVector3f TangentZDelta = FVector3f::ZeroVector;
	uint32 SourceIndex = 0;
};

struct FWoTMeshPayloadFrame
{
	TArray<FWoTMeshPayloadDelta> Deltas;
};

struct FWoTMeshPayloadCPUData
{
	int32 RenderVertexCount = 0;
	TArray<FWoTMeshPayloadFrame> Frames;
};

struct FWoTPreparedMeshPayload
{
	// FObjectKey is only the lookup identity. This weak owner is the lifetime
	// signal used to release the shared GPU buffers after the mesh is collected.
	TWeakObjectPtr<USkeletalMesh> Mesh;
	TSharedPtr<FExternalMorphSet> MorphSet;
	// Retained once per mesh so vertex-sprite consumers can reconstruct the
	// current CPU pose without reading the external GPU morph buffers back.
	FWoTMeshPayloadCPUData CPUData;
	int32 FrameCount = 0;
};

// Generated destination-side binding. PayloadKey spelling is authoritative so
// renamed HUD/font copies can intentionally share a retail source. The mip
// count belongs to this destination, never to the source cache entry.
UCLASS(BlueprintType, DefaultToInstanced, EditInlineNew)
class UWoTPayloadAssetUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FString PayloadKey;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload", meta=(ClampMin="1"))
	int32 DestinationMipCount = 1;
};

// Process-local owner for the player-generated payload cache. This subsystem
// deliberately does not participate in replication or gameplay state.
UCLASS()
class UWoTPayloadSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	/** Idempotent lease/resource shutdown performed before Mac uninstall. */
	void ShutdownForUninstall();

	FWoTBootstrapStatus ActivateVerifiedCache(
		const FWoTCacheGenerationLease& Snapshot);
	bool IsDormant() const { return !bPayloadActivated; }
	/** Borrow the generation-pinned owner. The caller may not retain it past
	 * subsystem shutdown. */
	FWoTFrontEndFontOwner* FrontEndFontOwnerForScene()
	{
		return FrontEndFontOwner.Get();
	}
	const FWoTFrontEndFontOwner* FrontEndFontOwnerForScene() const
	{
		return FrontEndFontOwner.Get();
	}
	/** Ordered bootstrap shutdown seam: scene/audio is stopped first, then the
	 * font owner releases its cloned payload lease before the payload lease. */
	void ShutdownFrontEndFontsForScene();
	/** Lazily decode the active generation's complete WOTDEF3 graph once. */
	FWoTRuntimeClassDefaults* RuntimeClassDefaults(FString& OutError);
	/** Active generation owner, including hidden candidates with no GameInstance yet. */
	static FWoTRuntimeClassDefaults* ActiveRuntimeClassDefaults(FString& OutError);
	/** Authenticated generation owner used by process-wide runtime asset routing. */
	static UWoTPayloadSubsystem* ActivePayloadOwnerForRuntimeAssets();
	bool FindRetailTextureFacts(FStringView Identity,
		FWoTRetailTextureFacts& Out);

	bool IsInjectionEnabled() const { return bInjectionEnabled; }
	bool IsManifestReady() const { return bManifestReady; }
	// Re-sweep every loaded payload-bound asset. The activation-time sweep
	// predates any texture the front-end menu style LoadObjects at hand-off,
	// and those loads are not reliably covered by the package-load hook, so
	// the menu rendered black in packaged guarded builds (2026-08-06). The
	// sweep is idempotent (already-injected assets return early).
	bool RebindLoadedPayloads();
	/** Re-sweep, then prove every texture required by the front-end root has
	 * authenticated payload metadata and reached the injected set. */
	bool RebindLoadedPayloads(
		TConstArrayView<UTexture2D*> RequiredFrontEndTextures);
	/** Atomically prepare exact-path transient textures from authenticated
	 * cache objects. No consumer is published unless every row validates. */
	TWoTBootstrapResult<TArray<UTexture2D*>> PrepareTextureConsumers(
		TConstArrayView<FWoTTextureConsumerBinding> Requirements);
	/** The closed six-texture retail front-end consumer set. */
	TWoTBootstrapResult<TArray<UTexture2D*>> PrepareFrontEndMenuTextures();
	const FString& GetCacheDirectory() const { return CacheDirectory; }
	bool IsBSPKeyReady(const FString& CacheKey) const
	{
		return ReadyBSPKeys.Contains(CacheKey);
	}
	bool HasBSPKeyFailed(const FString& CacheKey) const
	{
		return FailedRequiredBSPKeys.Contains(CacheKey);
	}
	bool HasSeenBSPCarrier() const { return bBSPCarrierSeen; }
	static FWoTBSPProofCounters GetBSPProofCounters();

	// Typed lookups reject keys belonging to another manifest payload type.
	const FWoTTexturePayloadEntry* FindTexturePayload(const FString& CacheKey) const;
	const FWoTSoundPayloadEntry* FindSoundPayload(const FString& CacheKey) const;
	const FWoTMeshPayloadEntry* FindMeshPayload(const FString& CacheKey) const;
	const FWoTBSPPayloadEntry* FindBSPPayload(const FString& CacheKey) const;

	// Verifies and decodes a texture blob on first use. A failure quarantines only
	// this semantic entry and never changes other lookups.
	TSharedPtr<const FWoTTexturePayloadData> LoadTexturePayload(
		const FString& CacheKey);
	TSharedPtr<const FWoTSoundPayloadData> LoadSoundPayload(
		const FString& CacheKey);
	TSharedPtr<const FWoTMeshPayloadData> LoadMeshPayload(
		const FString& CacheKey);
	TSharedPtr<const FWoTBSPPayloadData> LoadBSPPayload(
		const FString& CacheKey);

	// Pure CPU half of mesh injection. The subsystem calls this before any GPU
	// publication; automation can exercise the exact transform/normal/mapping
	// path with no RHI.
	static bool ComputeMeshPayloadDeltas(const FWoTMeshPayloadData& Payload,
		const UWoTMeshPayloadLayout& Layout, USkeletalMesh* Mesh,
		FWoTMeshPayloadCPUData& OutData, FString& OutError);
	static bool ValidateStrippedMeshLayout(USkeletalMesh* Mesh,
		const UWoTMeshPayloadLayout& Layout, FString& OutError);

	// Public narrow seam used by fixture specs and future generated binders.
	bool InjectTexture(UTexture2D* Texture, const FString& CacheKey,
		int32 DestinationMipCount);
	bool InjectSound(UWoTPayloadSoundWave* SoundWave, const FString& CacheKey);
	/** Exact per-wave teardown used by snapshot-scoped UI presentation. */
	bool RemoveInjectedSound(UWoTPayloadSoundWave* SoundWave);
	bool IsInjectedSoundBound(const UWoTPayloadSoundWave* SoundWave,
		const TSharedPtr<const FWoTSoundPayloadData>& ExpectedPayload) const;
	bool SharesSnapshotWith(const FWoTCacheGenerationLease& Lease) const;
	bool InjectBSPStaticMesh(UStaticMesh* Mesh, const FString& CacheKey);

	// Called by UWoTVertexAnimComponent at mesh assignment/registration. The
	// subsystem owns and shares the generated GPU morph set; the component owns
	// its registration and external weight array.
	bool BindMeshComponent(UWoTVertexAnimComponent* Component);

	// A payload wave calls this from PostLoad. When the game-instance service is
	// already active it binds immediately; the initialization walk remains the
	// fallback for waves that loaded before the game instance existed.
	static bool TryBindSoundWaveDuringPostLoad(UWoTPayloadSoundWave* SoundWave);

	// CreateSoundGenerator uses this synchronized seam to capture the current
	// active subsystem's immutable per-wave payload snapshot. It is safe to call
	// from the audio-render side and never dereferences a subsystem there.
	static TSharedPtr<const FWoTSoundPayloadData> ResolveSoundPayloadForGenerator(
		const UWoTPayloadSoundWave* SoundWave);

	// Automation needs to prove OFF even when its parent process uses the CLI
	// opt-in. Outside automation builds this override is ignored.
	static void SetInjectionEnabledOverrideForTests(
		bool bHasOverride, bool bEnabled = false);
#if WITH_AUTOMATION_TESTS
	/** Fail the next verified-snapshot activation at the test boundary. */
	static void FailNextActivationForTesting(bool bFail = true);
	/** Force one batch injection failure after all consumer preflight succeeds. */
	void FailTextureConsumerInjectionAtForTesting(int32 Index)
	{
		TextureConsumerInjectionFailureIndexForTests = Index;
	}
	/** Exercise front-end font materialization on a non-production fixture lease. */
	FWoTBootstrapStatus InitializeFrontEndFontsForTesting();
#endif

private:
	friend class FWoTBSPProofCounterSpec;
	friend class FWoTBSPPerMapBarrierSpec;
	FString DiscoverCacheDirectory() const;
	bool LoadManifest();
	void RebuildRequiredBSPKeysByMap();
	static bool TryGetBSPMapFromWorldPackage(
		const FString& WorldPackageName, FString& OutMapName);
	bool IsBSPBarrierReadyForMap(const FString& MapName) const;
	void HandlePackageLoadCompleted(UPackage* Package);
	void ProcessCompletedPackage(UPackage* Package);
	void DrainCompletedPackages();
	void HandlePostGarbageCollect();
	void BindAlreadyLoadedPayloads();
	void TryBindTexture(UTexture2D* Texture);
	TSharedPtr<const FWoTTexturePayloadData>
		LoadTexturePayloadForRetailBuildContext(const FString& CacheKey);
	TSharedPtr<const FWoTTexturePayloadData> LoadTexturePayloadInternal(
		const FString& CacheKey, bool bAllowActivatingGeneration);
	void TryBindSoundWave(UWoTPayloadSoundWave* SoundWave);
	void TryPrepareSkeletalMesh(USkeletalMesh* Mesh);
	void TryBindBSPStaticMesh(UStaticMesh* Mesh);
	void TryBindMeshComponent(UWoTVertexAnimComponent* Component);
	TSharedPtr<FWoTPreparedMeshPayload> PrepareMeshPayload(USkeletalMesh* Mesh,
		const UWoTMeshPayloadLayout& Layout, bool bRetainCPUData = false);
	void PrunePreparedMeshPayloads();
	void HandleWorldTickStart(UWorld* World, enum ELevelTick TickType, float DeltaSeconds);
	void HandleRuntimeIntegrityFailure(const FWoTBootstrapError& Failure);

	bool bInjectionEnabled = false;
	bool bManifestReady = false;
	bool bPayloadActivated = false;
	bool bShutdownForUninstall = false;
	bool bRuntimeIntegrityRecoveryHandled = false;
	TUniquePtr<FWoTCacheGenerationLease> ActiveCacheLease;
	TUniquePtr<FWoTFrontEndFontOwner> FrontEndFontOwner;
	TUniquePtr<FWoTRuntimeClassDefaults> ClassDefaultsOwner;
	FString CacheDirectory;
	FDelegateHandle PackageLoadCompletedHandle;
	FDelegateHandle PostGarbageCollectHandle;
	FDelegateHandle WorldTickStartHandle;
	TArray<TWeakObjectPtr<UPackage>> PendingCompletedPackages;
	TMap<FString, FWoTTexturePayloadEntry> TextureEntries;
	TMap<FName, FString> DirectTextureKeys;
	TMap<FString, TSharedPtr<const FWoTTexturePayloadData>> LoadedTexturePayloads;
	TMap<FString, FWoTSoundPayloadEntry> SoundEntries;
	TMap<FName, FString> DirectSoundKeys;
	TMap<FString, TSharedPtr<const FWoTSoundPayloadData>> LoadedSoundPayloads;
	TMap<FString, FWoTMeshPayloadEntry> MeshEntries;
	TMap<FString, TSharedPtr<const FWoTMeshPayloadData>> LoadedMeshPayloads;
	TMap<FString, FWoTBSPPayloadEntry> BSPEntries;
	TMap<FString, TArray<FString>> RequiredBSPKeysByMap;
	TMap<FString, TSharedPtr<const FWoTBSPPayloadData>> LoadedBSPPayloads;
	TMap<FObjectKey, TSharedPtr<FWoTPreparedMeshPayload>> PreparedMeshPayloads;
	TSet<TWeakObjectPtr<UWoTVertexAnimComponent>> BoundMeshComponents;
	TSet<FString> RejectedBlobKeys;
	TSet<FObjectKey> InjectedTextures;
	TSet<FObjectKey> InjectedSounds;
	TSet<FObjectKey> InjectedBSPMeshes;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> PreparedTextureConsumers;
#if WITH_AUTOMATION_TESTS
	int32 TextureConsumerInjectionFailureIndexForTests = INDEX_NONE;
#endif
	TSet<FString> ReadyBSPKeys;
	TSet<FString> FailedRequiredBSPKeys;
	TSet<TWeakObjectPtr<UWorld>> BSPPausedWorlds;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBodySetup>> PendingBSPBodySetups;
	bool bBSPCarrierSeen = false;
};
