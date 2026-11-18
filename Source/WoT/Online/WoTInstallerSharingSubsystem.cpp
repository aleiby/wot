#include "WoTInstallerSharingSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreMisc.h"
#include "WoTBootstrapSubsystem.h"
#include "WoTLocalNetworkAccess.h"
#include "WoTPreferencesSubsystem.h"
#include "Payload/WoTPayloadActivation.h"
#include "Updater/WoTLocalInstallerForSharing.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

namespace
{
constexpr TCHAR WoTSharingSideFileName[] = TEXT("InstallerSharing");
constexpr ANSICHAR WoTSharingSchemaLine[] = "Schema=WoTInstallerSharing-v1";
constexpr uint64 WoTSharingSideFileMaxBytes = 256;
constexpr float WoTSharingTickSeconds = 0.25f;
/** A Mac waiting for the heads-up re-reads the marker this often. */
constexpr double WoTSharingHeadsUpRecheckSeconds = 5.0;

/** Side files follow WoTHostBans: specs never touch the player's real
 * preference folder, only an explicit test preference path. */
bool WoTSharingSideFileAllowed()
{
#if WITH_AUTOMATION_TESTS
	if (GIsAutomationTesting
		&& !UWoTPreferencesSubsystem::HasPreferencePathForTesting())
	{
		return false;
	}
#endif
	return true;
}
} // namespace

bool UWoTInstallerSharingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// Only the guarded game shares: editor, PIE and development games never
	// broadcast. Specs construct the subsystem directly.
#if WOT_GUARDED_BOOTSTRAP && !UE_SERVER
	return !IsRunningDedicatedServer();
#else
	return false;
#endif
}

void UWoTInstallerSharingSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Events = MakeShared<FEventQueue>();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this, &UWoTInstallerSharingSubsystem::Tick),
		WoTSharingTickSeconds);
}

void UWoTInstallerSharingSubsystem::Deinitialize()
{
	bDeinitialized = true;
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	StopSharer();
	// Takes the notice off the viewport.
	NoticeLine.Reset();
	Super::Deinitialize();
}

TArray<uint8> UWoTInstallerSharingSubsystem::EncodeSharingSwitch(bool bOn)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(WoTSharingSchemaLine),
		UE_ARRAY_COUNT(WoTSharingSchemaLine) - 1);
	Bytes.Add('\n');
	const ANSICHAR* Value = bOn ? "on" : "off";
	Bytes.Append(reinterpret_cast<const uint8*>(Value),
		FCStringAnsi::Strlen(Value));
	Bytes.Add('\n');
	return Bytes;
}

