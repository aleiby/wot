// Production bootstrap worker composing updater-core policy and game transports.
#pragma once

#include "CoreMinimal.h"
#include "../WoTBootstrapSubsystem.h"
#include "WoTInstallerAcquisition.h"
#include "WoTCacheBootstrapOperation.h"
#include "WoTExecutableLayout.h"
#include "WoTReleaseIdentity.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUpdateTransports/WoTUpdateTransportPolicy.h"

/** Conservative space reserved for one immutable retail-content cache.
 * No complete cache-v3 census for all 11 current outputs is present locally:
 * .stripped-pak covers only texture/sound/mesh/BSP, so using its 296,757,708-B
 * older subtotal would omit seven domains. Fall back honestly to the reviewed
 * per-output maxima in CacheBuilder.cpp: animation 536,870,912; BSP
 * 1,073,741,824; class_default 268,435,456; font 268,435,456; localization
 * 67,108,864; mesh 536,870,912; procedural 268,435,456; registry 268,435,456;
 * sound 268,435,456; texture 268,435,456; world 1,073,741,824. Their sum is
 * 4,898,947,072 B. Add the store's 16,777,216-B maximum manifest/publication
 * document (4,915,724,288 B), then round up to the power-of-two 8-GiB bound
 * for the building/current rename and filesystem allocation overhead. */
inline constexpr uint64 WoTBootstrapCacheConstructionBudgetBytes =
	8ull * 1024ull * 1024ull * 1024ull;

class FWoTGameBootstrapWorker final : public IWoTBootstrapWorker
{
public:
	struct FDependencies
	{
		TUniquePtr<IWoTHttpHopExecutor> Hops;
		FString UserStateRoot;
		FWoTNativeFileIdentity UserStateRootIdentity;
		bool bHasUserStateRootIdentity = false;
		FString InstallRoot;
		FWoTNativeFileIdentity AdjacentInstallerParentIdentity;
		bool bHasAdjacentInstallerParentIdentity = false;
		FWoTNativeFileIdentity InstallRootIdentity;
		bool bHasInstallRootIdentity = false;
		FString LiveLeaf;
		/** Attested running artifact, independent of the mutable state root. */
		FString InstalledTreeRoot;
		FString CanonicalAdjacentInstallerPath;
		FString DownloadsDirectory;
		FString ContentCacheRoot;
		/** Precise notify-only reason when the per-copy update layout is absent. */
		FString UpdateDisableReason;
		FString AnnouncementJsonUrl;
		WoTReleaseIdentity::FIdentity RunningIdentity;
		/** The running game's build configuration ("Shipping", "Test"). A
		 * release attested for another configuration is never staged: its
		 * executable is not the one this copy's launcher starts. Empty skips
		 * the check. */
		FString RunningConfiguration;
		double AnnouncementDeadlineSeconds = 15.0;
		double DownloadOperationDeadlineSeconds = 0.0;
		double OriginalGameProbeIntervalSeconds = 2.0;
		TFunction<double()> NowSeconds;
		TFunction<void()> RequestRestartExit;
#if WITH_AUTOMATION_TESTS
		/** Small pinned artifact used only by real-acquisition worker specs. */
		TOptional<FWoTInstallerAcquisitionPins> InstallerPinsForTesting;
		FWoTInstallerAcquisitionTestHooks InstallerHooksForTesting;
		/** Spec-scale coordinator behind the otherwise production operation. */
		FWoTCacheBootstrapOperation::FBuildForTesting CacheBuildForTesting;
		/** In-memory semantic fixture store; production always opens strictly. */
		bool bUseSemanticCacheFixture = false;
		TOptional<FWoTCacheGenerationLease> InitialCacheSnapshotForTesting;
		/** Deterministic failure seam for the install-root writability probe. */
		TFunction<FWoTBootstrapStatus()> InstallWritabilityProbeForTesting;
		/** Deterministic running-tree translocation seam for notify-only specs. */
		TFunction<bool(const FString&)> RunningTreeTranslocatedForTesting;
		TFunction<TWoTBootstrapResult<bool>()>
			AdjacentParentWritableForTesting;
		/** Forces one installer candidate to be unavailable for this poll. */
		TFunction<TOptional<FWoTBootstrapError>(
			EWoTInstallerSource, const FString&)>
			InstallerLocationFaultForTesting;
		/** Observes each cooperative yield between staging hash chunks. */
		TFunction<void()> StagingYieldForTesting;
#endif
	};

	explicit FWoTGameBootstrapWorker(FDependencies&& InDeps);
	virtual ~FWoTGameBootstrapWorker() override;

