// Headless parity specs for ExplodingMover's break path and fragment setup.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../WoTExplodingMover.h"
#include "../WoTFragment.h"
#include "../WoTRotatingMover.h"
#include "../WoTVertexAnimComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UnrealType.h"

namespace
{
	int32 CountExplodingMoverSpecFragments(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTFragment> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	float MaxExplodingMoverSpecFragmentSpeed(UWorld* World)
	{
		float MaxSpeed = 0.0f;
		for (TActorIterator<AWoTFragment> It(World); It; ++It)
		{
			MaxSpeed = FMath::Max(MaxSpeed, It->GetFragmentVelocity().Size());
		}
		return MaxSpeed;
	}

	AWoTExplodingMover* SpawnExplodingMoverSpecMover(
		UWorld* World, int32 InHealth = 10)
	{
		AWoTExplodingMover* Mover = World->SpawnActorDeferred<AWoTExplodingMover>(
			AWoTExplodingMover::StaticClass(), FTransform::Identity);
		Mover->Health = InHealth;
		Mover->KeyPos = {FVector::ZeroVector, FVector(100.0f, 0.0f, 0.0f)};
		Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
		Mover->FinishSpawning(FTransform::Identity);
		return Mover;
	}

	FWoTDamageEvent ExplodingMoverSpecDamage(FName Type)
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = Type;
		Event.UE1Momentum = FVector(8000000.0f, 0.0f, 0.0f);
		return Event;
	}
}

