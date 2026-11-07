// Headless specs for AWoTHUD's pure scale/layout/state seams plus the retained N6
// readout. Pixel execution stays a parent-session visual smoke check.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTHUD.h"
#include "../WoTHUDFonts.h"
#include "../WoTCanvasScale.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTBattleGameMode.h"
#include "../WoTGameState.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTCitadelGameState.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTRetailText.h"
#include "../WoTBattleInfo.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvFireworks.h"
#include "../Inventory/WoTAngrealInvSpecial.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "WoTTestBattleClasses.h"
#include "WoTTestStatusIcons.h"

#ifdef FormatMessage
#undef FormatMessage
#endif

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/SoftObjectPath.h"
#include "GameFramework/InputSettings.h"
#include "Components/InputComponent.h"
#include "InputCoreTypes.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/UnrealType.h"

namespace
{
	// Minimal ticking Arena world with the base game mode (copied per the test-file
	// convention, now supplied by the shared standalone fixture).
	// As above, but with the Battle mode as authority (GameStateClass =
	// AWoTCitadelGameState, so SealGoal is present) for the seal read-out.
	UWorld* WoTHUDTests_CreateBattleWorld()
	{
		return WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	}

	FActorSpawnParameters WoTHUDTests_AlwaysSpawnParams()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	int32 WoTHUDTests_CountTexture(
		const TArray<FWoTHUDDrawCmd>& Commands, FName TextureId)
	{
		return Commands.FilterByPredicate([TextureId](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == TextureId;
		}).Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDScaleSpec,
	"WoT.HUD.Scale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDScaleSpec::RunTest(const FString&)
{
	FWoTCanvasScale Scale;
	Scale.ComputeFactors(320, 240);
	TestEqual(TEXT("half X"), Scale.FactorX, 0.5f);
	TestEqual(TEXT("half Y"), Scale.FactorY, 0.5f);
	Scale.ComputeFactors(640, 480);
	TestEqual(TEXT("unit X"), Scale.FactorX, 1.0f);
	TestEqual(TEXT("unit Y"), Scale.FactorY, 1.0f);
	Scale.ComputeFactors(1920, 1080);
	TestEqual(TEXT("no upscale X"), Scale.FactorX, 1.0f);
	Scale.ComputeFactors(0, 0);
	TestEqual(TEXT("degenerate X"), Scale.FactorX, 1.0f);
	Scale.ComputeFactors(320, 240);
	TestEqual(TEXT("ScaleVal"), Scale.ScaleValX(64.0f), 32.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDUpscaleSpec,
	"WoT.HUD.Upscale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDUpscaleSpec::RunTest(const FString&)
{
	struct FCase
	{
		int32 SizeX;
		int32 SizeY;
		int32 Expected;
	};
	const FCase AutoCases[] = {
		{640, 480, 1},
		{1512, 982, 2},
		{1920, 1080, 2},
		{2560, 1440, 3},
		{3024, 1964, 4},
		{320, 240, 1},
		{0, 0, 1},
	};
	for (const FCase& TestCase : AutoCases)
	{
		TestEqual(*FString::Printf(TEXT("auto %dx%d"),
			TestCase.SizeX, TestCase.SizeY),
			FWoTCanvasScale::ComputeUpscale(
				TestCase.SizeX, TestCase.SizeY, 0),
			TestCase.Expected);
	}
	TestEqual(TEXT("override 1 restores original size at Retina resolution"),
		FWoTCanvasScale::ComputeUpscale(3024, 1964, 1), 1);
	TestEqual(TEXT("override 8 clamps to the largest fitting integer"),
		FWoTCanvasScale::ComputeUpscale(1920, 1080, 8), 2);
	TestEqual(TEXT("override 3 clamps to one at the reference resolution"),
		FWoTCanvasScale::ComputeUpscale(640, 480, 3), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDVirtualCanvasSpec,
	"WoT.HUD.VirtualCanvas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDVirtualCanvasSpec::RunTest(const FString&)
{
	UCanvas* CanvasObject = NewObject<UCanvas>();
	if (!TestNotNull(TEXT("transient virtual canvas"), CanvasObject))
	{
		return false;
	}
	CanvasObject->SizeX = 1512;
	CanvasObject->SizeY = 982;
	CanvasObject->ClipX = 1500.0f;
	CanvasObject->ClipY = 960.0f;
	const FWoTCanvasDimensions Saved =
		FWoTCanvasScale::ApplyLogicalDimensions(CanvasObject, 2);
	TestEqual(TEXT("logical SizeX uses integer division"), CanvasObject->SizeX, 756);
	TestEqual(TEXT("logical SizeY uses integer division"), CanvasObject->SizeY, 491);
	TestEqual(TEXT("logical ClipX is divided"), CanvasObject->ClipX, 750.0f);
	TestEqual(TEXT("logical ClipY is divided"), CanvasObject->ClipY, 480.0f);
	FWoTCanvasScale::RestoreDimensions(CanvasObject, Saved);
	TestEqual(TEXT("restored SizeX"), CanvasObject->SizeX, 1512);
	TestEqual(TEXT("restored SizeY"), CanvasObject->SizeY, 982);
	TestEqual(TEXT("restored ClipX"), CanvasObject->ClipX, 1500.0f);
	TestEqual(TEXT("restored ClipY"), CanvasObject->ClipY, 960.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTeamSelectSpec,
	"WoT.HUD.TeamSelect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTeamSelectSpec::RunTest(const FString&)
{
	AWoTHUD* HUD = NewObject<AWoTHUD>();
	if (!TestNotNull(TEXT("team-select HUD"), HUD))
	{
		return false;
	}
	TestEqual(TEXT("neutral player sees fallback prompt and countdown"),
		HUD->TeamSelectPrompt(255, 3, 12),
		FString(TEXT("Choose a side: (1-3) within 12 sec")));
	TestEqual(TEXT("zero countdown omits suffix"),
		HUD->TeamSelectPrompt(255, 2, 0),
		FString(TEXT("Choose a side: (1-2)")));
	TestEqual(TEXT("with a controller the prompt names the D-pad buttons too"),
		HUD->TeamSelectPrompt(255, 2, 0, /*bControllerHint*/ true),
		FString(TEXT("Choose a side: (1-2 or D-pad Left/Right)")));
	TestEqual(TEXT("a three-team map adds D-pad Up"),
		HUD->TeamSelectPrompt(255, 3, 12, true),
		FString(TEXT("Choose a side: (1-3 or D-pad Left/Right/Up) within 12 sec")));
	TestTrue(TEXT("team-select fallback fragments remain localizable FText"),
		!HUD->SelectTeamStr.IsEmpty() && !HUD->TimeLimitStr.IsEmpty() &&
		!HUD->SecondsStr.IsEmpty());
	HUD->SelectTeamStr = FText::FromString(TEXT("Choose Side:"));
	HUD->TimeLimitStr = FText::FromString(TEXT(" within "));
	HUD->SecondsStr = FText::FromString(TEXT(" ticks"));
	TestEqual(TEXT("team prompt formats through the exposed localized fragments"),
		HUD->TeamSelectPrompt(255, 4, 9),
		FString(TEXT("Choose Side: (1-4) within 9 ticks")));
	TestEqual(TEXT("a four-team map names all four D-pad buttons"),
		HUD->TeamSelectPrompt(255, 4, 9, true),
		FString(TEXT("Choose Side: (1-4 or D-pad Left/Right/Up/Down) within 9 ticks")));
	const FVector2D WidePromptPosition = AWoTHUD::TeamSelectPosition(
		1920.0f, 240.0f, 16.0f);
	TestEqual(TEXT("wide team prompt centers against the actual viewport"),
		WidePromptPosition.X, 840.0);
	TestEqual(TEXT("team prompt Y is not legend-scaled"),
		WidePromptPosition.Y, 48.0);
	TestTrue(TEXT("neutral valid digit is intercepted"),
		AWoTHUD::ShouldInterceptTeamDigit(255, 3, 2));
	TestFalse(TEXT("neutral out-of-range digit falls through to hand selection"),
		AWoTHUD::ShouldInterceptTeamDigit(255, 3, 3));
	TestFalse(TEXT("assigned player's digit always reaches hand selection"),
		AWoTHUD::ShouldInterceptTeamDigit(1, 3, 1));
	TestTrue(TEXT("assigned player sees no team prompt"),
		HUD->TeamSelectPrompt(1, 3, 12).IsEmpty());

	UWorld* World = WoTHUDTests_CreateBattleWorld();
	AWoTBattleInfo* BattleInfo = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTPlayerController* WoTController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* WoTPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	if (!BattleInfo || !BattleMode || !WoTController || !WoTPawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BattleInfo->NumTeams = 2;
	BattleInfo->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BattleInfo->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BattleInfo->FinishSpawning(FTransform::Identity);
	BattleInfo->bTeleportersDisabled = false;
	BattleMode->Battle = BattleInfo;
	WoTController->InitPlayerState();
	WoTController->Possess(WoTPawn);
	AWoTPlayerState* WoTState =
		WoTController->GetPlayerState<AWoTPlayerState>();
	if (!WoTState)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTState->Team = 255;
	WoTController->JoinTeamTimeout = 1;
	BattleMode->Timer();
	TestEqual(TEXT("timeout zero assigns smallest team"),
		static_cast<int32>(WoTState->Team), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDPlayerIDSpec,
	"WoT.HUD.PlayerID",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDPlayerIDSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* OwnerController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* OwnerPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTPlayerController* TargetController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* TargetPawn = World->SpawnActor<AWoTCharacter>(
		FVector(400.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	if (!OwnerController || !OwnerPawn || !TargetController || !TargetPawn || !HUD)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	OwnerController->InitPlayerState();
	OwnerController->Possess(OwnerPawn);
	OwnerController->SetControlRotation(FRotator::ZeroRotator);   // aim +X at the target
	HUD->PlayerOwner = OwnerController;
	TargetController->InitPlayerState();
	TargetController->Possess(TargetPawn);
	if (APlayerState* TargetState = TargetController->PlayerState)
	{
		TargetState->SetPlayerName(TEXT("Moghedien"));
	}
	TestEqual(TEXT("visible player pawn resolves replicated player name"),
		AWoTHUD::PlayerIDName(TargetPawn), FString(TEXT("Moghedien")));

	// The REAL trace path (BaseHUD.uc:1394 TraceRecursive with bTraceActors): pawn
	// profiles ignore ECC_Visibility, so UpdatePlayerID must find the aimed-at pawn by
	// object type — a plain visibility trace can only ever hit world geometry.
	HUD->UpdatePlayerID(nullptr, 0.0f);
	TestEqual(TEXT("aiming at a player arms the ID overlay"),
		HUD->IdentifyTarget, FString(TEXT("Moghedien")));
	TestEqual(TEXT("arming resets the shipped 350 fade"),
		HUD->IdentifyFadeTime, 350.0f);

	// A wall between the pawns occludes the ID: the nearer blocking world hit must win
	// over the pawn hit (TraceRecursive respected world occlusion the same way). Use a
	// registered primitive rather than a shared engine mesh whose physics state can be
	// asynchronously recreated or mutated by an earlier full-suite test.
	AActor* Wall = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector(200.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Wall)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->SetRootComponent(WallBox);
	Wall->AddInstanceComponent(WallBox);
	WallBox->SetBoxExtent(FVector(10.0f, 200.0f, 200.0f));
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	WallBox->SetCollisionObjectType(ECC_WorldStatic);
	WallBox->SetCollisionResponseToAllChannels(ECR_Block);
	WallBox->RegisterComponent();
	Wall->SetActorLocation(FVector(200.0f, 0.0f, 100.0f));
	HUD->IdentifyTarget.Reset();
	HUD->IdentifyFadeTime = 0.0f;
	HUD->UpdatePlayerID(nullptr, 0.0f);
	TestTrue(TEXT("a wall-occluded player is not identified"),
		HUD->IdentifyTarget.IsEmpty());

	TargetPawn->SetActorHiddenInGame(true);
	TestTrue(TEXT("hidden player is not identified"),
		AWoTHUD::PlayerIDName(TargetPawn).IsEmpty());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// BattleHUD.KeyEvent (BattleHUD.uc:63-64): a neutral player's team digit changes team
// AND restarts them — the WoT restart RPC must replace the LIVE pawn with the faction
// class at a team-area start (the engine ServerRestartPlayer no-ops on a live pawn).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTeamSelectRestartSpec,
	"WoT.HUD.TeamSelectRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTeamSelectRestartSpec::RunTest(const FString&)
{
	UWorld* World = WoTHUDTests_CreateBattleWorld();
	AWoTBattleInfo* BattleInfo = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTPlayerController* WoTController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* NeutralPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	if (!BattleInfo || !BattleMode || !WoTController || !NeutralPawn || !HUD)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BattleInfo->NumTeams = 2;
	BattleInfo->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BattleInfo->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BattleInfo->FinishSpawning(FTransform::Identity);
	BattleInfo->bTeleportersDisabled = false;
	BattleMode->Battle = BattleInfo;

	// One start per team area (the Battle mode sets bSpawnInTeamArea): the digit-1
	// restart must come up in the team-1 area, far from the team-0 start.
	AWoTPlayerStart* Team0Start = World->SpawnActor<AWoTPlayerStart>(
		FVector(-2000.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTPlayerStart* Team1Start = World->SpawnActor<AWoTPlayerStart>(
		FVector(2000.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	if (!Team0Start || !Team1Start)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Team0Start->TeamNumber = 0;
	Team1Start->TeamNumber = 1;

	WoTController->InitPlayerState();
	WoTController->Possess(NeutralPawn);
	HUD->PlayerOwner = WoTController;
	AWoTPlayerState* WoTState = WoTController->GetPlayerState<AWoTPlayerState>();
	if (!WoTState)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTState->Team = 255;

	TestTrue(TEXT("neutral team digit is intercepted"),
		HUD->HandleTeamSelectDigit(1));
	TestEqual(TEXT("the team change was honored"),
		static_cast<int32>(WoTState->Team), 1);
	APawn* RestartedPawn = WoTController->GetPawn();
	TestNotNull(TEXT("the player restarted into a pawn"), RestartedPawn);
	TestTrue(TEXT("the live neutral pawn was replaced, not kept"),
		RestartedPawn != nullptr && RestartedPawn != NeutralPawn);
	TestFalse(TEXT("the old neutral pawn was consumed"), IsValid(NeutralPawn));
	TestTrue(TEXT("the restart spawned the team-1 faction class"),
		RestartedPawn && RestartedPawn->IsA(AWoTTestTeamBravo::StaticClass()));
	if (RestartedPawn)
	{
		TestTrue(TEXT("the restart came up in the team-1 start area"),
			FVector::Dist(RestartedPawn->GetActorLocation(),
				Team1Start->GetActorLocation()) < 500.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// giMPBattle.ChangeTeam team-flow feedback (giMPBattle.uc:328-362) + the WOTPlayer
// while-editing refusal (WOTPlayer.uc:4228-4232): rejects answer the requester
// (TeamIsYours/TeamIsFilled left messages, CantChangeTeams center message) and
// join/leave broadcast to every player's left queue — retail WoT.int:48,64-67 wording.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTeamFlowMessagesSpec,
	"WoT.HUD.TeamFlowMessages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTeamFlowMessagesSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping loaded message checks"));
		return true;
	}
	const FString* CantChangeTeams = FWoTRetailText::FindSectionValue(
		TEXT("WOTPlayer"), TEXT("CantChangeTeamsStr"));
	if (!TestNotNull(TEXT("CantChangeTeamsStr is loaded"), CantChangeTeams))
	{
		return false;
	}
	UWorld* World = WoTHUDTests_CreateBattleWorld();
	AWoTBattleInfo* BattleInfo = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!BattleInfo || !BattleMode || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BattleInfo->NumTeams = 2;
	BattleInfo->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BattleInfo->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BattleInfo->FinishSpawning(FTransform::Identity);
	BattleMode->Battle = BattleInfo;
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	PC->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD());
	AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("local controller owns the WoT HUD"), HUD) || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PS->SetPlayerName(TEXT("Rand"));
	PS->Team = 255;

	// Join: giMPBattle.uc:357 combines the player name, localized join fragment,
	// and team description for every player's left queue.
	BattleMode->ChangeTeam(PC, 0);
	TestEqual(TEXT("join broadcast reaches the left queue with localized wording"),
		HUD->LeftMessages[0].Message,
		FString(TEXT("Rand")) + BattleMode->PlayerJoinTeamStr + TEXT("Alpha"));

	// Re-request the same team: giMPBattle.uc:330 answers the requester.
	BattleMode->ChangeTeam(PC, 0);
	TestEqual(TEXT("same-team reject posts TeamIsYoursStr $ Team"),
		HUD->LeftMessages[0].Message,
		BattleMode->TeamIsYoursStr + TEXT("0"));

	// A full team: giMPBattle.uc:337 (the only false return).
	BattleInfo->NumPlayers[1] = BattleMode->MaxTeamSize;
	TestFalse(TEXT("a full team rejects the change"), BattleMode->ChangeTeam(PC, 1));
	TestFalse(TEXT("TeamIsFilledStr came from the loaded localization data"),
		BattleMode->TeamIsFilledStr.IsEmpty());
	TestEqual(TEXT("full-team reject posts TeamIsFilledStr $ Team"),
		HUD->LeftMessages[0].Message, BattleMode->TeamIsFilledStr + TEXT("1"));

	// Leave to neutral: giMPBattle.uc:362's retail wording.
	BattleMode->ChangeTeam(PC, 255);
	TestEqual(TEXT("leave broadcast uses the retail wording"),
		HUD->LeftMessages[0].Message,
		FString(TEXT("Rand")) + BattleMode->PlayerLeaveTeamStr);

	// While editing: WOTPlayer.ServerChangeTeam refuses with CantChangeTeamsStr
	// (WoT.int:48), surfaced center-screen.
	PC->EditSessionTeam = 0;
	PC->ServerChangeTeam(1);
	TestEqual(TEXT("editing reject posts CantChangeTeamsStr center-screen"),
		HUD->CenterMessages[0].Message,
		*CantChangeTeams);
	TestEqual(TEXT("editing reject leaves the player neutral"),
		static_cast<int32>(PS->Team), 255);
	PC->EditSessionTeam = 255;

	WoTTest::DestroySpecWorld(World);
	return true;
}

// AngrealInventory.uc:604-618 + :539: the first pickup of a NEW artifact type flashes
// I_NotifyPickup for the armed NotifyNewPickupFadeTime (one-shot — the instance disarms
// itself), and a held duplicate absorbs later copies without ever re-flashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDNotifyNewPickupSpec,
	"WoT.HUD.NotifyNewPickup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDNotifyNewPickupSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		WoTHUDTests_AlwaysSpawnParams());
	if (!PlayerController || !Character || !Fireball)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);
	PlayerController->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PlayerController->GetHUD());
	if (!TestNotNull(TEXT("local controller owns the WoT HUD"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// An armed instance (SP map data sets NotifyNewPickupFadeTime=7): first pickup.
	Fireball->NotifyNewPickupFadeTime = 7.0f;
	TestTrue(TEXT("first pickup is granted"), Fireball->TryPickupBy(Character));
	TestTrue(TEXT("first new pickup flashes I_NotifyPickup"),
		HUD->FlashTextureId == FName(TEXT("I_NotifyPickup")));
	TestEqual(TEXT("the flash runs the armed fade time"),
		HUD->FlashTextureTime, 7.0f);
	TestEqual(TEXT("the notify disarmed itself (one-shot)"),
		Fireball->NotifyNewPickupFadeTime, 0.0f);
	TestEqual(TEXT("the typed 'Pickup' message landed in the hand slot"),
		HUD->HandMessage.Message, Fireball->PickupMessage);

	// A duplicate is absorbed by HandlePickupQuery (charge steal), never granted, and
	// never re-flashes — the notify fires once per type entering the inventory.
	HUD->FlashTextureId = NAME_None;
	HUD->FlashTextureTime = 0.0f;
	HUD->FlashTextureTimer = 0.0f;
	AWoTAngrealInvFireball* Duplicate = World->SpawnActor<AWoTAngrealInvFireball>(
		WoTHUDTests_AlwaysSpawnParams());
	if (!Duplicate)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Duplicate->NotifyNewPickupFadeTime = 7.0f;
	TestFalse(TEXT("a duplicate is absorbed, not granted"),
		Duplicate->TryPickupBy(Character));
	TestTrue(TEXT("a duplicate of a held type never re-flashes"),
		HUD->FlashTextureId.IsNone());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// WOTPlayer.ClientMessage (WOTPlayer.uc:1862-1890) typed routing + the giWOT death
// message helpers the broadcast producer composes with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTypedMessagesSpec,
	"WoT.HUD.TypedMessages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTypedMessagesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD());
	if (!TestNotNull(TEXT("local controller owns the WoT HUD"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	PC->RouteTypedMessage(TEXT("pickup-probe"),
		FName(TEXT("Pickup")), true);
	TestEqual(TEXT("'Pickup' lands in the hand slot"),
		HUD->HandMessage.Message, FString(TEXT("pickup-probe")));
	PC->RouteTypedMessage(TEXT("Dart failed."),
		FName(TEXT("FailedMessage")), true);
	TestEqual(TEXT("'FailedMessage' lands in the hand slot"),
		HUD->HandMessage.Message, FString(TEXT("Dart failed.")));
	PC->RouteTypedMessage(TEXT("A was killed by B."),
		FName(TEXT("WOTDeathMessage")), false);
	TestEqual(TEXT("MP 'WOTDeathMessage' lands in the right queue"),
		HUD->RightMessages[0].Message, FString(TEXT("A was killed by B.")));
	PC->RouteTypedMessage(TEXT("C was killed by D."),
		FName(TEXT("WOTDeathMessage")), true);
	TestEqual(TEXT("standalone drops the death message (MP-only channel)"),
		HUD->RightMessages[0].Message, FString(TEXT("A was killed by B.")));
	PC->RouteTypedMessage(TEXT("legacy"), FName(TEXT("DeathMessage")), false);
	TestTrue(TEXT("deprecated 'DeathMessage' is swallowed"),
		HUD->RightMessages[0].Message == TEXT("A was killed by B.") &&
		HUD->HandMessage.Message == TEXT("Dart failed."));
	PC->RouteTypedMessage(TEXT(""), FName(TEXT("Pickup")), true);
	TestEqual(TEXT("empty messages are dropped"),
		HUD->HandMessage.Message, FString(TEXT("Dart failed.")));

	// giWOT.FormatMessage: strip leading spaces, capitalize the first character.
	TestEqual(TEXT("FormatMessage strips leading spaces and capitalizes"),
		AWoTGameMode::FormatMessage(TEXT("  bob was killed by Ann.")),
		FString(TEXT("Bob was killed by Ann.")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// giWOT.ScoreKill (giWOT.uc:127-158): kill/suicide/accident announcements broadcast to
// EVERY player as 'WOTDeathMessage', with the retail WoT.int wording (gendered suicide/
// accident fragments). Captured above the standalone gate via a recording controller.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDDeathMessageBroadcastSpec,
	"WoT.HUD.DeathMessageBroadcast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDDeathMessageBroadcastSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping loaded death messages"));
		return true;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestMessageCaptureController* KillerPC =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	AWoTTestMessageCaptureController* VictimPC =
		World->SpawnActor<AWoTTestMessageCaptureController>();
	AWoTCharacter* KillerPawn = World->SpawnActor<AWoTCharacter>(
		FVector(200.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTCharacter* VictimPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	if (!Game || !KillerPC || !VictimPC || !KillerPawn || !VictimPawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	KillerPC->InitPlayerState();
	VictimPC->InitPlayerState();
	Game->ChangeTeam(KillerPC, 255);   // unique free-for-all teams (enemies)
	Game->ChangeTeam(VictimPC, 255);
	KillerPC->Possess(KillerPawn);
	VictimPC->Possess(VictimPawn);
	KillerPC->PlayerState->SetPlayerName(TEXT("Lanfear"));
	VictimPC->PlayerState->SetPlayerName(TEXT("Moghedien"));

	// A frag: every player receives the composed announcement.
	Game->ScoreKill(KillerPC, VictimPawn);
	TestEqual(TEXT("the kill broadcast reaches the victim"),
		VictimPC->CapturedMessages.Num(), 1);
	TestEqual(TEXT("the kill broadcast reaches the killer too"),
		KillerPC->CapturedMessages.Num(), 1);
	if (VictimPC->CapturedMessages.Num() == 1)
	{
		TestEqual(TEXT("retail kill wording"), VictimPC->CapturedMessages[0],
			AWoTGameMode::FormatMessage(FString(TEXT("Moghedien"))
				+ Game->DeathConjunction + TEXT("Lanfear.")));
		TestTrue(TEXT("the broadcast is typed 'WOTDeathMessage'"),
			VictimPC->CapturedTypes[0] == FName(TEXT("WOTDeathMessage")));
	}

	// A suicide announces with the gendered retail fragment.
	VictimPawn->bIsFemale = true;
	Game->ScoreKill(VictimPC, VictimPawn);
	TestTrue(TEXT("retail female suicide wording"),
		VictimPC->CapturedMessages.Num() > 0 &&
		VictimPC->CapturedMessages.Last() ==
			AWoTGameMode::FormatMessage(FString(TEXT("Moghedien"))
				+ Game->SuicideConjunctionFemale + TEXT(".")));

	// A killer-less environment death (no recent displacement) is an accident.
	VictimPawn->bIsFemale = false;
	Game->ScoreKill(nullptr, VictimPawn);
	TestTrue(TEXT("retail accident wording"),
		VictimPC->CapturedMessages.Num() > 0 &&
		VictimPC->CapturedMessages.Last() ==
			AWoTGameMode::FormatMessage(FString(TEXT("Moghedien"))
				+ Game->AccidentConjunctionMale + TEXT(".")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTurtleSpec,
	"WoT.HUD.Turtle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTurtleSpec::RunTest(const FString&)
{
	AWoTHUD* HUD = NewObject<AWoTHUD>();
	if (!TestNotNull(TEXT("HUD defaults"), HUD))
	{
		return false;
	}
	TestFalse(TEXT("shipped config defaults turtle disabled"), HUD->bTurtleEnabled);
	HUD->BringBackTurtle();
	TestTrue(TEXT("BringBackTurtle enables perf warning"), HUD->bTurtleEnabled);
	HUD->GetRidOfTurtle();
	TestFalse(TEXT("GetRidOfTurtle disables perf warning"), HUD->bTurtleEnabled);
	HUD->SetTurtleFadeTime(2.0f);
	TestEqual(TEXT("SetTurtleFadeTime updates duration"), HUD->TurtleFadeTime, 2.0f);
	TestTrue(TEXT("SP threshold is strict"),
		AWoTHUD::ClassifyTurtle(0.05f, true, 0.05f, 0.05f,
			0.083333f, 0.066666f) == EWoTTurtleFlash::None);
	TestTrue(TEXT("SP slow frame selects live turtle"),
		AWoTHUD::ClassifyTurtle(0.06f, true, 0.05f, 0.05f,
			0.083333f, 0.066666f) == EWoTTurtleFlash::Turtle);
	TestTrue(TEXT("SP dead threshold selects dead turtle"),
		AWoTHUD::ClassifyTurtle(0.09f, true, 0.05f, 0.05f,
			0.083333f, 0.066666f) == EWoTTurtleFlash::TurtleDead);
	TestTrue(TEXT("MP uses its lower dead threshold"),
		AWoTHUD::ClassifyTurtle(0.07f, false, 0.05f, 0.05f,
			0.083333f, 0.066666f) == EWoTTurtleFlash::TurtleDead);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDWarnFlashSpec,
	"WoT.HUD.WarnFlash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDWarnFlashSpec::RunTest(const FString&)
{
	AWoTHUD* HUD = NewObject<AWoTHUD>();
	if (!TestNotNull(TEXT("HUD flash state"), HUD))
	{
		return false;
	}
	HUD->SetFlashTexture(FName(TEXT("Turtle")), 2.0f);
	TestTrue(TEXT("SetFlashTexture arms requested texture and duration"),
		HUD->FlashTextureId == FName(TEXT("Turtle")) &&
		HUD->FlashTextureTime == 2.0f && HUD->FlashTextureTimer == 2.0f);
	TestTrue(TEXT("warning remains active before timeout"),
		AWoTHUD::TimedExtraActive(10.0f, 1.5f, 11.49f));
	TestFalse(TEXT("warning expires exactly at timeout"),
		AWoTHUD::TimedExtraActive(10.0f, 1.5f, 11.5f));
	TestEqual(TEXT("flash begins fully bright"),
		AWoTHUD::FlashFade(1.0f, 1.0f), 1.0f);
	TestEqual(TEXT("flash fades linearly"),
		AWoTHUD::FlashFade(0.25f, 1.0f), 0.25f);
	TestEqual(TEXT("expired flash is dark"),
		AWoTHUD::FlashFade(-0.1f, 1.0f), 0.0f);
	TestEqual(TEXT("wide viewport is the shipped scaled width"),
		AWoTHUD::ScaledViewportWidth(1920), 1920);
	TestEqual(TEXT("small viewport retains the shipped 640 layout width"),
		AWoTHUD::ScaledViewportWidth(320), 640);
	TestEqual(TEXT("wide flash anchors 64 pixels from scaled right edge"),
		AWoTHUD::FlashTextureX(1920), 1856.0f);
	TestEqual(TEXT("wide warning centers from shipped 16-pixel extent"),
		AWoTHUD::WarningTextureX(1921), 952.5f);
	const FLinearColor HalfFade = AWoTHUD::FadedHUDColor(0.5f);
	TestTrue(TEXT("translucent HUD fade keeps white RGB and fades alpha"),
		HalfFade.R == 1.0f && HalfFade.G == 1.0f && HalfFade.B == 1.0f &&
		HalfFade.A == 0.5f);
	HUD->bWarningEnabled = false;
	HUD->SetWarning();
	TestFalse(TEXT("SetWarning retains shipped timestamp-only semantics"),
		HUD->bWarningEnabled);
	HUD->ClientWarn();
	TestTrue(TEXT("ClientWarn enables and arms the warning path"),
		HUD->bWarningEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDSealSelectableSpec,
	"WoT.HUD.SealSelectable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDSealSelectableSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		FVector::ZeroVector, FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	if (!Character || !Seal || !Character->InventoryComponent)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->InventoryComponent->AddInventory(Seal);
	TestEqual(TEXT("first carried Seal auto-selects like shipped WOTPlayer"),
		Character->InventoryComponent->SelectedItem.Get(),
		static_cast<AWoTInventory*>(Seal));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDCrosshairSpec,
	"WoT.HUD.Crosshair", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDCrosshairSpec::RunTest(const FString&)
{
	AWoTHUD* HUD = NewObject<AWoTHUD>();
	HUD->Crosshair = 20;
	HUD->ChangeCrosshair(1);
	TestEqual(TEXT("wrap up"), HUD->Crosshair, 0);
	HUD->Crosshair = 0;
	HUD->ChangeCrosshair(-1);
	TestEqual(TEXT("wrap down"), HUD->Crosshair, AWoTHUD::NumCrossHairs - 1);
	const TArray<FWoTHUDDrawCmd> Commands = AWoTHUD::LayoutCrosshair(0, 800, 600);
	TestEqual(TEXT("one cmd"), Commands.Num(), 1);
	TestTrue(TEXT("named chair1"), Commands[0].TexId == FName(TEXT("Crosshair1")));
	TestEqual(TEXT("centered X"), Commands[0].X, 392.0f);
	TestEqual(TEXT("centered Y"), Commands[0].Y, 292.0f);
	const TArray<FWoTHUDDrawCmd> None = AWoTHUD::LayoutCrosshair(20, 800, 600);
	TestEqual(TEXT("none"), None.Num(), 0);
	// Odd clip dims truncate the final centre (MainHUD.uc:114-118 assigns 0.5*Clip-8
	// to an int): 0.5*801-8 = 392.5 -> 392, 0.5*601-8 = 292.5 -> 292.
	const TArray<FWoTHUDDrawCmd> Odd = AWoTHUD::LayoutCrosshair(0, 801, 601);
	TestEqual(TEXT("odd width truncates"), Odd[0].X, 392.0f);
	TestEqual(TEXT("odd height truncates"), Odd[0].Y, 292.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDHealthIconIndexSpec,
	"WoT.HUD.HealthIconIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDHealthIconIndexSpec::RunTest(const FString&)
{
	TestEqual(TEXT("full"), AWoTCharacter::HealthIconIndex(100, 100), 0);
	TestEqual(TEXT("75pct"), AWoTCharacter::HealthIconIndex(75, 100), 0);
	TestEqual(TEXT("just below 75pct"), AWoTCharacter::HealthIconIndex(74, 100), 1);
	TestEqual(TEXT("50pct"), AWoTCharacter::HealthIconIndex(50, 100), 1);
	TestEqual(TEXT("just below 50pct"), AWoTCharacter::HealthIconIndex(49, 100), 2);
	TestEqual(TEXT("25pct"), AWoTCharacter::HealthIconIndex(25, 100), 2);
	TestEqual(TEXT("just below 25pct"), AWoTCharacter::HealthIconIndex(24, 100), 3);
	TestEqual(TEXT("1hp"), AWoTCharacter::HealthIconIndex(1, 100), 3);
	TestEqual(TEXT("0hp"), AWoTCharacter::HealthIconIndex(0, 100), 3);
	TestEqual(TEXT("overmax"), AWoTCharacter::HealthIconIndex(150, 100), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDDisguiseHealthIconSpec,
	"WoT.HUD.DisguiseHealthIcon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDDisguiseHealthIconSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAesSedai* Character = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	if (!Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->Health = 49;
	UTexture2D* Undisguised = Character->GetHealthIcon();
	TestTrue(TEXT("undisguised health selects slot 2"),
		Undisguised && Undisguised->GetFName() == FName(TEXT("H_AesSedai2")));

	Character->Disguise.bActive = 1;
	Character->Disguise.SourceClass = AWoTForsaken::StaticClass();
	UTexture2D* Disguised = Character->GetHealthIcon();
	TestTrue(TEXT("disguised health uses source-class DisguiseIcon"),
		Disguised &&
		Disguised->GetFName() == FName(TEXT("H_ForsakenDisguise")));

	Character->RevertDisguise();
	UTexture2D* Restored = Character->GetHealthIcon();
	TestTrue(TEXT("disguise end restores health-scaled icon"),
		Restored && Restored->GetFName() == FName(TEXT("H_AesSedai2")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDStatusIconModelSpec,
	"WoT.HUD.StatusIcons.Model",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDStatusIconModelSpec::RunTest(const FString&)
{
	const FProperty* StatusIconsProperty = FindFProperty<FProperty>(
		AWoTCharacter::StaticClass(), TEXT("StatusIcons"));
	TestTrue(TEXT("status icons are replicated"), StatusIconsProperty &&
		StatusIconsProperty->HasAnyPropertyFlags(CPF_Net));
	TestEqual(TEXT("status icons use owner-only replication"),
		static_cast<int32>(AWoTCharacter::StatusIconsReplicationCondition()),
		static_cast<int32>(COND_OwnerOnly));

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvSpecial* Angreal = World->SpawnActor<AWoTAngrealInvSpecial>(
		WoTHUDTests_AlwaysSpawnParams());
	if (!Character || !Angreal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Angreal->SetOwner(Character);
	Angreal->DurationType = EWoTDurationType::DT_Charge;
	Angreal->CurCharges = 5;
	Character->AddIconInfo(Angreal, true, FName(TEXT("AngrealIcon")));

	UWoTTestStatusLeech* Leech = NewObject<UWoTTestStatusLeech>(
		Character, TEXT("StatusModelLeech"));
	Leech->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Freeze.M_Freeze"));
	Leech->StatusInitialDuration = 11.0f;
	Leech->StatusCurrentDuration = 3.0f;
	Character->AddIconInfo(Leech, false, FName(TEXT("LeechIcon")));

	TestEqual(TEXT("two model entries"), Character->StatusIcons.Num(), 2);
	if (Character->StatusIcons.Num() == 2)
	{
		TestTrue(TEXT("push-front order is newest first"),
			Character->StatusIcons[0].IconID == FName(TEXT("LeechIcon")) &&
			Character->StatusIcons[1].IconID == FName(TEXT("AngrealIcon")));
		TestEqual(TEXT("angreal seeds initial from current GetDuration"),
			Character->StatusIcons[1].InitialDuration, 5.0f);
		TestEqual(TEXT("leech seeds initial from GetInitialDuration"),
			Character->StatusIcons[0].InitialDuration, 11.0f);
		TestEqual(TEXT("leech seed does not use current duration"),
			Character->StatusIcons[0].RemainingDuration, 11.0f);
	}

	Angreal->CurCharges = 2;
	Character->Tick(0.0f);
	if (Character->StatusIcons.Num() == 2)
	{
		TestEqual(TEXT("leech remaining re-polls GetDuration"),
			Character->StatusIcons[0].RemainingDuration, 3.0f);
		TestEqual(TEXT("angreal remaining re-polls GetDuration"),
			Character->StatusIcons[1].RemainingDuration, 2.0f);
	}
	Leech->RegisterComponent();
	Leech->DestroyComponent();
	Character->Tick(0.0f);
	TestTrue(TEXT("stale source remains listed with zero duration"),
		Character->StatusIcons.Num() == 2 &&
		Character->StatusIcons[0].IconID == FName(TEXT("LeechIcon")) &&
		Character->StatusIcons[0].RemainingDuration == 0.0f);

	Angreal->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Whirlwind.M_Whirlwind"));
	Angreal->CurCharges = 6;
	Character->AddIconInfo(Angreal, false, FName(TEXT("AngrealIcon")));
	TestEqual(TEXT("same IconID updates in place"), Character->StatusIcons.Num(), 2);
	if (Character->StatusIcons.Num() == 2)
	{
		TestTrue(TEXT("update preserves list position and replaces fields"),
			Character->StatusIcons[1].IconID == FName(TEXT("AngrealIcon")) &&
			Character->StatusIcons[1].IconTexture == FName(TEXT("M_Whirlwind")) &&
			!Character->StatusIcons[1].bGood &&
			Character->StatusIcons[1].InitialDuration == 6.0f);
	}
	Character->RemoveIconInfo(FName(TEXT("LeechIcon")));
	TestTrue(TEXT("remove unlinks the matching IconID"),
		Character->StatusIcons.Num() == 1 &&
		Character->StatusIcons[0].IconID == FName(TEXT("AngrealIcon")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDStatusIconProducerSpec,
	"WoT.HUD.StatusIcons.Producers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDStatusIconProducerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvSpecial* Angreal = World->SpawnActor<AWoTAngrealInvSpecial>(
		WoTHUDTests_AlwaysSpawnParams());
	if (!Character || !Angreal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Angreal->SetOwner(Character);
	Angreal->bDisplayIcon = true;
	Angreal->CastArtifact();
	TestTrue(TEXT("bDisplayIcon angreal cast adds a good icon"),
		Character->StatusIcons.Num() == 1 && Character->StatusIcons[0].bGood &&
		Character->StatusIcons[0].IconID == Angreal->GetFName());
	Angreal->UnCastArtifact();
	TestEqual(TEXT("angreal uncast removes its icon"),
		Character->StatusIcons.Num(), 0);

	UWoTTestStatusLeech* Leech = NewObject<UWoTTestStatusLeech>(
		Character, TEXT("StatusProducerLeech"));
	Leech->bDisplayIcon = true;
	Leech->bDeleterious = true;
	Leech->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Freeze.M_Freeze"));
	Leech->StatusInitialDuration = 8.0f;
	Leech->StatusCurrentDuration = 7.0f;
	Leech->AttachTo(Character);
	TestTrue(TEXT("deleterious leech attach adds a bad icon"),
		Character->StatusIcons.Num() == 1 && !Character->StatusIcons[0].bGood &&
		Character->StatusIcons[0].IconID == Leech->GetFName());
	Leech->Unattach();
	TestEqual(TEXT("leech unattach removes its icon"),
		Character->StatusIcons.Num(), 0);

	UWoTTestStatusReflector* Reflector = NewObject<UWoTTestStatusReflector>(
		Character, TEXT("StatusProducerReflector"));
	Reflector->bDeleterious = true;
	Reflector->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Reflect.M_Reflect"));
	Reflector->StatusInitialDuration = 12.0f;
	Reflector->StatusCurrentDuration = 10.0f;
	Reflector->Install(Character);
	TestTrue(TEXT("deleterious reflector install adds a bad icon"),
		Character->StatusIcons.Num() == 1 && !Character->StatusIcons[0].bGood &&
		Character->StatusIcons[0].IconID == Reflector->GetFName());
	Reflector->Uninstall();
	TestEqual(TEXT("reflector uninstall removes its icon"),
		Character->StatusIcons.Num(), 0);

	UWoTTestStatusLeech* MissingFrame = NewObject<UWoTTestStatusLeech>(
		Character, TEXT("StatusMissingFrame"));
	MissingFrame->bDisplayIcon = true;
	AddExpectedError(TEXT("Attached effect has no icon or originating artifact"),
		EAutomationExpectedErrorFlags::Contains, 1);
	MissingFrame->AttachTo(Character);
	TestEqual(TEXT("missing frame/source warns and adds nothing"),
		Character->StatusIcons.Num(), 0);
	MissingFrame->Unattach();

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDStatusIconLayoutSpec,
	"WoT.HUD.StatusIcons.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDStatusIconLayoutSpec::RunTest(const FString&)
{
	TestEqual(TEXT("zero initial uses first frame"),
		AWoTHUD::StatusTimerIndex(0.0f, 10.0f), 0);
	TestEqual(TEXT("remaining above initial clamps low"),
		AWoTHUD::StatusTimerIndex(100.0f, 150.0f), 0);
	TestEqual(TEXT("full duration uses first frame"),
		AWoTHUD::StatusTimerIndex(100.0f, 100.0f), 0);
	TestEqual(TEXT("half duration uses middle frame"),
		AWoTHUD::StatusTimerIndex(100.0f, 50.0f), 9);
	TestEqual(TEXT("expired uses last frame"),
		AWoTHUD::StatusTimerIndex(100.0f, 0.0f), 18);
	TestEqual(TEXT("negative remaining clamps high"),
		AWoTHUD::StatusTimerIndex(100.0f, -100.0f), 18);

	TArray<FWoTStatusIconEntry> Entries;
	auto AddEntry = [&Entries](const TCHAR* Texture, bool bGood,
		float Initial, float Remaining)
	{
		FWoTStatusIconEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.IconTexture = FName(Texture);
		Entry.bGood = bGood;
		Entry.InitialDuration = Initial;
		Entry.RemainingDuration = Remaining;
	};
	AddEntry(TEXT("M_Levitation"), true, 100.0f, 100.0f);
	AddEntry(TEXT("M_Freeze"), false, 100.0f, 0.0f);
	AddEntry(TEXT("M_Whirlwind"), true, 100.0f, 50.0f);
	AddEntry(TEXT("M_SoulBarb"), false, 0.0f, 0.0f);
	const TArray<FWoTHUDDrawCmd> Commands =
		AWoTHUD::LayoutStatusIcons(Entries, 640);
	TestEqual(TEXT("each entry emits icon plus timer"), Commands.Num(), 8);
	if (Commands.Num() == 8)
	{
		TestTrue(TEXT("first good starts right of 32px health icon"),
			Commands[0].TexId == FName(TEXT("M_Levitation")) &&
			Commands[0].X == 48.0f && Commands[0].Y == 8.0f &&
			Commands[0].Scale == 0.5f);
		TestTrue(TEXT("first timer is masked at the same position"),
			Commands[1].TexId == FName(TEXT("T32_01")) &&
			Commands[1].X == 48.0f && Commands[1].Y == 8.0f &&
			Commands[1].Scale == 1.0f &&
			Commands[1].Style == EWoTHUDStyle::Masked);
		TestTrue(TEXT("first bad starts at the right edge"),
			Commands[2].X == 600.0f && Commands[2].Scale == 0.5f &&
			Commands[3].TexId == FName(TEXT("T32_19")));
		TestTrue(TEXT("second good advances left-to-right by 40"),
			Commands[4].X == 88.0f &&
			Commands[5].TexId == FName(TEXT("T32_10")));
		TestTrue(TEXT("second bad advances right-to-left by 40"),
			Commands[6].X == 560.0f &&
			Commands[7].TexId == FName(TEXT("T32_01")));
	}
	TestFalse(TEXT("BaseHUD default shows status icons"),
		GetDefault<AWoTHUD>()->bHideStatusIcons);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDSeekerCountSpec,
	"WoT.HUD.SeekerCount", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDSeekerCountSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->IncrementSeekerCount();
	Character->IncrementSeekerCount();
	Character->DecrementSeekerCount();
	TestEqual(TEXT("two increments and one decrement"), Character->SeekerCount, 1);
	Character->DecrementSeekerCount();
	Character->DecrementSeekerCount();
	TestEqual(TEXT("decrement clamps at zero"), Character->SeekerCount, 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDHealthLayoutSpec,
	"WoT.HUD.HealthLayout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDHealthLayoutSpec::RunTest(const FString&)
{
	FWoTHUDReadout Readout;
	Readout.bHasPawn = true;
	Readout.Health = 42;
	const TArray<FWoTHUDDrawCmd> Commands = AWoTHUD::LayoutHealth(
		Readout, FName(TEXT("H_AesSedai1")), true);
	TestEqual(TEXT("3 cmds"), Commands.Num(), 3);
	TestTrue(TEXT("icon"), Commands[0].TexId == FName(TEXT("H_AesSedai1")) &&
		Commands[0].X == 8.0f && Commands[0].Y == 8.0f);
	TestTrue(TEXT("seeker"), Commands[1].TexId == FName(TEXT("SeekerFrame")) &&
		Commands[1].Style == EWoTHUDStyle::Masked);
	TestTrue(TEXT("number"), Commands[2].Text == TEXT("42") &&
		Commands[2].FontId == FName(TEXT("F_Charge")) && Commands[2].X == 24.0f &&
		Commands[2].Y == 54.0f && Commands[2].bCenterX);

	FWoTHUDReadout NoSeekerReadout;
	NoSeekerReadout.bHasPawn = true;
	NoSeekerReadout.Health = 7;
	TestEqual(TEXT("no seeker -> 2 cmds"), AWoTHUD::LayoutHealth(
		NoSeekerReadout, FName(TEXT("H_AesSedai3")), false).Num(), 2);
	FWoTHUDReadout NoPawnReadout;
	NoPawnReadout.bHasPawn = false;
	TestEqual(TEXT("no pawn -> 0"), AWoTHUD::LayoutHealth(
		NoPawnReadout, NAME_None, false).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDFontsSpec,
	"WoT.HUD.Fonts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDFontsSpec::RunTest(const FString&)
{
	UWoTHUDFonts* Registry = LoadObject<UWoTHUDFonts>(nullptr,
		TEXT("/Game/WoT/UI/Fonts/DA_WoTHUDFonts.DA_WoTHUDFonts"));
	if (!TestNotNull(TEXT("HUD font registry loads"), Registry))
	{
		return false;
	}

	AWoTHUD* HUD = NewObject<AWoTHUD>();
	const FName FontIds[] = {
		FName(TEXT("F_Charge")),
		FName(TEXT("F_Key")),
		FName(TEXT("F_KeySelected")),
		FName(TEXT("F_WOTReg14")),
	};
	for (const FName FontId : FontIds)
	{
		UFont* RegisteredFont = Registry->Resolve(FontId);
		TestNotNull(*FString::Printf(TEXT("registry resolves %s"), *FontId.ToString()),
			RegisteredFont);

		UFont* NormalFont = HUD->ResolveFont(FontId, 512);
		TestNotNull(*FString::Printf(TEXT("HUD resolves %s"), *FontId.ToString()),
			NormalFont);
		if (NormalFont)
		{
			TestEqual(TEXT("normal-width font name"), NormalFont->GetFName(), FontId);
		}

		UFont* SmallFont = HUD->ResolveFont(FontId, 511);
		TestNotNull(*FString::Printf(TEXT("HUD resolves small %s"), *FontId.ToString()),
			SmallFont);
		if (SmallFont)
		{
			const FName ExpectedSmallId(
				*FString::Printf(TEXT("%s_S"), *FontId.ToString()));
			TestEqual(TEXT("sub-512 font name"), SmallFont->GetFName(), ExpectedSmallId);
		}
	}
	return true;
}

// The native HUD is wired straight as HUDClass on the game mode CDO.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDClassWiredSpec,
	"WoT.HUD.ClassWired", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDClassWiredSpec::RunTest(const FString&)
{
	TestEqual(TEXT("Arena game mode wires the debug HUD"),
		GetDefault<AWoTGameMode>()->HUDClass, TSubclassOf<AHUD>(AWoTHUD::StaticClass()));
	TestEqual(TEXT("Battle game mode inherits the debug HUD"),
		GetDefault<AWoTBattleGameMode>()->HUDClass, TSubclassOf<AHUD>(AWoTHUD::StaticClass()));
	return true;
}

// GatherReadout pulls every player-facing field off the local state, and FormatLines
// turns it into the on-screen rows — asserted here without ever touching a canvas.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDReadoutSpec,
	"WoT.HUD.Readout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDReadoutSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();   // create the AWoTPlayerState (login-flow shortcut)
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	PC->Possess(Pawn);
	HUD->PlayerOwner = PC;   // AHUD::GetOwningPlayerController() reads this

	// --- Populate the local state the HUD reads ---
	Pawn->Health = 42;

	AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>();
	if (!PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PS->Kills = 3;
	PS->Deaths = 1;
	PS->Team = 2;

	// Give the pawn a selected Dart at 7 charges (AddInventory auto-selects the first
	// angreal — the readout reads SelectedItem, COND_OwnerOnly = the local player's own).
	AWoTAngrealInvDart* Dart = World->SpawnActor<AWoTAngrealInvDart>(WoTHUDTests_AlwaysSpawnParams());
	UWoTInventoryComponent* Inv = Pawn->FindComponentByClass<UWoTInventoryComponent>();
	if (!Dart || !Inv)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->AddInventory(Dart);
	Dart->CurCharges = 7;   // after BeginPlay's SetCharges seed

	AWoTGameState* GS = World->GetGameState<AWoTGameState>();
	if (!GS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	GS->RemainingTime = 90.0f;      // 1:30
	GS->bDisplayRemaining = true;

	// --- Gather + assert ---
	const FWoTHUDReadout R = HUD->GatherReadout();
	FWoTHUDReadout SameReadout = R;
	TestTrue(TEXT("identical readouts compare equal"), R == SameReadout);
	SameReadout.Health++;
	TestFalse(TEXT("a rendered input invalidates the readout"), R == SameReadout);
	TestTrue(TEXT("has pawn"), R.bHasPawn);
	TestEqual(TEXT("health"), R.Health, 42);
	TestEqual(TEXT("max health"), R.MaxHealth, 100);
	TestTrue(TEXT("selected artifact is the Dart"), R.Artifact.Contains(TEXT("Dart")));
	TestEqual(TEXT("charges"), R.Charges, 7);
	TestTrue(TEXT("has score"), R.bHasScore);
	TestEqual(TEXT("kills"), R.Kills, 3);
	TestEqual(TEXT("deaths"), R.Deaths, 1);
	TestEqual(TEXT("team"), R.Team, 2);
	TestTrue(TEXT("display remaining"), R.bDisplayRemaining);
	TestEqual(TEXT("clock formats M:SS"), R.Clock, FString(TEXT("1:30")));
	TestFalse(TEXT("match not ended yet"), R.bMatchEnded);

	const FString Lines = FString::Join(AWoTHUD::FormatLines(R), TEXT("\n"));
	TestTrue(TEXT("health line"), Lines.Contains(TEXT("Health: 42/100")));
	TestTrue(TEXT("artifact line names the Dart"), Lines.Contains(TEXT("Dart")));
	TestTrue(TEXT("artifact line shows charges"), Lines.Contains(TEXT("x7")));
	TestTrue(TEXT("score line"), Lines.Contains(TEXT("3 / 1")));
	TestTrue(TEXT("clock line"), Lines.Contains(TEXT("1:30")));

	// --- Match decided: the owner-only bMatchEnded latch flips the banner ---
	Game->EndGame(TEXT("timelimit"));
	const FWoTHUDReadout REnded = HUD->GatherReadout();
	TestTrue(TEXT("match ended after EndGame"), REnded.bMatchEnded);
	const FString EndedLines = FString::Join(AWoTHUD::FormatLines(REnded), TEXT("\n"));
	TestTrue(TEXT("match-over banner shown"), EndedLines.Contains(TEXT("MATCH OVER")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Battle/citadel: the read-out surfaces the per-team seal counts + the win goal from
// the first placed BattleInfo (BattleInfo.NumSeals) and the CitadelGameState.SealGoal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDBattleSealsSpec,
	"WoT.HUD.BattleSeals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDBattleSealsSpec::RunTest(const FString&)
{
	UWorld* World = WoTHUDTests_CreateBattleWorld();

	// BattleInfo is placed map data — defer so NumTeams/TeamClass are set before its
	// BeginPlay validation (the map-placement path, per WoTInventoryTests).
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	if (!BI)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	BI->IncrementTeamSealCount(1);
	BI->IncrementTeamSealCount(1);   // team 1 holds 2 seals

	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	PC->Possess(Pawn);
	HUD->PlayerOwner = PC;
	if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>())
	{
		PS->Team = 1;
	}

	const FWoTHUDReadout R = HUD->GatherReadout();
	TestTrue(TEXT("has seals (Battle map)"), R.bHasSeals);
	TestEqual(TEXT("team count"), R.NumTeams, 2);
	TestEqual(TEXT("team 1 seal count"), R.Seals[1], 2);
	TestEqual(TEXT("seal goal from citadel game state"), R.SealGoal, 4);

	const FString Lines = FString::Join(AWoTHUD::FormatLines(R), TEXT("\n"));
	TestTrue(TEXT("seal line present"), Lines.Contains(TEXT("Seals:")));
	TestTrue(TEXT("seal line shows team 1 = 2"), Lines.Contains(TEXT("T1=2")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDHandBarSpec,
	"WoT.HUD.HandBar", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDHandBarSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvFireworks* Fireworks = World->SpawnActor<AWoTAngrealInvFireworks>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvAirBurst* AirBurst = World->SpawnActor<AWoTAngrealInvAirBurst>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Fireball || !Fireworks || !AirBurst)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Fireball->CurCharges = 12;
	Fireworks->CurCharges = 7;

	UWoTHandSet* HandSet = NewObject<UWoTHandSet>(World);
	HandSet->Init(World);
	UWoTHandInfo* FirstHand = HandSet->GetHand(0);
	FirstHand->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	FirstHand->AddClassName(FName(TEXT("AngrealInvFireworks")), 3);
	FirstHand->bHaveThis[0] = true;
	FirstHand->bHaveThis[3] = true;
	FirstHand->Item[0] = Fireball;
	FirstHand->Item[3] = Fireworks;
	FirstHand->Selected = 3;
	UWoTHandInfo* SecondHand = HandSet->GetHand(1);
	SecondHand->AddClassName(FName(TEXT("AngrealInvAirBurst")), 0);
	SecondHand->bHaveThis[0] = true;
	SecondHand->Item[0] = AirBurst;
	SecondHand->Selected = 0;
	SecondHand->ItemAdded = FName(TEXT("AngrealInvAirBurst"));
	SecondHand->ItemAddedTime = 10.0f;
	HandSet->Selected = 1;

	const TArray<FWoTHUDDrawCmd> Commands =
		AWoTHUD::LayoutHands(HandSet, 640, 480);
	TestEqual(TEXT("two populated hand icons"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("I_Fireball"))) +
		WoTHUDTests_CountTexture(Commands, FName(TEXT("I_AirBurst"))), 2);
	TestEqual(TEXT("selected hand highlight"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("StatusIconSelected"))), 1);
	TestEqual(TEXT("one dim notch"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("D_Notch"))), 1);
	TestEqual(TEXT("one selected notch per populated hand"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("I_Notch"))), 2);

	bool bSelectedNumber = false;
	bool bFiniteCharge = false;
	bool bInfiniteCharge = false;
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		bSelectedNumber |= Command.Text == TEXT("2") &&
			Command.FontId == FName(TEXT("F_KeySelected")) &&
			Command.X == 65.0f && Command.Y == 436.0f;
		bFiniteCharge |= Command.Text == TEXT("7") &&
			Command.FontId == FName(TEXT("F_Charge")) &&
			Command.X == 34.0f && Command.Y == 467.0f && Command.bCenterX;
		bInfiniteCharge |= Command.Text == TEXT(":") &&
			Command.FontId == FName(TEXT("F_Charge")) &&
			Command.X == 98.0f && Command.Y == 467.0f && Command.bCenterX;
	}
	TestTrue(TEXT("selected hand uses F_KeySelected at shipped coords"), bSelectedNumber);
	TestTrue(TEXT("finite charge count uses F_Charge at shipped coords"), bFiniteCharge);
	TestTrue(TEXT("zero-cost angreal uses infinite-charge colon"), bInfiniteCharge);

	const TArray<FWoTHUDDrawCmd> Added =
		AWoTHUD::LayoutAddedItems(HandSet, 640, 480, 10.75f);
	TestEqual(TEXT("recent item slide emits one frame"), Added.Num(), 1);
	if (Added.Num() == 1)
	{
		TestTrue(TEXT("added frame and 50% scale"),
			Added[0].TexId == FName(TEXT("M_AirBurst")) && Added[0].Scale == 0.5f);
		TestEqual(TEXT("added X"), Added[0].X, 80.0f);
		TestEqual(TEXT("added Y halfway through slide"), Added[0].Y, 400.0f);
	}
	TestEqual(TEXT("expired added item hidden"),
		AWoTHUD::LayoutAddedItems(HandSet, 640, 480, 11.51f).Num(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDTaintedHandSpec,
	"WoT.HUD.TaintedHand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDTaintedHandSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Fireball)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Fireball->bTainted = true;
	UWoTHandSet* HandSet = NewObject<UWoTHandSet>(World);
	HandSet->Init(World);
	UWoTHandInfo* Hand = HandSet->GetHand(0);
	Hand->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	Hand->bHaveThis[0] = true;
	Hand->Item[0] = Fireball;
	Hand->Selected = 0;
	HandSet->Selected = 0;

	const TArray<FWoTHUDDrawCmd> Commands =
		AWoTHUD::LayoutHands(HandSet, 640, 480);
	TestEqual(TEXT("tainted live item emits one overlay"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("I_Tainted"))), 1);
	const FWoTHUDDrawCmd* Tainted = Commands.FindByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == FName(TEXT("I_Tainted"));
		});
	if (TestNotNull(TEXT("tainted overlay command"), Tainted))
	{
		TestTrue(TEXT("tainted overlay is masked at the item icon position"),
			Tainted->X == 0.0f && Tainted->Y == 416.0f &&
			Tainted->Style == EWoTHUDStyle::Masked);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDLiveNullChargeSpec,
	"WoT.HUD.LiveNullCharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDLiveNullChargeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTHandSet* HandSet = NewObject<UWoTHandSet>(World);
	HandSet->Init(World);
	UWoTHandInfo* Hand = HandSet->GetHand(0);
	Hand->AddClassName(FName(TEXT("AngrealInvAirBurst")), 0);
	Hand->bHaveThis[0] = true;
	Hand->Item[0] = nullptr;
	Hand->Selected = 0;
	HandSet->Selected = 0;

	// AirBurst's CDO is zero-cost. It supplies the unresolved slot's icon, but only
	// a live instance may supply the colon/count readout; shipped displays "0" here.
	const TArray<FWoTHUDDrawCmd> Commands =
		AWoTHUD::LayoutHands(HandSet, 640, 480);
	TestEqual(TEXT("class-default AirBurst icon remains visible"),
		WoTHUDTests_CountTexture(Commands, FName(TEXT("I_AirBurst"))), 1);
	bool bChargeIsZero = false;
	bool bChargeIsInfinite = false;
	for (const FWoTHUDDrawCmd& Command : Commands)
	{
		if (Command.FontId == FName(TEXT("F_Charge")))
		{
			bChargeIsZero |= Command.Text == TEXT("0");
			bChargeIsInfinite |= Command.Text == TEXT(":");
		}
	}
	TestTrue(TEXT("live-null AirBurst charge text is zero"), bChargeIsZero);
	TestFalse(TEXT("live-null AirBurst does not inherit the CDO's infinite charge"),
		bChargeIsInfinite);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDFirstPickupCurrentHandSpec,
	"WoT.HUD.FirstPickupCurrentHand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDFirstPickupCurrentHandSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		WoTHUDTests_AlwaysSpawnParams());
	if (!PlayerController || !Character || !Fireball)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);
	PlayerController->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PlayerController->GetHUD());
	UWoTInventoryComponent* Inventory = Character->InventoryComponent;
	if (!TestNotNull(TEXT("local controller owns the WoT HUD"), HUD) ||
		!TestNotNull(TEXT("character owns inventory component"), Inventory))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float BeforePickup = World->GetTimeSeconds();
	TestFalse(TEXT("current-hand window starts closed"),
		HUD->IsCurrentHandDisplayActive(BeforePickup));
	Inventory->AddInventory(Fireball);
	const float AfterPickup = World->GetTimeSeconds();
	TestEqual(TEXT("first selectable pickup becomes selected"),
		Inventory->SelectedItem.Get(), static_cast<AWoTInventory*>(Fireball));
	TestTrue(TEXT("first-pickup selection opens the current-hand window"),
		HUD->IsCurrentHandDisplayActive(AfterPickup));
	TestFalse(TEXT("current-hand window closes after the shipped 1.5 seconds"),
		HUD->IsCurrentHandDisplayActive(AfterPickup + 1.51f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDCurrentHandSpec,
	"WoT.HUD.CurrentHand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDCurrentHandSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvFireworks* Fireworks = World->SpawnActor<AWoTAngrealInvFireworks>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Fireball || !Fireworks)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTHandSet* HandSet = NewObject<UWoTHandSet>(World);
	HandSet->Init(World);
	UWoTHandInfo* Hand = HandSet->GetHand(0);
	Hand->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	Hand->AddClassName(FName(TEXT("AngrealInvFireworks")), 3);
	Hand->bHaveThis[0] = true;
	Hand->bHaveThis[3] = true;
	Hand->Item[0] = Fireball;
	Hand->Item[3] = Fireworks;
	Hand->Selected = 3;
	HandSet->Selected = 0;

	const TArray<FWoTHUDDrawCmd> Commands =
		AWoTHUD::LayoutCurrentHand(HandSet, 640, 480);
	TestEqual(TEXT("two frames plus selected overlay"), Commands.Num(), 3);
	if (Commands.Num() == 3)
	{
		TestTrue(TEXT("first item at 50%"), Commands[0].TexId == FName(TEXT("M_Fireball")) &&
			Commands[0].X == 0.0f && Commands[0].Y == 384.0f && Commands[0].Scale == 0.5f);
		TestTrue(TEXT("second item at dense half-icon spacing"),
			Commands[1].TexId == FName(TEXT("M_Fireball")) &&
			Commands[1].X == 32.0f && Commands[1].Scale == 0.5f);
		TestTrue(TEXT("selected slot overlay"),
			Commands[2].TexId == FName(TEXT("StatusIconFrame")) &&
			Commands[2].X == 32.0f && Commands[2].Style == EWoTHUDStyle::Masked);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDHandInputSpec,
	"WoT.HUD.HandInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDHandInputSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	if (!Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UInputComponent* Input = NewObject<UInputComponent>(Character);
	if (!Input)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->SetupPlayerInputComponent(Input);
	Character->AngrealHandSet = NewObject<UWoTHandSet>(Character);
	Character->AngrealHandSet->Init(Character);
	UWoTHandInfo* HandZero = Character->AngrealHandSet->GetHand(0);
	HandZero->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	HandZero->bHaveThis[0] = true;
	UWoTHandInfo* HandNine = Character->AngrealHandSet->GetHand(9);
	HandNine->AddClassName(FName(TEXT("AngrealInvSpecial")), 0);
	HandNine->bHaveThis[0] = true;
	TSet<FName> BoundActions;
	bool bExecutedZero = false;
	for (int32 Index = 0; Index < Input->GetNumActionBindings(); ++Index)
	{
		FInputActionBinding& Binding = Input->GetActionBinding(Index);
		BoundActions.Add(Binding.GetActionName());
		if (Binding.GetActionName() == FName(TEXT("SelectHand0")))
		{
			Binding.ActionDelegate.Execute(EKeys::Zero);
			bExecutedZero = true;
		}
	}
	TestTrue(TEXT("0 action delegate executed"), bExecutedZero);
	TestEqual(TEXT("0 action selects hand index 9"),
		Character->AngrealHandSet->Selected, 9);
	TestTrue(TEXT("wheel next-item action bound"),
		BoundActions.Contains(FName(TEXT("NextItem"))));
	TestTrue(TEXT("wheel prev-item action bound"),
		BoundActions.Contains(FName(TEXT("PrevItem"))));
	TestFalse(TEXT("flat next inventory no longer player-bound"),
		BoundActions.Contains(FName(TEXT("NextInventory"))));
	TestFalse(TEXT("flat prev inventory no longer player-bound"),
		BoundActions.Contains(FName(TEXT("PrevInventory"))));
	for (int32 Digit = 0; Digit < 10; ++Digit)
	{
		const FName Action(*FString::Printf(TEXT("SelectHand%d"), Digit));
		TestTrue(*FString::Printf(TEXT("%s bound"), *Action.ToString()),
			BoundActions.Contains(Action));
	}

	const UInputSettings* Settings = GetDefault<UInputSettings>();
	auto HasMapping = [Settings](FName Action, FKey Key)
	{
		TArray<FInputActionKeyMapping> Mappings;
		Settings->GetActionMappingByName(Action, Mappings);
		return Mappings.ContainsByPredicate([Key](const FInputActionKeyMapping& Mapping)
		{
			return Mapping.Key == Key;
		});
	};
	TestTrue(TEXT("scroll down maps NextItem"),
		HasMapping(FName(TEXT("NextItem")), EKeys::MouseScrollDown));
	TestTrue(TEXT("scroll up maps PrevItem"),
		HasMapping(FName(TEXT("PrevItem")), EKeys::MouseScrollUp));
	TestTrue(TEXT("1 maps hand zero action"),
		HasMapping(FName(TEXT("SelectHand1")), EKeys::One));
	TestTrue(TEXT("0 maps hand nine action"),
		HasMapping(FName(TEXT("SelectHand0")), EKeys::Zero));

	// The net harness API remains a concrete callable surface even though no player
	// action maps to it.
	void (AWoTCharacter::*NextFlat)() = &AWoTCharacter::NextInventory;
	void (AWoTCharacter::*PrevFlat)() = &AWoTCharacter::PrevInventory;
	TestTrue(TEXT("flat inventory harness methods retained"), NextFlat && PrevFlat);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDMessagesSpec,
	"WoT.HUD.Messages", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDMessagesSpec::RunTest(const FString&)
{
	AWoTHUD* HUD = NewObject<AWoTHUD>();
	if (!TestNotNull(TEXT("HUD"), HUD))
	{
		return false;
	}

	// BaseHUD.defaultproperties values, including the shipped dynamic sound names.
	TestEqual(TEXT("hand duration"), HUD->HandMessageDuration, 1.5f);
	TestEqual(TEXT("left duration"), HUD->LeftMessageDuration, 10.0f);
	TestEqual(TEXT("right duration"), HUD->RightMessageDuration, 10.0f);
	TestEqual(TEXT("center duration"), HUD->CenterMessageDuration, 5.0f);
	TestEqual(TEXT("subtitle duration"), HUD->SubtitleMessageDuration, 5.0f);
	TestEqual(TEXT("generic duration"), HUD->GenericMessageDuration, 5.0f);
	TestEqual(TEXT("hand fade"), HUD->HandMessageFadePercent, 0.5f);
	TestEqual(TEXT("left fade"), HUD->LeftMessageFadePercent, 0.2f);
	TestEqual(TEXT("center fade"), HUD->CenterMessageFadePercent, 0.25f);
	TestEqual(TEXT("all default intensities are full"),
		static_cast<int32>(HUD->GenericMessageIntensity), 255);
	TestEqual(TEXT("left notify name"), HUD->LeftMessageName,
		FString(TEXT("WOT.Interface.LeftNotify")));
	TestEqual(TEXT("right notify name"), HUD->RightMessageName,
		FString(TEXT("WOT.Interface.RightNotify")));
	// Keep the rest of this spec strictly state-only; sound asset resolution/playback
	// is the thin runtime side effect around the queue mutation.
	HUD->LeftMessageName.Reset();
	HUD->RightMessageName.Reset();

	for (int32 Index = 0; Index < 11; ++Index)
	{
		HUD->AddLeftMessage(FString::Printf(TEXT("L%d"), Index),
			static_cast<float>(Index + 1));
	}
	TestEqual(TEXT("left newest is slot zero"), HUD->LeftMessages[0].Message,
		FString(TEXT("L10")));
	TestEqual(TEXT("left newest custom lifespan"), HUD->LeftMessages[0].LifeSpan, 11.0f);
	TestEqual(TEXT("left oldest retained after overflow"), HUD->LeftMessages[9].Message,
		FString(TEXT("L1")));

	HUD->AddRightMessage(TEXT("right"));
	TestEqual(TEXT("right default lifespan"), HUD->RightMessages[0].LifeSpan,
		HUD->RightMessageDuration);
	HUD->AddHandMessage(TEXT("hand"), 0.25f);
	TestEqual(TEXT("hand replaces singleton"), HUD->HandMessage.Message,
		FString(TEXT("hand")));
	HUD->AddCenterMessage(TEXT("older"), 3.0f);
	HUD->AddCenterMessage(TEXT("newer"), 4.0f);
	TestTrue(TEXT("center slides two entries"),
		HUD->CenterMessages[0].Message == TEXT("newer") &&
		HUD->CenterMessages[1].Message == TEXT("older"));

	// AddSubtitleMessage shipped with CenterMessageDuration as its fallback even
	// though DrawMessages uses SubtitleMessageDuration for its fade calculation.
	HUD->CenterMessageDuration = 6.0f;
	HUD->SubtitleMessageDuration = 9.0f;
	HUD->AddSubtitleMessage(TEXT("subtitle"));
	TestEqual(TEXT("subtitle shipped fallback duration"),
		HUD->SubtitleMessage.LifeSpan, 6.0f);
	UFont* GenericFont = NewObject<UFont>(HUD);
	HUD->AddGenericMessage(TEXT("generic"), 123, 45, true, 91, GenericFont, 2.0f);
	TestTrue(TEXT("generic stores positioned per-message data"),
		HUD->GenericMessages[0].Message == TEXT("generic") &&
		HUD->GenericMessages[0].X == 123 && HUD->GenericMessages[0].Y == 45 &&
		HUD->GenericMessages[0].bCenter && HUD->GenericMessages[0].Intensity == 91 &&
		HUD->GenericMessages[0].Font == GenericFont &&
		HUD->GenericMessages[0].LifeSpan == 2.0f);
	TestEqual(TEXT("centered generic anchors between scaled X and right edge"),
		AWoTHUD::GenericMessageCenterX(123.0f, 640.0f), 381.5f);

	const auto MeasureCharacters = [](const FString& Text)
	{
		return static_cast<float>(Text.Len());
	};
	const TArray<FString> SplitTokenLines = AWoTHUD::WrapMessageLines(
		TEXT("abcdefghij"), 4.0f, MeasureCharacters);
	TestEqual(TEXT("over-long subtitle token splits across lines"),
		SplitTokenLines.Num(), 3);
	FString RejoinedToken;
	bool bEverySplitLineFits = true;
	for (const FString& Line : SplitTokenLines)
	{
		RejoinedToken += Line;
		bEverySplitLineFits &= MeasureCharacters(Line) <= 4.0f;
	}
	TestTrue(TEXT("split subtitle lines remain within the clip width"),
		bEverySplitLineFits);
	TestEqual(TEXT("token splitting preserves every character"), RejoinedToken,
		FString(TEXT("abcdefghij")));

	const TArray<FString> ForcedBreakLines = AWoTHUD::WrapMessageLines(
		TEXT("alpha  beta\tgamma\nnext  line"), 100.0f, MeasureCharacters);
	TestTrue(TEXT("subtitle forced break and interior whitespace are preserved"),
		ForcedBreakLines.Num() == 2 &&
		ForcedBreakLines[0] == TEXT("alpha  beta\tgamma") &&
		ForcedBreakLines[1] == TEXT("next  line"));

	// UE1 consumes the whitespace straddling a wrap boundary: a continuation line
	// never begins with the spaces that forced the wrap (interior whitespace that
	// fits, as above, is untouched).
	const TArray<FString> BoundarySkipLines = AWoTHUD::WrapMessageLines(
		TEXT("aaa  bbb"), 4.0f, MeasureCharacters);
	TestTrue(TEXT("wrap-boundary whitespace run is skipped, not carried"),
		BoundarySkipLines.Num() == 2 && BoundarySkipLines[0] == TEXT("aaa") &&
		BoundarySkipLines[1] == TEXT("bbb"));
	const TArray<FString> ExactFitLines = AWoTHUD::WrapMessageLines(
		TEXT("aaaa bbb"), 4.0f, MeasureCharacters);
	TestTrue(TEXT("continuation after an exact-fit word starts at the word"),
		ExactFitLines.Num() == 2 && ExactFitLines[0] == TEXT("aaaa") &&
		ExactFitLines[1] == TEXT("bbb"));

	HUD->TickMessages(0.25f);
	TestEqual(TEXT("hand expires and clamps at zero"), HUD->HandMessage.LifeSpan, 0.0f);
	TestEqual(TEXT("generic lifespan decrements"),
		HUD->GenericMessages[0].LifeSpan, 1.75f);
	HUD->TickMessages(100.0f);
	TestTrue(TEXT("center subtitle and generic entries all expire"),
		HUD->CenterMessages[0].LifeSpan == 0.0f &&
		HUD->CenterMessages[1].LifeSpan == 0.0f &&
		HUD->SubtitleMessage.LifeSpan == 0.0f &&
		HUD->GenericMessages[0].LifeSpan == 0.0f);
	TestEqual(TEXT("fade boundary remains full"),
		AWoTHUD::MessageFadeScale(2.0f, 10.0f, 0.2f), 1.0f);
	TestEqual(TEXT("halfway through fade tail"),
		AWoTHUD::MessageFadeScale(1.0f, 10.0f, 0.2f), 0.5f);
	TestEqual(TEXT("expired fade is zero"),
		AWoTHUD::MessageFadeScale(0.0f, 10.0f, 0.2f), 0.0f);

	TestTrue(TEXT("visible HUD draws messages"), HUD->ShouldDrawMessages(true));
	HUD->bHideMessages = true;
	TestFalse(TEXT("bHideMessages gates drawing"), HUD->ShouldDrawMessages(true));
	HUD->bHideMessages = false;
	TestFalse(TEXT("outer HUD visibility also gates messages"),
		HUD->ShouldDrawMessages(false));
	TestEqual(TEXT("hidden outer HUD leaves the message timestamp untouched"),
		HUD->UpdateMessageRenderTime(false, 2.0f), 0.0f);
	TestEqual(TEXT("first visible frame catches up outer-hidden elapsed time"),
		HUD->UpdateMessageRenderTime(true, 5.0f), 5.0f);
	HUD->bHideMessages = true;
	TestEqual(TEXT("message-only gate still advances the eligible timestamp"),
		HUD->UpdateMessageRenderTime(true, 8.0f), 3.0f);
	HUD->bHideMessages = false;
	TestEqual(TEXT("message-only gate pauses lifespan instead of catching up"),
		HUD->UpdateMessageRenderTime(true, 9.0f), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDD6MenuVisibilitySpec,
	"WoT.HUD.D6.MenuVisibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDD6MenuVisibilitySpec::RunTest(const FString&)
{
	TestTrue(TEXT("ordinary gameplay draws the HUD"),
		AWoTHUD::ShouldDrawHUD(true, false, false));
	TestFalse(TEXT("the owning open menu suppresses the HUD"),
		AWoTHUD::ShouldDrawHUD(true, false, true));
	TestFalse(TEXT("bShowHUD remains a gate"),
		AWoTHUD::ShouldDrawHUD(false, false, false));
	TestFalse(TEXT("seamless travel remains a gate"),
		AWoTHUD::ShouldDrawHUD(true, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDD11InterfaceLevelsSpec,
	"WoT.HUD.D11.InterfaceLevels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDD11InterfaceLevelsSpec::RunTest(const FString&)
{
	TestEqual(TEXT("ViewUp grows normal to minimized"),
		static_cast<int32>(AWoTHUD::NextInterfaceLevel(EWoTInterfaceLevel::Normal)),
		static_cast<int32>(EWoTInterfaceLevel::Minimized));
	TestEqual(TEXT("ViewUp grows minimized to ultra"),
		static_cast<int32>(AWoTHUD::NextInterfaceLevel(EWoTInterfaceLevel::Minimized)),
		static_cast<int32>(EWoTInterfaceLevel::UltraMinimized));
	TestEqual(TEXT("ViewUp clamps at retail MaxLevels minus one"),
		static_cast<int32>(AWoTHUD::NextInterfaceLevel(
			EWoTInterfaceLevel::UltraMinimized)),
		static_cast<int32>(EWoTInterfaceLevel::UltraMinimized));
	TestEqual(TEXT("ViewDown shrinks ultra to minimized"),
		static_cast<int32>(AWoTHUD::PreviousInterfaceLevel(
			EWoTInterfaceLevel::UltraMinimized)),
		static_cast<int32>(EWoTInterfaceLevel::Minimized));
	TestEqual(TEXT("ViewDown clamps at normal"),
		static_cast<int32>(AWoTHUD::PreviousInterfaceLevel(EWoTInterfaceLevel::Normal)),
		static_cast<int32>(EWoTInterfaceLevel::Normal));

	for (EWoTInterfaceLevel Level : {EWoTInterfaceLevel::Normal,
		EWoTInterfaceLevel::Minimized, EWoTInterfaceLevel::UltraMinimized})
	{
		const FWoTHUDSurfaceVisibility Visible =
			AWoTHUD::SurfaceVisibility(Level, false);
		TestTrue(TEXT("health stays visible at every InterfaceLevel"), Visible.bHealth);
		TestTrue(TEXT("status stays visible at every InterfaceLevel"), Visible.bStatusIcons);
		TestTrue(TEXT("keys stay visible at every InterfaceLevel"), Visible.bKeys);
		TestTrue(TEXT("messages stay visible at every InterfaceLevel"), Visible.bMessages);
		TestTrue(TEXT("progress stays visible at every InterfaceLevel"), Visible.bProgress);
	}
	TestTrue(TEXT("normal level permits the added-item slide"),
		AWoTHUD::SurfaceVisibility(EWoTInterfaceLevel::Normal, false).bAddedItems);
	TestFalse(TEXT("minimized suppresses the added-item slide"),
		AWoTHUD::SurfaceVisibility(EWoTInterfaceLevel::Minimized, false).bAddedItems);
	TestFalse(TEXT("ultra suppresses the added-item slide"),
		AWoTHUD::SurfaceVisibility(EWoTInterfaceLevel::UltraMinimized, false).bAddedItems);

	const FWoTHUDSurfaceVisibility ItemWindow =
		AWoTHUD::SurfaceVisibility(EWoTInterfaceLevel::Minimized, true);
	TestFalse(TEXT("item window hides health"), ItemWindow.bHealth);
	TestFalse(TEXT("item window hides status"), ItemWindow.bStatusIcons);
	TestFalse(TEXT("item window hides keys"), ItemWindow.bKeys);
	TestFalse(TEXT("item window hides ordinary messages"), ItemWindow.bMessages);
	TestTrue(TEXT("item window leaves hands visible"), ItemWindow.bHands);
	TestTrue(TEXT("item window leaves progress visible"), ItemWindow.bProgress);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDD11MinimizedHandsSpec,
	"WoT.HUD.D11.MinimizedHands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDD11MinimizedHandsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvAirBurst* AirBurst = World->SpawnActor<AWoTAngrealInvAirBurst>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	Fireball->CurCharges = 12;
	UWoTHandSet* HandSet = NewObject<UWoTHandSet>(World);
	HandSet->Init(World);
	UWoTHandInfo* First = HandSet->GetHand(0);
	First->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	First->bHaveThis[0] = true;
	First->Item[0] = Fireball;
	First->Selected = 0;
	UWoTHandInfo* Second = HandSet->GetHand(1);
	Second->AddClassName(FName(TEXT("AngrealInvAirBurst")), 0);
	Second->bHaveThis[0] = true;
	Second->Item[0] = AirBurst;
	Second->Selected = 0;
	HandSet->Selected = 0;

	const TArray<FWoTHUDDrawCmd> Minimized = AWoTHUD::LayoutMinimizedHands(
		HandSet, 640, 480, EWoTInterfaceLevel::Minimized,
		false, 10.0f, 10.0f, 1.5f);
	const FWoTHUDDrawCmd* CenterIcon = Minimized.FindByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == FName(TEXT("I_Fireball"))
				&& Command.Scale == 1.0f;
		});
	if (TestNotNull(TEXT("minimized draws the selected item icon"), CenterIcon))
	{
		TestTrue(TEXT("selected item is bottom centered"),
			CenterIcon->X == 288.0f && CenterIcon->Y == 416.0f);
	}
	TestEqual(TEXT("minimized draws both populated hand frames"),
		WoTHUDTests_CountTexture(Minimized, FName(TEXT("M_Fireball"))) +
		WoTHUDTests_CountTexture(Minimized, FName(TEXT("M_AirBurst"))), 2);
	TestTrue(TEXT("minimized uses the selected hand digit"),
		Minimized.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{
			return Command.Text == TEXT("1") &&
				Command.FontId == FName(TEXT("F_KeySelected")) &&
				Command.X == 289.0f && Command.Y == 436.0f;
		}));
	TestTrue(TEXT("minimized uses the live charge count"),
		Minimized.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{
			return Command.Text == TEXT("12") &&
				Command.FontId == FName(TEXT("F_Charge")) &&
				Command.X == 322.0f && Command.Y == 467.0f;
		}));

	const TArray<FWoTHUDDrawCmd> UltraIdle = AWoTHUD::LayoutMinimizedHands(
		HandSet, 640, 480, EWoTInterfaceLevel::UltraMinimized,
		false, 10.0f, 10.0f, 1.5f);
	TestEqual(TEXT("ultra idle suppresses the small hand frames"),
		WoTHUDTests_CountTexture(UltraIdle, FName(TEXT("M_Fireball"))) +
		WoTHUDTests_CountTexture(UltraIdle, FName(TEXT("M_AirBurst"))), 0);
	TestEqual(TEXT("ultra still draws the central selected icon"),
		WoTHUDTests_CountTexture(UltraIdle, FName(TEXT("I_Fireball"))), 1);

	const TArray<FWoTHUDDrawCmd> UltraActive = AWoTHUD::LayoutMinimizedHands(
		HandSet, 640, 480, EWoTInterfaceLevel::UltraMinimized,
		true, 10.75f, 10.0f, 1.5f);
	const FWoTHUDDrawCmd* SlidingFrame = UltraActive.FindByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == FName(TEXT("M_Fireball"));
		});
	if (TestNotNull(TEXT("active ultra briefly draws the small frames"), SlidingFrame))
	{
		TestEqual(TEXT("ultra frame slides halfway offscreen"), SlidingFrame->Y, 464.0f);
	}
	const TArray<FWoTHUDDrawCmd> Current = AWoTHUD::LayoutCurrentHand(
		HandSet, 640, 480, EWoTInterfaceLevel::Minimized);
	if (TestTrue(TEXT("minimized current-hand row has content"), !Current.IsEmpty()))
	{
		TestTrue(TEXT("current-hand row moves above the central tile"),
			Current[0].X == 304.0f && Current[0].Y == 384.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDD12ItemInfoSpec,
	"WoT.HUD.D12.ItemInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDD12ItemInfoSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping item-info text checks"));
		return true;
	}
	const FString* CommonRarity = FWoTRetailText::FindSectionValue(
		TEXT("InventoryInfoWindow"), TEXT("RarityCommonStr"));
	if (!TestNotNull(TEXT("common rarity text is loaded"), CommonRarity))
	{
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Character);
	Fireball->CurCharges = 11;
	UWoTHandSet* FixtureHands = NewObject<UWoTHandSet>(Character);
	Character->CurrentHandSet = FixtureHands;
	Character->CurrentHandSet->Init(Character);
	UWoTHandInfo* Hand = Character->CurrentHandSet->GetHand(0);
	Hand->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	Hand->bHaveThis[0] = true;
	Hand->Item[0] = Fireball;
	Hand->Selected = 0;
	Character->CurrentHandSet->Selected = 0;

	const FWoTItemInfoContent Content = FWoTItemInfoContent::FromItem(Fireball);
	TestEqual(TEXT("title comes from the selected item"), Content.Title, Fireball->Title);
	TestEqual(TEXT("icon comes from StatusIcon"), Content.Icon.ToString(),
		Fireball->StatusIcon.ToString());
	TestEqual(TEXT("common rarity is localized retail copy"),
		Content.Rarity, *CommonRarity);
	TestEqual(TEXT("charge line uses the live count"),
		Content.Charges, FString(TEXT("Charges:  11")));
	TestEqual(TEXT("all five element entries exist"), Content.Elements.Num(), 5);
	if (Content.Elements.Num() == 5)
	{
		TestEqual(TEXT("retail gem order 0"), Content.Elements[0].Label,
			FString(TEXT("EARTH")));
		TestEqual(TEXT("retail gem order 1"), Content.Elements[1].Label,
			FString(TEXT("AIR")));
		TestEqual(TEXT("retail gem order 2"), Content.Elements[2].Label,
			FString(TEXT("FIRE")));
		TestEqual(TEXT("retail gem order 3"), Content.Elements[3].Label,
			FString(TEXT("WATER")));
		TestEqual(TEXT("retail gem order 4"), Content.Elements[4].Label,
			FString(TEXT("SPIRIT")));
		TestTrue(TEXT("Fireball lights FIRE"), Content.Elements[2].bActive);
		TestFalse(TEXT("Fireball leaves WATER off"), Content.Elements[3].bActive);
	}
	TestEqual(TEXT("description is content-faithful"), Content.Description,
		Fireball->Description);
	TestEqual(TEXT("quote is content-faithful"), Content.Quote, Fireball->Quote);
	const TSharedRef<FWoTRequiredMenuFontGeneration> FontGeneration =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	const TSharedRef<FWoTRequiredMenuFonts> ValidFonts =
		MakeShared<FWoTRequiredMenuFonts>();
	ValidFonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	ValidFonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	ValidFonts->Generation = FontGeneration;
	TestTrue(TEXT("item-info font fixture is valid"), ValidFonts->IsValid());
	TSharedRef<SWoTItemInfoOverlay> Overlay = SNew(SWoTItemInfoOverlay)
		.PlayerController(Controller).Item(Fireball)
		.RequiredFontsForTesting(ValidFonts);
	TestEqual(TEXT("font-ready item overlay is globally hit-test invisible"),
		SWoTItemInfoOverlay::VisibilityForFonts(true),
		EVisibility::HitTestInvisible);
	TestEqual(TEXT("font-revoked item overlay collapses"),
		SWoTItemInfoOverlay::VisibilityForFonts(false), EVisibility::Collapsed);
	TestEqual(TEXT("constructed font-ready overlay is exactly hit-test invisible"),
		Overlay->GetVisibility(), EVisibility::HitTestInvisible);
	FontGeneration->RevokeForTesting();
	TestEqual(TEXT("revoking constructed overlay fonts collapses the root"),
		Overlay->GetVisibility(), EVisibility::Collapsed);
	const TSharedRef<FWoTRequiredMenuFonts> MissingFonts =
		MakeShared<FWoTRequiredMenuFonts>();
	TSharedRef<SWoTItemInfoOverlay> MissingFontOverlay =
		SNew(SWoTItemInfoOverlay).PlayerController(Controller).Item(Fireball)
		.RequiredFontsForTesting(MissingFonts);
	TestEqual(TEXT("constructed missing-font overlay is collapsed"),
		MissingFontOverlay->GetVisibility(), EVisibility::Collapsed);

	Controller->ShowInventoryInfo();
	TestTrue(TEXT("F2 opens selected-item info"), Controller->IsItemInfoOpen());
	Controller->ShowInventoryInfo();
	TestFalse(TEXT("F2 toggles the active info window closed"),
		Controller->IsItemInfoOpen());
	Controller->ShowInventoryInfo();
	Hand->Item[0] = nullptr;
	Controller->TickItemInfoLifecycle();
	TestFalse(TEXT("an invalid live selection fully closes item info"),
		Controller->IsItemInfoOpen());
	Hand->Item[0] = Fireball;
	Controller->ShowInventoryInfo();
	Controller->PreClientTravel(
		TEXT("127.0.0.1/Game/Maps/Arena_01?password=secret"),
		TRAVEL_Absolute, false);
	TestFalse(TEXT("travel fully closes item info"), Controller->IsItemInfoOpen());
	Controller->ShowInventoryInfo();
	Controller->UnPossess();
	TestFalse(TEXT("death/unpossess fully closes item info"),
		Controller->IsItemInfoOpen());
	TestTrue(TEXT("closed death item info no longer suppresses messages"),
		AWoTHUD::SurfaceVisibility(EWoTInterfaceLevel::Normal,
			Controller->IsItemInfoOpen()).bMessages);

	Controller->Possess(Character);
	// Possession re-runs EnsureAngrealHands, which swaps CurrentHandSet to the
	// reconciled (inventory-derived, empty here) AngrealHandSet — restore the
	// fixture's hand-crafted set before reopening.
	Character->CurrentHandSet = FixtureHands;
	Character->CurrentHandSet->Selected = 0;
	Controller->ShowInventoryInfo();
	AWoTCharacter* ReplacementCharacter = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHUDTests_AlwaysSpawnParams());
	ReplacementCharacter->CurrentHandSet =
		NewObject<UWoTHandSet>(ReplacementCharacter);
	ReplacementCharacter->CurrentHandSet->Init(ReplacementCharacter);
	UWoTHandInfo* ReplacementHand =
		ReplacementCharacter->CurrentHandSet->GetHand(0);
	ReplacementHand->AddClassName(FName(TEXT("AngrealInvFireball")), 0);
	ReplacementHand->bHaveThis[0] = true;
	ReplacementHand->Item[0] = Fireball;
	ReplacementHand->Selected = 0;
	ReplacementCharacter->CurrentHandSet->Selected = 0;
	Controller->Possess(ReplacementCharacter);
	TestFalse(TEXT("replacement possession fully closes item info"),
		Controller->IsItemInfoOpen());
	Controller->TickItemInfoLifecycle();
	TestFalse(TEXT("the replacement pawn does not auto-reopen item info"),
		Controller->IsItemInfoOpen());

	Controller->Possess(Character);
	Character->CurrentHandSet = FixtureHands;   // same reconcile swap as above
	Character->CurrentHandSet->Selected = 0;
	Controller->ShowInventoryInfo();
	Controller->ShowHelp();
	TestFalse(TEXT("F1 replaces the active retail item-info window"),
		Controller->IsItemInfoOpen());
	TestTrue(TEXT("help opens after replacing item info"),
		Controller->IsHelpOpen());
	Controller->ShowHelp();
	Controller->ShowInventoryInfo();
	Controller->ToggleInGameMenu();
	TestFalse(TEXT("first Escape closes the retail WOTWindow"),
		Controller->IsItemInfoOpen());
	TestFalse(TEXT("retail window precedence does not also open the menu"),
		Controller->IsInGameMenuOpen());
	Controller->ToggleInGameMenu();
	TestTrue(TEXT("the next Escape opens the menu"), Controller->IsInGameMenuOpen());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHUDD18ProgressSpec,
	"WoT.HUD.D18.Progress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTHUDD18ProgressSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	Controller->SetAsLocalPlayerController();
	Controller->ClearProgressMessages();
	for (int32 Index = 0; Index < AWoTPlayerController::NumProgressMessages; ++Index)
	{
		TestTrue(TEXT("clear empties every retail slot"),
			Controller->GetProgressMessage(Index).IsEmpty());
		TestEqual(TEXT("clear restores white"),
			Controller->GetProgressColor(Index), FLinearColor::White);
	}
	Controller->SetProgressMessage(TEXT("Connecting"), 0);
	Controller->SetProgressMessage(TEXT("example.invalid"), 2);
	Controller->SetProgressColor(FLinearColor::Red, 2);
	Controller->SetProgressTime(4.0f);
	TestEqual(TEXT("setter stores a line"), Controller->GetProgressMessage(0),
		FString(TEXT("Connecting")));
	TestTrue(TEXT("duration is active before its absolute timeout"),
		Controller->AreProgressMessagesActive(
			Controller->GetProgressTimeOut() - 0.01f));
	TestFalse(TEXT("duration expires at its strict boundary"),
		Controller->AreProgressMessagesActive(Controller->GetProgressTimeOut()));

	const TArray<FWoTHUDDrawCmd> Commands = AWoTHUD::LayoutProgressMessages(
		Controller->GetProgressEntries(), 640, 480, 14.0f);
	TestEqual(TEXT("only nonempty lines emit draw commands"), Commands.Num(), 2);
	if (Commands.Num() == 2)
	{
		TestTrue(TEXT("first line is centered at one-quarter ClipY"),
			Commands[0].Text == TEXT("Connecting") && Commands[0].X == 320.0f &&
			Commands[0].Y == 120.0f && Commands[0].bCenterX);
		TestTrue(TEXT("empty slot still advances the retail stack"),
			Commands[1].Text == TEXT("example.invalid") && Commands[1].Y == 150.0f);
		TestEqual(TEXT("line keeps its ProgressColor"),
			Commands[1].Tint, FLinearColor::Red);
	}

	Controller->PopulateNetworkFailureProgress(
		TEXT("ConnectionLost"), TEXT("The server closed the connection."));
	TestEqual(TEXT("network-failure producer sets the heading"),
		Controller->GetProgressMessage(0),
		FString(TEXT("Network link could not be established")));
	TestTrue(TEXT("network-failure producer carries the detail"),
		Controller->GetProgressMessage(1).Contains(TEXT("server closed")));
	TestTrue(TEXT("network-failure producer arms a timeout"),
		Controller->AreProgressMessagesActive(
			World->GetTimeSeconds() + 0.01f));

	const FString PendingURL =
		TEXT("127.0.0.1/Game/Maps/Arena_01?password=secret?Name=Rand#Start");
	const FString Destination =
		AWoTPlayerController::GetTravelDestinationDisplay(PendingURL);
	TestEqual(TEXT("travel destination keeps only host and map"), Destination,
		FString(TEXT("127.0.0.1/Game/Maps/Arena_01")));
	TestFalse(TEXT("travel destination redacts password options"),
		Destination.Contains(TEXT("secret"))
		|| Destination.Contains(TEXT("password")));

	Controller->PreClientTravel(PendingURL, TRAVEL_Absolute, true);
	TestEqual(TEXT("travel producer displays the redacted destination"),
		Controller->GetProgressMessage(1), Destination);
	Controller->PostSeamlessTravel();
	TestTrue(TEXT("seamless arrival clears the connecting lines"),
		Controller->GetProgressMessage(0).IsEmpty()
		&& Controller->GetProgressMessage(1).IsEmpty());
	TestEqual(TEXT("seamless arrival zeros the progress timeout"),
		Controller->GetProgressTimeOut(), 0.0f);

	Controller->PreClientTravel(PendingURL, TRAVEL_Absolute, false);
	Controller->NotifyLoadedWorld(NAME_None, false);
	TestEqual(TEXT("intermediate world load keeps connecting progress"),
		Controller->GetProgressMessage(0), FString(TEXT("Connecting")));
	Controller->NotifyLoadedWorld(NAME_None, true);
	TestTrue(TEXT("final world arrival clears the connecting lines"),
		Controller->GetProgressMessage(0).IsEmpty()
		&& Controller->GetProgressMessage(1).IsEmpty());
	TestEqual(TEXT("final world arrival zeros the progress timeout"),
		Controller->GetProgressTimeOut(), 0.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
