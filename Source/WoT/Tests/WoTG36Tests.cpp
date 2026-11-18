// G36 headless specs: the BreakableDecoration break path
// (BreakableDecoration.uc:76-112 state Animate), its GATELESS damage intake
// (no ActivatedBy — contrast the WoT.ExplodingWall specs), the engine Frag
// helper's 12-chunk authored-family debris (Engine.Decoration.Frag),
// the non-fatal knock, the stacked-pile support-loss collapse (the Arena_06
// BarrelTap piles), and the blocking-collision pin (retail bBlockPlayers/
// bBlockActors/bProjTarget all true — contrast the pawn-transparent wall).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTProjectile.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTDecoration.h"
#include "../WoTFragment.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	// A blocking slab whose top surface sits at Z = 0 (rootless-spawn rule:
	// SetActorLocation AFTER SetRootComponent).
	AActor* SpawnG36Floor(UWorld* World)
	{
		AActor* Floor = World->SpawnActor<AActor>();
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("Floor"));
		Box->SetBoxExtent(FVector(4000, 4000, 50));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(FVector(0, 0, -50));
		return Floor;
	}

	// The importer-stamped Arena_06 BarrelTap config (BarrelTap.uc:21-31 leaf
	// CDO: Health=20, WoodFragments, fragmentSize=1.75, Mass=60, 29x22 cylinder.
	AWoTBreakableDecoration* SpawnG36Barrel(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTBreakableDecoration* Barrel =
			World->SpawnActor<AWoTBreakableDecoration>(
				AWoTBreakableDecoration::StaticClass(), Location,
				FRotator::ZeroRotator, Params);
		Barrel->Health = 20;
		Barrel->FragmentType = EWoTFragmentType::Wood;
		Barrel->FragmentSize = 1.75f;
		Barrel->Mass = 60.0f;
		Barrel->Collision->SetCylinderSize(29.0f, 22.0f);
		return Barrel;
	}

	FWoTDamageEvent G36SpecDamage(FName Type,
		const FVector& Momentum = FVector::ZeroVector)
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = Type;
		Event.UE1Momentum = Momentum;
		return Event;
	}

	int32 CountG36Fragments(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTFragment> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}
}

