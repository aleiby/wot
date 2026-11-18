// G21: NPC defensive dodge + Minion standing/tentacle attacks. Like G20 these specs use
// fixed 1/60 world steps and avoid exact-timer-boundary asserts (Mac and Windows builds
// clamp timers at frame granularity). Helper names are file-unique (unity build folds all
// Tests/*.cpp into one TU).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Projectiles/WoTProjectile.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	void SpawnG21ActivityAnchor(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, Params))
		{
			// Activity relevance is bIsPlayer-only; dead avoids perception/combat.
			Anchor->Health = 0;
			Anchor->SetActorEnableCollision(false);
			Anchor->SetActorHiddenInGame(true);
		}
	}

	// A dodge-armed creature, faced +X and flying (no gravity drift until the dodge
	// launches its own falling physics).
	AWoTNPCCharacter* SpawnG21Dodger(UWorld* World, FName Row, const FVector& Location)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, Row, Location, 1, true, AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->SetActorRotation(FRotator::ZeroRotator);
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnG21ActivityAnchor(World, Location);
		}
		return NPC;
	}

	AWoTNPCController* G21Controller(AWoTNPCCharacter* NPC)
	{
		return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	}

	// A concrete projectile with its movement velocity aimed as given (the dodge scan +
	// FindDodgeDestination both read ProjectileMovement->Velocity).
	AWoTTestDodgeProjectile* SpawnG21Projectile(UWorld* World, const FVector& Location,
		const FVector& Velocity)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestDodgeProjectile* Proj = World->SpawnActor<AWoTTestDodgeProjectile>(
			Location, FRotator::ZeroRotator, Params);
		if (Proj && Proj->ProjectileMovement)
		{
			Proj->ProjectileMovement->Velocity = Velocity;
		}
		return Proj;
	}

	// A still, unpossessed melee target (a player pawn when bPlayer, else an NPC pawn) at a
	// fixed spot. bPlayer picks a plain AWoTCharacter (IsNPCPawn() false → drawn in by the
	// grab); otherwise AWoTTestNPCCharacter (IsNPCPawn() true → damage but no draw-in).
	AWoTCharacter* SpawnG21Victim(UWorld* World, const FVector& Location, uint8 Team, bool bPlayer)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Victim = bPlayer
			? World->SpawnActor<AWoTCharacter>(Location, FRotator::ZeroRotator, Params)
			: World->SpawnActor<AWoTTestNPCCharacter>(Location, FRotator::ZeroRotator, Params);
		if (Victim)
		{
			// AssignedTeam is NPC-side state; the plain-AWoTCharacter "player" victim
			// carries no team here (the grab specs drive goals directly, not factions).
			if (AWoTNPCCharacter* NPCVictim = Cast<AWoTNPCCharacter>(Victim))
			{
				NPCVictim->AssignedTeam = Team;
			}
			// A bare spawn has no configured Health — the damage chain no-ops on a
			// dead (<=0) victim, silently zeroing every assert downstream.
			Victim->Health = 100;
			Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			if (AController* C = Victim->GetController())
			{
				C->UnPossess();
			}
		}
		return Victim;
	}
}

