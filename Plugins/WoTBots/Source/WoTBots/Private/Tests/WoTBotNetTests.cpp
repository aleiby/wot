#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "WoTBotController.h"
#include "WoTBotRoster.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTHUD.h"
#include "WoTPlayerState.h"
#include "WoTSessionSubsystem.h"
#include "Tests/WoTTestWorld.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Tests/NetTestHelpers.h"

namespace
{
UE::Net::FTestWorldInstance CreateBotListenServer(FString& Error)
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_ListenServer;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	++PIEInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			Error = TEXT("The listen game instance did not initialize.");
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;

	// The ordinary listen fixture has no local player. Give this host a real
	// local player before Browse so the fill target counts host and client.
	FWorldContext* Context = GameInstance->GetWorldContext();
	UGameViewportClient* Viewport = NewObject<UGameViewportClient>(
		GEngine, GEngine->GameViewportClientClass);
	Viewport->Init(*Context, GameInstance);
	Context->GameViewport = Viewport;
	if (!Viewport->SetupInitialLocalPlayer(Error))
	{
		return Instance;
	}
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?listen?bIsLanMatch"
			"?Game=/Script/WoT.WoTGameMode?BotFill=4"), TRAVEL_Absolute);
		Instance.bIsLoaded = GEngine->Browse(*Context, URL, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}

