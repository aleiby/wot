#include "WoTBootstrapSubsystem.h"

#include "WoTBootFailureReceipt.h"
#include "WoTBootstrapHeartbeat.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Payload/WoTPayloadActivation.h"
#include "Updater/WoTGameBootstrapWorker.h"
#include "World/WoTWorldMaterializer.h"
#include "FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTPayloadSubsystem.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTSessionSubsystem.h"

#include <cstdlib>

namespace
{
#if WITH_AUTOMATION_TESTS
bool GWoTBootstrapCreationEnabledForTesting = false;
UWoTBootstrapSubsystem::FWorkerFactory GWoTBootstrapWorkerFactory;
UWoTBootstrapSubsystem::FFreeSpaceFactory GWoTBootstrapFreeSpaceFactory;
UWoTBootstrapSubsystem::FCacheActivationAdapter
	GWoTBootstrapCacheActivationAdapter;
TFunction<void(bool, bool)> GWoTBootstrapReadyAdoptionObserver;
TFunction<void(const FWoTCacheGenerationLease&)>
	GWoTBootstrapBeforeCacheActivationObserver;
#endif
#if WITH_AUTOMATION_TESTS \
	|| (defined(WOT_PROJECT_OWNS_ENTRYPOINT) && WOT_PROJECT_OWNS_ENTRYPOINT)
FCriticalSection GWoTBootstrapTestAdapterLock;
TOptional<FString> GWoTBootstrapCanonicalInstallerPathForTesting;
#endif

bool WoTBootstrapNeedsDiskPreflight(EWoTBootstrapPhase Phase)
{
	return Phase == EWoTBootstrapPhase::DownloadingUpdate
		|| Phase == EWoTBootstrapPhase::ExtractingOriginalGame
			|| Phase == EWoTBootstrapPhase::BuildingContent;
}

const TCHAR* WoTBootstrapAggregateSpanName(EWoTBootstrapPhase Phase)
{
	switch (Phase)
	{
	case EWoTBootstrapPhase::CheckingOriginalGameData:
		return TEXT("metadata-candidate-open");
	case EWoTBootstrapPhase::VerifyingContent:
		return TEXT("critical-closure-verification");
	case EWoTBootstrapPhase::StartingGame:
		return TEXT("lease-mint-adopt");
	default:
		return nullptr;
	}
}

void WoTRecordBootstrapSpanEnd(
	EWoTBootstrapPhase Phase, uint64 OperationEpoch, bool bSucceeded)
{
	if (const TCHAR* Name = WoTBootstrapAggregateSpanName(Phase))
	{
		WoTRecordBootstrapHeartbeat(TEXT("span-end"), Phase, OperationEpoch,
			{}, {}, FString::Printf(TEXT("name=%s result=%s"), Name,
				bSucceeded ? TEXT("success") : TEXT("failure")));
	}
}

TOptional<FString> WoTBootstrapCanonicalAdjacentInstallerPath(
	const FWoTResolvedExecutableLayout* ExecutableLayout)
{
#if WITH_AUTOMATION_TESTS \
	|| (defined(WOT_PROJECT_OWNS_ENTRYPOINT) && WOT_PROJECT_OWNS_ENTRYPOINT)
	{
		FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
		if (GWoTBootstrapCanonicalInstallerPathForTesting.IsSet())
		{
			return GWoTBootstrapCanonicalInstallerPathForTesting;
		}
	}
#endif
	if (ExecutableLayout == nullptr || !ExecutableLayout->IsValid())
	{
		return {};
	}
	return ExecutableLayout->CanonicalAdjacentInstallerPath;
}

FWoTBootstrapStatus WoTBootstrapActivationError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

}

bool UWoTBootstrapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
#if WOT_GUARDED_BOOTSTRAP
	return true;
#elif WITH_AUTOMATION_TESTS
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	return GWoTBootstrapCreationEnabledForTesting;
#else
	return false;
#endif
}

void UWoTBootstrapSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ViewModel.SetRunningReleaseSequence(
		WoTReleaseIdentity::LoadRunning().ReleaseSequence);

	// EarlyUI remains the first authenticated dependency before worker or Slate
	// construction.
	UWoTPreferencesSubsystem* Preferences =
		Collection.InitializeDependency<UWoTPreferencesSubsystem>();
	if (Preferences == nullptr
		|| !Preferences->HasAdoptedAuthenticatedEarlyUI())
	{
		FailInitialization(EWoTBootstrapDiagnosticCode::EarlyUINotAdopted);
		return;
	}

#if WOT_GUARDED_BOOTSTRAP
	FWoTResolvedExecutableLayout ResolvedLayout = WoTResolveExecutableLayout();
	if (ResolvedLayout.IsValid())
	{
		ExecutableLayout.Emplace(MoveTemp(ResolvedLayout));
	}
#endif
	const TOptional<FString> InstallerPath =
		WoTBootstrapCanonicalAdjacentInstallerPath(
			ExecutableLayout.IsSet() ? &ExecutableLayout.GetValue() : nullptr);
	if (InstallerPath.IsSet())
	{
		CanonicalAdjacentInstallerPath = InstallerPath.GetValue();
	}

	bool bTestAdaptersSet = false;
#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
		if (GWoTBootstrapWorkerFactory)
		{
			bTestAdaptersSet = true;
			Worker = GWoTBootstrapWorkerFactory();
		}
		if (GWoTBootstrapFreeSpaceFactory)
		{
			bTestAdaptersSet = true;
			FreeSpace = GWoTBootstrapFreeSpaceFactory();
		}
	}
