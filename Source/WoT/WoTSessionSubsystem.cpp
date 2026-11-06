#include "WoTSessionSubsystem.h"

#include "WoTBootstrapHeartbeat.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameDelegates.h"
#include "GameFramework/PlayerController.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystemUtils.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "Online/LANBeacon.h"
#include "WoTGameInstance.h"
#include "WoTBootstrapSubsystem.h"
#include "WoTFrontEndPlayerController.h"
#include "Payload/WoTPayloadActivation.h"
#include "WoTFrontEndCatalog.h"
#include "World/WoTMapDisplay.h"
#include "World/WoTWorldMaterializer.h"
#include "WoTNetVersion.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTPasswordToken.h"
#include "Engine/NetDriver.h"
#include "Misc/NetworkVersion.h"
#include "OnlineSubsystemNames.h"

namespace
{
	const FName WoTSession_ServerNameSetting(TEXT("WOT_SERVER_NAME"));
	// The host's WoT network version (WoTNetVersion::Current, hex). The browser
	// flags a row whose host differs: the engine handshake would refuse it.
	const FName WoTSession_NetVersionSetting(TEXT("WOT_NETVER"));
	const FName WoTSession_ReleaseSequenceSetting(TEXT("WOT_RELSEQ"));
	// Internet lobbies list at most this many hosts per search (EOS caps one
	// lobby search at EOS_LOBBY_MAX_SEARCH_RESULTS = 200).
	constexpr int32 WoTInternetMaxSearchResults = 100;
	// Browser sort sentinel for "no ping": EOS lobby results carry the search
	// round trip, not host latency (OnlineSessionEOS.cpp AddLobbySearchResult).
	constexpr int32 WoTUnknownPingMs = 9999;

	FString WoTSession_RedactTravelPasswords(const FString& Source)
	{
		FString Result = Source;
		static const TCHAR* SensitiveOptions[] =
		{
			TEXT("Password"), TEXT("PasswordToken"),
			TEXT("AdminPassword"), TEXT("GamePassword")
		};
		for (int32 Index = 0; Index < Result.Len(); ++Index)
		{
			if (Result[Index] != TEXT('?'))
			{
				continue;
			}
			const int32 NameStart = Index + 1;
			for (const TCHAR* Name : SensitiveOptions)
			{
				const int32 NameLength = FCString::Strlen(Name);
				if (NameStart + NameLength >= Result.Len()
					|| Result[NameStart + NameLength] != TEXT('=')
					|| !Result.Mid(NameStart, NameLength).Equals(
						Name, ESearchCase::IgnoreCase))
				{
					continue;
				}
				const int32 ValueStart = NameStart + NameLength + 1;
				int32 ValueEnd = ValueStart;
				while (ValueEnd < Result.Len()
					&& Result[ValueEnd] != TEXT('?')
					&& Result[ValueEnd] != TEXT('#')
					&& Result[ValueEnd] != TEXT('"')
					&& !FChar::IsWhitespace(Result[ValueEnd]))
				{
					++ValueEnd;
				}
				static const FString Redacted(TEXT("<redacted>"));
				Result = Result.Left(ValueStart) + Redacted + Result.Mid(ValueEnd);
				Index = ValueStart + Redacted.Len() - 1;
				break;
			}
		}
		return Result;
	}

	uint64 WoTSessionParseReleaseSequence(const FString& Text)
	{
		if (Text.IsEmpty()) return 0;
		for (const TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character)) return 0;
		}
		uint64 Sequence = 0;
		return LexTryParseString(Sequence, *Text) ? Sequence : 0;
	}

	void WoTSessionPopulateVersionFields(
		const FOnlineSessionSettings& Settings,
		const FString& LocalNetVersion, FWoTSessionListEntry& OutEntry)
	{
		FString HostNetVersion;
		Settings.Get(WoTSession_NetVersionSetting, HostNetVersion);
		OutEntry.bVersionMismatch = !HostNetVersion.IsEmpty()
			&& HostNetVersion != LocalNetVersion;
		FString HostReleaseSequence;
		Settings.Get(WoTSession_ReleaseSequenceSetting, HostReleaseSequence);
		OutEntry.HostReleaseSequence =
			WoTSessionParseReleaseSequence(HostReleaseSequence);
	}

	struct FWoTDetachedHostCreateCleanup
	{
		IOnlineSessionPtr Sessions;
		FDelegateHandle CreateHandle;
	};
	struct FWoTDetachedHostDestroyCleanup
	{
		IOnlineSessionPtr Sessions;
		FDelegateHandle DestroyHandle;
		TFunction<void()> OnDone;
	};
#if WITH_AUTOMATION_TESTS
	UWoTSessionSubsystem::FWoTDetachedDestroyDispatchHook
		GWoTDetachedDestroyDispatchHook;
#endif

	void WoTArmDetachedHostDestroyCleanup(const IOnlineSessionPtr& Sessions,
		TFunction<void()> OnDone = nullptr)
	{
		if (!Sessions.IsValid())
		{
			if (OnDone) OnDone();
			return;
		}
		const TSharedRef<FWoTDetachedHostDestroyCleanup> Cleanup =
			MakeShared<FWoTDetachedHostDestroyCleanup>();
		Cleanup->Sessions = Sessions;
		Cleanup->OnDone = MoveTemp(OnDone);
		Cleanup->DestroyHandle =
			Sessions->AddOnDestroySessionCompleteDelegate_Handle(
				FOnDestroySessionCompleteDelegate::CreateLambda(
					[Cleanup](FName DestroyedName, bool bDestroyOk)
					{
						if (DestroyedName != NAME_GameSession) return;
						const IOnlineSessionPtr ExactSessions = Cleanup->Sessions;
						if (!ExactSessions.IsValid()) return;
						if (Cleanup->DestroyHandle.IsValid())
						{
							ExactSessions->ClearOnDestroySessionCompleteDelegate_Handle(
								Cleanup->DestroyHandle);
							Cleanup->DestroyHandle.Reset();
						}
						if (!bDestroyOk
							|| ExactSessions->GetNamedSession(NAME_GameSession) != nullptr)
						{
							ExactSessions->RemoveNamedSession(NAME_GameSession);
						}
						Cleanup->Sessions.Reset();
						TFunction<void()> Done = MoveTemp(Cleanup->OnDone);
						Cleanup->OnDone = nullptr;
						if (Done) Done();
					}));
		TFunction<bool()> DispatchDestroy = [Sessions]()
		{
			return Sessions->DestroySession(NAME_GameSession);
		};
		bool bDispatched = false;
#if WITH_AUTOMATION_TESTS
		if (GWoTDetachedDestroyDispatchHook)
			bDispatched = GWoTDetachedDestroyDispatchHook(MoveTemp(DispatchDestroy));
		else
#endif
			bDispatched = DispatchDestroy();
		if (!bDispatched && Cleanup->Sessions.IsValid())
		{
			if (Cleanup->DestroyHandle.IsValid())
			{
				Sessions->ClearOnDestroySessionCompleteDelegate_Handle(
					Cleanup->DestroyHandle);
				Cleanup->DestroyHandle.Reset();
			}
			Sessions->RemoveNamedSession(NAME_GameSession);
			Cleanup->Sessions.Reset();
			TFunction<void()> Done = MoveTemp(Cleanup->OnDone);
			Cleanup->OnDone = nullptr;
			if (Done) Done();
		}
	}

	void WoTArmDetachedHostCreateCleanup(const IOnlineSessionPtr& Sessions)
	{
		if (!Sessions.IsValid()) return;
		const TSharedRef<FWoTDetachedHostCreateCleanup> Cleanup =
			MakeShared<FWoTDetachedHostCreateCleanup>();
		Cleanup->Sessions = Sessions;
		Cleanup->CreateHandle =
			Sessions->AddOnCreateSessionCompleteDelegate_Handle(
				FOnCreateSessionCompleteDelegate::CreateLambda(
					[Cleanup](FName SessionName, bool bOk)
					{
						if (SessionName != NAME_GameSession) return;
						const IOnlineSessionPtr ExactSessions = Cleanup->Sessions;
						if (!ExactSessions.IsValid()) return;
						if (Cleanup->CreateHandle.IsValid())
						{
							ExactSessions->ClearOnCreateSessionCompleteDelegate_Handle(
								Cleanup->CreateHandle);
							Cleanup->CreateHandle.Reset();
						}
						if (!bOk
							|| ExactSessions->GetNamedSession(NAME_GameSession) == nullptr)
						{
							Cleanup->Sessions.Reset();
							return;
						}
						Cleanup->Sessions.Reset();
						WoTArmDetachedHostDestroyCleanup(ExactSessions);
					}));
	}
	// A join whose UObject route was revoked mid-flight: wait for the provider's
	// terminal join callback, then leave whatever it joined (an EOS lobby or a
	// Null row) so a later Host/Join is not blocked by a stale named session.
	void WoTArmDetachedJoinCleanup(const IOnlineSessionPtr& Sessions,
		TFunction<void()> OnDone)
	{
		if (!Sessions.IsValid())
		{
			if (OnDone) OnDone();
			return;
		}
		struct FWoTDetachedJoinCleanup
		{
			IOnlineSessionPtr Sessions;
			FDelegateHandle JoinHandle;
			TFunction<void()> OnDone;
		};
		const TSharedRef<FWoTDetachedJoinCleanup> Cleanup =
			MakeShared<FWoTDetachedJoinCleanup>();
		Cleanup->Sessions = Sessions;
		Cleanup->OnDone = MoveTemp(OnDone);
		Cleanup->JoinHandle = Sessions->AddOnJoinSessionCompleteDelegate_Handle(
			FOnJoinSessionCompleteDelegate::CreateLambda(
				[Cleanup](FName SessionName, EOnJoinSessionCompleteResult::Type)
				{
					if (SessionName != NAME_GameSession) return;
					const IOnlineSessionPtr ExactSessions = Cleanup->Sessions;
					if (!ExactSessions.IsValid()) return;
					if (Cleanup->JoinHandle.IsValid())
					{
						ExactSessions->ClearOnJoinSessionCompleteDelegate_Handle(
							Cleanup->JoinHandle);
						Cleanup->JoinHandle.Reset();
					}
					Cleanup->Sessions.Reset();
					TFunction<void()> Done = MoveTemp(Cleanup->OnDone);
					Cleanup->OnDone = nullptr;
					if (ExactSessions->GetNamedSession(NAME_GameSession) != nullptr)
					{
						WoTArmDetachedHostDestroyCleanup(ExactSessions, MoveTemp(Done));
					}
					else if (Done)
					{
						Done();
					}
				}));
	}
#if WITH_AUTOMATION_TESTS
	TFunction<void()> GWoTSceneTravelObserver;
#endif
}

#if WITH_AUTOMATION_TESTS
void UWoTSessionSubsystem::SetSceneTravelObserverForTesting(
	TFunction<void()>&& Observer)
{
	GWoTSceneTravelObserver = MoveTemp(Observer);
}

void UWoTSessionSubsystem::SetDetachedDestroyDispatchHookForTesting(
	FWoTDetachedDestroyDispatchHook&& Hook)
{
	GWoTDetachedDestroyDispatchHook = MoveTemp(Hook);
}
#endif

void UWoTSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WorldReadinessHandle = RegisterWoTBootstrapWorldReadinessListener(
		FWoTBootstrapWorldReadinessEvent::FDelegate::CreateUObject(
			this, &UWoTSessionSubsystem::HandleWorldReadinessChanged));
	ActivateForAuthorizedWorld();
}

bool UWoTSessionSubsystem::CanUseAuthorizedWorld() const
{
	return bAuthorizedForWorld && IsWoTNormalWorldAuthorityAdopted()
		&& !IsRunningDedicatedServer()
		&& IsWoTPayloadBootstrapActive();
}

void UWoTSessionSubsystem::HandleWorldReadinessChanged(bool bReady)
{
	if (bReady)
	{
		ActivateForAuthorizedWorld();
	}
	else
	{
		DeactivateForUnauthorizedWorld();
	}
}

bool UWoTSessionSubsystem::ActivateForAuthorizedWorld()
{
	if (bAuthorizedForWorld)
	{
		return IsWoTBootstrapWorldReady();
	}
	if (!IsWoTBootstrapWorldReady())
	{
		return false;
	}
	return FinishAuthorizedWorldActivation(/*bRequireCurrentGate*/ true);
}

bool UWoTSessionSubsystem::ActivateForAdoptedWorldAuthority()
{
	if (bAuthorizedForWorld)
	{
		return true;
	}
	return FinishAuthorizedWorldActivation(/*bRequireCurrentGate*/ false);
}

