#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBounceableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTDecal.h"
#include "../WoTWorldFX.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

namespace
{
	AActor* SpawnFallingAuditSurface(UWorld* World, const FVector& Location,
		const FVector& Extent)
	{
		AActor* Surface = World->SpawnActor<AActor>();
		UBoxComponent* Box = NewObject<UBoxComponent>(Surface);
		Surface->SetRootComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		Surface->SetActorLocation(Location);
		return Surface;
	}

	AWoTSpewChunk* SpawnFallingAuditSpew(UWorld* World,
		const FVector& Location, const FVector& Velocity, float StopSpeed)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTSpewChunk* Chunk = World->SpawnActorDeferred<AWoTSpewChunk>(
			AWoTSpewChunk::StaticClass(), Transform);
		Chunk->bCollision = true;
		Chunk->bSilent = true;
		Chunk->bTumble = false;
		Chunk->CollisionRadius = 5.0f;
		Chunk->CollisionHeight = 5.0f;
		Chunk->InitialVelocity = Velocity;
		Chunk->MinBounceSpeed = StopSpeed;
		Chunk->SpeedDampeningRatio = 0.33f;
		Chunk->FinishSpawning(Transform);
		return Chunk;
	}
}

// Decal.uc:210 selects falling physics. Non-bouncing falling actors retain
// the original engine's default terminal speed of 2500 units per second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditDecalTerminalSpec,
	"WoT.Falling.Audit.DecalTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditDecalTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDecal* Decal = World->SpawnActor<AWoTDecal>(
		FVector(0.0f, 0.0f, 10000.0f), FRotator::ZeroRotator);
	Decal->bFlying = true;
	Decal->Velocity = FVector(0.0f, 0.0f, -2400.0f);
	Decal->SetActorTickEnabled(true);
	const FVector StartLocation = Decal->GetActorLocation();
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 360 && IsValid(Decal); ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Decal->Velocity.Size());
	}
	TestTrue(TEXT("a decal remains airborne above the outside-world fallback"),
		IsValid(Decal) && !Decal->IsPlaced());
	TestTrue(TEXT("the flight fixture actually moves through the world"),
		Decal->GetActorLocation().Z < StartLocation.Z - 1000.0f);
	TestTrue(FString::Printf(TEXT("decal terminal speed (peak %.2f)"), PeakSpeed),
		PeakSpeed <= 2500.01);
	WoTTest::DestroySpecWorld(World);
	return true;
}

// WOTCarcass.uc:157 enters falling; no shipped carcass default enables bounce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditCarcassTerminalSpec,
	"WoT.Falling.Audit.CarcassTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditCarcassTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(0.0f, 0.0f, 10000.0f), FRotator::ZeroRotator);
	Carcass->SetVelocityForTest(FVector(0.0f, 0.0f, -2400.0f));
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 360 && IsValid(Carcass); ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Carcass->GetVelocityForTest().Size());
	}
	TestTrue(FString::Printf(TEXT("carcass terminal speed (peak %.2f)"), PeakSpeed),
		PeakSpeed <= 2500.01);
	WoTTest::DestroySpecWorld(World);
	return true;
}

// WOTCarcass.uc:505-532 lands on the first floor contact. HitWall's bounce
// calculation (:575-584) does not turn an ordinary floor landing into a bounce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditCarcassFirstFloorSpec,
	"WoT.Falling.Audit.CarcassFirstFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditCarcassFirstFloorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFallingAuditSurface(World, FVector(0.0f, 0.0f, -25.0f),
		FVector(1000.0f, 1000.0f, 25.0f));
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(0.0f, 0.0f, 20.0f), FRotator::ZeroRotator);
	Carcass->Capsule->SetCapsuleSize(5.0f, 5.0f);
	Carcass->SetVelocityForTest(FVector(0.0f, 0.0f, -300.0f));
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("first floor contact ends the carcass fall"),
		Carcass->IsLyingStill());
	TestTrue(TEXT("landed carcass velocity is zero"),
		Carcass->GetVelocityForTest().IsNearlyZero());
	const FVector RestLocation = Carcass->GetActorLocation();
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(TEXT("landed carcass stays at rest"),
		Carcass->GetActorLocation().Equals(RestLocation, 0.01f)
		&& Carcass->GetVelocityForTest().IsNearlyZero());
	WoTTest::DestroySpecWorld(World);
	return true;
}

