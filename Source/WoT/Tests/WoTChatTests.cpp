#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTInGameSlate.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTRetailText.h"
#include "WoTTestBattleClasses.h"

#include "Engine/World.h"
#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Tests/NetTestHelpers.h"
#endif
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "UObject/CoreNet.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr EAutomationTestFlags ChatFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

AWoTTestLeftMessageCaptureController* SpawnChatPlayer(
	UWorld* World, const TCHAR* Name, uint8 Team)
{
	AWoTTestLeftMessageCaptureController* Controller =
		World->SpawnActor<AWoTTestLeftMessageCaptureController>();
	if (!Controller)
	{
		return nullptr;
	}
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->SetPlayerName(Name);
		State->Team = Team;
	}
	return Controller;
}

AWoTTestSealNotifyController* P1C_SpawnVotePlayer(
	UWorld* World, const TCHAR* Name)
{
	AWoTTestSealNotifyController* Controller =
		World->SpawnActor<AWoTTestSealNotifyController>();
	if (!Controller) return nullptr;
	Controller->SetAsLocalPlayerController();
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->SetPlayerName(Name);
	}
	return Controller;
}

AWoTBattleInfo* InstallBattleInfo(UWorld* World)
{
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	if (!Battle)
	{
		return nullptr;
	}
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	if (AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>())
	{
		Mode->Battle = Battle;
	}
	return Battle;
}

#if WITH_EDITOR
UE::Net::FTestWorldInstance CreateChatListenServer(const TCHAR* URL)
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
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL Parsed(nullptr, URL, TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*GameInstance->GetWorldContext(), Parsed, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}
#endif

void DispatchChatInput(APlayerController* Controller, const FKey& Key,
	EInputEvent Event, float Amount)
{
	Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE,
		Key, Event, Amount, false, 0));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CDuplicatePlayerNameSpec,
	"WoT.Parity.P1C.DuplicatePlayerName", ChatFlags)

bool FWoTP1CDuplicatePlayerNameSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestMessageCaptureController* First =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	AWoTTestMessageCaptureController* Duplicate =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	if (!TestNotNull(TEXT("game mode"), Game)
		|| !TestNotNull(TEXT("first login"), First)
		|| !TestNotNull(TEXT("duplicate login"), Duplicate))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	First->InitPlayerState();
	Duplicate->InitPlayerState();
	const FString DuplicateDefaultName = Duplicate->PlayerState->GetPlayerName();
	const FUniqueNetIdRepl InvalidUniqueId;
	Game->InitNewPlayer(First, InvalidUniqueId, TEXT("?Name=Rand"));
	Game->InitNewPlayer(Duplicate, InvalidUniqueId, TEXT("?Name=rAnD"));

	TestEqual(TEXT("the first login keeps its requested name"),
		First->PlayerState->GetPlayerName(), FString(TEXT("Rand")));
	TestEqual(TEXT("a case-insensitive duplicate keeps its assigned fallback name"),
		Duplicate->PlayerState->GetPlayerName(), DuplicateDefaultName);
	const FString* NoNameChange = FWoTRetailText::EnsureLoaded()
		? FWoTRetailText::FindSectionValue(TEXT("giCombatBase"), TEXT("NoNameChange"))
		: nullptr;
	TestNotNull(TEXT("retail duplicate-name suffix"), NoNameChange);
	if (NoNameChange)
	{
		TestTrue(TEXT("the rejected player receives the localized reason"),
			Duplicate->CapturedMessages.Contains(FString(TEXT("rAnD")) + *NoNameChange));
	}

	First->CapturedMessages.Reset();
	Duplicate->CapturedMessages.Reset();
	Game->ChangeName(First, TEXT("Perrin"), true);
	const FString* GlobalNameChange = FWoTRetailText::FindSectionValue(
		TEXT("giCombatBase"), TEXT("GlobalNameChange"));
	TestNotNull(TEXT("retail global name-change fragment"), GlobalNameChange);
	if (GlobalNameChange)
	{
		const FString Expected = FString(TEXT("Rand")) + *GlobalNameChange
			+ TEXT("Perrin");
		TestTrue(TEXT("an in-game change is broadcast to the renaming player"),
			First->CapturedMessages.Contains(Expected));
		TestTrue(TEXT("an in-game change is broadcast to peers"),
			Duplicate->CapturedMessages.Contains(Expected));
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CVoteCommandMajoritySpec,
	"WoT.Parity.P1C.VoteCommandMajority", ChatFlags)

