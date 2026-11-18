#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"

#include "../Payload/WoTPayloadActivation.h"
#include "../Platform/WoTInstallerPicker.h"
#include "../UI/SWoTBootstrapScreen.h"
#include "../WoTBootstrapDiskPreflight.h"
#include "../WoTBootFailureReceipt.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTBootstrapViewModel.h"
#include "../WoTFrontEnd.h"
#include "../WoTGameInstance.h"
#include "../WoTPreferencesSubsystem.h"
#include "WoTEarlyUI.h"
#include "WoTLaunchEngineHooks.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Async/TaskGraphInterfaces.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SWindow.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if PLATFORM_MAC
#include "Mac/MacPlatformApplicationMisc.h"
#endif

namespace
{
constexpr EAutomationTestFlags WoTBootstrapFlowSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

#if PLATFORM_MAC
struct FWoTBootstrapFlowMacPickerThreadState
{
	TAtomic<int32> ModalBeginCount{0};
	TAtomic<int32> ModalEndCount{0};
	TAtomic<int32> PickedCount{0};
	TAtomic<bool> bModalBeginOnGameThread{false};
	TAtomic<bool> bModalEndOnGameThread{false};
	TAtomic<bool> bPanelOnCocoaMainThread{false};
	TAtomic<bool> bPanelEntered{false};
	TAtomic<bool> bPanelCompleted{false};
	TAtomic<bool> bReleasePanel{false};
	TAtomic<bool> bPickerReturned{false};
	TAtomic<bool> bPickerReturnedOnGameThread{false};
	TAtomic<bool> bPickedOnGameThread{false};
	TAtomic<bool> bModalEndedBeforePicked{false};
	FString PickedPath;
	double Deadline = 0.0;
};

class FWoTBootstrapFlowReleaseMacPicker final : public IAutomationLatentCommand
{
public:
	FWoTBootstrapFlowReleaseMacPicker(
		TSharedRef<FWoTBootstrapFlowMacPickerThreadState> InState,
		FAutomationTestBase* InTest)
		: State(MoveTemp(InState))
		, Test(InTest)
	{
	}

	virtual bool Update() override
	{
		if (!State->bPanelEntered.Load()
			&& FPlatformTime::Seconds() < State->Deadline)
		{
			return false;
		}
		Test->TestTrue(TEXT("the fake panel starts before its deadline"),
			State->bPanelEntered.Load());
		Test->TestTrue(TEXT("the picker returns while the panel is pending"),
			State->bPickerReturned.Load());
		Test->TestTrue(TEXT("the picker returns on the game thread"),
			State->bPickerReturnedOnGameThread.Load());
		Test->TestFalse(TEXT("the fake panel stays pending until released"),
			State->bPanelCompleted.Load());
		State->bReleasePanel.Store(true);
		return true;
	}

private:
	TSharedRef<FWoTBootstrapFlowMacPickerThreadState> State;
	FAutomationTestBase* Test = nullptr;
};

class FWoTBootstrapFlowWaitForMacPicker final : public IAutomationLatentCommand
{
public:
	FWoTBootstrapFlowWaitForMacPicker(
		TSharedRef<FWoTBootstrapFlowMacPickerThreadState> InState,
		FAutomationTestBase* InTest, TOptional<FString> InExpectedPath)
		: State(MoveTemp(InState))
		, Test(InTest)
		, ExpectedPath(MoveTemp(InExpectedPath))
	{
	}

	virtual bool Update() override
	{
		if (State->ModalEndCount.Load() == 0
			&& FPlatformTime::Seconds() < State->Deadline)
		{
			return false;
		}
		WoTSetInstallerPickerMacHooksForTesting({}, {});
		Test->TestEqual(TEXT("modal state begins exactly once"),
			State->ModalBeginCount.Load(), 1);
		Test->TestEqual(TEXT("modal state ends exactly once"),
			State->ModalEndCount.Load(), 1);
		Test->TestTrue(TEXT("modal state begins on the game thread"),
			State->bModalBeginOnGameThread.Load());
		Test->TestTrue(TEXT("modal state ends on the game thread"),
			State->bModalEndOnGameThread.Load());
		Test->TestTrue(TEXT("the panel hook runs on the Cocoa main thread"),
			State->bPanelOnCocoaMainThread.Load());
		if (ExpectedPath.IsSet())
		{
			Test->TestEqual(TEXT("a selected path is delivered exactly once"),
				State->PickedCount.Load(), 1);
			Test->TestTrue(TEXT("the selected path returns on the game thread"),
				State->bPickedOnGameThread.Load());
			Test->TestTrue(TEXT("modal state ends before selection delivery"),
				State->bModalEndedBeforePicked.Load());
			Test->TestEqual(TEXT("the selected path is preserved"),
				State->PickedPath, ExpectedPath.GetValue());
		}
		else
		{
			Test->TestEqual(TEXT("cancellation delivers no selection callback"),
				State->PickedCount.Load(), 0);
		}
		return true;
	}

private:
	TSharedRef<FWoTBootstrapFlowMacPickerThreadState> State;
	FAutomationTestBase* Test = nullptr;
	TOptional<FString> ExpectedPath;
};

void WoTBootstrapFlow_RunMacPickerThreadSpec(
	FAutomationTestBase& Test, TOptional<FString> PanelResult)
{
	TSharedRef<FWoTBootstrapFlowMacPickerThreadState> State =
		MakeShared<FWoTBootstrapFlowMacPickerThreadState>();
	State->Deadline = FPlatformTime::Seconds() + 5.0;
	WoTSetInstallerPickerMacHooksForTesting(
		[State, PanelResult]()
		{
			State->bPanelOnCocoaMainThread.Store(
				FMacPlatformApplicationMisc::IsMainThread());
			State->bPanelEntered.Store(true);
			while (!State->bReleasePanel.Load()
				&& FPlatformTime::Seconds() < State->Deadline)
			{
				FPlatformProcess::Sleep(0.001f);
			}
			State->bPanelCompleted.Store(true);
			return PanelResult;
		},
		[State](bool bEntering)
		{
			if (bEntering)
			{
				++State->ModalBeginCount;
				State->bModalBeginOnGameThread.Store(IsInGameThread());
			}
			else
			{
				++State->ModalEndCount;
				State->bModalEndOnGameThread.Store(IsInGameThread());
			}
		});
	WoTShowInstallerPicker(FString(), [State](const FString& Path)
	{
		++State->PickedCount;
		State->bPickedOnGameThread.Store(IsInGameThread());
		State->bModalEndedBeforePicked.Store(
			State->ModalEndCount.Load() == 1);
		State->PickedPath = Path;
	});
	State->bPickerReturnedOnGameThread.Store(IsInGameThread());
	State->bPickerReturned.Store(true);
	Test.TestEqual(TEXT("modal entry is synchronous with the Browse action"),
		State->ModalBeginCount.Load(), 1);
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FWoTBootstrapFlowReleaseMacPicker>(State, &Test));
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FWoTBootstrapFlowWaitForMacPicker>(State, &Test, PanelResult));
}
#endif

bool WoTBootstrapFlow_PublishEarlyUI()
{
	FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
	Authority.ResetForTesting();
	const ANSICHAR Source[] =
		"Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"
		"ReducedMotion=1\nHighContrast=1\nScreenReader=1\n"
		"BootstrapVolume=1.0\nInputSensitivity=1.0\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Source),
		static_cast<int32>(UE_ARRAY_COUNT(Source) - 1));
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	if (Parsed.IsError()
		|| Authority.PublishOnce(MoveTemp(Parsed).GetValue()).IsError())
	{
		return false;
	}
	auto Apply = [](uint16, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus
	{
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{Apply};
	FString Error;
	return WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
		Sink, &Error);
}

struct FWoTBootstrapFlowWorkerState
{
	TArray<FWoTBootstrapWorkerRequest> Requests;
	FWoTBootstrapProgressCallback FirstProgress;
	FWoTBootstrapProgressCallback ActiveProgress;
	FWoTBootstrapCompletionCallback PausedCompletion;
	int32 PersistCalls = 0;
	int32 CancelCalls = 0;
	int32 ReleaseNotesCalls = 0;
	int32 BackgroundCalls = 0;
	int32 InstallerReceiveRequests = 0;
	int32 InstallerReceiveCancels = 0;
	TArray<FString> ReadyServiceOrder;
	TArray<FString> ShutdownOrder;
	bool bPersistResult = true;
	bool bPublishReadyCapabilities = true;
	bool bBackgroundReady = false;
	bool bReleaseNotesSelected = false;
	bool bAdjacentInstallerRoute = false;
	bool bNotifyOnlyUpdateCheck = false;
	int32 BurstProgressCount = 0;
	TOptional<EWoTBootstrapPhase> PausePhase;
	TOptional<EWoTBootstrapPhase> FailurePhase;
	EWoTBootstrapDiagnosticCode FailureDiagnosticCode =
		EWoTBootstrapDiagnosticCode::None;
	FString FailureInstallerPath;
	FString FailureMessage;
};

class FWoTBootstrapFlowWorker final : public IWoTBootstrapWorker
{
public:
	explicit FWoTBootstrapFlowWorker(
		TSharedRef<FWoTBootstrapFlowWorkerState> InState)
		: State(InState)
	{
	}

