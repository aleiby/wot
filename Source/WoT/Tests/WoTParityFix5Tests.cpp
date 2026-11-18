#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTCreatureChunk.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTRandom.h"
#include "../WoTSoundTable.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

namespace
{
constexpr EAutomationTestFlags Fix5_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters Fix5_Spawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AStaticMeshActor* Fix5_SpawnBox(UWorld& World, const FVector& Location,
	const FVector& Extent, const FRotator& Rotation = FRotator::ZeroRotator)
{
	AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(
		Location, Rotation, Fix5_Spawn());
	if (!Actor)
	{
		return nullptr;
	}
	UBoxComponent* Box = NewObject<UBoxComponent>(Actor, TEXT("Fix5_Box"));
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Actor->SetRootComponent(Box);
	Box->RegisterComponent();
	Actor->SetActorLocationAndRotation(Location, Rotation,
		false, nullptr, ETeleportType::TeleportPhysics);
	return Actor;
}

AWoTCreatureChunk* Fix5_ChunkUpPart(UWorld& World, const FVector& Location,
	bool bRequireZeroPitch = false)
{
	TSet<TWeakObjectPtr<AWoTCreatureChunk>> Before;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		Before.Add(*It);
	}

	AWoTCarcass* Source = World.SpawnActor<AWoTCarcass>(
		Location, FRotator::ZeroRotator, Fix5_Spawn());
	if (!Source)
	{
		return nullptr;
	}
	Source->CarcassTypeName = NAME_None;
	WoTRandom::Seed(0xF105);
	Source->ChunkUp(100.0f);

	AWoTCreatureChunk* Replacement = nullptr;
	AWoTCreatureChunk* ZeroPitchPart = nullptr;
	TArray<AWoTCreatureChunk*> NewChunks;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		if (Before.Contains(*It))
		{
			continue;
		}
		NewChunks.Add(*It);
		if (!Replacement || It->Mass > Replacement->Mass)
		{
			Replacement = *It;
		}
		if (It->HasZeroPitchForTest())
		{
			ZeroPitchPart = *It;
		}
	}
	AWoTCreatureChunk* Selected = bRequireZeroPitch ? ZeroPitchPart : Replacement;
	for (AWoTCreatureChunk* Chunk : NewChunks)
	{
		if (Chunk != Selected)
		{
			Chunk->Destroy();
		}
	}
	return Selected;
}

AWoTNPCCharacter* Fix5_SpawnCreature(UWorld& World, FName Row,
	const FVector& Location, uint8 Team)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(World, Row, Location,
		Team, true, AWoTTestNPCCharacter::StaticClass());
	if (NPC)
	{
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return NPC;
}

AWoTCharacter* Fix5_SpawnPlayer(UWorld& World, const FVector& Location,
	uint8 Team, AWoTPlayerController** OutController = nullptr)
{
	AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(
		Fix5_Spawn());
	if (!Controller)
	{
		return nullptr;
	}
	Controller->SetAsLocalPlayerController();
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = Team;
	}
	AWoTCharacter* Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, Fix5_Spawn());
	if (Pawn)
	{
		Controller->Possess(Pawn);
		Pawn->Health = Pawn->MaxHealth = 100;
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	if (OutController)
	{
		*OutController = Controller;
	}
	return Pawn;
}

void Fix5_ZeroRotParam(double& Degrees)
{
	const float Wrapped = FRotator::ClampAxis(Degrees);
	Degrees = Wrapped < 90.0f || Wrapped >= 270.0f ? 0.0f : 180.0f;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5CreatureChunkSlowWallSpec,
	"WoT.Parity.Fix5.CreatureChunkSlowWallFallsAndLands", Fix5_Flags)
bool FFix5CreatureChunkSlowWallSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Fix5_SpawnBox(*World, FVector(0, 0, 0), FVector(1000, 1000, 10));
	Fix5_SpawnBox(*World, FVector(40, 0, 300), FVector(10, 1000, 150));
	AWoTCreatureChunk* Chunk = Fix5_ChunkUpPart(
		*World, FVector(0, 0, 300));
	if (TestNotNull(TEXT("real ChunkUp replacement"), Chunk))
	{
		WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0, 0, 300));
		Chunk->MinBounceSpeed = 1000.0f;
		Chunk->SetVelocityForTest(FVector(120, 0, 0));
		Chunk->Tick(0.25f);
		TestTrue(TEXT("slow vertical-wall hit is a real blocking hit"),
			Chunk->GetBlockingHitCountForTest() >= 1);
		TestFalse(TEXT("slow wall hit disables bounce only"),
			Chunk->IsBouncingForTest());
		TestFalse(TEXT("slow wall hit leaves the chunk falling"),
			Chunk->IsResting());
		const float BeforeGravity = Chunk->GetVelocityForTest().Z;
		Chunk->Tick(0.05f);
		const float GravityDelta = Chunk->GetVelocityForTest().Z - BeforeGravity;
		TestEqual(TEXT("non-bounce fall applies the full gravity step"),
			GravityDelta, -47.5f, 3.0f);
		for (int32 Step = 0; Step < 300 && !Chunk->IsResting(); ++Step)
		{
			Chunk->Tick(0.02f);
		}
		TestTrue(*FString::Printf(TEXT("the wall-slowed chunk reaches the floor below "
			"(location=%s velocity=%s bounce=%d hits=%d)"),
			*Chunk->GetActorLocation().ToCompactString(),
			*Chunk->GetVelocityForTest().ToCompactString(),
			Chunk->IsBouncingForTest() ? 1 : 0,
			Chunk->GetBlockingHitCountForTest()), Chunk->IsResting());
		TestEqual(TEXT("the floor contact takes the real Landed path"),
			Chunk->GetLandedCountForTest(), 1);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5CreatureChunkRotationSpec,
	"WoT.Parity.Fix5.CreatureChunkRetailLandedRotation", Fix5_Flags)
