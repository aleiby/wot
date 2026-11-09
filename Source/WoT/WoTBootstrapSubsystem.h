#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Payload/WoTCacheLease.h"
#include "Updater/WoTExecutableLayout.h"
#include "Updater/WoTInstallerAcquisition.h"
#include "WoTBootstrapDiskPreflight.h"
#include "WoTBootstrapViewModel.h"
#include "FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "UI/WoTReleaseNotes.h"
#include "WoTBootstrapSubsystem.generated.h"

struct FWoTBootstrapWorkerRequest
{
	EWoTBootstrapPhase Phase = EWoTBootstrapPhase::Fatal;
	uint64 OperationEpoch = 0;
	/** Non-empty only for the sole bounded announcement operation. */
	TArray<FString> AnnouncementLogicalObjects;
};

struct FWoTBootstrapWorkerCompletion
{
	uint64 OperationEpoch = 0;
	EWoTBootstrapPhase CompletedPhase = EWoTBootstrapPhase::Fatal;
	EWoTBootstrapPhase NextPhase = EWoTBootstrapPhase::Fatal;
	FWoTDiskBudgetInputs NextPhaseDiskBudget;
	bool bSucceeded = false;
	bool bMandatoryWork = false;
	bool bAnnouncementUnavailable = false;
	bool bUpdateNotifyOnly = false;
	FString NotifyOnlyReason;
	uint64 NotifyOnlyReleaseSequence = 0;
	FString NotifyOnlyReleasePageUrl;
	EWoTOriginalGameNeededReason OriginalGameNeededReason =
		EWoTOriginalGameNeededReason::Missing;
	FString OriginalGameNeededDetail;
	// The update helper has taken over and this process's exit was requested:
	// the bootstrap stays on the current screen ("Installing update...") and
	// does not begin NextPhase.
	EWoTBootstrapDiagnosticCode DiagnosticCode =
		EWoTBootstrapDiagnosticCode::None;
	// Diagnostic-only: the underlying error's text for the heartbeat. The
	// coarse DiagnosticCode alone made packaged-build failures (Test config,
	// logging compiled out) undiagnosable (Win64 funnel, 2026-08-05). Never
	// drives behavior.
	FString FailureDetail;
	/** Actionable cache failure text for the player; diagnostic detail stays separate. */
	FString FailureMessage;
	TOptional<FWoTCacheGenerationLease> CacheSnapshot;
};

using FWoTBootstrapProgressCallback =
	TFunction<void(FWoTBootstrapProgressEvent&&)>;
using FWoTBootstrapCompletionCallback =
	TFunction<void(FWoTBootstrapWorkerCompletion&&)>;
using FWoTUpdateStateCallback = TFunction<void(FWoTUpdateState&&)>;
using FWoTReleaseNotesCallback = TFunction<void(FWoTReleaseNotesResult&&)>;
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTReleaseNotesCompleted,
	const FWoTReleaseNotesResult&);

/**
 * Abstract asynchronous engine seam. Implementations own their blocking worker
 * and may call callbacks from that worker; the subsystem always marshals them
 * back to the game thread and rejects stale epochs.
 */
class IWoTBootstrapWorker
{
public:
	virtual ~IWoTBootstrapWorker() = default;
	virtual void StartPhase(const FWoTBootstrapWorkerRequest& Request,
		FWoTBootstrapProgressCallback&& Progress,
		FWoTBootstrapCompletionCallback&& Completion) = 0;
	virtual bool PersistVerifiedResumeState() = 0;
	virtual void StartReleaseNotes(bool, FWoTReleaseNotesCallback&&) {}
	virtual void StartBackgroundUpdate(FWoTUpdateStateCallback&&) {}
	virtual bool RequestRestartToUpdate() { return false; }
	virtual void RequestOriginalGameRecheck() {}
	virtual void RequestPickedInstaller(FString) {}
	/** "Copy from another computer": start the LAN installer receiver while
	 * the original game is needed (spec B1, B5). */
	virtual void RequestInstallerReceive() {}
	/** Cancel the LAN copy in progress; the receiver keeps listening. */
	virtual void CancelInstallerReceive() {}
	virtual void CancelAndJoin() = 0;
};