	virtual void StartPhase(const FWoTBootstrapWorkerRequest& Request,
		FWoTBootstrapProgressCallback&& Progress,
		FWoTBootstrapCompletionCallback&& Completion) override;
	virtual bool PersistVerifiedResumeState() override;
	virtual void StartReleaseNotes(bool bSeen,
		FWoTReleaseNotesCallback&& Callback) override;
	virtual void StartBackgroundUpdate(
		FWoTUpdateStateCallback&& Callback) override;
	virtual bool RequestRestartToUpdate() override;
	virtual void RequestOriginalGameRecheck() override;
	virtual void RequestPickedInstaller(FString CanonicalPath) override;
	virtual void CancelAndJoin() override;
#if WITH_AUTOMATION_TESTS
	void SetRunningReleaseSequenceForTesting(uint64 ReleaseSequence);
	void SetRunningIdentityForTesting(bool bStamped, uint64 ReleaseSequence,
		FString ReleaseCommit);
	void SetInitialCacheSnapshotForTesting(FWoTCacheGenerationLease&& Snapshot);
#endif

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

/** Production assembly from compiled trust, adoption, and executable layout. */
TUniquePtr<IWoTBootstrapWorker> WoTCreateProductionBootstrapWorker(
	const FWoTResolvedExecutableLayout& Layout);

/** Production free-space probe bound to the updater state directory's volume. */
TUniquePtr<IWoTFreeSpaceProbe> WoTCreateProductionFreeSpaceProbe(
	const FWoTResolvedExecutableLayout& Layout);

/** Resolve the per-copy parent of the running release tree. */
FString WoTResolveInstallRootFromExecutable();

/** Resolve the unchanged OS-profile updater state root. */
FString WoTResolveUserStateRoot();

/** Resolve state root, exact containing tree/member, and adjacent installer
 * together so no consumer can reinterpret another copy as the running tree. */
FWoTResolvedExecutableLayout WoTResolveExecutableLayoutFromPath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot);
FWoTResolvedExecutableLayout WoTResolveExecutableLayout();

/** Derive R without creating it or any fixed leaf. Used before the session gate. */
FString WoTResolveProfileSessionRootForStartup();

/** Resolve the running executable against the root whose SHARED gate is held. */
FWoTResolvedExecutableLayout WoTResolveExecutableLayoutForProfileSession(
	FStringView UserStateRoot);

/** Retains the once-canonicalized startup root and its native object identity. */
void WoTAdoptResolvedExecutableLayout(
	const FWoTResolvedExecutableLayout& Layout);

/** Reopens the retained root without re-deriving the OS-profile default. */
TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
	WoTReopenResolvedUserStateRoot();

/** The OS-profile default user-state root path, unbound. Only for best-effort
 * diagnostics (the heartbeat) when the adopted, identity-bound root is unusable. */
FString WoTDefaultUserStateRootPathForDiagnostics();

/** Resolve the exact release tree containing an executable.
 *
 * On Mac this is the outermost enclosing app bundle; on Win64 it is the
 * safely named tree containing `WoT/Binaries/Win64/<exe>`.
 */
FString WoTResolveInstalledTreeRootFromExecutablePath(
	FStringView ExecutablePath);

/** Production form bound only to the current process image path. */
FString WoTResolveInstalledTreeRootFromExecutable();

/** Resolve the explicit `UserStateRoot/ContentCache` path. */
FString WoTResolveProductionContentCacheRoot();
FString WoTResolveProductionContentCacheRoot(
	const FWoTResolvedExecutableLayout& Layout);

#if WITH_AUTOMATION_TESTS
/** Path-injected form used only by the executable-layout specifications. */
FString WoTGameBootstrapWorkerSpecResolveInstallRootFromExecutablePath(
	FStringView ExecutablePath,
	FStringView DefaultUserStateRoot);

/** Path-injected cache-root derivation specification seam. */
FString WoTGameBootstrapWorkerSpecResolveProductionContentCacheRootFromExecutablePath(
	FStringView ExecutablePath,
	FStringView DefaultUserStateRoot);

/** Inject the native handle-name result without enumerating the parent. */
FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot,
	FStringView NativeTreePath, bool bFailNameQuery = false,
	bool bRunningTreeTranslocated = false);

/** Inject a real replacement tree path and reopen its final projected parent. */
FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithReboundNativeTreePath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot,
	FStringView NativeTreePath);

/** Injects the following-open/canonical-reopen identity-race result. */
FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithIdentityMismatch(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot);

void WoTGameBootstrapWorkerSpecResetLayoutResolutionCount();
int32 WoTGameBootstrapWorkerSpecLayoutResolutionCount();
void WoTGameBootstrapWorkerSpecResetAdjacentParentOpenCount();
int32 WoTGameBootstrapWorkerSpecAdjacentParentOpenCount();
#endif
