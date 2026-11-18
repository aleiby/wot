#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTFrontEnd.h"
#include "../WoTGameInstance.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSubsystemUtils.h"

#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags Fix9_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

void Fix9_SendActionKey(AWoTPlayerController& Controller,
	UInputComponent* PawnInput, const FKey& Key, EInputEvent Event,
	bool bPaused)
{
	Controller.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		Key, Event, Event == IE_Pressed ? 1.0f : 0.0f));
	TArray<UInputComponent*> Stack;
	if (PawnInput)
	{
		Stack.Push(PawnInput);
	}
	Stack.Push(Controller.InputComponent);
	Controller.PlayerInput->ProcessInputStack(
		Stack, 1.0f / 60.0f, bPaused);
}

AWoTNPCCharacter* Fix9_SpawnCreature(UWorld& World, const FVector& Location)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(World,
		TEXT("Trolloc"), Location, 1, true, AWoTTestNPCCharacter::StaticClass());
	if (NPC)
	{
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return NPC;
}

bool Fix9_PublishWorldAuthority()
{
	ResetWoTPayloadActivationForTesting();
	return !PublishWoTPayloadBootstrapActivationForTesting().IsError()
		&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
}

class Fix9_LogCapture final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type,
		const FName& Category) override
	{
		Text += Category.ToString() + TEXT(": ") + Message + TEXT("\n");
	}
	FString Text;
};

#if WITH_EDITOR
int32 Fix9_UnusedPIEInstance()
{
	int32 Maximum = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		Maximum = FMath::Max(Maximum, Context.PIEInstance);
	}
	return Maximum + 1;
}

