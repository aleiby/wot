#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelGameState.h"
#include "../WoTFactionCharacters.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTGameEngine.h"
#include "../WoTGameMode.h"
#include "../WoTGameSession.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTRetailText.h"
#include "../WoTSessionSubsystem.h"
#include "../Payload/WoTPayloadActivation.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/ChildConnection.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/ScopeExit.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Online/OnlineSessionNames.h"
#include "Net/OnlineEngineInterface.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"
#include "IpNetDriver.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Styling/CoreStyle.h"

#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags Fix8_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

AWoTNPCCharacter* Fix8_SpawnCreature(UWorld& World, const FVector& Location)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(World,
		TEXT("Trolloc"), Location, 1, true, AWoTTestNPCCharacter::StaticClass());
	if (NPC)
	{
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return NPC;
}

void Fix8_SendPausedKey(AWoTPlayerController& Controller, const FKey& Key)
{
	Controller.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		Key, IE_Pressed, 1.0f));
	TArray<UInputComponent*> Stack;
	Stack.Push(Controller.InputComponent);
	Controller.PlayerInput->ProcessInputStack(Stack, 1.0f / 60.0f,
		/*bGamePaused*/ true);
	Controller.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		Key, IE_Released, 0.0f));
	Controller.PlayerInput->ProcessInputStack(Stack, 1.0f / 60.0f,
		/*bGamePaused*/ true);
}

int32 Fix8_CountWorldAngreal(UWorld& World)
{
	int32 Count = 0;
	for (TActorIterator<AWoTAngrealInventory> It(&World); It; ++It)
	{
		if (IsValid(*It) && !It->GetOwner()) ++Count;
	}
	return Count;
}

bool Fix8_PublishWorldAuthority()
{
	ResetWoTPayloadActivationForTesting();
	return !PublishWoTPayloadBootstrapActivationForTesting().IsError()
		&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
}

class FFix8_Feedback final : public IWoTFrontEndFeedbackSink
{
public:
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

FUniqueNetIdRepl Fix8_UniqueId(const TCHAR* StableId)
{
	const FUniqueNetIdRef UniqueId = FUniqueNetIdString::Create(StableId,
		UOnlineEngineInterface::Get()->GetDefaultOnlineSubsystemName());
	return FUniqueNetIdRepl(UniqueId);
}

AWoTPlayerController* Fix8_SpawnRemotePlayer(UWorld& World,
	const TCHAR* Name, const FUniqueNetIdRepl& UniqueId)
{
	AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>();
	AWoTPlayerState* State = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Controller || !State) return nullptr;
	State->SetPlayerName(Name);
	State->SetUniqueId(UniqueId);
	UChildConnection* Connection = NewObject<UChildConnection>(GEngine);
	Connection->PlayerController = Controller;
	Controller->Player = Connection;
	return Controller;
}

class FFix8_TravelLogCapture final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type,
		const FName& Category) override
	{
		Text += Category.ToString() + TEXT(": ") + Message + TEXT("\n");
	}
	FString Text;
};