// --- 1. Dodge trigger: incoming projectile inside the window fires the dodge; outside the
//        window / not moving toward does not. ------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21DodgeTriggerSpec,
	"WoT.AI.DodgeTrigger", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21DodgeTriggerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A Warder in Tracking (Defensive slot live), a projectile 300uu ahead closing at
	// 600uu/s → arrival 0.5s, inside [0.05,1.5].
	AWoTNPCCharacter* Warder = SpawnG21Dodger(World, TEXT("Warder"), FVector(0, 0, 100));
	AWoTNPCController* Ctl = G21Controller(Warder);
	if (!TestNotNull(TEXT("Warder"), Warder) || !TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("Warder is dodge-armed"), Warder->bDefensiveDodge);
	Ctl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	TestTrue(TEXT("Defensive notifier live in Tracking"),
		Warder->Brain->IsNotifierEnabled(EWoTNotifierSlot::Defensive));
	SpawnG21Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0));

	TestTrue(TEXT("incoming projectile in window fires a dodge"), Ctl->TryBeginDodgeForTest());
	TestEqual(TEXT("dodge enters the Dodging state"),
		static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Dodging));
	const FVector Launch = Warder->GetCharacterMovement()->Velocity;
	TestTrue(TEXT("dodge gains the DodgeVelocityAltitude 160 rise"),
		FMath::IsNearlyEqual(Launch.Z, 160.0f, 1.0f));
	TestTrue(TEXT("dodge gains the DodgeVelocityFactor 400 horizontal speed"),
		FMath::IsNearlyEqual(Launch.Size2D(), 400.0f, 1.0f));
	TestFalse(TEXT("dodge disables the Defensive notifier"),
		Warder->Brain->IsNotifierEnabled(EWoTNotifierSlot::Defensive));

	// Outside the window: 700uu away closing at only 100uu/s → arrival 7s, past 1.5.
	AWoTNPCCharacter* Warder2 = SpawnG21Dodger(World, TEXT("Warder"), FVector(4000, 0, 100));
	AWoTNPCController* Ctl2 = G21Controller(Warder2);
	Ctl2->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	SpawnG21Projectile(World, FVector(4700, 0, 100), FVector(-100, 0, 0));
	TestFalse(TEXT("arrival past the window does not dodge"), Ctl2->TryBeginDodgeForTest());
	TestEqual(TEXT("no dodge stays in Tracking"),
		static_cast<int32>(Ctl2->GetBrainState()), static_cast<int32>(EWoTBrainState::Tracking));

	// Not moving toward: a projectile in range but receding.
	AWoTNPCCharacter* Warder3 = SpawnG21Dodger(World, TEXT("Warder"), FVector(8000, 0, 100));
	AWoTNPCController* Ctl3 = G21Controller(Warder3);
	Ctl3->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	SpawnG21Projectile(World, FVector(8300, 0, 100), FVector(600, 0, 0));
	TestFalse(TEXT("a receding projectile does not dodge"), Ctl3->TryBeginDodgeForTest());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 2. Dodge destination: the sidestep is perpendicular to the offender's velocity, and