bool FFix5CreatureChunkRotationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Fix5_SpawnBox(*World, FVector(0, 0, 0), FVector(1000, 1000, 20),
		FRotator(0, 0, 20));
	AWoTCreatureChunk* Chunk = Fix5_ChunkUpPart(
		*World, FVector(0, 0, 260), true);
	if (TestNotNull(TEXT("real zero-pitch ChunkUp part"), Chunk))
	{
		TestTrue(TEXT("selected retail part authors bZeroPitch"),
			Chunk->HasZeroPitchForTest());
		WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0, 0, 260));
		Chunk->SetActorRotation(FRotator(135, 32, 15));
		Chunk->SetTumbleRateForTest(FRotator::ZeroRotator);
		Chunk->SetVelocityForTest(FVector(0, 0, -80));
		Chunk->MinBounceSpeed = 1000.0f;
		for (int32 Step = 0; Step < 400 && !Chunk->IsResting(); ++Step)
		{
			Chunk->Tick(0.02f);
		}
		TestTrue(TEXT("slow bounce settles through Landed"), Chunk->IsResting());
		const FVector HitNormal = Chunk->GetLastLandedNormalForTest();
		TestTrue(TEXT("fixture lands on the sloped floor"),
			HitNormal.Z > 0.7f && HitNormal.Z < 0.99f);
		FRotator Expected = Chunk->GetLastPreLandedRotationForTest();
		Fix5_ZeroRotParam(Expected.Pitch);
		const FVector SurfaceDirection =
			(FVector::UpVector - HitNormal).GetSafeNormal();
		if (!SurfaceDirection.IsNearlyZero())
		{
			Expected += SurfaceDirection.Rotation();
		}
		TestTrue(TEXT("Landed applies ZeroRotParam plus the surface rotator"),
			Chunk->GetActorRotation().Equals(Expected, 0.1f));
		TestTrue(TEXT("the authoritative rest pose still replicates as movement"),
			Chunk->GetIsReplicated() && Chunk->IsReplicatingMovement());
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5CreatureChunkFastBounceSpec,
	"WoT.Parity.Fix5.CreatureChunkFastFloorBounce", Fix5_Flags)
bool FFix5CreatureChunkFastBounceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Fix5_SpawnBox(*World, FVector(0, 0, 0), FVector(1000, 1000, 10));
	AWoTCreatureChunk* Chunk = Fix5_ChunkUpPart(
		*World, FVector(0, 0, 260));
	if (TestNotNull(TEXT("real ChunkUp replacement"), Chunk))
	{
		WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0, 0, 260));
		Chunk->SetVelocityForTest(FVector(0, 0, -1000));
		Chunk->MinBounceSpeed = 120.0f;
		Chunk->Tick(0.25f);
		TestTrue(TEXT("fast floor contact reaches HitWall"),
			Chunk->GetBlockingHitCountForTest() >= 1);
		TestTrue(TEXT("fast floor hit remains in bounce mode"),
			Chunk->IsBouncingForTest());
		TestEqual(TEXT("a bouncing floor hit does not call Landed"),
			Chunk->GetLandedCountForTest(), 0);
		TestFalse(TEXT("fast floor hit remains airborne"), Chunk->IsResting());
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5CreatureChunkBounceCarrySpec,
	"WoT.Parity.Fix5.CreatureChunkTwoBounceTimeCarry", Fix5_Flags)
bool FFix5CreatureChunkBounceCarrySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Fix5_SpawnBox(*World, FVector(-27, 0, 500), FVector(10, 1000, 300));
	Fix5_SpawnBox(*World, FVector(27, 0, 500), FVector(10, 1000, 300));
	AWoTCreatureChunk* Chunk = Fix5_ChunkUpPart(
		*World, FVector(0, 0, 500));
	if (TestNotNull(TEXT("real ChunkUp replacement"), Chunk))
	{
		WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0, 0, 500));
		Chunk->MinBounceSpeed = 0.0f;
		Chunk->SetVelocityForTest(FVector(1000, 0, 0));
		WoTRandom::Seed(0xB05CE);
		Chunk->Tick(0.1f);
		TestEqual(TEXT("only two bounces carry their unused slice time"),
			Chunk->GetBlockingHitCountForTest(), 3);
		TestTrue(TEXT("corridor impacts remain bouncing"),
			Chunk->IsBouncingForTest());
		TestEqual(TEXT("vertical corridor does not land"),
			Chunk->GetLandedCountForTest(), 0);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5NoPlayerActivitySpec,
	"WoT.Parity.Fix5.ActivityManagerNoPlayerNoOp", Fix5_Flags)
bool FFix5NoPlayerActivitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Guard = Fix5_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTNPCController* Controller = Guard
		? Cast<AWoTNPCController>(Guard->GetController()) : nullptr;
	if (TestNotNull(TEXT("active production guard"), Guard)
		&& TestNotNull(TEXT("real NPC controller"), Controller))
	{
		Controller->SetBrainStateForG20Test(EWoTBrainState::Waiting);
		Controller->Tick(1.1f);
		TestEqual(TEXT("no player pawn leaves an active guard unchanged"),
			Controller->GetBrainState(), EWoTBrainState::Waiting);
		AWoTCharacter* Distant = Fix5_SpawnPlayer(
			*World, FVector(6000, 0, 100), 2);
		if (TestNotNull(TEXT("distant player pawn"), Distant))
		{
			Controller->SetBrainStateForG20Test(EWoTBrainState::Waiting);
			Controller->Tick(1.1f);
			TestEqual(TEXT("an existing distant player still force-inactivates"),
				Controller->GetBrainState(), EWoTBrainState::Inactive);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5InactiveDamageSpec,
	"WoT.Parity.Fix5.InactiveDamageSeedsThreat", Fix5_Flags)
bool FFix5InactiveDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Guard = Fix5_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTPlayerController* AttackerController = nullptr;
	AWoTCharacter* Attacker = Fix5_SpawnPlayer(
		*World, FVector(3000, 0, 100), 2, &AttackerController);
	AWoTNPCController* Controller = Guard
		? Cast<AWoTNPCController>(Guard->GetController()) : nullptr;
	if (TestNotNull(TEXT("inactive production guard"), Guard)
		&& TestNotNull(TEXT("real NPC controller"), Controller)
		&& TestNotNull(TEXT("distant player attacker"), Attacker)
		&& TestNotNull(TEXT("attacker controller"), AttackerController))
	{
		Controller->SetBrainStateForG20Test(EWoTBrainState::Inactive);
		Guard->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
		const float BeforeHealth = Guard->Health;
		UGameplayStatics::ApplyDamage(Guard, 5.0f, AttackerController,
			Attacker, nullptr);
		TestTrue(TEXT("real damage event reaches the guard"),
			Guard->Health < BeforeHealth);
		TestEqual(TEXT("low damage does not wake the Inactive state"),
			Controller->GetBrainState(), EWoTBrainState::Inactive);
		TestEqual(TEXT("Inactive TakeDamage remembers the distant instigator"),
			Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
			static_cast<AActor*>(Attacker));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5ApproachingOffsetSpec,
	"WoT.Parity.Fix5.ApproachingVerticalGoalLateralOffset", Fix5_Flags)
bool FFix5ApproachingOffsetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Fix5_SpawnBox(*World, FVector(0, 0, 0), FVector(2000, 2000, 10));
	Fix5_SpawnBox(*World, FVector(140, 0, 140), FVector(5, 10, 220));
	AWoTNPCCharacter* Soldier = Fix5_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 54), 1);
	AWoTCharacter* Target = Fix5_SpawnPlayer(
		*World, FVector(900, 0, 954), 2);
	AWoTNPCController* Controller = Soldier
		? Cast<AWoTNPCController>(Soldier->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = Soldier
		? Cast<UWoTTestBrainComponent>(Soldier->Brain) : nullptr;
	if (TestNotNull(TEXT("production Soldier"), Soldier)
		&& TestNotNull(TEXT("real controller"), Controller)
		&& TestNotNull(TEXT("forced-query brain"), Brain)
		&& TestNotNull(TEXT("vertically offset threat"), Target))
	{
		Brain->bForceVisible = true;
		Brain->bForceReachable = true;
		Brain->bForcePathable = true;
		Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
		FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
		const FVector GoalLocation = Target->GetActorLocation();
		const FVector Origin = Soldier->GetActorLocation();
		Threat.Cache.Stamp(EWoTGoalCacheItem::VisibleFail, GoalLocation, Origin);
		Threat.Cache.Stamp(EWoTGoalCacheItem::ReachableFail, GoalLocation, Origin);
		Threat.Cache.Stamp(EWoTGoalCacheItem::PathableFail, GoalLocation, Origin);
		Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
		WoTRandom::Seed(0xA770);
		Controller->Tick(0.05f);
		FVector ExpectedGoalVector = GoalLocation - Origin;
		const float ExpectedProjected = ExpectedGoalVector.Size2D();
		if (ExpectedGoalVector.Z >= ExpectedProjected)
		{
			ExpectedGoalVector.Z = ExpectedProjected;
		}
		const float RawCrossMagnitude = FVector::CrossProduct(
			ExpectedGoalVector.GetSafeNormal(), FVector::UpVector).Size();
		TestTrue(TEXT("real approaching selector sampled its lateral distance"),
			Controller->GetApproachingOffsetDistanceForTest() >= 128.0f);
		TestEqual(TEXT("vertical goal scales the lateral offset by raw cross magnitude"),
			Controller->GetApproachingLateralOffsetForTest(),
			Controller->GetApproachingOffsetDistanceForTest() * RawCrossMagnitude,
			0.1f);
		const FWoTGoal& Waypoint =
			Brain->GetGoal(EWoTGoalIndex::CurrentWaypoint);
		TestTrue(TEXT("blocked straight trace selects a lateral waypoint"),
			Waypoint.HasVector() && FMath::Abs(Waypoint.GoalVector.Y) > 1.0f);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5MovementStartAnimationSpec,
	"WoT.Parity.Fix5.MoveFromRestStartsLocomotionAnim", Fix5_Flags)
bool FFix5MovementStartAnimationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Trolloc = Fix5_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTNPCController* Controller = Trolloc
		? Cast<AWoTNPCController>(Trolloc->GetController()) : nullptr;
	if (TestNotNull(TEXT("production Trolloc"), Trolloc)
		&& TestNotNull(TEXT("real controller"), Controller))
	{
		Trolloc->Brain->SetWaitingGoal(FVector(1000, 0, 100),
			FRotator::ZeroRotator);
		Controller->TransitionOnGoalPriority(true);
		Trolloc->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Controller->Tick(0.05f);
		TestTrue(TEXT("controller has real movement intent from rest"),
			Controller->GetIntendedMovementSpeedForAnimation() > 0.0f);
		Trolloc->Tick(0.0f);
		TestTrue(TEXT("first move frame selects Walk or Run instead of Wait"),
			Trolloc->RepAnim.Sequence == TEXT("Walk")
			|| Trolloc->RepAnim.Sequence == TEXT("Run"));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix5NonResetOddsCooldownSpec,
	"WoT.Parity.Fix5.NonResetOddsCooldown", Fix5_Flags)
bool FFix5NonResetOddsCooldownSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Questioner = Fix5_SpawnCreature(
		*World, TEXT("Questioner"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("production Questioner"), Questioner))
	{
		const TArray<FWoTSoundSlotTimer>* Timers =
			WoTSoundTable::FindTimers(Questioner->SoundTableName);
		const FWoTSoundSlotTimer* Reflected = Timers
			? Timers->FindByPredicate([](const FWoTSoundSlotTimer& Timer)
				{ return Timer.Slot == TEXT("Reflected"); }) : nullptr;
		if (TestNotNull(TEXT("retail non-reset Reflected timer"), Reflected))
		{
			Questioner->SoundSlotNextTime.Add(TEXT("Reflected"),
				World->GetTimeSeconds() + 4.0f);
			uint32 SuccessSeed = 1;
			for (; SuccessSeed < 10000; ++SuccessSeed)
			{
				WoTRandom::Seed(SuccessSeed);
				if (WoTRandom::FRand() < Reflected->Odds) break;
			}
			WoTRandom::Seed(SuccessSeed);
			TestFalse(TEXT("odds success inside cooldown does not bark"),
				Questioner->PlayVoiceSlot(TEXT("Reflected")));

			Questioner->SoundSlotNextTime.Add(TEXT("Reflected"),
				World->GetTimeSeconds());
			uint32 FailureSeed = 1;
			for (; FailureSeed < 10000; ++FailureSeed)
			{
				WoTRandom::Seed(FailureSeed);
				if (WoTRandom::FRand() >= Reflected->Odds) break;
			}
			WoTRandom::Seed(FailureSeed);
			TestTrue(TEXT("early odds success forces the next eligible attempt"),
				Questioner->PlayVoiceSlot(TEXT("Reflected")));

			WoTRandom::Seed(SuccessSeed);
			TestFalse(TEXT("successful playback starts the new 3-5 second cooldown"),
				Questioner->PlayVoiceSlot(TEXT("Reflected")));
		}
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
