#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Online/WoTInternetPlay.h"
#include "Online/WoTKeychainAccess.h"
#include "WoTFrontEnd.h"
#include "WoTSessionSubsystem.generated.h"

class UNetDriver;
class UWorld;
class APlayerController;

// UI-safe copy of the provider-owned FOnlineSessionSearchResult. The generation
// and raw index together remain valid only for the completed search that made it.
struct FWoTSessionListEntry
{
	uint64 SearchGeneration = 0;
	int32 ResultIndex = INDEX_NONE;
	FString ServerName;
	FString MapName;
	int32 CurrentPlayers = 0;
	int32 MaxPlayers = 0;
	int32 PingMs = 0;
	/** An EOS lobby (internet) row; LAN rows carry the beacon round trip. */
	bool bInternet = false;
	/** The host advertised a different WoT network version: joining would be
	 * refused by the engine's version handshake, so the browser says so. */
	bool bVersionMismatch = false;
	/** Advertised running release sequence, or zero for an old/unknown host. */
	uint64 HostReleaseSequence = 0;
};

/** UI/diagnostic snapshot of internet play (EOS). LAN never consults it. */
struct FWoTInternetStatus
{
	EWoTInternetState State = EWoTInternetState::Unavailable;
	/** Machine token: not-configured, disabled, login-failed, ... */
	FString Reason;
	/** Provider detail (EOS result code), diagnostic only. */
	FString Detail;
	/** Player-facing one-liner. */
	FString UserText() const;
	bool CanAttempt() const
	{
		return State == EWoTInternetState::Idle
			|| State == EWoTInternetState::Ready
			|| State == EWoTInternetState::Failed;
	}
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FWoTOnSessionSearchComplete,
	bool, const TArray<FWoTSessionListEntry>&);
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTOnSessionHostComplete, bool);
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTOnSessionJoinComplete, bool);
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTOnConnectionFailure, const FString&);
DECLARE_MULTICAST_DELEGATE(FWoTOnInternetStatusChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTOnOnlineIdentityResetComplete, bool);

enum class EWoTPasswordChallenge : uint8
{
	None,
	NeedPassword,
	WrongPassword,
};