// (a) No damage-type gate (BreakableDecoration.uc:76-110 has no ActivatedBy —
// contrast WoT.ExplodingWall.MPWeavesInert): 'hurt', a weave compound, ANY
// name chips Health. In MP the barrels are genuinely breakable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36NoTypeGateSpec,
	"WoT.BreakableDecoration.NoDamageTypeGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36NoTypeGateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Barrel = SpawnG36Barrel(World, FVector(0, 0, 22));

	Barrel->TakeDamage(5.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestEqual(TEXT("'hurt' chips Health (no ActivatedBy gate)"),
		Barrel->Health, 15);
	Barrel->TakeDamage(5.0f, G36SpecDamage(TEXT("xxFxx")), nullptr, nullptr);
	TestEqual(TEXT("a weave compound chips Health too"), Barrel->Health, 10);
	TestFalse(TEXT("non-fatal hits do not break the barrel"),
		Barrel->HasFragged());
	TestEqual(TEXT("no debris before the break"), CountG36Fragments(World), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (b) The fatal break on the stamped config: exactly 12 fragments AT the
// barrel (Frag NumFrags=12, BreakableDecoration.uc:102), every MeshScale
// inside fragmentSize*[0.5, 1.2] (Engine.Decoration.Frag: DSize*0.5+0.7*DSize*FRand),
// SkinName None (Frag, not skinnedFrag — no skin override), barrel destroyed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36FatalBreakSpec,
	"WoT.BreakableDecoration.FatalBreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36FatalBreakSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Barrel = SpawnG36Barrel(World, FVector(0, 0, 22));

	Barrel->TakeDamage(20.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestTrue(TEXT("a 20-damage hit breaks the Health=20 barrel"),
		Barrel->HasFragged());
	TestEqual(TEXT("exactly 12 fragments shed"), CountG36Fragments(World), 12);
	for (TActorIterator<AWoTFragment> It(World); It; ++It)
	{
		TestTrue(TEXT("each chunk carries the authored wood family"),
			It->MeshName.ToString().StartsWith(TEXT("wfrag")));
		TestTrue(TEXT("MeshScale >= 1.75*0.5"), It->MeshScale >= 0.875f);
		TestTrue(TEXT("MeshScale <= 1.75*1.2"), It->MeshScale <= 2.1f);
		TestEqual(TEXT("Frag (not skinnedFrag) sets NO skin"),
			It->SkinName, FName(NAME_None));
	}
	TestFalse(TEXT("the broken barrel is destroyed outright"), IsValid(Barrel));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (c) The Health<=0 early-return (uc:86-89 — the already-dead pre-check the
// G35 wall does NOT have). bOnlyTriggerable ordering: the fatal hit drains
// Health but Frag aborts (Engine.Decoration, no destroy), so the corpse
// actor survives to receive the second hit — which must be a total no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36AlreadyDeadSpec,
	"WoT.BreakableDecoration.AlreadyDeadNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36AlreadyDeadSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Barrel = SpawnG36Barrel(World, FVector(0, 0, 22));
	Barrel->bOnlyTriggerable = true;

	const float FirstApplied = Barrel->TakeDamage(
		100.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestEqual(TEXT("the fatal hit lands its damage"), FirstApplied, 100.0f);
	TestEqual(TEXT("Health goes negative"), Barrel->Health, -80);
	TestTrue(TEXT("bOnlyTriggerable: no destroy"), IsValid(Barrel));
	TestEqual(TEXT("bOnlyTriggerable: no debris"), CountG36Fragments(World), 0);

	const float SecondApplied = Barrel->TakeDamage(
		100.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestEqual(TEXT("the second hit on the dead barrel applies nothing"),
		SecondApplied, 0.0f);
	TestEqual(TEXT("Health untouched by the already-dead hit"),
		Barrel->Health, -80);
	TestFalse(TEXT("the dead barrel does not get knocked falling"),
		Barrel->IsFallingLoose());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (d) The non-fatal knock (uc:105-111): PHYS_Falling with Momentum.Z REPLACED
// by 1000 then Velocity = Momentum*0.01 — scaled XY plus a fixed Z=10 boost,
// regardless of the incoming momentum's Z.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36KnockSpec,
	"WoT.BreakableDecoration.NonFatalKnock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36KnockSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Barrel = SpawnG36Barrel(World, FVector(0, 0, 22));

	Barrel->TakeDamage(5.0f,
		G36SpecDamage(TEXT("hurt"), FVector(800.0f, -400.0f, -5555.0f)),
		nullptr, nullptr);
	TestTrue(TEXT("a non-fatal hit knocks the barrel loose"),
		Barrel->IsFallingLoose());
	TestEqual(TEXT("Velocity = Momentum*0.01 with the Z=10 boost"),
		Barrel->GetFallVelocity(), FVector(8.0, -4.0, 10.0));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (e) Stack collapse — the MP-observable composite of BaseChange dispatch:
// two stacked barrels on a floor; break the bottom one and the top loses its
// support (uc:28-31 base==None -> PHYS_Falling) and DROPS INTACT — an
// unknocked fall lands through Decoration.Landed, which deals no damage
// (UE1's physFalling gates floor HitWall on bBounce), and
// the ~-290uu/s impact clears the -500 BaseChange crush gate (uc:23-27).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36StackCollapseSpec,
	"WoT.BreakableDecoration.StackCollapse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36StackCollapseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnG36Floor(World);
	// Bottom rests on the floor at cylinder half-height; the top's bottom face
	// touches the bottom's top face (22 + 22 + 22 = 66).
	AWoTBreakableDecoration* Bottom = SpawnG36Barrel(World, FVector(0, 0, 22));
	AWoTBreakableDecoration* Top = SpawnG36Barrel(World, FVector(0, 0, 66));

	Bottom->TakeDamage(20.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestFalse(TEXT("the bottom barrel is gone"), IsValid(Bottom));
	TestEqual(TEXT("the bottom break sheds its 12 chunks"),
		CountG36Fragments(World), 12);
	TestTrue(TEXT("the top barrel loses its support and starts falling"),
		Top->IsFallingLoose());

	// 44uu of free fall lands at ~-290uu/s well inside a second of 1/60s
	// ticks; stay under the fragments' 1.5s visibility-cleanup timer so the
	// count is stable.
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("the unknocked top barrel lands INTACT (retail Landed is "
		"damage-free; -290 clears the -500 crush gate)"), IsValid(Top));
	TestFalse(TEXT("the landed barrel is at rest"), Top->IsFallingLoose());
	TestEqual(TEXT("no extra debris from the survivable landing"),
		CountG36Fragments(World), 12);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (e2) The two landing-damage legs the survivable drop clears: a KNOCKED fall routes floor contact through Animate.HitWall
// (retail bBounce, uc:108 -> UE1's physFalling) and shatters past
// -200uu/s; an UNKNOCKED fall from high enough trips the BaseChange crush
// gate instead (uc:23-27: Vz <= -500, damage 1 - Vz/30 — ~-627 from a 200uu
// drop deals 21 > the stamped Health 20).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36LandingDamageSpec,
	"WoT.BreakableDecoration.LandingDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36LandingDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnG36Floor(World);

	// Knocked: the non-fatal hit arms the retail bBounce; dropped from 44uu it
	// lands at ~-290 — past the -200 HitWall shatter gate.
	AWoTBreakableDecoration* Knocked = SpawnG36Barrel(World, FVector(0, 0, 66));
	Knocked->TakeDamage(1.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestTrue(TEXT("the non-fatal hit knocks the barrel loose"),
		Knocked->IsFallingLoose() && Knocked->IsKnocked());
	WoTTest::TickWorld(World, 1.0f);
	TestFalse(TEXT("a knocked fall shatters on the floor (HitWall gate)"),
		IsValid(Knocked));
	TestEqual(TEXT("the shatter sheds the 12 chunks"),
		CountG36Fragments(World), 12);

	// Clear batch 1 before the second leg. Its debris rests on
	// FRand-rolled velocities, and an early rester's 1.5s not-rendered cleanup
	// (StartCleanup; nothing renders under nullrhi) could fire inside the next
	// 1.2s window — the old cumulative 24-count raced that edge (the recorded
	// 23/24 flake). Each leg now counts only its own debris.
	for (TActorIterator<AWoTFragment> It(World); It; ++It)
	{
		It->Destroy();
	}
	TestEqual(TEXT("batch 1 cleared"), CountG36Fragments(World), 0);

	// Unknocked from a 200uu drop: ~-627 at the floor trips the -500 crush
	// gate for Trunc(1 + 627/30) = 21 damage — fatal for Health 20.
	AWoTBreakableDecoration* Crushed =
		SpawnG36Barrel(World, FVector(400, 0, 222));
	Crushed->StartFalling(FVector::ZeroVector);
	WoTTest::TickWorld(World, 1.2f);
	TestFalse(TEXT("an unknocked 200uu drop crushes (BaseChange -500 gate)"),
		IsValid(Crushed));
	TestEqual(TEXT("the crush sheds its 12 chunks"),
		CountG36Fragments(World), 12);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (f) The blocking-collision pin: retail bBlockPlayers/bBlockActors=True +
// bProjTarget=True (BreakableDecoration.uc:121-126) — the collision cylinder blocks
// pawns AND the WoTProjectile channel AND world geometry (contrast the G35
// wall, which ignores pawns). Plus the standing profile-clobber legs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36CollisionSpec,
	"WoT.BreakableDecoration.BlockingCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36CollisionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Barrel =
		World->SpawnActor<AWoTBreakableDecoration>();

	TestEqual(TEXT("BreakableDecoration CDO 29x29 cylinder"),
		Barrel->Collision->GetUnscaledCapsuleRadius(), 29.0f);
	TestEqual(TEXT("BreakableDecoration CDO 29x29 half-height"),
		Barrel->Collision->GetUnscaledCapsuleHalfHeight(), 29.0f);
	TestEqual(TEXT("pawns are blocked (bBlockActors/bBlockPlayers)"),
		Barrel->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	TestEqual(TEXT("weave projectiles overlap it and dispatch Touch (bProjTarget)"),
		Barrel->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestEqual(TEXT("world geometry blocks the fall (bCollideWorld)"),
		Barrel->Collision->GetCollisionResponseToChannel(ECC_WorldStatic),
		ECR_Block);

	// Clobber leg 1: the PostLoad profile reapply must not strip the contract.
	Barrel->Collision->GetBodyInstance()->LoadProfileData(false);
	TestEqual(TEXT("pawn block survives a profile reload"),
		Barrel->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	TestEqual(TEXT("projectile overlap survives a profile reload"),
		Barrel->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);

	// Clobber leg 2: a stale-umap instance deserialized with a broken profile
	// heals back at init (PostInitializeComponents re-assert).
	AWoTBreakableDecoration* Stale =
		World->SpawnActorDeferred<AWoTBreakableDecoration>(
			AWoTBreakableDecoration::StaticClass(), FTransform::Identity);
	Stale->Collision->SetCollisionProfileName(
		UCollisionProfile::NoCollision_ProfileName);
	Stale->Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Stale->FinishSpawning(FTransform::Identity);
	TestTrue(TEXT("clobbered instance healed to blocking at init"),
		Stale->Collision->GetCollisionEnabled() ==
		ECollisionEnabled::QueryAndPhysics);
	TestEqual(TEXT("clobbered instance re-blocks pawns"),
		Stale->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	TestEqual(TEXT("clobbered instance re-overlaps projectiles"),
		Stale->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (g) The WireLamp census pin: WireLamp expands
// WOTDecoration directly (WireLamp.uc — no Health, no fragmentClass, no
// TakeDamage break), so the Arena_12 x8 + Battle2_02 x9 lamps are plain
// non-breakable decorations: damage records and nothing else happens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG36WireLampSpec,
	"WoT.BreakableDecoration.WireLampNotBreakable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG36WireLampSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDecoration* Lamp = World->SpawnActor<AWoTDecoration>();

	Lamp->TakeDamage(500.0f, G36SpecDamage(TEXT("hurt")), nullptr, nullptr);
	TestEqual(TEXT("the hit records on the plain decoration"),
		Lamp->LastDamageTaken, 500.0f);
	TestTrue(TEXT("the lamp survives any damage (no break path)"),
		IsValid(Lamp));
	TestEqual(TEXT("no debris ever sheds from a plain decoration"),
		CountG36Fragments(World), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