	void StartPhase(const FWoTBootstrapWorkerRequest& Request,
		FWoTBootstrapProgressCallback&& Progress,
		FWoTBootstrapCompletionCallback&& Completion) override
	{
		State->Requests.Add(Request);
		if (!State->FirstProgress)
		{
			State->FirstProgress = Progress;
		}
		State->ActiveProgress = Progress;
		if (State->BurstProgressCount > 0
			&& FWoTBootstrapViewModel::IsBytePhase(Request.Phase))
		{
			for (int32 Index = 1; Index <= State->BurstProgressCount; ++Index)
			{
				FWoTBootstrapProgressEvent Event;
				Event.OperationEpoch = Request.OperationEpoch;
				Event.CompletedBytes = static_cast<uint64>(Index);
				Event.TotalBytes = static_cast<uint64>(State->BurstProgressCount);
				Progress(MoveTemp(Event));
			}
		}
		if (State->PausePhase.IsSet()
			&& State->PausePhase.GetValue() == Request.Phase)
		{
			State->PausedCompletion = MoveTemp(Completion);
			return;
		}
		if (FWoTBootstrapViewModel::IsBytePhase(Request.Phase))
		{
			FWoTBootstrapProgressEvent Event;
			Event.OperationEpoch = Request.OperationEpoch;
			Event.CompletedBytes = 25;
			Event.TotalBytes = 100;
			Progress(MoveTemp(Event));
		}
		else if (FWoTBootstrapViewModel::IsItemPhase(Request.Phase))
		{
			FWoTBootstrapProgressEvent Event;
			Event.OperationEpoch = Request.OperationEpoch;
			Event.CompletedUnits = 3;
			Event.TotalUnits = 9;
			Progress(MoveTemp(Event));
		}

		FWoTBootstrapWorkerCompletion Result;
		Result.OperationEpoch = Request.OperationEpoch;
		Result.CompletedPhase = Request.Phase;
		if (State->FailurePhase.IsSet()
			&& State->FailurePhase.GetValue() == Request.Phase)
		{
			Result.bSucceeded = false;
			Result.DiagnosticCode = State->FailureDiagnosticCode;
			Result.FailureMessage = State->FailureMessage;
			Completion(MoveTemp(Result));
			return;
		}
		Result.bSucceeded = true;
		switch (Request.Phase)
		{
		case EWoTBootstrapPhase::EstablishingIdentity:
			Result.NextPhase = EWoTBootstrapPhase::CheckingGameUpdates;
			break;
		case EWoTBootstrapPhase::CheckingGameUpdates:
			if (State->bNotifyOnlyUpdateCheck)
			{
				Result.NextPhase =
					EWoTBootstrapPhase::CheckingOriginalGameData;
				Result.bUpdateNotifyOnly = true;
				Result.NotifyOnlyReason = TEXT("reserved-live-leaf");
				Result.NotifyOnlyReleaseSequence = 22;
				Result.NotifyOnlyReleasePageUrl =
					TEXT("https://example.invalid/release/22");
				break;
			}
			Result.NextPhase = EWoTBootstrapPhase::DownloadingUpdate;
			Result.bMandatoryWork = true;
			Result.bAnnouncementUnavailable = true;
			Result.NextPhaseDiskBudget.UpdateDownloadBytes = 100;
			Result.NextPhaseDiskBudget.SafetyMarginBytes = 10;
			break;
        case EWoTBootstrapPhase::DownloadingUpdate:
            Result.NextPhase = EWoTBootstrapPhase::CheckingOriginalGameData;
            break;
		case EWoTBootstrapPhase::CheckingOriginalGameData:
			Result.NextPhase = State->bAdjacentInstallerRoute
				? EWoTBootstrapPhase::ExtractingOriginalGame
				: EWoTBootstrapPhase::OriginalGameNeeded;
			if (!State->bAdjacentInstallerRoute)
			{
				Result.bMandatoryWork = true;
				Result.OriginalGameNeededReason =
					EWoTOriginalGameNeededReason::Missing;
			}
			else
			{
				Result.NextPhaseDiskBudget.CacheConstructionBytes = 400;
				Result.NextPhaseDiskBudget.SafetyMarginBytes = 10;
			}
			break;
		case EWoTBootstrapPhase::OriginalGameNeeded:
			Result.NextPhase = EWoTBootstrapPhase::ExtractingOriginalGame;
			Result.NextPhaseDiskBudget.CacheConstructionBytes = 400;
			Result.NextPhaseDiskBudget.SafetyMarginBytes = 10;
			break;
		case EWoTBootstrapPhase::ExtractingOriginalGame:
			Result.NextPhase = EWoTBootstrapPhase::BuildingContent;
			Result.NextPhaseDiskBudget.CacheConstructionBytes = 400;
			Result.NextPhaseDiskBudget.SafetyMarginBytes = 10;
			break;
		case EWoTBootstrapPhase::BuildingContent:
			Result.NextPhase = EWoTBootstrapPhase::VerifyingContent;
			break;
		case EWoTBootstrapPhase::VerifyingContent:
			Result.NextPhase = EWoTBootstrapPhase::StartingGame;
			break;
		case EWoTBootstrapPhase::StartingGame:
			Result.NextPhase = EWoTBootstrapPhase::Ready;
			Result.CacheSnapshot.Emplace();
			if (State->bPublishReadyCapabilities)
			{
				PublishWoTPayloadBootstrapActivationForTesting();
				PublishWoTBootstrapWorldReadyForTesting();
			}
			break;
		case EWoTBootstrapPhase::Ready:
		case EWoTBootstrapPhase::Fatal:
		default:
			Result.bSucceeded = false;
			Result.DiagnosticCode =
				EWoTBootstrapDiagnosticCode::UnexpectedPhase;
			break;
		}
		Completion(MoveTemp(Result));
	}

	bool PersistVerifiedResumeState() override
	{
		++State->PersistCalls;
		return State->bPersistResult;
	}

	void StartReleaseNotes(bool,
		FWoTReleaseNotesCallback&& Callback) override
	{
		++State->ReleaseNotesCalls;
		State->ReadyServiceOrder.Add(TEXT("release-notes"));
		FWoTReleaseNotesResult Result;
		Result.ReleaseCommit = TEXT("subsystem-release-commit");
		Result.ReleaseSequence = 8;
		Result.Outcome = State->bReleaseNotesSelected
			? EWoTReleaseNotesOutcome::Selected
			: EWoTReleaseNotesOutcome::NoEntry;
		if (State->bReleaseNotesSelected)
		{
			Result.Text = TEXT("subsystem release notes");
		}
		Callback(MoveTemp(Result));
	}

	void StartBackgroundUpdate(FWoTUpdateStateCallback&& Callback) override
	{
		++State->BackgroundCalls;
		State->ReadyServiceOrder.Add(TEXT("background-update"));
		if (State->bBackgroundReady)
		{
			FWoTUpdateState Ready;
			Ready.Status = EWoTUpdateStatus::Ready;
			Ready.ReleaseSequence = 2;
			Callback(MoveTemp(Ready));
		}
	}

	void RequestInstallerReceive() override
	{
		++State->InstallerReceiveRequests;
	}

	void CancelInstallerReceive() override
	{
		++State->InstallerReceiveCancels;
	}

	void CancelAndJoin() override
	{
		++State->CancelCalls;
		State->ShutdownOrder.Add(TEXT("cancel"));
	}

private:
	TSharedRef<FWoTBootstrapFlowWorkerState> State;
};

struct FWoTBootstrapFlowProbeState
{
	TArray<EWoTBootstrapPhase> Phases;
	TMap<EWoTBootstrapPhase, uint64> Available;
};

class FWoTBootstrapFlowProbe final : public IWoTFreeSpaceProbe
{
public:
	explicit FWoTBootstrapFlowProbe(
		TSharedRef<FWoTBootstrapFlowProbeState> InState)
		: State(InState)
	{
	}
	bool QueryAvailableBytes(
		EWoTBootstrapPhase Phase, uint64& OutAvailableBytes) override
	{
		State->Phases.Add(Phase);
		const uint64* Found = State->Available.Find(Phase);
		OutAvailableBytes = Found ? *Found : 1000;
		return true;
	}
private:
	TSharedRef<FWoTBootstrapFlowProbeState> State;
};

class FWoTBootstrapFixedProbe final : public IWoTFreeSpaceProbe
{
public:
	explicit FWoTBootstrapFixedProbe(uint64 InAvailable,
		bool bInSucceeds = true)
		: Available(InAvailable), bSucceeds(bInSucceeds)
	{
	}
	bool QueryAvailableBytes(EWoTBootstrapPhase, uint64& Out) override
	{
		Out = Available;
		return bSucceeds;
	}
private:
	uint64 Available;
	bool bSucceeds;
};

struct FWoTBootstrapFlowGlobalCleanup
{
	~FWoTBootstrapFlowGlobalCleanup()
	{
		UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
		UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
		UWoTPreferencesSubsystem::SetDelayEarlyUIAdoptionForTesting(false);
		GetEarlyUIAuthority().ResetForTesting();
		ResetWoTPayloadActivationForTesting();
	}
};