bool UWoTSessionSubsystem::FinishAuthorizedWorldActivation(
	bool bRequireCurrentGate)
{
	bAuthorizedForWorld = true;
	if (bRequireCurrentGate && !CanUseAuthorizedWorld())
	{
		bAuthorizedForWorld = false;
		return false;
	}
	const UWorld* World = GetGameInstance()
		? GetGameInstance()->GetWorld() : nullptr;
	// LAN + direct IP are pinned to the Null OSS explicitly (it is also the
	// default service). The EOS provider is never touched here: it is created
	// by name only on the first internet action (PrepareInternetProvider).
	if (const IOnlineSubsystem* OSS = Online::GetSubsystem(World, NULL_SUBSYSTEM))
	{
		AuthorizedSessions = OSS->GetSessionInterface();
	}
	if (GEngine)
	{
		DisconnectHandle = FGameDelegates::Get().GetHandleDisconnectDelegate()
			.AddWeakLambda(this,
				[this](UWorld* FailedWorld, UNetDriver* NetDriver)
				{
					// UEngine's own failure listener clears a pending net game
					// before later OnNetworkFailure listeners necessarily run.
					// Snapshot only the matching null-world driver while its world
					// context still identifies this game instance.
					if (bClientConnectionPendingOrActive && !FailedWorld
						&& NetDriver && OwnsFailure(nullptr, NetDriver))
					{
						DisconnectingPendingNetDriver = NetDriver;
					}
				});
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddWeakLambda(this,
			[this](UWorld* FailedWorld, UNetDriver* NetDriver,
				ENetworkFailure::Type FailureType, const FString& ErrorString)
			{
				const bool bOwnedPendingDriver =
					NetDriver && NetDriver == DisconnectingPendingNetDriver;
				if (bOwnedPendingDriver)
				{
					DisconnectingPendingNetDriver = nullptr;
				}
				const bool bOwnedFailure =
					bOwnedPendingDriver || OwnsFailure(FailedWorld, NetDriver);
				if (bOwnedFailure)
				{
					const ENetMode Mode = FailedWorld != nullptr
						? FailedWorld->GetNetMode() : NM_Client;
					const TCHAR* Role = FailedWorld == nullptr ? TEXT("pending")
						: Mode == NM_Client ? TEXT("client")
						: Mode == NM_ListenServer ? TEXT("listen")
						: Mode == NM_DedicatedServer ? TEXT("dedicated")
						: TEXT("standalone");
					WoTRecordBootstrapHeartbeat(TEXT("net-failure"),
						EWoTBootstrapPhase::Ready, 0, {}, {},
						NetFailureHeartbeatDetail(TEXT("network"),
							ENetworkFailure::ToString(FailureType), Role,
							bClientConnectionPendingOrActive, ErrorString));
				}
				if (bClientConnectionPendingOrActive && bOwnedFailure)
				{
					if (HandlePasswordFailure(ErrorString))
					{
						return;
					}
					RecordConnectionFailure(
						ENetworkFailure::ToString(FailureType), ErrorString);
					UWorld* RecoveryWorld = FailedWorld != nullptr ? FailedWorld
						: (GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr);
					if (RecoveryWorld != nullptr
						&& Cast<AWoTFrontEndPlayerController>(
							RecoveryWorld->GetFirstPlayerController()) != nullptr)
						if (UWoTBootstrapSubsystem* Bootstrap = GetGameInstance()
							? GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
							Bootstrap->EnsureFrontEndSceneForWorld(*RecoveryWorld,
								RecoveryWorld->GetFirstPlayerController());
				}
			});
		TravelFailureHandle = GEngine->OnTravelFailure().AddWeakLambda(this,
			[this](UWorld* FailedWorld, ETravelFailure::Type FailureType,
				const FString& ErrorString)
			{
				if (OwnsFailure(FailedWorld, nullptr))
				{
					WoTRecordBootstrapHeartbeat(TEXT("net-failure"),
						EWoTBootstrapPhase::Ready, 0, {}, {},
						NetFailureHeartbeatDetail(TEXT("travel"),
							ETravelFailure::ToString(FailureType),
							bHostTravelPending ? TEXT("host-travel")
								: TEXT("travel"),
							bClientConnectionPendingOrActive, ErrorString));
					if (bHostTravelPending && !bFailedHostDestroyInFlight
						&& (!HostTravelWorld.IsValid()
							|| HostTravelWorld.Get() == FailedWorld))
					{
						RetainConnectionFailure(
							ETravelFailure::ToString(FailureType), ErrorString);
						BeginFailedHostCleanup(FailedWorld);
						return;
					}
					if (bClientConnectionPendingOrActive)
						RecordConnectionFailure(
							ETravelFailure::ToString(FailureType), ErrorString);
					if (FailedWorld != nullptr)
					{
						if (UWoTBootstrapSubsystem* Bootstrap = GetGameInstance()
							? GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
						{
							if (Bootstrap->IsFrontEndSceneSuspendedForWorld(*FailedWorld)
								|| Cast<AWoTFrontEndPlayerController>(
									FailedWorld->GetFirstPlayerController()) != nullptr)
							{
								Bootstrap->EnsureFrontEndSceneForWorld(*FailedWorld,
									FailedWorld->GetFirstPlayerController());
							}
						}
					}
				}
			});
	}
	if (AuthorizedSessions.IsValid())
	{
		InviteHandle = AuthorizedSessions->
			AddOnSessionUserInviteAcceptedDelegate_Handle(
		    FOnSessionUserInviteAcceptedDelegate::CreateUObject(
		        this, &UWoTSessionSubsystem::OnInviteAccepted));
	}
	return true;
}

void UWoTSessionSubsystem::DeactivateForUnauthorizedWorld()
{
	if (!bAuthorizedForWorld)
	{
		return;
	}
	bAuthorizedForWorld = false;
	bRelayControlReady = false;
	if (IsWoTFrontEndIntegrityRecoveryAuthorized())
	{
		const IOnlineSessionPtr Candidates[] = {
			JoinSessions, HostCreateSessions, FailedHostSessions,
			LeaveSessions, AuthorizedSessions
		};
		for (const IOnlineSessionPtr& Candidate : Candidates)
		{
			if (Candidate.IsValid()
				&& Candidate->GetNamedSession(NAME_GameSession) != nullptr)
			{
				RuntimeIntegrityRecoverySessions = Candidate;
				break;
			}
		}
	}
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	FGameDelegates::Get().GetHandleDisconnectDelegate().Remove(DisconnectHandle);
	NetworkFailureHandle.Reset();
	TravelFailureHandle.Reset();
	DisconnectHandle.Reset();
	DisconnectingPendingNetDriver = nullptr;
	// An accepted Find (in flight on a provider, or parked behind an internet
	// sign-in) publishes its one terminal failure now; a retained front end
	// would otherwise stay "Searching" forever. CompleteFind consumes the
	// generation, so a late provider completion is ignored.
	if (bFindInFlight)
	{
		PendingInternetFindGeneration = 0;
		if (PendingInternetAction == EWoTPendingInternetAction::Find)
		{
			PendingInternetAction = EWoTPendingInternetAction::None;
		}
		CompleteFind(false, SearchGeneration);
		LastFindOutcome = TEXT("revoked");
	}
	// A join the provider may still complete: its result can no longer
	// authorize travel, so leave whatever it joins.
	const IOnlineSessionPtr RevokedJoinProvider =
		bJoinInFlight ? (JoinSessions.IsValid() ? JoinSessions : AuthorizedSessions)
			: nullptr;
	if (AuthorizedSessions.IsValid())
	{
		AuthorizedSessions->ClearOnSessionUserInviteAcceptedDelegate_Handle(
			InviteHandle);
	}
	// Each async operation's delegate lives on the provider it was dispatched
	// on (LAN or internet); clear it there.
	const IOnlineSessionPtr FindProvider =
		SearchSessions.IsValid() ? SearchSessions : AuthorizedSessions;
	if (FindProvider.IsValid() && FindHandle.IsValid())
	{
		FindProvider->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
	}
	if (AuthorizedSessions.IsValid())
	{
		AuthorizedSessions->ClearOnJoinSessionCompleteDelegates(this);
	}
	if (JoinSessions.IsValid() && JoinSessions != AuthorizedSessions)
	{
		JoinSessions->ClearOnJoinSessionCompleteDelegates(this);
	}
	const IOnlineSessionPtr DestroyProvider =
		LeaveSessions.IsValid() ? LeaveSessions : AuthorizedSessions;
	if (DestroyProvider.IsValid() && DestroyHandle.IsValid())
	{
		DestroyProvider->ClearOnDestroySessionCompleteDelegate_Handle(
			DestroyHandle);
	}
	InviteHandle.Reset();
	FindHandle.Reset();
	DestroyHandle.Reset();
	if (RevokedJoinProvider.IsValid())
	{
		bJoinCleanupPending = true;
		const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
		// The accepted Join still gets exactly one terminal result: a failure,
		// published once whatever the provider joined has been left again.
		WoTArmDetachedJoinCleanup(RevokedJoinProvider, [WeakThis]()
		{
			if (UWoTSessionSubsystem* This = WeakThis.Get())
			{
				This->FinishJoinCleanup(/*bPublishJoinFailure*/ true);
			}
		});
	}
	AuthorizedSessions.Reset();
	SearchSessions.Reset();
	bSearchInternet = false;
	JoinSessions.Reset();
	LeaveSessions.Reset();
	SearchResults.Reset();
	LastSearch.Reset();
	bFindInFlight = false;
	bJoinInFlight = false;
	const bool bRetainHostCreate = bCreateInFlight
		&& HostCreateSessions.IsValid() && HostCreateHandle.IsValid()
		&& ActiveHostCreateSerial != 0;
	if (bRetainHostCreate)
	{
		// This provider operation can no longer authorize travel, even if a new
		// world is adopted before its callback arrives. Keep the exact completion
		// route alive solely to remove a late successful named session.
		bHostCreateAuthorityRevoked = true;
	}
	else
	{
		bCreateInFlight = false;
		bHostCreateAuthorityRevoked = false;
		ActiveHostCreateSerial = 0;
		HostCreateHandle.Reset();
		HostCreateSessions.Reset();
	}
	if (!bFailedHostDestroyInFlight)
	{
		FailedHostDestroyHandle.Reset();
		FailedHostWorld.Reset();
		FailedHostSessions.Reset();
	}
	bHostTravelPending = false;
	bHostTravelFailureHandled = false;
	HostTravelWorld.Reset();
	bClientConnectionPendingOrActive = false;
	if (!bRetainHostCreate)
	{
		PendingHostTravelURL.Reset();
		bPendingHostTravelAbsolute = false;
	}
	LastReturnTravelURL.Reset();
	LeaveState.Reset();
	ActiveHostAdminPassword.Reset();
	ActiveHostGamePassword.Reset();
	// An internet sign-in parked an accepted Host/Find. The Host publishes its
	// one terminal failure now, like every other revoked host operation; a
	// parked Find ends silently like the in-flight Find above. Late sign-in
	// results carry a stale serial and are ignored.
	CancelPendingInternetAction(/*bPublishHostFailure*/ true);
}

void UWoTSessionSubsystem::Deinitialize()
{
	UnregisterWoTBootstrapWorldReadinessListener(WorldReadinessHandle);
	const IOnlineSessionPtr PendingHostCreateSessions = HostCreateSessions;
	DeactivateForUnauthorizedWorld();
	if (PendingHostCreateSessions.IsValid() && HostCreateHandle.IsValid())
	{
		PendingHostCreateSessions->ClearOnCreateSessionCompleteDelegate_Handle(
			HostCreateHandle);
		HostCreateHandle.Reset();
		// The UObject cannot safely own a callback after subsystem shutdown.
		// Transfer only the best-effort cleanup obligation to a detached holder.
		if (PendingHostCreateSessions->GetNamedSession(NAME_GameSession) != nullptr)
		{
			WoTArmDetachedHostDestroyCleanup(PendingHostCreateSessions);
		}
		else if (bCreateInFlight)
		{
			WoTArmDetachedHostCreateCleanup(PendingHostCreateSessions);
		}
	}
	bCreateInFlight = false;
	bHostCreateAuthorityRevoked = false;
	ActiveHostCreateSerial = 0;
	HostCreateSessions.Reset();
	PendingHostTravelURL.Reset();
	bPendingHostTravelAbsolute = false;
	if (FailedHostSessions.IsValid() && FailedHostDestroyHandle.IsValid())
		FailedHostSessions->ClearOnDestroySessionCompleteDelegate_Handle(
			FailedHostDestroyHandle);
	if (FailedHostSessions.IsValid()
		&& FailedHostSessions->GetNamedSession(NAME_GameSession) != nullptr)
	{
		// The provider may still be completing an earlier destroy. Re-entering the
		// detached helper either owns its terminal callback or, if the provider
		// refuses the duplicate dispatch, removes the local row immediately.
		WoTArmDetachedHostDestroyCleanup(FailedHostSessions);
	}
	FailedHostDestroyHandle.Reset();
	FailedHostSessions.Reset();
	FailedHostWorld.Reset();
	bFailedHostDestroyInFlight = false;
	SearchCompleteEvent.Clear();
	HostCompleteEvent.Clear();
	JoinCompleteEvent.Clear();
	ConnectionFailureEvent.Clear();
	LastConnectionFailure.Reset();
	ClearPasswordTravelState();
	bRuntimeIntegrityFailureRetained = false;
	bRuntimeIntegrityRecoveryInFlight = false;
	RuntimeIntegrityRecoverySessions.Reset();
	StopInternetSignInTimeout();
	if (InternetBackend.IsValid())
	{
		InternetBackend->SetSignInLostHandler(nullptr);
		InternetBackend->SetTelemetryHandler(nullptr);
	}
	InternetBackend.Reset();
	bRelayControlReady = false;
	InternetStatusEvent.Clear();
	IdentityResetEvent.Clear();
#if WITH_AUTOMATION_TESTS
	ClientTravelHookForTesting = nullptr;
	ServerTravelHookForTesting = nullptr;
	CreateSessionDispatchHookForTesting = nullptr;
	ClientTravelControllerForTesting.Reset();
#endif
	Super::Deinitialize();
}

bool UWoTSessionSubsystem::OwnsFailure(
	UWorld* FailedWorld, const UNetDriver* NetDriver) const
{
	const UGameInstance* ThisGameInstance = GetGameInstance();
	if (!ThisGameInstance)
	{
		return false;
	}
	if (FailedWorld)
	{
		return FailedWorld->GetGameInstance() == ThisGameInstance;
	}
	if (NetDriver && GEngine)
	{
		// Pending connection failures are broadcast with a null world. Resolve
		// their pending net driver back to the owning world context/GI instead.
		const FWorldContext* Context =
			GEngine->GetWorldContextFromPendingNetGameNetDriver(NetDriver);
		return Context && Context->OwningGameInstance == ThisGameInstance;
	}
	return false;
}

void UWoTSessionSubsystem::RecordConnectionFailure(
	const TCHAR* FailureType, const FString& ErrorString)
{
	bClientConnectionPendingOrActive = false;
	ClearPasswordTravelState();
	RetainConnectionFailure(FailureType, ErrorString);
	// Keep the event argument stable independently of the copy retained for the
	// replacement menu world.
	const FString Failure = LastConnectionFailure;
	// Engine failure handling may also browse to the default map. Still drive the
	// explicit leave path so an OSS join is destroyed before our chosen return
	// travel, while a direct-IP failure takes the no-session branch.
	BeginLeave(/*bPreserveConnectionFailure*/ true);
	ConnectionFailureEvent.Broadcast(Failure);
}

void UWoTSessionSubsystem::RetainConnectionFailure(
	const TCHAR* FailureType, const FString& ErrorString)
{
	if (bRuntimeIntegrityFailureRetained) return;
	FString Detail = WoTSession_RedactTravelPasswords(ErrorString);
	Detail.TrimStartAndEndInline();
	LastConnectionFailure = Detail.IsEmpty()
		? FString::Printf(TEXT("Connection failed (%s)."), FailureType)
		: FString::Printf(TEXT("Connection failed (%s): %s"), FailureType, *Detail);
}

void UWoTSessionSubsystem::ReportRuntimeIntegrityFailure(FString Message)
{
	Message.TrimStartAndEndInline();
	if (Message.IsEmpty()) return;
	LastConnectionFailure = MoveTemp(Message);
	bRuntimeIntegrityFailureRetained = true;
	ConnectionFailureEvent.Broadcast(LastConnectionFailure);
}

bool UWoTSessionSubsystem::RecoverFromRuntimeIntegrityFailure(FString Message)
{
	ReportRuntimeIntegrityFailure(MoveTemp(Message));
	if (!IsWoTFrontEndIntegrityRecoveryAuthorized())
	{
		return false;
	}
	bRuntimeIntegrityRecoveryInFlight = true;
	if (!BeginLeave(/*bPreserveConnectionFailure*/ true))
	{
		bRuntimeIntegrityRecoveryInFlight = false;
		RuntimeIntegrityRecoverySessions.Reset();
		return false;
	}
	return true;
}

void UWoTSessionSubsystem::ClearHostTravelState()
{
	bHostTravelPending = false;
	bHostTravelFailureHandled = false;
	HostTravelWorld.Reset();
}

void UWoTSessionSubsystem::NotifySuccessfulHostDestinationExposure()
{
	ClearHostTravelState();
}

FString UWoTSessionSubsystem::ConsumeConnectionFailure()
{
	FString Failure = MoveTemp(LastConnectionFailure);
	LastConnectionFailure.Reset();
	// TickWorldTravel can synchronously construct the fallback front end before
	// broadcasting its terminal travel failure. An empty early consume must not
	// erase the classification needed by that later broadcast. Clean Leave
	// already clears it before travel; a non-empty consume is terminal too.
	if (!Failure.IsEmpty())
	{
		bRuntimeIntegrityFailureRetained = false;
		bClientConnectionPendingOrActive = false;
		DisconnectingPendingNetDriver = nullptr;
	}
	return Failure;
}

void UWoTSessionSubsystem::OnInviteAccepted(const bool bOk, const int32 /*ControllerId*/,
                                            FUniqueNetIdPtr /*UserId*/,
                                            const FOnlineSessionSearchResult& InviteResult)
{
	// The Steam overlay's "Join Game": ride the normal join/travel path.
	IOnlineSessionPtr S = Sessions();
	if (!CanUseAuthorizedWorld() || !bOk || !S.IsValid()
		|| !InviteResult.IsValid())
	{
		UE_LOG(LogNet, Display, TEXT("WoTSession: invite accept unusable"));
		return;
	}
	if (bJoinInFlight)
	{
		// Do not clear or stack onto the delegate owned by the active join.
		UE_LOG(LogNet, Display, TEXT("WoTSession: invite rejected; join in flight"));
		return;
	}
	if (LeaveState.GetState() != EWoTLeaveState::Idle)
	{
		UE_LOG(LogNet, Display, TEXT("WoTSession: invite rejected; leave in flight"));
		return;
	}
	bJoinInFlight = true;
	JoinSessions = S;
	S->OnJoinSessionCompleteDelegates.AddUObject(
	    this, &UWoTSessionSubsystem::OnJoinComplete);
	if (!S->JoinSession(0, NAME_GameSession, InviteResult))
	{
		// Some providers invoke the completion synchronously before returning.
		if (bJoinInFlight)
		{
			bJoinInFlight = false;
			S->ClearOnJoinSessionCompleteDelegates(this);
		}
	}
}

FString UWoTSessionSubsystem::ResolvedSubsystemName() const
{
	// The LAN provider actually adopted (the explicit Null OSS). Measured,
	// not assumed: a Mac run once refused FindSessions with no host present.
	const UWorld* World = GetGameInstance()
		? GetGameInstance()->GetWorld() : nullptr;
	const IOnlineSubsystem* OSS = Online::GetSubsystem(World, NULL_SUBSYSTEM);
	return OSS != nullptr ? OSS->GetSubsystemName().ToString()
		: FString(TEXT("none"));
}

IOnlineSessionPtr UWoTSessionSubsystem::Sessions() const
{
	if (!CanUseAuthorizedWorld())
	{
		return nullptr;
	}
	return AuthorizedSessions;
}

FString UWoTSessionSubsystem::HostDiagnostics() const
{
	IOnlineSessionPtr S = AuthorizedSessions;
	const IOnlineSessionPtr Internet = InternetBackend.IsValid()
		? InternetBackend->GetSessionInterface() : nullptr;
	const bool bNamed =
		(S.IsValid() && S->GetNamedSession(NAME_GameSession))
		|| (Internet.IsValid() && Internet->GetNamedSession(NAME_GameSession));
	const UWorld* World = GetGameInstance()
		? GetGameInstance()->GetWorld() : nullptr;
	const UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
	const FWoTInternetStatus Status = GetInternetStatus();
	// Packaged guarded builds compile logging out: this readout (and the
	// eos-state heartbeat) is the only internet-play evidence a field run has.
	// Appended keys never shadow the earlier tokens the funnel parses.
	return FString::Printf(
		TEXT("authorized=%d adopted=%d frontend=%d dedicated=%d sessions=%d ")
		TEXT("create_in_flight=%d leave=%d named=%d last=%s route=%s ")
		TEXT("find=%s join=%s results=%d oss=%s ")
		TEXT("internet=%s inet_reason=%s puid=%s host_provider=%s search=%s ")
		TEXT("%s bucket=%d netver=%s netdriver=%s"),
		bAuthorizedForWorld ? 1 : 0,
		IsWoTNormalWorldAuthorityAdopted() ? 1 : 0,
		FWoTFrontEndModel::CanConstructNormalFrontEnd(GetGameInstance()) ? 1 : 0,
		IsRunningDedicatedServer() ? 1 : 0,
		S.IsValid() ? 1 : 0,
		bCreateInFlight ? 1 : 0,
		static_cast<int32>(LeaveState.GetState()),
		bNamed ? 1 : 0,
		LastHostOutcome.IsEmpty() ? TEXT("none") : *LastHostOutcome,
		LastHostRoute.IsEmpty() ? TEXT("none") : *LastHostRoute,
		LastFindOutcome.IsEmpty() ? TEXT("none") : *LastFindOutcome,
		LastJoinOutcome.IsEmpty() ? TEXT("none") : *LastJoinOutcome,
		SearchResults.Num(),
		*ResolvedSubsystemName(),
		LexToString(Status.State),
		Status.Reason.IsEmpty() ? TEXT("none") : *Status.Reason,
		*Backend().RedactedUserId(),
		LastHostProvider.IsEmpty() ? TEXT("none") : *LastHostProvider,
		LastSearch.IsValid() ? (bSearchInternet ? TEXT("internet") : TEXT("lan"))
			: TEXT("none"),
		*Backend().Diagnostics(),
		GetBuildUniqueId(),
		*LocalNetVersionString(),
		Driver ? *Driver->GetClass()->GetName() : TEXT("none"));
}

FString UWoTSessionSubsystem::NetFailureHeartbeatDetail(const TCHAR* Kind,
	const TCHAR* FailureType, const TCHAR* Role, bool bClientConnection,
	const FString& Message)
{
	FString Trimmed = WoTSession_RedactTravelPasswords(Message);
	Trimmed.TrimStartAndEndInline();
	// The message is engine/peer text of unbounded length; the heartbeat line
	// escapes it, and the bound keeps one failure from dominating the log.
	return FString::Printf(TEXT("kind=%s type=%s role=%s client_connection=%d msg=%s"),
		Kind, FailureType, Role, bClientConnection ? 1 : 0,
		Trimmed.IsEmpty() ? TEXT("-") : *Trimmed.Left(400));
}

FString UWoTSessionSubsystem::SessionLeaveHeartbeatDetail(
	bool bConnectionFailure, bool bHasNamedSession)
{
	return FString::Printf(TEXT("reason=%s named_session=%d"),
		bConnectionFailure ? TEXT("connection-failure") : TEXT("requested"),
		bHasNamedSession ? 1 : 0);
}

APlayerController* UWoTSessionSubsystem::ResolveLocalPlayerController() const
{
#if WITH_AUTOMATION_TESTS
	if (ClientTravelControllerForTesting.IsValid())
	{
		return ClientTravelControllerForTesting.Get();
	}
#endif
	return GetGameInstance()
		? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
}

bool UWoTSessionSubsystem::TravelClientIfAuthorized(
	APlayerController* Controller, const FString& URL,
	ETravelType TravelType)
{
	// This is the last instruction before every client browse. Async OSS
	// completion can outlive receipt revocation, so entry-point checks alone are
	// insufficient.
	const bool bIntegrityReturn = bRuntimeIntegrityRecoveryInFlight
		&& URL == FWoTLeaveStateMachine::FrontEndURL()
		&& IsWoTFrontEndIntegrityRecoveryAuthorized();
	if ((!CanUseAuthorizedWorld() && !bIntegrityReturn)
		|| !Controller || URL.IsEmpty())
	{
		return false;
	}
	if (UWoTBootstrapSubsystem* Bootstrap = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->SuspendFrontEndSceneBeforeTravel();
	}
#if WITH_AUTOMATION_TESTS
	if (GWoTSceneTravelObserver) GWoTSceneTravelObserver();
#endif
#if WITH_AUTOMATION_TESTS
	if (ClientTravelHookForTesting)
	{
		ClientTravelHookForTesting(Controller, URL, TravelType);
		return true;
	}
#endif
	if (GIsAutomationTesting)
	{
		return true;
	}
	Controller->ClientTravel(URL, TravelType);
	return true;
}

bool UWoTSessionSubsystem::TravelServerIfAuthorized(
	UWorld* World, const FString& URL, bool bAbsoluteTravel)
{
	// Same final receipt recheck as the client path, immediately before the
	// asynchronous host-completion browse.
	if (!CanUseAuthorizedWorld() || !World || URL.IsEmpty())
	{
		return false;
	}
	if (UWoTBootstrapSubsystem* Bootstrap = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->SuspendFrontEndSceneBeforeTravel();
	}
#if WITH_AUTOMATION_TESTS
	if (GWoTSceneTravelObserver) GWoTSceneTravelObserver();
#endif
	bool bStarted = true;
#if WITH_AUTOMATION_TESTS
	if (ServerTravelHookForTesting)
		bStarted = ServerTravelHookForTesting(World, URL, bAbsoluteTravel);
	else
#endif
	if (!GIsAutomationTesting)
		bStarted = World->ServerTravel(URL, bAbsoluteTravel);
	return bStarted;
}

bool UWoTSessionSubsystem::HasSession() const
{
	return SessionsHoldingGameSession().IsValid();
}

bool UWoTSessionSubsystem::Host(int32 MaxPlayers, bool bLan)
{
	if (!CanUseAuthorizedWorld())
	{
		LastHostOutcome = TEXT("verb-can-use-refused");
		return false;
	}
	FWoTHostRequest Request;
	Request.MaxPlayers = MaxPlayers;
	Request.bLAN = bLan;
	// Preserve the retail-era console verb: current map, inherited current rules,
	// and no front-end faction injection. Only the menu request API adds defaults.
	// A LAN listen keeps the IP driver even when EOS started this run.
	return BeginHost(Request, AppendLanMatchOption(TEXT("?listen"), bLan),
		/*bAbsoluteTravel*/ false);
}

EWoTPlayerFaction UWoTSessionSubsystem::SelectedFaction() const
{
	if (const UWoTGameInstance* WoTGameInstance = Cast<UWoTGameInstance>(GetGameInstance()))
	{
		return WoTGameInstance->GetSelectedFaction();
	}
	return EWoTPlayerFaction::AesSedai;
}

EWoTPlayerSkinColor UWoTSessionSubsystem::SelectedSkinColor() const
{
	if (const UWoTGameInstance* WoTGameInstance =
		Cast<UWoTGameInstance>(GetGameInstance()))
	{
		return WoTGameInstance->GetSelectedSkinColor();
	}
	return EWoTPlayerSkinColor::Base;
}

FString UWoTSessionSubsystem::PlayerTravelURL(const FString& URL) const
{
	return FWoTFrontEndModel::AppendPlayerOptions(
		URL, SelectedFaction(), SelectedSkinColor());
}

FString UWoTSessionSubsystem::PasswordServerKey(const FString& URL)
{
	FString Server = URL;
	Server.TrimStartAndEndInline();
	int32 End = INDEX_NONE;
	const int32 Slash = Server.Find(TEXT("/"), ESearchCase::CaseSensitive);
	const int32 Options = Server.Find(TEXT("?"), ESearchCase::CaseSensitive);
	if (Slash != INDEX_NONE)
	{
		End = Slash;
	}
	if (Options != INDEX_NONE && (End == INDEX_NONE || Options < End))
	{
		End = Options;
	}
	if (End != INDEX_NONE)
	{
		Server.LeftInline(End, EAllowShrinking::No);
	}
	return Server.Left(128);
}

void UWoTSessionSubsystem::BeginPasswordCapableClientTravel(
	const FString& Connect, const FString& TravelURL)
{
	PendingClientTravelURL = TravelURL;
	PendingPasswordServer = PasswordServerKey(Connect);
	PasswordChallenge = EWoTPasswordChallenge::None;
	bSavedPasswordAttempted = false;
}

void UWoTSessionSubsystem::ClearPasswordTravelState()
{
	PasswordChallenge = EWoTPasswordChallenge::None;
	PendingClientTravelURL.Reset();
	PendingPasswordServer.Reset();
	bSavedPasswordAttempted = false;
}

bool UWoTSessionSubsystem::RetryRetainedClientTravel(
	const FString& Password, bool bSave)
{
	FString Sanitized;
	if (PendingClientTravelURL.IsEmpty() || PendingPasswordServer.IsEmpty()
		|| !FWoTFrontEndModel::SanitizeHostPassword(Password, Sanitized))
	{
		return false;
	}
	APlayerController* Controller = ResolveLocalPlayerController();
	const FString PasswordToken = WoTPasswordToken::Derive(Sanitized);
	if (PasswordToken.IsEmpty())
	{
		return false;
	}
	const FString TravelURL = PendingClientTravelURL
		+ TEXT("?PasswordToken=") + PasswordToken;
	if (!TravelClientIfAuthorized(Controller, TravelURL, TRAVEL_Absolute))
	{
		return false;
	}
	if (bSave)
	{
		if (UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
		{
			const FWoTBootstrapStatus Saved = Preferences->SaveServerPassword(
				PendingPasswordServer, Sanitized);
			if (Saved.IsError())
			{
				UE_LOG(LogNet, Warning,
					TEXT("WoTSession: server password preference was not saved"));
			}
		}
	}
	PasswordChallenge = EWoTPasswordChallenge::None;
	bClientConnectionPendingOrActive = true;
	LastConnectionFailure.Reset();
	return true;
}

bool UWoTSessionSubsystem::HandlePasswordFailure(const FString& ErrorString)
{
	const bool bNeed = ErrorString.Equals(TEXT("NEEDPW"),
		ESearchCase::IgnoreCase);
	const bool bWrong = ErrorString.Equals(TEXT("WRONGPW"),
		ESearchCase::IgnoreCase);
	if ((!bNeed && !bWrong) || PendingClientTravelURL.IsEmpty())
	{
		return false;
	}
	bClientConnectionPendingOrActive = false;
	DisconnectingPendingNetDriver = nullptr;
	if (bNeed && !bSavedPasswordAttempted)
	{
		bSavedPasswordAttempted = true;
		FString SavedPassword;
		if (const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
			Preferences && Preferences->GetSavedServerPassword(
				PendingPasswordServer, SavedPassword)
			&& RetryRetainedClientTravel(SavedPassword, /*bSave*/ false))
		{
			return true;
		}
	}
	PasswordChallenge = bWrong
		? EWoTPasswordChallenge::WrongPassword
		: EWoTPasswordChallenge::NeedPassword;
	LastConnectionFailure.Reset();
	ConnectionFailureEvent.Broadcast(ErrorString);
	return true;
}

bool UWoTSessionSubsystem::SubmitServerPassword(const FString& Password)
{
	return PasswordChallenge != EWoTPasswordChallenge::None
		&& RetryRetainedClientTravel(Password, /*bSave*/ true);
}

void UWoTSessionSubsystem::DismissPasswordChallenge()
{
	ClearPasswordTravelState();
	if (bCreateInFlight || bJoinInFlight)
	{
		return;
	}
	BeginLeave(/*bPreserveConnectionFailure*/ false,
		/*bReturnToMenu*/ false);
}

bool UWoTSessionSubsystem::Host(const FWoTHostRequest& Request)
{
	LastHostRoute.Reset();
	if (!CanUseAuthorizedWorld())
	{
		LastHostOutcome = TEXT("menu-can-use-refused");
		return false;
	}
	FString HostTravelURL =
		FWoTFrontEndModel::BuildHostTravelURL(
			Request, SelectedFaction(), SelectedSkinColor());
	if (HostTravelURL.IsEmpty())
	{
		LastHostOutcome = TEXT("empty-travel-url");
		return false;
	}
	// Guarded worlds materialize only through their neutral shells: the
	// engine hook's allow-list rejects a direct /Game/WoT/Maps/<map> travel
	// fail-closed (which surfaced as the host exiting mid-listen-travel).
	// Reroute the map segment to the authenticated shell while
	// keeping every travel option; unguarded contexts have no adopted
	// authority and keep the raw URL.
	if (const TOptional<FString> Shell = WoTGuardedShellPackageForMapIdentity(
			FWoTFrontEndModel::ShortMapName(Request.MapPackage));
		Shell.IsSet())
	{
		int32 OptionsIndex = INDEX_NONE;
		if (!HostTravelURL.FindChar(TEXT('?'), OptionsIndex))
		{
			HostTravelURL.FindChar(TEXT('#'), OptionsIndex);
		}
		HostTravelURL = OptionsIndex == INDEX_NONE
			? Shell.GetValue()
			: Shell.GetValue() + HostTravelURL.Mid(OptionsIndex);
		LastHostOutcome = FString::Printf(
			TEXT("shell-routed %s"), *Shell.GetValue());
		LastHostRoute = Shell.GetValue();
	}
	// The gate readout dies with the process, and a failed travel kills it:
	// record the resolved URL durably so a post-mortem shows whether routing
	// happened at all and exactly what was dispatched.
	WoTRecordBootstrapHeartbeat(TEXT("host-travel"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("url=%s route=%s"),
			*WoTSession_RedactTravelPasswords(HostTravelURL),
			LastHostRoute.IsEmpty() ? TEXT("none") : *LastHostRoute));
	FString SanitizedAdminPassword;
	FString SanitizedGamePassword;
	if (!FWoTFrontEndModel::SanitizeHostPassword(
			Request.AdminPassword, SanitizedAdminPassword)
		|| !FWoTFrontEndModel::SanitizeHostPassword(
			Request.GamePassword, SanitizedGamePassword))
	{
		return false;
	}
	FURL PasswordFreeURL(nullptr, *HostTravelURL, TRAVEL_Absolute);
	PasswordFreeURL.RemoveOption(TEXT("AdminPassword"));
	PasswordFreeURL.RemoveOption(TEXT("GamePassword"));
	HostTravelURL = PasswordFreeURL.ToString();
	const FString PreviousAdminPassword = ActiveHostAdminPassword;
	const FString PreviousGamePassword = ActiveHostGamePassword;
	ActiveHostAdminPassword = MoveTemp(SanitizedAdminPassword);
	ActiveHostGamePassword = MoveTemp(SanitizedGamePassword);
	const bool bAccepted = BeginHost(
		Request, HostTravelURL, /*bAbsoluteTravel*/ true);
	if (bAccepted)
	{
		ClearPasswordTravelState();
	}
	else
	{
		ActiveHostAdminPassword = PreviousAdminPassword;
		ActiveHostGamePassword = PreviousGamePassword;
	}
	return bAccepted;
}

bool UWoTSessionSubsystem::BeginHost(const FWoTHostRequest& Request,
	const FString& HostTravelURL, bool bAbsoluteTravel)
{
	if (!CanUseAuthorizedWorld())
	{
		LastHostOutcome = TEXT("begin-can-use-refused");
		return false;
	}
	const bool bNamedSessionExists = HasSession();
	if (LeaveState.GetState() != EWoTLeaveState::Idle
		|| bCreateInFlight || bFailedHostDestroyInFlight || bNamedSessionExists
		|| PendingInternetAction != EWoTPendingInternetAction::None
		|| bIdentityResetInFlight || bJoinCleanupPending)
	{
		LastHostOutcome = FString::Printf(
			TEXT("begin-state-refused leave=%d inflight=%d sessions=%d named=%d ")
			TEXT("internet_pending=%d reset=%d join_cleanup=%d"),
			static_cast<int32>(LeaveState.GetState()), bCreateInFlight ? 1 : 0,
			LanSessions().IsValid() ? 1 : 0, bNamedSessionExists ? 1 : 0,
			PendingInternetAction != EWoTPendingInternetAction::None ? 1 : 0,
			bIdentityResetInFlight ? 1 : 0, bJoinCleanupPending ? 1 : 0);
		return false;
	}
	IOnlineSessionPtr S;
	if (Request.bLAN)
	{
		S = LanSessions();
	}
	else
	{
		// Internet: an unavailable/unacknowledged EOS refuses synchronously
		// (the caller shows why); a usable but signed-out EOS accepts the
		// request, parks it, and signs in - the sign-in result resumes it or
		// publishes its one terminal failure.
		const FString Problem = PrepareInternetProvider();
		if (!Problem.IsEmpty())
		{
			LastHostOutcome = FString::Printf(
				TEXT("internet-unavailable reason=%s"), *Problem);
			return false;
		}
		if (!Backend().IsLoggedIn())
		{
			PendingInternetAction = EWoTPendingInternetAction::Host;
			PendingInternetHostRequest = Request;
			PendingInternetHostURL = HostTravelURL;
			bPendingInternetHostAbsolute = bAbsoluteTravel;
			LastHostOutcome = TEXT("internet-sign-in");
			if (!StartInternetSignIn())
			{
				PendingInternetAction = EWoTPendingInternetAction::None;
				LastHostOutcome = TEXT("internet-sign-in-refused");
				return false;
			}
			return true;
		}
		S = InternetSessions();
	}
	if (!S.IsValid())
	{
		LastHostOutcome = FString::Printf(
			TEXT("begin-state-refused provider=%s sessions=0"),
			Request.bLAN ? TEXT("lan") : TEXT("internet"));
		return false;
	}
	LastHostProvider = Request.bLAN ? TEXT("lan") : TEXT("internet");

	bCreateInFlight = true;
	bHostCreateAuthorityRevoked = false;
	ActiveHostCreateSerial = ++NextHostCreateSerial;
	if (ActiveHostCreateSerial == 0)
		ActiveHostCreateSerial = ++NextHostCreateSerial;
	const uint64 OperationSerial = ActiveHostCreateSerial;
	HostCreateSessions = S;
	bHostTravelFailureHandled = false;
	PendingHostTravelURL = HostTravelURL;
	bPendingHostTravelAbsolute = bAbsoluteTravel;
	LastHostTravelURL = WoTSession_RedactTravelPasswords(PendingHostTravelURL);

	FOnlineSessionSettings Settings = BuildSessionSettings(Request);

	HostCreateHandle = S->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateWeakLambda(this,
			[this, OperationSerial](FName SessionName, bool bOk)
			{
				OnCreateComplete(OperationSerial, SessionName, bOk);
			}));
	// OSS v1 completion carries only the session name, not an operation token.
	// The serial protects a copied/in-flight callback after its handle is cleared;
	// provider broadcasts are required by the interface contract to be terminal
	// exactly once for an accepted CreateSession operation.
	TFunction<bool()> DispatchCreate =
		[S, Settings = MoveTemp(Settings)]() mutable
		{
			return S->CreateSession(0, NAME_GameSession, Settings);
		};
	bool bDispatched = false;
#if WITH_AUTOMATION_TESTS
	if (CreateSessionDispatchHookForTesting)
		bDispatched = CreateSessionDispatchHookForTesting(MoveTemp(DispatchCreate));
	else
#endif
		bDispatched = DispatchCreate();
	if (!bDispatched)
	{
		if (bCreateInFlight && ActiveHostCreateSerial == OperationSerial)
		{
			bCreateInFlight = false;
			bHostCreateAuthorityRevoked = false;
			if (HostCreateHandle.IsValid())
				S->ClearOnCreateSessionCompleteDelegate_Handle(HostCreateHandle);
			HostCreateHandle.Reset();
			HostCreateSessions.Reset();
			ActiveHostCreateSerial = 0;
			PendingHostTravelURL.Reset();
			bPendingHostTravelAbsolute = false;
		}
		LastHostOutcome = TEXT("create-dispatch-failed");
		return false;
	}
	LastHostOutcome = TEXT("create-dispatched");
	return true;
}

void UWoTSessionSubsystem::OnCreateComplete(
	uint64 OperationSerial, FName SessionName, bool bOk)
{
	if (!bCreateInFlight || OperationSerial == 0
		|| OperationSerial != ActiveHostCreateSerial
		|| SessionName != NAME_GameSession)
	{
		return;
	}
	const bool bAuthorityWasRevoked = bHostCreateAuthorityRevoked;
	const IOnlineSessionPtr CompletedSessions = HostCreateSessions;
	bCreateInFlight = false;
	bHostCreateAuthorityRevoked = false;
	ActiveHostCreateSerial = 0;
	if (CompletedSessions.IsValid() && HostCreateHandle.IsValid())
	{
		CompletedSessions->ClearOnCreateSessionCompleteDelegate_Handle(
			HostCreateHandle);
	}
	HostCreateHandle.Reset();
	HostCreateSessions.Reset();
	UE_LOG(LogNet, Display, TEXT("WoTSession: create %s -> %s"),
	       *SessionName.ToString(), bOk ? TEXT("OK") : TEXT("FAILED"));
	const FString HostTravelURL = PendingHostTravelURL;
	const bool bAbsoluteTravel = bPendingHostTravelAbsolute;
	PendingHostTravelURL.Reset();
	bPendingHostTravelAbsolute = false;
	const bool bCreateOk = bOk;
	bOk = bOk && !bAuthorityWasRevoked && CanUseAuthorizedWorld();
	LastHostOutcome = FString::Printf(
		TEXT("create-complete ok=%d can_use=%d"),
		bCreateOk ? 1 : 0, bOk ? 1 : 0);
	// The host is a listen server. The menu request captured the selected map,
	// game mode, limits, and local player's Class before this async callback. Its
	// absolute travel prevents a prior LastURL game option surviving an Arena URL.
	// The legacy ?listen wrapper remains relative to preserve its old semantics.
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (bCreateOk)
	{
		if (bOk && World && !World->IsPlayingReplay())
		{
			// Install the failure route before dispatch because an engine/provider
			// hook may report ServerTravelFailure reentrantly.
			bHostTravelPending = true;
			HostTravelWorld = World;
			bOk = TravelServerIfAuthorized(World, HostTravelURL, bAbsoluteTravel);
			// The guarded client writes no log: name why a listen travel died.
			WoTRecordBootstrapHeartbeat(TEXT("host-travel-outcome"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("started=%d pending=%d can_use=%d handled=%d destroy=%d net=%d gamemode=%d"),
					bOk ? 1 : 0, bHostTravelPending ? 1 : 0,
					CanUseAuthorizedWorld() ? 1 : 0,
					bHostTravelFailureHandled ? 1 : 0,
					bFailedHostDestroyInFlight ? 1 : 0,
					static_cast<int32>(World->GetNetMode()),
					World->GetAuthGameMode() ? 1 : 0));
			// A reentrant engine/provider failure consumes the marker and may
			// complete teardown before ServerTravel returns. It is already the
			// sole terminal outcome regardless of the dispatch return value.
			if (bHostTravelFailureHandled || bFailedHostDestroyInFlight)
				return;
			if (bOk && bHostTravelPending && CanUseAuthorizedWorld())
			{
				HostCompleteEvent.Broadcast(true);
				return;
			}
		}
		BeginFailedHostCleanup(World, CompletedSessions);
		return;
	}
	ActiveHostAdminPassword.Reset();
	ActiveHostGamePassword.Reset();
	HostCompleteEvent.Broadcast(false);
}

void UWoTSessionSubsystem::RestoreFailedHostScene(UWorld* FailedWorld)
{
	if (FailedWorld == nullptr) return;
	if (UWoTBootstrapSubsystem* Bootstrap = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->EnsureFrontEndSceneForWorld(*FailedWorld,
			FailedWorld->GetFirstPlayerController());
	}
}

void UWoTSessionSubsystem::BeginFailedHostCleanup(
	UWorld* FailedWorld, IOnlineSessionPtr ExactSessions)
{
	check(!bFailedHostDestroyInFlight);
	bHostTravelFailureHandled = true;
	bHostTravelPending = false;
	HostTravelWorld.Reset();
	bFailedHostDestroyInFlight = true;
	FailedHostWorld = FailedWorld;
	// Presentation recovery is independent of provider teardown latency. The
	// retry/completion gate remains closed until DestroySession finishes.
	RestoreFailedHostScene(FailedWorld);
	IOnlineSessionPtr S = ExactSessions;
	if (!S.IsValid())
	{
		// Ungated on purpose (cleanup may run while authority is revoked): an
		// internet host's named session lives on the EOS provider.
		const IOnlineSessionPtr Internet = InternetBackend.IsValid()
			? InternetBackend->GetSessionInterface() : nullptr;
		S = (Internet.IsValid() && Internet->GetNamedSession(NAME_GameSession))
			? Internet : AuthorizedSessions;
	}
	if (!S.IsValid())
	{
		UWorld* LookupWorld = FailedWorld != nullptr ? FailedWorld
			: (GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr);
		if (const IOnlineSubsystem* OSS = Online::GetSubsystem(LookupWorld))
			S = OSS->GetSessionInterface();
	}
	FailedHostSessions = S;
	if (!S.IsValid() || S->GetNamedSession(NAME_GameSession) == nullptr)
	{
		CompleteFailedHostCleanup(false);
		return;
	}
	FailedHostDestroyHandle = S->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(
			this, &UWoTSessionSubsystem::OnFailedHostDestroyComplete));
	if (!S->DestroySession(NAME_GameSession) && bFailedHostDestroyInFlight)
		CompleteFailedHostCleanup(false);
}

void UWoTSessionSubsystem::OnFailedHostDestroyComplete(
	FName SessionName, bool bOk)
{
	if (!bFailedHostDestroyInFlight || SessionName != NAME_GameSession) return;
	CompleteFailedHostCleanup(bOk);
}

void UWoTSessionSubsystem::CompleteFailedHostCleanup(bool bDestroyOk)
{
	IOnlineSessionPtr S = FailedHostSessions;
	if (S.IsValid() && FailedHostDestroyHandle.IsValid())
		S->ClearOnDestroySessionCompleteDelegate_Handle(FailedHostDestroyHandle);
	FailedHostDestroyHandle.Reset();
	if (S.IsValid() && S->GetNamedSession(NAME_GameSession) != nullptr)
		S->RemoveNamedSession(NAME_GameSession);
	FailedHostWorld.Reset();
	FailedHostSessions.Reset();
	bFailedHostDestroyInFlight = false;
	ActiveHostAdminPassword.Reset();
	ActiveHostGamePassword.Reset();
	LastHostOutcome = FString::Printf(TEXT("failed-travel-destroy ok=%d"),
		bDestroyOk ? 1 : 0);
	HostCompleteEvent.Broadcast(false);
}

namespace
{
/**
 * Reproduce FLanBeacon::Init's socket setup to name WHICH step the engine
 * failed on.
 *
 * The joiner's FindSessions() returns false with no network traffic, and the
 * engine logs the reason -- but the guarded Test build compiles logging out
 * entirely (verified: -log/-abslog produce no file), so that line is
 * unreachable. This mirrors the same four calls and reports the failing one
 * plus the socket error.
 *
 * Runs ONLY after a refusal, never before delegating: binding this port
 * speculatively could cause the very failure it is meant to explain.
 */
FString WoTDescribeLanBeaconBind()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		return TEXT("no-socket-subsystem");
	}
	int32 Port = LAN_ANNOUNCE_PORT;
	if (GConfig != nullptr)
	{
		GConfig->GetInt(TEXT("LANSession"), TEXT("LanAnnouncePort"), Port,
			GEngineIni);
	}
	const TSharedPtr<FInternetAddr> BindAddr = Sockets->GetLocalBindAddr(*GLog);
	if (!BindAddr.IsValid())
	{
		return TEXT("no-local-bind-addr");
	}
	BindAddr->SetPort(Port);
	const FString Where = FString::Printf(TEXT("addr=%s"),
		*BindAddr->ToString(true));
	FSocket* Probe = Sockets->CreateSocket(NAME_DGram, TEXT("WoT LAN probe"),
		BindAddr->GetProtocolType());
	if (Probe == nullptr)
	{
		return Where + FString::Printf(TEXT(" create-failed err=%s"),
			Sockets->GetSocketError());
	}
	FString Result;
	Probe->SetReuseAddr();
	Probe->SetNonBlocking();
	if (!Probe->Bind(*BindAddr))
	{
		Result = Where + FString::Printf(TEXT(" bind-failed err=%s"),
			Sockets->GetSocketError());
	}
	else if (!Probe->SetBroadcast())
	{
		Result = Where + FString::Printf(TEXT(" broadcast-failed err=%s"),
			Sockets->GetSocketError());
	}
	else
	{
		Result = Where + TEXT(" bind-ok");
	}
	Probe->Close();
	Sockets->DestroySocket(Probe);
	return Result;
}
}

