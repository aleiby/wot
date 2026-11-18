// Class automation specs for the NPC weapon layer (WotWeapon subsystem + the
// Archer bow): the target-leading math (WotWeapon.uc:568 AdjustProjectileWeapondAim
// / :592 lead) and the end-to-end Archer fight — a bow-armed Whitecloak captain
// kneels and fires arrows at range, then commits to melee when its target closes.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTRangeBands.h"
#include "../AI/WoTWeaponComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "Components/SphereComponent.h"
#include "WoTTestAIClasses.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"

namespace
{
	// A forced-nav (no navmesh headless), flying combat pawn on Team, its
	// controller removed so it holds still as a firing/melee target.
	AWoTTestNPCCharacter* SpawnStillTarget(UWorld* World, const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* NPC =
			World->SpawnActor<AWoTTestNPCCharacter>(Location, FRotator::ZeroRotator, Params);
		NPC->AssignedTeam = Team;
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		if (AController* C = NPC->GetController())
		{
			C->UnPossess();
		}
		return NPC;
	}

	void SpawnWeaponActivityAnchor(UWorld* World)
	{
		AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator);
		Anchor->Health = 0;
		Anchor->bDead = true;
		Anchor->SetActorHiddenInGame(true);
		Anchor->SetActorEnableCollision(false);
	}
}