#endif
#if WOT_GUARDED_BOOTSTRAP
	if (!bTestAdaptersSet)
	{
		if (!ExecutableLayout.IsSet())
		{
			FailInitialization(EWoTBootstrapDiagnosticCode::BootstrapFailure);
			return;
		}
		Worker = WoTCreateProductionBootstrapWorker(
			ExecutableLayout.GetValue());
		FreeSpace = WoTCreateProductionFreeSpaceProbe(
			ExecutableLayout.GetValue());
	}
	if (!Worker.IsValid())
	{
		FailInitialization(
			EWoTBootstrapDiagnosticCode::BootstrapFailure);
		return;
	}
#else
	(void)bTestAdaptersSet;
#endif
	Start();
}

void UWoTBootstrapSubsystem::FailInitialization(
	EWoTBootstrapDiagnosticCode DiagnosticCode)
{
	bInitializationFailed = true;
	const EWoTBootstrapPhase FailedPhase = ViewModel.Snapshot().Phase;
	const uint64 FailedEpoch = ViewModel.Snapshot().OperationEpoch;
	// Deliberately no worker, screen, or externally published progress snapshot.
	// The receipt publishes BEFORE the heartbeat: both write the same volume,
	// so if the filesystem wedges, the wedge lands on the secondary
	// diagnostic, never the primary durable signal.
#if WOT_GUARDED_BOOTSTRAP
	const FWoTBootstrapStatus Receipt = WoTWriteBootFailureReceipt(
		DiagnosticCode, FailedPhase);
	if (Receipt.IsError())
	{
		// LowLevelOutput is the only reporter alive in the log-less guarded
		// build; UE_LOG keeps the failure visible in automation logs.
		FPlatformMisc::LowLevelOutputDebugStringf(
			TEXT("WoT boot-failure receipt publication failed: %s\n"),
			*Receipt.GetError().ToString());
		UE_LOG(LogTemp, Error,
			TEXT("WoT boot-failure receipt publication failed: %s"),
			*Receipt.GetError().ToString());
	}
#endif
	WoTRecordBootstrapHeartbeat(TEXT("initialization-failed"),
		FailedPhase, FailedEpoch, {}, {},
		FWoTBootstrapViewModel::DiagnosticCodeText(DiagnosticCode).ToString());
	ViewModel.Fail(DiagnosticCode);
#if !WITH_AUTOMATION_TESTS
	const FString Code =
		FWoTBootstrapViewModel::DiagnosticCodeText(DiagnosticCode).ToString();
#if WOT_GUARDED_BOOTSTRAP
	UE_LOG(LogTemp, Error, TEXT("WoT bootstrap initialization failed (%s)"),
		*Code);
	// RequestExitWithStatus is deliberately not used: UE's generic
	// implementation (inherited by Mac) ignores the status. _Exit terminates
	// without Fatal/RaiseException or static-destruction crash paths. The
	// receipt is already durable above.
	std::_Exit(EXIT_FAILURE);
#else
	UE_LOG(LogTemp, Fatal, TEXT("WoT bootstrap initialization failed (%s)"),
		*Code);
#endif
#endif
}

void UWoTBootstrapSubsystem::Start()
{
	if (bStarted || bShuttingDown || bInitializationFailed)
	{
		return;
	}
	bStarted = true;
	BeginCurrentPhase();
}

void UWoTBootstrapSubsystem::BeginCurrentPhase(
	const FWoTDiskBudgetInputs* DiskBudget)
{
	if (bShuttingDown || !Worker.IsValid())
	{
		// Real engines land in later tasks. Remaining in the measured current
		// phase is intentional; no production progress is synthesized here.
		return;
	}
	const FWoTBootstrapSnapshot Current = ViewModel.Snapshot();
	if (Current.IsTerminal())
	{
		return;
	}
	WoTRecordBootstrapHeartbeat(
		TEXT("phase-begin"), Current.Phase, Current.OperationEpoch);
	if (const TCHAR* SpanName =
		WoTBootstrapAggregateSpanName(Current.Phase))
	{
		WoTRecordBootstrapHeartbeat(TEXT("span-begin"), Current.Phase,
			Current.OperationEpoch, {}, {},
			FString::Printf(TEXT("name=%s"), SpanName));
	}
	if (WoTBootstrapNeedsDiskPreflight(Current.Phase))
	{
			if (DiskBudget == nullptr || !FreeSpace.IsValid())
			{
				FailOperation(
					EWoTBootstrapDiagnosticCode::DiskPreflightAbsent);
				return;
		}
		FWoTDiskPreflightResult Preflight =
			CheckDiskBudget(Current.Phase, *DiskBudget, *FreeSpace);
		if (!ViewModel.ReportDiskBudget(Current.OperationEpoch,
			Preflight.RequiredBytes, Preflight.AvailableBytes))
		{
				FailOperation(
					EWoTBootstrapDiagnosticCode::DiskReportRejected);
				return;
		}
		if (!Preflight.bPassed)
		{
				FailOperation(Preflight.DiagnosticCode);
				return;
		}
	}

	FWoTBootstrapWorkerRequest Request;
	Request.Phase = Current.Phase;
	Request.OperationEpoch = Current.OperationEpoch;
	if (Current.Phase == EWoTBootstrapPhase::CheckingGameUpdates)
	{
		if (bAnnouncementOperationStarted)
		{
				FailOperation(EWoTBootstrapDiagnosticCode::AnnouncementReplay);
				return;
		}
		bAnnouncementOperationStarted = true;
		Request.AnnouncementLogicalObjects = {
			WoT::Bootstrap::Trust::AnnouncementUrl,
			FString(WoT::Bootstrap::Trust::AnnouncementUrl) + TEXT(".sig")};
	}
	++WorkerStartCount;
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	Worker->StartPhase(Request,
		[WeakThis](FWoTBootstrapProgressEvent&& Event)
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->DispatchProgress(MoveTemp(Event));
			}
		},
		[WeakThis](FWoTBootstrapWorkerCompletion&& Completion)
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->DispatchCompletion(MoveTemp(Completion));
			}
		});
}

