// Headless specs for Legend climb math and WOTPlayer's ladder landing hook.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTClimbVelocitySpec,
	"WoT.Movement.WalkSurfaces.ClimbVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTClimbVelocitySpec::RunTest(const FString&)
{
	const FVector Initial(100.0f, 50.0f, 12.0f);
	const FVector Flat = UWoTCharacterMovement::ClimbVelocity(
		Initial, FRotator::ZeroRotator, 400.0f);
	const float BiasedZ = 400.0f
		* FMath::Sin(FMath::DegreesToRadians(22.5f));
	TestTrue(TEXT("forward view gets the +4096 pitch climb bias"),
		FMath::IsNearlyEqual(Flat.Z, BiasedZ, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("positive biased pitch damps horizontal X by 0.8"),
		FMath::IsNearlyEqual(Flat.X, 80.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("positive biased pitch damps horizontal Y by 0.8"),
		FMath::IsNearlyEqual(Flat.Y, 40.0f, KINDA_SMALL_NUMBER));

	const FVector Down = UWoTCharacterMovement::ClimbVelocity(
		Initial, FRotator(-45.0f, 0.0f, 0.0f), 400.0f);
	TestTrue(TEXT("looking down past the bias descends"), Down.Z < 0.0f);
	TestTrue(TEXT("negative biased pitch leaves horizontal X undamped"),
		FMath::IsNearlyEqual(Down.X, Initial.X, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("negative biased pitch leaves horizontal Y undamped"),
		FMath::IsNearlyEqual(Down.Y, Initial.Y, KINDA_SMALL_NUMBER));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLadderFlyingVelocitySpec,
	"WoT.Movement.WalkSurfaces.RetailFlyingVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLadderFlyingVelocitySpec::RunTest(const FString&)
{
	constexpr float DeltaTime = 1.0f / 30.0f;
	constexpr float FluidFriction = 1.2f;
	constexpr float AccelRate = 2048.0f;
	constexpr float AirSpeed = 400.0f;
	const FVector Initial(300.0f, 0.0f, 0.0f);

	// Hand-computed UE1 APawn::calcVelocity with AccelDir=0:
	// 300*(1-max(1,1.2)/30)*(1-1.2/30) = 276.48.
	const FVector NoAcceleration = UWoTCharacterMovement::RetailFlyingVelocity(
		Initial, FVector::ZeroVector, AccelRate, AirSpeed,
		FluidFriction, DeltaTime);
	TestTrue(TEXT("retail no-input flying damping at 30 Hz"),
		FMath::IsNearlyEqual(NoAcceleration.X, 276.48, 0.0001));
	TestTrue(TEXT("no-input flying damping does not invent lateral velocity"),
		FMath::IsNearlyZero(NoAcceleration.Y, 0.0001));

	// A transverse 4096 input is capped to AccelRate=2048 (:1053-54).
	// Direction friction first gives (288,12,0), fluid gives
	// (276.48,11.52,0), then acceleration adds (0,2048/30,0).
	const FVector WithAcceleration = UWoTCharacterMovement::RetailFlyingVelocity(
		Initial, FVector(0.0f, 4096.0f, 0.0f), AccelRate, AirSpeed,
		FluidFriction, DeltaTime);
	TestTrue(TEXT("retail accelerated flying X at 30 Hz"),
		FMath::IsNearlyEqual(WithAcceleration.X, 276.48, 0.0001));
	TestTrue(TEXT("retail accelerated flying caps to AccelRate"),
		FMath::IsNearlyEqual(
			WithAcceleration.Y, 79.7866666667, 0.0001));

	TestEqual(TEXT("LevelInfo/default ZoneFluidFriction"),
		GetDefault<AWoTZoneVolume>()->ZoneFluidFriction, 1.2f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWalkSurfaceMoverOcclusionSpec,
	"WoT.Movement.WalkSurfaces.MoverOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWalkSurfaceMoverOcclusionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>();
	// Start x=3 -> end x=1 hits the CSG boundary at x=2 (node 1).
	Table->ZoneBspPlanes = {
		FVector4(1.0, 0.0, 0.0, 0.0),
		FVector4(1.0, 0.0, 0.0, 2.0)
	};
	Table->ZoneBspFront = {1, -1};
	Table->ZoneBspBack = {-1, -1};
	Table->LeafBspCsg = {0, 1};
	Table->bBspRootOutside = true;
	Table->FrictionNodeIndices = {1};
	Table->FrictionNodeValues = {10.0f};
	const FVector Start(3.0f, 0.0f, 0.0f);
	const FVector End(1.0f, 0.0f, 0.0f);
	FWoTWalkSurfaceHit SurfaceHit;
	TestTrue(TEXT("unoccluded climbable BSP is returned"),
		UWoTCharacterMovement::TraceWalkSurface(
			Table, World, nullptr, Start, End, SurfaceHit));
	TestEqual(TEXT("unoccluded trace returns the climb node"),
		SurfaceHit.NodeIndex, 1);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMover* Mover = World->SpawnActor<AWoTMover>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("blocking mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UBoxComponent* Blocker = NewObject<UBoxComponent>(Mover, TEXT("TraceBlocker"));
	Mover->AddInstanceComponent(Blocker);
	Blocker->SetMobility(EComponentMobility::Movable);
	Blocker->SetupAttachment(Mover->GetRootComponent());
	Blocker->SetBoxExtent(FVector(0.1f, 10.0f, 10.0f));
	Blocker->SetCollisionProfileName(TEXT("BlockAll"));
	Blocker->RegisterComponent();
	// The +X face is exactly x=2: retail's coplanar/equal-distance mover hit
	// wins over LevelInfo and therefore yields no texture/no climb.
	Blocker->SetWorldLocation(FVector(1.9f, 0.0f, 0.0f));
	FWoTWalkSurfaceHit BlockedHit;
	TestFalse(TEXT("an equal-distance mover rejects the BSP surface"),
		UWoTCharacterMovement::TraceWalkSurface(
			Table, World, nullptr, Start, End, BlockedHit));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeProjectedLadderClimbSpec,
	"WoT.Movement.WalkSurfaces.RuntimeProjectedFriction10Climb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeProjectedLadderClimbSpec::RunTest(const FString&)
{
	UWorld* World=WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table=NewObject<UWoTMapLightTable>(World);
	// Retail-shaped output of world-v3 projection: a non-CSG routing root and
	// a solid ladder node, with the sparse Texture.Friction=10 channel.
	Table->ZoneBspPlanes={
		FVector4(1.0,0.0,0.0,0.0),
		FVector4(1.0,0.0,0.0,20.0)};
	Table->ZoneBspFront={1,-1};
	Table->ZoneBspBack={-1,-1};
	Table->LeafBspLeaves={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
	Table->LeafBspCsg={0,1};
	Table->bBspRootOutside=true;
	Table->FrictionNodeIndices={1};
	Table->FrictionNodeValues={10.0f};
	UWoTMeshLight::RegisterRuntimeMapLightTable(World,Table);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride=
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn=World->SpawnActor<AWoTCharacter>(
		FVector(30.0,0.0,100.0),FRotator(0.0,180.0,0.0),Params);
	if (!TestNotNull(TEXT("runtime ladder pawn"),Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTCharacterMovement* Movement=Cast<UWoTCharacterMovement>(
		Pawn->GetCharacterMovement());
	if (TestNotNull(TEXT("WoT movement component"),Movement))
	{
		Movement->SetMovementMode(MOVE_Walking);
		Movement->Velocity=FVector::ZeroVector;
		Movement->SetAccelerationForTesting(FVector(-2048.0,0.0,0.0));
		const double StartZ=Pawn->GetActorLocation().Z;
		Movement->PhysWalking(1.0f/30.0f,0);
		TestTrue(TEXT("friction-10 ladder selects climb velocity"),
			Movement->Velocity.Z>0.0);
		TestTrue(TEXT("one runtime ladder tick climbs upward"),
			Pawn->GetActorLocation().Z>StartZ);
		TestEqual(TEXT("ladder move keeps the persistent walking mode"),
			Movement->MovementMode,MOVE_Walking);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLadderLandingSpec,
	"WoT.Movement.WalkSurfaces.LadderLanding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLadderLandingSpec::RunTest(const FString&)
{
	const AWoTCharacter* Defaults = GetDefault<AWoTCharacter>();
	TestEqual(TEXT("shipped LadderLandingNoiseVelocity"),
		Defaults->LadderLandingNoiseVelocity, 200.0f);
	TestEqual(TEXT("shipped LadderLandedHitNormalZ"),
		Defaults->LadderLandedHitNormalZ, 0.001f);
	TestTrue(TEXT("a quiet velocity below 200 triggers ladder noise"),
		AWoTCharacter::ShouldMakeLadderLandingNoise(
			FVector(199.0f, 0.0f, 0.0f), 200.0f));
	TestFalse(TEXT("the shipped comparison is strict: exactly 200 is silent"),
		AWoTCharacter::ShouldMakeLadderLandingNoise(
			FVector(200.0f, 0.0f, 0.0f), 200.0f));

	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("ladder landing pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCharacterMovement()->Velocity = FVector(199.0f, 0.0f, 0.0f);
	FHitResult LadderHit;
	LadderHit.Normal = FVector(1.0f, 0.0f, 0.001f);
	Pawn->Landed(LadderHit);
	TestEqual(TEXT("side-normal Landed tweens toward the Run slot"),
		Pawn->GetReplicatedAnimName(), FName(TEXT("Run")));
	TestEqual(TEXT("the immediate replicated intent represents the tween target"),
		Pawn->RepAnim.bLoop, uint8(0));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