int32 WoTBootstrapFlow_CountProgressBars(const TSharedRef<SWidget>& Widget)
{
	int32 Count = Widget->GetType() == FName(TEXT("SProgressBar")) ? 1 : 0;
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
	{
		Count += WoTBootstrapFlow_CountProgressBars(Children->GetChildAt(Index));
	}
	return Count;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapFlowSpec,
	"WoT.Bootstrap.Flow.EndToEnd", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapFlowSpec::RunTest(const FString&)
{
	FWoTBootstrapFlowGlobalCleanup Cleanup;
	ResetWoTPayloadActivationForTesting();

	// The closed transition graph contains every workflow branch and no retry
	// edge. Ready is additionally snapshot-gated by TransitionTo.
	const TPair<EWoTBootstrapPhase, EWoTBootstrapPhase> Legal[] = {
		{EWoTBootstrapPhase::EstablishingIdentity,
			EWoTBootstrapPhase::CheckingGameUpdates},
		{EWoTBootstrapPhase::CheckingGameUpdates,
			EWoTBootstrapPhase::DownloadingUpdate},
		{EWoTBootstrapPhase::CheckingGameUpdates,
			EWoTBootstrapPhase::CheckingOriginalGameData},
		{EWoTBootstrapPhase::DownloadingUpdate,
			EWoTBootstrapPhase::CheckingOriginalGameData},
		{EWoTBootstrapPhase::CheckingOriginalGameData,
			EWoTBootstrapPhase::OriginalGameNeeded},
		{EWoTBootstrapPhase::CheckingOriginalGameData,
			EWoTBootstrapPhase::ExtractingOriginalGame},
		{EWoTBootstrapPhase::CheckingOriginalGameData,
			EWoTBootstrapPhase::BuildingContent},
		{EWoTBootstrapPhase::CheckingOriginalGameData,
			EWoTBootstrapPhase::VerifyingContent},
		{EWoTBootstrapPhase::OriginalGameNeeded,
			EWoTBootstrapPhase::ExtractingOriginalGame},
		{EWoTBootstrapPhase::ExtractingOriginalGame,
			EWoTBootstrapPhase::BuildingContent},
		{EWoTBootstrapPhase::BuildingContent,
			EWoTBootstrapPhase::VerifyingContent},
		{EWoTBootstrapPhase::VerifyingContent,
			EWoTBootstrapPhase::StartingGame},
		{EWoTBootstrapPhase::StartingGame, EWoTBootstrapPhase::Ready},
	};
	for (const auto& Edge : Legal)
	{
		TestTrue(TEXT("declared workflow edge is legal"),
			FWoTBootstrapViewModel::IsLegalTransition(Edge.Key, Edge.Value));
	}
	TestFalse(TEXT("no automatic retry edge"),
		FWoTBootstrapViewModel::IsLegalTransition(
			EWoTBootstrapPhase::CheckingOriginalGameData,
			EWoTBootstrapPhase::DownloadingUpdate));
	TestFalse(TEXT("Ready has no outgoing edge"),
		FWoTBootstrapViewModel::IsLegalTransition(
			EWoTBootstrapPhase::Ready,
			EWoTBootstrapPhase::CheckingGameUpdates));
	TestFalse(TEXT("ordinary failures do not weaken Ready's terminal contract"),
		FWoTBootstrapViewModel::IsLegalTransition(
			EWoTBootstrapPhase::Ready, EWoTBootstrapPhase::Fatal));
	FWoTBootstrapViewModel RuntimeFailureModel;
	RuntimeFailureModel.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	RuntimeFailureModel.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	RuntimeFailureModel.TransitionTo(EWoTBootstrapPhase::VerifyingContent);
	RuntimeFailureModel.TransitionTo(EWoTBootstrapPhase::StartingGame);
	RuntimeFailureModel.TransitionTo(
		EWoTBootstrapPhase::Ready, /*bPayloadCapabilityIsObservable*/ true);
	const FString RuntimeMessage =
		TEXT("Some of WoT Reborn's game data is damaged (Shadar Logoth). It will be rebuilt the next time you start the game.");
	RuntimeFailureModel.FailRuntimeIntegrity(RuntimeMessage);
	const FWoTBootstrapSnapshot RuntimeFailure =
		RuntimeFailureModel.Snapshot();
	TestTrue(TEXT("the dedicated post-Ready integrity path becomes fatal"),
		RuntimeFailure.Phase == EWoTBootstrapPhase::Fatal
		&& RuntimeFailure.FailedPhase.Get(EWoTBootstrapPhase::Fatal)
			== EWoTBootstrapPhase::Ready
		&& RuntimeFailure.DiagnosticCode
			== EWoTBootstrapDiagnosticCode::CacheIntegrityFailure);
	TSharedRef<SWoTBootstrapScreen> RuntimeFailureScreen =
		SNew(SWoTBootstrapScreen).Snapshot(RuntimeFailure);
	TestEqual(TEXT("the bootstrap error screen presents the exact damage message"),
		RuntimeFailureScreen->DiagnosticText().ToString(), RuntimeMessage);

	FWoTBootstrapViewModel ProgressModel;
	TestTrue(TEXT("identity advances"), ProgressModel.TransitionTo(
		EWoTBootstrapPhase::CheckingGameUpdates));
	TestTrue(TEXT("check advances to byte phase"), ProgressModel.TransitionTo(
		EWoTBootstrapPhase::DownloadingUpdate));
	const uint64 ByteEpoch = ProgressModel.Snapshot().OperationEpoch;
	TestTrue(TEXT("computed byte progress accepted"),
		ProgressModel.UpdateProgress(ByteEpoch, 0, 0, 40, 100));
	TestFalse(TEXT("byte regression rejected"),
		ProgressModel.UpdateProgress(ByteEpoch, 0, 0, 39, 100));
	TestTrue(TEXT("byte phase advances"), ProgressModel.TransitionTo(
		EWoTBootstrapPhase::CheckingOriginalGameData));
	TestFalse(TEXT("stale progress cannot cross epoch"),
		ProgressModel.UpdateProgress(ByteEpoch, 0, 0, 80, 100));
	FWoTBootstrapProgressEvent StageEvent;
	StageEvent.OperationEpoch = ProgressModel.Snapshot().OperationEpoch;
	StageEvent.StageId = TEXT("texture-sound");
	StageEvent.CompletedUnits = 128;
	StageEvent.TotalUnits = 5508;
	TestTrue(TEXT("stage-bearing item progress is accepted"),
		ProgressModel.UpdateProgress(MoveTemp(StageEvent)));
	TestEqual(TEXT("stage survives the worker boundary"),
		ProgressModel.Snapshot().StageId, FString(TEXT("texture-sound")));

	FWoTBootstrapSnapshot BuildSnapshot;
	BuildSnapshot.Phase = EWoTBootstrapPhase::BuildingContent;
	BuildSnapshot.CompletedUnits = 128;
	BuildSnapshot.TotalUnits = 5508;
	BuildSnapshot.RequiredBytes = 400;
	BuildSnapshot.AvailableBytes = 1000;
	TSharedRef<SWoTBootstrapScreen> BuildScreen =
		SNew(SWoTBootstrapScreen).Snapshot(BuildSnapshot);
	TestTrue(TEXT("build reports units"),
		BuildScreen->ProgressText().ToString().Contains(TEXT("128 / 5508")));
	TestFalse(TEXT("build hides disk capacity"),
		BuildScreen->ProgressText().ToString().Contains(TEXT("Required:")));
	FWoTBootstrapSnapshot StageSnapshot = BuildSnapshot;
	StageSnapshot.StageId = TEXT("texture-sound");
	StageSnapshot.StageText = FWoTBootstrapViewModel::StageText(
		StageSnapshot.StageId);
	TSharedRef<SWoTBootstrapScreen> StageScreen =
		SNew(SWoTBootstrapScreen).Snapshot(StageSnapshot);
	TestTrue(TEXT("build projects the localized worker stage"),
		StageScreen->ProgressText().ToString().Contains(
			TEXT("Processing textures and sounds")));
	const TPair<const TCHAR*, const TCHAR*> RetailStages[] = {
		{TEXT("cache-builder-complete"), TEXT("Verifying imported content")},
		{TEXT("mesh-animation"), TEXT("Processing meshes and animations")},
		{TEXT("required-world-set"), TEXT("Processing maps")},
		{TEXT("front-end-entry"), TEXT("Preparing the main menu")},
		{TEXT("front-end-world"), TEXT("Processing the main-menu world")},
		{TEXT("font-procedural"), TEXT("Processing fonts and effects")},
		{TEXT("registry-localization-defaults"), TEXT("Processing game data and text")},
		{TEXT("front-end-scene"), TEXT("Finishing the main menu")}};
	for (const auto& Stage : RetailStages)
	{
		TestEqual(*FString::Printf(TEXT("retail setup names stage %s"), Stage.Key),
			FWoTBootstrapViewModel::StageText(Stage.Key).ToString(), FString(Stage.Value));
	}

	FWoTBootstrapSnapshot ZeroPreflightSnapshot;
	ZeroPreflightSnapshot.RequiredBytes = 0;
	ZeroPreflightSnapshot.AvailableBytes = 1000;
	ZeroPreflightSnapshot.bDiskPreflightVisible = true;
	TSharedRef<SWoTBootstrapScreen> ZeroPreflightScreen =
		SNew(SWoTBootstrapScreen).Snapshot(ZeroPreflightSnapshot);
	TestTrue(TEXT("zero preflight is honest"),
		ZeroPreflightScreen->ProgressText().ToString().Contains(
			TEXT("No additional disk space required")));
	FWoTBootstrapViewModel NoCapabilityModel;
	NoCapabilityModel.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	NoCapabilityModel.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	NoCapabilityModel.TransitionTo(EWoTBootstrapPhase::VerifyingContent);
	NoCapabilityModel.TransitionTo(EWoTBootstrapPhase::StartingGame);
	TestFalse(TEXT("Ready cannot be stamped without an active snapshot"),
		NoCapabilityModel.TransitionTo(EWoTBootstrapPhase::Ready));

	// Every large allocation category is computed from the probe and reported.
	auto CheckBudget = [this](const TCHAR* Label, EWoTBootstrapPhase Phase,
		const FWoTDiskBudgetInputs& Inputs, uint64 Expected)
	{
		FWoTBootstrapFixedProbe Probe(Expected - 1);
		const FWoTDiskPreflightResult Result =
			CheckDiskBudget(Phase, Inputs, Probe);
		TestEqual(FString::Printf(TEXT("%s required"), Label),
			Result.RequiredBytes, Expected);
		TestEqual(FString::Printf(TEXT("%s available"), Label),
			Result.AvailableBytes, Expected - 1);
		TestFalse(FString::Printf(TEXT("%s fails conservatively"), Label),
			Result.bPassed);
	};
	FWoTDiskBudgetInputs Budget;
	Budget.UpdateDownloadBytes = 10; Budget.SafetyMarginBytes = 1;
	CheckBudget(TEXT("update download"),
		EWoTBootstrapPhase::DownloadingUpdate, Budget, 11);
	Budget = {}; Budget.ExtractionBytes = 20; Budget.SafetyMarginBytes = 1;
	CheckBudget(TEXT("extraction"),
		EWoTBootstrapPhase::ExtractingOriginalGame, Budget, 21);
	Budget = {}; Budget.CandidateInstallBytes = 30; Budget.SafetyMarginBytes = 1;
	CheckBudget(TEXT("candidate install"),
		EWoTBootstrapPhase::DownloadingUpdate, Budget, 31);
	Budget = {}; Budget.CacheConstructionBytes = 50; Budget.SafetyMarginBytes = 1;
	CheckBudget(TEXT("cache construction"),
		EWoTBootstrapPhase::BuildingContent, Budget, 51);
	Budget = {}; Budget.CacheConstructionBytes = 50;
	Budget.SafetyMarginBytes = 1;
	CheckBudget(TEXT("adjacent extraction cache reserve"),
		EWoTBootstrapPhase::ExtractingOriginalGame, Budget, 51);
	Budget = {}; Budget.UpdateDownloadBytes = MAX_uint64;
	Budget.SafetyMarginBytes = 1;
	FWoTBootstrapFixedProbe Plenty(MAX_uint64);
	const FWoTDiskPreflightResult Overflow = CheckDiskBudget(
		EWoTBootstrapPhase::DownloadingUpdate, Budget, Plenty);
	TestFalse(TEXT("overflow never passes"), Overflow.bPassed);
	TestEqual(TEXT("overflow is privacy-safe"),
		static_cast<int32>(Overflow.DiagnosticCode),
		static_cast<int32>(EWoTBootstrapDiagnosticCode::DiskBudgetOverflow));

	// Exercise the actual completion -> transition -> BeginCurrentPhase chain.
	// Pausing after StartPhase leaves the subsystem snapshot carrying the
	// preflight value that was evaluated before the worker was invoked.
	auto CheckCarriedBudget = [this](const TCHAR* Label, uint8 Seed,
		bool bAdjacentRoute, EWoTBootstrapPhase PausePhase,
		uint64 ExpectedRequired)
	{
		(void)Seed;
		UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
		UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
		UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
		TestTrue(FString::Printf(TEXT("%s EarlyUI publishes"), Label),
			WoTBootstrapFlow_PublishEarlyUI());
		TSharedRef<FWoTBootstrapFlowWorkerState> State =
			MakeShared<FWoTBootstrapFlowWorkerState>();
		State->bAdjacentInstallerRoute = bAdjacentRoute;
		State->PausePhase = PausePhase;
		UWoTBootstrapSubsystem::SetAdaptersForTesting(
			[State]() -> TUniquePtr<IWoTBootstrapWorker>
			{
				return MakeUnique<FWoTBootstrapFlowWorker>(State);
			},
			[]() -> TUniquePtr<IWoTFreeSpaceProbe>
			{
				return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64);
			});
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bStartPlay = false;
		Options.bActivatePayload = false;
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		if (TestNotNull(FString::Printf(TEXT("%s world"), Label), World))
		{
			UWoTBootstrapSubsystem* Subsystem = World->GetGameInstance()
				->GetSubsystem<UWoTBootstrapSubsystem>();
			if (TestNotNull(FString::Printf(TEXT("%s subsystem"), Label),
				Subsystem))
			{
				const FWoTBootstrapSnapshot Snapshot = Subsystem->Snapshot();
				TestEqual(FString::Printf(TEXT("%s reaches preflight phase"), Label),
					Snapshot.Phase, PausePhase);
				if (ExpectedRequired == 0)
				{
					TestFalse(FString::Printf(
						TEXT("%s requests no disk preflight"), Label),
						Snapshot.RequiredBytes.IsSet());
				}
				else
				{
					TestTrue(FString::Printf(
						TEXT("%s reports required bytes"), Label),
						Snapshot.RequiredBytes.IsSet());
				}
				if (ExpectedRequired != 0 && Snapshot.RequiredBytes.IsSet())
				{
					TestEqual(FString::Printf(TEXT("%s required bytes"), Label),
						Snapshot.RequiredBytes.GetValue(), ExpectedRequired);
				}
			}
			WoTTest::DestroySpecWorld(World);
		}
	};
	CheckCarriedBudget(TEXT("original-game-needed route"), 40, false,
		EWoTBootstrapPhase::OriginalGameNeeded, 0);
	CheckCarriedBudget(TEXT("adjacent extraction route"), 41, true,
		EWoTBootstrapPhase::ExtractingOriginalGame, 410);
	CheckCarriedBudget(TEXT("post-detection extraction route"), 42, false,
		EWoTBootstrapPhase::ExtractingOriginalGame, 410);

	// The worker's precise per-copy disable reason survives the asynchronous
	// completion boundary and remains in the view-model snapshot after the
	// update check advances into content work.
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	TestTrue(TEXT("notify-only EarlyUI publishes"),
		WoTBootstrapFlow_PublishEarlyUI());
	TSharedRef<FWoTBootstrapFlowWorkerState> NotifyState =
		MakeShared<FWoTBootstrapFlowWorkerState>();
	NotifyState->bNotifyOnlyUpdateCheck = true;
	NotifyState->PausePhase = EWoTBootstrapPhase::CheckingOriginalGameData;
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[NotifyState]() -> TUniquePtr<IWoTBootstrapWorker>
		{
			return MakeUnique<FWoTBootstrapFlowWorker>(NotifyState);
		},
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64);
		});
	WoTTest::FSpecWorldOptions NotifyOptions;
	NotifyOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	NotifyOptions.bStartPlay = false;
	NotifyOptions.bActivatePayload = false;
	UWorld* NotifyWorld = WoTTest::CreateSpecWorld(NotifyOptions);
	if (TestNotNull(TEXT("notify-only snapshot world"), NotifyWorld))
	{
		UWoTBootstrapSubsystem* NotifySubsystem = NotifyWorld->GetGameInstance()
			->GetSubsystem<UWoTBootstrapSubsystem>();
		if (TestNotNull(TEXT("notify-only snapshot subsystem"), NotifySubsystem))
		{
			const FWoTBootstrapSnapshot NotifySnapshot =
				NotifySubsystem->Snapshot();
			TestEqual(TEXT("notify-only flow reaches content checks"),
				NotifySnapshot.Phase,
				EWoTBootstrapPhase::CheckingOriginalGameData);
			TestTrue(TEXT("notify-only state reaches the view model"),
				NotifySnapshot.bUpdateNotifyOnly);
			TestEqual(TEXT("notify-only reason reaches the view model"),
				NotifySnapshot.NotifyOnlyReason,
				FString(TEXT("reserved-live-leaf")));
			TestEqual(TEXT("notify-only sequence reaches the view model"),
				NotifySnapshot.NotifyOnlyReleaseSequence, uint64(22));
			TestEqual(TEXT("notify-only URL reaches the view model"),
				NotifySnapshot.NotifyOnlyReleasePageUrl,
				FString(TEXT("https://example.invalid/release/22")));
		}
		WoTTest::DestroySpecWorld(NotifyWorld);
	}

	// Actionable cache failures survive the real completion/subsystem/view-model
	// boundary and are displayed verbatim by the production fatal screen.
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	TestTrue(TEXT("cache-failure EarlyUI publishes"), WoTBootstrapFlow_PublishEarlyUI());
	auto FailureState = MakeShared<FWoTBootstrapFlowWorkerState>();
	FailureState->FailurePhase = EWoTBootstrapPhase::CheckingOriginalGameData;
	FailureState->FailureDiagnosticCode = EWoTBootstrapDiagnosticCode::BootstrapFailure;
	FailureState->FailureMessage = TEXT("Close other copies of WoT Reborn and relaunch.\nContent cache: C:/WoT/Data/ContentCache");
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[FailureState]() -> TUniquePtr<IWoTBootstrapWorker>
		{ return MakeUnique<FWoTBootstrapFlowWorker>(FailureState); },
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{ return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64); });
	WoTTest::FSpecWorldOptions FailureOptions;
	FailureOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	FailureOptions.bStartPlay = false;
	FailureOptions.bActivatePayload = false;
	UWorld* FailureWorld = WoTTest::CreateSpecWorld(FailureOptions);
	if (TestNotNull(TEXT("cache-failure world exists"), FailureWorld))
	{
		auto* Failed = FailureWorld->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>();
		if (TestNotNull(TEXT("cache-failure subsystem exists"), Failed))
		{
			TestEqual(TEXT("cache contention reaches Fatal"), Failed->Snapshot().Phase, EWoTBootstrapPhase::Fatal);
			TestEqual(TEXT("cache contention reaches FatalMessage"),
				Failed->Snapshot().FatalMessage, FailureState->FailureMessage);
			TSharedRef<SWoTBootstrapScreen> FailureScreen = SNew(SWoTBootstrapScreen)
				.Snapshot_Lambda([Failed]() { return Failed->Snapshot(); });
			TestEqual(TEXT("fatal UI displays the close-other-copies instruction and cache path"),
				FailureScreen->DiagnosticText().ToString(), FailureState->FailureMessage);
		}
		WoTTest::DestroySpecWorld(FailureWorld);
	}

	// A worker can report a hot burst before the game thread drains. The real
	// subsystem must publish only its newest state once, not enqueue one Slate
	// update per callback.
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	TestTrue(TEXT("burst EarlyUI publishes"), WoTBootstrapFlow_PublishEarlyUI());
	TSharedRef<FWoTBootstrapFlowWorkerState> BurstState =
		MakeShared<FWoTBootstrapFlowWorkerState>();
	BurstState->PausePhase = EWoTBootstrapPhase::DownloadingUpdate;
	BurstState->BurstProgressCount = 2000;
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[BurstState]() -> TUniquePtr<IWoTBootstrapWorker>
		{
			return MakeUnique<FWoTBootstrapFlowWorker>(BurstState);
		},
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64);
		});
	WoTTest::FSpecWorldOptions BurstOptions;
	BurstOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	BurstOptions.bStartPlay = false;
	BurstOptions.bActivatePayload = false;
	UWorld* BurstWorld = WoTTest::CreateSpecWorld(BurstOptions);
	if (TestNotNull(TEXT("burst world"), BurstWorld))
	{
		UWoTBootstrapSubsystem* Burst = BurstWorld->GetGameInstance()
			->GetSubsystem<UWoTBootstrapSubsystem>();
		if (TestNotNull(TEXT("burst subsystem"), Burst))
		{
			TestTrue(TEXT("successful preflight remains observable before progress"),
				Burst->Snapshot().bDiskPreflightVisible);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(
				ENamedThreads::GameThread);
			const FWoTBootstrapSnapshot BurstSnapshot = Burst->Snapshot();
			TestEqual(TEXT("burst publishes the newest byte sample"),
				BurstSnapshot.CompletedBytes.Get(0), uint64(2000));
			TestEqual(TEXT("thousands of callbacks coalesce to one publication"),
				Burst->ProgressDeliveryCountForTesting(), 1);
			TestFalse(TEXT("accepted progress clears the preflight snapshot"),
				BurstSnapshot.bDiskPreflightVisible);
		}
		WoTTest::DestroySpecWorld(BurstWorld);
	}

	// Dependency absence is fatal before the session take or any work/UI.
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetDelayEarlyUIAdoptionForTesting(true);
	TestTrue(TEXT("late-adoption EarlyUI publishes"),
		WoTBootstrapFlow_PublishEarlyUI());
	WoTTest::FSpecWorldOptions MissingOptions;
	MissingOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	MissingOptions.bStartPlay = false;
	MissingOptions.bActivatePayload = false;
	UWorld* MissingWorld = WoTTest::CreateSpecWorld(MissingOptions);
	if (TestNotNull(TEXT("missing dependency fixture world"), MissingWorld))
	{
		UWoTBootstrapSubsystem* Missing = MissingWorld->GetGameInstance()
			->GetSubsystem<UWoTBootstrapSubsystem>();
		if (TestNotNull(TEXT("bootstrap requested before preference"), Missing))
		{
			TestTrue(TEXT("missing preference is fatal"),
				Missing->HasInitializationFailed());
			TestEqual(TEXT("zero bootstrap work"),
				Missing->WorkerStartCountForTesting(), 0);
			TestEqual(TEXT("zero bootstrap UI"),
				Missing->ScreenConstructionCountForTesting(), 0);
		}
		WoTTest::DestroySpecWorld(MissingWorld);
	}
	UWoTPreferencesSubsystem::SetDelayEarlyUIAdoptionForTesting(false);

	// Full initialization proves preference adoption precedes factory/work, the
	// fixed pair is used once, and all snapshots come from worker/probe results.
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	TestTrue(TEXT("EarlyUI publishes for ordered initialization"),
		WoTBootstrapFlow_PublishEarlyUI());
	TSharedRef<FWoTBootstrapFlowWorkerState> WorkerState =
		MakeShared<FWoTBootstrapFlowWorkerState>();
	WorkerState->bBackgroundReady = true;
	WorkerState->bReleaseNotesSelected = true;
	TSharedRef<FWoTBootstrapFlowProbeState> ProbeState =
		MakeShared<FWoTBootstrapFlowProbeState>();
	// This suite owns the state-machine edge only. The dedicated activation
	// suite exercises the real two-lease cache/payload/world handoff.
	UWoTBootstrapSubsystem::SetCacheActivationAdapterForTesting(
		[](FWoTCacheGenerationLease&&)
			-> FWoTBootstrapStatus
		{
			return FWoTBootstrapStatus::Success();
		});
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[WorkerState]() -> TUniquePtr<IWoTBootstrapWorker>
		{
			return MakeUnique<FWoTBootstrapFlowWorker>(WorkerState);
		},
		[ProbeState]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<FWoTBootstrapFlowProbe>(ProbeState);
		});
	WoTTest::FSpecWorldOptions FlowOptions;
	FlowOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	FlowOptions.bStartPlay = false;
	FlowOptions.bActivatePayload = false;
	UWorld* FlowWorld = WoTTest::CreateSpecWorld(FlowOptions);
	if (TestNotNull(TEXT("flow world"), FlowWorld))
	{
		UGameInstance* GameInstance = FlowWorld->GetGameInstance();
		UWoTPreferencesSubsystem* Preferences =
			GameInstance->GetSubsystem<UWoTPreferencesSubsystem>();
		UWoTBootstrapSubsystem* Bootstrap =
			GameInstance->GetSubsystem<UWoTBootstrapSubsystem>();
		if (TestNotNull(TEXT("explicit preference dependency exists"), Preferences))
		{
			TestTrue(TEXT("authenticated EarlyUI adopted first"),
				Preferences->HasAdoptedAuthenticatedEarlyUI());
		}
		if (TestNotNull(TEXT("ordered bootstrap exists"), Bootstrap))
		{
			const FWoTBootstrapSnapshot Ready = Bootstrap->Snapshot();
			TestFalse(TEXT("ordered initialization did not fail"),
				Bootstrap->HasInitializationFailed());
			TestEqual(TEXT("all nine nonterminal phases used real worker"),
				Bootstrap->WorkerStartCountForTesting(), 9);
			// StartingGame is the ninth and final worker operation. Its new
			// activation handoff is consumed on that edge; it is not another phase.
			TestEqual(TEXT("computed flow reaches Ready"),
				static_cast<int32>(Ready.Phase),
				static_cast<int32>(EWoTBootstrapPhase::Ready));
			TestEqual(TEXT("Ready starts the background service exactly once"),
				WorkerState->BackgroundCalls, 1);
			TestEqual(TEXT("Ready starts release notes exactly once"),
				WorkerState->ReleaseNotesCalls, 1);
			TestEqual(TEXT("Ready enqueues two serialized services"),
				WorkerState->ReadyServiceOrder.Num(), 2);
			if (WorkerState->ReadyServiceOrder.Num() == 2)
			{
				TestEqual(TEXT("release notes enqueue precedes update enqueue"),
					WorkerState->ReadyServiceOrder[0], FString(TEXT("release-notes")));
				TestEqual(TEXT("background update enqueue follows release notes"),
					WorkerState->ReadyServiceOrder[1], FString(TEXT("background-update")));
			}
			TestTrue(TEXT("release-notes completion is retained"),
				Bootstrap->ReleaseNotesResult().IsSet());
			if (Bootstrap->ReleaseNotesResult().IsSet())
			{
				TestEqual(TEXT("retained release-notes outcome is selected"),
					static_cast<int32>(Bootstrap->ReleaseNotesResult()->Outcome),
					static_cast<int32>(EWoTReleaseNotesOutcome::Selected));
				TestEqual(TEXT("retained release-notes commit comes from worker"),
					Bootstrap->ReleaseNotesResult()->ReleaseCommit,
					FString(TEXT("subsystem-release-commit")));
			}
			TestEqual(TEXT("background Ready reaches the snapshot"),
				static_cast<int32>(Ready.Update.Status),
				static_cast<int32>(EWoTUpdateStatus::Ready));
			TestEqual(TEXT("background Ready exposes the staged sequence"),
				Ready.Update.ReleaseSequence, uint64(2));
			TestTrue(TEXT("state-machine Ready retains its payload snapshot"),
				IsWoTPayloadBootstrapActive());
			TestFalse(TEXT("legacy exposure publication cannot imitate authority adoption"),
				FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance));
			TestTrue(TEXT("window close persists only verified resume state"),
				Bootstrap->RequestResumeAndExit());
			TestTrue(TEXT("verified resume result is reported"),
				Bootstrap->Snapshot().bResumeStateVerified);
		}
		TestEqual(TEXT("one operation starts for every phase"),
			WorkerState->Requests.Num(), 9);
		int32 AnnouncementRequests = 0;
		for (const FWoTBootstrapWorkerRequest& Request : WorkerState->Requests)
		{
			if (!Request.AnnouncementLogicalObjects.IsEmpty())
			{
				++AnnouncementRequests;
				TestEqual(TEXT("exactly two announcement logical objects"),
					Request.AnnouncementLogicalObjects.Num(), 2);
				if (Request.AnnouncementLogicalObjects.Num() == 2)
				{
					TestEqual(TEXT("signature is literal adjacent logical object"),
						Request.AnnouncementLogicalObjects[1],
						Request.AnnouncementLogicalObjects[0] + TEXT(".sig"));
				}
			}
		}
		TestEqual(TEXT("no retry, mirror, alternate, or second pair"),
			AnnouncementRequests, 1);
		TestEqual(TEXT("waiting state adds no disk preflight"),
			ProbeState->Phases.Num(), 3);
		const FWoTBootstrapSnapshot BeforeStale =
			FlowWorld->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>()
				->Snapshot();
		if (WorkerState->FirstProgress)
		{
			FWoTBootstrapProgressEvent StaleEvent;
			StaleEvent.OperationEpoch = 1;
			StaleEvent.TotalUnits = 1;
			WorkerState->FirstProgress(MoveTemp(StaleEvent));
		}
		const FWoTBootstrapSnapshot AfterStale =
			FlowWorld->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>()
				->Snapshot();
		TestEqual(TEXT("late callback cannot cross operation epochs"),
			AfterStale.OperationEpoch, BeforeStale.OperationEpoch);
		TestEqual(TEXT("late callback cannot change phase"),
			static_cast<int32>(AfterStale.Phase),
			static_cast<int32>(BeforeStale.Phase));
		WoTTest::DestroySpecWorld(FlowWorld);
		TestEqual(TEXT("subsystem shutdown cancels and joins worker once"),
			WorkerState->CancelCalls, 1);
		TestEqual(TEXT("Deinitialize records only worker teardown"),
			WorkerState->ShutdownOrder.Num(), 1);
		if (WorkerState->ShutdownOrder.Num() == 1)
		{
			TestEqual(TEXT("Deinitialize cancels without launching an update"),
				WorkerState->ShutdownOrder[0], FString(TEXT("cancel")));
		}
	}
	TestEqual(TEXT("resume persistence invoked once"),
		WorkerState->PersistCalls, 1);

	// OriginalGameNeeded is a recoverable, mandatory screen rather than a
	// fatal installer-remediation path.
	FWoTBootstrapViewModel OriginalGame;
	TestTrue(TEXT("original-game fixture reaches update check"),
		OriginalGame.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates));
	TestTrue(TEXT("original-game fixture reaches content check"),
		OriginalGame.TransitionTo(
			EWoTBootstrapPhase::CheckingOriginalGameData));
	TestTrue(TEXT("original-game fixture reaches the waiting state"),
		OriginalGame.TransitionTo(EWoTBootstrapPhase::OriginalGameNeeded));
	OriginalGame.SetOriginalGameNeeded(EWoTOriginalGameNeededReason::Missing);
	OriginalGame.SetMandatoryWork(true);
	FWoTBootstrapSnapshot SlateSnapshot = OriginalGame.Snapshot();
	int32 ContinueRequests = 0;
	int32 SkipRequests = 0;
	int32 ExitRequests = 0;
	int32 BrowseRequests = 0;
	int32 GogRequests = 0;
	TSharedRef<SWoTBootstrapScreen> Screen =
		SNew(SWoTBootstrapScreen)
		.Snapshot_Lambda([&SlateSnapshot]() { return SlateSnapshot; })
		.OnContinueRequested(FSimpleDelegate::CreateLambda(
			[&ContinueRequests]() { ++ContinueRequests; }))
		.OnSkipRequested(FSimpleDelegate::CreateLambda(
			[&SkipRequests]() { ++SkipRequests; }))
		.OnExitRequested(FSimpleDelegate::CreateLambda(
			[&ExitRequests]() { ++ExitRequests; }))
		.OnBrowseInstaller(FSimpleDelegate::CreateLambda(
			[&BrowseRequests]() { ++BrowseRequests; }))
		.OnGetOriginalGame(FSimpleDelegate::CreateLambda(
			[&GogRequests]() { ++GogRequests; }));
	TestEqual(TEXT("one overall progress bar"),
		WoTBootstrapFlow_CountProgressBars(Screen), 1);
	TestEqual(TEXT("waiting title is exact"),
		Screen->PhaseText().ToString(),
		FString(TEXT("Original game needed")));
	const FString MissingText =
		TEXT("WoT Reborn builds its game data from your copy of The Wheel of Time "
			"(1999). Download the offline installer from your GOG library — WoT "
			"Reborn finds it in your Downloads folder automatically. If it's "
			"somewhere else, click Browse.");
	TestEqual(TEXT("missing text is exact"),
		Screen->OriginalGameText().ToString(), MissingText);
	TestTrue(TEXT("waiting state exposes Browse"),
		Screen->BrowseInstallerWidgetForTesting()->GetVisibility()
			== EVisibility::Visible);
	TestTrue(TEXT("waiting state exposes GOG"),
		Screen->GetOriginalGameWidgetForTesting()->GetVisibility()
			== EVisibility::Visible);
	TestTrue(TEXT("GOG steps stay hidden until Get it on GOG is clicked"),
		Screen->GogStepsVisibility() == EVisibility::Collapsed);
	Screen->BrowseInstallerForTesting();
	TestTrue(TEXT("Browse does not reveal the GOG steps"),
		Screen->GogStepsVisibility() == EVisibility::Collapsed);
	Screen->GetOriginalGameForTesting();
	TestEqual(TEXT("Browse dispatches once"), BrowseRequests, 1);
	TestEqual(TEXT("GOG dispatches once"), GogRequests, 1);
	TestTrue(TEXT("Get it on GOG reveals the GOG steps"),
		Screen->GogStepsVisibility() == EVisibility::Visible);
	{
		const FString Steps = Screen->GogStepsText().ToString();
		for (const TCHAR* Needed : {TEXT("Download offline backup game installers"),
			TEXT("Backups & goodies"), TEXT("Game Backup Copy"),
			TEXT("setup_the_wheel_of_time_333b_(54846).exe"),
			TEXT("installed through Galaxy isn't enough")})
		{
			TestTrue(*FString::Printf(TEXT("GOG steps mention: %s"), Needed),
				Steps.Contains(Needed, ESearchCase::CaseSensitive));
		}
	}
	SlateSnapshot.OriginalGameNeededReason =
		EWoTOriginalGameNeededReason::OnlineOnly;
	SlateSnapshot.OriginalGameNeededDetail =
		TEXT("The installer at C:/temp/setup.exe is an online-only file. Make it "
			"available on this device (in File Explorer, right-click it and choose "
			"'Always keep on this device'), then start WoT Reborn again.");
	TestEqual(TEXT("online-only acquisition detail reaches the production screen"),
		Screen->OriginalGameText().ToString(),
		SlateSnapshot.OriginalGameNeededDetail);
	const TPair<EWoTOriginalGameNeededReason, FString> ReasonTexts[] = {
		{EWoTOriginalGameNeededReason::Downloading,
			TEXT("Found the GOG installer in your Downloads folder. Waiting for "
				"the download to finish…")},
		{EWoTOriginalGameNeededReason::NotTheInstaller,
			TEXT("A file with the installer's name is there, but it isn't the "
				"expected GOG installer (its size or checksum differs). Download it "
				"again from your GOG library, or click Browse.")},
		{EWoTOriginalGameNeededReason::PickedWrongFile,
			TEXT("That file isn't the GOG installer for The Wheel of Time (1999). "
				"Choose setup_the_wheel_of_time_333b_(54846).exe.")},
		{EWoTOriginalGameNeededReason::UnsupportedReparsePoint,
			TEXT("That installer is stored through an unsupported Windows reparse "
				"point. Move or copy it to a normal local file, then click Browse.")},
		{EWoTOriginalGameNeededReason::HardLinked,
			TEXT("That installer is a hard link (one file with more than one name), "
				"which WoT Reborn won't read. Copy it to an ordinary file instead of "
				"linking it, then click Browse.")},
	};
	for (const TPair<EWoTOriginalGameNeededReason, FString>& Case : ReasonTexts)
	{
		SlateSnapshot.OriginalGameNeededReason = Case.Key;
		TestEqual(TEXT("reason text is exact"),
			Screen->OriginalGameText().ToString(), Case.Value);
		const FString Body = Screen->OriginalGameText().ToString();
		for (const TCHAR* Forbidden : {TEXT("Applications"),
			TEXT("folder beside"), TEXT("Move the")})
		{
			TestFalse(TEXT("body contains no placement workaround"),
				Body.Contains(Forbidden, ESearchCase::CaseSensitive));
		}
	}

	TestTrue(TEXT("Continue is a real SButton"),
		Screen->ContinueWidgetForTesting()->GetType()
			== FName(TEXT("SButton")));
	TestTrue(TEXT("Skip is a real SButton"),
		Screen->SkipWidgetForTesting()->GetType()
			== FName(TEXT("SButton")));
	TestTrue(TEXT("Exit is a real SButton"),
		Screen->ExitWidgetForTesting()->GetType()
			== FName(TEXT("SButton")));
	Screen->ContinueWidgetForTesting()->UpdateAllAttributes();
	Screen->SkipWidgetForTesting()->UpdateAllAttributes();
	Screen->ExitWidgetForTesting()->UpdateAllAttributes();
	TestTrue(TEXT("mandatory work collapses Continue"),
		Screen->ContinueWidgetForTesting()->GetVisibility()
			== EVisibility::Collapsed);
	TestTrue(TEXT("mandatory work collapses Skip"),
		Screen->SkipWidgetForTesting()->GetVisibility()
			== EVisibility::Collapsed);
	TestTrue(TEXT("OriginalGameNeeded shows no third Exit button"),
		Screen->ExitWidgetForTesting()->GetVisibility()
			== EVisibility::Collapsed);
	const FKeyEvent Escape(EKeys::Escape, FModifierKeysState(),
		0, false, 0, 0);
	Screen->OnKeyDown(FGeometry(), Escape);
	TestEqual(TEXT("Escape uses the exit-only path"), ExitRequests, 1);
	TestEqual(TEXT("screen does not synthesize Continue"),
		ContinueRequests, 0);
	TestEqual(TEXT("screen does not synthesize Skip"), SkipRequests, 0);