void UWoTBootstrapSubsystem::DispatchProgress(FWoTBootstrapProgressEvent&& Event)
{
	bool bScheduleDelivery = false;
	{
		FScopeLock Lock(&PendingProgressLock);
		if (bShuttingDown)
		{
			return;
		}
		const bool bReplace = !PendingProgress.IsSet()
			|| Event.OperationEpoch > PendingProgress->OperationEpoch
			|| (Event.OperationEpoch == PendingProgress->OperationEpoch
				&& ((Event.CompletedBytes.IsSet()
					&& (!PendingProgress->CompletedBytes.IsSet()
						|| Event.CompletedBytes.GetValue()
							>= PendingProgress->CompletedBytes.GetValue()))
					|| (!Event.CompletedBytes.IsSet()
						&& Event.CompletedUnits >= PendingProgress->CompletedUnits)));
		if (bReplace)
		{
			PendingProgress = MoveTemp(Event);
		}
		if (!bProgressDeliveryQueued)
		{
			bProgressDeliveryQueued = true;
			bScheduleDelivery = true;
		}
	}
	if (!bScheduleDelivery)
	{
		return;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread,
		[WeakThis]()
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->SchedulePendingProgressDelivery();
			}
		});
}

void UWoTBootstrapSubsystem::SchedulePendingProgressDelivery()
{
	if (bShuttingDown)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const double Delay = FMath::Max(0.0,
		0.1 - (Now - LastProgressPublicationSeconds));
	if (Delay <= 0.0)
	{
		DeliverPendingProgress(0.0f);
		return;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	ProgressTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float DeltaSeconds)
		{
			return WeakThis.IsValid()
				&& WeakThis->DeliverPendingProgress(DeltaSeconds);
		}), static_cast<float>(Delay));
}

void UWoTBootstrapSubsystem::BeginProgressDispatchShutdown()
{
	// DispatchProgress is worker-callable and observes this state only while
	// holding PendingProgressLock. Pair the lifecycle write with that lock.
	{
		FScopeLock Lock(&PendingProgressLock);
		bShuttingDown = true;
		PendingProgress.Reset();
		bProgressDeliveryQueued = false;
	}
	if (ProgressTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ProgressTickerHandle);
		ProgressTickerHandle.Reset();
	}
}

bool UWoTBootstrapSubsystem::DeliverPendingProgress(float)
{
	TOptional<FWoTBootstrapProgressEvent> Event;
	{
		FScopeLock Lock(&PendingProgressLock);
		bProgressDeliveryQueued = false;
		ProgressTickerHandle.Reset();
		if (bShuttingDown || !PendingProgress.IsSet())
		{
			return false;
		}
		Event = MoveTemp(PendingProgress);
		PendingProgress.Reset();
	}
	LastProgressPublicationSeconds = FPlatformTime::Seconds();
#if WITH_AUTOMATION_TESTS
	++ProgressDeliveryCountForTestingValue;
#endif
	HandleProgress(MoveTemp(Event.GetValue()));
	return false;
}

void UWoTBootstrapSubsystem::DispatchCompletion(
	FWoTBootstrapWorkerCompletion&& Completion)
{
	if (IsInGameThread())
	{
		HandleCompletion(MoveTemp(Completion));
		return;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread,
		[WeakThis, Completion = MoveTemp(Completion)]() mutable
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->HandleCompletion(MoveTemp(Completion));
			}
		});
}

void UWoTBootstrapSubsystem::HandleProgress(FWoTBootstrapProgressEvent&& Event)
{
	if (!bShuttingDown)
	{
		const uint64 OperationEpoch = Event.OperationEpoch;
		const uint64 Completed = Event.CompletedUnits;
		const uint64 Total = Event.TotalUnits;
		if (!ViewModel.UpdateProgress(MoveTemp(Event)))
		{
			return;
		}
		// Keep successful preflight capacity visible until the first real,
		// accepted worker observation for this operation.
		ViewModel.ClearDiskBudget(OperationEpoch);
		const double Now = FPlatformTime::Seconds();
		if (Now - LastProgressHeartbeatSeconds >= 5.0)
		{
			LastProgressHeartbeatSeconds = Now;
			WoTRecordBootstrapHeartbeat(TEXT("progress"),
				ViewModel.Snapshot().Phase, OperationEpoch, Completed, Total);
		}
	}
}

