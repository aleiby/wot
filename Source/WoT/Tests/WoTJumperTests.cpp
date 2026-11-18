#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../AI/WoTNPCCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTJumper.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
constexpr EAutomationTestFlags WoTJumperTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters JumperAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AActor* SpawnStepClearanceBox(UWorld* World, const FVector& Center,
	const FVector& Extent)
{
	AActor* BoxActor = World->SpawnActor<AActor>(
		AActor::StaticClass(), Center, FRotator::ZeroRotator, JumperAlwaysSpawn());
	if (!BoxActor)
	{
		return nullptr;
	}
	UBoxComponent* Box = NewObject<UBoxComponent>(
		BoxActor, TEXT("StepClearanceCollision"));
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	BoxActor->SetRootComponent(Box);
	BoxActor->AddInstanceComponent(Box);
	Box->RegisterComponent();
	// A plain AActor has no root when spawned, so apply the transform after the
	// collision box becomes its root.
	BoxActor->SetActorLocation(Center);
	return BoxActor;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTJumperAuthorityLaunchSpec,
	"WoT.Events.Jumper.AuthorityLaunchAndFilters", WoTJumperTestFlags)

bool FWoTJumperAuthorityLaunchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	AWoTJumper* Jumper = World->SpawnActorDeferred<AWoTJumper>(
		AWoTJumper::StaticClass(), FTransform(FRotator(0, 90, 0),
			FVector(1000, 0, 200)));
	Jumper->JumpZ = 320.0f;
	Jumper->bOnceOnly = true;
	Jumper->FinishSpawning(FTransform(FRotator(0, 90, 0),
		FVector(1000, 0, 200)));
	Pawn->GetCharacterMovement()->MaxWalkSpeed = 450.0f;
	Jumper->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	const FVector Velocity = Pawn->GetVelocity();
	TestTrue(TEXT("authority jumper launches along its authored rotation"),
		Velocity.Y > 400.0 && FMath::Abs(Velocity.X) < 5.0);
	TestTrue(TEXT("authored JumpZ becomes upward velocity"), Velocity.Z > 250.0);
	TestEqual(TEXT("launched character enters falling movement"),
		Pawn->GetCharacterMovement()->MovementMode, MOVE_Falling);
	TestEqual(TEXT("launched pawn faces the jumper direction"),
		Pawn->GetActorRotation().Yaw, 90.0, 0.1);
	TestEqual(TEXT("once-only jumper disables its collision"),
		Jumper->Capsule->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

	AWoTJumper* GruntOnly = World->SpawnActor<AWoTJumper>(
		FVector(2000, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	GruntOnly->LimitedToClassIdentity = TEXT("WOT.Grunt#export-4");
	Pawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_None);
	// Each filtered touch is delivered from inside the jumper's UE1 cylinder:
	// the class limit, not distance, must be what rejects or accepts.
	WoTTest::PlaceWithoutOverlapEvents(Pawn, GruntOnly->GetActorLocation());
	GruntOnly->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	TestTrue(TEXT("Grunt class limit rejects a player pawn"),
		Pawn->GetVelocity().IsNearlyZero());
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(2400, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	NPC->GetCharacterMovement()->MaxWalkSpeed = 300.0f;
	WoTTest::PlaceWithoutOverlapEvents(NPC, GruntOnly->GetActorLocation());
	GruntOnly->OnOverlap(nullptr, NPC, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	TestTrue(TEXT("Grunt #export class limit accepts a native retail creature"),
		NPC->GetVelocity().X > 250.0f);

	AWoTJumper* WOTPawnOnly = World->SpawnActor<AWoTJumper>(
		FVector(2600, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	WOTPawnOnly->LimitedToClassIdentity = TEXT("WOT.WOTPawn#export-17");
	WoTTest::PlaceWithoutOverlapEvents(Pawn, WOTPawnOnly->GetActorLocation());
	WOTPawnOnly->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	TestTrue(TEXT("WOTPawn #export class limit rejects a player pawn"),
		Pawn->GetVelocity().IsNearlyZero());
	NPC->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	NPC->GetCharacterMovement()->SetMovementMode(MOVE_None);
	WoTTest::PlaceWithoutOverlapEvents(NPC, WOTPawnOnly->GetActorLocation());
	WOTPawnOnly->OnOverlap(nullptr, NPC, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	TestTrue(TEXT("WOTPawn #export class limit accepts a native retail creature"),
		NPC->GetVelocity().X > 250.0f);

	AWoTJumper* ClientCopy = World->SpawnActor<AWoTJumper>(
		FVector(3000, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	ClientCopy->SetRole(ROLE_None);
	ClientCopy->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 0.02f, 0.005f);
	TestTrue(TEXT("ROLE_None client jumper cannot launch a pawn"),
		Pawn->GetVelocity().IsNearlyZero());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCollisionBlockerProfilesSpec,
	"WoT.Events.CollisionBlockers.RetailProfiles", WoTJumperTestFlags)

bool FWoTCollisionBlockerProfilesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCollisionBlocker* BlockAll = World->SpawnActor<AWoTCollisionBlocker>(
		FVector::ZeroVector, FRotator::ZeroRotator, JumperAlwaysSpawn());
	BlockAll->Configure(false, 72.0f, 120.0f);
	TestFalse(TEXT("BlockAll is not player-only"), BlockAll->bPlayerOnly);
	TestEqual(TEXT("BlockAll radius is authored"),
		BlockAll->Capsule->GetUnscaledCapsuleRadius(), 72.0f);
	TestEqual(TEXT("BlockAll height is authored"),
		BlockAll->Capsule->GetUnscaledCapsuleHalfHeight(), 120.0f);
	TestEqual(TEXT("BlockAll blocks world dynamics"),
		BlockAll->Capsule->GetCollisionResponseToChannel(ECC_WorldDynamic),
		ECR_Block);

	AWoTCollisionBlocker* BlockPlayer =
		World->SpawnActorDeferred<AWoTCollisionBlocker>(
			AWoTCollisionBlocker::StaticClass(),
			FTransform(FVector(500, 0, 0)));
	BlockPlayer->Configure(true, 40.0f, 80.0f);
	BlockPlayer->FinishSpawning(FTransform(FVector(500, 0, 0)));
	TestTrue(TEXT("BlockPlayer records its limited profile"),
		BlockPlayer->bPlayerOnly);
	TestEqual(TEXT("BlockPlayer blocks pawns"),
		BlockPlayer->Capsule->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	TestEqual(TEXT("BlockPlayer ignores world dynamics"),
		BlockPlayer->Capsule->GetCollisionResponseToChannel(ECC_WorldDynamic),
		ECR_Ignore);
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(1000, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	TestTrue(TEXT("BlockPlayer's runtime filter lets retail NPCs move through"),
		NPC->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(BlockPlayer));
	TestTrue(TEXT("BlockPlayer's filter is symmetric for blocker movement"),
		BlockPlayer->Capsule->GetMoveIgnoreActors().Contains(NPC));
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(1500, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	TestFalse(TEXT("BlockPlayer does not ignore player characters"),
		Player->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(BlockPlayer));

	AWoTCollisionBlocker* BlockMonsters =
		World->SpawnActorDeferred<AWoTCollisionBlocker>(
			AWoTCollisionBlocker::StaticClass(), FTransform(FVector(2500, 0, 100)));
	BlockMonsters->Configure(EWoTCollisionBlockerMode::Monsters, 48.0f, 12.0f);
	BlockMonsters->FinishSpawning(FTransform(FVector(2500, 0, 100)));
	const UBodySetup* SquatBody = BlockMonsters->Capsule->GetBodySetup();
	TestNotNull(TEXT("squat blocker has generated collision"), SquatBody);
	if (SquatBody && SquatBody->AggGeom.ConvexElems.Num() == 1)
	{
		const FBox Bounds = SquatBody->AggGeom.ConvexElems[0].ElemBox;
		TestEqual(TEXT("squat blocker preserves authored half-height"),
			Bounds.GetExtent().Z, 12.0, 0.1);
		TestTrue(TEXT("16-sided blocker preserves round radius"),
			Bounds.GetExtent().X > 47.9 && Bounds.GetExtent().Y > 47.9);
	}
	else
	{
		AddError(TEXT("squat blocker did not build exactly one convex cylinder"));
	}

	AWoTCharacter* LaterPlayer = World->SpawnActor<AWoTCharacter>(
		FVector(2380, 0, 100), FRotator::ZeroRotator, JumperAlwaysSpawn());
	// Spawned clear of the player so the player's sweep below cannot report a
	// start-penetrating pawn-vs-pawn hit instead of the blocker.
	AWoTNPCCharacter* LaterNPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(2380, 400, 100), FRotator::ZeroRotator, JumperAlwaysSpawn());
	TestTrue(TEXT("BlockMonsters ignores a player spawned after BeginPlay"),
		LaterPlayer->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(BlockMonsters));
	TestFalse(TEXT("BlockMonsters blocks an NPC spawned after BeginPlay"),
		LaterNPC->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(BlockMonsters));

	FHitResult PlayerHit;
	LaterPlayer->SetActorLocation(FVector(2620, 0, 100), true, &PlayerHit);
	TestFalse(FString::Printf(TEXT("player passes through squat BlockMonsters (hit %s)"),
		*GetNameSafe(PlayerHit.GetActor())), PlayerHit.bBlockingHit);
	LaterPlayer->SetActorLocation(FVector(4000, 0, 100), false);
	LaterNPC->SetActorLocation(FVector(2380, 0, 180), false);
	FHitResult AboveHit;
	LaterNPC->SetActorLocation(FVector(2620, 0, 180), true, &AboveHit);
	TestFalse(TEXT("NPC one floor above squat blocker is not blocked"),
		AboveHit.bBlockingHit);
	LaterNPC->SetActorLocation(FVector(2380, 0, 20), false);
	FHitResult BelowHit;
	LaterNPC->SetActorLocation(FVector(2620, 0, 20), true, &BelowHit);
	TestFalse(TEXT("NPC one floor below squat blocker is not blocked"),
		BelowHit.bBlockingHit);
	LaterNPC->SetActorLocation(FVector(2380, 0, 100), false);
	FHitResult InsideHit;
	LaterNPC->SetActorLocation(FVector(2620, 0, 100), true, &InsideHit);
	TestTrue(TEXT("NPC inside authored squat height is blocked"),
		InsideHit.bBlockingHit);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWalkableSlopeSpec,
	"WoT.Movement.WalkableSlope",
	WoTJumperTestFlags)
bool FWoTWalkableSlopeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, JumperAlwaysSpawn());
	UCharacterMovementComponent* Move = Pawn ? Pawn->GetCharacterMovement() : nullptr;
	if (!TestNotNull(TEXT("character movement"), Move))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	auto FloorHit = [](double NormalZ)
	{
		FHitResult Hit(1.0f);
		Hit.bBlockingHit = true;
		Hit.ImpactNormal = FVector(FMath::Sqrt(1.0 - NormalZ * NormalZ), 0.0, NormalZ);
		Hit.Normal = Hit.ImpactNormal;
		return Hit;
	};
	TestEqual(TEXT("walkable floor Z is UE1's 0.7"), Move->GetWalkableFloorZ(), 0.7f);
	TestTrue(TEXT("an exact 45 degree chamfer is floor, as in retail"),
		Move->IsWalkable(FloorHit(FMath::Cos(FMath::DegreesToRadians(45.0)))));
	// SetWalkableFloorZ round-trips through a float angle, so the stored
	// threshold sits a hair above 0.7; no retail surface is exactly 0.7.
	TestTrue(TEXT("Normal.Z 0.701 is floor (UE1 wall test is < 0.7)"),
		Move->IsWalkable(FloorHit(0.701)));
	TestFalse(TEXT("Normal.Z 0.69 is a sloped wall"), Move->IsWalkable(FloorHit(0.69)));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTStepUpClearanceSpec,
	"WoT.Movement.StepUpClearance",
	WoTJumperTestFlags)
bool FWoTStepUpClearanceSpec::RunTest(const FString&)
{
	struct FStepClearanceCase
	{
		float Height;
		bool bShouldClimb;
	};
	const FStepClearanceCase Cases[] = {
		{24.0f, true},
		{26.8f, true},
		{28.0f, false},
	};
	constexpr float RiserNearX = 200.0f;
	constexpr float TickStep = 1.0f / 120.0f;

	for (const FStepClearanceCase& TestCase : Cases)
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		if (!TestNotNull(TEXT("step-clearance world"), World))
		{
			return false;
		}
		AActor* Floor = SpawnStepClearanceBox(World,
			FVector(500.0f, 0.0f, -50.0f), FVector(1500.0f, 500.0f, 50.0f));
		AActor* Riser = SpawnStepClearanceBox(World,
			FVector(RiserNearX + 900.0f, 0.0f, 0.5f * TestCase.Height),
			FVector(900.0f, 500.0f, 0.5f * TestCase.Height));
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator,
			JumperAlwaysSpawn());
		UCharacterMovementComponent* Move = Pawn
			? Pawn->GetCharacterMovement() : nullptr;
		if (!TestNotNull(TEXT("step-clearance floor"), Floor)
			|| !TestNotNull(TEXT("step-clearance riser"), Riser)
			|| !TestNotNull(TEXT("step-clearance pawn"), Pawn)
			|| !TestNotNull(TEXT("step-clearance movement"), Move))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}

		Move->bRunPhysicsWithNoController = true;
		Move->SetMovementMode(MOVE_Falling);
		WoTTest::TickWorld(World, 120, TickStep);
		TestEqual(TEXT("step-clearance pawn settles into walking"),
			Move->MovementMode, MOVE_Walking);
		for (int32 Tick = 0; Tick < 240; ++Tick)
		{
			Pawn->AddMovementInput(FVector::ForwardVector, 1.0f, true);
			WoTTest::TickWorld(World, 1, TickStep);
		}

		const FVector End = Pawn->GetActorLocation();
		const double FeetZ = End.Z - Pawn->GetSimpleCollisionHalfHeight();
		const bool bClimbed = End.X > RiserNearX + 100.0f
			&& FeetZ > TestCase.Height - 1.0f;
		const FString Outcome = FString::Printf(
			TEXT("%.1f uu riser %s (end %.1f, %.1f)"), TestCase.Height,
			TestCase.bShouldClimb ? TEXT("is climbed") : TEXT("blocks"),
			End.X, FeetZ);
		TestEqual(*Outcome, bClimbed, TestCase.bShouldClimb);
		TestEqual(TEXT("MaxStepHeight restores to retail 25 after StepUp"),
			Move->MaxStepHeight, 25.0f);
		TestEqual(TEXT("WaterJump lip still reads 1.1 times retail 25"),
			1.1f * Move->MaxStepHeight, 27.5f);

		WoTTest::DestroySpecWorld(World);
	}
	return !HasAnyErrors();
}

#endif