struct FFix8_GameEngineScope
{
	FFix8_GameEngineScope()
		: PriorEngine(GEngine), PriorWorld(GWorld),
		  bPriorPIE(GIsPlayInEditorWorld)
	{
		Engine = NewObject<UWoTGameEngine>(GetTransientPackage());
		Engine->AddToRoot();
		Engine->WorldSettingsClass = PriorEngine->WorldSettingsClass;
		Engine->DefaultPhysMaterial = PriorEngine->DefaultPhysMaterial;
		Engine->DefaultDestructiblePhysMaterial =
			PriorEngine->DefaultDestructiblePhysMaterial;
		for (const FWorldContext& PriorContext : PriorEngine->GetWorldContexts())
		{
			UWorld* World = PriorContext.World();
			if (!World) continue;
			FWorldContext& Borrowed =
				Engine->CreateNewWorldContext(PriorContext.WorldType);
			Borrowed.PIEInstance = PriorContext.PIEInstance;
			Borrowed.SetCurrentWorld(World);
			BorrowedWorlds.Add(World);
		}
		GEngine = Engine;
	}
	~FFix8_GameEngineScope()
	{
		TArray<FName> Handles;
		for (const FWorldContext& Context : Engine->GetWorldContexts())
			Handles.Add(Context.ContextHandle);
		for (const FName Handle : Handles)
		{
			FWorldContext* Context = Engine->GetWorldContextFromHandle(Handle);
			if (!Context) continue;
			UWorld* World = Context->World();
			if (World && BorrowedWorlds.Contains(World))
			{
				Engine->DestroyWorldContext(World);
				continue;
			}
			if (World && World->HasBegunPlay())
			{
				World->BeginTearingDown();
				World->EndPlay(EEndPlayReason::Quit);
			}
			if (Context->OwningGameInstance) Context->OwningGameInstance->Shutdown();
			if (World && !World->HasAnyFlags(
				RF_BeginDestroyed | RF_FinishDestroyed))
			{
				Engine->ShutdownWorldNetDriver(World);
				Engine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}
		GEngine = PriorEngine;
		GWorld = PriorWorld;
		GIsPlayInEditorWorld = bPriorPIE;
	}
	UWoTGameEngine* Engine = nullptr;
	UEngine* PriorEngine = nullptr;
	UWorld* PriorWorld = nullptr;
	bool bPriorPIE = false;
	TSet<UWorld*> BorrowedWorlds;
};

int32 Fix8_UnusedPIEInstance()
{
	static int32 LastInstance = INDEX_NONE;
	int32 Maximum = LastInstance;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		Maximum = FMath::Max(Maximum, Context.PIEInstance);
	LastInstance = Maximum + 1;
	return LastInstance;
}
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8AdminPausedInputSpec,
	"WoT.Parity.Fix8.AdminPausedInput", Fix8_Flags)

bool FFix8AdminPausedInputSpec::RunTest(const FString&)
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
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerState* State = Admin
		? Admin->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("listen rules"), Game)
		|| !TestNotNull(TEXT("admin controller"), Admin)
		|| !TestNotNull(TEXT("admin state"), State))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	State->bAdmin = true;
	Admin->SetAsLocalPlayerController();
	Admin->PlayerInput = NewObject<UPlayerInput>(Admin);
	Admin->SetupInputComponent();

	TestTrue(TEXT("admin establishes the listen pause"),
		Game->TrySetPlayerPause(Admin, NM_ListenServer));
	TestTrue(TEXT("listen world is paused"), World->IsPaused());
	Fix8_SendPausedKey(*Admin, EKeys::Pause);
	TestFalse(TEXT("the real bound Pause key resumes while paused"),
		World->IsPaused());
	if (World->IsPaused())
	{
		Game->TrySetPlayerPause(Admin, NM_ListenServer);
	}

	TestTrue(TEXT("admin establishes a second listen pause"),
		Game->TrySetPlayerPause(Admin, NM_ListenServer));
	Fix8_SendPausedKey(*Admin, EKeys::SpaceBar);
	TestFalse(TEXT("the pauser's real bound Jump key resumes while paused"),
		World->IsPaused());

	const FName PausedActions[] = {
		TEXT("Pause"), TEXT("InGameMenu"), TEXT("Type"), TEXT("Talk"),
		TEXT("TeamTalk"), TEXT("Jump")};
	for (const FName Action : PausedActions)
	{
		bool bFoundPausedBinding = false;
		for (int32 Index = 0; Index < Admin->InputComponent->GetNumActionBindings();
			++Index)
		{
			const FInputActionBinding& Binding =
				Admin->InputComponent->GetActionBinding(Index);
			if (Binding.GetActionName() == Action && Binding.KeyEvent == IE_Pressed)
			{
				bFoundPausedBinding = Binding.bExecuteWhenPaused;
				break;
			}
		}
		TestTrue(FString::Printf(TEXT("%s executes while paused"),
			*Action.ToString()), bFoundPausedBinding);
	}

	TestTrue(TEXT("admin establishes a disconnect pause"),
		Game->TrySetPlayerPause(Admin, NM_ListenServer));
	Admin->Destroy();
	TestFalse(TEXT("destroying the retail pauser releases the match"),
		World->IsPaused());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8PasswordCancelBrowserSpec,
	"WoT.Parity.Fix8.PasswordCancelBrowser", Fix8_Flags)