UE::Net::FTestWorldInstance Fix9_CreateListenServer(
	const FString& AdditionalOptions = FString(),
	const FString& GameMode = TEXT("/Script/WoT.WoTBattleGameMode"))
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UWoTGameInstance* GameInstance = NewObject<UWoTGameInstance>(GEngine);
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
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(
				Fix9_UnusedPIEInstance(), Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	FWorldContext* Context = GameInstance->GetWorldContext();
	UGameViewportClient* Viewport = NewObject<UGameViewportClient>(
		GEngine, GEngine->GameViewportClientClass);
	Viewport->Init(*Context, GameInstance);
	Context->GameViewport = Viewport;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FString ListenURL = TEXT("/Engine/Maps/Entry?listen?Game=")
			+ GameMode + AdditionalOptions;
		const FURL URL(nullptr, *ListenURL, TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*Context, URL, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}

UE::Net::FTestWorldInstance Fix9_CreateBattleClient(int32 ServerPort,
	const FString& AdditionalOptions = FString())
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UWoTGameInstance* GameInstance = NewObject<UWoTGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_Client;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(
				Fix9_UnusedPIEInstance(), Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	FWorldContext* Context = GameInstance->GetWorldContext();
	UGameViewportClient* Viewport = NewObject<UGameViewportClient>(
		GEngine, GEngine->GameViewportClientClass);
	Viewport->Init(*Context, GameInstance);
	Context->GameViewport = Viewport;
	FString PlayerError;
	Viewport->SetupInitialLocalPlayer(PlayerError);
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		GEngine->BrowseToDefaultMap(*Context);
		const FString Address = FString::Printf(TEXT("127.0.0.1:%d"), ServerPort)
			+ AdditionalOptions;
		const FURL URL(nullptr, *Address, TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(*Context, URL, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}
#endif
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix9BattleClientCitadelSkinSpec,
	"WoT.Parity.Fix9.BattleClientCitadelSkin", Fix9_Flags)

bool FFix9BattleClientCitadelSkinSpec::RunTest(const FString&)
{
	using namespace UE::Net;
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix9BattleSkin-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};

	FTestWorlds Worlds;
	Worlds.Server = Fix9_CreateListenServer();
	Worlds.SetDescription(TEXT("Fix9 owning-client citadel skin selection"));
	if (!TestTrue(TEXT("listen Battle server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	AWoTBattleGameMode* Mode = ServerWorld
		? ServerWorld->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	UWoTGameInstance* HostInstance = Cast<UWoTGameInstance>(
		Worlds.Server.GameInstance);
	AWoTBattleInfo* Battle = ServerWorld
		? ServerWorld->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity) : nullptr;
	if (Battle)
	{
		Battle->NumTeams = 2;
		Battle->TeamClass[0] = AWoTHound::StaticClass();
		Battle->TeamClass[1] = AWoTForsaken::StaticClass();
		Battle->bTeleportersDisabled = false;
		Battle->FinishSpawning(FTransform::Identity);
	}
	if (!TestNotNull(TEXT("Battle rules"), Mode)
		|| !TestNotNull(TEXT("host game instance"), HostInstance)
		|| !TestNotNull(TEXT("BattleInfo"), Battle))
	{
		return false;
	}
	Mode->Battle = Battle;
	HostInstance->SetCitadelSkinColor(EWoTPlayerSkinColor::Green, false);
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(-4000.0, -1000.0, 5000.0), FRotator::ZeroRotator);
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(-4000.0, 1000.0, 5000.0), FRotator::ZeroRotator);

	AWoTPlayerController* HostController =
		ServerWorld->SpawnActor<AWoTPlayerController>();
	AWoTAesSedai* HostPawn = ServerWorld->SpawnActor<AWoTAesSedai>(
		FVector(-1000.0, 0.0, 5000.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("listen host controller"), HostController)
		|| !TestNotNull(TEXT("listen host pawn"), HostPawn))
	{
		return false;
	}
	FString HostPlayerError;
	ULocalPlayer* HostPlayer = HostInstance->CreateLocalPlayer(
		0, HostPlayerError, /*bSpawnPlayerController*/ false);
	if (!TestNotNull(TEXT("listen host local player"), HostPlayer))
	{
		return false;
	}
	HostController->SetPlayer(HostPlayer);
	HostController->Possess(HostPawn);
	HostPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	HostPawn->ForceNetUpdate();

	Worlds.Clients.Emplace(Fix9_CreateBattleClient(Worlds.Server.GetPort()));
	if (!TestTrue(TEXT("remote Battle client connects"),
		Worlds.WaitForClientConnect(Worlds.Clients.Last())))
	{
		return false;
	}
	UWoTGameInstance* ClientInstance = Cast<UWoTGameInstance>(
		Worlds.Clients[0].GameInstance);
	AWoTPlayerController* ClientController = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTPlayerController* RemoteServerController = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* RemoteServerPawn = RemoteServerController
		? Cast<AWoTCharacter>(RemoteServerController->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("remote game instance"), ClientInstance)
		|| !TestNotNull(TEXT("remote owning controller"), ClientController)
		|| !TestNotNull(TEXT("remote authority controller"), RemoteServerController)
		|| !TestNotNull(TEXT("remote authority pawn"), RemoteServerPawn))
	{
		return false;
	}
	ClientInstance->SetCitadelSkinColor(EWoTPlayerSkinColor::Blue, false);

	TestTrue(TEXT("host requested team assignment succeeds"),
		Mode->ChangeTeam(HostController, 0));
	ClientController->ServerChangeTeam(1);
	TestTrue(TEXT("remote requested team assignment reaches authority"),
		Worlds.TickUntil([RemoteServerController]()
		{
			const AWoTPlayerState* State =
				RemoteServerController->GetPlayerState<AWoTPlayerState>();
			return State && State->Team == 1;
		}, 120));

	AWoTCharacter* HostClientPawn = nullptr;
	AWoTCharacter* RemoteClientPawn = nullptr;
	TestTrue(TEXT("both requested-assignment skins replicate to every machine"),
		Worlds.TickUntil([&]()
		{
			HostClientPawn = Worlds.FindReplicatedObjectOnClient(HostPawn, 0);
			RemoteClientPawn = Worlds.FindReplicatedObjectOnClient(RemoteServerPawn, 0);
			return HostClientPawn && RemoteClientPawn
				&& HostPawn->PersonalSkin == FName(TEXT("Hou_Green"))
				&& RemoteServerPawn->PersonalSkin == FName(TEXT("For_Blue"))
				&& HostClientPawn->PersonalSkin == FName(TEXT("Hou_Green"))
				&& RemoteClientPawn->PersonalSkin == FName(TEXT("For_Blue"));
		}, 180));
	TestEqual(TEXT("requested host skin on listen authority"),
		HostPawn->PersonalSkin, FName(TEXT("Hou_Green")));
	TestEqual(TEXT("requested remote skin on listen authority"),
		RemoteServerPawn->PersonalSkin, FName(TEXT("For_Blue")));
	if (HostClientPawn && RemoteClientPawn)
	{
		TestEqual(TEXT("requested host skin on remote machine"),
			HostClientPawn->PersonalSkin, FName(TEXT("Hou_Green")));
		TestEqual(TEXT("requested owner skin on remote machine"),
			RemoteClientPawn->PersonalSkin, FName(TEXT("For_Blue")));
	}

	ClientController->ServerChangeTeam(255);
	TestTrue(TEXT("remote player becomes neutral before automatic assignment"),
		Worlds.TickUntil([RemoteServerController]()
		{
			const AWoTPlayerState* State =
				RemoteServerController->GetPlayerState<AWoTPlayerState>();
			return State && State->Team == 255;
		}, 120));
	ClientInstance->SetCitadelSkinColor(EWoTPlayerSkinColor::Red, false);
	RemoteServerController->JoinTeamTimeout = 1;
	Mode->Timer();
	TestTrue(TEXT("JoinTeamTimeout uses the owner's changed citadel colour"),
		Worlds.TickUntil([&]()
		{
			const AWoTPlayerState* State =
				RemoteServerController->GetPlayerState<AWoTPlayerState>();
			return State && State->Team == 1 && RemoteClientPawn
				&& RemoteServerPawn->PersonalSkin == FName(TEXT("For_Red"))
				&& RemoteClientPawn->PersonalSkin == FName(TEXT("For_Red"))
				&& HostPawn->PersonalSkin == FName(TEXT("Hou_Green"))
				&& HostClientPawn
				&& HostClientPawn->PersonalSkin == FName(TEXT("Hou_Green"));
		}, 180));
	TestEqual(TEXT("automatic remote skin on listen authority"),
		RemoteServerPawn->PersonalSkin, FName(TEXT("For_Red")));
	if (RemoteClientPawn)
	{
		TestEqual(TEXT("automatic owner skin on remote machine"),
			RemoteClientPawn->PersonalSkin, FName(TEXT("For_Red")));
	}
	return !HasAnyErrors();
}
#endif

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix9PausedChatKeepsWeaveSpec,
	"WoT.Parity.Fix9.PausedChatKeepsWeave", Fix9_Flags)

bool FFix9PausedChatKeepsWeaveSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		World->WorldType = EWorldType::PIE;
		World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Controller = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator) : nullptr;
	AWoTCharacter* Victim = World ? World->SpawnActor<AWoTCharacter>(
		FVector(300.0, 0.0, 542.0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("listen rules"), Game)
		|| !TestNotNull(TEXT("local controller"), Controller)
		|| !TestNotNull(TEXT("caster"), Pawn)
		|| !TestNotNull(TEXT("balefire victim"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	Controller->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	Controller->PlayerInput = NewObject<UPlayerInput>(Controller);
	Controller->SetupInputComponent();
	UInputComponent* PawnInput = NewObject<UInputComponent>(Pawn);
	Pawn->SetupPlayerInputComponent(PawnInput);
	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
	Balefire->GiveTo(Pawn);
	Pawn->InventoryComponent->SelectItem(Balefire);
	WoTTest::TickWorld(World, 0.1f);

	Fix9_SendActionKey(*Controller, PawnInput, EKeys::LeftMouseButton,
		IE_Pressed, false);
	WoTTest::TickWorld(World, 2.5f);
	TestEqual(TEXT("held balefire remains charged before pause"),
		Victim->Health, 1000);
	TestTrue(TEXT("real listen pause succeeds"),
		Game->TrySetPlayerPause(Controller, NM_ListenServer));
	Fix9_SendActionKey(*Controller, PawnInput, EKeys::T, IE_Pressed, true);
	TestTrue(TEXT("Talk still opens typing while paused"),
		Controller->IsChatTyping());
	TestEqual(TEXT("opening paused chat does not release the active weave"),
		Victim->Health, 1000);

	Fix9_SendActionKey(*Controller, PawnInput, EKeys::LeftMouseButton,
		IE_Released, true);
	TestEqual(TEXT("the preserved cast releases while chat remains paused"),
		Victim->Health, 600);
	TestTrue(TEXT("the gameplay release does not close chat"),
		Controller->IsChatTyping());
	Controller->CancelChatInput();
	Game->ClearPause();
	TestEqual(TEXT("unpause adds no second Fire transition"),
		Victim->Health, 600);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix9NPCReturnBackoffSpec,
	"WoT.Parity.Fix9.NPCReturnBackoff", Fix9_Flags)

bool FFix9NPCReturnBackoffSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = World
		? Fix9_SpawnCreature(*World, FVector(0.0, 0.0, 100.0)) : nullptr;
	AWoTNPCController* Controller = NPC
		? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = NPC
		? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
	if (TestNotNull(TEXT("returning guard"), NPC)
		&& TestNotNull(TEXT("returning controller"), Controller)
		&& TestNotNull(TEXT("test brain"), Brain))
	{
		Brain->bForceReachable = false;
		Brain->bForcePathable = true;
		const FVector OriginalPost(1000.0, 0.0, 100.0);
		Brain->SetWaitingGoal(OriginalPost,
			FRotator::ZeroRotator);
		Controller->TransitionOnGoalPriority(true);
		Brain->Fix9PathableQueryCount = 0;
		for (int32 Tick = 0; Tick < 50; ++Tick)
		{
			Controller->Tick(0.001f);
		}
		TestTrue(TEXT("persistent failure does not classify and request every tick"),
			Brain->Fix9PathableQueryCount < 50);
		TestEqual(TEXT("off-mesh retries do not consume the return failure bound"),
			Controller->GetBrainState(), EWoTBrainState::Returning);
		TestTrue(TEXT("off-mesh retries preserve the authored guard post"),
			Brain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
				OriginalPost, 0.01f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix9PasswordLogNetScopeSpec,
	"WoT.Parity.Fix9.PasswordLogNetScope", Fix9_Flags)

bool FFix9PasswordLogNetScopeSpec::RunTest(const FString&)
{
#if NO_LOGGING
	AddInfo(TEXT("Skipped: this target compiles logging out."));
	return true;
#else
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix9PasswordLog-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = nullptr;
	IOnlineSessionPtr Sessions;
	const ELogVerbosity::Type PriorVerbosity = LogNet.GetVerbosity();
	LogNet.SetVerbosity(ELogVerbosity::Display);
	ON_SCOPE_EXIT
	{
		LogNet.SetVerbosity(PriorVerbosity);
		if (Sessions.IsValid() && Sessions->GetNamedSession(NAME_GameSession))
		{
			Sessions->RemoveNamedSession(NAME_GameSession);
		}
		if (World)
		{
			WoTTest::DestroySpecWorld(World);
		}
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("authenticated world authority publishes"),
		Fix9_PublishWorldAuthority()))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTGameMode::StaticClass();
	World = WoTTest::CreateSpecWorld(Options);
	UWoTSessionSubsystem* Subsystem = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	const IOnlineSubsystem* OSS = World
		? Online::GetSubsystem(World, NULL_SUBSYSTEM) : nullptr;
	if (!TestNotNull(TEXT("session subsystem"), Subsystem)
		|| !TestNotNull(TEXT("LAN provider"), OSS))
	{
		return false;
	}
	Sessions = OSS->GetSessionInterface();
	Subsystem->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT { Subsystem->SetServerTravelHookForTesting({}); };
	const FString Secret(TEXT("Fix9Credential-83"));
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Request.ServerName = TEXT("Fix9 password log scope");
	Request.bLAN = true;
	Request.GamePassword = Secret;
	Fix9_LogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Capture); };
	TestTrue(TEXT("passworded host request is accepted"), Subsystem->Host(Request));
	UE_LOG(LogNet, Display, TEXT("Fix9 informational network diagnostic"));
	GLog->FlushThreadedLogs();
	TestTrue(TEXT("unrelated informational LogNet remains visible"),
		Capture.Text.Contains(TEXT("Fix9 informational network diagnostic")));
	TestFalse(TEXT("the host credential is absent from the real log sink"),
		Capture.Text.Contains(Secret));
	return !HasAnyErrors();
#endif
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix17PasswordNetLoginLogSpec,
	"WoT.Parity.Fix17.PasswordNetLoginLog", Fix9_Flags)