#if WITH_ACCESSIBILITY
	TestTrue(TEXT("root exposes accessible children"),
		Screen->CanChildrenBeAccessible());
	TestTrue(TEXT("initial action has accessible text"),
		!Screen->InitialFocusWidgetForTesting()->GetAccessibleText().IsEmpty());
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapBrowsePickerSeamSpec,
	"WoT.Bootstrap.Flow.BrowseUsesPickerSeam", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapBrowsePickerSeamSpec::RunTest(const FString&)
{
	const FString Previous = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"));
	const FString Picked = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("picked-installer.exe")));
	FPlatformMisc::SetEnvironmentVar(
		TEXT("WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"), *Picked);
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"), *Previous);
	};
	FWoTBootstrapSnapshot Snapshot;
	Snapshot.Phase = EWoTBootstrapPhase::OriginalGameNeeded;
	FString Received;
	TSharedRef<SWoTBootstrapScreen> Screen =
		SNew(SWoTBootstrapScreen)
		.Snapshot(Snapshot)
		.OnBrowseInstaller(FSimpleDelegate::CreateLambda([&Received]()
		{
			WoTShowInstallerPicker(FString(),
				[&Received](const FString& Path) { Received = Path; });
		}));
	Screen->BrowseInstallerForTesting();
	TestEqual(TEXT("Browse forwards the picker seam's absolute path"),
		Received, Picked);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapAutoBrowseOnceSpec,
	"WoT.Bootstrap.Flow.AutoBrowseSwitchInvokesOnceOnOriginalGameNeededEntry",
	WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapAutoBrowseOnceSpec::RunTest(const FString&)
{
	const FString PreviousCommandLine(FCommandLine::Get());
	ON_SCOPE_EXIT { FCommandLine::Set(*PreviousCommandLine); };
	FCommandLine::Set(*(PreviousCommandLine + TEXT(" -wottestautobrowse")));

	FWoTBootstrapSnapshot Snapshot;
	Snapshot.Phase = EWoTBootstrapPhase::CheckingOriginalGameData;
	int32 BrowseRequests = 0;
	TSharedRef<SWoTBootstrapScreen> Screen =
		SNew(SWoTBootstrapScreen)
		.Snapshot_Lambda([&Snapshot]() { return Snapshot; })
		.OnBrowseInstaller(FSimpleDelegate::CreateLambda(
			[&BrowseRequests]() { ++BrowseRequests; }));
	Screen->Tick(FGeometry(), 0.0, 0.0f);
	TestEqual(TEXT("switch does not browse before the waiting state"),
		BrowseRequests, 0);
	Snapshot.Phase = EWoTBootstrapPhase::OriginalGameNeeded;
	Screen->Tick(FGeometry(), 0.0, 0.0f);
	Screen->Tick(FGeometry(), 0.0, 0.0f);
	TestEqual(TEXT("switch browses exactly once on entry"), BrowseRequests, 1);
	Snapshot.Phase = EWoTBootstrapPhase::CheckingOriginalGameData;
	Screen->Tick(FGeometry(), 0.0, 0.0f);
	Snapshot.Phase = EWoTBootstrapPhase::OriginalGameNeeded;
	Screen->Tick(FGeometry(), 0.0, 0.0f);
	TestEqual(TEXT("switch is one-shot for the screen lifetime"),
		BrowseRequests, 1);
	return !HasAnyErrors();
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapMacPickerCancellationThreadSpec,
	"WoT.Bootstrap.Flow.MacPickerCancellationRestoresModalOnGameThread",
	WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapMacPickerCancellationThreadSpec::RunTest(const FString&)
{
	WoTBootstrapFlow_RunMacPickerThreadSpec(*this, {});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapMacPickerResultThreadSpec,
	"WoT.Bootstrap.Flow.MacPickerResultReturnsOnGameThread",
	WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapMacPickerResultThreadSpec::RunTest(const FString&)
{
	WoTBootstrapFlow_RunMacPickerThreadSpec(
		*this, FString(TEXT("/tmp/picked-installer.exe")));
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyBootstrapSpec,
	"WoT.BootstrapDormancy.Bootstrap", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapDormancyBootstrapSpec::RunTest(const FString&)
{
	FWoTBootstrapFlowGlobalCleanup Cleanup;
	ResetWoTPayloadActivationForTesting();
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCanonicalAdjacentInstallerPathForTesting(
		FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("WoT-Setup.exe")));
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	if (!TestTrue(TEXT("dormancy EarlyUI publishes"),
		WoTBootstrapFlow_PublishEarlyUI()))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("dormancy world"), World))
	{
		return false;
	}
	UWoTBootstrapSubsystem* Bootstrap =
		World->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>();
	if (TestNotNull(TEXT("bootstrap subsystem exists"), Bootstrap))
	{
		TestTrue(TEXT("authenticated shell started"), Bootstrap->HasStarted());
		TestEqual(TEXT("no absent production engine fakes work"),
			Bootstrap->WorkerStartCountForTesting(), 0);
		TestEqual(TEXT("no normal front end before Ready"),
			FWoTFrontEndModel::CanConstructNormalFrontEnd(
				World->GetGameInstance()), false);
		TestEqual(TEXT("dormant phase remains establishing identity"),
			static_cast<int32>(Bootstrap->Snapshot().Phase),
			static_cast<int32>(EWoTBootstrapPhase::EstablishingIdentity));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapProgressFlowSpec,
	"WoT.Bootstrap.Flow.Progress", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapProgressFlowSpec::RunTest(const FString&)
{
	FWoTBootstrapViewModel PhaseModel;
	PhaseModel.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	PhaseModel.TransitionTo(EWoTBootstrapPhase::DownloadingUpdate);
	FWoTBootstrapProgressEvent UpdateComplete;
	UpdateComplete.OperationEpoch = PhaseModel.Snapshot().OperationEpoch;
	UpdateComplete.CompletedBytes = 100;
	UpdateComplete.TotalBytes = 100;
	UpdateComplete.ElapsedSeconds = 10.0;
	TestTrue(TEXT("update completion is accepted"),
		PhaseModel.UpdateProgress(MoveTemp(UpdateComplete)));
	TestEqual(TEXT("a completed download fills its own phase"),
		PhaseModel.Snapshot().PhaseFraction.Get(-1.0f), 1.0f);
	PhaseModel.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	TestFalse(TEXT("every transition restarts the phase bar"),
		PhaseModel.Snapshot().PhaseFraction.IsSet());
	PhaseModel.TransitionTo(EWoTBootstrapPhase::OriginalGameNeeded);
	FWoTBootstrapProgressEvent NeededState;
	NeededState.OperationEpoch = PhaseModel.Snapshot().OperationEpoch;
	NeededState.OriginalGameNeededReason =
		EWoTOriginalGameNeededReason::NotTheInstaller;
	TestTrue(TEXT("original-game-needed state update is accepted"),
		PhaseModel.UpdateProgress(MoveTemp(NeededState)));
	TestEqual(TEXT("waiting state publishes its replaceable-file reason"),
		PhaseModel.Snapshot().OriginalGameNeededReason,
		EWoTOriginalGameNeededReason::NotTheInstaller);
	TestFalse(TEXT("waiting state has no synthetic progress fraction"),
		PhaseModel.Snapshot().PhaseFraction.IsSet());

	PhaseModel.TransitionTo(EWoTBootstrapPhase::ExtractingOriginalGame);
	PhaseModel.TransitionTo(EWoTBootstrapPhase::BuildingContent);
	TSharedRef<SWoTBootstrapScreen> UnknownBuildScreen =
		SNew(SWoTBootstrapScreen).Snapshot(PhaseModel.Snapshot());
	// Never the marquee (unset): the guarded package has no marquee image, so
	// it drew as a full bar for every phase.
	TestEqual(TEXT("unknown current build total shows an empty bar"),
		UnknownBuildScreen->PhaseProgress().Get(-1.0f), 0.0f);
	FWoTBootstrapProgressEvent BuildFirst;
	BuildFirst.OperationEpoch = PhaseModel.Snapshot().OperationEpoch;
	BuildFirst.CompletedUnits = 2;
	BuildFirst.TotalUnits = 10;
	BuildFirst.ElapsedSeconds = 4.0;
	PhaseModel.UpdateProgress(MoveTemp(BuildFirst));
	FWoTBootstrapProgressEvent BuildSecond;
	BuildSecond.OperationEpoch = PhaseModel.Snapshot().OperationEpoch;
	BuildSecond.CompletedUnits = 5;
	BuildSecond.TotalUnits = 10;
	BuildSecond.ElapsedSeconds = 10.0;
	PhaseModel.UpdateProgress(MoveTemp(BuildSecond));
	TestEqual(TEXT("item rate uses phase-relative item deltas"),
		PhaseModel.Snapshot().UnitsPerSecond, 0.5);
	TSharedRef<SWoTBootstrapScreen> HalfBuildScreen =
		SNew(SWoTBootstrapScreen).Snapshot(PhaseModel.Snapshot());
	TestEqual(TEXT("the screen bar shows the build phase's own half"),
		HalfBuildScreen->PhaseProgress().Get(-1.0f), 0.5f);

	// The adjacent-installer first run skips both downloads. Under the old
	// whole-install weighting the bar first appeared at 60% here.
	FWoTBootstrapViewModel AdjacentModel;
	AdjacentModel.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	AdjacentModel.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	AdjacentModel.TransitionTo(EWoTBootstrapPhase::ExtractingOriginalGame);
	AdjacentModel.TransitionTo(EWoTBootstrapPhase::BuildingContent);
	FWoTBootstrapProgressEvent AdjacentBuildStart;
	AdjacentBuildStart.OperationEpoch = AdjacentModel.Snapshot().OperationEpoch;
	AdjacentBuildStart.CompletedUnits = 0;
	AdjacentBuildStart.TotalUnits = 5508;
	AdjacentBuildStart.ElapsedSeconds = 1.0;
	TestTrue(TEXT("adjacent build start is accepted"),
		AdjacentModel.UpdateProgress(MoveTemp(AdjacentBuildStart)));
	TestEqual(TEXT("an adjacent-installer build starts its bar at zero"),
		AdjacentModel.Snapshot().PhaseFraction.Get(-1.0f), 0.0f);

	FWoTBootstrapViewModel ByteRateModel;
	ByteRateModel.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	ByteRateModel.TransitionTo(EWoTBootstrapPhase::DownloadingUpdate);
	FWoTBootstrapProgressEvent ByteFirst;
	ByteFirst.OperationEpoch = ByteRateModel.Snapshot().OperationEpoch;
	ByteFirst.CompletedBytes = 100;
	ByteFirst.TotalBytes = 200;
	ByteFirst.ElapsedSeconds = 10.0;
	ByteRateModel.UpdateProgress(MoveTemp(ByteFirst));
	FWoTBootstrapProgressEvent ByteSecond;
	ByteSecond.OperationEpoch = ByteRateModel.Snapshot().OperationEpoch;
	ByteSecond.CompletedBytes = 150;
	ByteSecond.TotalBytes = 200;
	ByteSecond.ElapsedSeconds = 15.0;
	ByteRateModel.UpdateProgress(MoveTemp(ByteSecond));
	TestEqual(TEXT("byte rate uses byte deltas rather than empty item units"),
		ByteRateModel.Snapshot().UnitsPerSecond, 10.0);

	FWoTBootstrapSnapshot BuildSnapshot;
	BuildSnapshot.Phase = EWoTBootstrapPhase::BuildingContent;
	BuildSnapshot.StageId = TEXT("texture-sound");
	BuildSnapshot.StageText = FWoTBootstrapViewModel::StageText(
		BuildSnapshot.StageId);
	BuildSnapshot.CompletedUnits = 128;
	BuildSnapshot.TotalUnits = 5508;
	BuildSnapshot.RequiredBytes = 400;
	BuildSnapshot.AvailableBytes = 1000;
	TSharedRef<SWoTBootstrapScreen> BuildScreen =
		SNew(SWoTBootstrapScreen).Snapshot(BuildSnapshot);
	TestTrue(TEXT("build projects the localized worker stage"),
		BuildScreen->ProgressText().ToString().Contains(
			TEXT("Processing textures and sounds")));
	TestTrue(TEXT("build reports units"),
		BuildScreen->ProgressText().ToString().Contains(TEXT("128 / 5508")));
	TestFalse(TEXT("build hides disk capacity"),
		BuildScreen->ProgressText().ToString().Contains(TEXT("Required:")));

	FWoTBootstrapSnapshot ZeroPreflightSnapshot;
	ZeroPreflightSnapshot.RequiredBytes = 0;
	ZeroPreflightSnapshot.AvailableBytes = 1000;
	ZeroPreflightSnapshot.bDiskPreflightVisible = true;
	TSharedRef<SWoTBootstrapScreen> ZeroPreflightScreen =
		SNew(SWoTBootstrapScreen).Snapshot(ZeroPreflightSnapshot);
	TestTrue(TEXT("zero preflight is honest"),
		ZeroPreflightScreen->ProgressText().ToString().Contains(
			TEXT("No additional disk space required")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapProgressSchedulingSpec,
	"WoT.Bootstrap.Flow.ProgressScheduling", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapProgressSchedulingSpec::RunTest(const FString&)
{
	FWoTBootstrapFlowGlobalCleanup Cleanup;
	ResetWoTPayloadActivationForTesting();
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	if (!TestTrue(TEXT("burst EarlyUI publishes"), WoTBootstrapFlow_PublishEarlyUI()))
	{
		return false;
	}
	TSharedRef<FWoTBootstrapFlowWorkerState> BurstState =
		MakeShared<FWoTBootstrapFlowWorkerState>();
	BurstState->PausePhase = EWoTBootstrapPhase::DownloadingUpdate;
	BurstState->BurstProgressCount = 2000;
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[BurstState]() -> TUniquePtr<IWoTBootstrapWorker>
		{
			return MakeUnique<FWoTBootstrapFlowWorker>(BurstState);
		},
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64);
		});
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("burst world"), World))
	{
		return false;
	}
	UWoTBootstrapSubsystem* Bootstrap = World->GetGameInstance()
		->GetSubsystem<UWoTBootstrapSubsystem>();
	if (TestNotNull(TEXT("burst subsystem"), Bootstrap))
	{
		TestTrue(TEXT("successful preflight remains observable before progress"),
			Bootstrap->Snapshot().bDiskPreflightVisible);
		// Complete the old phase before its queued 2,000-event burst reaches the
		// game thread. The old epoch must be discarded, while the next epoch can
		// still use the outstanding single delivery.
		BurstState->PausePhase = EWoTBootstrapPhase::CheckingOriginalGameData;
		FWoTBootstrapWorkerCompletion Completion;
		Completion.OperationEpoch = Bootstrap->Snapshot().OperationEpoch;
		Completion.CompletedPhase = EWoTBootstrapPhase::DownloadingUpdate;
		Completion.NextPhase = EWoTBootstrapPhase::CheckingOriginalGameData;
		Completion.bSucceeded = true;
		Completion.NextPhaseDiskBudget.ExtractionBytes = 20;
		Completion.NextPhaseDiskBudget.CandidateInstallBytes = 30;
		Completion.NextPhaseDiskBudget.RollbackBackupBytes = 40;
		Completion.NextPhaseDiskBudget.SafetyMarginBytes = 10;
		TestTrue(TEXT("paused completion callback is captured"),
			static_cast<bool>(BurstState->PausedCompletion));
		if (BurstState->PausedCompletion)
		{
			BurstState->PausedCompletion(MoveTemp(Completion));
		}
		FWoTBootstrapProgressEvent NewEpochEvent;
		NewEpochEvent.OperationEpoch = Bootstrap->Snapshot().OperationEpoch;
		NewEpochEvent.StageId = TEXT("new-epoch");
		TestTrue(TEXT("next-phase callback is captured"),
			static_cast<bool>(BurstState->ActiveProgress));
		if (BurstState->ActiveProgress)
		{
			BurstState->ActiveProgress(MoveTemp(NewEpochEvent));
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(
			ENamedThreads::GameThread);
		const FWoTBootstrapSnapshot Snapshot = Bootstrap->Snapshot();
		TestEqual(TEXT("old burst never publishes into the new phase"),
			Snapshot.StageId, FString(TEXT("new-epoch")));
		TestFalse(TEXT("old byte progress is not misclassified as new work"),
			Snapshot.CompletedBytes.IsSet());
		TestEqual(TEXT("old burst and new event share one publication"),
			Bootstrap->ProgressDeliveryCountForTesting(), 1);
		TestFalse(TEXT("accepted progress clears the preflight snapshot"),
			Snapshot.bDiskPreflightVisible);

		TFuture<void> UpdateBurst = Async(EAsyncExecution::Thread,
			[Bootstrap]()
			{
				for (int32 Index = 1; Index <= 2000; ++Index)
				{
					FWoTUpdateState State;
					State.Status = EWoTUpdateStatus::Downloading;
					State.ReleaseSequence = 2;
					State.CompletedBytes = static_cast<uint64>(Index);
					State.TotalBytes = 2000;
					Bootstrap->DispatchUpdateStateForTesting(MoveTemp(State));
				}
			});
		UpdateBurst.Wait();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(
			ENamedThreads::GameThread);
		const FWoTBootstrapSnapshot UpdateSnapshot = Bootstrap->Snapshot();
		TestEqual(TEXT("a multi-chunk update burst has one game-thread delivery"),
			Bootstrap->UpdateStateDeliveryCountForTesting(), 1);
		TestEqual(TEXT("the coalesced update delivery keeps the newest chunk"),
			UpdateSnapshot.Update.CompletedBytes, static_cast<uint64>(2000));

		FWoTBootstrapProgressEvent ShutdownEvent;
		ShutdownEvent.OperationEpoch = Snapshot.OperationEpoch;
		ShutdownEvent.StageId = TEXT("must-not-publish");
		BurstState->ActiveProgress(MoveTemp(ShutdownEvent));
		const int32 BeforeShutdown = Bootstrap->ProgressDeliveryCountForTesting();
		// Exercise the production game-instance lifecycle, which invokes the
		// subsystem's Deinitialize exactly once. Detach before the shared world
		// helper runs so it does not issue a second game-instance shutdown.
		UGameInstance* GameInstance = World->GetGameInstance();
		GameInstance->Shutdown();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(
			ENamedThreads::GameThread);
		TestEqual(TEXT("shutdown clears queued publication deterministically"),
			Bootstrap->ProgressDeliveryCountForTesting(), BeforeShutdown);
	}
	World->SetGameInstance(nullptr);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapInstallerCopyRoutingSpec,
	"WoT.Bootstrap.Flow.InstallerCopyReachesTheWorker", WoTBootstrapFlowSpecFlags)

bool FWoTBootstrapInstallerCopyRoutingSpec::RunTest(const FString&)
{
	// "Copy from another computer" and Cancel follow the Browse chain: the
	// screen's event, the controller, this subsystem method, then the worker,
	// and only while the original game is needed.
	FWoTBootstrapFlowGlobalCleanup Cleanup;
	ResetWoTPayloadActivationForTesting();
	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	if (!TestTrue(TEXT("EarlyUI publishes"), WoTBootstrapFlow_PublishEarlyUI()))
	{
		return false;
	}
	TSharedRef<FWoTBootstrapFlowWorkerState> State =
		MakeShared<FWoTBootstrapFlowWorkerState>();
	State->PausePhase = EWoTBootstrapPhase::DownloadingUpdate;
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[State]() -> TUniquePtr<IWoTBootstrapWorker>
		{
			return MakeUnique<FWoTBootstrapFlowWorker>(State);
		},
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<FWoTBootstrapFixedProbe>(MAX_uint64);
		});
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("world"), World))
	{
		return false;
	}
	UWoTBootstrapSubsystem* Bootstrap = World->GetGameInstance()
		->GetSubsystem<UWoTBootstrapSubsystem>();
	if (TestNotNull(TEXT("subsystem"), Bootstrap))
	{
		TestEqual(TEXT("paused while downloading"), Bootstrap->Snapshot().Phase,
			EWoTBootstrapPhase::DownloadingUpdate);
		Bootstrap->RequestInstallerCopy();
		Bootstrap->CancelInstallerCopy();
		TestEqual(TEXT("no receiver outside the waiting screen"),
			State->InstallerReceiveRequests + State->InstallerReceiveCancels, 0);
		State->PausePhase = EWoTBootstrapPhase::OriginalGameNeeded;
		FWoTBootstrapWorkerCompletion Completion;
		Completion.OperationEpoch = Bootstrap->Snapshot().OperationEpoch;
		Completion.CompletedPhase = EWoTBootstrapPhase::DownloadingUpdate;
		Completion.NextPhase = EWoTBootstrapPhase::CheckingOriginalGameData;
		Completion.bSucceeded = true;
		if (TestTrue(TEXT("the download's completion is captured"),
			static_cast<bool>(State->PausedCompletion)))
		{
			State->PausedCompletion(MoveTemp(Completion));
		}
		TestEqual(TEXT("now waiting for the original game"),
			Bootstrap->Snapshot().Phase, EWoTBootstrapPhase::OriginalGameNeeded);
		Bootstrap->RequestInstallerCopy();
		TestEqual(TEXT("the click reaches the worker"),
			State->InstallerReceiveRequests, 1);
		Bootstrap->CancelInstallerCopy();
		TestEqual(TEXT("Cancel reaches the worker"),
			State->InstallerReceiveCancels, 1);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootFailureReceiptSpec,
	"WoT.Bootstrap.Flow.BootFailureReceipt",
	WoTBootstrapFlowSpecFlags)