bool FFix8PasswordCancelBrowserSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix8PasswordCancel-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = nullptr;
	IOnlineSessionPtr Lan;
	IOnlineSessionPtr Remote;
	ON_SCOPE_EXIT
	{
		for (const IOnlineSessionPtr& Provider : {Lan, Remote})
		{
			if (Provider.IsValid()
				&& Provider->GetNamedSession(NAME_GameSession))
			{
				Provider->RemoveNamedSession(NAME_GameSession);
			}
		}
		if (World) WoTTest::DestroySpecWorld(World);
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("authenticated world authority publishes"),
		Fix8_PublishWorldAuthority()))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	World = WoTTest::CreateSpecWorld(Options);
	UWoTSessionSubsystem* Sessions = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	AWoTFrontEndPlayerController* Controller = World
		? World->SpawnActor<AWoTFrontEndPlayerController>() : nullptr;
	const IOnlineSubsystem* LanOSS = World
		? Online::GetSubsystem(World, NULL_SUBSYSTEM) : nullptr;
	IOnlineSubsystem* RemoteOSS =
		IOnlineSubsystem::Get(FName(TEXT("NULL:Fix8PasswordCancelRemote")));
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("front-end controller"), Controller)
		|| !TestNotNull(TEXT("LAN provider"), LanOSS)
		|| !TestNotNull(TEXT("remote provider"), RemoteOSS))
	{
		return false;
	}
	Lan = LanOSS->GetSessionInterface();
	Remote = RemoteOSS->GetSessionInterface();
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT
	{
		Sessions->SetClientTravelControllerForTesting(nullptr);
		Sessions->SetServerTravelHookForTesting({});
	};

	FWoTHostRequest Stale = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Stale.ServerName = TEXT("Fix8 stale joined row");
	Stale.bLAN = true;
	TestTrue(TEXT("stale named session is created"), Lan->CreateSession(
		0, NAME_GameSession, Sessions->BuildSessionSettingsForTesting(Stale)));
	Sessions->SetPasswordChallengeForTesting(
		EWoTPasswordChallenge::NeedPassword, TEXT("127.0.0.1:7777"));

	TSharedRef<FFix8_Feedback> Feedback = MakeShared<FFix8_Feedback>();
	TMap<FName, FString> Localization;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("fix8-password-cancel"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Localization), Feedback);
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller)
		.PresentationAuthorityForTesting(Authority);
	TestEqual(TEXT("a password prompt keeps the browser Play screen visible"),
		Root->CurrentScreenForTesting(), EWoTFrontEndScreen::Play);
	Root->CancelServerPasswordForTesting();
	TestEqual(TEXT("Cancel stays on the browser"),
		Root->CurrentScreenForTesting(), EWoTFrontEndScreen::Play);
	TestEqual(TEXT("Cancel clears the prompt"),
		static_cast<int32>(Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::None));
	TestFalse(TEXT("Cancel destroys the stale named session"),
		Sessions->HasSession());
	TestTrue(TEXT("Cancel issues no return-to-menu travel"),
		Sessions->GetLastReturnTravelURL().IsEmpty());
	if (!Sessions->GetLastReturnTravelURL().IsEmpty())
	{
		return false;
	}

	FWoTHostRequest Fresh = Stale;
	Fresh.ServerName = TEXT("Fix8 host after cancel");
	TestTrue(TEXT("the player can host after Cancel"), Sessions->Host(Fresh));
	TestTrue(TEXT("the fresh host owns a named session"), Sessions->HasSession());
	Lan->RemoveNamedSession(NAME_GameSession);

	Fresh.ServerName = TEXT("Fix8 join after cancel");
	TestTrue(TEXT("remote fixture session is created"), Remote->CreateSession(
		0, NAME_GameSession, Sessions->BuildSessionSettingsForTesting(Fresh)));
	const FNamedOnlineSession* Hosted =
		Remote->GetNamedSession(NAME_GameSession);
	if (TestNotNull(TEXT("remote fixture row"), Hosted))
	{
		FOnlineSessionSearchResult Row;
		Row.Session = FOnlineSession(*Hosted);
		const FWoTSessionListEntry Entry = Sessions->InjectSearchResultForTesting(
			Lan, false, Row);
		TestTrue(TEXT("the player can join after Cancel"), Sessions->Join(Entry));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8HostPasswordTravelSpec,
	"WoT.Parity.Fix8.HostPasswordTravel", Fix8_Flags)

