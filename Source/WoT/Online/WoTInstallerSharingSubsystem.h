// LAN installer sharing for this game instance (spec B1, B5).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SWoTSharingNotice.h"
#include "WoTInstallerSharer.h"
#include "WoTInstallerSharingSubsystem.generated.h"

/** What the running sharer's last search found. */
enum class EWoTInstallerSharingSearch : uint8
{
	/** Not running, or no search finished yet. */
	Unknown,
	Found,
	NotFound,
};

#if WITH_AUTOMATION_TESTS
/** Inline mode for specs: no thread and no ticker; the spec drives Evaluate
 * and Pump, and the sharer runs on the seams it supplies. */
struct FWoTInstallerSharingTestConfig
{
	TFunction<bool()> IsBootstrapReady;
	TFunction<bool()> HasSeenLocalNetworkHeadsUp;
	FWoTInstallerSharer::FConfig SharerConfig;
	TFunction<FWoTInstallerSharer::FDependencies()> MakeDependencies;
	/** The notice's presenter; the production viewport one when unset. */
	TFunction<TUniquePtr<IWoTSharingNoticePresenter>()> MakeNoticePresenter;
	/** The notice's hide-timer clock; FPlatformTime::Seconds when unset. */
	TFunction<double()> NoticeNowSeconds;
};
#endif

/**
 * Runs the sharer once the bootstrap is Ready while the Settings switch is on
 * and, where the OS asks before local-network traffic (macOS), only after the
 * player has been through the game's Local Network heads-up. Stops it when the
 * switch goes off and at shutdown. Each transfer start and end shows a short
 * notice line over every screen (SWoTSharingNotice, game viewport content), so
 * the sharer's player always sees it; nothing else draws it.
 */
UCLASS()
class UWoTInstallerSharingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The switch, from the preferences side file InstallerSharing (schema
	 * WoTInstallerSharing-v1, then one line holding on or off). A missing or
	 * invalid file means on. Never GameUserSettings: guarded builds never
	 * write that ini. */
	static bool LoadSharingSwitch();
	/** False when the side file could not be written. */
	static bool SaveSharingSwitch(bool bOn);
	static TArray<uint8> EncodeSharingSwitch(bool bOn);
	static TOptional<bool> DecodeSharingSwitch(TConstArrayView<uint8> Bytes);

	/** The start rule: Ready, switched on, and where the OS asks for
	 * local-network access, only after the heads-up. */
	static bool ShouldShare(bool bBootstrapReady, bool bSwitchOn,
		bool bOSAsksForAccess, bool bHeadsUpSeen);

	/** The notice line for a transfer event; empty for other events. */
	static FString NoticeText(const FWoTInstallerSharerEvent& Event);

	/** The Settings hint while sharing is on and nothing was found. Where the
	 * finder skips Downloads (Mac), it names the places it does search. */
	static FString NothingToShareHint();

	/** Settings saved the switch: start or stop now. Turning it off stops any
	 * transfer in progress. */
	void ApplySharingSwitch(bool bOn);
	bool IsSharingSwitchOn() const { return bSwitchOn; }
	bool IsSharerRunning() const;
	EWoTInstallerSharingSearch GetSearchState() const { return SearchState; }
	/** Sharing is on and the running sharer found nothing (the Settings hint). */
	bool HasNothingToShare() const
	{
		return bSwitchOn && SearchState == EWoTInstallerSharingSearch::NotFound;
	}

#if WITH_AUTOMATION_TESTS
	void ConfigureForTesting(FWoTInstallerSharingTestConfig&& InConfig,
		bool bInSwitchOn);
	void EvaluateForTesting() { Evaluate(); }
	/** One ticker step: Evaluate, then the notice's hide timer. */
	void TickForTesting() { Tick(0.0f); }
	/** Null until the first notice. */
	const FWoTSharingNoticeLine* NoticeLineForTesting() const
	{
		return NoticeLine.Get();
	}
	/** One inline sharer step, then the game-thread delivery. */
	void PumpForTesting();
	FWoTInstallerSharer* InlineSharerForTesting() const
	{
		return InlineSharer.Get();
	}
#endif

private:
	bool Tick(float DeltaSeconds);
	void Evaluate();
	void StartSharer();
	void StopSharer();
	/** bPresent false: search results only, no notice lines (a stop that
	 * hides the notice anyway). */
	void DrainEvents(bool bPresent = true);
	void Deliver(const FWoTInstallerSharerEvent& Event, bool bPresent);
	FWoTSharingNoticeLine& GetNoticeLine();
	bool IsBootstrapReady() const;
	bool HasSeenHeadsUp();
	TFunction<void(const FWoTInstallerSharerEvent&)> MakeEventSink() const;

	using FEventQueue = TQueue<FWoTInstallerSharerEvent, EQueueMode::Mpsc>;
	TSharedPtr<FEventQueue> Events;
	TUniquePtr<FWoTInstallerSharerThread> SharerThread;
	FTSTicker::FDelegateHandle TickerHandle;
	TUniquePtr<FWoTSharingNoticeLine> NoticeLine;
	EWoTInstallerSharingSearch SearchState = EWoTInstallerSharingSearch::Unknown;
	double NextHeadsUpCheckSeconds = 0.0;
	bool bSwitchLoaded = false;
	bool bSwitchOn = true;
	bool bHeadsUpSeen = false;
	bool bDeinitialized = false;
#if WITH_AUTOMATION_TESTS
	TOptional<FWoTInstallerSharingTestConfig> TestConfig;
	TUniquePtr<FWoTInstallerSharer> InlineSharer;
#endif
};