// MP N7: the session seam over the OSS Sessions API (design doc §6 N7). Two
// explicit providers, chosen per request by bLAN:
//  - LAN: the Null OSS (LAN beacon) + IpNetDriver. Always available; the host
//    URL carries ?bIsLanMatch so the listen driver stays on IP even after EOS
//    started in this process. Direct IP shares the same transport.
//  - Internet: OnlineSubsystemEOS (OSS v1), created BY NAME on the first
//    internet action after the bootstrap gate (never the default service).
//    EOS Connect Device ID sign-in (no Epic account), one EOS lobby per listen
//    host (presence off, host migration off), NetDriverEOS P2P addressed by
//    product user id with relay fallback. Unconfigured builds, -NoEOS, a
//    missing SDK, or Epic being unreachable only disable the internet half.
// The host is a LISTEN server; the Steam N7 layer is dormant (DefaultEngine.ini).
UCLASS()
class UWoTSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Create the advertised listen session (NAME_GameSession). bLan forces the
	// LAN discovery path (the Null OSS is always effectively LAN). Kept for the
	// existing console verb; it preserves the old behavior exactly by reopening
	// the current map with only ?listen and no injected rules or player options.
	bool Host(int32 MaxPlayers = 8, bool bLan = false);

	// Parameterized menu host. The chosen map/rules/class URL is captured before
	// CreateSession and traveled only after a successful completion callback.
	bool Host(const FWoTHostRequest& Request);

	// Async search; provider results remain private while UI-safe copies land in
	// GetSearchResults and are announced through OnSearchComplete. An overlapping
	// call returns false without changing the active request/snapshot or firing a
	// completion for the rejected call. Other synchronous refusals complete once.
	bool Find(bool bLan = false);
	/** A Find has been accepted and has not completed yet. */
	bool IsFindInFlight() const { return bFindInFlight; }
	/** Drops the accepted Find without a completion: the menu cut its search
	 * short to host or connect. A late provider result is ignored, and the
	 * Null LAN beacon is freed at once (a LAN host only starts advertising
	 * once the search beacon stops). */
	void CancelFind();

	// Join only an entry from the current completed search. Stale generations and
	// overlapping joins return false without touching the active join delegates.
	// Successful resolved travel appends the saved retail Class option.
	bool Join(const FWoTSessionListEntry& Entry);

	// Compatibility console-index lookup into the current snapshot.
	bool Join(int32 Index);

	// Direct LAN/IP travel shares the exact same Class-option construction as
	// resolved session joins. Only a DNS-style host/IPv4 spelling, optionally
	// followed by a numeric port in [1, 65535], is accepted.
	bool ConnectDirect(const FString& Address);
	EWoTPasswordChallenge GetPasswordChallenge() const
	{
		return PasswordChallenge;
	}
	/** Retry the retained browser/invite/direct destination and remember the
	 * bounded password in the player's own preference file. */
	bool SubmitServerPassword(const FString& Password);
	void DismissPasswordChallenge();

	// One leave path for listen hosts, OSS clients, and direct-IP clients. A named
	// session is destroyed first and return travel waits for its async completion;
	// without one, ClientTravel to the generated front end starts immediately.
	bool Leave();

	// Empty during a new accepted search. A rejected overlapping Find leaves this
	// accessor and the one-completion-per-accepted-attempt event untouched.
	const TArray<FWoTSessionListEntry>& GetSearchResults() const { return SearchResults; }
	FWoTOnSessionSearchComplete& OnSearchComplete() { return SearchCompleteEvent; }
	// Accepted async Host/Join attempts announce their terminal result so Slate
	// can unlock actions and report failure without retaining OSS objects.
	FWoTOnSessionHostComplete& OnHostComplete() { return HostCompleteEvent; }
	FWoTOnSessionJoinComplete& OnJoinCompleteForUI() { return JoinCompleteEvent; }
	// Engine connection/travel failures are retained across the engine's
	// fallback-to-default-map path, then consumed by the menu controller.
	FWoTOnConnectionFailure& OnConnectionFailure() { return ConnectionFailureEvent; }
	FString ConsumeConnectionFailure();
	const FString& RetainedConnectionFailure() const
	{
		return LastConnectionFailure;
	}
	/** Retain and publish the exact player-facing cache-integrity message across
	 * the engine's rejected-travel fallback. */
	void ReportRuntimeIntegrityFailure(FString Message);
	/** Retain the damage message, retire any named session, and use the same
	 * authenticated front-end return as an ordinary Leave. */
	bool RecoverFromRuntimeIntegrityFailure(FString Message);
	/** Retire host-only failure classification after its destination is exposed. */
	void NotifySuccessfulHostDestinationExposure();

	// Captured even when automation suppresses ServerTravel; a spec observable.
	const FString& GetLastHostTravelURL() const { return LastHostTravelURL; }
	// Captured after (never before) leave destroy completion. Automation consumes
	// the same state transition but suppresses the actual ClientTravel.
	EWoTLeaveState GetLeaveState() const { return LeaveState.GetState(); }
	const FString& GetLastReturnTravelURL() const { return LastReturnTravelURL; }

	// True once the named session exists locally (the spec observable).
	bool HasSession() const;
	/** Active listen-host credentials live in process memory, never in travel
	 * URLs. Game modes on later server-travel maps read the same values. */
	void GetActiveHostPasswords(FString& OutAdminPassword,
		FString& OutGamePassword) const
	{
		OutAdminPassword = ActiveHostAdminPassword;
		OutGamePassword = ActiveHostGamePassword;
	}

	/** Internet play availability/sign-in. Cheap: never creates EOS. */
	FWoTInternetStatus GetInternetStatus() const;
	FWoTOnInternetStatusChanged& OnInternetStatusChanged() { return InternetStatusEvent; }
	/** The one-time online-identity disclosure was accepted on this profile. */
	/**
	 * Whether starting internet sign-in now would make macOS ask for keychain
	 * access (Online/WoTKeychainAccess.h). NoPrompt once signed in or signing
	 * in, and wherever internet play is unavailable.
	 */
	EWoTKeychainAccess ProbeSignInKeychainPrompt() const;
	bool IsOnlineIdentityAcknowledged() const;
	/** Records the player's OK; internet actions are refused until then. */
	bool AcknowledgeOnlineIdentity();
	/** Shared persisted relay preference used by both Multiplayer panels. */
	bool AlwaysUseEpicRelay() const;
	/** Persist and apply the relay preference; existing connections are unchanged. */
	bool SetAlwaysUseEpicRelay(bool bAlwaysUseRelay);
	/**
	 * Logout + EOS_Connect_DeleteDeviceId, then forget the acknowledgement: the
	 * next internet action asks again and mints a new anonymous identity.
	 * Refused while any session operation or sign-in is in flight.
	 */
	bool ResetOnlineIdentity();
	FWoTOnOnlineIdentityResetComplete& OnOnlineIdentityResetComplete()
	{
		return IdentityResetEvent;
	}
	/** Fixed, non-identifying Connect display name (Epic stores it). */
	static const TCHAR* InternetDisplayName() { return TEXT("WoT Player"); }
	/** Advertised per session; the browser flags rows that differ. */
	static FString LocalNetVersionString();

	// Bind the friend-invite acceptance (the Steam overlay's "Join Game"):
	// accepted invite -> JoinSession -> the OnJoinComplete travel below.
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Idempotent lifecycle driven by authenticated world-receipt publication. */
	bool ActivateForAuthorizedWorld();
	/** Called only by the bootstrap owner while preparing normal-authority
	 * adoption. Public operations remain closed until the predicate publishes. */
	bool ActivateForAdoptedWorldAuthority();
	void DeactivateForUnauthorizedWorld();