bool UWoTSessionSubsystem::Find(bool bLan)
{
	if (!CanUseAuthorizedWorld()
		|| LeaveState.GetState() != EWoTLeaveState::Idle || bFindInFlight
		|| (!bLan && (PendingInternetAction != EWoTPendingInternetAction::None
			|| bIdentityResetInFlight)))
	{
		LastFindOutcome = FString::Printf(
			TEXT("refused can_use=%d leave=%d in_flight=%d internet_pending=%d"),
			CanUseAuthorizedWorld() ? 1 : 0,
			static_cast<int32>(LeaveState.GetState()),
			bFindInFlight ? 1 : 0,
			PendingInternetAction != EWoTPendingInternetAction::None ? 1 : 0);
		return false;
	}
	LastFindOutcome = FString::Printf(TEXT("dispatched lan=%d"), bLan ? 1 : 0);

	bFindInFlight = true;
	if (++SearchGeneration == 0)
	{
		++SearchGeneration;
	}
	const uint64 RequestGeneration = SearchGeneration;
	SearchResults.Reset();
	LastSearch.Reset();
	SearchSessions.Reset();
	bSearchInternet = !bLan;
	if (bLan)
	{
		return DispatchFind(LanSessions(), /*bLan*/ true, RequestGeneration);
	}
	const FString Problem = PrepareInternetProvider();
	if (!Problem.IsEmpty())
	{
		// CompleteFind records its own outcome; keep the refusal reason.
		CompleteFind(false, RequestGeneration);
		LastFindOutcome = FString::Printf(
			TEXT("internet-unavailable reason=%s"), *Problem);
		return false;
	}
	if (!Backend().IsLoggedIn())
	{
		PendingInternetAction = EWoTPendingInternetAction::Find;
		PendingInternetFindGeneration = RequestGeneration;
		LastFindOutcome = TEXT("internet-sign-in");
		if (!StartInternetSignIn())
		{
			PendingInternetAction = EWoTPendingInternetAction::None;
			CompleteFind(false, RequestGeneration);
			LastFindOutcome = TEXT("internet-sign-in-refused");
			return false;
		}
		return true;
	}
	return DispatchFind(InternetSessions(), /*bLan*/ false, RequestGeneration);
}