bool FWoTP1CVoteCommandMajoritySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	Game->bChangeLevels = true;
	Game->MapPrefix = TEXT("Arena_");
	Game->Map = TEXT("Arena_01");
	Game->SetInstalledMapsForTesting({TEXT("Arena_01"), TEXT("Arena_02")});
	AWoTTestSealNotifyController* Rand = P1C_SpawnVotePlayer(World, TEXT("Rand"));
	AWoTTestSealNotifyController* Mat = P1C_SpawnVotePlayer(World, TEXT("Mat"));
	AWoTTestSealNotifyController* Perrin = P1C_SpawnVotePlayer(World, TEXT("Perrin"));
	if (!TestNotNull(TEXT("first voter"), Rand)
		|| !TestNotNull(TEXT("second voter"), Mat)
		|| !TestNotNull(TEXT("third voter"), Perrin))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	auto SubmitVote = [](AWoTTestSealNotifyController* Controller,
		const TCHAR* Request)
	{
		Controller->Type();
		Controller->SetChatTextForTesting(FString(TEXT("Vote ")) + Request);
		Controller->SubmitChatForTesting();
	};

	SubmitVote(Rand, TEXT("restartserver"));
	TestTrue(TEXT("one of three votes does not execute"), Game->PendingMapURL.IsEmpty());
	SubmitVote(Rand, TEXT("RESTARTSERVER"));
	TestTrue(TEXT("one player cannot vote twice"), Game->PendingMapURL.IsEmpty());
	SubmitVote(Mat, TEXT("RESTARTSERVER"));
	TestEqual(TEXT("two of three votes execute the authorized restart"),
		Game->PendingMapURL, AWoTGameMode::TravelURLForMap(TEXT("Arena_02")));
	TestTrue(TEXT("the vote lifecycle broadcasts status to every player"),
		!Rand->CenterMessages.IsEmpty() && !Mat->CenterMessages.IsEmpty()
			&& !Perrin->CenterMessages.IsEmpty());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CVoteValidationExpiryActionsSpec,
	"WoT.Parity.P1C.VoteValidationExpiryActions", ChatFlags)