// GenericProjectile.uc:307-312 accepts a splash target when it is EITHER a
// damage-triggered mover OR an ordinary bProjTarget actor. ExplodingMover is
// bProjTarget even when bDamageTriggered is false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33SplashProjTargetMoverSpec,
	"WoT.RC33.R2.SplashProjTargetMover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33SplashProjTargetMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingMover* Mover = SpawnExplodingMoverSpecMover(World, 5);
	Mover->bDamageTriggered = false;
	UBoxComponent* QueryBody = NewObject<UBoxComponent>(Mover);
	QueryBody->SetBoxExtent(FVector(20.0f));
	QueryBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	QueryBody->SetCollisionObjectType(ECC_WorldDynamic);
	QueryBody->SetCollisionResponseToAllChannels(ECR_Overlap);
	QueryBody->RegisterComponent();
	QueryBody->AttachToComponent(Mover->GetRootComponent(),
		FAttachmentTransformRules::KeepRelativeTransform);

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTDartProjectile* Blast = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("splash projectile spawned"), Blast))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Keep the live dart outside direct-contact range: this must prove the
	// GenericProjectile.uc:307-312 splash filter rather than Touch damage.
	Blast->Damage = 20.0f;
	Blast->DamageRadius = 200.0f;
	Blast->ProcessSplashDamage();
	TestTrue(TEXT("non-damage-triggered bProjTarget ExplodingMover takes splash"),
		Mover->HasExploded());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExplodingMoverDamageSpec,
	"WoT.Mover.Exploding.DamageBreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTExplodingMoverDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTRotatingMover* EventTarget = World->SpawnActorDeferred<AWoTRotatingMover>(
		AWoTRotatingMover::StaticClass(), FTransform::Identity);
	EventTarget->TagName = TEXT("WallBroken");
	EventTarget->FinishSpawning(FTransform::Identity);

	AWoTExplodingMover* Mover = World->SpawnActorDeferred<AWoTExplodingMover>(
		AWoTExplodingMover::StaticClass(), FTransform::Identity);
	Mover->Health = 10;
	Mover->EventName = TEXT("WallBroken");
	Mover->NumWallChunks = 2;
	Mover->NumWoodChunks = 1;
	Mover->NumGlassChunks = 1;
	Mover->KeyPos = {FVector::ZeroVector, FVector(100.0f, 0.0f, 0.0f)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->FinishSpawning(FTransform::Identity);

	const FWoTDamageEvent Hurt = ExplodingMoverSpecDamage(TEXT("hurt"));
	Mover->TakeDamage(9.0f, Hurt, nullptr, nullptr);
	TestFalse(TEXT("damage below Health leaves the wall intact"), Mover->HasExploded());
	TestEqual(TEXT("damage below Health spawns no debris"),
		CountExplodingMoverSpecFragments(World), 0);

	Mover->TakeDamage(1.0f, Hurt, nullptr, nullptr);
	TestTrue(TEXT("damage reaching zero explodes"), Mover->HasExploded());
	TestTrue(TEXT("the break fires Event"), EventTarget->IsRotating());
	TestEqual(TEXT("all configured fragment families spawn"),
		CountExplodingMoverSpecFragments(World), 4);
	TestTrue(TEXT("glass debris receives UE1Momentum rather than ShotDirection"),
		MaxExplodingMoverSpecFragmentSpeed(World) > 90000.0f);
	TestEqual(TEXT("the spent wall no longer blocks pawns"),
		Mover->Mesh->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);
	TestTrue(TEXT("the wall is driven fully open"),
		Mover->GetActorLocation().Equals(FVector(100.0f, 0.0f, 0.0f), 0.1f));

	Mover->TakeDamage(100.0f, Hurt, nullptr, nullptr);
	TestEqual(TEXT("a second hit is a no-op"),
		CountExplodingMoverSpecFragments(World), 4);

	const FProperty* ExplodedProperty = FindFProperty<FProperty>(
		AWoTExplodingMover::StaticClass(), TEXT("bExploded"));
	TestTrue(TEXT("exploded state replicates"),
		ExplodedProperty && ExplodedProperty->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExplodingMoverDamageGateSpec,
	"WoT.Mover.Exploding.DamageGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTExplodingMoverDamageGateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingMover* Matching = SpawnExplodingMoverSpecMover(World, 5);
	Matching->ActivatedBy = {FName(TEXT("Fire")), NAME_None, NAME_None, NAME_None, NAME_None};

	const FWoTDamageEvent Sliced = ExplodingMoverSpecDamage(TEXT("Sliced"));
	Matching->TakeDamage(10.0f, Sliced, nullptr, nullptr);
	TestFalse(TEXT("a non-matching damage name is ignored"), Matching->HasExploded());
	TestEqual(TEXT("ignored damage does not reduce Health"), Matching->Health, 5);

	const FWoTDamageEvent Fire = ExplodingMoverSpecDamage(TEXT("Fire"));
	Matching->TakeDamage(5.0f, Fire, nullptr, nullptr);
	TestTrue(TEXT("an exact damage name breaks the wall"), Matching->HasExploded());

	AWoTExplodingMover* IncomingAll = SpawnExplodingMoverSpecMover(World, 5);
	IncomingAll->ActivatedBy = {FName(TEXT("Fire")), NAME_None, NAME_None, NAME_None, NAME_None};
	const FWoTDamageEvent All = ExplodingMoverSpecDamage(TEXT("All"));
	IncomingAll->TakeDamage(5.0f, All, nullptr, nullptr);
	TestTrue(TEXT("incoming All bypasses the configured gate"), IncomingAll->HasExploded());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ExplodingMover.uc:45 third clause: an element-name ActivatedBy entry accepts
// any AEFWS combo damage containing that element (DamageTypeContains).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExplodingMoverElementHierarchySpec,
	"WoT.Mover.Exploding.ElementHierarchyGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTExplodingMoverElementHierarchySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingMover* FireWall = SpawnExplodingMoverSpecMover(World, 5);
	FireWall->ActivatedBy = {FName(TEXT("Fire")), NAME_None, NAME_None, NAME_None, NAME_None};

	const FWoTDamageEvent NoFire = ExplodingMoverSpecDamage(TEXT("AxxWx"));
	FireWall->TakeDamage(10.0f, NoFire, nullptr, nullptr);
	TestFalse(TEXT("a combo without the element is ignored"), FireWall->HasExploded());
	TestEqual(TEXT("ignored combo does not reduce Health"), FireWall->Health, 5);

	const FWoTDamageEvent WithFire = ExplodingMoverSpecDamage(TEXT("xxFxx"));
	FireWall->TakeDamage(5.0f, WithFire, nullptr, nullptr);
	TestTrue(TEXT("a combo containing the element breaks the wall"),
		FireWall->HasExploded());

	// Invalid combo names ('hurt', 'SelfInflicted') contain no element —
	// exact-name misses stay misses.
	AWoTExplodingMover* Other = SpawnExplodingMoverSpecMover(World, 5);
	Other->ActivatedBy = {FName(TEXT("Water")), NAME_None, NAME_None, NAME_None, NAME_None};
	Other->TakeDamage(10.0f, ExplodingMoverSpecDamage(TEXT("hurt")), nullptr, nullptr);
	Other->TakeDamage(10.0f, ExplodingMoverSpecDamage(TEXT("SelfInflicted")),
		nullptr, nullptr);
	Other->TakeDamage(10.0f, ExplodingMoverSpecDamage(TEXT("xxFxx")), nullptr, nullptr);
	TestFalse(TEXT("non-matching elements and invalid combos stay ignored"),
		Other->HasExploded());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExplodingMoverTriggerOnlySpec,
	"WoT.Mover.Exploding.TriggerOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTExplodingMoverTriggerOnlySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTExplodingMover* Mover = SpawnExplodingMoverSpecMover(World, 1);
	Mover->bOnlyTriggerable = true;
	const FWoTDamageEvent Hurt = ExplodingMoverSpecDamage(TEXT("hurt"));
	Mover->TakeDamage(100.0f, Hurt, nullptr, nullptr);
	TestFalse(TEXT("trigger-only ignores damage"), Mover->HasExploded());
	TestEqual(TEXT("trigger-only damage does not reduce Health"), Mover->Health, 1);
	Mover->WoTTrigger(nullptr, nullptr);
	TestTrue(TEXT("trigger-only still explodes on Trigger"), Mover->HasExploded());
	WoTTest::DestroySpecWorld(World);
	return true;
}

// Movers block like world geometry but are WorldDynamic OBJECTS: retail trap
// placement accepts Level BSP only (AWoTTrap::CalcLocation's ECC_WorldStatic
// object-type gate; `Other == Level`), so no mover face may qualify as a
// trap floor. (Live-content census regression pin.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverObjectTypeSpec,
	"WoT.Mover.WorldDynamicObjectType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverObjectTypeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = World->SpawnActor<AWoTMover>();
	TestEqual(TEXT("mover mesh ships the BlockAllDynamic profile"),
		Mover->Mesh->GetCollisionProfileName(), FName(TEXT("BlockAllDynamic")));
	TestEqual(TEXT("mover collision object type is WorldDynamic"),
		Mover->Mesh->GetCollisionObjectType(), ECC_WorldDynamic);
	TestEqual(TEXT("movers still block pawns"),
		Mover->Mesh->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFragmentInitSpec,
	"WoT.Mover.Exploding.FragmentInit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFragmentInitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTFragment* Fragment = World->SpawnActor<AWoTFragment>();
	Fragment->Init(EWoTFragmentType::Wood, 200.0f, FVector::ZeroVector, 1.25f);
	TestTrue(TEXT("wood Init selects a wood mesh name"),
		Fragment->MeshName.ToString().StartsWith(TEXT("wfrag")));
	TestFalse(TEXT("Init computes a nonzero launch velocity"),
		Fragment->GetFragmentVelocity().IsNearlyZero());
	TestTrue(TEXT("display scale applies even when the mesh asset is absent"),
		Fragment->Mesh->GetRelativeScale3D().Equals(FVector(1.25f)));
	Fragment->OnRep_Display();
	TestNotNull(TEXT("null-safe display keeps the component valid"), Fragment->Mesh.Get());
	const FVector Start = Fragment->GetActorLocation();
	WoTTest::TickWorld(World, 1);
	TestFalse(TEXT("a fragment with no bound mesh still simulates"),
		Fragment->GetActorLocation().Equals(Start));
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