bool UWoTSessionSubsystem::DispatchFind(IOnlineSessionPtr S, bool bLan,
	uint64 RequestGeneration)
{
	if (!CanUseAuthorizedWorld() || !bFindInFlight
		|| RequestGeneration != SearchGeneration)
	{
		return false;
	}
	if (!S.IsValid())
	{
		LastFindOutcome = TEXT("no-session-interface");
		CompleteFind(false, RequestGeneration);
		return false;
	}
	SearchSessions = S;
	bSearchInternet = !bLan;
	LastSearch = BuildSearch(bLan);
	FindHandle = S->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(
			this, &UWoTSessionSubsystem::OnFindComplete, RequestGeneration));
	if (!S->FindSessions(0, LastSearch.ToSharedRef()))
	{
		if (!bLan)
		{
			CompleteFind(false, RequestGeneration);
			LastFindOutcome = TEXT("find-lobbies-refused");
			return false;
		}
		// Capture the engine's own failure code FIRST: the mirror below makes
		// socket calls of its own and clobbers errno, and this thread is only
		// a few frames past the beacon's failed sendto.
		ISocketSubsystem* const ErrSockets =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		const FString EngineError = ErrSockets != nullptr
			? FString(ErrSockets->GetSocketError()) : FString(TEXT("none"));
		LastFindOutcome = FString::Printf(
			TEXT("find-sessions-refused err=%s beacon[%s]"),
			*EngineError, *WoTDescribeLanBeaconBind());
		// Null and other providers may have called OnFindComplete synchronously.
		// CompleteFind's in-flight consumption makes this fallback exactly-once.
		CompleteFind(false, RequestGeneration);
		return false;
	}
	return true;
}