// --- Lead math: the intercept matches WotWeapon.AdjustProjectileWeapondAim ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWeaponLeadAimTest,
	"WoT.Weapon.LeadAim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWeaponLeadAimTest::RunTest(const FString&)
{
	// The bare intercept (WotWeapon.uc:592-594): ProjectileDest = TargetLoc +
	// FMin(1, 0.7 + 0.6*FRand()) * (TargetVel * dist / ProjectileSpeed). Drive the
	// FRand term explicitly to lock the formula.
	const FVector TargetLoc(1000.0f, 0.0f, 0.0f);
	const FVector TargetVel(0.0f, 500.0f, 0.0f);
	const FVector Start(0.0f, 0.0f, 0.0f);
	const float Speed = 16384.0f;                 // ArcherArrow.uc:37
	const float Dist = (TargetLoc - Start).Size();

	// FRand -> 1.0: FMin(1, 1.3) = 1.0 (full lead).
	const FVector DestFull =
		UWoTWeaponComponent::ComputeLeadDestination(TargetLoc, TargetVel, Start, Speed, 1.0f);
	const float ExpectedFullY = TargetVel.Y * Dist / Speed;   // 500*1000/16384
	TestTrue(FString::Printf(TEXT("full lead Y = %.4f (expected %.4f)"), DestFull.Y, ExpectedFullY),
	         FMath::IsNearlyEqual(DestFull.Y, ExpectedFullY, 0.01f));
	TestTrue(TEXT("full lead keeps target X"), FMath::IsNearlyEqual(DestFull.X, TargetLoc.X, 0.01f));

	// FRand -> 0.0: FMin(1, 0.7) = 0.7 (partial lead).
	const FVector DestPartial =
		UWoTWeaponComponent::ComputeLeadDestination(TargetLoc, TargetVel, Start, Speed, 0.0f);
	TestTrue(FString::Printf(TEXT("partial lead Y = %.4f (expected %.4f)"), DestPartial.Y, 0.7f * ExpectedFullY),
	         FMath::IsNearlyEqual(DestPartial.Y, 0.7f * ExpectedFullY, 0.01f));

	// A faster projectile leads less (shorter time of flight).
	const FVector DestFast =
		UWoTWeaponComponent::ComputeLeadDestination(TargetLoc, TargetVel, Start, 2.0f * Speed, 1.0f);
	TestTrue(TEXT("double speed halves the lead"),
	         FMath::IsNearlyEqual(DestFast.Y, 0.5f * ExpectedFullY, 0.01f));

	// The full aim (uc:568) applied through a live shooter: a target moving +Y is
	// led to +Y (positive yaw), a stationary target is aimed straight (~zero yaw).
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Shooter = SpawnStillTarget(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Mark = SpawnStillTarget(World, FVector(1000, 0, 100), 2);
	const FVector Muzzle = UWoTWeaponComponent::GetAimSource(Shooter, Mark->GetActorLocation());

	Mark->GetCharacterMovement()->Velocity = FVector(0, 500, 0);
	const FRotator LeadAim =
		UWoTWeaponComponent::AdjustProjectileWeaponAim(Shooter, Mark, Speed, Muzzle, 1.0f);
	TestTrue(FString::Printf(TEXT("moving target leads +Y (yaw %.3f deg)"), LeadAim.Yaw),
	         LeadAim.Yaw > 0.3f && LeadAim.Yaw < 5.0f);

	Mark->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	const FRotator StraightAim =
		UWoTWeaponComponent::AdjustProjectileWeaponAim(Shooter, Mark, Speed, Muzzle, 1.0f);
	TestTrue(FString::Printf(TEXT("stationary target aimed straight (yaw %.3f deg)"), StraightAim.Yaw),
	         FMath::Abs(StraightAim.Yaw) < 0.2f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Archer bow: a row NPC fires arrows at range, then closes to melee ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWeaponArcherBowTest,
	"WoT.Weapon.ArcherBow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWeaponArcherBowTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnWeaponActivityAnchor(World);

	// The Archer creature row on a forced-nav test pawn (bForceVisible/Reachable —
	// no navmesh headless). SpawnCreature runs ConfigureNPC (equips the ArcherBow
	// behavior) then possesses; OnPossess equips the bow + hands out the
	// projectile-fighter band ladder.
	AWoTNPCCharacter* Archer = WoTCreatureData::SpawnCreature(
		*World, TEXT("Archer"), FVector(0, 0, 100), /*Team*/ 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Archer spawned from the row"), Archer))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Archer->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// The bow was equipped and the ladder prefers visibility (kneel-and-shoot).
	UWoTWeaponComponent* Bow = Archer->FindComponentByClass<UWoTWeaponComponent>();
	UWoTArcherBowBehavior* BowHook = Cast<UWoTArcherBowBehavior>(
		Archer->FindComponentByClass<UWoTCreatureBehavior>());
	AWoTNPCController* Controller = Cast<AWoTNPCController>(Archer->GetController());
	if (!TestNotNull(TEXT("Archer equipped a UWoTWeaponComponent (bow)"), Bow) ||
	    !TestNotNull(TEXT("Archer has the ArcherBow behavior"), BowHook) ||
	    !TestNotNull(TEXT("Archer possessed by the NPC controller"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("bow fires the ArcherArrow"),
	          Bow->ProjectileClass.Get(), AWoTArcherArrow::StaticClass());
	TestTrue(TEXT("projectile-fighter ladder prefers visibility"),
	         Controller->GetTrackingBands().bPreferVisibility);

	// A still enemy at 660uu (cylinder gap ~600 — inside the bow window 150..10000,
	// beyond SwitchToMeleeDistance 320, so the Archer stays in bow mode).
	AWoTTestNPCCharacter* Enemy = SpawnStillTarget(World, FVector(660, 0, 100), 2);
	const int32 EnemyStartHealth = 400;
	Enemy->Health = EnemyStartHealth;
	Enemy->HealthMPMin = Enemy->HealthMPMax = EnemyStartHealth;   // BeginPlay re-roll
	Enemy->MeleeRange = 0.0f;                                     // no counter-attack

	// Threat -> Tracking -> the Offensive pulse routes to the bow: kneel, draw,
	// release an arrow. Poll while ticking — a landed arrow destroys itself.
	Archer->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	int32 ArrowsSeen = 0;
	for (int32 Chunk = 0; Chunk < 70; ++Chunk)   // ~7s: KNEEL + Draw + Release
	{
		WoTTest::TickWorld(World, 0.1f);
		for (TActorIterator<AWoTArcherArrow> It(World); It; ++It)
		{
			++ArrowsSeen;
		}
	}
	const int32 HealthAfterVolley = Enemy->Health;
	TestTrue(FString::Printf(TEXT("bow volley damaged the enemy at range (%d in-flight arrows, %d->%d hp)"),
	                         ArrowsSeen, EnemyStartHealth, HealthAfterVolley),
	         ArrowsSeen > 0 || HealthAfterVolley < EnemyStartHealth);
	TestTrue(TEXT("enemy took arrow damage at range"), HealthAfterVolley < EnemyStartHealth);
	TestFalse(TEXT("Archer still in bow mode while the enemy is far"), BowHook->IsCommittedToMelee());

	// The enemy closes into melee range (cylinder gap well inside MeleeRange 50, but
	// the cylinders stay apart so neither pawn is shoved). After the bow-engagement
	// interval the Archer commits to melee (Archer.uc:167 CheckForMeleeSwitch) and
	// swings its sword damage (the MeleeAttacks set).
	Enemy->SetActorLocation(FVector(80, 0, 100));
	WoTTest::TickWorld(World, 7.0f);

	TestTrue(TEXT("Archer committed to melee once the enemy closed"),
	         BowHook->IsCommittedToMelee());
	TestFalse(TEXT("prefer-visibility dropped for the melee commit"),
	          Controller->GetTrackingBands().bPreferVisibility);
	TestTrue(FString::Printf(TEXT("enemy took melee damage after closing (%d->%d hp)"),
	                         HealthAfterVolley, Enemy->Health),
	         Enemy->Health < HealthAfterVolley);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Arrow wall-embed geometry: divergence register #5, RESTORE-RULED ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWeaponArrowZeroExtentTest,
	"WoT.Weapon.ArrowZeroExtent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWeaponArrowZeroExtentTest::RunTest(const FString&)
{
	// Retail arrows are zero-extent (engine Projectile CollisionRadius/
	// Height=0, no override in WotWeaponProjectile.uc/ArcherArrow.uc): the
	// origin stops ON the wall so the shaft embeds, and pawn hits are a
	// point trace. The 2uu sphere is the sweep-safe stand-in; the UE5
	// 32uu USphereComponent default floated arrows off walls and widened
	// hit detection.
	const AWoTArcherArrow* ArrowCDO =
		GetDefault<AWoTArcherArrow>(AWoTArcherArrow::StaticClass());
	if (!TestNotNull(TEXT("arrow CDO"), ArrowCDO))
	{
		return false;
	}
	const USphereComponent* Sphere =
		Cast<USphereComponent>(ArrowCDO->GetRootComponent());
	if (!TestNotNull(TEXT("arrow root collision sphere"), Sphere))
	{
		return false;
	}
	TestTrue(TEXT("arrow collision approximates retail zero-extent (<=2uu)"),
		Sphere->GetUnscaledSphereRadius() <= 2.0f + KINDA_SMALL_NUMBER);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