bool FFix8HostPasswordTravelSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix8HostPassword-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = nullptr;
	IOnlineSessionPtr Lan;
	ON_SCOPE_EXIT
	{
		if (Lan.IsValid() && Lan->GetNamedSession(NAME_GameSession))
			Lan->RemoveNamedSession(NAME_GameSession);
		if (World) WoTTest::DestroySpecWorld(World);
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("authenticated world authority publishes"),
		Fix8_PublishWorldAuthority())) return false;
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTGameMode::StaticClass();
	World = WoTTest::CreateSpecWorld(Options);
	UWoTSessionSubsystem* Sessions = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	const IOnlineSubsystem* LanOSS = World
		? Online::GetSubsystem(World, NULL_SUBSYSTEM) : nullptr;
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("LAN provider"), LanOSS)) return false;
	Lan = LanOSS->GetSessionInterface();
	FString ServerTravelURL;
	Sessions->SetServerTravelHookForTesting(
		[&ServerTravelURL](UWorld*, const FString& URL, bool)
		{
			ServerTravelURL = URL;
			return true;
		});
	ON_SCOPE_EXIT { Sessions->SetServerTravelHookForTesting({}); };
	Sessions->SetPasswordChallengeForTesting(
		EWoTPasswordChallenge::NeedPassword, TEXT("127.0.0.1:7777"));

	const FString AdminSecret(TEXT("Fix8AdminSecret-31"));
	const FString GameSecret(TEXT("Fix8GameSecret-47"));
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Request.ServerName = TEXT("Fix8 password host");
	Request.bLAN = true;
	Request.AdminPassword = AdminSecret;
	Request.GamePassword = GameSecret;
	TestTrue(TEXT("password host is accepted"), Sessions->Host(Request));
	TestEqual(TEXT("accepted Host clears an obsolete join prompt"),
		static_cast<int32>(Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::None));
	TestFalse(TEXT("server travel URL omits the admin password"),
		ServerTravelURL.Contains(AdminSecret));
	TestFalse(TEXT("server travel URL omits the game password"),
		ServerTravelURL.Contains(GameSecret));
	FString StoredAdminPassword;
	FString StoredGamePassword;
	Sessions->GetActiveHostPasswords(
		StoredAdminPassword, StoredGamePassword);
	TestEqual(TEXT("the listen host keeps the admin secret out of its URL"),
		StoredAdminPassword, AdminSecret);
	TestEqual(TEXT("the listen host keeps the game secret out of its URL"),
		StoredGamePassword, GameSecret);
	AWoTGameMode* Destination = World->GetAuthGameMode<AWoTGameMode>();
	if (TestNotNull(TEXT("destination rules"), Destination))
	{
		FString InitError;
		Destination->InitGame(TEXT("Arena_01"), ServerTravelURL, InitError);
		TestTrue(TEXT("the destination rules recover the active admin secret"),
			Destination->IsAdminPassword(TEXT(
				"c0bf76ccdd0f06a20517d64bae62e66c7028587b83fe9ef5a55846b0c9f75efb")));
		TestTrue(TEXT("the destination rules recover the active game secret"),
			Destination->IsCapacityOverridePassword(TEXT(
				"ae5f2412efe1db48df6a6805aede30ee9eac571f0d0bbc79327c6effc161f1b6")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8PasswordWarningsVisibleSpec,
	"WoT.Parity.Fix8.PasswordWarningsVisible", Fix8_Flags)

bool FFix8PasswordWarningsVisibleSpec::RunTest(const FString&)
{
	ISocketSubsystem* SocketSubsystem =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!TestNotNull(TEXT("platform socket subsystem"), SocketSubsystem))
	{
		return false;
	}
	FSocket* PortBlocker = SocketSubsystem->CreateSocket(
		NAME_DGram, TEXT("Fix8PasswordWarningPortBlocker"), true);
	if (!TestNotNull(TEXT("listen port blocker"), PortBlocker))
	{
		return false;
	}
	ON_SCOPE_EXIT { SocketSubsystem->DestroySocket(PortBlocker); };
	PortBlocker->SetReuseAddr(false);
	TSharedRef<FInternetAddr> BlockedAddress =
		SocketSubsystem->CreateInternetAddr();
	BlockedAddress->SetAnyAddress();
	BlockedAddress->SetPort(0);
	if (!TestTrue(TEXT("reserve a real listen port"),
		PortBlocker->Bind(*BlockedAddress)))
	{
		return false;
	}
	PortBlocker->GetAddress(*BlockedAddress);
	if (!TestTrue(TEXT("read the reserved listen port"),
		BlockedAddress->GetPort() > 0))
	{
		return false;
	}

	UIpNetDriver* IpDriverDefaults = GetMutableDefault<UIpNetDriver>();
	const uint32 PriorMaxPortCount = IpDriverDefaults->MaxPortCountToTry;
	IpDriverDefaults->MaxPortCountToTry = 0;
	ON_SCOPE_EXIT { IpDriverDefaults->MaxPortCountToTry = PriorMaxPortCount; };

	FFix8_GameEngineScope Scope;
	for (FNetDriverDefinition& Definition : Scope.Engine->NetDriverDefinitions)
	{
		if (Definition.DefName == NAME_GameNetDriver)
		{
			Definition.DriverClassName =
				FName(TEXT("/Script/OnlineSubsystemUtils.IpNetDriver"));
			Definition.DriverClassNameFallback = Definition.DriverClassName;
		}
	}
	UGameInstance* Instance = NewObject<UGameInstance>(Scope.Engine);
	Instance->InitializeStandalone(FName(TEXT("Fix8PasswordWarnings")));
	FWorldContext& Context =
		Scope.Engine->GetWorldContextFromWorldChecked(Instance->GetWorld());
	Context.PIEInstance = Fix8_UnusedPIEInstance();
	const FString PasswordToken(TEXT(
		"a9f302997ad2889985ca4513f387e5b6987f6189b39ef5731a63e32bf5613cca"));
	const FURL URL(&Context.LastURL,
		*FString::Printf(
			TEXT("/Game/WoT/Maps/WoTFrontEnd?listen?PasswordToken=%s"),
			*PasswordToken), TRAVEL_Absolute);
	FURL ListenURL = URL;
	ListenURL.Port = BlockedAddress->GetPort();
	FFix8_TravelLogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	AddExpectedError(TEXT("InitBindSockets failed"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("BroadcastNetworkFailure: FailureType = NetDriverListenFailure"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("LoadMap: failed to Listen"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FString Error;
	Scope.Engine->Browse(Context, ListenURL, Error);
	GLog->RemoveOutputDevice(&Capture);
	TestTrue(TEXT("passworded Browse preserves the real failed-listen error"),
		Capture.Text.Contains(TEXT("LogNet: LoadMap: failed to Listen")));
	// The engine's retained warning may include the derived token. Fix17 keeps
	// plaintext out of the URL before this path; the token-only Browse redaction
	// contract is covered independently by Fix7.EngineTravelPasswordLogs.
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8BanPersistenceRuntimeGateSpec,
	"WoT.Parity.Fix8.BanPersistenceRuntimeGate", Fix8_Flags)

bool FFix8BanPersistenceRuntimeGateSpec::RunTest(const FString&)
{
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	TArray<TPair<FString, bool>> Accesses;
	UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting(
		[&Accesses](const FString& Path, bool bWrite) -> TOptional<bool>
		{
			Accesses.Emplace(Path, bWrite);
			return false;
		});
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting({});
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	};
	{
		TGuardValue<bool> RuntimeMode(GIsAutomationTesting, false);
		UWorld* RuntimeWorld =
			WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
		WoTTest::DestroySpecWorld(RuntimeWorld);
	}
	TestTrue(TEXT("a Development runtime reaches bounded host-ban I/O"),
		!Accesses.IsEmpty());

	Accesses.Reset();
	UWorld* IsolatedWorld =
		WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	WoTTest::DestroySpecWorld(IsolatedWorld);
	TestTrue(TEXT("running automation without a test path touches no real profile"),
		Accesses.IsEmpty());

	UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting({});
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix8BanPersistence-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
	const FUniqueNetIdRepl BannedId = Fix8_UniqueId(TEXT("fix8-ban-persist"));
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* Victim = World
		? Fix8_SpawnRemotePlayer(*World, TEXT("Fix8BanVictim"), BannedId)
		: nullptr;
	if (!TestNotNull(TEXT("ban rules"), Game)
		|| !TestNotNull(TEXT("ban admin"), Admin)
		|| !TestNotNull(TEXT("ban victim"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Admin->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	Game->SetPlayerRemovalHookForTesting(
		[](APlayerController* Player, const FText&)
		{
			Player->Destroy();
			return true;
		});
	TestTrue(TEXT("KICKBAN persists through the isolated test-path seam"),
		Game->ExecuteAdminCommand(Admin, TEXT("KICKBAN Fix8BanVictim")));
	WoTTest::DestroySpecWorld(World);
	World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (TestNotNull(TEXT("replacement ban rules"), Game))
	{
		TestEqual(TEXT("the isolated persisted ban reloads"),
			Game->HostBanCountForTesting(), 1);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8BattlePlayerConfigSpec,
	"WoT.Parity.Fix8.BattlePlayerConfig", Fix8_Flags)

bool FFix8BattlePlayerConfigSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTBattleGameMode::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	AWoTBattleGameMode* Mode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	UWoTGameInstance* Instance = World
		? Cast<UWoTGameInstance>(World->GetGameInstance()) : nullptr;
	AWoTBattleInfo* Battle = World
		? World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity) : nullptr;
	if (Battle)
	{
		Battle->NumTeams = 2;
		Battle->TeamClass[0] = AWoTHound::StaticClass();
		Battle->TeamClass[1] = AWoTForsaken::StaticClass();
		Battle->FinishSpawning(FTransform::Identity);
	}
	AWoTPlayerController* Controller = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTAesSedai* Pawn = World ? World->SpawnActor<AWoTAesSedai>() : nullptr;
	if (!TestNotNull(TEXT("Battle rules"), Mode)
		|| !TestNotNull(TEXT("game instance"), Instance)
		|| !TestNotNull(TEXT("BattleInfo"), Battle)
		|| !TestNotNull(TEXT("controller"), Controller)
		|| !TestNotNull(TEXT("Aes Sedai pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mode->Battle = Battle;
	Instance->SetSelectedFaction(EWoTPlayerFaction::AesSedai, false);
	Instance->SetSelectedSkinColor(EWoTPlayerSkinColor::Blue, false);
	Instance->SetCitadelSkinColor(EWoTPlayerSkinColor::Green, false);
	if (FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
	{
		Context->LastURL.AddOption(TEXT("Skin=AesSedaiSkins.Aes_Blue"));
	}
	Controller->Possess(Pawn);
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	State->Team = 255;
	TestTrue(TEXT("real Battle team join succeeds"),
		Mode->ChangeTeam(Controller, 0));
	TestEqual(TEXT("team join applies the saved Hound citadel colour"),
		Controller->PersonalSkinURL, FString(TEXT("HoundSkins.Hou_Green")));
	Controller->UnPossess();

	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(Controller);
	Overlay->SetPendingPlayerConfigForTesting(
		TEXT("Perrin"), EWoTPlayerSkinColor::Red);
	Overlay->SavePlayerConfigForTesting();
	TestEqual(TEXT("pawnless Battle save uses controller PlayerState team faction"),
		Controller->PersonalSkinURL, FString(TEXT("HoundSkins.Hou_Red")));
	TestEqual(TEXT("Battle save preserves the Arena faction"),
		static_cast<int32>(Instance->GetSelectedFaction()),
		static_cast<int32>(EWoTPlayerFaction::AesSedai));
	TestEqual(TEXT("Battle save preserves the Arena colour"),
		static_cast<int32>(Instance->GetSelectedSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Blue));
	TestEqual(TEXT("Battle save persists the separate citadel colour"),
		static_cast<int32>(Instance->GetCitadelSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Red));
	if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
	{
		TestEqual(TEXT("Battle save preserves the Arena LastURL skin"),
			FString(Context->LastURL.GetOption(TEXT("Skin="), TEXT(""))),
			FString(TEXT("AesSedaiSkins.Aes_Blue")));
	}
	TestEqual(TEXT("the name still updates in Battle"),
		State->GetPlayerName(), FString(TEXT("Perrin")));

	FWoTHostRequest Arena = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	const FString NextArena = FWoTFrontEndModel::BuildHostTravelURL(Arena,
		Instance->GetSelectedFaction(), Instance->GetSelectedSkinColor());
	TestTrue(TEXT("the next Arena URL still carries Aes Sedai"),
		NextArena.Contains(TEXT("Class=WOTPawns.AesSedai")));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8NPCReturnClassificationSpec,
	"WoT.Parity.Fix8.NPCReturnClassification", Fix8_Flags)

bool FFix8NPCReturnClassificationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = World
		? Fix8_SpawnCreature(*World, FVector(0, 0, 100)) : nullptr;
	AWoTNPCController* Controller = NPC
		? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = NPC
		? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
	if (TestNotNull(TEXT("guard"), NPC)
		&& TestNotNull(TEXT("controller"), Controller)
		&& TestNotNull(TEXT("brain"), Brain))
	{
		const FVector OriginalPost(1000, 0, 100);
		Brain->bForceReachable = false;
		Brain->bForcePathable = true;
		Brain->SetWaitingGoal(OriginalPost, FRotator::ZeroRotator);
		Controller->TransitionOnGoalPriority(true);
		Controller->Tick(0.05f);
		TestEqual(TEXT("a classified path request failure remains Returning"),
			Controller->GetBrainState(), EWoTBrainState::Returning);
		TestTrue(TEXT("a temporary failure preserves the original post"),
			Brain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
				OriginalPost, 0.01f));
		TestFalse(TEXT("a failed request clears locomotion intent"),
			Controller->HasActiveMovementIntentForAnimation());

		Brain->bForceReachable = true;
		Brain->bForcePathable = true;
		Brain->GetGoal(EWoTGoalIndex::Waiting).Cache.Invalidate();
		Controller->Tick(0.05f);
		TestEqual(TEXT("the guard resumes its return when a route appears"),
			Controller->GetBrainState(), EWoTBrainState::Returning);
		TestTrue(TEXT("the recovered route creates movement intent"),
			Controller->HasActiveMovementIntentForAnimation());

		Brain->bForceReachable = false;
		Brain->bForcePathable = false;
		Brain->GetGoal(EWoTGoalIndex::Waiting).Cache.Invalidate();
		Controller->Tick(0.05f);
		TestEqual(TEXT("an UnNavigable post takes UnableToReturn continuation"),
			Controller->GetBrainState(), EWoTBrainState::Waiting);
		TestTrue(TEXT("only UnNavigable adopts the stranded position"),
			Brain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
				NPC->GetActorLocation(), 0.01f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8HelpDoesNotPauseSpec,
	"WoT.Parity.Fix8.HelpDoesNotPause", Fix8_Flags)

bool FFix8HelpDoesNotPauseSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		World->WorldType = EWorldType::PIE;
		World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerState* State = Admin
		? Admin->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (TestNotNull(TEXT("listen admin"), Admin)
		&& TestNotNull(TEXT("admin state"), State))
	{
		State->bAdmin = true;
		Admin->ShowHelp();
		TestTrue(TEXT("real ShowHelp opens the help model"), Admin->IsHelpOpen());
		TestFalse(TEXT("F1 help never pauses the listen match"), World->IsPaused());
		Admin->ShowHelp();
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8RemoteAdminSingleAngrealSpec,
	"WoT.Parity.Fix8.RemoteAdminSingleAngreal", Fix8_Flags)

bool FFix8RemoteAdminSingleAngrealSpec::RunTest(const FString&)
{
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix8 remote admin single angreal commands"));
	if (!TestTrue(TEXT("dedicated authority loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(0.0f, 0.0f, 5000.0f), FRotator::ZeroRotator);
	if (!TestTrue(TEXT("remote client connects"),
		Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerController = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerPawn = ServerController
		? Cast<AWoTCharacter>(ServerController->GetPawn()) : nullptr;
	AWoTPlayerController* ClientController = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* ClientPawn = ClientController
		? Cast<AWoTCharacter>(ClientController->GetPawn()) : nullptr;
	AWoTPlayerState* ServerState = ServerController
		? ServerController->GetPlayerState<AWoTPlayerState>() : nullptr;
	AWoTPlayerState* ClientState = ClientController
		? ClientController->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("authority pawn"), ServerPawn)
		|| !TestNotNull(TEXT("client pawn"), ClientPawn)
		|| !TestNotNull(TEXT("authority state"), ServerState)
		|| !TestNotNull(TEXT("client state"), ClientState))
	{
		return false;
	}
	ServerState->bAdmin = true;
	ServerState->ForceNetUpdate();
	TestTrue(TEXT("admin bit reaches the owner"),
		Worlds.TickUntil([ClientState]() { return ClientState->bAdmin; }, 120));

	const int32 BeforeInventory = ServerPawn->InventoryComponent->Items.Num();
	ClientPawn->GiveAngreal(TEXT("Fireball"));
	TestTrue(TEXT("remote GiveAngreal grants on authority"),
		Worlds.TickUntil([ServerPawn, BeforeInventory]()
		{
			return ServerPawn->InventoryComponent
				&& ServerPawn->InventoryComponent->Items.Num() > BeforeInventory;
		}, 120));

	const int32 BeforeWorld = Fix8_CountWorldAngreal(*Worlds.Server.GetWorld());
	ClientPawn->SpawnAngreal(TEXT("Fireball"));
	TestTrue(TEXT("remote SpawnAngreal spawns on authority"),
		Worlds.TickUntil([&Worlds, BeforeWorld]()
		{
			return Fix8_CountWorldAngreal(*Worlds.Server.GetWorld()) > BeforeWorld;
		}, 120));
	return !HasAnyErrors();
}
#endif

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix8ServerFullRetailTextSpec,
	"WoT.Parity.Fix8.ServerFullRetailText", Fix8_Flags)

bool FFix8ServerFullRetailTextSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay = [](UWorld* InWorld)
	{
		InWorld->WorldType = EWorldType::PIE;
		InWorld->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTGameSession* Session = Game
		? Cast<AWoTGameSession>(Game->GameSession) : nullptr;
	if (TestNotNull(TEXT("rules"), Game)
		&& TestNotNull(TEXT("game session"), Session))
	{
		Session->MaxPlayers = 1;
		World->SpawnActor<AWoTPlayerController>();
		const FString Error = Session->ApproveLogin(FString());
		FWoTRetailText::EnsureLoaded();
		const FString* Retail = FWoTRetailText::FindSectionValue(
			TEXT("GameInfo"), TEXT("MaxedOutMessage"));
		if (TestNotNull(TEXT("runtime GameInfo MaxedOutMessage"), Retail))
		{
			TestEqual(TEXT("engine-first capacity refusal uses runtime retail text"),
				Error, *Retail);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}
#endif

#endif // WITH_AUTOMATION_TESTS