bool FWoTP1CVoteValidationExpiryActionsSpec::RunTest(const FString&)
{
	UFunction* ServerVoteFunction = AWoTPlayerController::StaticClass()
		->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(AWoTPlayerController, ServerVote));
	TestTrue(TEXT("Vote uses a reliable validated authority RPC"), ServerVoteFunction
		&& ServerVoteFunction->HasAllFunctionFlags(
			FUNC_Net | FUNC_NetServer | FUNC_NetReliable | FUNC_NetValidate));
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TArray<FString> Installed;
	for (int32 Index = 1; Index <= 13; ++Index)
	{
		Installed.Add(FString::Printf(TEXT("Arena_%02d"), Index));
	}
	Game->SetInstalledMapsForTesting(Installed);
	AWoTTestSealNotifyController* Rand = P1C_SpawnVotePlayer(World, TEXT("Rand"));
	AWoTTestSealNotifyController* Mat = P1C_SpawnVotePlayer(World, TEXT("Mat"));
	AWoTTestSealNotifyController* Perrin = P1C_SpawnVotePlayer(World, TEXT("Perrin"));
	if (!Rand || !Mat || !Perrin)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->SetPlayerRemovalHookForTesting(
		[](APlayerController*, const FText&) { return true; });

	Rand->Vote(TEXT("FOOKICK Mat"));
	Rand->Vote(TEXT("OPEN MissingMap"));
	TestEqual(TEXT("only exact authorized verbs and installed maps are accepted"),
		Game->ActiveVoteRequestCountForTesting(), 0);

	Rand->Vote(TEXT("OPEN Arena_02"));
	TestEqual(TEXT("a valid request occupies one vote slot"),
		Game->ActiveVoteRequestCountForTesting(), 1);
	TestTrue(TEXT("new requests start with a 90 second lifetime"),
		FMath::IsNearlyEqual(Game->VoteRequestRemainingForTesting(0), 90.0f));
	Game->Tick(89.0f);
	TestEqual(TEXT("the request remains active before 90 seconds"),
		Game->ActiveVoteRequestCountForTesting(), 1);
	Game->Tick(1.1f);
	TestEqual(TEXT("Tick resets an expired request"),
		Game->ActiveVoteRequestCountForTesting(), 0);
	TestTrue(TEXT("expiry is broadcast to every player"),
		!Rand->CenterMessages.IsEmpty() && !Mat->CenterMessages.IsEmpty()
			&& !Perrin->CenterMessages.IsEmpty());

	Rand->Vote(TEXT("OPEN Arena_02"));
	Mat->Vote(TEXT("OPEN Arena_02"));
	TestEqual(TEXT("an OPEN majority travels only to an installed map"),
		Game->PendingMapURL, AWoTGameMode::TravelURLForMap(TEXT("Arena_02")));
	Rand->Vote(TEXT("KICK Perrin"));
	Mat->Vote(TEXT("KICK Perrin"));
	TestEqual(TEXT("a KICK majority reaches the kick action"),
		Game->LastExecutedVoteRequestForTesting(), FString(TEXT("KICK PERRIN")));
	Rand->Vote(TEXT("KICKBAN Perrin"));
	Mat->Vote(TEXT("KICKBAN Perrin"));
	TestEqual(TEXT("a KICKBAN majority reaches the ban action"),
		Game->LastExecutedVoteRequestForTesting(), FString(TEXT("KICKBAN PERRIN")));

	for (int32 Index = 1; Index <= 12; ++Index)
	{
		Rand->Vote(FString::Printf(TEXT("OPEN Arena_%02d"), Index));
	}
	TestEqual(TEXT("the game mode holds twelve simultaneous requests"),
		Game->ActiveVoteRequestCountForTesting(), 12);
	Rand->Vote(TEXT("OPEN Arena_13"));
	TestEqual(TEXT("a thirteenth request is rejected until a slot resets"),
		Game->ActiveVoteRequestCountForTesting(), 12);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatListenRoutingSpec,
	"WoT.Chat.Listen.Routing", ChatFlags)

bool FWoTChatListenRoutingSpec::RunTest(const FString&)
{
	UFunction* SayFunction = AWoTPlayerController::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AWoTPlayerController, Say));
	UFunction* TeamSayFunction = AWoTPlayerController::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AWoTPlayerController, TeamSay));
	TestTrue(TEXT("Say is a reliable client-to-authority RPC"), SayFunction
		&& SayFunction->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable));
	TestTrue(TEXT("TeamSay is a reliable client-to-authority RPC"), TeamSayFunction
		&& TeamSayFunction->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable));

	UWorld* Arena = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestLeftMessageCaptureController* Rand = SpawnChatPlayer(Arena, TEXT("Rand"), 0);
	AWoTTestLeftMessageCaptureController* Mat = SpawnChatPlayer(Arena, TEXT("Mat"), 1);
	if (!TestNotNull(TEXT("listen sender"), Rand)
		|| !TestNotNull(TEXT("listen peer"), Mat))
	{
		WoTTest::DestroySpecWorld(Arena);
		return false;
	}
	Rand->Say(TEXT("The Wheel weaves"));
	TestEqual(TEXT("listen host sees all-chat through its left queue"),
		Rand->LeftMessages, TArray<FString>({TEXT("Rand: The Wheel weaves")}));
	TestEqual(TEXT("listen peer sees all-chat through its left queue"),
		Mat->LeftMessages, TArray<FString>({TEXT("Rand: The Wheel weaves")}));

	Rand->LeftMessages.Reset();
	Mat->LeftMessages.Reset();
	Rand->TeamSay(TEXT("arena fallback"));
	TestEqual(TEXT("Arena TeamSay falls back to Say for the sender"),
		Rand->LeftMessages, TArray<FString>({TEXT("Rand: arena fallback")}));
	TestEqual(TEXT("Arena TeamSay falls back to Say for the peer"),
		Mat->LeftMessages, TArray<FString>({TEXT("Rand: arena fallback")}));
	WoTTest::DestroySpecWorld(Arena);

	UWorld* BattleWorld = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	if (!TestNotNull(TEXT("BattleInfo"), InstallBattleInfo(BattleWorld)))
	{
		WoTTest::DestroySpecWorld(BattleWorld);
		return false;
	}
	Rand = SpawnChatPlayer(BattleWorld, TEXT("Rand"), 0);
	Mat = SpawnChatPlayer(BattleWorld, TEXT("Mat"), 0);
	AWoTTestLeftMessageCaptureController* Ishamael =
		SpawnChatPlayer(BattleWorld, TEXT("Ishamael"), 1);
	if (!Rand || !Mat || !Ishamael)
	{
		WoTTest::DestroySpecWorld(BattleWorld);
		return false;
	}
	Rand->TeamSay(TEXT("Hold the line"));
	const TArray<FString> TeamMessage({TEXT("[Alpha]Rand: Hold the line")});
	TestEqual(TEXT("Battle TeamSay reaches the sender"), Rand->LeftMessages, TeamMessage);
	TestEqual(TEXT("Battle TeamSay reaches a teammate"), Mat->LeftMessages, TeamMessage);
	TestTrue(TEXT("Battle TeamSay does not reach an opponent"),
		Ishamael->LeftMessages.IsEmpty());
	WoTTest::DestroySpecWorld(BattleWorld);
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatConnectedHUDSpec,
	"WoT.Chat.Listen.ConnectedHUDs", ChatFlags)

