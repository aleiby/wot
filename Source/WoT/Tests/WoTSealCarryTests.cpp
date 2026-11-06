// Headless specs for the G13 seal-carry wave: the altar Touch state machine
// (bank / hand-back / debounce / win), the on-altar placement physics, the
// team gate + capture/possession notifications, the faction skin identity,
// the carry-indicator sprayer, the pawn Trigger fan-out, and the registry rows.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestBattleClasses.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTFactionCharacters.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTRetailText.h"
#include "../WoTWeaveFX.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"

namespace
{
UWorld* SealCarrySpecCreateWorld()
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());

	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	if (AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>())
	{
		BattleMode->Battle = Battle;
	}
	return World;
}

// A possessed, team-assigned player pawn (flying — no floor in the test world).
AWoTCharacter* SealCarrySpecSpawnPlayer(UWorld* World, uint8 InTeam,
	const FVector& Location,
	TSubclassOf<AWoTCharacter> PawnClass = nullptr,
	TSubclassOf<AWoTPlayerController> ControllerClass = nullptr)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		PawnClass ? *PawnClass : AWoTCharacter::StaticClass(),
		Location, FRotator::ZeroRotator, Params);
	if (!Pawn)
	{
		return nullptr;
	}
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(
		ControllerClass ? *ControllerClass : AWoTPlayerController::StaticClass(),
		Params);
	if (PC)
	{
		PC->InitPlayerState();
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = InTeam;
		}
		PC->Possess(Pawn);
	}
	return Pawn;
}

AWoTSealAltar* SealCarrySpecSpawnAltar(UWorld* World, uint8 InTeam,
	const FVector& Location, int32 Goal)
{
	const FTransform Transform(FRotator::ZeroRotator, Location);
	AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
		AWoTSealAltar::StaticClass(), Transform);
	Altar->Team = InTeam;
	Altar->SealGoal = Goal;
	Altar->FinishSpawning(Transform);
	return Altar;
}