/** Owner of the integrated bootstrap state machine. */
UCLASS()
class UWoTBootstrapSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	/** Idempotent synchronous shutdown used before the Mac uninstaller removes
	 * user state. Deinitialize reuses this exact path later during engine exit. */
	void ShutdownForUninstall();
	void CancelWorkerForUninstall();

	void Start();
	FWoTBootstrapSnapshot Snapshot() const { return ViewModel.Snapshot(); }
	bool RequestResumeAndExit();
	bool RequestRestartToUpdate();
	void RequestOriginalGameRecheck();
	void RequestPickedInstaller(const FString& AbsolutePath);
	/** The screen's "Copy from another computer": only while the original
	 * game is needed. */
	void RequestInstallerCopy();
	/** The screen's Cancel during a LAN copy. */
	void CancelInstallerCopy();
	bool IsReady() const;
	/** The installer location beside the game exactly as the worker searches
	 * it (path and, when the layout resolved one, parent identity); unset when
	 * no layout resolved. Read-only: LAN installer sharing's finder uses it. */
	TOptional<FWoTInstallerLocation> RetainedAdjacentInstallerLocation() const;
	/** Transition the already-ready UI to the runtime-integrity error screen
	 * when retained front-end recovery is not admissible. */
	void HandleRuntimeCacheIntegrityFailure(
		FString Message, bool bFrontEndRecoverable);
	bool ShouldExitWithFailure() const { return bRuntimeIntegrityExitFailure; }
	bool HasStarted() const { return bStarted; }
	bool HasInitializationFailed() const { return bInitializationFailed; }
	const TOptional<FWoTReleaseNotesResult>& ReleaseNotesResult() const
	{
		return RetainedReleaseNotesResult;
	}
	FWoTReleaseNotesCompleted& OnReleaseNotesCompleted()
	{
		return ReleaseNotesCompleted;
	}
	bool IsFrontEndSceneCommitted() const;
	/** Last-instruction travel hook. Stops tick/audio and destroys the scene,
	 * while retaining the authenticated scene plan and world lease. */
	void SuspendFrontEndSceneBeforeTravel();
	/** Rebuild a suspended scene in the newly loaded front-end world. */
	bool EnsureFrontEndSceneForWorld(
		UWorld& World, APlayerController* ViewOwner = nullptr);
	bool IsFrontEndSceneSuspendedForWorld(const UWorld& World) const;

	static bool IsNormalFrontEndAuthorized(const UGameInstance* GameInstance);

#if WITH_AUTOMATION_TESTS
	using FWorkerFactory = TFunction<TUniquePtr<IWoTBootstrapWorker>()>;
	using FFreeSpaceFactory = TFunction<TUniquePtr<IWoTFreeSpaceProbe>()>;
	using FCacheActivationAdapter = TFunction<FWoTBootstrapStatus(
		FWoTCacheGenerationLease&&)>;
	static void SetCreationEnabledForTesting(bool bEnabled);
	void AdoptFrontEndSceneForTesting(
		TUniquePtr<FWoTFrontEndSceneMaterializer>&& Scene)
	{
		FrontEndScene = MoveTemp(Scene);
		bFrontEndSceneRequired = FrontEndScene.IsValid();
	}
	static void SetAdaptersForTesting(
		FWorkerFactory&& WorkerFactory,
		FFreeSpaceFactory&& FreeSpaceFactory);
	/** State-machine-only suites may isolate the StartingGame edge. */
	static void SetCacheActivationAdapterForTesting(
		FCacheActivationAdapter&& ActivationAdapter);
	static void SetReadyAdoptionObserverForTesting(
		TFunction<void(bool, bool)>&& Observer);
	static void SetBeforeCacheActivationObserverForTesting(
		TFunction<void(const FWoTCacheGenerationLease&)>&& Observer);
	static void ResetTestingAuthorityAndAdapters();
	const FString& CanonicalAdjacentInstallerPathForTesting() const
	{
		return CanonicalAdjacentInstallerPath;
	}
	int32 WorkerStartCountForTesting() const { return WorkerStartCount; }
	int32 ProgressDeliveryCountForTesting() const
	{
		return ProgressDeliveryCountForTestingValue;
	}
	int32 UpdateStateDeliveryCountForTesting() const
	{
		return UpdateStateDeliveryCountForTestingValue;
	}
	void SetRunningReleaseSequenceForTesting(uint64 Sequence)
	{
		ViewModel.SetRunningReleaseSequence(Sequence);
	}
	int32 RestartRequestCountForTesting() const
	{
		return RestartRequestCountForTestingValue;
	}
	void DispatchUpdateStateForTesting(FWoTUpdateState&& State)
	{
		DispatchUpdateState(MoveTemp(State));
	}
	void DispatchReleaseNotesForTesting(FWoTReleaseNotesResult&& Result)
	{
		DispatchReleaseNotes(MoveTemp(Result));
	}
	int32 ScreenConstructionCountForTesting() const
	{
		return ScreenConstructionCount;
	}
	void RecordScreenConstructionForTesting() { ++ScreenConstructionCount; }