//        the verbatim right-first double negation is pinned. --------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21DodgeDestinationSpec,
	"WoT.AI.DodgeDestination", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21DodgeDestinationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = SpawnG21Dodger(World, TEXT("Warder"), FVector(0, 0, 100));
	AWoTNPCController* Ctl = G21Controller(Warder);
	if (!TestNotNull(TEXT("Warder"), Warder) || !TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Offender velocity +X → PursuerDirection (1,0,0) → DodgeDirection (0,1,0) (+Y).
	AWoTTestDodgeProjectile* Offender =
		SpawnG21Projectile(World, FVector(0, 0, 100), FVector(600, 0, 0));
	const FVector Origin = Warder->GetActorLocation();
	FVector Dest;

	// left-first, both sides open → the +Y (perpendicular) side.
	Ctl->DodgeBlockedDirsForTest.Empty();
	TestTrue(TEXT("left-first finds a destination"),
		Ctl->FindDodgeDestinationForTest(Dest, Offender, /*bLeftFirst*/ true));
	TestTrue(TEXT("left-first dodges to the +perpendicular side"), (Dest - Origin).Y > 0.0f);

	// left-first, +Y blocked → falls back to the -Y side.
	Ctl->DodgeBlockedDirsForTest = {FVector(0, 1, 0)};
	TestTrue(TEXT("left-first blocked still finds a destination"),
		Ctl->FindDodgeDestinationForTest(Dest, Offender, true));
	TestTrue(TEXT("blocked left-first fallback goes to the other side"), (Dest - Origin).Y < 0.0f);

	// right-first, both open → the -Y side.
	Ctl->DodgeBlockedDirsForTest.Empty();
	TestTrue(TEXT("right-first finds a destination"),
		Ctl->FindDodgeDestinationForTest(Dest, Offender, /*bLeftFirst*/ false));
	TestTrue(TEXT("right-first dodges to the -perpendicular side"), (Dest - Origin).Y < 0.0f);

	// right-first, -Y blocked → PARITY-LOCKED: the verbatim two `*= -1` negations flip the
	// direction back to +Y, so a faithful port searches -Y then +Y (the "same-side retry"
	// one might expect does NOT manifest). Pin the true behavior.
	Ctl->DodgeBlockedDirsForTest = {FVector(0, -1, 0)};
	TestTrue(TEXT("right-first blocked still finds a destination"),
		Ctl->FindDodgeDestinationForTest(Dest, Offender, false));
	TestTrue(TEXT("right-first fallback (verbatim double-negation) flips to +Y"),
		(Dest - Origin).Y > 0.0f);

	// both sides blocked → no dodge.
	Ctl->DodgeBlockedDirsForTest = {FVector(0, 1, 0), FVector(0, -1, 0)};
	TestFalse(TEXT("both sides blocked yields no destination"),
		Ctl->FindDodgeDestinationForTest(Dest, Offender, true));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 3. Dodge anim: DodgeLeft vs DodgeRight by the local-Y sign of the sidestep. -------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21DodgeAnimSpec,
	"WoT.AI.DodgeAnim", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21DodgeAnimSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Projectile incoming from +X moving -X → DodgeDirection (0,-1,0). Facing +X, local Y
	// equals world Y. Left-first lands the -Y side (local Y<0 → DodgeLeft); right-first
	// flips to +Y (local Y>0 → DodgeRight).
	AWoTNPCCharacter* Left = SpawnG21Dodger(World, TEXT("Warder"), FVector(0, 0, 100));
	AWoTNPCController* LeftCtl = G21Controller(Left);
	if (!TestNotNull(TEXT("left Warder"), Left) || !TestNotNull(TEXT("left controller"), LeftCtl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	LeftCtl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	LeftCtl->DodgeForcedSidePick = 0; // left-first
	SpawnG21Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0));
	TestTrue(TEXT("left-first dodge fires"), LeftCtl->TryBeginDodgeForTest());
	TestEqual(TEXT("local-Y<0 picks DodgeLeft"),
		LeftCtl->GetLastDodgeSlot(), FName(TEXT("DodgeLeft")));

	AWoTNPCCharacter* Right = SpawnG21Dodger(World, TEXT("Warder"), FVector(4000, 0, 100));
	AWoTNPCController* RightCtl = G21Controller(Right);
	RightCtl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	RightCtl->DodgeForcedSidePick = 1; // right-first
	SpawnG21Projectile(World, FVector(4300, 0, 100), FVector(-600, 0, 0));
	TestTrue(TEXT("right-first dodge fires"), RightCtl->TryBeginDodgeForTest());
	TestEqual(TEXT("local-Y>0 picks DodgeRight"),
		RightCtl->GetLastDodgeSlot(), FName(TEXT("DodgeRight")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 4. Landing: on touchdown Velocity.Z is zeroed, the Defensive notifier re-enables,
//        and the pawn returns to Tracking. ---------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21DodgeLandingSpec,
	"WoT.AI.DodgeLanding", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21DodgeLandingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = SpawnG21Dodger(World, TEXT("Warder"), FVector(0, 0, 100));
	AWoTNPCController* Ctl = G21Controller(Warder);
	if (!TestNotNull(TEXT("Warder"), Warder) || !TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Ctl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	SpawnG21Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0));
	TestTrue(TEXT("dodge fires"), Ctl->TryBeginDodgeForTest());
	TestEqual(TEXT("mid-dodge state"),
		static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Dodging));

	// The launch put the pawn into falling physics; simulate touchdown by leaving that
	// mode, then tick so WaitForLanding resolves.
	Warder->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestEqual(TEXT("landing returns to Tracking"),
		static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Tracking));
	TestTrue(TEXT("landing zeroes Velocity.Z"),
		FMath::IsNearlyZero(Warder->GetCharacterMovement()->Velocity.Z, 1.0f));
	TestTrue(TEXT("landing re-enables the Defensive notifier"),
		Warder->Brain->IsNotifierEnabled(EWoTNotifierSlot::Defensive));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 5. RespondedToActor with bRecollect=true (LegendPawn.uc:1376 →
//        DefensiveDetector.uc:186 bRejectResponded=false): retail ALLOWS re-dodging the
//        same offender on the next poll — a re-acquiring seeker keeps getting dodged.
//        The double-response guard is the Defensive-off Dodging episode, not a skip. ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21DodgeRespondedSpec,
	"WoT.AI.DodgeResponded", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21DodgeRespondedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = SpawnG21Dodger(World, TEXT("Warder"), FVector(0, 0, 100));
	AWoTNPCController* Ctl = G21Controller(Warder);
	if (!TestNotNull(TEXT("Warder"), Warder) || !TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Ctl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	AWoTTestDodgeProjectile* First =
		SpawnG21Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0));
	TestTrue(TEXT("first offender fires a dodge"), Ctl->TryBeginDodgeForTest());

	// Land and return to Tracking with the same offender still inbound (a seeker that
	// re-acquired): the SAME offender re-triggers a dodge (bRecollect=true semantics).
	Warder->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestEqual(TEXT("back in Tracking"),
		static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Tracking));
	TestTrue(TEXT("the same still-inbound offender IS re-dodged (retail bRecollect=true)"),
		Ctl->TryBeginDodgeForTest());

	// And a genuinely new incoming projectile also triggers.
	Warder->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	First->Destroy();
	SpawnG21Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0));
	TestTrue(TEXT("a fresh offender dodges again"), Ctl->TryBeginDodgeForTest());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 6. Minion attack selector: standing+gap100 → tentacle grab; standing+gap400 →
