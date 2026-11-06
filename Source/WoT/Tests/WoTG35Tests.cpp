// G35 headless specs: the WOTExplodingWall break path (WOTExplodingWall.uc:34-113),
// its exact-match-only damage gate (deliberately NOT the mover's — no wildcard
// ActivatedBy entry, no DamageTypeContains), and the parity-locked MP presence
// (an invisible projectile occluder that never blocks pawns).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTProjectile.h"
#include "../WoTExplodingWall.h"
#include "../WoTFragment.h"

#include "Components/CapsuleComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	// The six identical MP authors (Battle2_05 x3 + Battle4_01 x3): silent
	// 8-wood-chunk break gated on 'break01', which shipped MP never emits.
	AWoTExplodingWall* SpawnG35AuthoredWall(UWorld* World)
	{
		AWoTExplodingWall* Wall = World->SpawnActor<AWoTExplodingWall>();
		Wall->ExplosionSize = 100.0f;
		Wall->WoodParticleSize = 2.5f;
		Wall->NumWallChunks = 0;
		Wall->NumWoodChunks = 8;
		Wall->WoodTextureName = FName(TEXT("WoodFlor3670"));
		Wall->Health = 15;
		Wall->ActivatedBy = {FName(TEXT("break01")), NAME_None, NAME_None,
			NAME_None, NAME_None};
		return Wall;
	}

	FWoTDamageEvent G35SpecDamage(FName Type)
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = Type;
		Event.UE1Momentum = FVector(8000000.0f, 0.0f, 0.0f);
		return Event;
	}

	int32 CountG35SpecFragments(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTFragment> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

// (a) The authored MP config breaks under its own gate: two 8-damage 'break01'
// hits (15 -> 7 -> -1) shed exactly 8 wood fragments carrying the authored
// WoodFlor3670 skin at scale 2.5, and the wall itself is destroyed (uc:112 —
// unlike the mover, nothing remains).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG35AuthoredBreakSpec,
	"WoT.ExplodingWall.AuthoredBreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG35AuthoredBreakSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingWall* Wall = SpawnG35AuthoredWall(World);

	const FWoTDamageEvent Break = G35SpecDamage(TEXT("break01"));
	Wall->TakeDamage(8.0f, Break, nullptr, nullptr);
	TestFalse(TEXT("the first hit leaves the wall standing"), Wall->HasExploded());
	TestEqual(TEXT("the first hit lands its damage"), Wall->Health, 7);
	TestEqual(TEXT("no debris before the break"), CountG35SpecFragments(World), 0);

	Wall->TakeDamage(8.0f, Break, nullptr, nullptr);
	TestTrue(TEXT("the second hit breaks the wall"), Wall->HasExploded());
	TestEqual(TEXT("exactly the 8 authored wood chunks spawn"),
		CountG35SpecFragments(World), 8);
	for (TActorIterator<AWoTFragment> It(World); It; ++It)
	{
		TestEqual(TEXT("each chunk carries the authored wood skin"),
			It->SkinName, FName(TEXT("WoodFlor3670")));
		TestEqual(TEXT("each chunk carries the authored particle size"),
			It->MeshScale, 2.5f);
	}
	TestFalse(TEXT("the broken wall is destroyed outright"), IsValid(Wall));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (b) The MP-census pin: the shipped weave damage vocabulary (AEFWS combos,
// 'hurt') can never pass the six authors' 'break01' exact gate — the reason the
// placed walls are inert (never explode) in shipped MP. Parity means WE never
// break them either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG35MPInertSpec,
	"WoT.ExplodingWall.MPWeavesInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG35MPInertSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingWall* Wall = SpawnG35AuthoredWall(World);

	Wall->TakeDamage(100.0f, G35SpecDamage(TEXT("xxFxx")), nullptr, nullptr);
	Wall->TakeDamage(100.0f, G35SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestFalse(TEXT("MP weave damage never breaks the authored walls"),
		Wall->HasExploded());
	TestEqual(TEXT("rejected damage types leave Health untouched"), Wall->Health, 15);
	TestEqual(TEXT("no debris sheds"), CountG35SpecFragments(World), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (c) The gate differences vs the mover (uc:50-57 vs ExplodingMover.uc:45):
// an ActivatedBy entry of 'All' is NOT a wildcard (the wall has no such
// clause), incoming DamageType 'All' DOES bypass the gate, and there is no
// DamageTypeContains element hierarchy — ActivatedBy=[Fire] rejects 'xxFxx',
// the compound the mover accepts (FWoTExplodingMoverElementHierarchySpec).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG35GateDifferenceSpec,
	"WoT.ExplodingWall.ExactMatchGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG35GateDifferenceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTExplodingWall* AllEntry = World->SpawnActor<AWoTExplodingWall>();
	AllEntry->Health = 5;
	AllEntry->ActivatedBy = {FName(TEXT("All")), NAME_None, NAME_None,
		NAME_None, NAME_None};
	AllEntry->TakeDamage(10.0f, G35SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestFalse(TEXT("an ActivatedBy 'All' entry is not a wildcard"),
		AllEntry->HasExploded());
	TestEqual(TEXT("the rejected hit leaves Health untouched"), AllEntry->Health, 5);

	AWoTExplodingWall* IncomingAll = World->SpawnActor<AWoTExplodingWall>();
	IncomingAll->Health = 5;
	IncomingAll->ActivatedBy = {FName(TEXT("Fire")), NAME_None, NAME_None,
		NAME_None, NAME_None};
	IncomingAll->TakeDamage(5.0f, G35SpecDamage(TEXT("All")), nullptr, nullptr);
	TestTrue(TEXT("incoming DamageType 'All' bypasses the gate"),
		IncomingAll->HasExploded());

	AWoTExplodingWall* FireWall = World->SpawnActor<AWoTExplodingWall>();
	FireWall->Health = 5;
	FireWall->ActivatedBy = {FName(TEXT("Fire")), NAME_None, NAME_None,
		NAME_None, NAME_None};
	FireWall->TakeDamage(10.0f, G35SpecDamage(TEXT("xxFxx")), nullptr, nullptr);
	TestFalse(TEXT("no element hierarchy: a fire compound misses ActivatedBy=Fire"),
		FireWall->HasExploded());
	TestEqual(TEXT("the compound miss leaves Health untouched"), FireWall->Health, 5);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (d) Exploding.Trigger (uc:36-39) is unconditional: it ignores
// ActivatedBy/Health entirely, and bOnlyTriggerable gates ONLY TakeDamage
// (uc:47-48) — the trigger leg stays live.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG35TriggerLegSpec,
	"WoT.ExplodingWall.TriggerLeg",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG35TriggerLegSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTExplodingWall* Triggered = SpawnG35AuthoredWall(World);
	Triggered->Health = 1000;
	Triggered->WoTTrigger(nullptr, nullptr);
	TestTrue(TEXT("Trigger explodes regardless of gate and Health"),
		Triggered->HasExploded());

	AWoTExplodingWall* TriggerOnly = SpawnG35AuthoredWall(World);
	TriggerOnly->bOnlyTriggerable = true;
	TriggerOnly->TakeDamage(100.0f, G35SpecDamage(TEXT("break01")), nullptr, nullptr);
	TestFalse(TEXT("bOnlyTriggerable blocks TakeDamage"), TriggerOnly->HasExploded());
	TestEqual(TEXT("blocked damage leaves Health untouched"), TriggerOnly->Health, 15);
	TriggerOnly->WoTTrigger(nullptr, nullptr);
	TestTrue(TEXT("bOnlyTriggerable does NOT block Trigger"),
		TriggerOnly->HasExploded());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (e) The parity-locked MP presence: the retail 32x32 cylinder is a pure weave
// occluder (bProjTarget=True) that never blocks pawns (bBlockActors/
// bBlockPlayers false) — the capsule blocks only the WoTProjectile channel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG35OccluderCollisionSpec,
	"WoT.ExplodingWall.ProjectileOccluder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG35OccluderCollisionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingWall* Wall = World->SpawnActor<AWoTExplodingWall>();

	TestEqual(TEXT("the flat cylinder keeps the UE1 32x32 radius"),
		Wall->Capsule->GetUnscaledCapsuleRadius(), 32.0f);
	TestEqual(TEXT("the flat cylinder keeps the UE1 32x32 half-height"),
		Wall->Capsule->GetUnscaledCapsuleHalfHeight(), 32.0f);
	TestTrue(TEXT("query-only collision (no physics body)"),
		Wall->Capsule->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	TestEqual(TEXT("weave projectiles overlap the wall and dispatch Touch"),
		Wall->Capsule->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestTrue(TEXT("the wall generates projectile overlap events"),
		Wall->Capsule->GetGenerateOverlapEvents());
	TestTrue(TEXT("the wall is a retail projectile Touch target"),
		Wall->IsRetailProjectileTouchTarget());
	TestEqual(TEXT("pawns pass through (bBlockActors/bBlockPlayers false)"),
		Wall->Capsule->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);

	// The clobber legs (WoTSkyBoxTests precedent — a plain spawn never runs
	// FBodyInstance::LoadProfileData, so the asserts above alone would stay
	// green with the PostInitializeComponents re-assert deleted).
	// Leg 1: the PostLoad profile reapplication must not disturb the contract.
	Wall->Capsule->GetBodyInstance()->LoadProfileData(false);
	TestEqual(TEXT("occluder contract survives a profile reload"),
		Wall->Capsule->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestEqual(TEXT("pawn ignore survives a profile reload"),
		Wall->Capsule->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);

	// Leg 2: stale-umap shape — an instance deserialized with a clobbered
	// blocking profile must be healed back at init.
	AWoTExplodingWall* Stale = World->SpawnActorDeferred<AWoTExplodingWall>(
		AWoTExplodingWall::StaticClass(), FTransform::Identity);
	Stale->Capsule->SetCollisionProfileName(
		UCollisionProfile::BlockAllDynamic_ProfileName);
	Stale->Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Stale->FinishSpawning(FTransform::Identity);
	TestTrue(TEXT("clobbered instance healed to query-only at init"),
		Stale->Capsule->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	TestEqual(TEXT("clobbered instance re-overlaps the weave channel"),
		Stale->Capsule->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestEqual(TEXT("clobbered instance re-ignores pawns"),
		Stale->Capsule->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
