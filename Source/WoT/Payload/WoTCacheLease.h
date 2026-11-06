// A lease is the only authority from which cache payload bytes may be opened.
#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/BSPWorld.h"

class FWoTCacheStore;
class FWoTRuntimeRegistry;
class FWoTFrontEndFontOwner;
class FWoTFrontEndPresentationAuthority;
class FWoTFrontEndSceneMaterializer;
class FWoTImmutableVerifiedBlob;
class IWoTProgressSink;
class UWorld;
class UWoTPayloadSubsystem;
struct FWoTCacheBlobDescriptor;
struct FWoTCacheBlobReadLimits;
struct FWoTCacheGenerationSharedState;
struct FWoTFrontEndDescriptor;
struct FWoTWorldDescriptor;

/** One immutable record parsed from the exact verified manifest bytes. */
struct FWoTCacheLeaseManifestEntry
{
	FString LogicalId;
	FString Schema;
	uint64 Size = 0;
	FWoTSha256 Sha256;
	/** Exact ordered fact identities retained from the verified manifest. */
	TArray<FString> Satisfies;
};

/** Build pins parsed from the exact manifest bytes held by this lease. */
struct FWoTCacheGenerationBuildIdentity
{
	bool bHasBuildIdentity = false;
	FWoTSha256 InstallerSha256;
	FWoTSha256 ContractSha256;
	FWoTSha256 ProductionDescriptorSha256;
};

#if WITH_AUTOMATION_TESTS
/** Observe physical snapshot reads until the final returned token owner is
 * destroyed; final destruction drains callbacks already in flight. */
WOT_API TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTObserveCacheSnapshotPhysicalReadsForTesting(
	TFunction<void(FStringView)> Observer);
/** Internal production-path instrumentation hook used at exact ReadAll seams. */
WOT_API void WoTObserveCacheSnapshotPhysicalReadForTesting(
	FStringView LogicalObject);
/** Exact seam after revocation admission closes while the verified-read guard
 * is still held. Used to prove a waiting second reader cannot enter. */
WOT_API void WoTSetCacheRevocationStartedObserverForTesting(
	TFunction<void()> Observer);
#endif

class WOT_API FWoTCacheGenerationLease final
{
public:
	FWoTCacheGenerationLease() = default;
	~FWoTCacheGenerationLease();
	FWoTCacheGenerationLease(FWoTCacheGenerationLease&& Other);
	FWoTCacheGenerationLease& operator=(FWoTCacheGenerationLease&& Other);

	FWoTCacheGenerationLease(const FWoTCacheGenerationLease&) = default;
	FWoTCacheGenerationLease& operator=(const FWoTCacheGenerationLease&) = default;

	const FString& GetCanonicalRoot() const;
	FWoTNativeFileIdentity GetRootIdentity() const;
	const FWoTSha256& GetManifestSha256() const;
	const FWoTCacheGenerationBuildIdentity& GetBuildIdentity() const;
	bool HasFrontEndDescriptorAttestation() const;
	const FWoTSha256& GetExpectedFrontEndDescriptorRoot() const;
	uint64 GetVerifiedCriticalObjectCount() const;
	uint64 GetVerifiedCriticalBytes() const;
	bool SharesSnapshotWith(const FWoTCacheGenerationLease& Other) const;
	bool IsValid() const;
	/** A runtime payload read failed after this activated snapshot was admitted. */
	bool HasRuntimeIntegrityFailure() const;
	/** The durable invalidation/diagnostic record completed and the retained,
	 * startup-critical front-end snapshot is independent of the failed output. */
	bool CanRecoverFrontEndAfterRuntimeIntegrityFailure() const;
	FString GetRuntimeIntegrityFailureOutput() const;
	FString GetRuntimeIntegrityFailureReason() const;
	bool IsDamaged() const { return HasRuntimeIntegrityFailure(); }
	TOptional<FWoTBootstrapError> TakeReadFailure();
	TConstArrayView<FWoTWorldDescriptor> GetWorldDescriptors() const;
	const FWoTFrontEndDescriptor& GetFrontEndDescriptor() const;
#if WITH_AUTOMATION_TESTS
	FWoTBootstrapStatus VerifyCriticalClosureForTesting(TConstArrayView<FString> LogicalIds, IWoTProgressSink* Progress);
	const FWoTCacheLeaseManifestEntry* FindManifestEntryForTesting(
		const FString& LogicalId) const { return FindManifestEntry(LogicalId); }
	void RevokeForAutomationTests() const { Revoke(); }
#endif

private:
	static TWoTBootstrapResult<FWoTCacheGenerationLease> Acquire(
		FString InCanonicalRoot, const FWoTNativeFileIdentity& InRootIdentity,
		const FWoTSha256& InManifestSha256,
		const FWoTCacheGenerationBuildIdentity& InBuildIdentity,
		TArray<FWoTWorldDescriptor>&& InWorldDescriptors,
		const FWoTSha256& InExpectedFrontEndDescriptorRoot,
		FWoTNativeDirectoryHandle&& InGenerationDirectory,
		TArray<FWoTCacheLeaseManifestEntry>&& InManifestEntries);
	FWoTBootstrapStatus VerifyCriticalClosure(IWoTProgressSink* Progress);
	FWoTBootstrapStatus VerifyLogicalIds(TConstArrayView<FString> LogicalIds, IWoTProgressSink* Progress, bool bRequireFrontEnd);