bool FFix17PasswordNetLoginLogSpec::RunTest(const FString&)
{
#if NO_LOGGING
	AddInfo(TEXT("Skipped: this target compiles logging out."));
	return true;
#else
	using namespace UE::Net;
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix17PasswordNet-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};

	const FString PlaintextPassword(TEXT("Fix17Secret-Right"));
	const FString CorrectToken(TEXT(
		"d379e49860f6f6f35e652268c79b062098ba6e50919330802eb92bdbcc03ec49"));
	const FString WrongToken(TEXT(
		"cf4bf1924c3a92dab608bc7387ab7b32dcc4dcabdc3ec0d7dc1505f742e33806"));
	FTestWorlds Worlds;
	Worlds.Server = Fix9_CreateListenServer(
		TEXT("?GamePassword=Fix17Secret-Right"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix17 password-token login and host LogNet"));
	if (!TestTrue(TEXT("passworded listen server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector::ZeroVector, FRotator::ZeroRotator);

	Fix9_LogCapture Capture;
	const ELogVerbosity::Type PriorVerbosity = LogNet.GetVerbosity();
	LogNet.SetVerbosity(ELogVerbosity::Log);
	GLog->AddOutputDevice(&Capture);
	AddExpectedError(
		TEXT("UEngine::BroadcastNetworkFailure: FailureType = PendingConnectionFailure"),
		EAutomationExpectedErrorFlags::Contains, 2);
	ON_SCOPE_EXIT
	{
		GLog->RemoveOutputDevice(&Capture);
		LogNet.SetVerbosity(PriorVerbosity);
	};

	Worlds.Clients.Emplace(Fix9_CreateBattleClient(Worlds.Server.GetPort(),
		TEXT("?PasswordToken=") + CorrectToken));
	TestTrue(TEXT("the correct password token is admitted by the real net login"),
		Worlds.WaitForClientConnect(Worlds.Clients.Last()));

	int32 FailureStart = Capture.Text.Len();
	Worlds.Clients.Emplace(Fix9_CreateBattleClient(Worlds.Server.GetPort(),
		TEXT("?PasswordToken=") + WrongToken));
	TestTrue(TEXT("the wrong password token returns WRONGPW on the real net login"),
		Worlds.TickUntil([&Capture, FailureStart]()
		{
			return Capture.Text.Mid(FailureStart).Contains(
				TEXT("PreLogin failure: WRONGPW"));
		}, 180));

	FailureStart = Capture.Text.Len();
	Worlds.Clients.Emplace(Fix9_CreateBattleClient(Worlds.Server.GetPort()));
	TestTrue(TEXT("an omitted password token returns NEEDPW on the real net login"),
		Worlds.TickUntil([&Capture, FailureStart]()
		{
			return Capture.Text.Mid(FailureStart).Contains(
				TEXT("PreLogin failure: NEEDPW"));
		}, 180));
	GLog->FlushThreadedLogs();

	TArray<FString> Lines;
	Capture.Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
	FString RequestLines;
	for (const FString& Line : Lines)
	{
		if (Line.Contains(TEXT("LogNet: Login request:"))
			|| Line.Contains(TEXT("LogNet: Join request:")))
		{
			RequestLines += Line + TEXT("\n");
		}
	}
	TestTrue(TEXT("the host captures a real engine Login request line"),
		RequestLines.Contains(TEXT("LogNet: Login request:")));
	TestTrue(TEXT("the host captures a real engine Join request line"),
		RequestLines.Contains(TEXT("LogNet: Join request:")));
	TestFalse(TEXT("host engine login logs contain no plaintext password"),
		RequestLines.Contains(PlaintextPassword, ESearchCase::CaseSensitive));
	TestFalse(TEXT("host engine login logs contain no legacy password option"),
		RequestLines.Contains(TEXT("?Password="), ESearchCase::IgnoreCase));
	return !HasAnyErrors();
#endif
}
#endif // WITH_EDITOR

#endif