#if WITH_AUTOMATION_TESTS
	bool CanUseAuthorizedWorldForTesting() const
	{
		return CanUseAuthorizedWorld();
	}
	using FWoTClientTravelHook =
		TFunction<void(APlayerController*, const FString&, ETravelType)>;
	void SetClientTravelHookForTesting(FWoTClientTravelHook&& Hook)
	{
		ClientTravelHookForTesting = MoveTemp(Hook);
	}
	void SetClientTravelControllerForTesting(APlayerController* Controller)
	{
		ClientTravelControllerForTesting = Controller;
	}
	using FWoTServerTravelHook =
		TFunction<bool(UWorld*, const FString&, bool)>;
	void SetServerTravelHookForTesting(FWoTServerTravelHook&& Hook)
	{
		ServerTravelHookForTesting = MoveTemp(Hook);
	}
	using FWoTCreateSessionDispatchHook =
		TFunction<bool(TFunction<bool()>&&)>;
	void SetCreateSessionDispatchHookForTesting(
		FWoTCreateSessionDispatchHook&& Hook)
	{
		CreateSessionDispatchHookForTesting = MoveTemp(Hook);
	}
	using FWoTDetachedDestroyDispatchHook =
		TFunction<bool(TFunction<bool()>&&)>;
	static void SetDetachedDestroyDispatchHookForTesting(
		FWoTDetachedDestroyDispatchHook&& Hook);
	uint64 ActiveHostCreateSerialForTesting() const
	{
		return ActiveHostCreateSerial;
	}
	TFunction<void(bool)> CaptureHostCreateCompletionForTesting()
	{
		const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
		const uint64 OperationSerial = ActiveHostCreateSerial;
		return [WeakThis, OperationSerial](bool bOk)
		{
			if (UWoTSessionSubsystem* This = WeakThis.Get())
				This->OnCreateComplete(OperationSerial, NAME_GameSession, bOk);
		};
	}
	static void SetSceneTravelObserverForTesting(
		TFunction<void()>&& Observer);
	bool IsHostTravelPendingForTesting() const { return bHostTravelPending; }
	bool WasHostTravelFailureHandledForTesting() const
	{
		return bHostTravelFailureHandled;
	}
	UWorld* HostTravelWorldForTesting() const { return HostTravelWorld.Get(); }
	/** Replace the EOS backend with a spec fake (nullptr restores lazily). */
	void SetInternetBackendForTesting(TSharedPtr<IWoTInternetBackend> Backend)
	{
		InternetBackend = MoveTemp(Backend);
		bRelayControlReady = false;
	}
	void SetInternetSignInTimeoutForTesting(double Seconds)
	{
		InternetSignInTimeoutSeconds = Seconds;
	}
	void SetHostProviderForTesting(const FString& Provider)
	{
		LastHostProvider = Provider;
	}
	/** Bypass the preference marker (unset restores it). */
	void SetOnlineIdentityAcknowledgedForTesting(TOptional<bool> bAcknowledged)
	{
		OnlineIdentityAcknowledgedForTesting = bAcknowledged;
	}
	void SetAlwaysUseEpicRelayForTesting(TOptional<bool> bAlwaysUseRelay)
	{
		AlwaysUseEpicRelayForTesting = bAlwaysUseRelay;
	}
	using FWoTRelayPreferenceSaveHook = TFunction<bool(bool)>;
	void SetRelayPreferenceSaveHookForTesting(
		FWoTRelayPreferenceSaveHook&& Hook)
	{
		RelayPreferenceSaveHookForTesting = MoveTemp(Hook);
	}
	/** Drive the sign-in timeout without waiting on the core ticker. */
	bool TickInternetSignInTimeoutForTesting(double Now)
	{
		return TickInternetSignInTimeout(Now);
	}
	bool IsInternetActionPendingForTesting() const
	{
		return PendingInternetAction != EWoTPendingInternetAction::None;
	}
	static FString AppendLanMatchOptionForTesting(const FString& URL, bool bLan)
	{
		return AppendLanMatchOption(URL, bLan);
	}
	FOnlineSessionSettings BuildSessionSettingsForTesting(
		const FWoTHostRequest& Request) const
	{
		return BuildSessionSettings(Request);
	}
	TSharedRef<FOnlineSessionSearch> BuildSearchForTesting(bool bLan) const
	{
		return BuildSearch(bLan);
	}
	/** Publish one completed search row on an exact provider (join specs). */
	FWoTSessionListEntry InjectSearchResultForTesting(
		IOnlineSessionPtr Provider, bool bInternet,
		const FOnlineSessionSearchResult& Result);
	void SetRunningReleaseSequenceForTesting(uint64 Sequence)
	{
		RunningReleaseSequenceForTesting = Sequence;
	}
	bool IsJoinCleanupPendingForTesting() const { return bJoinCleanupPending; }
	const FString& LastJoinOutcomeForTesting() const { return LastJoinOutcome; }
	void SetPasswordChallengeForTesting(EWoTPasswordChallenge Challenge,
		const FString& PendingURL)
	{
		PasswordChallenge = Challenge;
		PendingClientTravelURL = PendingURL;
		PendingPasswordServer = PasswordServerKey(PendingURL);
	}