bool FWoTBootFailureReceiptSpec::RunTest(const FString&)
{
	FString Parent = FPaths::Combine(
		FPaths::AutomationTransientDir(),
		TEXT("wot-boot-failure-receipt-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Parent = FPaths::ConvertRelativePathToFull(Parent);
	FPaths::NormalizeDirectoryName(Parent);
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("receipt fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};

	const EWoTBootstrapDiagnosticCode Codes[] = {
		EWoTBootstrapDiagnosticCode::BootstrapFailure,
		EWoTBootstrapDiagnosticCode::EarlyUINotAdopted,
		EWoTBootstrapDiagnosticCode::GuardedSessionAbsent,
		EWoTBootstrapDiagnosticCode::DiskPreflightAbsent,
		EWoTBootstrapDiagnosticCode::DiskReportRejected,
		EWoTBootstrapDiagnosticCode::AnnouncementReplay,
		EWoTBootstrapDiagnosticCode::DiskPreflightPhase,
		EWoTBootstrapDiagnosticCode::DiskBudgetOverflow,
		EWoTBootstrapDiagnosticCode::DiskProbeFailed,
		EWoTBootstrapDiagnosticCode::InsufficientDiskSpace,
		EWoTBootstrapDiagnosticCode::UnexpectedPhase,
		EWoTBootstrapDiagnosticCode::InstallerInvalid,
		EWoTBootstrapDiagnosticCode::InstallerPathRejected,
		EWoTBootstrapDiagnosticCode::PayloadCapabilityAbsent,
		EWoTBootstrapDiagnosticCode::IllegalPhaseTransition,
		EWoTBootstrapDiagnosticCode::SignatureInvalid,
		EWoTBootstrapDiagnosticCode::ConfigInvalid,
		EWoTBootstrapDiagnosticCode::InstallFailed,
	};
	const FString ReceiptPath = FPaths::Combine(
		InstallRoot, TEXT("UpdaterState"), WoTBootFailureReceiptLeaf);
	TArray<FString> FirstDirectoryFlushOrder;
	for (const EWoTBootstrapDiagnosticCode Code : Codes)
	{
		const EWoTBootstrapPhase Phase =
			Code == EWoTBootstrapDiagnosticCode::EarlyUINotAdopted
			|| Code == EWoTBootstrapDiagnosticCode::GuardedSessionAbsent
				? EWoTBootstrapPhase::EstablishingIdentity
				: EWoTBootstrapPhase::OriginalGameNeeded;
		const FWoTBootstrapStatus Written =
			WoTWriteBootFailureReceiptForTesting(
				InstallRoot, Code, Phase,
				FirstDirectoryFlushOrder.IsEmpty()
					? &FirstDirectoryFlushOrder : nullptr);
		if (!TestFalse(TEXT("diagnostic-family receipt writes atomically"),
			Written.IsError()))
		{
			if (Written.IsError())
			{
				AddError(Written.GetError().ToString());
			}
			return false;
		}
		FString Receipt;
		if (!TestTrue(TEXT("canonical receipt is readable"),
			FFileHelper::LoadFileToString(Receipt, *ReceiptPath)))
		{
			return false;
		}
		const FString Diagnostic =
			FWoTBootstrapViewModel::DiagnosticCodeText(Code).ToString();
		TestTrue(TEXT("receipt binds the diagnostic family"),
			Receipt.Contains(
				TEXT("\"diagnostic_code\":\"") + Diagnostic + TEXT("\""),
				ESearchCase::CaseSensitive));
		TestTrue(TEXT("receipt binds its failure phase"),
			Receipt.Contains(Phase
				== EWoTBootstrapPhase::EstablishingIdentity
					? TEXT("\"phase\":\"EstablishingIdentity\"")
					: TEXT("\"phase\":\"OriginalGameNeeded\""),
				ESearchCase::CaseSensitive));
		TestTrue(TEXT("receipt carries an ISO UTC timestamp"),
			Receipt.Contains(TEXT("\"timestamp_utc\":\""),
				ESearchCase::CaseSensitive));
		TestFalse(TEXT("receipt has no source path field"),
			Receipt.Contains(TEXT("\"source\""),
				ESearchCase::CaseSensitive));
	}
	const TArray<FString> ExpectedFirstDirectoryFlushOrder{
		TEXT("UpdaterState"), TEXT("InstallRoot"), TEXT("InstallParent")};
	TestTrue(TEXT("new receipt ancestry flushes child to parent"),
		FirstDirectoryFlushOrder == ExpectedFirstDirectoryFlushOrder);
	TArray<FString> Temporaries;
	IFileManager::Get().FindFiles(
		Temporaries,
		*FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"), TEXT(".boot-failure*.tmp")),
		true, false);
	TestEqual(TEXT("atomic publication leaves no temporary receipt"),
		Temporaries.Num(), 0);

	const TArray<uint8> Golden =
		WoTBootFailureReceiptCanonicalBytesForTesting(
			EWoTBootstrapDiagnosticCode::GuardedSessionAbsent,
			EWoTBootstrapPhase::EstablishingIdentity,
			TEXT("2026-07-29T00:00:00.000Z"));
	const FUTF8ToTCHAR GoldenUtf8(
		reinterpret_cast<const ANSICHAR*>(Golden.GetData()), Golden.Num());
	const FString GoldenText =
		FString::ConstructFromPtrSize(GoldenUtf8.Get(), GoldenUtf8.Length());
	TestEqual(TEXT("canonical receipt field order is frozen"),
		GoldenText,
		FString(TEXT("{\"diagnostic_code\":\"GUARDED-SESSION-ABSENT\",")
			TEXT("\"format\":\"wot-bootstrap-boot-failure-v1\",")
			TEXT("\"phase\":\"EstablishingIdentity\",")
			TEXT("\"timestamp_utc\":\"2026-07-29T00:00:00.000Z\"}")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