bool FWoTChatConnectedHUDSpec::RunTest(const FString&)
{
	using namespace UE::Net;
	auto ClientController = [](FTestWorlds& Worlds, int32 Index)
	{
		return Worlds.Clients.IsValidIndex(Index)
			? Cast<AWoTPlayerController>(
				Worlds.Clients[Index].GetWorld()->GetFirstPlayerController())
			: nullptr;
	};
	auto ClientHUD = [&ClientController](FTestWorlds& Worlds, int32 Index)
	{
		AWoTPlayerController* Controller = ClientController(Worlds, Index);
		return Controller ? Cast<AWoTHUD>(Controller->GetHUD()) : nullptr;
	};
	auto SetServerIdentity = [](FTestWorlds& Worlds, int32 Index,
		const TCHAR* Name, uint8 Team)
	{
		return Worlds.TickUntil([&Worlds, Index, Name, Team]()
		{
			AWoTPlayerController* Controller = Cast<AWoTPlayerController>(
				Worlds.GetServerPlayerControllerOfClient(Index));
			AWoTPlayerState* State = Controller
				? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
			if (!State) return false;
			State->SetPlayerName(Name);
			State->Team = Team;
			State->ForceNetUpdate();
			return true;
		}, 180);
	};

	{
		FTestWorlds Arena;
		Arena.Server = CreateChatListenServer(TEXT(
			"/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"));
		Arena.SetDescription(TEXT("WoT connected Arena chat"));
		if (!TestTrue(TEXT("Arena listen server loads"), Arena.Server.IsLoaded())
			|| !TestNotNull(TEXT("Arena spare start one"),
				Arena.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, -1000, 5000), FRotator::ZeroRotator))
			|| !TestNotNull(TEXT("Arena spare start two"),
				Arena.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, 1000, 5000), FRotator::ZeroRotator))
			|| !TestTrue(TEXT("first Arena client connects"),
				Arena.CreateAndConnectClient())
			|| !TestTrue(TEXT("second Arena client connects"),
				Arena.CreateAndConnectClient()))
		{
			return false;
		}
		TestEqual(TEXT("Arena fixture is a real listen server"),
			Arena.Server.GetWorld()->GetNetMode(), NM_ListenServer);
		AWoTPlayerController* Sender = ClientController(Arena, 0);
		AWoTHUD* SenderHUD = ClientHUD(Arena, 0);
		AWoTHUD* PeerHUD = ClientHUD(Arena, 1);
		if (!TestNotNull(TEXT("connected Arena sender"), Sender)
			|| !TestNotNull(TEXT("connected Arena sender HUD"), SenderHUD)
			|| !TestNotNull(TEXT("connected Arena peer HUD"), PeerHUD)
			|| !TestTrue(TEXT("Arena server identities are authoritative"),
				SetServerIdentity(Arena, 0, TEXT("Rand"), 0)
				&& SetServerIdentity(Arena, 1, TEXT("Mat"), 1)))
		{
			return false;
		}
		Sender->Say(TEXT("The Wheel weaves"));
		TestTrue(TEXT("Say crosses the net driver into both HUD queues"),
			Arena.TickUntil([SenderHUD, PeerHUD]()
			{
				return SenderHUD->LeftMessages[0].Message
						== TEXT("Rand: The Wheel weaves")
					&& PeerHUD->LeftMessages[0].Message
						== TEXT("Rand: The Wheel weaves");
			}, 180));
		Sender->TeamSay(TEXT("arena fallback"));
		TestTrue(TEXT("Arena TeamSay crosses the net driver as Say"),
			Arena.TickUntil([SenderHUD, PeerHUD]()
			{
				return SenderHUD->LeftMessages[0].Message
						== TEXT("Rand: arena fallback")
					&& PeerHUD->LeftMessages[0].Message
						== TEXT("Rand: arena fallback");
			}, 180));
	}

	{
		FTestWorlds Battle;
		Battle.Server = CreateChatListenServer(TEXT(
			"/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTBattleGameMode"));
		Battle.SetDescription(TEXT("WoT connected Battle chat"));
		if (!TestTrue(TEXT("Battle listen server loads"), Battle.Server.IsLoaded())
			|| !TestNotNull(TEXT("connected BattleInfo"),
				InstallBattleInfo(Battle.Server.GetWorld()))
			|| !TestNotNull(TEXT("Battle spare start one"),
				Battle.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, -2000, 5000), FRotator::ZeroRotator))
			|| !TestNotNull(TEXT("Battle spare start two"),
				Battle.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, 0, 5000), FRotator::ZeroRotator))
			|| !TestNotNull(TEXT("Battle spare start three"),
				Battle.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
					FVector(-10000, 2000, 5000), FRotator::ZeroRotator))
			|| !TestTrue(TEXT("Battle sender connects"),
				Battle.CreateAndConnectClient())
			|| !TestTrue(TEXT("Battle teammate connects"),
				Battle.CreateAndConnectClient())
			|| !TestTrue(TEXT("Battle opponent connects"),
				Battle.CreateAndConnectClient()))
		{
			return false;
		}
		AWoTPlayerController* Sender = ClientController(Battle, 0);
		AWoTHUD* SenderHUD = ClientHUD(Battle, 0);
		AWoTHUD* TeammateHUD = ClientHUD(Battle, 1);
		AWoTHUD* OpponentHUD = ClientHUD(Battle, 2);
		if (!TestNotNull(TEXT("connected Battle sender"), Sender)
			|| !TestNotNull(TEXT("connected Battle sender HUD"), SenderHUD)
			|| !TestNotNull(TEXT("connected Battle teammate HUD"), TeammateHUD)
			|| !TestNotNull(TEXT("connected Battle opponent HUD"), OpponentHUD)
			|| !TestTrue(TEXT("Battle server teams are authoritative"),
				SetServerIdentity(Battle, 0, TEXT("Rand"), 0)
				&& SetServerIdentity(Battle, 1, TEXT("Mat"), 0)
				&& SetServerIdentity(Battle, 2, TEXT("Ishamael"), 1)))
		{
			return false;
		}
		const FString OpponentBefore = OpponentHUD->LeftMessages[0].Message;
		Sender->TeamSay(TEXT("Hold the line"));
		TestTrue(TEXT("TeamSay crosses the net driver into teammate HUD queues"),
			Battle.TickUntil([SenderHUD, TeammateHUD]()
			{
				return SenderHUD->LeftMessages[0].Message
						== TEXT("[Alpha]Rand: Hold the line")
					&& TeammateHUD->LeftMessages[0].Message
						== TEXT("[Alpha]Rand: Hold the line");
			}, 180));
		TestEqual(TEXT("TeamSay never reaches an opponent HUD queue"),
			OpponentHUD->LeftMessages[0].Message, OpponentBefore);
	}
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatFloodAndLengthSpec,
	"WoT.Chat.Authority.FloodAndLength", ChatFlags)

bool FWoTChatFloodAndLengthSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Mode = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestLeftMessageCaptureController* Rand = SpawnChatPlayer(World, TEXT("Rand"), 0);
	if (!TestNotNull(TEXT("chat game mode"), Mode)
		|| !TestNotNull(TEXT("chat sender"), Rand))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("first 128-character broadcast is below the retail budget"),
		Mode->AllowsBroadcast(Rand, 128));
	TestTrue(TEXT("second 128-character broadcast remains below the retail budget"),
		Mode->AllowsBroadcast(Rand, 128));
	TestTrue(TEXT("259 aggregate characters remain allowed"),
		Mode->AllowsBroadcast(Rand, 3));
	TestFalse(TEXT("260 aggregate characters are suppressed"),
		Mode->AllowsBroadcast(Rand, 1));
	Mode->Timer();
	TestTrue(TEXT("the one-second GameInfo timer resets the shared budget"),
		Mode->AllowsBroadcast(Rand, 128));

	Mode->Timer();
	const FString Oversized = FString::ChrN(
		AWoTPlayerController::MaxChatMessageLength + 20, TEXT('x'));
	Rand->Say(Oversized);
	Rand->TeamSay(Oversized);
	TestEqual(TEXT("RPC validation rejects a payload over Console.TextMsgSize"),
		Rand->LeftMessages.Num(), 0);
	TestNotNull(TEXT("the rejection is reported as an RPC validation failure"),
		RPC_GetLastFailedReason());
	RPC_ResetLastFailedReason();
	const FString Longest = FString::ChrN(
		AWoTPlayerController::MaxChatMessageLength, TEXT('y'));
	Rand->Say(Longest);
	TestEqual(TEXT("a Console.TextMsgSize payload is delivered whole"),
		Rand->LeftMessages.Num() > 0 ? Rand->LeftMessages.Last() : FString(),
		FString(TEXT("Rand: ")) + Longest);

	Mode->Timer();
	Rand->Say(TEXT("hi\nMat: forged\r\tend"));
	TestEqual(TEXT("line breaks and control characters cannot forge a second row"),
		Rand->LeftMessages.Last(), FString(TEXT("Rand: hi Mat: forged  end")));
	TestEqual(TEXT("sanitizing keeps a clean message unchanged"),
		AWoTPlayerController::SanitizeChatMessage(TEXT("Light's mercy!")),
		FString(TEXT("Light's mercy!")));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatEmptyTeamSaySpec,
	"WoT.Chat.Authority.EmptyTeamSay", ChatFlags)

bool FWoTChatEmptyTeamSaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = InstallBattleInfo(World);
	AWoTTestLeftMessageCaptureController* Rand = SpawnChatPlayer(World, TEXT("Rand"), 0);
	AWoTTestLeftMessageCaptureController* Mat = SpawnChatPlayer(World, TEXT("Mat"), 0);
	if (!TestNotNull(TEXT("battle mode"), Mode) || !TestNotNull(TEXT("battle"), Battle)
		|| !TestNotNull(TEXT("sender"), Rand) || !TestNotNull(TEXT("teammate"), Mat))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (int32 Attempt = 0; Attempt < 300; ++Attempt)
	{
		Rand->TeamSay(FString());
	}
	TestEqual(TEXT("an empty Battle TeamSay reaches no teammate"),
		Mat->LeftMessages.Num(), 0);
	TestTrue(TEXT("empty TeamSay spam leaves the flood budget untouched"),
		Mode->AllowsBroadcast(Rand, 259));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatTypingSpec,
	"WoT.Chat.Input.Typing", ChatFlags)

bool FWoTChatTypingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestLeftMessageCaptureController* Rand = SpawnChatPlayer(World, TEXT("Rand"), 0);
	if (!TestNotNull(TEXT("typing controller"), Rand))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Rand->SetAsLocalPlayerController();
	TSharedRef<SWoTChatInputOverlay> RapidText =
		SNew(SWoTChatInputOverlay)
		.PlayerController(Rand)
		.InitialText(TEXT("Say "));
	RapidText->TextWidget()->OnKeyChar(FGeometry(),
		FCharacterEvent(TEXT('h'), FModifierKeysState(), 0, false));
	TestEqual(TEXT("the first real character is accepted immediately"),
		RapidText->GetText(), FString(TEXT("Say h")));
	Rand->Talk();
	TestTrue(TEXT("Talk enters typing"), Rand->IsChatTyping());
	TestEqual(TEXT("Talk uses the retail Say prefill"),
		Rand->GetChatTextForTesting(), FString(TEXT("Say ")));
	TestFalse(TEXT("typing does not globally suppress movement releases"),
		Rand->IsMoveInputIgnored());
	TestFalse(TEXT("typing leaves gameplay axes available"),
		Rand->IsLookInputIgnored());
	Rand->SetChatTextForTesting(TEXT("Say hello"));
	Rand->SubmitChatForTesting();
	TestFalse(TEXT("Enter leaves typing"), Rand->IsChatTyping());
	TestEqual(TEXT("Enter parses Say and reaches the server RPC"),
		Rand->LeftMessages, TArray<FString>({TEXT("Rand: hello")}));

	Rand->TeamTalk();
	TestEqual(TEXT("TeamTalk uses the retail TeamSay prefill"),
		Rand->GetChatTextForTesting(), FString(TEXT("TeamSay ")));
	Rand->ToggleInGameMenu();
	TestFalse(TEXT("Escape cancels typing"), Rand->IsChatTyping());
	TestFalse(TEXT("Escape used to cancel chat does not open the game menu"),
		Rand->IsInGameMenuOpen());

	Rand->Type();
	TestTrue(TEXT("bare Type enters typing with an empty line"),
		Rand->GetChatTextForTesting().IsEmpty());
	Rand->SetChatTextForTesting(TEXT("stat fps"));
	Rand->SubmitChatForTesting();
	TestEqual(TEXT("bare Type never exposes a console command"),
		Rand->LeftMessages.Num(), 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatInputDispatchSpec,
	"WoT.Chat.Input.DispatchIsolation", ChatFlags)