void UWoTBootstrapSubsystem::HandleCompletion(
	FWoTBootstrapWorkerCompletion&& Completion)
{
	const FWoTBootstrapSnapshot Current = ViewModel.Snapshot();
	if (bShuttingDown || Completion.OperationEpoch != Current.OperationEpoch
		|| Completion.CompletedPhase != Current.Phase)
	{
		WoTRecordBootstrapHeartbeat(TEXT("completion-dropped"),
			Completion.CompletedPhase, Completion.OperationEpoch, {}, {},
			FString::Printf(TEXT("current=%s epoch=%llu shutting_down=%d"),
				WoTBootstrapPhaseName(Current.Phase),
				static_cast<unsigned long long>(Current.OperationEpoch),
				bShuttingDown ? 1 : 0));
		return;
	}
	{
		FScopeLock Lock(&PendingProgressLock);
		if (PendingProgress.IsSet()
			&& PendingProgress->OperationEpoch == Completion.OperationEpoch)
		{
			PendingProgress.Reset();
		}
	}
	// A valid phase completion also ends the preflight-only display state.
	ViewModel.ClearDiskBudget(Current.OperationEpoch);
	WoTRecordBootstrapHeartbeat(TEXT("completion"),
		Completion.CompletedPhase, Completion.OperationEpoch, {}, {},
		FString::Printf(TEXT("next=%s succeeded=%d diagnostic=%s%s%s"),
			WoTBootstrapPhaseName(Completion.NextPhase),
			Completion.bSucceeded ? 1 : 0,
			*FWoTBootstrapViewModel::DiagnosticCodeText(
				Completion.DiagnosticCode).ToString(),
			Completion.FailureDetail.IsEmpty() ? TEXT("") : TEXT(" detail="),
			*Completion.FailureDetail));
	if (!Completion.bSucceeded)
	{
		WoTRecordBootstrapSpanEnd(
			Current.Phase, Current.OperationEpoch, false);
		FailOperation(Completion.DiagnosticCode, MoveTemp(Completion.FailureMessage));
		return;
	}

	if (Completion.bAnnouncementUnavailable
		&& Current.Phase == EWoTBootstrapPhase::CheckingGameUpdates)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Bootstrap announcement pair unavailable (ANNOUNCEMENT-UNAVAILABLE); continuing with protected installed knowledge"));
	}
	if (Current.Phase == EWoTBootstrapPhase::StartingGame)
	{
		// Validate the complete envelope before invoking either the test seam or
		// the production snapshot consumer. A successful but malformed
		// StartingGame completion cannot activate a cache snapshot.
		if (Completion.NextPhase != EWoTBootstrapPhase::Ready
			|| !FWoTBootstrapViewModel::IsLegalTransition(
				Current.Phase, Completion.NextPhase)
			|| !Completion.CacheSnapshot.IsSet())
		{
			WoTRecordBootstrapSpanEnd(
				Current.Phase, Current.OperationEpoch, false);
			FailOperation(EWoTBootstrapDiagnosticCode::IllegalPhaseTransition);
			return;
		}
		bool bActivationHandled = false;
		bool bActivationSucceeded = false;
#if WITH_AUTOMATION_TESTS
		FCacheActivationAdapter TestAdapter;
		TFunction<void(const FWoTCacheGenerationLease&)>
			BeforeActivationObserver;
		{
			FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
			TestAdapter = GWoTBootstrapCacheActivationAdapter;
			BeforeActivationObserver =
				GWoTBootstrapBeforeCacheActivationObserver;
		}
		if (TestAdapter)
		{
			bActivationHandled = true;
			bActivationSucceeded =
				!TestAdapter(MoveTemp(
					Completion.CacheSnapshot.GetValue())).IsError();
		}
#endif
		if (!bActivationHandled)
		{
			#if WITH_AUTOMATION_TESTS
			if (BeforeActivationObserver)
			{
				BeforeActivationObserver(
					Completion.CacheSnapshot.GetValue());
			}
			#endif
			bActivationHandled = true;
			FWoTBootstrapStatus Activated = ActivateVerifiedGeneration(
				MoveTemp(Completion.CacheSnapshot.GetValue()));
			bActivationSucceeded = !Activated.IsError();
			if (!bActivationSucceeded)
			{
				// Packaged builds compile logging out and the screen shows only
				// PAYLOAD-CAPABILITY-ABSENT; keep the reason in the heartbeat.
				WoTRecordBootstrapHeartbeat(TEXT("activation-failed"),
					Current.Phase, Current.OperationEpoch, {}, {},
					Activated.GetError().ToString().Left(1024));
			}
		}
		if (!bActivationHandled || !bActivationSucceeded)
		{
			WoTRecordBootstrapSpanEnd(
				Current.Phase, Current.OperationEpoch, false);
			FailOperation(
				EWoTBootstrapDiagnosticCode::PayloadCapabilityAbsent);
			return;
		}
		ViewModel.SetMandatoryWork(Completion.bMandatoryWork);
#if WITH_AUTOMATION_TESTS
		if (TestAdapter)
		{
			if (!ViewModel.TransitionTo(EWoTBootstrapPhase::Ready,
				IsWoTPayloadBootstrapActive()))
			{
				WoTRecordBootstrapSpanEnd(
					Current.Phase, Current.OperationEpoch, false);
				FailOperation(
					EWoTBootstrapDiagnosticCode::PayloadCapabilityAbsent);
				return;
			}
			WoTRecordBootstrapSpanEnd(
				Current.Phase, Current.OperationEpoch, true);
			const FWoTBootstrapSnapshot Ready = ViewModel.Snapshot();
			WoTRecordBootstrapHeartbeat(TEXT("phase-ready"),
				Ready.Phase, Ready.OperationEpoch);
			StartReleaseNotes();
			StartBackgroundUpdate();
			WoTRecordBootstrapHeartbeat(TEXT("span-begin"),
				Ready.Phase, Ready.OperationEpoch, {}, {},
				TEXT("name=scene-preparation-handoff"));
			return;
		}
#endif
		// ActivateVerifiedGeneration returns only after payload and world
		// adoption are observable. The prevalidated Ready publication is the
		// sole remaining operation and has no failure branch.
		#if WITH_AUTOMATION_TESTS
		TFunction<void(bool, bool)> ReadyObserver;
		{
			FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
			ReadyObserver = GWoTBootstrapReadyAdoptionObserver;
		}
		if (ReadyObserver)
		{
			ReadyObserver(IsWoTPayloadBootstrapActive(),
				IsWoTNormalWorldAuthorityAdopted());
		}
		#endif
		const bool bPublishedReady =
			ViewModel.TransitionTo(EWoTBootstrapPhase::Ready, true);
		checkf(bPublishedReady,
			TEXT("prevalidated StartingGame activation failed Ready publication"));
		WoTRecordBootstrapSpanEnd(
			Current.Phase, Current.OperationEpoch, true);
		const FWoTBootstrapSnapshot Ready = ViewModel.Snapshot();
		WoTRecordBootstrapHeartbeat(TEXT("phase-ready"),
			Ready.Phase, Ready.OperationEpoch);
		StartReleaseNotes();
		StartBackgroundUpdate();
		WoTRecordBootstrapHeartbeat(TEXT("span-begin"),
			Ready.Phase, Ready.OperationEpoch, {}, {},
			TEXT("name=scene-preparation-handoff"));
		return;
	}
	ViewModel.SetMandatoryWork(Completion.bMandatoryWork);
	const bool bPayloadObservable = IsWoTPayloadBootstrapActive();
	if (!ViewModel.TransitionTo(
		Completion.NextPhase, bPayloadObservable))
	{
		WoTRecordBootstrapSpanEnd(
			Current.Phase, Current.OperationEpoch, false);
		FailOperation(Completion.NextPhase == EWoTBootstrapPhase::Ready
			? EWoTBootstrapDiagnosticCode::PayloadCapabilityAbsent
			: EWoTBootstrapDiagnosticCode::IllegalPhaseTransition);
		return;
	}
	if (Current.Phase == EWoTBootstrapPhase::CheckingGameUpdates)
	{
		ViewModel.SetUpdateNotifyOnly(Completion.bUpdateNotifyOnly,
			MoveTemp(Completion.NotifyOnlyReason),
			Completion.NotifyOnlyReleaseSequence,
			MoveTemp(Completion.NotifyOnlyReleasePageUrl));
	}
	if (Completion.NextPhase == EWoTBootstrapPhase::OriginalGameNeeded)
	{
		ViewModel.SetOriginalGameNeeded(Completion.OriginalGameNeededReason,
			MoveTemp(Completion.OriginalGameNeededDetail));
	}
	WoTRecordBootstrapSpanEnd(Current.Phase, Current.OperationEpoch, true);
	BeginCurrentPhase(&Completion.NextPhaseDiskBudget);
}

