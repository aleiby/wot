// Automation specs for the P28 carcass residuals: the Legion LandThump/ThrowOthers
// heavy-corpse knockback (WOTCarcass.uc:212-260), the zone-scoped carcass budget +
// eviction (:57/173/433-469), and the Balefire inert/no-gib guards (:154/185/343).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBalefireFX.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTPlayerController.h"
#include "../WoTZoneVolume.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	// Minimal ticking Standalone world (the shared test-file convention; helper names
	// kept file-unique for the unity build).
	// A big blocking slab; top surface at Z = 0.
	AActor* P28SpawnFloor(UWorld* World)
	{
		AActor* Floor = World->SpawnActor<AActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("Floor"));
		Box->SetBoxExtent(FVector(6000, 6000, 50));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(FVector(0, 0, -50));
		return Floor;
	}

	// A walking player: a possessed bare character settled on the floor.
	AWoTCharacter* P28SpawnPlayer(UWorld* World, const FVector& At)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, Params);
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		if (PC && Pawn)
		{
			// The standing fixture trap: a bare spawn is DEAD (Health 0) and dead
			// pawns don't settle into MOVE_Walking.
			Pawn->Health = 100;
			PC->Possess(Pawn);
		}
		return Pawn;
	}

	AWoTCarcass* P28SpawnCarcass(UWorld* World, const FVector& At, float Mass)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCarcass* Carc =
			World->SpawnActor<AWoTCarcass>(At, FRotator::ZeroRotator, Params);
		if (Carc)
		{
			Carc->Mass = Mass;
		}
		return Carc;
	}

	int32 P28CountForZone(UWorld* World)
	{
		int32 N = 0;
		for (TActorIterator<AWoTCarcass> It(World); It; ++It)
		{
			if (IsValid(*It)) { ++N; }
		}
		return N;
	}
}

