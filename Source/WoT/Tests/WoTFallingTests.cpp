#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

namespace
{
constexpr EAutomationTestFlags FallingFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

void AddFallingFloor(UWorld* World)
{
	AActor* Floor = World->SpawnActor<AActor>();
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
	Box->SetBoxExtent(FVector(1000.0, 1000.0, 50.0));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(Box);
	Box->RegisterComponent();
	Floor->SetActorLocation(FVector(0.0, 0.0, -50.0));
}

AWoTBreakableDecoration* AddBreakable(UWorld* World, const FVector& Location)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AWoTBreakableDecoration>(Location, FRotator::ZeroRotator, Params);
}

void Knock(AWoTBreakableDecoration* Prop)
{
	FWoTDamageEvent Damage;
	Damage.WoTDamageType = TEXT("hurt");
	Prop->TakeDamage(1.0f, Damage, nullptr, nullptr);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingTossedTerminalSpec,
	"WoT.Falling.TossedWeapon.BottomlessTerminalSpeed", FallingFlags)
bool FWoTFallingTossedTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->GetWorldSettings()->bEnableWorldBoundsChecks = false;
	AWoTTossedWeapon* Weapon = World->SpawnActor<AWoTTossedWeapon>();
	Weapon->Movement->Velocity = FVector(1500.0, 0.0, -2000.0);
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Weapon->Movement->Velocity.Size());
	}
	// This safety bound deliberately includes tossed actors that still bounce;
	// PORT_NOTES records the difference from the original bouncing exception.
	TestTrue(FString::Printf(TEXT("tossed weapon total speed is bounded (peak %.3f)"), PeakSpeed),
		PeakSpeed <= 2500.001);
	TestTrue(TEXT("the weapon really falls across the bottomless fixture"), Weapon->GetActorLocation().Z < -10000.0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingBreakableTerminalSpec,
	"WoT.Falling.Breakable.NonBouncingTerminalSpeed", FallingFlags)
bool FWoTFallingBreakableTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Prop = AddBreakable(World, FVector::ZeroVector);
	Prop->StartFalling(FVector(2000.0, 0.0, 0.0));
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Prop->GetFallVelocity().Size());
	}
	// The original engine bounds total nonbouncing speed at 2500 by default.
	TestTrue(FString::Printf(TEXT("total speed stays at terminal speed (peak %.3f)"), PeakSpeed),
		PeakSpeed <= 2500.001);
	TestTrue(TEXT("the drop remains a real moving fall"), Prop->GetActorLocation().Z < -10000.0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingTossedFloorSpec,
	"WoT.Falling.TossedWeapon.FloorStopsWithoutMeshAsset", FallingFlags)
bool FWoTFallingTossedFloorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AddFallingFloor(World);
	AWoTTossedWeapon* Weapon = World->SpawnActor<AWoTTossedWeapon>(
		FVector(0.0, 0.0, 80.0), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 3.0f);
	TestTrue(TEXT("world collision exists independently of the presentation mesh"),
		Weapon->GetActorLocation().Z >= 4.0);
	TestTrue(TEXT("floor contact eventually stops a tossed weapon"), Weapon->Movement->Velocity.IsNearlyZero());
	const FVector Rest = Weapon->GetActorLocation();
	WoTTest::TickWorld(World, 2.0f);
	TestEqual(TEXT("the landed weapon remains at rest"), Weapon->GetActorLocation(), Rest);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingBreakableFloorSpec,
	"WoT.Falling.Breakable.PenetratingFloorStops", FallingFlags)
bool FWoTFallingBreakableFloorSpec::RunTest(const FString&)
{
	// Exactly centred over the floor box and off-centre, knocked and not, each
	// in its own world: the physics engine's push-out direction for a shape
	// centred over a box differs by platform (it can point down on ARM).
	for (const double X : {0.0, 200.0})
	for (const bool bKnocked : {false, true})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AddFallingFloor(World);
		// The 29-unit cylinder begins half a unit inside the floor, as a projected
		// prop can after collision rounding. This is a blocking initial overlap.
		AWoTBreakableDecoration* Prop = AddBreakable(World, FVector(X, 0.0, 28.5));
		if (bKnocked) Knock(Prop);
		else Prop->StartFalling(FVector::ZeroVector);
		WoTTest::TickWorld(World, 1.0f);
		TestTrue(TEXT("a shallow floor overlap is survivable"), IsValid(Prop));
		TestFalse(TEXT("floor contact ends falling"), Prop->IsFallingLoose());
		TestFalse(TEXT("BreakableDecoration.uc:73 clears bounce on contact"), Prop->IsKnocked());
		TestEqual(TEXT("resting velocity is zero"), Prop->GetFallVelocity(), FVector::ZeroVector);
		TestTrue(TEXT("the cylinder is recovered above the floor"), Prop->GetActorLocation().Z >= 29.0);
		const FVector Rest = Prop->GetActorLocation();
		WoTTest::TickWorld(World, 4.0f);
		TestEqual(TEXT("resting velocity cannot accumulate gravity"), Prop->GetFallVelocity(), FVector::ZeroVector);
		TestEqual(TEXT("resting position stays fixed"), Prop->GetActorLocation(), Rest);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingTossedProjectileSpec,
	"WoT.Falling.TossedWeapon.ProjectileContact", FallingFlags)
bool FWoTFallingTossedProjectileSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTossedWeapon* Weapon = World->SpawnActor<AWoTTossedWeapon>(
		FVector(100.0, 0.0, 100.0), FRotator::ZeroRotator);
	Weapon->Movement->Deactivate();
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	// BounceableDecoration.uc:190 keeps a tossed decoration a projectile target.
	TestFalse(TEXT("a real swept projectile touches the tossed weapon and detonates"), IsValid(Dart));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingTossedUprightSpec,
	"WoT.Falling.TossedWeapon.RotatedPresentationUprightCollision", FallingFlags)
bool FWoTFallingTossedUprightSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AddFallingFloor(World);
	const FRotator PresentationRotation(90.0, 45.0, 30.0);
	AWoTTossedWeapon* Weapon = World->SpawnActor<AWoTTossedWeapon>(
		FVector(0.0, 0.0, 80.0), PresentationRotation);
	WoTTest::TickWorld(World, 3.0f);
	TestTrue(TEXT("the falling cylinder stays world-up for a pitched weapon"),
		Weapon->GetActorUpVector().Equals(FVector::UpVector, 0.001));
	TestTrue(TEXT("floor rest uses the four-unit cylinder height, not its radius"),
		FMath::IsNearlyEqual(Weapon->GetActorLocation().Z, 4.0, 0.2));
	TestTrue(TEXT("the presentation keeps its source rotation"),
		Weapon->DisplayMesh->GetComponentQuat().Equals(PresentationRotation.Quaternion(), 0.001));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingBreakableBounceSpec,
	"WoT.Falling.Breakable.BounceGravityAndContact", FallingFlags)
bool FWoTFallingBreakableBounceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTBreakableDecoration* Falling = AddBreakable(World, FVector(2000.0, 0.0, 10000.0));
	Knock(Falling);
	// BreakableDecoration.uc:107-110 enables bounce and starts at Z velocity 10.
	// The original bouncing path retains the half-gravity step until contact.
	WoTTest::TickWorld(World, 20, 0.05f);
	TestTrue(TEXT("bouncing fall retains half-gravity velocity"),
		FMath::IsNearlyEqual(Falling->GetFallVelocity().Z, -465.0, 0.01));
	TestTrue(TEXT("no contact leaves bouncing enabled"), Falling->IsKnocked());
	AddFallingFloor(World);
	AWoTBreakableDecoration* Low = AddBreakable(World, FVector(0.0, 0.0, 30.0));
	AWoTBreakableDecoration* High = AddBreakable(World, FVector(200.0, 0.0, 129.0));
	Knock(Low);
	Knock(High);
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("slow bounced floor contact survives"), IsValid(Low));
	TestFalse(TEXT("slow floor contact clears bounce and lands"), Low->IsKnocked() || Low->IsFallingLoose());
	TestEqual(TEXT("slow floor contact stops velocity"), Low->GetFallVelocity(), FVector::ZeroVector);
	TestFalse(TEXT("BreakableDecoration.uc:69-71 shatters below -200 on bounced contact"), IsValid(High));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingWorldExitSpec,
	"WoT.Falling.WorldExit.TossedAndBreakable", FallingFlags)
bool FWoTFallingWorldExitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->GetWorldSettings()->bEnableWorldBoundsChecks = false;
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	Table->ZoneBspPlanes.Add(FVector4(0.0, 0.0, 1.0, -100.0));
	Table->ZoneBspFront.Add(-2); // zone one above the boundary
	Table->ZoneBspBack.Add(-1); // zone zero below it
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table);
	AWoTBreakableDecoration* Prop = AddBreakable(World, FVector::ZeroVector);
	Prop->StartFalling(FVector::ZeroVector);
	AWoTTossedWeapon* Weapon = World->SpawnActor<AWoTTossedWeapon>(FVector(200.0, 0.0, 0.0), FRotator::ZeroRotator);
	TestFalse(TEXT("fixture starts inside the authored world"), WoTIsPointOutsideWorld(World, Prop->GetActorLocation()));
	TestTrue(TEXT("fixture identifies the void"), WoTIsPointOutsideWorld(World, FVector(0.0, 0.0, -101.0)));
	WoTTest::TickWorld(World, 2.0f);
	TestFalse(TEXT("falling breakable is destroyed after leaving all world zones"), IsValid(Prop));
	TestFalse(TEXT("tossed weapon is destroyed after leaving all world zones"), IsValid(Weapon));
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, nullptr);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