#endif

	/** Live host-path gate readout + the last recorded host outcome. The
	 * packaged guarded client writes no logs, so the funnel's net-test
	 * snapshot surfaces this string to diagnose host refusals in place. */
	FString HostDiagnostics() const;
	/** Stable provider label used by listen-host client lifecycle telemetry. */
	FString HostProviderForTelemetry() const
	{
		return LastHostProvider.IsEmpty() ? FString(TEXT("none")) : LastHostProvider;
	}

	/** Detail of the permanent `net-failure` heartbeat: every engine network
	 * or travel failure owned by this game instance, with its engine type and
	 * message. The guarded packaged build compiles logging out, so without this
	 * a dropped connection returned to the menu with no recorded reason. */
	static FString NetFailureHeartbeatDetail(const TCHAR* Kind,
		const TCHAR* FailureType, const TCHAR* Role, bool bClientConnection,
		const FString& Message);
	/** Detail of the permanent `session-leave` heartbeat: why a Leave began
	 * (the explicit UI/console request or a connection failure) and whether a
	 * named OSS session is destroyed before the return travel. */
	static FString SessionLeaveHeartbeatDetail(bool bConnectionFailure,
		bool bHasNamedSession);

private:
	enum class EWoTPendingInternetAction : uint8
	{
		None,
		Host,
		Find
	};

	bool CanUseAuthorizedWorld() const;
	/** The Null (LAN) provider adopted at activation. */
	IOnlineSessionPtr LanSessions() const;
	/** The EOS provider once started this run, else nullptr. */
	IOnlineSessionPtr InternetSessions() const;
	/** Whichever provider currently holds NAME_GameSession (internet first). */
	IOnlineSessionPtr SessionsHoldingGameSession() const;
	IWoTInternetBackend& Backend() const;
	/** Apply the requested preference to this authenticated EOS session. */
	bool ApplyRelayControlPreference();
	/** Gate + acknowledgement + lazy EOS start. Empty = usable. */
	FString PrepareInternetProvider();
	bool StartInternetSignIn();
	void OnInternetDeviceId(uint64 Serial, bool bOk, const FString& Detail);
	void OnInternetLogin(uint64 Serial, bool bOk, const FString& Detail);
	void CompleteInternetSignIn(bool bOk, const FString& Reason,
		const FString& Detail);
	void CancelPendingInternetAction(bool bPublishHostFailure);
	bool TickInternetSignInTimeout(double Now);
	void StopInternetSignInTimeout();
	void HandleInternetSignInLost(const FString& Reason);
	void SetInternetState(EWoTInternetState State, const FString& Reason,
		const FString& Detail);
	FOnlineSessionSettings BuildSessionSettings(const FWoTHostRequest& Request) const;
	uint64 RunningReleaseSequence() const;
	TSharedRef<FOnlineSessionSearch> BuildSearch(bool bLan) const;
	bool DispatchFind(IOnlineSessionPtr S, bool bLan, uint64 RequestGeneration);
	void FinishJoinCleanup(bool bPublishJoinFailure);
	static FString AppendLanMatchOption(const FString& URL, bool bLan);
	bool FinishAuthorizedWorldActivation(bool bRequireCurrentGate);
	APlayerController* ResolveLocalPlayerController() const;
	IOnlineSessionPtr Sessions() const;
	void HandleWorldReadinessChanged(bool bReady);
	bool TravelClientIfAuthorized(
		APlayerController* Controller, const FString& URL,
		ETravelType TravelType);
	bool TravelServerIfAuthorized(
		UWorld* World, const FString& URL, bool bAbsoluteTravel);
	EWoTPlayerFaction SelectedFaction() const;
	EWoTPlayerSkinColor SelectedSkinColor() const;
	FString PlayerTravelURL(const FString& URL) const;
	static FString PasswordServerKey(const FString& URL);
	bool RetryRetainedClientTravel(const FString& Password, bool bSave);
	bool HandlePasswordFailure(const FString& ErrorString);
	void BeginPasswordCapableClientTravel(const FString& Connect,
		const FString& TravelURL);
	void ClearPasswordTravelState();
	bool BeginHost(const FWoTHostRequest& Request, const FString& HostTravelURL,
		bool bAbsoluteTravel);
	void CompleteFind(bool bOk, uint64 ExpectedGeneration);

	void OnInviteAccepted(const bool bOk, const int32 ControllerId,
	                      FUniqueNetIdPtr UserId,
	                      const FOnlineSessionSearchResult& InviteResult);
	FDelegateHandle InviteHandle;
	FDelegateHandle WorldReadinessHandle;

	void OnCreateComplete(uint64 OperationSerial, FName SessionName, bool bOk);
	void BeginFailedHostCleanup(UWorld* FailedWorld,
		IOnlineSessionPtr ExactSessions = nullptr);
	void RestoreFailedHostScene(UWorld* FailedWorld);
	void OnFailedHostDestroyComplete(FName SessionName, bool bOk);
	void CompleteFailedHostCleanup(bool bDestroyOk);
	void OnFindComplete(bool bOk, uint64 RequestGeneration);
	void OnJoinComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void OnDestroyComplete(FName SessionName, bool bOk);
	bool BeginLeave(bool bPreserveConnectionFailure,
		bool bReturnToMenu = true);
	void CompleteReturnToMenu();
	bool OwnsFailure(UWorld* FailedWorld, const UNetDriver* NetDriver) const;
	void RecordConnectionFailure(const TCHAR* FailureType, const FString& ErrorString);
	void RetainConnectionFailure(const TCHAR* FailureType, const FString& ErrorString);
	void ClearHostTravelState();
	TSharedPtr<class FOnlineSessionSearch> LastSearch;
	FDelegateHandle FindHandle;
	TArray<FWoTSessionListEntry> SearchResults;
	FWoTOnSessionSearchComplete SearchCompleteEvent;
	FWoTOnSessionHostComplete HostCompleteEvent;
	FWoTOnSessionJoinComplete JoinCompleteEvent;
	FWoTOnConnectionFailure ConnectionFailureEvent;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle DisconnectHandle;
	FDelegateHandle DestroyHandle;
	FDelegateHandle HostCreateHandle;
	FDelegateHandle FailedHostDestroyHandle;
	IOnlineSessionPtr AuthorizedSessions;
	/** Exact provider retained across normal-authority revocation solely so a
	 * durable, independently authorized integrity return can destroy its row. */
	IOnlineSessionPtr RuntimeIntegrityRecoverySessions;
	IOnlineSessionPtr HostCreateSessions;
	const UNetDriver* DisconnectingPendingNetDriver = nullptr;
	FString LastConnectionFailure;
	bool bRuntimeIntegrityFailureRetained = false;
	bool bRuntimeIntegrityRecoveryInFlight = false;
	uint64 SearchGeneration = 0;
	uint64 CompletedSearchGeneration = 0;
	bool bFindInFlight = false;
	bool bJoinInFlight = false;
	bool bCreateInFlight = false;
	bool bHostCreateAuthorityRevoked = false;
	uint64 NextHostCreateSerial = 0;
	uint64 ActiveHostCreateSerial = 0;
	bool bFailedHostDestroyInFlight = false;
	TWeakObjectPtr<UWorld> FailedHostWorld;
	IOnlineSessionPtr FailedHostSessions;
	bool bHostTravelPending = false;
	bool bHostTravelFailureHandled = false;
	TWeakObjectPtr<UWorld> HostTravelWorld;
	FString LastHostOutcome;
	// Shell-routing survives here: LastHostOutcome advances through
	// create-dispatched/create-complete synchronously, so the routing
	// decision needs its own field to stay observable in the gate readout.
	FString LastHostRoute;
	// The joiner half had NO diagnostics: the readout tracked host outcomes
	// only, so "instance B did not discover and join" could not distinguish a
	// refused Find from a Find that completed with zero results from a failed
	// Join. Packaged Test builds compile logging out, so these strings are the
	// only channel (Mac host-join, 2026-08-05).
	FString ResolvedSubsystemName() const;
	FString LastFindOutcome;
	FString LastJoinOutcome;
	// Provider bookkeeping: each async operation remembers the exact provider
	// it was dispatched on, so completion, join and leave never ask the other.
	IOnlineSessionPtr SearchSessions;
	bool bSearchInternet = false;
	IOnlineSessionPtr JoinSessions;
	IOnlineSessionPtr LeaveSessions;
	FString LastHostProvider;
	// Internet (EOS) sign-in state machine: Idle -> CreatingDeviceId ->
	// LoggingIn -> Ready | Failed. An accepted Host/Find that needs a sign-in
	// is parked here and resumed (or failed exactly once) by the result.
	mutable TSharedPtr<IWoTInternetBackend> InternetBackend;
	EWoTInternetState InternetState = EWoTInternetState::Idle;
	// Authentication is not sufficient for internet play: this must also be
	// true, so a failed ForceRelays request can never fall through to direct P2P.
	bool bRelayControlReady = false;
	FString InternetReason;
	FString InternetDetail;
	uint64 InternetSignInSerial = 0;
	EWoTPendingInternetAction PendingInternetAction = EWoTPendingInternetAction::None;
	FWoTHostRequest PendingInternetHostRequest;
	FString PendingInternetHostURL;
	bool bPendingInternetHostAbsolute = false;
	uint64 PendingInternetFindGeneration = 0;
	double InternetSignInDeadline = 0.0;
	double InternetSignInTimeoutSeconds = 45.0;
	FTSTicker::FDelegateHandle InternetTimeoutHandle;
	bool bIdentityResetInFlight = false;
	// A provider join we will not travel on (travel refused / no controller /
	// no connect string, or authority revoked mid-join) is LEFT again before
	// anything else may Host/Join/Leave/Reset: the provider still holds the
	// named session (EOS: the player is a lobby member) and its late join
	// callback carries no operation identity.
	bool bJoinCleanupPending = false;