bool FWoTChatInputDispatchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestLeftMessageCaptureController* Controller =
		SpawnChatPlayer(World, TEXT("Rand"), 0);
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("input controller"), Controller)
		|| !TestNotNull(TEXT("input pawn"), Character))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Character);
	Controller->InitInputSystem();
	Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestChatAngreal* Probe = World->SpawnActor<AWoTTestChatAngreal>();
	if (!TestNotNull(TEXT("cast probe"), Probe))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Probe->GiveTo(Character);
	Probe->CurCharges = 10;
	WoTTest::TickWorld(World, 0.1f);

	DispatchChatInput(Controller, EKeys::LeftMouseButton, IE_Pressed, 1.0f);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("real Fire press reaches the selected angreal"),
		Probe->CastCount, 1);
	const int32 ReleasesBeforeChat = Probe->UnCastCount;
	Controller->Talk();
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("opening chat preserves an already-held cast"),
		Probe->UnCastCount, ReleasesBeforeChat);

	const FVector TypingLocation = Character->GetActorLocation();
	DispatchChatInput(Controller, EKeys::LeftMouseButton, IE_Pressed, 1.0f);
	DispatchChatInput(Controller, EKeys::W, IE_Pressed, 1.0f);
	WoTTest::TickWorld(World, 5);
	TestEqual(TEXT("Fire input dispatched while typing never casts"),
		Probe->CastCount, 1);
	TestTrue(TEXT("movement input dispatched while typing never moves the pawn"),
		Character->GetActorLocation().Equals(TypingLocation, 0.01));

	DispatchChatInput(Controller, EKeys::W, IE_Released, 0.0f);
	DispatchChatInput(Controller, EKeys::LeftMouseButton, IE_Released, 0.0f);
	WoTTest::TickWorld(World, 1);
	TestTrue(TEXT("a held Fire release dispatched while typing ends the cast"),
		Probe->UnCastCount > ReleasesBeforeChat);
	Controller->CancelChatInput();
	DispatchChatInput(Controller, EKeys::LeftMouseButton, IE_Pressed, 1.0f);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("gameplay Fire input is restored after chat closes"),
		Probe->CastCount, 2);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