void UWoTBootstrapSubsystem::StartReleaseNotes()
{
	if (bShuttingDown || !Worker.IsValid())
	{
		return;
	}
#if !WITH_AUTOMATION_TESTS
	if (!ExecutableLayout.IsSet())
	{
		return;
	}
#endif
	const WoTReleaseIdentity::FIdentity Identity =
		WoTReleaseIdentity::LoadRunning();
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>()
		: nullptr;
	const bool bSeen = Identity.bStamped && !Identity.ReleaseCommit.IsEmpty()
		&& Preferences != nullptr
		&& Preferences->HasSeenReleaseNotes(Identity.ReleaseCommit);
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	Worker->StartReleaseNotes(bSeen,
		[WeakThis](FWoTReleaseNotesResult&& Result) mutable
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->DispatchReleaseNotes(MoveTemp(Result));
			}
		});
}

void UWoTBootstrapSubsystem::DispatchReleaseNotes(
	FWoTReleaseNotesResult&& Result)
{
	if (IsInGameThread())
	{
		CompleteReleaseNotes(MoveTemp(Result));
		return;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread,
		[WeakThis, Result = MoveTemp(Result)]() mutable
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->CompleteReleaseNotes(MoveTemp(Result));
			}
		});
}

void UWoTBootstrapSubsystem::CompleteReleaseNotes(
	FWoTReleaseNotesResult&& Result)
{
	check(IsInGameThread());
	if (bShuttingDown)
	{
		return;
	}
	RetainedReleaseNotesResult = MoveTemp(Result);
	ReleaseNotesCompleted.Broadcast(RetainedReleaseNotesResult.GetValue());
}

void UWoTBootstrapSubsystem::StartBackgroundUpdate()
{
	if (bShuttingDown || !Worker.IsValid())
	{
		return;
	}
#if !WITH_AUTOMATION_TESTS
	if (!ExecutableLayout.IsSet())
	{
		return;
	}
#endif
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	Worker->StartBackgroundUpdate(
		[WeakThis](FWoTUpdateState&& State) mutable
		{
			if (UWoTBootstrapSubsystem* Self = WeakThis.Get())
			{
				Self->DispatchUpdateState(MoveTemp(State));
			}
		});
}

void UWoTBootstrapSubsystem::DispatchUpdateState(FWoTUpdateState&& State)
{
	if (IsInGameThread())
	{
		if (!bShuttingDown)
		{
#if WITH_AUTOMATION_TESTS
			++UpdateStateDeliveryCountForTestingValue;
#endif
			ViewModel.SetUpdateState(MoveTemp(State));
		}
		return;
	}
	{
		FScopeLock Lock(&PendingUpdateStateLock);
		PendingUpdateState = MoveTemp(State);
		if (bUpdateStateDeliveryQueued)
		{
			return;
		}
		bUpdateStateDeliveryQueued = true;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread,
		[WeakThis]() mutable
		{
			UWoTBootstrapSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			TOptional<FWoTUpdateState> Latest;
			{
				FScopeLock Lock(&Self->PendingUpdateStateLock);
				Latest = MoveTemp(Self->PendingUpdateState);
				Self->PendingUpdateState.Reset();
				Self->bUpdateStateDeliveryQueued = false;
			}
			if (!Self->bShuttingDown && Latest.IsSet())
			{
#if WITH_AUTOMATION_TESTS
				++Self->UpdateStateDeliveryCountForTestingValue;
#endif
				Self->ViewModel.SetUpdateState(MoveTemp(Latest.GetValue()));
			}
		});
}