void UWoTSessionSubsystem::CancelFind()
{
	if (!bFindInFlight)
	{
		return;
	}
	bFindInFlight = false;
	if (++SearchGeneration == 0)
	{
		++SearchGeneration;
	}
	// A Find parked behind an internet sign-in is dropped; the sign-in itself
	// runs on and is reused (or restarted) by a following internet host.
	PendingInternetFindGeneration = 0;
	if (PendingInternetAction == EWoTPendingInternetAction::Find)
	{
		PendingInternetAction = EWoTPendingInternetAction::None;
	}
	const IOnlineSessionPtr S = SearchSessions;
	if (S.IsValid())
	{
		if (FindHandle.IsValid())
		{
			S->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
		}
		S->CancelFindSessions();
	}
	FindHandle.Reset();
	SearchResults.Reset();
	LastSearch.Reset();
	LastFindOutcome = TEXT("cancelled");
}

void UWoTSessionSubsystem::OnFindComplete(bool bOk, uint64 RequestGeneration)
{
	CompleteFind(bOk, RequestGeneration);
}

void UWoTSessionSubsystem::CompleteFind(bool bOk, uint64 ExpectedGeneration)
{
	if (!bFindInFlight || ExpectedGeneration != SearchGeneration)
	{
		return;
	}
	bFindInFlight = false;
	bOk = bOk && CanUseAuthorizedWorld();
	IOnlineSessionPtr S = SearchSessions;
	if (S.IsValid() && FindHandle.IsValid())
	{
		S->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
	}
	FindHandle.Reset();
	SearchResults.Reset();
	const int32 Num = bOk && LastSearch.IsValid() ? LastSearch->SearchResults.Num() : 0;
	const FString LocalNetVersion = LocalNetVersionString();
	// Resolved into a member first: a UE_LOG argument is not evaluated in the
	// guarded build (NO_LOGGING), which is the repo-wide bug class.
	LastFindOutcome = FString::Printf(TEXT("complete ok=%d results=%d lan=%d"),
		bOk ? 1 : 0, Num, bSearchInternet ? 0 : 1);
	UE_LOG(LogNet, Display, TEXT("WoTSession: find -> %s, %d result(s)"),
	       bOk ? TEXT("OK") : TEXT("FAILED"), Num);
	for (int32 I = 0; I < Num; ++I)
	{
		const FOnlineSessionSearchResult& Raw = LastSearch->SearchResults[I];
		FWoTSessionListEntry& Entry = SearchResults.AddDefaulted_GetRef();
		Entry.SearchGeneration = SearchGeneration;
		Entry.ResultIndex = I;
		Raw.Session.SessionSettings.Get(WoTSession_ServerNameSetting, Entry.ServerName);
		if (Entry.ServerName.IsEmpty())
		{
			Entry.ServerName = Raw.Session.OwningUserName;
		}
		Raw.Session.SessionSettings.Get(SETTING_MAPNAME, Entry.MapName);
		Entry.MaxPlayers = Raw.Session.SessionSettings.NumPublicConnections;
		Entry.CurrentPlayers = FMath::Clamp(
			Entry.MaxPlayers - Raw.Session.NumOpenPublicConnections, 0, Entry.MaxPlayers);
		Entry.bInternet = bSearchInternet;
		Entry.PingMs = bSearchInternet ? WoTUnknownPingMs : Raw.PingInMs;
		WoTSessionPopulateVersionFields(
			Raw.Session.SessionSettings, LocalNetVersion, Entry);
		UE_LOG(LogNet, Display, TEXT("  [%d] %s (%s)"), I,
			*Entry.ServerName, *Entry.MapName);
	}
	CompletedSearchGeneration = SearchGeneration;
	if (!bOk)
	{
		LastSearch.Reset();
	}
	SearchCompleteEvent.Broadcast(bOk, SearchResults);
}

