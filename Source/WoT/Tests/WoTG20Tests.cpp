// G20: retail pawn/combat animation state machines. These specs deliberately use
// fixed 1/60 world steps: UWorld clamps/timers at frame granularity and exact timer
// boundary assertions are not portable across the Mac/Windows lanes.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTVertexAnimComponent.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	void SpawnG20ActivityAnchor(UWorld* World, const FVector& Location)
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

	AWoTNPCCharacter* SpawnG20Creature(UWorld* World, FName Row,
		const FVector& Location, uint8 Team)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, Row, Location, Team, true, AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnG20ActivityAnchor(World, Location);
		}
		return NPC;
	}

	AWoTTestNPCCharacter* SpawnG20StillTarget(UWorld* World,
		const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* Target = World->SpawnActor<AWoTTestNPCCharacter>(
			Location, FRotator::ZeroRotator, Params);
		if (Target)
		{
			Target->AssignedTeam = Team;
			Target->MeleeRange = 0.0f;
			Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			if (AController* Controller = Target->GetController())
			{
				Controller->UnPossess();
			}
		}
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20TweenPlaySpec,
	"WoT.Anim.TweenPlay", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20TweenPlaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(Pawn->GetMesh());
	if (!TestNotNull(TEXT("vertex animation component"), Anim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTAnimSequence Source;
	Source.Name = TEXT("Source"); Source.StartFrame = 0; Source.NumFrames = 10; Source.Rate = 10.0f;
	FWoTAnimSequence Target;
	Target.Name = TEXT("Target"); Target.StartFrame = 20; Target.NumFrames = 6; Target.Rate = 10.0f;
	Anim->Sequences = {Source, Target};

	Anim->PlayAnim(Source.Name);
	WoTTest::TickWorld(World, 0.12f);
	Anim->TweenAnim(Target.Name, 0.2f);
	WoTTest::TickWorld(World, 0.10f);
	TestTrue(TEXT("tween-only is active during its blend"), Anim->IsAnimating());
	TestTrue(TEXT("tween captures both rendered source frames"),
		Anim->GetMorphTarget(FName(TEXT("F1"))) > 0.0f &&
		Anim->GetMorphTarget(FName(TEXT("F2"))) > 0.0f);
	TestTrue(TEXT("tween mixes in the destination frame"),
		Anim->GetMorphTarget(FName(TEXT("F20"))) > 0.0f);
	TestEqual(TEXT("tween-only remains at target frame zero"), Anim->GetAnimFrame(), 0.0f);
	WoTTest::TickWorld(World, 0.14f);
	TestFalse(TEXT("tween-only stops after the blend"), Anim->IsAnimating());
	TestEqual(TEXT("tween-only holds target frame zero"), Anim->GetAnimFrame(), 0.0f);

	Anim->PlayAnim(Source.Name);
	WoTTest::TickWorld(World, 0.12f);
	Anim->TweenPlayAnim(Target.Name, 1.0f, 0.2f);
	WoTTest::TickWorld(World, 0.10f);
	TestEqual(TEXT("TweenPlay does not advance the sequence during blend"),
		Anim->GetAnimFrame(), 0.0f);
	WoTTest::TickWorld(World, 0.18f);
	TestTrue(TEXT("TweenPlay advances after reaching frame zero"), Anim->GetAnimFrame() > 0.0f);

	Pawn->AnimTableName = TEXT("Whitecloak");
	Pawn->StampSlotAnim(TEXT("AttackRun")); // authored TweenTime is zero
	TestEqual(TEXT("ProcessAnim's unset TweenTime uses the 0.2 fallback"),
		Pawn->RepAnim.TweenTime, 0.2f);
	TestFalse(TEXT("combat stamp is TweenPlay, not tween-only"), Pawn->RepAnim.bTweenOnly != 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20TakeHitDetourSpec,
	"WoT.AI.TakeHitDetour", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20TakeHitDetourSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Subject = SpawnG20Creature(World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTNPCController* Controller = Subject
		? Cast<AWoTNPCController>(Subject->GetController()) : nullptr;
	if (!TestNotNull(TEXT("Trolloc"), Subject) || !TestNotNull(TEXT("controller"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Subject->ChanceOfHitAnim = 1.0f;
	const EWoTBrainState HardReturnState = Controller->GetBrainState();
	Subject->GetCharacterMovement()->Velocity = FVector(200, 0, 0);
	Subject->PlayHitReactAnim(40, TEXT("Sliced"));
	TestEqual(TEXT("hard hit enters TakeHit"), static_cast<int32>(Controller->GetBrainState()),
		static_cast<int32>(EWoTBrainState::TakeHit));
	TestTrue(TEXT("hard hit stops movement"), Subject->GetVelocity().IsNearlyZero());
	TestFalse(TEXT("hard hit uses TweenPlay"), Subject->RepAnim.bTweenOnly != 0);
	WoTTest::TickWorld(World, 0.9f);
	TestEqual(TEXT("hard hit returns to its stored state"),
		static_cast<int32>(Controller->GetBrainState()), static_cast<int32>(HardReturnState));

	Subject->GetCharacterMovement()->Velocity = FVector(175, 0, 0);
	Subject->PlayHitReactAnim(10, TEXT("Sliced"));
	TestEqual(TEXT("soft hit enters TakeHit"), static_cast<int32>(Controller->GetBrainState()),
		static_cast<int32>(EWoTBrainState::TakeHit));
	TestTrue(TEXT("soft hit is tween-only"), Subject->RepAnim.bTweenOnly != 0);
	TestTrue(TEXT("soft hit preserves movement"),
		Subject->GetCharacterMovement()->Velocity.Equals(FVector(175, 0, 0)));
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("soft hit returns after its tween"),
		static_cast<int32>(Controller->GetBrainState()), static_cast<int32>(HardReturnState));

	const uint8 BeforeSmallHit = Subject->RepAnim.PlayToken;
	Subject->PlayHitReactAnim(5, TEXT("Sliced"));
	TestEqual(TEXT("sub-threshold hit has no detour or stamp"),
		Subject->RepAnim.PlayToken, BeforeSmallHit);
	Controller->SetBrainStateForG20Test(EWoTBrainState::PrepareWeapon);
	const uint8 BeforePrepareHit = Subject->RepAnim.PlayToken;
	Subject->PlayHitReactAnim(40, TEXT("Sliced"));
	TestEqual(TEXT("prepare state suppresses TakeHit"),
		static_cast<int32>(Controller->GetBrainState()),
		static_cast<int32>(EWoTBrainState::PrepareWeapon));
	TestEqual(TEXT("prepare suppression stamps no flinch"),
		Subject->RepAnim.PlayToken, BeforePrepareHit);

	AWoTNPCCharacter* BAT = SpawnG20Creature(World, TEXT("BATrolloc"), FVector(500, 0, 100), 1);
	UWoTBATrollocRecoveryBehavior* Recovery = BAT
		? BAT->FindComponentByClass<UWoTBATrollocRecoveryBehavior>() : nullptr;
	AWoTNPCController* BATController = BAT
		? Cast<AWoTNPCController>(BAT->GetController()) : nullptr;
	if (TestNotNull(TEXT("BAT recovery behavior"), Recovery) && BATController)
	{
		Recovery->bNoInitialRecovery = false;
		Recovery->RecoveryInterval = 0.0f;
		WoTTest::TickWorld(World, 0.05f);
		Recovery->NotifyTookDamage(20);
		TestTrue(TEXT("BAT is mid-Recovering"), Recovery->IsRecovering());
		BAT->ChanceOfHitAnim = 1.0f;
		const uint8 BeforeRecoveryHit = BAT->RepAnim.PlayToken;
		BAT->PlayHitReactAnim(100, TEXT("Sliced"));
		TestFalse(TEXT("mid-Recover BAT does not enter TakeHit"),
			BATController->GetBrainState() == EWoTBrainState::TakeHit);
		TestEqual(TEXT("recovery suppression keeps the Recover stamp"),
			BAT->RepAnim.PlayToken, BeforeRecoveryHit);
	}

	AWoTNPCCharacter* Swinger = SpawnG20Creature(World, TEXT("Trolloc"), FVector(1000, 0, 100), 1);
	AWoTTestNPCCharacter* Victim = SpawnG20StillTarget(World, FVector(1040, 0, 100), 2);
	AWoTNPCController* SwingController = Swinger
		? Cast<AWoTNPCController>(Swinger->GetController()) : nullptr;
	if (!TestNotNull(TEXT("interruptible swinger"), Swinger) ||
		!TestNotNull(TEXT("swing victim"), Victim) ||
		!TestNotNull(TEXT("swing controller"), SwingController))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Swinger->ChanceOfHitAnim = 1.0f;
	Swinger->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
	for (int32 Frame = 0; Frame < 240 &&
		SwingController->GetBrainState() != EWoTBrainState::PerformMelee; ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	const int32 VictimHealth = Victim->Health;
	Swinger->PlayHitReactAnim(40, TEXT("Sliced"));
	TestEqual(TEXT("mid-swing hit detours"),
		static_cast<int32>(SwingController->GetBrainState()),
		static_cast<int32>(EWoTBrainState::TakeHit));
	// Observe THROUGH the detour's return and past the old swing's entire notify
	// window: a broken implementation that merely pauses the swing and resumes it
	// after TakeHit would land the old damage after return and fail here.
	for (int32 Frame = 0; Frame < 600 &&
		SwingController->GetBrainState() == EWoTBrainState::TakeHit; ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("detour returned"),
		SwingController->GetBrainState() != EWoTBrainState::TakeHit);
	TestEqual(TEXT("interrupted swing's pending damage notify is cancelled"),
		Victim->Health, VictimHealth);
	// A FRESH post-return swing may legitimately damage later; the cancelled OLD
	// notifies (authored ≤0.70 into the swing) must not fire in the immediate
	// post-return frames before any new episode can reach its own notify.
	WoTTest::TickWorld(World, 0.2f);
	TestEqual(TEXT("no stale notify lands right after return"),
		Victim->Health, VictimHealth);

	// Caster continuation: retail's TakeHit return re-runs the interrupted state's
	// Begin (GetNextStateAndLabelForReturn default), so a hard-hit mid-cast Sister
	// RESTARTS a fresh cast — she must come back to a caster state, not leak to
	// Tracking with the weave silently dropped.
	AWoTNPCCharacter* Caster = SpawnG20Creature(World, TEXT("Sister"), FVector(2000, 0, 100), 1);
	AWoTTestNPCCharacter* CastTarget = SpawnG20StillTarget(World, FVector(2400, 0, 100), 2);
	AWoTNPCController* CasterController = Caster
		? Cast<AWoTNPCController>(Caster->GetController()) : nullptr;
	if (TestNotNull(TEXT("caster"), Caster) &&
		TestNotNull(TEXT("caster controller"), CasterController))
	{
		Caster->ChanceOfHitAnim = 1.0f;
		// Zero the offensive re-use interval: a gated re-pick correctly falls to
		// Tracking (retail's restarted Begin also exits when selection fails), but
		// this leg tests the RESTART path, so selection must be allowed to succeed.
		if (UWoTCasterComponent* CasterComp =
			Caster->FindComponentByClass<UWoTCasterComponent>())
		{
			CasterComp->MinOffensiveUsageInterval = 0.0f;
		}
		// The P1C retail adapter keeps the just-released AdaptedItem excluded.
		// Make the Sister's alternate offensive weaves immediately eligible so
		// this fixture continues to exercise TakeHit's restarted-Begin path.
		for (AWoTInventory* Item : Caster->InventoryComponent->Items)
		{
			if (AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item);
				Weave && Weave->bOffensive)
			{
				Weave->MinChargeGroupInterval = 0.0f;
			}
		}
		Caster->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, CastTarget);
		for (int32 Frame = 0; Frame < 600 &&
			CasterController->GetBrainState() != EWoTBrainState::CasterUse; ++Frame)
		{
			WoTTest::TickWorld(World, 1.0f / 60.0f);
		}
		if (TestEqual(TEXT("caster reaches CasterUse"),
			static_cast<int32>(CasterController->GetBrainState()),
			static_cast<int32>(EWoTBrainState::CasterUse)))
		{
			Caster->PlayHitReactAnim(60, TEXT("Sliced"));
			TestEqual(TEXT("mid-cast hard hit detours"),
				static_cast<int32>(CasterController->GetBrainState()),
				static_cast<int32>(EWoTBrainState::TakeHit));
			for (int32 Frame = 0; Frame < 600 &&
				CasterController->GetBrainState() == EWoTBrainState::TakeHit; ++Frame)
			{
				WoTTest::TickWorld(World, 1.0f / 60.0f);
			}
			TestEqual(TEXT("detour returns to a restarted cast, not Tracking"),
				static_cast<int32>(CasterController->GetBrainState()),
				static_cast<int32>(EWoTBrainState::CasterUse));
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20ArcherChainSpec,
	"WoT.AI.ArcherChain", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20ArcherChainSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Archer = SpawnG20Creature(World, TEXT("Archer"), FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Target = SpawnG20StillTarget(World, FVector(660, 0, 100), 2);
	AWoTNPCController* Controller = Archer
		? Cast<AWoTNPCController>(Archer->GetController()) : nullptr;
	UWoTTestBrainComponent* TestBrain = Archer
		? Cast<UWoTTestBrainComponent>(Archer->Brain) : nullptr;
	if (!TestNotNull(TEXT("Archer"), Archer) || !TestNotNull(TEXT("controller"), Controller) ||
		!TestNotNull(TEXT("injectable brain"), TestBrain))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Archer->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	for (int32 Frame = 0; Frame < 240 && !Controller->IsArcherKneeling(); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestEqual(TEXT("chain begins with KNEEL"), Archer->GetReplicatedAnimName(), FName(TEXT("KNEEL")));
	Archer->ChanceOfHitAnim = 1.0f;
	Archer->PlayHitReactAnim(100, TEXT("Sliced"));
	TestEqual(TEXT("kneeling hard hit uses KNEELHITH"),
		Archer->GetReplicatedAnimName(), FName(TEXT("KNEELHITH")));
	WoTTest::TickWorld(World, 1.0f);

	TArray<FName> Seen = {FName(TEXT("KNEEL"))}; // observed before the hit detour above
	FName Previous = Archer->GetReplicatedAnimName();
	if (Previous == FName(TEXT("Draw")) || Previous == FName(TEXT("RELEASE")))
	{
		Seen.Add(Previous);
	}
	for (int32 Frame = 0; Frame < 900 && !Seen.Contains(FName(TEXT("RELEASE"))); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
		const FName Current = Archer->GetReplicatedAnimName();
		if (Current != Previous && (Current == FName(TEXT("KNEEL")) ||
			Current == FName(TEXT("Draw")) || Current == FName(TEXT("RELEASE"))))
		{
			Seen.Add(Current);
			Previous = Current;
		}
	}
	const int32 KneelIndex = Seen.IndexOfByKey(FName(TEXT("KNEEL")));
	const int32 DrawIndex = Seen.IndexOfByKey(FName(TEXT("Draw")));
	const int32 ReleaseIndex = Seen.IndexOfByKey(FName(TEXT("RELEASE")));
	TestTrue(TEXT("real-time chain orders kneel -> draw -> release"),
		KneelIndex != INDEX_NONE && DrawIndex > KneelIndex && ReleaseIndex > DrawIndex);

	// Force LOS off for the next Draw completion: the Archer must hold the drawn
	// final frame, then use the 0.15s poll + Ricky pause when visibility is injected.
	// The wait below can catch a Draw MID-phase: that Draw completes while the goal
	// is still visible, releases once more, and only the NEXT full Draw (~4.3s at
	// the authored 24f/0.4 rate) lands in Hold — so the Hold wait must cover a
	// release + draw cycle, not one phase (the 300-frame version flaked).
	for (int32 Frame = 0; Frame < 300 &&
		Controller->GetArcherAttackPhaseName() != FName(TEXT("Draw")); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestBrain->bForceVisible = false;
	// The goal-visibility cache is position-pair keyed (retail: stationary LOS
	// can't change; movement invalidates; FWoTGoalCache::CurrentThreshold = 32uu).
	// Move the target PAST the drift threshold so the injected flag is actually
	// re-queried instead of served from the stamped TRUE.
	Target->SetActorLocation(FVector(660, 48, 100));
	for (int32 Frame = 0; Frame < 900 &&
		Controller->GetArcherAttackPhaseName() != FName(TEXT("Hold")); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestEqual(TEXT("unseen goal holds the bow drawn"),
		Controller->GetArcherAttackPhaseName(), FName(TEXT("Hold")));
	// Remain in this state long enough to satisfy CheckForMeleeSwitch's strict
	// five-second gate while staying inside Hold's own five-second expiry (which
	// ends the attack via GetUp — Archer.uc:236-238 + LegendPawn.uc:1734-1738):
	// visibility lands at 4.8s, the 0.15s poll fires by 4.95s, before the 5.0s cut.
	WoTTest::TickWorld(World, 4.8f);
	TestBrain->bForceVisible = true;
	Target->SetActorLocation(FVector(660, 96, 100)); // >32uu cache-bust (see above)
	WoTTest::TickWorld(World, 0.34f);
	TestEqual(TEXT("visibility poll plus Ricky pause reaches Release"),
		Controller->GetArcherAttackPhaseName(), FName(TEXT("Release")));

	// The state has now lasted >5s. Closing inside 320 before Release finishes
	// commits the sword, then runs GETUP before returning to tracking.
	Target->SetActorLocation(FVector(80, 0, 100));
	// The freshly entered Release phase runs its full authored length (~4.3s)
	// before the melee-switch check fires — cover it with headroom.
	for (int32 Frame = 0; Frame < 600 &&
		Controller->GetArcherAttackPhaseName() != FName(TEXT("GetUp")); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestEqual(TEXT("close target after exit interval routes to GETUP"),
		Controller->GetArcherAttackPhaseName(), FName(TEXT("GetUp")));

	// WOTPawn.uc:172 switch gate: the melee commit stamped LastWeaponSwitchTime, so
	// reopening the range does NOT re-arm the bow for MinTimeBetweenWeaponSwitch=5
	// (Archer.uc:258); after the gate the bow re-arms AND the held sword clears
	// (the bow is component-backed — the held-actor seam goes to body/none).
	if (UWoTArcherBowBehavior* BowBehavior =
		Archer->FindComponentByClass<UWoTArcherBowBehavior>())
	{
		// Far enough that the chasing Archer (GroundSpeed 750) cannot close back
		// inside SwitchToMeleeDistance during the gate window.
		Target->SetActorLocation(FVector(6000, 0, 100));
		WoTTest::TickWorld(World, 1.0f);
		TestTrue(TEXT("switch gate holds melee commit inside 5s"),
			BowBehavior->IsCommittedToMelee());
		bool bSawSwordHeldDuringPreparation = false;
		bool bSawPrematureBowLatch = false;
		for (int32 Frame = 0; Frame < 360 && Archer->GetHeldWeapon(); ++Frame)
		{
			WoTTest::TickWorld(World, 1.0f / 60.0f);
			const bool bPreparingHeldSword =
				Archer->GetReplicatedAnimName() == FName(TEXT("DRAWSWRD"))
				&& Archer->GetHeldWeapon() == Archer->GetMeleeWeapon();
			bSawSwordHeldDuringPreparation |= bPreparingHeldSword;
			bSawPrematureBowLatch |= bPreparingHeldSword
				&& !BowBehavior->IsCommittedToMelee();
		}
		TestTrue(TEXT("re-arm leaves the sword held until DRAWSWRD's notify"),
			bSawSwordHeldDuringPreparation);
		TestFalse(TEXT("bow latch waits for DRAWSWRD's notify"),
			bSawPrematureBowLatch);
		for (int32 Frame = 0; Frame < 360
			&& BowBehavior->IsCommittedToMelee(); ++Frame)
		{
			WoTTest::TickWorld(World, 1.0f / 60.0f);
		}
		TestFalse(TEXT("bow re-arms after the 5s gate and notify"),
			BowBehavior->IsCommittedToMelee());
		TestNull(TEXT("re-arming clears the held sword"), Archer->GetHeldWeapon());
	}

	AWoTNPCCharacter* DyingArcher = SpawnG20Creature(
		World, TEXT("Archer"), FVector(1500, 0, 100), 1);
	AWoTTestNPCCharacter* DeathTarget = SpawnG20StillTarget(
		World, FVector(2160, 0, 100), 2);
	AWoTNPCController* DyingController = DyingArcher
		? Cast<AWoTNPCController>(DyingArcher->GetController()) : nullptr;
	if (!TestNotNull(TEXT("death-override Archer"), DyingArcher) ||
		!TestNotNull(TEXT("death-override target"), DeathTarget) ||
		!TestNotNull(TEXT("death-override controller"), DyingController))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	DyingArcher->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, DeathTarget);
	for (int32 Frame = 0; Frame < 240 && !DyingController->IsArcherKneeling(); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	// This spec isolates the authored non-gib kneeling death animation; killing-
	// blow gib coverage lives in WoT.Parity.E.PawnKillingBlowGib.
	DyingArcher->Health = 1;
	DyingArcher->TakeRawDamage(1, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestEqual(TEXT("death while kneeling uses DEATHKNEEL"),
		DyingArcher->GetReplicatedAnimName(), FName(TEXT("DEATHKNEEL")));

	// Never-visible timeout: Sleep(BowDrawnSleepDuration) expiring calls the
	// PerformingAttack FUNCTION -> PostPerformingAttack/GetUp (Archer.uc:236-238,
	// LegendPawn.uc:1734-1738) — the attack ENDS; no redraw loop.
	AWoTNPCCharacter* BlindArcher = SpawnG20Creature(
		World, TEXT("Archer"), FVector(3000, 0, 100), 1);
	AWoTTestNPCCharacter* HiddenTarget = SpawnG20StillTarget(
		World, FVector(3660, 0, 100), 2);
	AWoTNPCController* BlindController = BlindArcher
		? Cast<AWoTNPCController>(BlindArcher->GetController()) : nullptr;
	UWoTTestBrainComponent* BlindBrain = BlindArcher
		? Cast<UWoTTestBrainComponent>(BlindArcher->Brain) : nullptr;
	if (!TestNotNull(TEXT("timeout Archer"), BlindArcher) ||
		!TestNotNull(TEXT("timeout controller"), BlindController) ||
		!TestNotNull(TEXT("timeout brain"), BlindBrain))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BlindArcher->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, HiddenTarget);
	for (int32 Frame = 0; Frame < 240 && !BlindController->IsArcherKneeling(); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	// Go invisible before the first Draw completes (>32uu cache-bust nudge).
	BlindBrain->bForceVisible = false;
	HiddenTarget->SetActorLocation(FVector(3660, 48, 100));
	for (int32 Frame = 0; Frame < 900 &&
		BlindController->GetArcherAttackPhaseName() != FName(TEXT("Hold")); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestEqual(TEXT("timeout leg reaches Hold"),
		BlindController->GetArcherAttackPhaseName(), FName(TEXT("Hold")));
	// Ride out the full 5s hold plus the GetUp sequence: the chain must END.
	WoTTest::TickWorld(World, 5.2f);
	TestTrue(TEXT("expired hold ends the attack (GetUp or already back tracking)"),
		BlindController->GetArcherAttackPhaseName() == FName(TEXT("GetUp")) ||
		BlindController->GetBrainState() != EWoTBrainState::PerformProjectile);
	for (int32 Frame = 0; Frame < 600 &&
		BlindController->GetBrainState() == EWoTBrainState::PerformProjectile; ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("never-visible Archer leaves the attack chain"),
		BlindController->GetBrainState() != EWoTBrainState::PerformProjectile);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20WeaponLossSwitchSpec,
	"WoT.AI.WeaponLossSwitching", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20WeaponLossSwitchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Questioner = SpawnG20Creature(
		World, TEXT("Questioner"), FVector(0, 0, 100), 1);
	AWoTNPCController* QuestionerController = Questioner
		? Cast<AWoTNPCController>(Questioner->GetController()) : nullptr;
	AWoTNPCWeapon* Shield = Questioner ? Questioner->GetMeleeWeapon() : nullptr;
	if (!TestNotNull(TEXT("Questioner"), Questioner) ||
		!TestNotNull(TEXT("Questioner controller"), QuestionerController) ||
		!TestNotNull(TEXT("Questioner shield"), Shield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Shield->TakeDamage(5000.0f, FDamageEvent(), nullptr, Questioner);
	TestNull(TEXT("destroyed shield clears melee slot"), Questioner->GetMeleeWeapon());
	QuestionerController->RouteWeaponForProximity(EWoTGoalProximity::Reached);
	TestNull(TEXT("next selection routes the Questioner to body attack"),
		Questioner->GetHeldWeapon());
	TestEqual(TEXT("weapon loss selects MeleeAttack animation slot"),
		Questioner->GetEffectiveMeleeAnimSlot(), FName(TEXT("MeleeAttack")));

	AWoTNPCCharacter* BAT = SpawnG20Creature(World, TEXT("BATrolloc"), FVector(500, 0, 100), 1);
	AWoTTestNPCCharacter* Target = SpawnG20StillTarget(World, FVector(1200, 0, 100), 2);
	AWoTNPCWeapon* Halberd = BAT ? BAT->GetRangedWeapon() : nullptr;
	if (TestNotNull(TEXT("BATrolloc"), BAT) && TestNotNull(TEXT("throw target"), Target) &&
		TestNotNull(TEXT("BAT halberd"), Halberd))
	{
		Halberd->UseProjectileWeapon(BAT, Target);
		TestTrue(TEXT("halberd enters in-flight state"), Halberd->IsInFlight());
		TestNull(TEXT("throw clears the held-weapon state"), BAT->GetHeldWeapon());
		TestEqual(TEXT("in-flight melee uses bare-handed MeleeAttack"),
			BAT->GetEffectiveMeleeAnimSlot(), FName(TEXT("MeleeAttack")));
		for (TActorIterator<AWoTWeaponProjectile> It(World); It; ++It)
		{
			if (It->GetOwner() == Halberd) { It->Destroy(); break; }
		}
		WoTTest::TickWorld(World, 0.05f);
		TestFalse(TEXT("halberd return clears in-flight state"), Halberd->IsInFlight());
		TestEqual(TEXT("returned halberd is held again"), BAT->GetHeldWeapon(), Halberd);
		TestEqual(TEXT("returned halberd restores MeleeWeaponAttack"),
			BAT->GetEffectiveMeleeAnimSlot(), FName(TEXT("MeleeWeaponAttack")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20WarderFadeNotifySpec,
	"WoT.AI.WarderFadeNotifies", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20WarderFadeNotifySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = SpawnG20Creature(World, TEXT("Warder"), FVector(0, 0, 100), 1);
	AWoTNPCController* Controller = Warder
		? Cast<AWoTNPCController>(Warder->GetController()) : nullptr;
	UWoTWarderFadeCloakBehavior* Fade = Warder
		? Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>() : nullptr;
	if (!TestNotNull(TEXT("Warder"), Warder) ||
		!TestNotNull(TEXT("Warder controller"), Controller) ||
		!TestNotNull(TEXT("Warder fade behavior"), Fade))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Fade->TickBehavior(0.0f); // establish WaitingIdle's default fade-out wiring
	Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	Fade->TickBehavior(0.0f); // EndState -> DisableFade / normal 1s fade-in
	Fade->SetFadeLevel(0.0f);
	Fade->IncreaseVisibilitySlow();
	Fade->TickBehavior(1.0f);
	TestEqual(TEXT("IncreaseVisibilitySlow selects the 10s rate"),
		Fade->GetActiveFadeChangeTime(), 10.0f);
	TestTrue(TEXT("one second of slow fade-in advances about one tenth"),
		FMath::IsNearlyEqual(Fade->GetFadeLevel(), 100.0f, 1.0f));

	Fade->SetFadeLevel(1000.0f);
	Fade->DecreaseVisibilitySlow();
	Fade->TickBehavior(1.0f);
	TestEqual(TEXT("DecreaseVisibilitySlow selects the authored 2s rate"),
		Fade->GetActiveFadeChangeTime(), 2.0f);
	TestTrue(TEXT("one second of slow fade-out reaches half level"),
		FMath::IsNearlyEqual(Fade->GetFadeLevel(), 500.0f, 1.0f));

	Fade->SetFadeLevel(0.0f);
	TestTrue(TEXT("faded endpoint selects modulated body skins"),
		Warder->GetFadeSkinIndex() != AWoTNPCCharacter::VisibleFadeSkinIndex);
	if (AWoTNPCWeapon* Weapon = Warder->GetHeldWeapon())
	{
		TestTrue(TEXT("faded endpoint makes the weapon translucent"), Weapon->bStyleTranslucent);
	}
	Fade->SetFadeLevel(1000.0f);
	TestEqual(TEXT("visible endpoint restores body style/material marker"),
		Warder->GetFadeSkinIndex(), AWoTNPCCharacter::VisibleFadeSkinIndex);
	if (AWoTNPCWeapon* Weapon = Warder->GetHeldWeapon())
	{
		TestFalse(TEXT("visible endpoint restores weapon style"), Weapon->bStyleTranslucent);
	}
	Fade->SetFadeLevel(0.0f);
	Warder->TakeRawDamage(5000, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("Died restores visible body style"), Warder->GetFadeSkinIndex(),
		AWoTNPCCharacter::VisibleFadeSkinIndex);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG20LegionStompNotifySpec,
	"WoT.AI.LegionStompNotify", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG20LegionStompNotifySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = SpawnG20Creature(World, TEXT("Legion"), FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Target = SpawnG20StillTarget(World, FVector(400, 0, 100), 2);
	UWoTLegionSpiritsStompBehavior* Stomp = Legion
		? Legion->FindComponentByClass<UWoTLegionSpiritsStompBehavior>() : nullptr;
	if (!TestNotNull(TEXT("Legion"), Legion) || !TestNotNull(TEXT("stomp target"), Target) ||
		!TestNotNull(TEXT("Legion stomp behavior"), Stomp))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Capture BEFORE the threat exists: any pre-notify damage fails the "remains
	// pending" assert instead of being silently folded into the baseline.
	const int32 HealthBeforeThreat = Target->Health;
	Legion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	for (int32 Frame = 0; Frame < 180 && !Stomp->IsStompPending(); ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("stomp episode starts"), Stomp->IsStompPending());
	TestEqual(TEXT("no damage before the notify"), Target->Health, HealthBeforeThreat);

	// Compute the expected notify time INDEPENDENTLY of the SUT: identify the
	// picked sequence from the replicated stamp, take its authored SpawnDamageWave1
	// fraction (ATTACKSTOMP1 @0.65 / ATTACKSTOMP2 @0.4, Legion.uc:24/29), and use
	// the Legion table row's rate/tween (rows 11-12: rate 0.55, tween 0.2).
	const FName Picked = Legion->GetReplicatedAnimName();
	TestTrue(TEXT("a stomp sequence was stamped"),
		Picked == FName(TEXT("ATTACKSTOMP1")) || Picked == FName(TEXT("ATTACKSTOMP2")));
	const float Fraction = Picked == FName(TEXT("ATTACKSTOMP1")) ? 0.65f : 0.4f;
	const FWoTAnimTableEntry* Row = WoTAnimTable::PickSlotAnim(
		Legion->AnimTableName, Picked, /*Source*/ nullptr, /*bNotSlot: exact anim*/ true);
	const float Rate = WoTAnimTable::ResolveRate(Row);
	const float Tween = WoTAnimTable::ResolveTweenTime(Row);
	const float Expected = Tween + Fraction * Legion->SequenceDurationSeconds(Picked, Rate);
	TestEqual(TEXT("SUT notify time matches the independent computation"),
		Stomp->GetPendingStompNotifyTime(), Expected, 2.0f / 60.0f);
	WoTTest::TickWorld(World, FMath::Max(Expected - 0.10f, 0.01f));
	TestEqual(TEXT("damage remains pending before the authored fraction"),
		Target->Health, HealthBeforeThreat);
	WoTTest::TickWorld(World, 0.20f);
	TestTrue(TEXT("damage lands as the picked stomp crosses its authored notify"),
		Target->Health < HealthBeforeThreat);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