#if !WOT_GUARDED_BOOTSTRAP
	// Unguarded development runs have no preference owner: the disclosure is
	// accepted for this process only (guarded builds always persist it).
	bool bOnlineIdentityAcknowledgedThisRun = false;
#endif
	FWoTOnInternetStatusChanged InternetStatusEvent;
	FWoTOnOnlineIdentityResetComplete IdentityResetEvent;
#if WITH_AUTOMATION_TESTS
	TOptional<bool> OnlineIdentityAcknowledgedForTesting;
	TOptional<bool> AlwaysUseEpicRelayForTesting;
	TOptional<uint64> RunningReleaseSequenceForTesting;
	FWoTRelayPreferenceSaveHook RelayPreferenceSaveHookForTesting;
#endif
	bool bClientConnectionPendingOrActive = false;
	EWoTPasswordChallenge PasswordChallenge = EWoTPasswordChallenge::None;
	FString PendingClientTravelURL;
	FString PendingPasswordServer;
	bool bSavedPasswordAttempted = false;
	FString PendingHostTravelURL;
	FString LastHostTravelURL;
	FString LastReturnTravelURL;
	FWoTLeaveStateMachine LeaveState;
	bool bLeaveReturnsToMenu = true;
	FString ActiveHostAdminPassword;
	FString ActiveHostGamePassword;
	bool bPendingHostTravelAbsolute = false;
	bool bAuthorizedForWorld = false;
#if WITH_AUTOMATION_TESTS
	FWoTClientTravelHook ClientTravelHookForTesting;
	FWoTServerTravelHook ServerTravelHookForTesting;
	FWoTCreateSessionDispatchHook CreateSessionDispatchHookForTesting;
	TWeakObjectPtr<APlayerController> ClientTravelControllerForTesting;
#endif
};