// Give a fresh seal directly to the pawn (the pickup path's end state). Spawned
// clear of the pawn: a spawn-overlap would auto-grant it, and the explicit
// TryPickupBy would then be denied as a duplicate (HandlePickupQuery).
AWoTSeal* SealCarrySpecGiveSeal(UWorld* World, APawn* Pawn)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		Pawn->GetActorLocation() + FVector(0, 0, 500), FRotator::ZeroRotator,
		Params);
	if (Seal && !Seal->TryPickupBy(Pawn))
	{
		return nullptr;
	}
	return Seal;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealAltarFilledWinLoseSoundsTest,
	"WoT.Seal.AltarFilledWinLoseSounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealAltarFilledWinLoseSoundsTest::RunTest(const FString&)
{
	// giMPBattle.EndGame (uc:488-510): filling an altar ends the match with
	// WOT.EndGameSounds.WinGame for the winning team and LoseGame for everyone
	// else, neutral players included.
	UWorld* World = SealCarrySpecCreateWorld();
	const FVector AltarSpot(1000, 0, 0);
	AWoTSealAltar* Altar = SealCarrySpecSpawnAltar(World, 0, AltarSpot, 1);
	SealCarrySpecSpawnAltar(World, 1, FVector(-1000, 0, 0), 1);
	AWoTCharacter* Winner = SealCarrySpecSpawnPlayer(World, 0, AltarSpot,
		nullptr, AWoTTestSealNotifyController::StaticClass());
	AWoTCharacter* Loser = SealCarrySpecSpawnPlayer(World, 1,
		FVector(0, 1000, 0), nullptr, AWoTTestSealNotifyController::StaticClass());
	AWoTCharacter* Neutral = SealCarrySpecSpawnPlayer(World, 255,
		FVector(0, -1000, 0), nullptr, AWoTTestSealNotifyController::StaticClass());
	AWoTSeal* Seal = Winner ? SealCarrySpecGiveSeal(World, Winner) : nullptr;
	TestNotNull(TEXT("seal carried"), Seal);
	if (!Seal || !Loser || !Neutral)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	auto Sounds = [](AWoTCharacter* Pawn)
	{
		const AWoTTestSealNotifyController* PC =
			Cast<AWoTTestSealNotifyController>(Pawn->GetController());
		return PC ? PC->UISounds : TArray<FName>();
	};
	const FName WinGame(TEXT("WOT/WinGame"));
	const FName LoseGame(TEXT("WOT/LoseGame"));
	TestFalse(TEXT("no end sound before the goal"),
		Sounds(Winner).Contains(WinGame) || Sounds(Loser).Contains(LoseGame));

	Altar->TouchedByPawn(Winner);
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	TestTrue(TEXT("match decided"), Mode && Mode->bGameEnded);
	TestTrue(TEXT("winning team hears WinGame"), Sounds(Winner).Contains(WinGame));
	TestFalse(TEXT("winning team does not hear LoseGame"),
		Sounds(Winner).Contains(LoseGame));
	TestTrue(TEXT("other team hears LoseGame"), Sounds(Loser).Contains(LoseGame));
	TestFalse(TEXT("other team does not hear WinGame"), Sounds(Loser).Contains(WinGame));
	TestTrue(TEXT("neutral player hears LoseGame"), Sounds(Neutral).Contains(LoseGame));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealAltarBankAndWinTest,
	"WoT.Seal.AltarBankAndWin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealAltarBankAndWinTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();

	const FVector AltarSpot(1000, 0, 0);
	AWoTSealAltar* Altar = SealCarrySpecSpawnAltar(World, 0, AltarSpot, 2);
	AWoTSealAltar* OtherAltar = SealCarrySpecSpawnAltar(World, 1,
		FVector(-1000, 0, 0), 2);
	AWoTCharacter* Pawn = SealCarrySpecSpawnPlayer(World, 0, AltarSpot);
	AWoTSeal* Seal = SealCarrySpecGiveSeal(World, Pawn);
	TestNotNull(TEXT("seal carried"), Seal);
	if (!Seal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// First bank: a lone seal sits centred (retail put slot 0 left of centre at
	// i*2 - (Goal-1) = -1; PORT_NOTES "Banked seals sit centred on the altar").
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("seal banked into slot 0"), Altar->FindSeal() == Seal);
	TestTrue(TEXT("pulled from the carrier's inventory"),
		Pawn->InventoryComponent->Items.Num() == 0);
	TestTrue(TEXT("altar owns the banked seal"), Seal->GetOwner() == Altar);
	const FVector Expected = AltarSpot
		+ FVector(0, 0, 1.0f + 1.5f);              // altar h + seal h/2, centred
	TestTrue(TEXT("banked at the centered slot spot"),
		Seal->GetActorLocation().Equals(Expected, 0.5));
	TestFalse(TEXT("banked seal's own collision is off (the altar hands it back)"),
		Seal->GetActorEnableCollision());
	TestTrue(TEXT("banked state flagged (the presentation never re-enables)"),
		Seal->bRestingOnAltar);
	TestEqual(TEXT("credited to the placer's team"),
		static_cast<int32>(Seal->ControllingTeam), 0);
	TestTrue(TEXT("altar debounces after a non-winning bank"), Altar->bInactive);
	TestFalse(TEXT("no winner at 1/2"), Altar->bEnded);

	// The debounce window rejects the second bank until the 1s Inactive lapses.
	AWoTSeal* Seal2 = SealCarrySpecGiveSeal(World, Pawn);
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("inactive altar ignores the touch"),
		Pawn->InventoryComponent->Items.Num() == 1);
	WoTTest::TickWorld(World, 1.1f);
	TestFalse(TEXT("debounce lapsed"), Altar->bInactive);

	// Second bank reaches the goal: every altar ends, the mode records the team.
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("second seal banked"), Altar->FindSeal() == Seal2);
	// Both re-centre one seal radius either side (retail spacing fits 2 on 48 uu).
	TestTrue(TEXT("first seal moved to the left of a centred pair"),
		Seal->GetActorLocation().Equals(Expected - FVector(17, 0, 0), 0.5));
	TestTrue(TEXT("second seal on the right of a centred pair"),
		Seal2->GetActorLocation().Equals(Expected + FVector(17, 0, 0), 0.5));
	TestTrue(TEXT("winning altar ended"), Altar->bEnded);
	TestTrue(TEXT("EVERY altar ended"), OtherAltar->bEnded);
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	TestNotNull(TEXT("battle mode"), Mode);
	if (Mode)
	{
		TestEqual(TEXT("mode WinningTeam"),
			static_cast<int32>(Mode->WinningTeam), 0);
		TestTrue(TEXT("match decided (EndGame AltarFilled)"), Mode->bGameEnded);
	}
	// A GameEnded altar is dead to touches.
	AWoTSeal* Seal3 = SealCarrySpecGiveSeal(World, Pawn);
	TestNotNull(TEXT("post-game seal carried"), Seal3);
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("GameEnded altar ignores touches"),
		Pawn->InventoryComponent->Items.Num() == 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealAltarHandBackTest,
	"WoT.Seal.AltarHandsSealBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealAltarHandBackTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();

	const FVector AltarSpot(500, 0, 0);
	AWoTSealAltar* Altar = SealCarrySpecSpawnAltar(World, 0, AltarSpot, 4);
	AWoTCharacter* Pawn = SealCarrySpecSpawnPlayer(World, 0, AltarSpot);

	// Bank one seal, wait out the debounce.
	AWoTSeal* Seal = SealCarrySpecGiveSeal(World, Pawn);
	TestNotNull(TEXT("carried"), Seal);
	if (!Seal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("banked"), Altar->FindSeal() == Seal);
	WoTTest::TickWorld(World, 1.1f);

	// An empty-handed player touch hands the LAST banked seal back through the
	// seal's own pickup path.
	Altar->TouchedByPawn(Pawn);
	TestTrue(TEXT("seal handed back to the toucher"),
		Seal->GetOwner() == Pawn
		&& Pawn->InventoryComponent->Items.Contains(Seal));
	TestNull(TEXT("slot cleared"), Altar->FindSeal());
	TestFalse(TEXT("hand-back clears the banked flag"), Seal->bRestingOnAltar);
	TestTrue(TEXT("hand-back debounces"), Altar->bInactive);

	// NPC (non-player) touches are ignored entirely (SealAltar.uc:214-217).
	WoTTest::TickWorld(World, 1.1f);
	Altar->TouchedByPawn(nullptr);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APawn* Controllerless = World->SpawnActor<AWoTCharacter>(
		AltarSpot, FRotator::ZeroRotator, Params);
	Altar->TouchedByPawn(Controllerless);
	TestTrue(TEXT("controllerless touch left the altar active"),
		!Altar->bInactive);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealTeamGateCaptureTest,
	"WoT.Seal.TeamGateCaptureAndNotifications",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealTeamGateCaptureTest::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping loaded seal message"));
		return true;
	}
	const FString* PossessionChange = FWoTRetailText::FindSectionValue(
		TEXT("Seal"), TEXT("PossesionChangeStr"));
	if (!TestNotNull(TEXT("seal possession text is loaded"), PossessionChange))
	{
		return false;
	}
	UWorld* World = SealCarrySpecCreateWorld();

	const FVector AltarSpot(800, 0, 0);
	AWoTSealAltar* Altar = SealCarrySpecSpawnAltar(World, 0, AltarSpot, 4);

	// Wrong-team placements are rejected: the seal stays carried, no slot fills,
	// and (retail) the touch still debounces via the non-winning branch.
	AWoTCharacter* Enemy = SealCarrySpecSpawnPlayer(World, 1, AltarSpot,
		nullptr, AWoTTestSealNotifyController::StaticClass());
	AWoTSeal* EnemySeal = SealCarrySpecGiveSeal(World, Enemy);
	TestNotNull(TEXT("enemy carried"), EnemySeal);
	if (!EnemySeal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Altar->TouchedByPawn(Enemy);
	TestTrue(TEXT("wrong-team seal stays carried"),
		Enemy->InventoryComponent->Items.Contains(EnemySeal));
	TestNull(TEXT("no slot filled"), Altar->FindSeal());
	// Empty the enemy's hands for the later take step (the carried branch
	// always outranks the hand-back branch).
	EnemySeal->Destroy();

	// A team-0 AesSedai banks a seal credited to team 1 -> the capture
	// notification reaches EVERY player, and the seal adopts the AesSedai
	// faction identity.
	AWoTCharacter* Player = SealCarrySpecSpawnPlayer(World, 0, AltarSpot,
		AWoTAesSedai::StaticClass(),
		AWoTTestSealNotifyController::StaticClass());
	AWoTSeal* Seal = SealCarrySpecGiveSeal(World, Player);
	TestNotNull(TEXT("player carried"), Seal);
	if (!Seal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Credit it to the ENEMY team first (a captured seal).
	Seal->SetControllingTeam(Enemy->GetController());
	TestEqual(TEXT("enemy credited"),
		static_cast<int32>(Seal->ControllingTeam), 1);

	WoTTest::TickWorld(World, 1.1f);
	Altar->TouchedByPawn(Player);
	TestTrue(TEXT("captured seal banked"), Altar->FindSeal() == Seal);
	TestEqual(TEXT("re-credited to the capturing team"),
		static_cast<int32>(Seal->ControllingTeam), 0);
	TestEqual(TEXT("faction identity adopted"),
		Seal->ControllingFactionName, FName(TEXT("AesSedai")));
	TestTrue(TEXT("faction status icon adopted"),
		Seal->StatusIcon.ToString().Contains(TEXT("I_AesSedaiSeal")));

	AWoTTestSealNotifyController* EnemyPC =
		Cast<AWoTTestSealNotifyController>(Enemy->GetController());
	AWoTTestSealNotifyController* PlayerPC =
		Cast<AWoTTestSealNotifyController>(Player->GetController());
	TestNotNull(TEXT("enemy capture PC"), EnemyPC);
	TestNotNull(TEXT("player capture PC"), PlayerPC);
	if (EnemyPC && PlayerPC)
	{
		TestTrue(TEXT("capture notification reached every player"),
			EnemyPC->UISounds.Contains(FName(TEXT("WOT/CaptureSeal")))
			&& PlayerPC->UISounds.Contains(FName(TEXT("WOT/CaptureSeal"))));
		TestTrue(TEXT("capture message carries the team description"),
			EnemyPC->CenterMessages.ContainsByPredicate([](const FString& S)
			{
				return S.Contains(TEXT(" captured a seal."));
			}));

		// Taking the enemy-credited seal notifies the losing team only
		// (Seal.SendTakeNotification). Hand it back to the enemy carrier.
		WoTTest::TickWorld(World, 1.1f);
		EnemyPC->UISounds.Empty();
		PlayerPC->UISounds.Empty();
		Altar->TouchedByPawn(Enemy);   // enemy picks the team-0-credited seal up
		TestTrue(TEXT("altar handed the seal to the enemy"),
			Seal->GetOwner() == Enemy);
		TestTrue(TEXT("losing team heard the possession change"),
			PlayerPC->UISounds.Contains(FName(TEXT("WOT/PossesionChangeSound"))));
		TestFalse(TEXT("the taker's team did not"),
			EnemyPC->UISounds.Contains(FName(TEXT("WOT/PossesionChangeSound"))));
		TestTrue(TEXT("losing team read the possession message"),
			PlayerPC->CenterMessages.Contains(*PossessionChange));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealCarryIndicatorTest,
	"WoT.Seal.CarryIndicator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealCarryIndicatorTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();

	// A possessed AesSedai reads PlayerColor PC_Blue -> the Blue fountain; an
	// unpossessed carrier would fall to the retail default GREEN.
	AWoTCharacter* Player = SealCarrySpecSpawnPlayer(World, 0,
		FVector(0, 0, 200), AWoTAesSedai::StaticClass());
	AWoTSeal* Seal = SealCarrySpecGiveSeal(World, Player);
	TestNotNull(TEXT("carried"), Seal);
	if (!Seal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTParticleSprayer* Indicator = Seal->GetIndicatorForTest();
	TestNotNull(TEXT("carry indicator spawned"), Indicator);
	if (Indicator)
	{
		TestEqual(TEXT("carrier color row"), Indicator->ConfigName,
			FName(TEXT("CarrySealSprayerBlue")));
		TestTrue(TEXT("rides the carrier"),
			Indicator->FollowActor.Get() == Player);
		TestEqual(TEXT("over-the-head PrePivot seat"),
			Indicator->FollowOffset.Z, 75.0);
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("fountain pinned 75uu over the carrier"),
			Indicator->GetActorLocation().Equals(
				Player->GetActorLocation() + FVector(0, 0, 75), 1.0));
	}

	// Dropping retires it gracefully (bOn off + 1s linger) and the ground
	// pickup carries none.
	Seal->DropFrom(Player->GetActorLocation() + FVector(60, 0, 0));
	TestNull(TEXT("drop cleared the indicator handle"),
		Seal->GetIndicatorForTest());
	if (Indicator)
	{
		TestFalse(TEXT("dropped indicator stopped emitting"), Indicator->bOn);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealPawnFanoutTest,
	"WoT.Seal.PawnTriggerFanout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealPawnFanoutTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();

	// An NPC whose controller records the fan-out REACH (every pawn's
	// controller is notified at pickup/drop/destroy — Util.TriggerAllInstances).
	// The Captain.Trigger GuardSeal gate behind the counter is RETAIL-DEAD
	// (census: ChangeOrders collapses GuardSeal out of AssignedOrders at
	// SetInitialState, Captain.uc:91/235-246, and no other author exists), so
	// the reach is the only live surface to pin.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(2000, 0, 0), FRotator::ZeroRotator, Params);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* NPCController =
		NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	TestNotNull(TEXT("NPC controller"), NPCController);
	if (!NPCController)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTCharacter* Player = SealCarrySpecSpawnPlayer(World, 0,
		FVector(0, 0, 200));
	const int32 Before = NPCController->SealTriggerNotifiesForTest;
	AWoTSeal* Seal = SealCarrySpecGiveSeal(World, Player);
	TestNotNull(TEXT("carried"), Seal);
	TestEqual(TEXT("pickup fanned out (PickupFunction)"),
		NPCController->SealTriggerNotifiesForTest, Before + 1);

	Seal->DropFrom(Player->GetActorLocation() + FVector(60, 0, 0));
	TestEqual(TEXT("drop fanned out"),
		NPCController->SealTriggerNotifiesForTest, Before + 2);

	Seal->Destroy();
	TestEqual(TEXT("destroy fanned out (DestroyedEvent)"),
		NPCController->SealTriggerNotifiesForTest, Before + 3);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealRegistryRowsTest,
	"WoT.Seal.CarrySprayerRegistryRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealRegistryRowsTest::RunTest(const FString&)
{
	const TCHAR* Colors[] = {TEXT("Blue"), TEXT("Green"), TEXT("Purple"),
		TEXT("Red"), TEXT("Yellow")};
	for (const TCHAR* Color : Colors)
	{
		const FWoTSprayerBundleRow* R = WoTWeaveFX::Find(
			FName(*FString::Printf(TEXT("CarrySealSprayer%s"), Color)));
		TestNotNull(Color, R);
		if (!R)
		{
			continue;
		}
		TestEqual(TEXT("spread"), R->Spread, 15.0f);
		TestEqual(TEXT("volume"), R->Volume, 11.0f);
		TestEqual(TEXT("min volume"), R->MinVolume, 0.25f);
		// Authored VisibilityRadius/Height 0 = no distance culling.
		TestEqual(TEXT("unlimited visibility"), R->VisibilityRadius, 0.0f);
		// NumTemplates=1: the authored Templates(1) burst is data the retail
		// sim never selects — exactly one live template.
		TestEqual(TEXT("T0 only (NumTemplates=1)"), R->Templates.Num(), 1);
		if (R->Templates.Num() == 1)
		{
			const FWoTParticleTemplate& T = R->Templates[0].Template;
			TestEqual(TEXT("T0 lifespan"), T.LifeSpan, 1.75f);
			// Unlisted Weight inherits the base template element's 1.0.
			TestEqual(TEXT("T0 inherited weight"), T.Weight, 1.0f);
			TestEqual(TEXT("T0 draw max"), T.MaxDrawScale, 0.12f);
			TestTrue(TEXT("color slug"),
				R->Templates[0].MaterialSlug.ToString().Contains(
					FString::Printf(TEXT("CSseal%s"), Color)));
		}
	}
	// The Nimbus child ships Particles(0)=None (binary CDO) — an invisible
	// emitter; no speculative row exists for it.
	TestNull(TEXT("no nimbus row"),
		WoTWeaveFX::Find(TEXT("CarrySealNimbusSprayer")));
	return true;
}

// The REAL overlap path of a projected-size altar (no direct TouchedByPawn):
// a player dropped onto the enemy altar from above first overlaps only the
// containing broad-phase capsule, then meets the flat 140x1 UE1 cylinder as it
// descends - Touch must fire then and hand over the last banked seal
// (SealAltar.Active.Touch, empty-handed branch; PickupQuery open after play).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealAltarRealOverlapTakeTest,
	"WoT.Seal.AltarRealOverlapTake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealAltarRealOverlapTakeTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (Mode && Mode->Battle)
	{
		Mode->Battle->bTeleportersDisabled = false;   // play has begun
	}
	const FTransform AltarTransform(FRotator::ZeroRotator, FVector::ZeroVector);
	AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
		AWoTSealAltar::StaticClass(), AltarTransform);
	Altar->Team = 1;
	Altar->SealGoal = 4;
	Altar->SetUE1CollisionSize(140.0f, 1.0f);   // the Battle2_01 team-1 altar
	Altar->FinishSpawning(AltarTransform);
	AWoTCharacter* Owner = SealCarrySpecSpawnPlayer(World, 1, FVector(0, 3000, 0));
	AWoTSeal* Banked = SealCarrySpecGiveSeal(World, Owner);
	if (!TestNotNull(TEXT("team-1 seal"), Banked))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Altar->PlaceSeal(Banked);
	TestTrue(TEXT("seal banked on the enemy altar"), Altar->FindSeal() == Banked);
	WoTTest::TickWorld(World, 1.1f);   // the bank's debounce lapses
	TestFalse(TEXT("altar active again"), Altar->bInactive);

	AWoTCharacter* Taker = SealCarrySpecSpawnPlayer(World, 0, FVector(2000, 0, 0));
	float Radius = 0.0f, HalfHeight = 0.0f;
	Taker->GetSimpleCollisionCylinder(Radius, HalfHeight);
	Taker->SetActorLocation(FVector(0, 0, HalfHeight + 20.0f));   // above the flat cap
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("above the cylinder: in the broad phase only, no touch yet"),
		Altar->Touch->IsOverlappingActor(Taker) && Altar->FindSeal() == Banked);
	Taker->SetActorLocation(FVector(0, 0, HalfHeight - 20.0f));   // meets the cylinder
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("descending into the flat cylinder takes the banked seal"),
		Taker->InventoryComponent->Items.Contains(Banked));
	TestNull(TEXT("the altar slot is empty"), Altar->FindSeal());
	TestTrue(TEXT("the take debounces the altar"), Altar->bInactive);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealBankedSealsFitTheAltarTest,
	"WoT.Seal.BankedSealsFitTheAltar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealBankedSealsFitTheAltarTest::RunTest(const FString&)
{
	UWorld* World = SealCarrySpecCreateWorld();
	auto Retail = [](int32 Slot, int32 Goal) { return 17.0f * (Slot * 2 - (Goal - 1)); };

	// The default 48 uu altar with the default 4-seal goal: retail's 102 uu row put
	// slot 0's centre 3 uu past the edge. Every slot must now keep its mesh (half the
	// 17 uu collision radius) on the altar, evenly spaced and centred.
	AWoTSealAltar* Narrow = SealCarrySpecSpawnAltar(World, 0, FVector(1000, 0, 0), 4);
	TestTrue(TEXT("retail's slot 0 overhung a 48 uu altar"),
		FMath::Abs(Retail(0, 4)) > 48.0f);
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		TestTrue(FString::Printf(TEXT("slot %d stays on the 48 uu altar"), Slot),
			FMath::Abs(Narrow->BankedSealOffset(Slot, 4)) + 8.5f
				<= 48.0f + KINDA_SMALL_NUMBER);
	}
	TestTrue(TEXT("the compressed row is centred"), FMath::IsNearlyEqual(
		Narrow->BankedSealOffset(0, 4), -Narrow->BankedSealOffset(3, 4)));
	TestTrue(TEXT("the compressed row is evenly spaced"), FMath::IsNearlyEqual(
		Narrow->BankedSealOffset(1, 4) - Narrow->BankedSealOffset(0, 4),
		Narrow->BankedSealOffset(3, 4) - Narrow->BankedSealOffset(2, 4)));

	// A row that already fits keeps retail spacing exactly: a 2-seal goal on the
	// default altar, and the 4-seal goal on Battle2_01's 140 uu altar.
	AWoTSealAltar* Pair = SealCarrySpecSpawnAltar(World, 0, FVector(2000, 0, 0), 2);
	AWoTSealAltar* Wide = SealCarrySpecSpawnAltar(World, 0, FVector(3000, 0, 0), 4);
	Wide->SetUE1CollisionSize(140.0f, 1.0f);
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		if (Slot < 2)
		{
			TestEqual(FString::Printf(TEXT("2-seal slot %d keeps retail spacing"), Slot),
				Pair->BankedSealOffset(Slot, 2), Retail(Slot, 2));
		}
		TestEqual(FString::Printf(TEXT("140 uu altar slot %d keeps retail spacing"), Slot),
			Wide->BankedSealOffset(Slot, 4), Retail(Slot, 4));
	}

	// A partly filled altar centres the seals it holds: three of a 4-seal goal sit
	// at -34/0/+34, where retail's slots -51/-17/+17 left the row lopsided.
	for (int32 Rank = 0; Rank < 3; ++Rank)
	{
		TestEqual(FString::Printf(TEXT("3 of 4 seals: seal %d centred"), Rank),
			Wide->BankedSealOffset(Rank, 3), 34.0f * (Rank - 1));
	}
	TestEqual(TEXT("a lone seal sits at the centre"), Wide->BankedSealOffset(0, 1), 0.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