	explicit FWoTCacheGenerationLease(
		TSharedPtr<FWoTCacheGenerationSharedState, ESPMode::ThreadSafe> InState);

	const FWoTCacheLeaseManifestEntry* FindManifestEntry(
		const FString& LogicalId) const;
	const TMap<FString, FWoTCacheLeaseManifestEntry>& GetManifestTable() const;
	const FWoTNativeDirectoryHandle& GetGenerationDirectory() const;
	const FWoTNativeDirectoryHandle& GetBlobDirectory() const;
	FCriticalSection& GetVerifiedReadGuard() const;
	bool MayBeginVerifiedReadUnderVerifiedReadGuard() const;
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> FindVerifiedBuffer(
		const FWoTCacheBlobDescriptor& Descriptor) const;
	void RememberVerifiedBuffer(const FWoTCacheBlobDescriptor& Descriptor,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Buffer) const;
	bool PinCriticalBuffer(const FWoTCacheBlobDescriptor& Descriptor,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Buffer) const;
	bool CanPublishPresentation() const;
	bool BeginRevocationUnderVerifiedReadGuard(
		TArray<TFunction<bool()>>& OutCallbacks) const;
	bool BeginRuntimeIntegrityFailureUnderVerifiedReadGuard(
		const FString& LogicalId, const FString& Reason,
		TArray<TFunction<bool()>>& OutCallbacks) const;
	FString MarkDamagedBestEffort() const;
	void FinishRevocation(TArray<TFunction<bool()>>&& Callbacks) const;
	bool IsRevocationInProgress() const;
	void CompleteDeferredRevocation() const;
	void Revoke() const;
	uint64 SubscribeRevocation(TFunction<bool()> Callback) const;
	void UnsubscribeRevocation(uint64 Subscription) const;

	TSharedPtr<FWoTCacheGenerationSharedState, ESPMode::ThreadSafe> State;

	friend class FWoTCacheStore;
	friend class FWoTRuntimeRegistry;
	friend class FWoTFrontEndFontOwner;
	friend class FWoTFrontEndPresentationAuthority;
	friend class FWoTFrontEndSceneMaterializer;
	friend class UWoTPayloadSubsystem;
	friend WOT_API bool WoTCommitAuthenticatedWorldExposure(
		UWorld*, FString*);
	friend WOT_API TWoTBootstrapResult<FWoTImmutableVerifiedBlob> ReadVerifiedCacheBlob(
		const FWoTCacheGenerationLease&, const FWoTCacheBlobDescriptor&,
		const FWoTCacheBlobReadLimits&);
	friend WOT_API TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>
		WoTDeriveWorldDescriptorsFromActiveGeneration(
			const FWoTCacheGenerationLease&);
	friend WOT_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
		WoTDeriveFrontEndDescriptorFromActiveGeneration(
			const FWoTCacheGenerationLease&);
	friend WOT_API TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>>
		WoTDeriveFrontEndWorldManifestAuthorityFromActiveGeneration(
			const FWoTCacheGenerationLease&);
};

/** Copy the immutable, verified entry descriptor held by a world lease.
 * No hash, parser, or cache pathname is used after the generation handoff. */
WOT_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
WoTDeriveFrontEndDescriptorFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease);

/** Resolve the separately attested front-end World/BSP through the retained
 * descriptor, current production contract, and exact held manifest. */
WOT_API TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>>
WoTDeriveFrontEndWorldManifestAuthorityFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease);