bool UWoTSessionSubsystem::Join(const FWoTSessionListEntry& Entry)
{
	if (!CanUseAuthorizedWorld()
		|| LeaveState.GetState() != EWoTLeaveState::Idle || bJoinInFlight
		|| bJoinCleanupPending)
	{
		LastJoinOutcome = FString::Printf(
			TEXT("refused can_use=%d leave=%d in_flight=%d join_cleanup=%d"),
			CanUseAuthorizedWorld() ? 1 : 0,
			static_cast<int32>(LeaveState.GetState()),
			bJoinInFlight ? 1 : 0, bJoinCleanupPending ? 1 : 0);
		return false;
	}
	if (bSearchInternet)
	{
		const FString Problem = PrepareInternetProvider();
		if (!Problem.IsEmpty())
		{
			LastJoinOutcome = FString::Printf(
				TEXT("internet-unavailable reason=%s"), *Problem);
			return false;
		}
	}
	IOnlineSessionPtr S = SearchSessions;
	if (S.IsValid() && bSearchInternet
		&& (!InternetBackend.IsValid() || !InternetBackend->IsLoggedIn()
			|| !bRelayControlReady || S != InternetSessions()))
	{
		// The internet search outlived its sign-in (expiry, reset): its lobby
		// handles belong to a user that is gone.
		LastJoinOutcome = TEXT("internet-signed-out");
		return false;
	}
	if (!S.IsValid() || bFindInFlight || !LastSearch.IsValid()
		|| Entry.SearchGeneration == 0
		|| Entry.SearchGeneration != SearchGeneration
		|| Entry.SearchGeneration != CompletedSearchGeneration
		|| !LastSearch->SearchResults.IsValidIndex(Entry.ResultIndex))
	{
		LastJoinOutcome = FString::Printf(
			TEXT("stale-entry sessions=%d finding=%d search=%d gen=%llu/%llu/%llu idx=%d valid=%d"),
			S.IsValid() ? 1 : 0, bFindInFlight ? 1 : 0,
			LastSearch.IsValid() ? 1 : 0,
			static_cast<unsigned long long>(Entry.SearchGeneration),
			static_cast<unsigned long long>(SearchGeneration),
			static_cast<unsigned long long>(CompletedSearchGeneration),
			Entry.ResultIndex,
			LastSearch.IsValid()
				&& LastSearch->SearchResults.IsValidIndex(Entry.ResultIndex)
				? 1 : 0);
		return false;
	}
	if (Entry.bVersionMismatch)
	{
		LastJoinOutcome = TEXT("version-mismatch");
		return false;
	}
	if (HasSession())
	{
		LastJoinOutcome = TEXT("named-session-exists");
		return false;
	}
	LastJoinOutcome = FString::Printf(TEXT("dispatched idx=%d provider=%s"),
		Entry.ResultIndex, bSearchInternet ? TEXT("internet") : TEXT("lan"));
	const EWoTPasswordChallenge PreviousPasswordChallenge = PasswordChallenge;
	const FString PreviousClientTravelURL = PendingClientTravelURL;
	const FString PreviousPasswordServer = PendingPasswordServer;
	const bool bPreviousSavedPasswordAttempted = bSavedPasswordAttempted;
	bJoinInFlight = true;
	ClearPasswordTravelState();
	JoinSessions = S;
	S->OnJoinSessionCompleteDelegates.AddUObject(
	    this, &UWoTSessionSubsystem::OnJoinComplete);
	if (!S->JoinSession(0, NAME_GameSession,
		LastSearch->SearchResults[Entry.ResultIndex]))
	{
		if (bJoinInFlight)
		{
			bJoinInFlight = false;
			S->ClearOnJoinSessionCompleteDelegates(this);
			PasswordChallenge = PreviousPasswordChallenge;
			PendingClientTravelURL = PreviousClientTravelURL;
			PendingPasswordServer = PreviousPasswordServer;
			bSavedPasswordAttempted = bPreviousSavedPasswordAttempted;
		}
		return false;
	}
	return true;
}

bool UWoTSessionSubsystem::Join(int32 Index)
{
	if (!CanUseAuthorizedWorld())
	{
		return false;
	}
	for (const FWoTSessionListEntry& Entry : SearchResults)
	{
		if (Entry.ResultIndex == Index)
		{
			return Join(Entry);
		}
	}
	return false;
}

void UWoTSessionSubsystem::OnJoinComplete(FName SessionName,
                                          EOnJoinSessionCompleteResult::Type Result)
{
	if (!bJoinInFlight)
	{
		return;
	}
	bJoinInFlight = false;
	const IOnlineSessionPtr JoinProvider =
		JoinSessions.IsValid() ? JoinSessions : Sessions();
	if (JoinProvider.IsValid())
	{
		JoinProvider->ClearOnJoinSessionCompleteDelegates(this);
	}
	IOnlineSessionPtr S = CanUseAuthorizedWorld() ? JoinProvider : nullptr;
	FString Connect;
	APlayerController* PC = ResolveLocalPlayerController();
	bool bSucceeded = Result == EOnJoinSessionCompleteResult::Success
		&& CanUseAuthorizedWorld() && S.IsValid()
		&& S->GetResolvedConnectString(SessionName, Connect) && PC;
	if (bSucceeded)
	{
		BeginPasswordCapableClientTravel(Connect, Connect);
		Connect = PlayerTravelURL(Connect);
		PendingClientTravelURL = Connect;
		UE_LOG(LogNet, Display, TEXT("WoTSession: joining %s"),
			*WoTSession_RedactTravelPasswords(Connect));
		bSucceeded = TravelClientIfAuthorized(
			PC, Connect, TRAVEL_Absolute);
		if (!bSucceeded)
		{
			ClearPasswordTravelState();
		}
	}
	if (bSucceeded)
	{
		bClientConnectionPendingOrActive = true;
		LastConnectionFailure.Reset();
		bRuntimeIntegrityFailureRetained = false;
	}
	if (!bSucceeded && JoinProvider.IsValid()
		&& JoinProvider->GetNamedSession(NAME_GameSession) != nullptr)
	{
		// The provider joined (EOS: the player is a lobby member) but no client
		// travel will happen: leave it BEFORE the failure is published, and
		// keep Host/Join/Leave/Reset closed until it is gone.
		LastJoinOutcome = TEXT("joined-but-not-travelled-leaving");
		bJoinCleanupPending = true;
		const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
		WoTArmDetachedHostDestroyCleanup(JoinProvider, [WeakThis]()
		{
			if (UWoTSessionSubsystem* This = WeakThis.Get())
			{
				This->FinishJoinCleanup(/*bPublishJoinFailure*/ true);
			}
		});
		UE_LOG(LogNet, Display, TEXT("WoTSession: join failed (%d)"),
		       static_cast<int32>(Result));
		return;
	}
	JoinCompleteEvent.Broadcast(bSucceeded);
	if (!bSucceeded)
	{
		UE_LOG(LogNet, Display, TEXT("WoTSession: join failed (%d)"),
		       static_cast<int32>(Result));
	}
}

bool UWoTSessionSubsystem::ConnectDirect(const FString& Address)
{
	if (!CanUseAuthorizedWorld()
		|| LeaveState.GetState() != EWoTLeaveState::Idle)
	{
		return false;
	}
	FString Connect;
	APlayerController* PC = ResolveLocalPlayerController();
	if (!FWoTFrontEndModel::SanitizeDirectAddress(Address, Connect) || !PC)
	{
		return false;
	}
	const FString TravelURL = PlayerTravelURL(Connect);
	BeginPasswordCapableClientTravel(Connect, TravelURL);
	if (!TravelClientIfAuthorized(PC, TravelURL, TRAVEL_Absolute))
	{
		ClearPasswordTravelState();
		return false;
	}
	bClientConnectionPendingOrActive = true;
	LastConnectionFailure.Reset();
	bRuntimeIntegrityFailureRetained = false;
	return true;
}

bool UWoTSessionSubsystem::Leave()
{
	if (!CanUseAuthorizedWorld())
	{
		return false;
	}
	ClearPasswordTravelState();
	return BeginLeave(/*bPreserveConnectionFailure*/ false);
}

bool UWoTSessionSubsystem::BeginLeave(bool bPreserveConnectionFailure,
	bool bReturnToMenu)
{
	// A pending join cleanup is already leaving the named session; a second
	// destroy through Leave would race it.
	const bool bIntegrityReturn = bRuntimeIntegrityRecoveryInFlight
		&& IsWoTFrontEndIntegrityRecoveryAuthorized();
	if ((!CanUseAuthorizedWorld() && !bIntegrityReturn)
		|| LeaveState.GetState() != EWoTLeaveState::Idle || bJoinCleanupPending
		|| bCreateInFlight || bJoinInFlight)
	{
		return false;
	}
	if (!bPreserveConnectionFailure)
	{
		LastConnectionFailure.Reset();
		bRuntimeIntegrityFailureRetained = false;
	}
	LastReturnTravelURL.Reset();
	bLeaveReturnsToMenu = bReturnToMenu;

	IOnlineSessionPtr S = bIntegrityReturn
		? RuntimeIntegrityRecoverySessions : SessionsHoldingGameSession();
	if (S.IsValid() && S->GetNamedSession(NAME_GameSession) == nullptr)
	{
		S.Reset();
	}
	const bool bHasNamedSession = S.IsValid();
	if (!LeaveState.Begin(bHasNamedSession))
	{
		return false;
	}
	WoTRecordBootstrapHeartbeat(TEXT("session-leave"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		SessionLeaveHeartbeatDetail(bPreserveConnectionFailure,
			bHasNamedSession));
	ClearHostTravelState();
	ActiveHostAdminPassword.Reset();
	ActiveHostGamePassword.Reset();
	// A deliberate destroy/disconnect must not be reclassified by the engine's
	// failure delegates as a new connection failure while Leave is in flight.
	bClientConnectionPendingOrActive = false;
	DisconnectingPendingNetDriver = nullptr;

	if (!bHasNamedSession)
	{
		if (bLeaveReturnsToMenu)
		{
			CompleteReturnToMenu();
		}
		else
		{
			LeaveState.Reset();
		}
		return true;
	}

	LeaveSessions = S;
	DestroyHandle = S->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(
			this, &UWoTSessionSubsystem::OnDestroyComplete));
	if (!S->DestroySession(NAME_GameSession))
	{
		// Providers may complete synchronously before returning false. Only
		// synthesize the terminal callback when the state was not consumed.
		if (LeaveState.GetState() == EWoTLeaveState::DestroyingSession)
		{
			OnDestroyComplete(NAME_GameSession, false);
		}
	}
	return true;
}

void UWoTSessionSubsystem::OnDestroyComplete(FName SessionName, bool bOk)
{
	const bool bIntegrityReturn = bRuntimeIntegrityRecoveryInFlight
		&& IsWoTFrontEndIntegrityRecoveryAuthorized();
	if ((!CanUseAuthorizedWorld() && !bIntegrityReturn)
		|| LeaveState.GetState() != EWoTLeaveState::DestroyingSession
		|| SessionName != NAME_GameSession)
	{
		return;
	}
	IOnlineSessionPtr S = LeaveSessions.IsValid() ? LeaveSessions : Sessions();
	LeaveSessions.Reset();
	if (S.IsValid() && DestroyHandle.IsValid())
	{
		S->ClearOnDestroySessionCompleteDelegate_Handle(DestroyHandle);
	}
	DestroyHandle.Reset();
	UE_LOG(LogNet, Display, TEXT("WoTSession: destroy %s -> %s"),
		*SessionName.ToString(), bOk ? TEXT("OK") : TEXT("FAILED"));
	// A provider's terminal failure must not strand a local named-session row and
	// block the roadmap's leave -> second host/join acceptance path. The remote
	// provider outcome is already terminal; this removes only our stale local row.
	if (S.IsValid() && S->GetNamedSession(NAME_GameSession))
	{
		S->RemoveNamedSession(NAME_GameSession);
	}
	LeaveState.CompleteDestroy();
	if (bLeaveReturnsToMenu)
	{
		CompleteReturnToMenu();
	}
	else
	{
		LeaveState.Reset();
	}
}

void UWoTSessionSubsystem::CompleteReturnToMenu()
{
	const bool bIntegrityReturn = bRuntimeIntegrityRecoveryInFlight
		&& IsWoTFrontEndIntegrityRecoveryAuthorized();
	if (!CanUseAuthorizedWorld() && !bIntegrityReturn)
	{
		return;
	}
	const FString ReturnURL = LeaveState.TakeReturnURL();
	if (ReturnURL.IsEmpty())
	{
		return;
	}

	LastReturnTravelURL = ReturnURL;
	bClientConnectionPendingOrActive = false;
	DisconnectingPendingNetDriver = nullptr;
	UE_LOG(LogNet, Display, TEXT("WoTSession: returning to %s"), *ReturnURL);
	APlayerController* PC = ResolveLocalPlayerController();
	if (PC)
	{
		// ClientTravel also browses the local world for a listen host's local
		// controller, closing that server and keeping every leave case on one URL.
		TravelClientIfAuthorized(PC, ReturnURL, TRAVEL_Absolute);
	}
	else
	{
		UE_LOG(LogNet, Warning,
			TEXT("WoTSession: no local controller available for front-end travel"));
	}
	if (bIntegrityReturn)
	{
		bRuntimeIntegrityRecoveryInFlight = false;
		RuntimeIntegrityRecoverySessions.Reset();
	}
}

// ---------------------------------------------------------------------------
// Internet play (EOS): providers, sign-in state machine, settings/query.
// ---------------------------------------------------------------------------

FString FWoTInternetStatus::UserText() const
{
	switch (State)
	{
	case EWoTInternetState::Unavailable:
	case EWoTInternetState::Failed:
		return FWoTEOSConfig::DescribeReason(Reason);
	case EWoTInternetState::Idle:
		return Reason == TEXT("signed-out")
			? FWoTEOSConfig::DescribeReason(Reason)
			: FString(TEXT("Internet: hosting or refreshing signs in anonymously through Epic Online Services."));
	case EWoTInternetState::CreatingDeviceId:
	case EWoTInternetState::LoggingIn:
		// The macOS keychain prompt is explained before sign-in starts (the
		// front end's keychain heads-up): this line is never drawn behind it.
		return TEXT("Signing in to Epic Online Services...");
	case EWoTInternetState::Ready:
		return TEXT("Internet: signed in to Epic Online Services.");
	case EWoTInternetState::Resetting:
		return TEXT("Resetting the online identity...");
	default:
		return FString();
	}
}