TOptional<bool> UWoTInstallerSharingSubsystem::DecodeSharingSwitch(
	TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty()
		|| Bytes.Num() > static_cast<int32>(WoTSharingSideFileMaxBytes))
	{
		return {};
	}
	FString Text;
	for (const uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7F)
		{
			return {};
		}
		Text.AppendChar(static_cast<TCHAR>(Byte));
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
	if (Lines.Num() != 2
		|| !Lines[0].TrimEnd().Equals(ANSI_TO_TCHAR(WoTSharingSchemaLine),
			ESearchCase::CaseSensitive))
	{
		return {};
	}
	const FString Value = Lines[1].TrimStartAndEnd();
	if (Value.Equals(TEXT("on"), ESearchCase::CaseSensitive))
	{
		return true;
	}
	if (Value.Equals(TEXT("off"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	return {};
}

bool UWoTInstallerSharingSubsystem::LoadSharingSwitch()
{
	FString Path;
	TArray<uint8> Bytes;
	if (!WoTSharingSideFileAllowed()
		|| !UWoTPreferencesSubsystem::SideFilePath(WoTSharingSideFileName, Path)
		|| !UWoTPreferencesSubsystem::LoadSideFileBytes(
			Path, WoTSharingSideFileMaxBytes, Bytes))
	{
		return true;
	}
	return DecodeSharingSwitch(Bytes).Get(true);
}

bool UWoTInstallerSharingSubsystem::SaveSharingSwitch(bool bOn)
{
	FString Path;
	return WoTSharingSideFileAllowed()
		&& UWoTPreferencesSubsystem::SideFilePath(WoTSharingSideFileName, Path)
		&& UWoTPreferencesSubsystem::SaveSideFileBytes(
			Path, EncodeSharingSwitch(bOn));
}

bool UWoTInstallerSharingSubsystem::ShouldShare(bool bBootstrapReady,
	bool bSwitchOn, bool bOSAsksForAccess, bool bHeadsUpSeen)
{
	return bBootstrapReady && bSwitchOn && (!bOSAsksForAccess || bHeadsUpSeen);
}

FString UWoTInstallerSharingSubsystem::NoticeText(
	const FWoTInstallerSharerEvent& Event)
{
	switch (Event.Kind)
	{
	case FWoTInstallerSharerEvent::EKind::SendStarted:
		return WoTInstallerShare::SendingNotice(Event.Label);
	case FWoTInstallerSharerEvent::EKind::SendEnded:
		return Event.Result == EWoTShareTransferResult::Sent
			? WoTInstallerShare::SentNotice(Event.Label)
			: WoTInstallerShare::StoppedNotice(Event.Label);
	default:
		return FString();
	}
}

FString UWoTInstallerSharingSubsystem::NothingToShareHint()
{
	return WoTSharingFinderSkipsDownloads()
		? FString(TEXT("On a Mac, keep the original installer beside WoT Reborn or choose it with Browse to share it."))
		: FString(TEXT("No original installer found on this computer to share."));
}

void UWoTInstallerSharingSubsystem::ApplySharingSwitch(bool bOn)
{
	bSwitchOn = bOn;
	bSwitchLoaded = true;
	Evaluate();
}

bool UWoTInstallerSharingSubsystem::IsSharerRunning() const
{
#if WITH_AUTOMATION_TESTS
	if (InlineSharer.IsValid())
	{
		return true;
	}
#endif
	return SharerThread.IsValid();
}

bool UWoTInstallerSharingSubsystem::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;
	Evaluate();
	if (NoticeLine.IsValid())
	{
		NoticeLine->Tick();
	}
	return true;
}

void UWoTInstallerSharingSubsystem::Evaluate()
{
	DrainEvents();
	if (bDeinitialized)
	{
		return;
	}
	const bool bReady = IsBootstrapReady();
	if (bReady && !bSwitchLoaded)
	{
		bSwitchOn = LoadSharingSwitch();
		bSwitchLoaded = true;
	}
	const bool bRunning = IsSharerRunning();
	const bool bAsks = WoTLocalNetworkAccess::OSAsksForAccess();
	// The marker only ever goes from unset to set, so a running sharer is
	// never re-gated; a waiting Mac re-reads it now and then.
	const bool bSeen = !bAsks || bRunning || (bReady && bSwitchOn && HasSeenHeadsUp());
	const bool bShould = ShouldShare(bReady, bSwitchOn, bAsks, bSeen);
	if (bShould && !bRunning)
	{
		StartSharer();
	}
	else if (!bShould && bRunning)
	{
		StopSharer();
	}
}

bool UWoTInstallerSharingSubsystem::IsBootstrapReady() const
{
#if WITH_AUTOMATION_TESTS
	if (TestConfig.IsSet())
	{
		return TestConfig->IsBootstrapReady && TestConfig->IsBootstrapReady();
	}
#endif
	// Setup has finished: the bootstrap reached Ready and the verified cache
	// is active. Not UWoTBootstrapSubsystem::IsReady(), which also needs the
	// front-end scene and so turns false during every match; sharing keeps
	// running there.
	const UGameInstance* Instance = GetGameInstance();
	const UWoTBootstrapSubsystem* Bootstrap = Instance != nullptr
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
	return Bootstrap != nullptr
		&& Bootstrap->Snapshot().Phase == EWoTBootstrapPhase::Ready
		&& IsWoTPayloadBootstrapActive();
}

bool UWoTInstallerSharingSubsystem::HasSeenHeadsUp()
{
#if WITH_AUTOMATION_TESTS
	if (TestConfig.IsSet())
	{
		return TestConfig->HasSeenLocalNetworkHeadsUp
			&& TestConfig->HasSeenLocalNetworkHeadsUp();
	}
#endif
	if (bHeadsUpSeen)
	{
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < NextHeadsUpCheckSeconds)
	{
		return false;
	}
	NextHeadsUpCheckSeconds = Now + WoTSharingHeadsUpRecheckSeconds;
	const UGameInstance* Instance = GetGameInstance();
	const UWoTPreferencesSubsystem* Preferences = Instance != nullptr
		? Instance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	bHeadsUpSeen = Preferences != nullptr
		&& Preferences->HasSeenLocalNetworkHeadsUp();
	return bHeadsUpSeen;
}

TFunction<void(const FWoTInstallerSharerEvent&)>
UWoTInstallerSharingSubsystem::MakeEventSink() const
{
	// The queue outlives the sharer's thread even if this object goes first.
	return [Queue = Events](const FWoTInstallerSharerEvent& Event)
	{
		if (Queue.IsValid())
		{
			Queue->Enqueue(Event);
		}
	};
}

void UWoTInstallerSharingSubsystem::StartSharer()
{
	if (!Events.IsValid())
	{
		Events = MakeShared<FEventQueue>();
	}
	SearchState = EWoTInstallerSharingSearch::Unknown;
#if WITH_AUTOMATION_TESTS
	if (TestConfig.IsSet())
	{
		FWoTInstallerSharer::FDependencies Dependencies =
			TestConfig->MakeDependencies ? TestConfig->MakeDependencies()
				: FWoTInstallerSharer::FDependencies();
		Dependencies.OnEvent = MakeEventSink();
		InlineSharer = MakeUnique<FWoTInstallerSharer>(
			TestConfig->SharerConfig, MoveTemp(Dependencies));
		return;
	}
#endif
	TWoTBootstrapResult<FWoTSha256> Pin =
		FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
	if (Pin.IsError())
	{
		return;
	}
	FWoTInstallerSharer::FConfig Config;
	Config.Label = WoTInstallerShare::LocalLabel();
	Config.Size = WoT::Bootstrap::InstallerSize;
	Config.Sha256 = MoveTemp(Pin).GetValue();
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (WoTInstallerShare::HarnessLoopback())
	{
		// A funnel cell pairs two processes on this PC: offers go to loopback
		// only (a socket bound to loopback never sees a broadcast), and only
		// loopback receivers are served. Never compiled into Shipping.
		Config.bAllowLoopbackPeers = true;
		Config.DestinationsOverride = {0x7F000001u};
	}
#endif
	TOptional<FWoTInstallerLocation> Adjacent;
	if (const UGameInstance* Instance = GetGameInstance())
	{
		if (const UWoTBootstrapSubsystem* Bootstrap =
				Instance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			Adjacent = Bootstrap->RetainedAdjacentInstallerLocation();
		}
	}
	FWoTInstallerSharer::FDependencies Dependencies;
	Dependencies.Network = WoTCreateProductionInstallerShareNetwork();
	Dependencies.Source = WoTCreateLocalInstallerShareSource(
		WoTProductionLocalInstallerSearch(MoveTemp(Adjacent)));
	Dependencies.OnEvent = MakeEventSink();
	SharerThread = MakeUnique<FWoTInstallerSharerThread>(
		MakeUnique<FWoTInstallerSharer>(MoveTemp(Config),
			MoveTemp(Dependencies)));
	if (!SharerThread->Start())
	{
		SharerThread.Reset();
	}
}

void UWoTInstallerSharingSubsystem::StopSharer()
{
	if (SharerThread.IsValid())
	{
		SharerThread->StopAndJoin();
		SharerThread.Reset();
	}
#if WITH_AUTOMATION_TESTS
	if (InlineSharer.IsValid())
	{
		InlineSharer->Shutdown();
		InlineSharer.Reset();
	}
#endif
	// The stop ended every transfer ("... stopped"); sharing is off now, so
	// the notice goes away instead of showing that line.
	DrainEvents(/*bPresent*/ false);
	if (NoticeLine.IsValid())
	{
		NoticeLine->Reset();
	}
	SearchState = EWoTInstallerSharingSearch::Unknown;
}

void UWoTInstallerSharingSubsystem::DrainEvents(bool bPresent)
{
	if (!Events.IsValid())
	{
		return;
	}
	FWoTInstallerSharerEvent Event;
	while (Events->Dequeue(Event))
	{
		Deliver(Event, bPresent);
	}
}

void UWoTInstallerSharingSubsystem::Deliver(
	const FWoTInstallerSharerEvent& Event, bool bPresent)
{
	if (Event.Kind == FWoTInstallerSharerEvent::EKind::SearchFinished)
	{
		SearchState = Event.bFound ? EWoTInstallerSharingSearch::Found
			: EWoTInstallerSharingSearch::NotFound;
		return;
	}
	const FString Text = NoticeText(Event);
	if (Text.IsEmpty() || bDeinitialized || !bPresent)
	{
		return;
	}
	// One notice over every screen (front end, Settings, a match); the front
	// end's status line and the HUD's message queue are never used for it.
	if (Event.Kind == FWoTInstallerSharerEvent::EKind::SendStarted)
	{
		GetNoticeLine().TransferStarted(Text);
	}
	else
	{
		GetNoticeLine().TransferEnded(Text);
	}
}

FWoTSharingNoticeLine& UWoTInstallerSharingSubsystem::GetNoticeLine()
{
	if (!NoticeLine.IsValid())
	{
		TUniquePtr<IWoTSharingNoticePresenter> Presenter;
		TFunction<double()> Clock;
#if WITH_AUTOMATION_TESTS
		if (TestConfig.IsSet())
		{
			if (TestConfig->MakeNoticePresenter)
			{
				Presenter = TestConfig->MakeNoticePresenter();
			}
			Clock = TestConfig->NoticeNowSeconds;
		}
#endif
		if (!Presenter.IsValid())
		{
			Presenter = WoTCreateViewportSharingNoticePresenter(GetGameInstance());
		}
		NoticeLine = MakeUnique<FWoTSharingNoticeLine>(
			MoveTemp(Presenter), MoveTemp(Clock));
	}
	return *NoticeLine;
}

#if WITH_AUTOMATION_TESTS
void UWoTInstallerSharingSubsystem::ConfigureForTesting(
	FWoTInstallerSharingTestConfig&& InConfig, bool bInSwitchOn)
{
	StopSharer();
	// The next notice is drawn by the spec's presenter.
	NoticeLine.Reset();
	TestConfig = MoveTemp(InConfig);
	bSwitchOn = bInSwitchOn;
	bSwitchLoaded = true;
	if (!Events.IsValid())
	{
		Events = MakeShared<FEventQueue>();
	}
}

void UWoTInstallerSharingSubsystem::PumpForTesting()
{
	if (InlineSharer.IsValid())
	{
		InlineSharer->Pump();
	}
	DrainEvents();
}
#endif