// WOTCarcassLegion.uc:70-75 retains the normal support-loss behavior while
// sinking; the resulting falling motion still has the normal terminal speed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditSinkingCarcassTerminalSpec,
	"WoT.Falling.Audit.SinkingCarcassTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditSinkingCarcassTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* Support = SpawnFallingAuditSurface(World,
		FVector(0.0f, 0.0f, -25.0f), FVector(1000.0f, 1000.0f, 25.0f));
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(0.0f, 0.0f, 6.0f), FRotator::ZeroRotator);
	Carcass->Capsule->SetCapsuleSize(5.0f, 5.0f);
	Carcass->CarcassTypeName = TEXT("WoTPawns.WOTCarcassLegion");
	for (int32 Tick = 0; Tick < 120 && !Carcass->IsSinkingForTest(); ++Tick)
	{
		WoTTest::TickWorld(World, 1);
	}
	if (TestTrue(TEXT("floor contact begins sinking on its real support"),
		Carcass->IsSinkingForTest() && Carcass->GetSinkingBaseForTest() == Support))
	{
		Support->Destroy();
		WoTTest::TickWorld(World, 2);
		TestFalse(TEXT("support loss resumes falling while sinking"),
			Carcass->IsLyingStill());
		Carcass->SetVelocityForTest(FVector(0.0f, 0.0f, -2490.0f));
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("sinking carcass fall respects terminal speed"),
			Carcass->GetVelocityForTest().Size() <= 2500.01);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

// BounceableDecoration.uc:81-85 disables the next bounce. A subsequent floor
// contact lands and stops physics (:135-163), including spew-created actors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditSpewFloorRestSpec,
	"WoT.Falling.Audit.SpewFloorRest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditSpewFloorRestSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFallingAuditSurface(World, FVector(0.0f, 0.0f, -25.0f),
		FVector(1000.0f, 1000.0f, 25.0f));
	AWoTSpewChunk* Chunk = SpawnFallingAuditSpew(World,
		FVector(0.0f, 0.0f, 20.0f), FVector(0.0f, 0.0f, -60.0f), 10.0f);
	WoTTest::TickWorld(World, 1.0f);
	TestFalse(TEXT("small floor bounces eventually disarm bouncing"),
		Chunk->IsBouncingForTest());
	TestTrue(TEXT("spew chunk has zero velocity after landing"),
		Chunk->GetVelocityForTest().IsNearlyZero());
	const FVector RestLocation = Chunk->GetActorLocation();
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("resting spew chunk does not accumulate gravity"),
		Chunk->GetVelocityForTest().IsNearlyZero()
		&& Chunk->GetActorLocation().Equals(RestLocation, 0.01f));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditSpewWallTerminalSpec,
	"WoT.Falling.Audit.SpewWallTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditSpewWallTerminalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFallingAuditSurface(World, FVector(20.0f, 0.0f, 100.0f),
		FVector(5.0f, 100.0f, 100.0f));
	AWoTSpewChunk* Chunk = SpawnFallingAuditSpew(World,
		FVector(0.0f, 0.0f, 100.0f), FVector(200.0f, 0.0f, 0.0f), 100.0f);
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("real wall sweep disables the next bounce"),
		Chunk->GetBlockingHitCountForTest() > 0 && !Chunk->IsBouncingForTest());
	const float AfterWallZ = Chunk->GetActorLocation().Z;
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 360; ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Chunk->GetVelocityForTest().Size());
	}
	TestTrue(TEXT("a wall contact does not land the spew chunk"),
		Chunk->GetActorLocation().Z < AfterWallZ - 100.0f);
	TestTrue(FString::Printf(TEXT("non-bouncing spew terminal speed (peak %.2f)"),
		PeakSpeed), PeakSpeed <= 2500.01);
	WoTTest::DestroySpecWorld(World);
	return true;
}

// BounceableDecoration.uc:81-85 only changes the next hit's bounce behavior;
// a low-speed wall collision cannot invoke the floor-only Landed handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallingAuditBounceableWallSpec,
	"WoT.Falling.Audit.BounceableWallContinuesFalling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallingAuditBounceableWallSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFallingAuditSurface(World, FVector(20.0f, 0.0f, 100.0f),
		FVector(5.0f, 100.0f, 100.0f));
	AWoTBounceableDecoration* Decoration = World->SpawnActor<AWoTBounceableDecoration>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator);
	Decoration->ConfigureRuntimeCollision(true, false, false, 5.0f, 5.0f);
	Decoration->MinBounceSpeed = 100.0f;
	Decoration->SetupBounce(FVector(200.0f, 0.0f, 0.0f));
	WoTTest::TickWorld(World, 0.2f);
	const float AfterWallZ = Decoration->GetActorLocation().Z;
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(TEXT("a slow wall impact continues falling until a floor is reached"),
		Decoration->GetActorLocation().Z < AfterWallZ - 10.0f);
	TestTrue(TEXT("the airborne decoration retains downward velocity"),
		Decoration->GetBounceVelocity().Z < 0.0f);
	double PeakSpeed = 0.0;
	for (int32 Tick = 0; Tick < 360; ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSpeed = FMath::Max(PeakSpeed, Decoration->GetBounceVelocity().Size());
	}
	TestTrue(TEXT("after bouncing is disabled the continued fall reaches terminal speed"),
		FMath::IsNearlyEqual(Decoration->GetBounceVelocity().Size(), 2500.0, 0.1));
	TestTrue(FString::Printf(TEXT("non-bouncing decoration terminal speed (peak %.2f)"),
		PeakSpeed), PeakSpeed <= 2500.01);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