FWoTBootstrapStatus UWoTBootstrapSubsystem::ActivateVerifiedGeneration(
	FWoTCacheGenerationLease&& Snapshot)
{
	if (!IsInGameThread() || bShuttingDown)
	{
		return WoTBootstrapActivationError(
			TEXT("cache activation requires the live game-thread bootstrap owner"));
	}
	if (!Snapshot.IsValid())
	{
		return WoTBootstrapActivationError(
			TEXT("cache activation requires one live verified snapshot"));
	}
	bFrontEndSceneRequired = Snapshot.HasFrontEndDescriptorAttestation();
	TOptional<FWoTFrontEndDescriptor> FrontEndDescriptor;
	if (bFrontEndSceneRequired)
	{
		auto DerivedFrontEnd =
			WoTDeriveFrontEndDescriptorFromActiveGeneration(Snapshot);
		if (DerivedFrontEnd.IsError())
		{
			return FWoTBootstrapStatus::Error(
				MoveTemp(DerivedFrontEnd).GetError());
		}
		FrontEndDescriptor.Emplace(MoveTemp(DerivedFrontEnd).GetValue());
	}
	TWoTBootstrapResult<TArray<FWoTWorldDescriptor>> Descriptors =
		WoTDeriveWorldDescriptorsFromActiveGeneration(Snapshot);
	if (Descriptors.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Descriptors).GetError());
	}
	UGameInstance* GameInstance = GetGameInstance();
	UWoTPayloadSubsystem* PayloadSubsystem = GameInstance
		? GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (PayloadSubsystem == nullptr || Sessions == nullptr)
	{
		return WoTBootstrapActivationError(
			TEXT("a later-needed activation subsystem is absent"));
	}
	// Session hooks are prepared while the authority predicate is still false.
	// Their public gate cannot open until the final world adoption publishes.
	if (!Sessions->ActivateForAdoptedWorldAuthority())
	{
		return WoTBootstrapActivationError(
			TEXT("the session consumer could not prepare for normal-world authority"));
	}

	FWoTBootstrapStatus PayloadStatus =
		PayloadSubsystem->ActivateVerifiedCache(Snapshot);
	if (PayloadStatus.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(PayloadStatus).GetError());
	}
	if (bFrontEndSceneRequired)
	{
		UWorld* EntryWorld = GetWorld();
		const FWoTFrontEndFontOwner* FontOwner =
			PayloadSubsystem->FrontEndFontOwnerForScene();
		UWoTPreferencesSubsystem* Preferences = GameInstance
			? GameInstance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
		if (EntryWorld == nullptr || FontOwner == nullptr || Preferences == nullptr
			|| !FrontEndDescriptor.IsSet())
		{
			return WoTBootstrapActivationError(
				TEXT("retail front-end scene dependencies are incomplete"));
		}
		FWoTFrontEndScenePreferences ScenePreferences;
		ScenePreferences.AudioVolume =
			Preferences->GetPreferences().EarlyUI.BootstrapVolume;
		ScenePreferences.bReducedMotion =
			Preferences->GetPreferences().EarlyUI.bReducedMotion;
		const uint64 SceneProgressEpoch = ViewModel.Snapshot().OperationEpoch;
		const double SceneProgressStarted = FPlatformTime::Seconds();
		auto PublishSceneProgress = [this, SceneProgressEpoch,
			SceneProgressStarted](const FWoTFrontEndSceneProgress& Progress)
		{
			FWoTBootstrapProgressEvent Event;
			Event.OperationEpoch = SceneProgressEpoch;
			Event.StageId = TEXT("front-end-scene");
			switch (Progress.Stage)
			{
			case EWoTFrontEndSceneProgressStage::WorldAndBSP:
				Event.CurrentItem = TEXT("Authenticated world and BSP"); break;
			case EWoTFrontEndSceneProgressStage::SurfacesAndLightmaps:
				Event.CurrentItem = TEXT("Authenticated surfaces and lightmaps"); break;
			case EWoTFrontEndSceneProgressStage::WeatherAndAmbience:
				Event.CurrentItem = TEXT("Authenticated weather and ambience"); break;
			case EWoTFrontEndSceneProgressStage::FontsAndLocalization:
				Event.CurrentItem = TEXT("Authenticated fonts and localization"); break;
			case EWoTFrontEndSceneProgressStage::Commit:
				Event.CurrentItem = TEXT("Authenticated entry scene committed"); break;
			}
			Event.CompletedUnits = Progress.TotalUnits > 0
				? 1000ull * Progress.CompletedUnits / Progress.TotalUnits : 0;
			Event.TotalUnits = 1000;
			Event.ElapsedSeconds = FMath::Max(0.0,
				FPlatformTime::Seconds() - SceneProgressStarted);
			HandleProgress(MoveTemp(Event));
		};
		FrontEndScene = MakeUnique<FWoTFrontEndSceneMaterializer>();
		auto Staged = FrontEndScene->Stage(*EntryWorld,
			FrontEndDescriptor.GetValue(), Snapshot, *PayloadSubsystem,
			*FontOwner, ScenePreferences, [this]() { return bShuttingDown; },
			PublishSceneProgress);
		if (Staged.IsError())
		{
			FrontEndScene.Reset();
			return FWoTBootstrapStatus::Error(MoveTemp(Staged).GetError());
		}
		FWoTBootstrapStatus SceneCommit =
			MoveTemp(Staged).GetValue().Commit(
				GameInstance->GetFirstLocalPlayerController());
		if (SceneCommit.IsError())
		{
			FrontEndScene.Reset();
			return FWoTBootstrapStatus::Error(MoveTemp(SceneCommit).GetError());
		}
	}
	FWoTBootstrapStatus WorldStatus = WoTAuthorizeNormalWorldMaterialization(
		Snapshot,
		MoveTemp(Descriptors).GetValue());
	if (WorldStatus.IsError())
	{
		if (FrontEndScene.IsValid()) FrontEndScene->Rollback();
		return FWoTBootstrapStatus::Error(MoveTemp(WorldStatus).GetError());
	}
	if (!IsWoTPayloadBootstrapActive()
		|| !IsWoTNormalWorldAuthorityAdopted())
	{
		return WoTBootstrapActivationError(
			TEXT("the verified snapshot was not observably adopted"));
	}
	return FWoTBootstrapStatus::Success();
}

void UWoTBootstrapSubsystem::FailOperation(
	EWoTBootstrapDiagnosticCode DiagnosticCode, FString Message)
{
	const EWoTBootstrapPhase FailedPhase = ViewModel.Snapshot().Phase;
	const uint64 FailedEpoch = ViewModel.Snapshot().OperationEpoch;
	ViewModel.Fail(DiagnosticCode, MoveTemp(Message));
	// Unattended guarded: receipt, then heartbeat, then _Exit — never
	// returns. Attended (and test) builds fall through and record here.
	ExitIfUnattendedGuardedFailure(DiagnosticCode, FailedPhase, FailedEpoch);
	WoTRecordBootstrapHeartbeat(TEXT("fail"),
		FailedPhase, FailedEpoch, {}, {},
		FWoTBootstrapViewModel::DiagnosticCodeText(DiagnosticCode).ToString());
}