FString UWoTSessionSubsystem::LocalNetVersionString()
{
	// The exact value the engine's version handshake compares (the WoT
	// override is bound at module startup: WoTNetVersion::Bind).
	return FString::Printf(TEXT("%08x"), FNetworkVersion::GetLocalNetworkVersion());
}

FString UWoTSessionSubsystem::AppendLanMatchOption(const FString& URL, bool bLan)
{
	// NetDriverEOS::InitListen keeps an IP listen socket for a URL carrying
	// ?bIsLanMatch (NetDriverEOS.cpp); without it a LAN host would try an EOS
	// P2P listen as soon as EOS is started in this process.
	if (!bLan || URL.Contains(TEXT("?bIsLanMatch"), ESearchCase::IgnoreCase))
	{
		return URL;
	}
	int32 FragmentIndex = INDEX_NONE;
	if (URL.FindChar(TEXT('#'), FragmentIndex))
	{
		return URL.Left(FragmentIndex) + TEXT("?bIsLanMatch")
			+ URL.Mid(FragmentIndex);
	}
	return URL + TEXT("?bIsLanMatch");
}

IOnlineSessionPtr UWoTSessionSubsystem::LanSessions() const
{
	return Sessions();
}

IOnlineSessionPtr UWoTSessionSubsystem::InternetSessions() const
{
	if (!CanUseAuthorizedWorld() || !InternetBackend.IsValid())
	{
		return nullptr;
	}
	return InternetBackend->GetSessionInterface();
}

IOnlineSessionPtr UWoTSessionSubsystem::SessionsHoldingGameSession() const
{
	if (!CanUseAuthorizedWorld())
	{
		return nullptr;
	}
	const IOnlineSessionPtr Internet = InternetSessions();
	if (Internet.IsValid() && Internet->GetNamedSession(NAME_GameSession))
	{
		return Internet;
	}
	const IOnlineSessionPtr Lan = LanSessions();
	if (Lan.IsValid() && Lan->GetNamedSession(NAME_GameSession))
	{
		return Lan;
	}
	return nullptr;
}

IWoTInternetBackend& UWoTSessionSubsystem::Backend() const
{
	// Constructing the backend reads config and modular features only; the
	// EOS platform is created by EnsureStarted on an authorized action.
	if (!InternetBackend.IsValid())
	{
		InternetBackend = MakeWoTEOSInternetBackend();
	}
	return *InternetBackend;
}

FWoTInternetStatus UWoTSessionSubsystem::GetInternetStatus() const
{
	FWoTInternetStatus Status;
	if (InternetState == EWoTInternetState::CreatingDeviceId
		|| InternetState == EWoTInternetState::LoggingIn
		|| InternetState == EWoTInternetState::Resetting)
	{
		Status.State = InternetState;
		Status.Reason = InternetReason;
		Status.Detail = InternetDetail;
		return Status;
	}
	const FString Problem = Backend().QuickUnavailability();
	if (!Problem.IsEmpty())
	{
		Status.State = EWoTInternetState::Unavailable;
		Status.Reason = Problem;
		return Status;
	}
	if (Backend().IsLoggedIn())
	{
		if (bRelayControlReady && InternetState == EWoTInternetState::Ready)
		{
			Status.State = EWoTInternetState::Ready;
			return Status;
		}
		Status.State = EWoTInternetState::Failed;
		Status.Reason = InternetReason.IsEmpty()
			? FString(TEXT("relay-control-failed")) : InternetReason;
		Status.Detail = InternetDetail;
		return Status;
	}
	if (InternetState == EWoTInternetState::Failed)
	{
		Status.State = EWoTInternetState::Failed;
		Status.Reason = InternetReason;
		Status.Detail = InternetDetail;
		return Status;
	}
	Status.State = EWoTInternetState::Idle;
	if (InternetReason == TEXT("signed-out"))
	{
		Status.Reason = InternetReason;
	}
	return Status;
}

bool UWoTSessionSubsystem::IsOnlineIdentityAcknowledged() const
{
#if WITH_AUTOMATION_TESTS
	if (OnlineIdentityAcknowledgedForTesting.IsSet())
	{
		return OnlineIdentityAcknowledgedForTesting.GetValue();
	}
#endif
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
#if !WOT_GUARDED_BOOTSTRAP
	if (Preferences == nullptr)
	{
		return bOnlineIdentityAcknowledgedThisRun;
	}
#endif
	return Preferences != nullptr && Preferences->HasAcknowledgedOnlineIdentity();
}

EWoTKeychainAccess UWoTSessionSubsystem::ProbeSignInKeychainPrompt() const
{
	// Only a sign-in that is about to start can raise the prompt: the SDK
	// reads its Device ID credential in CreateDeviceId/Login.
	if (InternetState != EWoTInternetState::Idle
		&& InternetState != EWoTInternetState::Failed)
	{
		return EWoTKeychainAccess::NoPrompt;
	}
	// Failed can also mean "signed in, relay policy not applied": the next
	// action then retries the policy without reading the credential.
	if (!Backend().QuickUnavailability().IsEmpty() || Backend().IsLoggedIn())
	{
		return EWoTKeychainAccess::NoPrompt;
	}
	FString Detail;
	const EWoTKeychainAccess Access =
		WoTKeychainAccess::Probe(Backend().KeychainAccountId(), &Detail);
	WoTRecordBootstrapHeartbeat(TEXT("eos-keychain-probe"),
		EWoTBootstrapPhase::Ready, 0, {}, {}, Detail);
	return Access;
}

bool UWoTSessionSubsystem::AcknowledgeOnlineIdentity()
{
	if (!CanUseAuthorizedWorld())
	{
		return false;
	}
	bool bRecorded = false;
#if WITH_AUTOMATION_TESTS
	if (OnlineIdentityAcknowledgedForTesting.IsSet())
	{
		OnlineIdentityAcknowledgedForTesting = true;
		bRecorded = true;
	}
	else
#endif
	if (UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
	{
		bRecorded = !Preferences->SetOnlineIdentityAcknowledged(true).IsError();
	}
	else
	{
#if !WOT_GUARDED_BOOTSTRAP
		// Unguarded development run (editor / -game): no preference owner
		// exists; accept for this process only.
		bOnlineIdentityAcknowledgedThisRun = true;
		bRecorded = true;
#endif
	}
	WoTRecordBootstrapHeartbeat(TEXT("eos-identity"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("disclosure=accepted recorded=%d"), bRecorded ? 1 : 0));
	InternetStatusEvent.Broadcast();
	return bRecorded;
}

bool UWoTSessionSubsystem::AlwaysUseEpicRelay() const
{
#if WITH_AUTOMATION_TESTS
	if (AlwaysUseEpicRelayForTesting.IsSet())
	{
		return AlwaysUseEpicRelayForTesting.GetValue();
	}
#endif
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	return Preferences != nullptr && Preferences->HasLoadedFullPreferences()
		&& Preferences->GetPreferences().bAlwaysUseEpicRelay;
}

bool UWoTSessionSubsystem::SetAlwaysUseEpicRelay(bool bAlwaysUseRelay)
{
	if (!CanUseAuthorizedWorld())
	{
		return false;
	}
	const bool bPrevious = AlwaysUseEpicRelay();
	auto SavePreference = [this](bool bValue)
	{
#if WITH_AUTOMATION_TESTS
		if (RelayPreferenceSaveHookForTesting)
		{
			const bool bSaved = RelayPreferenceSaveHookForTesting(bValue);
			if (bSaved && AlwaysUseEpicRelayForTesting.IsSet())
			{
				AlwaysUseEpicRelayForTesting = bValue;
			}
			return bSaved;
		}
		if (AlwaysUseEpicRelayForTesting.IsSet())
		{
			AlwaysUseEpicRelayForTesting = bValue;
			return true;
		}
#endif
		if (UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
		{
			FWoTPlayerPreferences Candidate = Preferences->GetPreferences();
			Candidate.bAlwaysUseEpicRelay = bValue;
			return !Preferences->Save(Candidate).IsError();
		}
		return false;
	};
	if (!SavePreference(bAlwaysUseRelay))
	{
		return false;
	}
	if (InternetBackend.IsValid() && InternetBackend->IsLoggedIn())
	{
		const bool bWasRelayControlReady = bRelayControlReady;
		if (!ApplyRelayControlPreference())
		{
			// Do not leave the checkbox claiming privacy that EOS did not apply.
			const bool bRolledBack = SavePreference(bPrevious);
			bRelayControlReady = bRolledBack && bWasRelayControlReady;
			if (!bRelayControlReady)
			{
				SetInternetState(EWoTInternetState::Failed,
					TEXT("relay-control-failed"), FString());
			}
			return false;
		}
		SetInternetState(EWoTInternetState::Ready, FString(), FString());
	}
	InternetStatusEvent.Broadcast();
	return true;
}

bool UWoTSessionSubsystem::ResetOnlineIdentity()
{
	if (!CanUseAuthorizedWorld() || bIdentityResetInFlight
		|| PendingInternetAction != EWoTPendingInternetAction::None
		|| InternetState == EWoTInternetState::CreatingDeviceId
		|| InternetState == EWoTInternetState::LoggingIn
		|| bCreateInFlight || bFindInFlight || bJoinInFlight
		|| bJoinCleanupPending || bFailedHostDestroyInFlight || HasSession()
		|| LeaveState.GetState() != EWoTLeaveState::Idle)
	{
		return false;
	}
	// Deleting the Device ID needs the EOS platform; starting it here is an
	// explicit, authorized player action like any other internet action.
	const FString Problem = Backend().QuickUnavailability();
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!Problem.IsEmpty() || !Backend().EnsureStarted(World).IsEmpty())
	{
		return false;
	}
	bIdentityResetInFlight = true;
	bRelayControlReady = false;
	SearchResults.Reset();
	LastSearch.Reset();
	SearchSessions.Reset();
	const uint64 Serial = ++InternetSignInSerial;
	SetInternetState(EWoTInternetState::Resetting, FString(), FString());
	const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
	Backend().ResetIdentity(
		[WeakThis, Serial](bool bResetOk, const FString& ResetDetail)
		{
			UWoTSessionSubsystem* This = WeakThis.Get();
			if (This == nullptr || Serial != This->InternetSignInSerial)
			{
				return;
			}
			This->bIdentityResetInFlight = false;
			// Success means BOTH halves: the Device ID is gone from EOS and the
			// disclosure marker was really cleared (not merely unreadable while
			// preferences are dormant), so it is shown again before a new id.
			bool bForgot = false;
			if (bResetOk)
			{
#if WITH_AUTOMATION_TESTS
				if (This->OnlineIdentityAcknowledgedForTesting.IsSet())
				{
					This->OnlineIdentityAcknowledgedForTesting = false;
					bForgot = true;
				}
				else
#endif
				if (UWoTPreferencesSubsystem* Preferences = This->GetGameInstance()
					? This->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>()
					: nullptr)
				{
					bForgot = !Preferences->SetOnlineIdentityAcknowledged(false).IsError();
				}
				else
				{
#if !WOT_GUARDED_BOOTSTRAP
					This->bOnlineIdentityAcknowledgedThisRun = false;
					bForgot = true;
#endif
				}
			}
			const bool bReset = bResetOk && bForgot;
			WoTRecordBootstrapHeartbeat(TEXT("eos-identity"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("reset ok=%d device_id_deleted=%d disclosure_cleared=%d detail=%s"),
					bReset ? 1 : 0, bResetOk ? 1 : 0, bForgot ? 1 : 0,
					ResetDetail.IsEmpty() ? TEXT("none") : *ResetDetail.Left(120)));
			This->SetInternetState(
				bReset ? EWoTInternetState::Idle : EWoTInternetState::Failed,
				bReset ? FString() : FString(TEXT("reset-failed")), ResetDetail);
			This->IdentityResetEvent.Broadcast(bReset);
		});
	return true;
}

FString UWoTSessionSubsystem::PrepareInternetProvider()
{
	if (!CanUseAuthorizedWorld())
	{
		return TEXT("inactive");
	}
	const FString Problem = Backend().QuickUnavailability();
	if (!Problem.IsEmpty())
	{
		SetInternetState(EWoTInternetState::Unavailable, Problem, FString());
		return Problem;
	}
	// No Device ID is ever minted, and no EOS platform created, before the
	// player has accepted the one-time disclosure.
	if (!IsOnlineIdentityAcknowledged())
	{
		return TEXT("not-acknowledged");
	}
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const FString StartProblem = Backend().EnsureStarted(World);
	if (!StartProblem.IsEmpty())
	{
		SetInternetState(EWoTInternetState::Unavailable, StartProblem, FString());
		return StartProblem;
	}
	const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
	Backend().SetTelemetryHandler(
		[WeakThis](const TCHAR* Event, const FString& TelemetryDetail)
		{
			if (WeakThis.IsValid())
			{
				WoTRecordBootstrapHeartbeat(Event, EWoTBootstrapPhase::Ready,
					0, {}, {}, TelemetryDetail.Left(300));
			}
		});
	Backend().SetSignInLostHandler([WeakThis](const FString& LostReason)
	{
		if (UWoTSessionSubsystem* This = WeakThis.Get())
		{
			This->HandleInternetSignInLost(LostReason);
		}
	});
	if (!InternetSessions().IsValid())
	{
		SetInternetState(EWoTInternetState::Unavailable, TEXT("init-failed"),
			FString());
		return TEXT("init-failed");
	}
	if (Backend().IsLoggedIn() && !bRelayControlReady)
	{
		// A previous login or preference update may have authenticated EOS while
		// failing to enforce the requested relay policy. Retry it before every
		// new internet action; never treat authentication alone as readiness.
		if (!ApplyRelayControlPreference())
		{
			SetInternetState(EWoTInternetState::Failed,
				TEXT("relay-control-failed"), FString());
			return TEXT("relay-control-failed");
		}
		SetInternetState(EWoTInternetState::Ready, FString(), FString());
	}
	if (InternetState == EWoTInternetState::Unavailable)
	{
		SetInternetState(EWoTInternetState::Idle, FString(), FString());
	}
	return FString();
}

bool UWoTSessionSubsystem::StartInternetSignIn()
{
	if (!CanUseAuthorizedWorld())
	{
		return false;
	}
	const uint64 Serial = ++InternetSignInSerial;
	bRelayControlReady = false;
	InternetSignInDeadline = FPlatformTime::Seconds() + InternetSignInTimeoutSeconds;
	if (!InternetTimeoutHandle.IsValid())
	{
		InternetTimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateWeakLambda(this, [this](float)
			{
				return TickInternetSignInTimeout(FPlatformTime::Seconds());
			}), 0.25f);
	}
	SetInternetState(EWoTInternetState::CreatingDeviceId, FString(), FString());
	const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
	Backend().CreateDeviceId(
		[WeakThis, Serial](bool bDeviceIdOk, const FString& DeviceIdDetail)
		{
			if (UWoTSessionSubsystem* This = WeakThis.Get())
			{
				This->OnInternetDeviceId(Serial, bDeviceIdOk, DeviceIdDetail);
			}
		});
	return true;
}