//        standing bite/claws; moving → the running set. --------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21MinionSelectorSpec,
	"WoT.AI.MinionSelector", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21MinionSelectorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Each scenario is spaced far apart so the pawns never overlap in the shared world.
	auto RunSelector = [&](float BaseX, float VictimGapX, bool bMoving) -> FName
	{
		const FVector Base(BaseX, 0, 100);
		AWoTNPCCharacter* Minion = SpawnG21Dodger(World, TEXT("Minion"), Base);
		AWoTNPCController* Ctl = G21Controller(Minion);
		AWoTCharacter* Victim = SpawnG21Victim(
			World, Base + FVector(VictimGapX, 0, 0), 2, /*bPlayer*/ true);
		if (!Minion || !Ctl || !Victim)
		{
			return NAME_None;
		}
		if (bMoving)
		{
			Victim->GetCharacterMovement()->Velocity = FVector(0, 300, 0);
			Victim->GetCharacterMovement()->UpdateComponentVelocity();
		}
		Minion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
		Minion->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Ctl->SetBrainStateForG20Test(EWoTBrainState::PerformMelee);
		return Ctl->GetLastSwingSlot();
	};

	// Standing target inside the tentacle band [40,200] (gap ~100).
	TestEqual(TEXT("standing + tentacle range picks TentacleGrab"),
		RunSelector(0.0f, 160.0f, false), FName(TEXT("TentacleGrab")));
	// Standing target beyond the tentacle band (gap ~400) — the standing bite/claws.
	TestEqual(TEXT("standing + past-tentacle range picks MeleeStandingAttack"),
		RunSelector(4000.0f, 460.0f, false), FName(TEXT("MeleeStandingAttack")));
	// Moving target (speed > 1) keeps the running set (the generic MeleeAttack slot).
	TestEqual(TEXT("moving target keeps the running MeleeAttack slot"),
		RunSelector(8000.0f, 160.0f, true), FName(TEXT("MeleeAttack")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 7. Tentacle grab: the @0.40 notify damages 10 'Grabbed'; a PLAYER victim is reeled
//        toward the Minion, an NPC victim is not; the Movement notifier is disabled for
//        the episode. --------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG21MinionGrabSpec,
	"WoT.AI.MinionGrab", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG21MinionGrabSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// --- PLAYER victim: damage + draw-in toward the Minion.
	AWoTNPCCharacter* Minion = SpawnG21Dodger(World, TEXT("Minion"), FVector(0, 0, 100));
	AWoTNPCController* Ctl = G21Controller(Minion);
	AWoTCharacter* Player = SpawnG21Victim(World, FVector(160, 0, 100), 2, /*bPlayer*/ true);
	if (!TestNotNull(TEXT("Minion"), Minion) || !TestNotNull(TEXT("controller"), Ctl) ||
		!TestNotNull(TEXT("player victim"), Player))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 PlayerStart = Player->Health;
	Minion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
	Minion->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
	Ctl->SetBrainStateForG20Test(EWoTBrainState::PerformMelee);
	TestEqual(TEXT("player-in-range grab picks TentacleGrab"),
		Ctl->GetLastSwingSlot(), FName(TEXT("TentacleGrab")));
	TestFalse(TEXT("grab disables the Movement notifier for the episode"),
		Minion->Brain->IsNotifierEnabled(EWoTNotifierSlot::Movement));

	// The @0.40 damage notify rides the REAL ATTACKTENT sequence duration (the anim
	// registry binds sequences even headless — no 1.0s stand-in). Compute the notify
	// time independently, like the G20 stomp spec, and sample just past it; read the
	// reel velocity promptly (a stationary flying victim brakes toward zero each frame).
	const FWoTAnimTableEntry* GrabRow = WoTAnimTable::PickSlotAnim(
		Minion->AnimTableName, FName(TEXT("ATTACKTENT")), nullptr, /*bNotSlot*/ true);
	const float GrabNotifyTime = WoTAnimTable::ResolveTweenTime(GrabRow) +
		0.40f * FMath::Max(Minion->SequenceDurationSeconds(FName(TEXT("ATTACKTENT")),
			WoTAnimTable::ResolveRate(GrabRow)), 1.0f);
	WoTTest::TickWorld(World, GrabNotifyTime + 0.05f);
	TestEqual(TEXT("grab deals 10 'Grabbed'"), PlayerStart - Player->Health, 10);
	const FVector Reel = Player->GetCharacterMovement()->Velocity;
	TestTrue(TEXT("player is reeled toward the Minion (-X)"), Reel.X < -300.0f);
	TestTrue(TEXT("draw-in magnitude is on the retail scale"),
		Reel.Size() > 400.0f && Reel.Size() < 4000.0f);

	// The episode completes and returns to the tracking loop. The victim is still
	// standing in the tentacle band, so the offensive pulse may legitimately start
	// a NEW grab right after — wait for the Tracking transition instead of
	// sampling a fixed instant (racy against the re-grab).
	bool bReturnedToTracking = false;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
		if (Ctl->GetBrainState() == EWoTBrainState::Tracking)
		{
			bReturnedToTracking = true;
			break;
		}
	}
	TestTrue(TEXT("grab episode returns to Tracking"), bReturnedToTracking);

	// --- NPC victim: damage but NO draw-in.
	AWoTNPCCharacter* Minion2 = SpawnG21Dodger(World, TEXT("Minion"), FVector(6000, 0, 100));
	AWoTNPCController* Ctl2 = G21Controller(Minion2);
	AWoTCharacter* NPCVictim = SpawnG21Victim(World, FVector(6160, 0, 100), 2, /*bPlayer*/ false);
	if (TestNotNull(TEXT("second Minion"), Minion2) && TestNotNull(TEXT("NPC victim"), NPCVictim))
	{
		const int32 NPCStart = NPCVictim->Health;
		Minion2->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, NPCVictim);
		Minion2->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Ctl2->SetBrainStateForG20Test(EWoTBrainState::PerformMelee);
		// Same real-sequence notify computation as the player branch.
		const FWoTAnimTableEntry* GrabRow2 = WoTAnimTable::PickSlotAnim(
			Minion2->AnimTableName, FName(TEXT("ATTACKTENT")), nullptr, /*bNotSlot*/ true);
		const float GrabNotify2 = WoTAnimTable::ResolveTweenTime(GrabRow2) +
			0.40f * FMath::Max(Minion2->SequenceDurationSeconds(FName(TEXT("ATTACKTENT")),
				WoTAnimTable::ResolveRate(GrabRow2)), 1.0f);
		WoTTest::TickWorld(World, GrabNotify2 + 0.05f);
		TestEqual(TEXT("NPC victim still takes the grab damage"),
			NPCStart - NPCVictim->Health, 10);
		TestTrue(TEXT("NPC victim is NOT reeled in"),
			NPCVictim->GetCharacterMovement()->Velocity.Size() < 50.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