void UWoTBootstrapSubsystem::ExitIfUnattendedGuardedFailure(
	EWoTBootstrapDiagnosticCode DiagnosticCode, EWoTBootstrapPhase FailedPhase,
	uint64 FailedEpoch)
{
	// An operation failure lands the interactive player on the Fatal screen
	// (message + installer instructions where applicable) — correct UX for a
	// human. Under -unattended nothing can acknowledge that screen: before
	// this exit the packaged guarded client sat rendering it forever, which
	// the funnel harness observed as a nondeterministic "boot stall". Match
	// FailInitialization's proven quiet-fatal contract: durable receipt first,
	// then clean _Exit.
#if WOT_GUARDED_BOOTSTRAP && !WITH_AUTOMATION_TESTS
	if (FApp::IsUnattended())
	{
		UE_LOG(LogTemp, Error,
			TEXT("WoT bootstrap operation failed (%s) in %s; unattended guarded boot exits after the receipt"),
			*FWoTBootstrapViewModel::DiagnosticCodeText(
				DiagnosticCode).ToString(),
			WoTBootstrapPhaseName(FailedPhase));
		// Receipt first, heartbeat second: both write the same volume, so a
		// wedged filesystem can only cost the diagnostic, never the primary
		// durable signal.
		const FWoTBootstrapStatus Receipt =
			WoTWriteBootFailureReceipt(DiagnosticCode, FailedPhase);
		if (Receipt.IsError())
		{
			FPlatformMisc::LowLevelOutputDebugStringf(
				TEXT("WoT boot-failure receipt publication failed: %s\n"),
				*Receipt.GetError().ToString());
		}
		WoTRecordBootstrapHeartbeat(TEXT("fail"),
			FailedPhase, FailedEpoch, {}, {},
			FWoTBootstrapViewModel::DiagnosticCodeText(
				DiagnosticCode).ToString());
		// RequestExitWithStatus is deliberately not used: UE's generic
		// implementation (inherited by Mac) ignores the status. _Exit
		// terminates without Fatal/RaiseException or static-destruction
		// crash paths.
		std::_Exit(EXIT_FAILURE);
	}
#else
	(void)DiagnosticCode;
	(void)FailedPhase;
	(void)FailedEpoch;
#endif
}

bool UWoTBootstrapSubsystem::RequestResumeAndExit()
{
	if (bShuttingDown)
	{
		return false;
	}
	const bool bPersisted = Worker.IsValid()
		&& Worker->PersistVerifiedResumeState();
	ViewModel.MarkVerifiedResumeState(bPersisted);
	return bPersisted;
}

bool UWoTBootstrapSubsystem::RequestRestartToUpdate()
{
#if WITH_AUTOMATION_TESTS
	++RestartRequestCountForTestingValue;
#endif
	if (bShuttingDown || !Worker.IsValid()
		|| ViewModel.Snapshot().Update.Status != EWoTUpdateStatus::Ready)
	{
		return false;
	}
	return Worker->RequestRestartToUpdate();
}

void UWoTBootstrapSubsystem::RequestOriginalGameRecheck()
{
	if (!bShuttingDown && Worker.IsValid()
		&& ViewModel.Snapshot().Phase
			== EWoTBootstrapPhase::OriginalGameNeeded)
	{
		Worker->RequestOriginalGameRecheck();
	}
}

void UWoTBootstrapSubsystem::RequestPickedInstaller(
	const FString& AbsolutePath)
{
	if (bShuttingDown || !Worker.IsValid()
		|| ViewModel.Snapshot().Phase
			!= EWoTBootstrapPhase::OriginalGameNeeded
		|| AbsolutePath.IsEmpty() || FPaths::IsRelative(AbsolutePath))
	{
		return;
	}
	TWoTBootstrapResult<FString> CanonicalPath =
		WoTCanonicalizeInstallerPickPath(AbsolutePath);
	if (CanonicalPath.IsError())
	{
		return;
	}
	Worker->RequestPickedInstaller(MoveTemp(CanonicalPath).GetValue());
}

bool UWoTBootstrapSubsystem::IsReady() const
{
	return ViewModel.Snapshot().Phase == EWoTBootstrapPhase::Ready
		&& IsWoTPayloadBootstrapActive()
		&& (!bFrontEndSceneRequired || IsFrontEndSceneCommitted());
}

bool UWoTBootstrapSubsystem::IsFrontEndSceneCommitted() const
{
	return FrontEndScene.IsValid() && FrontEndScene->IsCommitted();
}

void UWoTBootstrapSubsystem::SuspendFrontEndSceneBeforeTravel()
{
	if (FrontEndScene.IsValid()) FrontEndScene->SuspendBeforeTravel();
}

bool UWoTBootstrapSubsystem::EnsureFrontEndSceneForWorld(
	UWorld& World, APlayerController* ViewOwner)
{
	if (!bFrontEndSceneRequired) return true;
	if (!FrontEndScene.IsValid()) return false;
	if (FrontEndScene->IsCommitted()) return true;
	return !FrontEndScene->Rematerialize(World, ViewOwner).IsError();
}

bool UWoTBootstrapSubsystem::IsFrontEndSceneSuspendedForWorld(
	const UWorld& World) const
{
	return bFrontEndSceneRequired && FrontEndScene.IsValid()
		&& FrontEndScene->IsSuspendedForWorld(World);
}