TArray<AWoTPlayerState*> BotStates(UWorld* World)
{
	TArray<AWoTPlayerState*> Result;
	if (AGameStateBase* GameState = World ? World->GetGameState() : nullptr)
	{
		for (APlayerState* State : GameState->PlayerArray)
		{
			AWoTPlayerState* Player = Cast<AWoTPlayerState>(State);
			if (IsValid(Player) && Player->IsABot() && !Player->IsInactive())
			{
				Result.Add(Player);
			}
		}
	}
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotsNetListenClientSpec,
	"WoT.Bots.Net.ListenClient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBotsNetListenClientSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions ActivationOptions;
	ActivationOptions.bStartPlay = false;
	UWorld* ActivationWorld = WoTTest::CreateSpecWorld(ActivationOptions);
	if (!TestNotNull(TEXT("session publication authority fixture exists"), ActivationWorld))
		return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(ActivationWorld); };

	UE::Net::FTestWorlds Worlds;
	FString Error;
	Worlds.Server = CreateBotListenServer(Error);
	Worlds.SetDescription(TEXT("bot listen-server lifecycle and client presentation"));
	if (!TestTrue(*FString::Printf(TEXT("listen server loads: %s"), *Error),
		Worlds.Server.IsLoaded())) return false;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	TestEqual(TEXT("host uses a real listen net driver"),
		ServerWorld->GetNetMode(), NM_ListenServer);
	AWoTGameMode* Mode = ServerWorld->GetAuthGameMode<AWoTGameMode>();
	UWoTBotRoster* Roster = ServerWorld->GetSubsystem<UWoTBotRoster>();
	APlayerController* Host = ServerWorld->GetFirstPlayerController();
	if (!TestNotNull(TEXT("Arena rules are active"), Mode)
		|| !TestNotNull(TEXT("authority roster exists"), Roster)
		|| !TestNotNull(TEXT("host has a local player controller"), Host)
		|| !TestTrue(TEXT("host controller is local"), Host->IsLocalController())
		|| !TestTrue(TEXT("URL activates the roster"), Roster->IsActive())) return false;

	// The empty map supplies no arena floor; keep the participants in the test
	// area while normal roster timers and the network drivers advance.
	ServerWorld->GetWorldSettings()->bGlobalGravitySet = true;
	ServerWorld->GetWorldSettings()->GlobalGravityZ = 0.0f;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!TestNotNull(TEXT("spare player start exists"),
			ServerWorld->SpawnActor<APlayerStart>(
				FVector(-10000.0f, Index * 1500.0f, 5000.0f), FRotator::ZeroRotator)))
		{
			return false;
		}
	}
	if (!Host->GetPawn()) Mode->RestartPlayer(Host);

	UWoTSessionSubsystem* HostSessions =
		Worlds.Server.GameInstance->GetSubsystem<UWoTSessionSubsystem>();
	const IOnlineSubsystem* ProviderSubsystem =
		Online::GetSubsystem(ServerWorld, FName(TEXT("NULL")));
	if (!TestNotNull(TEXT("host session subsystem exists"), HostSessions)
		|| !TestNotNull(TEXT("LAN session provider exists"), ProviderSubsystem)
		|| !TestTrue(TEXT("session publication is authorized"),
			HostSessions->ActivateForAuthorizedWorld())) return false;
	const IOnlineSessionPtr Provider = ProviderSubsystem->GetSessionInterface();
	if (!TestTrue(TEXT("LAN session interface exists"), Provider.IsValid())) return false;
	ON_SCOPE_EXIT
	{
		if (Provider->GetNamedSession(NAME_GameSession))
		{
			Provider->DestroySession(NAME_GameSession);
		}
	};
	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Engine/Maps/Entry");
	Request.MapTitle = TEXT("Arena");
	Request.MaxPlayers = 16;
	Request.bLAN = true;
	FOnlineSessionSettings Settings = HostSessions->BuildSessionSettingsForTesting(Request);
	// Exercise the real provider's update path without a machine-wide LAN beacon.
	Settings.bShouldAdvertise = false;
	if (!TestTrue(TEXT("named LAN session is created"),
		Provider->CreateSession(0, NAME_GameSession, Settings))
		|| !TestTrue(TEXT("one remote client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	TestEqual(TEXT("remote world is a client"), ClientWorld->GetNetMode(), NM_Client);
	TestNull(TEXT("client has no authority game mode"), ClientWorld->GetAuthGameMode());
	UWoTBotRoster* ClientRoster = ClientWorld->GetSubsystem<UWoTBotRoster>();
	TestTrue(TEXT("client does not manage a bot roster"),
		!ClientRoster || !ClientRoster->IsActive());
	if (!TestTrue(TEXT("host and client leave exactly two paced bot slots"),
		Worlds.TickUntil([Roster, ServerWorld]()
		{
			return Roster->GetDesiredBotCount() == 2 && Roster->GetBotCount() == 2
				&& BotStates(ServerWorld).Num() == 2;
		}, 1200))) return false;
	const TArray<AWoTPlayerState*> AuthorityBots = BotStates(ServerWorld);
	for (const AWoTPlayerState* State : AuthorityBots)
	{
		TestFalse(TEXT("authority bot has a name"), State->GetPlayerName().IsEmpty());
		TestNotNull(TEXT("authority bot has a playable class"), State->FactionClass.Get());
	}

	AWoTHUD* ClientHUD = nullptr;
	if (!TestTrue(TEXT("bot flags, names, classes and scoreboard rows reach the client"),
		Worlds.TickUntil([&ClientHUD, ClientWorld, &AuthorityBots]()
		{
			APlayerController* Controller = ClientWorld->GetFirstPlayerController();
			ClientHUD = Controller ? Cast<AWoTHUD>(Controller->GetHUD()) : nullptr;
			const TArray<AWoTPlayerState*> ReplicatedBots = BotStates(ClientWorld);
			if (!ClientHUD || ReplicatedBots.Num() != 2) return false;
			const FWoTScoreboardSnapshot Board = ClientHUD->GatherArenaScoreboard();
			if (Board.Players.Num() != 4) return false;
			for (const AWoTPlayerState* ServerState : AuthorityBots)
			{
				const bool bHasState = ReplicatedBots.ContainsByPredicate(
					[ServerState](const AWoTPlayerState* ClientState)
					{
						return ClientState != ServerState
							&& ClientState->GetPlayerId() == ServerState->GetPlayerId()
							&& ClientState->GetPlayerName() == ServerState->GetPlayerName()
							&& ClientState->FactionClass == ServerState->FactionClass;
					});
				const bool bHasRow = Board.Players.ContainsByPredicate(
					[ServerState](const FWoTScoreboardPlayer& Row)
					{
						return Row.bIsBot && Row.Name == ServerState->GetPlayerName();
					});
				if (!bHasState || !bHasRow) return false;
			}
			return true;
		}, 360))) return false;
	TestEqual(TEXT("client scoreboard contains two tagged bot rows"),
		ClientHUD->GatherArenaScoreboard().Players.FilterByPredicate(
			[](const FWoTScoreboardPlayer& Row) { return Row.bIsBot; }).Num(), 2);
	TestEqual(TEXT("client scoreboard contains the host and remote human"),
		ClientHUD->GatherArenaScoreboard().Players.FilterByPredicate(
			[](const FWoTScoreboardPlayer& Row) { return !Row.bIsBot; }).Num(), 2);
	TestFalse(TEXT("bot controllers stay on the server"),
		static_cast<bool>(TActorIterator<AWoTBotController>(ClientWorld)));

	const FName BotCountKey(TEXT("WOTBOTS"));
	if (!TestTrue(TEXT("roster coalescing publishes two bots to the live session"),
		Worlds.TickUntil([&Provider, BotCountKey]()
		{
			const FNamedOnlineSession* Session = Provider->GetNamedSession(NAME_GameSession);
			int32 Count = INDEX_NONE;
			return Session && Session->SessionSettings.Get(BotCountKey, Count) && Count == 2;
		}, 360))) return false;
	const FNamedOnlineSession* LiveSession = Provider->GetNamedSession(NAME_GameSession);
	const FOnlineSessionSetting* Published = LiveSession->SessionSettings.Settings.Find(BotCountKey);
	TestTrue(TEXT("bot count is available to service and LAN discovery"), Published
		&& Published->AdvertisementType == EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	UWoTSessionSubsystem* ClientSessions =
		Worlds.Clients[0].GameInstance->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("client session subsystem exists"), ClientSessions)) return false;
	// Session settings use discovery, not actor replication. Pass the live
	// provider's actual session through the client-side search-result adapter.
	FOnlineSessionSearchResult SearchResult;
	SearchResult.Session = *LiveSession;
	const FWoTSessionListEntry ClientEntry =
		ClientSessions->InjectSearchResultForTesting(nullptr, false, SearchResult);
	TestEqual(TEXT("client browser reads the host's advertised bot count"), ClientEntry.BotCount, 2);
	TestEqual(TEXT("client retains the browser result"),
		ClientSessions->GetSearchResults().Num(), 1);
	return !HasAnyErrors();
}

#endif