// R2: a Mass >= 500 corpse (the Legion) coming to rest throws every human player —
// a distance-scaled view shake on all, plus a formula knockback on walking players
// within 1500uu; a light corpse throws no one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP28LandThumpSpec,
	"WoT.P28.LandThump", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP28LandThumpSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	P28SpawnFloor(World);

	AWoTCharacter* Near = P28SpawnPlayer(World, FVector(900, 0, 100));
	AWoTCharacter* Far = P28SpawnPlayer(World, FVector(2600, 0, 100));
	if (!TestNotNull(TEXT("near player"), Near) || !TestNotNull(TEXT("far player"), Far))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTPlayerController* NearPC = Cast<AWoTPlayerController>(Near->GetController());
	AWoTPlayerController* FarPC = Cast<AWoTPlayerController>(Far->GetController());

	// A headless player-controller pawn's CMC does not gravity-settle (no local
	// player drives its tick), so place both players exactly ON the floor and set
	// the walking mode explicitly — the throw's kick gate reads MovementMode, and
	// a pawn standing on the slab keeps Walking whether or not CMC ticks.
	const float NearHalf = Near->GetSimpleCollisionHalfHeight();
	Near->SetActorLocation(FVector(900, 0, NearHalf + 2.0f));
	Near->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Far->SetActorLocation(FVector(2600, 0, Far->GetSimpleCollisionHalfHeight() + 2.0f));
	Far->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("near player is walking before the thump"),
	         Near->GetCharacterMovement()->MovementMode == MOVE_Walking);

	const int32 NearShakeBefore = NearPC ? NearPC->GetShakeRequestCountForTest() : -1;

	// A LIGHT corpse (< 500): LandThump never calls ThrowOthers — no shake, no kick.
	AWoTCarcass* Light = P28SpawnCarcass(World, FVector(0, 0, 300), 175.0f);
	Light->SetVelocityForTest(FVector(0, 0, -200));
	float LightMaxNearVz = 0.0f;
	for (float T = 0.0f; T < 3.0f && !Light->IsLyingStill(); T += 1.0f / 60.0f)
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
		LightMaxNearVz = FMath::Max(LightMaxNearVz, Near->GetCharacterMovement()->Velocity.Z);
	}
	TestTrue(TEXT("light corpse landed"), Light->IsLyingStill());
	TestTrue(TEXT("light corpse threw no one (no upward kick)"), LightMaxNearVz < 10.0f);
	TestEqual(TEXT("light corpse fired no view shake"),
	          NearPC ? NearPC->GetShakeRequestCountForTest() : -1, NearShakeBefore);

	// A HEAVY corpse via the REAL InitFor path (review D2: setting Mass directly
	// masked the ConfigureNPC plumbing — this leg now regresses if the dead pawn's
	// CMC Mass doesn't reach the carcass). A Legion-mass source pawn dies:
	FActorSpawnParameters HeavyParams;
	HeavyParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* DeadLegion = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 300), FRotator::ZeroRotator, HeavyParams);
	DeadLegion->GetCharacterMovement()->Mass = 3000.0f;   // the Legion row's Mass
	AWoTCarcass* Heavy = World->SpawnActor<AWoTCarcass>(
		FVector(0, 0, 300), FRotator::ZeroRotator, HeavyParams);
	Heavy->InitFor(*DeadLegion);
	DeadLegion->Destroy();
	TestTrue(TEXT("InitFor carried the dead pawn's Mass (>=500 gate armed)"),
	         Heavy->Mass >= 500.0f);
	Heavy->SetVelocityForTest(FVector(0, 0, -200));
	float HeavyMaxNearVz = 0.0f;
	float FarMaxVz = 0.0f;
	bool bLanded = false;
	for (float T = 0.0f; T < 3.0f; T += 1.0f / 60.0f)
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
		HeavyMaxNearVz = FMath::Max(HeavyMaxNearVz, Near->GetCharacterMovement()->Velocity.Z);
		FarMaxVz = FMath::Max(FarMaxVz, Far->GetCharacterMovement()->Velocity.Z);
		if (Heavy->IsLyingStill()) { bLanded = true; break; }
		// keep integrating a couple frames past the thump to catch the velocity spike
	}
	// let one more frame carry the applied kick into the sampled velocity
	World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	GFrameCounter++;
	HeavyMaxNearVz = FMath::Max(HeavyMaxNearVz, Near->GetCharacterMovement()->Velocity.Z);

	TestTrue(TEXT("heavy corpse landed"), bLanded);

	// Hand-computed knockback for the near player's distance:
	// Momentum.Z = 7000000 / ((0.4*dist + 350) * playerMass).
	const float Dist = FVector::Dist(Heavy->GetActorLocation(), Near->GetActorLocation());
	const float ExpectedZ = 7000000.0f / ((0.4f * Dist + 350.0f) * 175.0f);
	TestTrue(TEXT("near walking player took the formula knockback"),
	         HeavyMaxNearVz > 0.4f * ExpectedZ && HeavyMaxNearVz < 1.2f * ExpectedZ);
	TestTrue(TEXT("player beyond 1500uu took no kick"), FarMaxVz < 10.0f);
	TestTrue(TEXT("heavy corpse fired the view shake on the near player"),
	         NearPC && NearPC->GetShakeRequestCountForTest() > NearShakeBefore);
	TestTrue(TEXT("the shake reaches even the far player (all players shake)"),
	         FarPC && FarPC->GetShakeRequestCountForTest() > 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// R3a: a zone holds at most MaxCarcasses corpses; the (MaxCarcasses+1)th spawn evicts
// the oldest and the count holds. A second zone is accounted independently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP28ZoneBudgetSpec,
	"WoT.P28.ZoneBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP28ZoneBudgetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTZoneVolume* ZoneA =
		World->SpawnActor<AWoTZoneVolume>(FVector(0, 0, 0), FRotator::ZeroRotator);
	AWoTZoneVolume* ZoneB =
		World->SpawnActor<AWoTZoneVolume>(FVector(5000, 0, 0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("zone A"), ZoneA) || !TestNotNull(TEXT("zone B"), ZoneB))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ZoneA->Collision->SetBoxExtent(FVector(500));
	ZoneB->Collision->SetBoxExtent(FVector(500));
	ZoneA->MaxCarcasses = 2;
	ZoneB->MaxCarcasses = 2;

	// Spawn MaxCarcasses+1 = 3 corpses in zone A. BeginPlay accounts + evicts inline.
	// Advance world time a hair between spawns so their CreationTimes are distinct
	// (the oldest-eviction key); otherwise same-frame spawns tie.
	AWoTCarcass* A0 = P28SpawnCarcass(World, FVector(0, 0, 50), 175.0f);
	WoTTest::TickWorld(World, 0.05f);
	AWoTCarcass* A1 = P28SpawnCarcass(World, FVector(20, 0, 50), 175.0f);
	WoTTest::TickWorld(World, 0.05f);
	AWoTCarcass* A2 = P28SpawnCarcass(World, FVector(40, 0, 50), 175.0f);

	TestEqual(TEXT("zone A holds exactly MaxCarcasses after eviction"),
	          ZoneA->NumCarcasses, 2);
	TestFalse(TEXT("the oldest corpse was the one evicted"), IsValid(A0));
	TestTrue(TEXT("the two newer corpses survive"), IsValid(A1) && IsValid(A2));
	TestEqual(TEXT("only two carcasses remain in the world"), P28CountForZone(World), 2);

	// A corpse in zone B is unaffected by zone A's budget.
	AWoTCarcass* B0 = P28SpawnCarcass(World, FVector(5000, 0, 50), 175.0f);
	TestTrue(TEXT("zone B corpse survives"), IsValid(B0));
	TestEqual(TEXT("zone B counts its own corpse"), ZoneB->NumCarcasses, 1);
	TestEqual(TEXT("zone A is untouched by zone B"), ZoneA->NumCarcasses, 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// R1 edge: a balefired corpse is inert (never falls), takes no damage (no bleed/gib),
// and is removed whole by the Balefire sweep — while an untagged corpse gibs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP28BalefiredSpec,
	"WoT.P28.Balefired", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP28BalefiredSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	P28SpawnFloor(World);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A balefired pawn's corpse: tag the pawn, then InitFor copies the tag.
	AWoTCharacter* Doomed = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 200), FRotator::ZeroRotator, Params);
	AWoTCharacter* Normal = World->SpawnActor<AWoTAesSedai>(
		FVector(400, 0, 200), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("doomed pawn"), Doomed) || !TestNotNull(TEXT("normal pawn"), Normal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 0.1f);
	Doomed->Tags.Add(FName(TEXT("Balefired")));

	AWoTCarcass* Bale = World->SpawnActor<AWoTCarcass>(
		FVector(0, 0, 300), FRotator::ZeroRotator, Params);
	Bale->InitFor(*Doomed);
	AWoTCarcass* Plain = World->SpawnActor<AWoTCarcass>(
		FVector(400, 0, 300), FRotator::ZeroRotator, Params);
	Plain->InitFor(*Normal);

	// Inert: the balefired corpse never falls (InitFor armed PHYS_None).
	TestTrue(TEXT("balefired corpse is inert (lies still immediately)"), Bale->IsLyingStill());
	TestFalse(TEXT("untagged corpse is still falling"), Plain->IsLyingStill());

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Exploded");
	Ev.UE1Momentum = FVector::ZeroVector;
	Ev.HitInfo.ImpactPoint = Bale->GetActorLocation();

	// Lethal damage to the balefired corpse: no gib, corpse survives.
	Bale->TakeDamage(500.0f, Ev, nullptr, nullptr);
	TestTrue(TEXT("balefired corpse survives lethal damage (no gib)"), IsValid(Bale));
	TestFalse(TEXT("balefired corpse did not chunk up"), Bale->WasChunkedUp());

	// The same lethal damage to the untagged corpse gibs it (guard contrast).
	Ev.HitInfo.ImpactPoint = Plain->GetActorLocation();
	Plain->TakeDamage(500.0f, Ev, nullptr, nullptr);
	TestFalse(TEXT("untagged corpse gibs and is destroyed"), IsValid(Plain));

	// The Balefire sweep removes the inert corpse whole.
	WoTBalefireFX::SpawnRemoval(*World, Bale, Bale->GetActorLocation(), /*bRemoveTarget*/ true);
	WoTTest::TickWorld(World, 0.7f);
	TestFalse(TEXT("the Balefire sweep removed the balefired corpse"), IsValid(Bale));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif  // WITH_AUTOMATION_TESTS