#endif
#if WITH_AUTOMATION_TESTS \
	|| (defined(WOT_PROJECT_OWNS_ENTRYPOINT) && WOT_PROJECT_OWNS_ENTRYPOINT)
	/** Canonical-layout injection for test processes outside a packaged tree. */
	static void SetCanonicalAdjacentInstallerPathForTesting(FString Path);
#endif

private:
	void FailInitialization(EWoTBootstrapDiagnosticCode DiagnosticCode);
	void BeginCurrentPhase(const FWoTDiskBudgetInputs* DiskBudget = nullptr);
	void DispatchProgress(FWoTBootstrapProgressEvent&& Event);
	void BeginProgressDispatchShutdown();
	void SchedulePendingProgressDelivery();
	bool DeliverPendingProgress(float DeltaSeconds);
	void DispatchCompletion(FWoTBootstrapWorkerCompletion&& Completion);
	void HandleProgress(FWoTBootstrapProgressEvent&& Event);
	void HandleCompletion(FWoTBootstrapWorkerCompletion&& Completion);
	void StartReleaseNotes();
	void DispatchReleaseNotes(FWoTReleaseNotesResult&& Result);
	void CompleteReleaseNotes(FWoTReleaseNotesResult&& Result);
	void StartBackgroundUpdate();
	void DispatchUpdateState(FWoTUpdateState&& State);
	FWoTBootstrapStatus ActivateVerifiedGeneration(
		FWoTCacheGenerationLease&& Snapshot);
	void FailOperation(EWoTBootstrapDiagnosticCode DiagnosticCode,
		FString Message = FString());
	static void ExitIfUnattendedGuardedFailure(
		EWoTBootstrapDiagnosticCode DiagnosticCode,
		EWoTBootstrapPhase FailedPhase, uint64 FailedEpoch);

	FWoTBootstrapViewModel ViewModel;
	TUniquePtr<IWoTBootstrapWorker> Worker;
	TUniquePtr<IWoTFreeSpaceProbe> FreeSpace;
	TUniquePtr<FWoTFrontEndSceneMaterializer> FrontEndScene;
	TOptional<FWoTResolvedExecutableLayout> ExecutableLayout;
	FString CanonicalAdjacentInstallerPath;
	double LastProgressHeartbeatSeconds = 0.0;
	FCriticalSection PendingProgressLock;
	TOptional<FWoTBootstrapProgressEvent> PendingProgress;
	FTSTicker::FDelegateHandle ProgressTickerHandle;
	double LastProgressPublicationSeconds = 0.0;
	bool bProgressDeliveryQueued = false;
	FCriticalSection PendingUpdateStateLock;
	TOptional<FWoTUpdateState> PendingUpdateState;
	bool bUpdateStateDeliveryQueued = false;
	TOptional<FWoTReleaseNotesResult> RetainedReleaseNotesResult;
	FWoTReleaseNotesCompleted ReleaseNotesCompleted;

#if WITH_AUTOMATION_TESTS
	int32 ProgressDeliveryCountForTestingValue = 0;
	int32 UpdateStateDeliveryCountForTestingValue = 0;
#endif
	bool bStarted = false;
	bool bInitializationFailed = false;
	bool bShuttingDown = false;
	bool bAnnouncementOperationStarted = false;
	bool bFrontEndSceneRequired = false;
	bool bRuntimeIntegrityExitFailure = false;
	int32 WorkerStartCount = 0;
	int32 ScreenConstructionCount = 0;

#if WITH_AUTOMATION_TESTS
	int32 RestartRequestCountForTestingValue = 0;
#endif
};