bool UWoTBootstrapSubsystem::IsNormalFrontEndAuthorized(
	const UGameInstance* GameInstance)
{
	if (GameInstance == nullptr)
	{
		return false;
	}
#if !WOT_GUARDED_BOOTSTRAP
	// A non-guarded GameInstance normally filters this subsystem out.  Asking
	// its collection for that absent class while another subsystem is inside
	// Initialize() triggers the engine's re-entrant GetSubsystem ensure.  Keep
	// the existing unguarded fallback without querying a class that was not
	// selected for this GameInstance.  Automation that explicitly enables the
	// bootstrap subsystem still exercises the Ready gate below.
	if (!GetDefault<UWoTBootstrapSubsystem>()->ShouldCreateSubsystem(
		const_cast<UGameInstance*>(GameInstance)))
	{
		return true;
	}
#endif
	const UWoTBootstrapSubsystem* Bootstrap =
		GameInstance->GetSubsystem<UWoTBootstrapSubsystem>();
	if (Bootstrap == nullptr)
	{
#if WOT_GUARDED_BOOTSTRAP
		return false;
#else
		return true;
#endif
	}
	const bool bSceneReady = !Bootstrap->bFrontEndSceneRequired
		|| Bootstrap->IsFrontEndSceneCommitted();
	return (Bootstrap->IsReady() && IsWoTNormalWorldAuthorityAdopted()
			&& bSceneReady)
		|| (Bootstrap->ViewModel.Snapshot().Phase
				== EWoTBootstrapPhase::Ready
			&& IsWoTFrontEndIntegrityRecoveryAuthorized() && bSceneReady);
}

void UWoTBootstrapSubsystem::HandleRuntimeCacheIntegrityFailure(
	FString Message, bool bFrontEndRecoverable)
{
	if (bFrontEndRecoverable) return;
	bRuntimeIntegrityExitFailure = true;
	ViewModel.FailRuntimeIntegrity(MoveTemp(Message));
	if (FApp::IsUnattended() && !GIsAutomationTesting)
	{
		std::_Exit(EXIT_FAILURE);
	}
}

void UWoTBootstrapSubsystem::Deinitialize()
{
	ShutdownForUninstall();
	Super::Deinitialize();
}

void UWoTBootstrapSubsystem::ShutdownForUninstall()
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	if (FrontEndScene.IsValid())
	{
		FrontEndScene->Shutdown();
		FrontEndScene.Reset();
	}
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UWoTPayloadSubsystem* Payload =
			Instance->GetSubsystem<UWoTPayloadSubsystem>())
		{
			Payload->ShutdownFrontEndFontsForScene();
		}
	}
	WoTShutdownNormalWorldMaterializationAuthority();
	BeginProgressDispatchShutdown();
	ViewModel.InvalidateOutstandingOperation();
	if (Worker.IsValid())
	{
		Worker->CancelAndJoin();
		Worker.Reset();
	}
	FreeSpace.Reset();
	ExecutableLayout.Reset();
	ReleaseNotesCompleted.Clear();
	RetainedReleaseNotesResult.Reset();
	bStarted = false;
}

#if WITH_AUTOMATION_TESTS
void UWoTBootstrapSubsystem::SetCreationEnabledForTesting(bool bEnabled)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapCreationEnabledForTesting = bEnabled;
}

void UWoTBootstrapSubsystem::SetAdaptersForTesting(
	FWorkerFactory&& WorkerFactory, FFreeSpaceFactory&& FreeSpaceFactory)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapWorkerFactory = MoveTemp(WorkerFactory);
	GWoTBootstrapFreeSpaceFactory = MoveTemp(FreeSpaceFactory);
	if (!GWoTBootstrapCanonicalInstallerPathForTesting.IsSet())
	{
		FString TestAdjacent = FPaths::Combine(
			FPaths::GetPath(FPlatformProcess::ExecutablePath()),
			WoT::Bootstrap::InstallerName);
		TestAdjacent = FPaths::ConvertRelativePathToFull(TestAdjacent);
		FPaths::NormalizeFilename(TestAdjacent);
		GWoTBootstrapCanonicalInstallerPathForTesting = MoveTemp(TestAdjacent);
	}
}

void UWoTBootstrapSubsystem::SetCacheActivationAdapterForTesting(
	FCacheActivationAdapter&& ActivationAdapter)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapCacheActivationAdapter = MoveTemp(ActivationAdapter);
}

void UWoTBootstrapSubsystem::SetReadyAdoptionObserverForTesting(
	TFunction<void(bool, bool)>&& Observer)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapReadyAdoptionObserver = MoveTemp(Observer);
}

void UWoTBootstrapSubsystem::SetBeforeCacheActivationObserverForTesting(
	TFunction<void(const FWoTCacheGenerationLease&)>&& Observer)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapBeforeCacheActivationObserver = MoveTemp(Observer);
}

void UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters()
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapWorkerFactory = nullptr;
	GWoTBootstrapFreeSpaceFactory = nullptr;
	GWoTBootstrapCacheActivationAdapter = nullptr;
	GWoTBootstrapReadyAdoptionObserver = nullptr;
	GWoTBootstrapBeforeCacheActivationObserver = nullptr;
	GWoTBootstrapCanonicalInstallerPathForTesting.Reset();
	GWoTBootstrapCreationEnabledForTesting = false;
}
#endif

#if WITH_AUTOMATION_TESTS \
	|| (defined(WOT_PROJECT_OWNS_ENTRYPOINT) && WOT_PROJECT_OWNS_ENTRYPOINT)
void UWoTBootstrapSubsystem::SetCanonicalAdjacentInstallerPathForTesting(
	FString Path)
{
	FScopeLock Lock(&GWoTBootstrapTestAdapterLock);
	GWoTBootstrapCanonicalInstallerPathForTesting = MoveTemp(Path);
}
#endif

void UWoTBootstrapSubsystem::CancelWorkerForUninstall()
{
	if (Worker.IsValid()) Worker->CancelAndJoin();
}