void UWoTSessionSubsystem::OnInternetDeviceId(uint64 Serial, bool bOk,
	const FString& Detail)
{
	if (Serial != InternetSignInSerial
		|| InternetState != EWoTInternetState::CreatingDeviceId)
	{
		return;
	}
	if (!bOk || !CanUseAuthorizedWorld())
	{
		CompleteInternetSignIn(false, TEXT("device-id-failed"), Detail);
		return;
	}
	SetInternetState(EWoTInternetState::LoggingIn, FString(), Detail);
	const TWeakObjectPtr<UWoTSessionSubsystem> WeakThis(this);
	Backend().Login(InternetDisplayName(),
		[WeakThis, Serial](bool bLoginOk, const FString& LoginDetail)
		{
			if (UWoTSessionSubsystem* This = WeakThis.Get())
			{
				This->OnInternetLogin(Serial, bLoginOk, LoginDetail);
			}
		});
}

void UWoTSessionSubsystem::OnInternetLogin(uint64 Serial, bool bOk,
	const FString& Detail)
{
	if (Serial != InternetSignInSerial
		|| InternetState != EWoTInternetState::LoggingIn)
	{
		return;
	}
	CompleteInternetSignIn(bOk, bOk ? FString() : FString(TEXT("login-failed")),
		Detail);
}

void UWoTSessionSubsystem::CompleteInternetSignIn(bool bOk,
	const FString& Reason, const FString& Detail)
{
	StopInternetSignInTimeout();
	const bool bSignedIn = bOk && CanUseAuthorizedWorld()
		&& InternetBackend.IsValid() && InternetBackend->IsLoggedIn();
	// Apply before the parked Host/Find resumes. The engine's socket subsystem
	// reads its config once at init; no later engine write replaces this call.
	const bool bRelayApplied = bSignedIn && ApplyRelayControlPreference();
	const bool bReady = bSignedIn && bRelayApplied;
	SetInternetState(
		bReady ? EWoTInternetState::Ready : EWoTInternetState::Failed,
		bReady ? FString()
			: (!bRelayApplied && bSignedIn ? FString(TEXT("relay-control-failed"))
				: (Reason.IsEmpty() ? FString(TEXT("login-failed")) : Reason)),
		Detail);
	const EWoTPendingInternetAction Action = PendingInternetAction;
	PendingInternetAction = EWoTPendingInternetAction::None;
	if (Action == EWoTPendingInternetAction::Host)
	{
		const FWoTHostRequest Request = PendingInternetHostRequest;
		const FString HostTravelURL = PendingInternetHostURL;
		const bool bAbsoluteTravel = bPendingInternetHostAbsolute;
		PendingInternetHostURL.Reset();
		bPendingInternetHostAbsolute = false;
		// The accepted Host gets exactly one terminal result: either the
		// dispatched create's completion, or this failure.
		bool bDispatched = false;
		if (bReady)
		{
			bDispatched = BeginHost(Request, HostTravelURL, bAbsoluteTravel);
		}
		else
		{
			LastHostOutcome = FString::Printf(
				TEXT("internet-sign-in-failed reason=%s"), *InternetReason);
		}
		if (!bDispatched)
		{
			ActiveHostAdminPassword.Reset();
			ActiveHostGamePassword.Reset();
			HostCompleteEvent.Broadcast(false);
		}
	}
	else if (Action == EWoTPendingInternetAction::Find)
	{
		const uint64 Generation = PendingInternetFindGeneration;
		PendingInternetFindGeneration = 0;
		if (bFindInFlight && Generation == SearchGeneration)
		{
			if (bReady)
			{
				DispatchFind(InternetSessions(), /*bLan*/ false, Generation);
			}
			else
			{
				CompleteFind(false, Generation);
				LastFindOutcome = FString::Printf(
					TEXT("internet-sign-in-failed reason=%s"), *InternetReason);
			}
		}
	}
}

void UWoTSessionSubsystem::CancelPendingInternetAction(bool bPublishHostFailure)
{
	if (InternetState == EWoTInternetState::CreatingDeviceId
		|| InternetState == EWoTInternetState::LoggingIn)
	{
		++InternetSignInSerial;
		StopInternetSignInTimeout();
		SetInternetState(EWoTInternetState::Idle, FString(), FString());
	}
	const EWoTPendingInternetAction Action = PendingInternetAction;
	PendingInternetAction = EWoTPendingInternetAction::None;
	PendingInternetHostURL.Reset();
	bPendingInternetHostAbsolute = false;
	PendingInternetFindGeneration = 0;
	if (Action == EWoTPendingInternetAction::Host && bPublishHostFailure)
	{
		LastHostOutcome = TEXT("internet-sign-in-revoked");
		HostCompleteEvent.Broadcast(false);
	}
}

bool UWoTSessionSubsystem::TickInternetSignInTimeout(double Now)
{
	const bool bSigningIn = InternetState == EWoTInternetState::CreatingDeviceId
		|| InternetState == EWoTInternetState::LoggingIn;
	if (!bSigningIn)
	{
		InternetTimeoutHandle.Reset();
		return false;
	}
	if (Now < InternetSignInDeadline)
	{
		return true;
	}
	// Never RemoveTicker from inside this ticker's own delegate: forget the
	// handle and return false instead. Late backend results become stale.
	InternetTimeoutHandle.Reset();
	++InternetSignInSerial;
	CompleteInternetSignIn(false, TEXT("login-timeout"), FString());
	return false;
}

void UWoTSessionSubsystem::StopInternetSignInTimeout()
{
	if (InternetTimeoutHandle.IsValid())
	{
		FTSTicker::RemoveTicker(InternetTimeoutHandle);
	}
	InternetTimeoutHandle.Reset();
}

void UWoTSessionSubsystem::HandleInternetSignInLost(const FString& Reason)
{
	// A reset logs out on purpose, and an in-flight sign-in is decided by its
	// own result.
	if (bIdentityResetInFlight
		|| InternetState == EWoTInternetState::Resetting
		|| InternetState == EWoTInternetState::CreatingDeviceId
		|| InternetState == EWoTInternetState::LoggingIn)
	{
		return;
	}
	bRelayControlReady = false;
	SetInternetState(EWoTInternetState::Idle, Reason, FString());
}

bool UWoTSessionSubsystem::ApplyRelayControlPreference()
{
	if (!InternetBackend.IsValid() || !InternetBackend->IsLoggedIn())
	{
		bRelayControlReady = false;
		return false;
	}
	// EOS documents this as affecting new connections and reconnects only;
	// existing P2P connections are intentionally left unchanged.
	bRelayControlReady = InternetBackend->SetRelayControl(AlwaysUseEpicRelay()
		? EWoTRelayControl::ForceRelays : EWoTRelayControl::AllowRelays);
	return bRelayControlReady;
}

void UWoTSessionSubsystem::SetInternetState(EWoTInternetState State,
	const FString& Reason, const FString& Detail)
{
	const bool bChanged = State != InternetState || Reason != InternetReason;
	InternetState = State;
	InternetReason = Reason;
	InternetDetail = Detail;
	if (!bChanged)
	{
		return;
	}
	// Guarded Test/Shipping compile logging out: this is the field evidence.
	WoTRecordBootstrapHeartbeat(TEXT("eos-state"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("state=%s reason=%s detail=%s puid=%s %s"),
			LexToString(State), Reason.IsEmpty() ? TEXT("none") : *Reason,
			Detail.IsEmpty() ? TEXT("none") : *Detail.Left(200),
			*Backend().RedactedUserId(), *Backend().Diagnostics()));
	InternetStatusEvent.Broadcast();
}

FOnlineSessionSettings UWoTSessionSubsystem::BuildSessionSettings(
	const FWoTHostRequest& Request) const
{
	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections = FMath::Clamp(Request.MaxPlayers, 1, 16);
	Settings.bIsLANMatch = Request.bLAN;
	Settings.bShouldAdvertise = true;
	Settings.bUseLobbiesIfAvailable = true;
	Settings.bAllowJoinInProgress = true;
	if (Request.bLAN)
	{
		// LAN beacon (Null OSS): unchanged lobby-as-session settings.
		Settings.bUsesPresence = true;
		Settings.bAllowJoinViaPresence = true;
	}
	else
	{
		// One EOS lobby per listen host. Presence is an Epic Account Services
		// feature (not used): OSS EOS forces presence joins off without it
		// (OnlineSessionEOS.cpp CreateLobbySession). A listen host IS the
		// server, so the lobby must die with it instead of migrating to a
		// client that is not listening (lobby host migration defaults ON).
		Settings.bUsesPresence = false;
		Settings.bAllowJoinViaPresence = false;
		Settings.bAllowJoinViaPresenceFriendsOnly = false;
		Settings.Set(SETTING_HOST_MIGRATION, false,
			EOnlineDataAdvertisementType::DontAdvertise);
	}

	FString ServerName = Request.ServerName;
	ServerName.TrimStartAndEndInline();
	if (ServerName.IsEmpty())
	{
		ServerName = TEXT("WoT Server");
	}
	FString MapName = FWoTFrontEndModel::ShortMapName(Request.MapPackage);
	if (MapName.IsEmpty())
	{
		if (const UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
		{
			// Never advertise a guarded neutral shell (WoTMapShellNN).
			MapName = WoTWorldMapIdentity(World);
			MapName.RemoveFromStart(World->StreamingLevelsPrefix);
		}
	}
	Settings.Set(WoTSession_ServerNameSetting, ServerName,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	FString MapTitle = Request.MapTitle;
	MapTitle.TrimStartAndEndInline();
	if (MapTitle.IsEmpty())
	{
		MapTitle = FWoTFrontEndMapCatalog::ResolveRetailTitle(MapName);
	}
	Settings.Set(SETTING_MAPNAME, MapTitle,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(SETTING_GAMEMODE,
		FWoTFrontEndModel::IsBattleMap(Request.MapPackage)
			? FString(TEXT("Battle")) : FString(TEXT("Arena")),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(WoTSession_NetVersionSetting, LocalNetVersionString(),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(WoTSession_ReleaseSequenceSetting,
		FString::Printf(TEXT("%llu"),
			static_cast<unsigned long long>(RunningReleaseSequence())),
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	return Settings;
}

uint64 UWoTSessionSubsystem::RunningReleaseSequence() const
{
#if WITH_AUTOMATION_TESTS
	if (RunningReleaseSequenceForTesting.IsSet())
	{
		return RunningReleaseSequenceForTesting.GetValue();
	}
#endif
	const UGameInstance* Instance = GetGameInstance();
	const UWoTBootstrapSubsystem* Bootstrap = Instance
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
	return Bootstrap ? Bootstrap->Snapshot().RunningReleaseSequence : 0;
}

TSharedRef<FOnlineSessionSearch> UWoTSessionSubsystem::BuildSearch(bool bLan) const
{
	TSharedRef<FOnlineSessionSearch> Search = MakeShared<FOnlineSessionSearch>();
	Search->bIsLanQuery = bLan;
	if (bLan)
	{
		// Unchanged LAN beacon query.
		Search->MaxSearchResults = 32;
		Search->QuerySettings.Set(FName(TEXT("PRESENCESEARCH")), true,
			EOnlineComparisonOp::Equals);
	}
	else
	{
		// OSS EOS searches LOBBIES only when asked (OnlineSessionEOS.cpp
		// FindSessions), and turns every other query parameter into a lobby
		// attribute filter (FindLobbySession) - so nothing else is set: no
		// PRESENCESEARCH, which no lobby carries. The bucket (BuildIdOverride)
		// is applied by the provider itself.
		Search->MaxSearchResults = WoTInternetMaxSearchResults;
		Search->QuerySettings.Set(SEARCH_LOBBIES, true, EOnlineComparisonOp::Equals);
	}
	return Search;
}

void UWoTSessionSubsystem::FinishJoinCleanup(bool bPublishJoinFailure)
{
	if (!bJoinCleanupPending)
	{
		return;
	}
	bJoinCleanupPending = false;
	LastJoinOutcome = TEXT("joined-but-not-travelled-left");
	if (bPublishJoinFailure)
	{
		JoinCompleteEvent.Broadcast(false);
	}
}

#if WITH_AUTOMATION_TESTS
FWoTSessionListEntry UWoTSessionSubsystem::InjectSearchResultForTesting(
	IOnlineSessionPtr Provider, bool bInternet,
	const FOnlineSessionSearchResult& Result)
{
	if (++SearchGeneration == 0)
	{
		++SearchGeneration;
	}
	CompletedSearchGeneration = SearchGeneration;
	bFindInFlight = false;
	SearchSessions = Provider;
	bSearchInternet = bInternet;
	LastSearch = BuildSearch(!bInternet);
	LastSearch->SearchResults.Reset();
	LastSearch->SearchResults.Add(Result);
	SearchResults.Reset();
	FWoTSessionListEntry& Entry = SearchResults.AddDefaulted_GetRef();
	Entry.SearchGeneration = SearchGeneration;
	Entry.ResultIndex = 0;
	Entry.bInternet = bInternet;
	Result.Session.SessionSettings.Get(WoTSession_ServerNameSetting, Entry.ServerName);
	WoTSessionPopulateVersionFields(Result.Session.SessionSettings,
		LocalNetVersionString(), Entry);
	return Entry;
}
#endif
