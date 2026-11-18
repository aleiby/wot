#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCarcass.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Traps/WoTSpear.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTActorRotator.h"
#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTCreatureChunk.h"
#include "../WoTDecoration.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTRandom.h"
#include "../WoTSoundTable.h"
#include "../WoTTeleporter.h"
#include "../WoTWorldFX.h"
#include "../WoTZoneVolume.h"
#include "WoTTestAIClasses.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
constexpr EAutomationTestFlags P1B_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters P1B_Spawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTCreatureChunk* P1B_ChunkUpReplacement(UWorld& World,
	const FVector& Location)
{
	TSet<TWeakObjectPtr<AWoTCreatureChunk>> Before;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		Before.Add(*It);
	}

	AWoTCarcass* Source = World.SpawnActor<AWoTCarcass>(
		Location, FRotator::ZeroRotator, P1B_Spawn());
	if (!Source)
	{
		return nullptr;
	}
	Source->CarcassTypeName = NAME_None;
	WoTRandom::Seed(0xB41);
	Source->ChunkUp(100.0f);

	AWoTCreatureChunk* Replacement = nullptr;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		if (Before.Contains(*It))
		{
			continue;
		}
		if (!Replacement || It->Mass > Replacement->Mass)
		{
			Replacement = *It;
		}
	}
	return Replacement;
}

AWoTNPCCharacter* P1B_SpawnCreature(UWorld& World, FName Row,
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

AWoTCharacter* P1B_SpawnPlayer(UWorld& World, const FVector& Location,
	uint8 Team, AWoTPlayerController** OutController = nullptr)
{
	AWoTPlayerController* Controller = World.SpawnActor<AWoTPlayerController>(
		P1B_Spawn());
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
		Location, FRotator::ZeroRotator, P1B_Spawn());
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

template <typename T>
FWoTRuntimeWorldProperty P1B_ValueProperty(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, const T& Value,
	const TCHAR* StructName = TEXT(""))
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.StructName = StructName;
	Property.Value.SetNumUninitialized(sizeof(T));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(T));
	return Property;
}

FWoTRuntimeWorldProperty P1B_NameProperty(const TCHAR* Name,
	const TCHAR* Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.NameValue = Value;
	return Property;
}

FWoTRuntimeWorldProperty P1B_ObjectProperty(const TCHAR* Name,
	const TCHAR* Identity)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Object;
	Property.ObjectIdentity = Identity;
	return Property;
}

FWoTRuntimeClassDefaultRecord& P1B_AddClass(
	TArray<FWoTRuntimeClassDefaultRecord>& Classes, const TCHAR* Identity,
	const TCHAR* Superclass)
{
	FWoTRuntimeClassDefaultRecord& Record = Classes.AddDefaulted_GetRef();
	Record.Identity = Identity;
	Record.SuperclassIdentity = Superclass;
	return Record;
}

bool P1B_Project(UWorld& World, TArray<FWoTRuntimeWorldActor> Actors,
	TArray<FWoTRuntimeClassDefaultRecord> Classes,
	FWoTWorldProjectionResult& Out, FString& OutError)
{
	FWoTRuntimeClassDefaults Defaults;
	if (!Defaults.LoadFixture(MoveTemp(Classes), OutError))
	{
		return false;
	}
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("P1BFixture");
	Source.Records = Actors;
	return FWoTWorldProjector::ProjectDeferred(World, Source, Defaults, Out,
		OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_R4ChunkSubstepSpec,
	"WoT.Parity.P1B.R4ChunkSubstep", P1B_Flags)
bool FP1B_R4ChunkSubstepSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1B_Spawn());
	if (!TestNotNull(TEXT("water zone"), Water))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Water->Collision->SetBoxExtent(FVector(5000.0f));
	Water->bWaterZone = true;
	Water->ZoneFluidFriction = 1.2f;

	AWoTCreatureChunk* Chunk = P1B_ChunkUpReplacement(
		*World, FVector(0.0f, 0.0f, 1000.0f));
	if (!TestNotNull(TEXT("real underwater ChunkUp replacement"), Chunk))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0.0f, 0.0f, 1000.0f));
	TestEqual(TEXT("replacement begins in the authored water zone"),
		AWoTZoneVolume::ZoneAt(World, Chunk->GetActorLocation()), Water);
	Chunk->SetVelocityForTest(FVector(100.0f, 0.0f, 0.0f));
	const FVector StartLocation = Chunk->GetActorLocation();
	FVector ExpectedLocation = StartLocation;
	FVector ExpectedVelocity = Chunk->GetVelocityForTest();
	float RemainingTime = 0.5f;
	int32 Iterations = 0;
	while (RemainingTime > 0.0f && Iterations < 8)
	{
		++Iterations;
		const float TimeTick = RemainingTime > 0.1f
			? FMath::Min(0.1f, RemainingTime * 0.5f)
			: RemainingTime;
		RemainingTime -= TimeTick;
		ExpectedVelocity = ExpectedVelocity
			* (1.0f - 2.0f * Water->ZoneFluidFriction * TimeTick)
			+ 0.5f * Water->ZoneGravity
				* (1.0f - Chunk->Buoyancy / FMath::Max(1.0f, Chunk->Mass))
				* TimeTick;
		ExpectedLocation += ExpectedVelocity * TimeTick;
	}

	Chunk->Tick(0.5f);
	TestTrue(TEXT("retail substeps preserve horizontal direction"),
		Chunk->GetVelocityForTest().X > 0.0f);
	TestTrue(TEXT("hitch velocity matches retail falling substeps"),
		Chunk->GetVelocityForTest().Equals(ExpectedVelocity, 0.01f));
	TestTrue(TEXT("hitch displacement matches retail falling substeps"),
		Chunk->GetActorLocation().Equals(ExpectedLocation, 0.05f));

	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_ActivityManagerSpec,
	"WoT.Parity.P1B.ActivityManager", P1B_Flags)
bool FP1B_ActivityManagerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Guard = P1B_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTCharacter* Player = P1B_SpawnPlayer(
		*World, FVector(500, 0, 100), 2);
	AWoTNPCController* Controller = Guard
		? Cast<AWoTNPCController>(Guard->GetController()) : nullptr;
	if (TestNotNull(TEXT("production guard"), Guard)
		&& TestNotNull(TEXT("real NPC controller"), Controller)
		&& TestNotNull(TEXT("possessed player"), Player))
	{
		WoTTest::TickWorld(World, 1.1f);
		TestNotEqual(TEXT("nearby player keeps the guard active"),
			Controller->GetBrainState(), EWoTBrainState::Inactive);
		WoTTest::PlaceWithoutOverlapEvents(Player, FVector(4000, 0, 100));
		WoTTest::TickWorld(World, 1.2f);
		TestEqual(TEXT("one retail activity pulse force-inactivates the remote guard"),
			Controller->GetBrainState(), EWoTBrainState::Inactive);
		TestTrue(TEXT("ForceInactive stops movement"),
			Guard->GetVelocity().IsNearlyZero());
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_AcquiredTargetLockSpec,
	"WoT.Parity.P1B.AcquiredTargetLock", P1B_Flags)
bool FP1B_AcquiredTargetLockSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Guard = P1B_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTCharacter* First = P1B_SpawnPlayer(
		*World, FVector(600, 0, 100), 2);
	AWoTNPCController* Controller = Guard
		? Cast<AWoTNPCController>(Guard->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = Guard
		? Cast<UWoTTestBrainComponent>(Guard->Brain) : nullptr;
	if (TestNotNull(TEXT("guard"), Guard)
		&& TestNotNull(TEXT("controller"), Controller)
		&& TestNotNull(TEXT("forced-query brain"), Brain)
		&& TestNotNull(TEXT("first player"), First))
	{
		WoTTest::TickWorld(World, 0.6f);
		TestEqual(TEXT("first sighting becomes the acquired threat"),
			Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
			static_cast<AActor*>(First));
		Brain->bForceVisible = false;
		Brain->bForceReachable = false;
		Brain->bForcePathable = true;
		Brain->GetGoal(EWoTGoalIndex::Threat).Cache.Invalidate();
		Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
		Controller->SetForcedStationary(true);
		FWoTDurationNotifier& Offensive = Brain->Notifiers[
			static_cast<int32>(EWoTNotifierSlot::Offensive)];
		Offensive.Period = 1000.0f;
		Offensive.Elapsed = 0.0f;
		Offensive.bRandomizePhase = false;
		AWoTCharacter* Second = P1B_SpawnPlayer(
			*World, FVector(700, 40, 100), 2);
		if (TestNotNull(TEXT("second player"), Second))
		{
			WoTTest::TickWorld(World, 0.6f);
			TestEqual(TEXT("Acquired SeeEnemy stubs retain the existing threat"),
				Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
				static_cast<AActor*>(First));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_SoundTimerSpawnDelaySpec,
	"WoT.Parity.P1B.SoundTimerSpawnDelay", P1B_Flags)
bool FP1B_SoundTimerSpawnDelaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = P1B_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("real Soldier spawn"), Soldier))
	{
		const float* Next = Soldier->SoundSlotNextTime.Find(TEXT("WaitingRandom"));
		TestNotNull(TEXT("spawn randomized every listed timer slot"), Next);
		if (Next)
		{
			TestTrue(TEXT("WaitingRandom cannot fire immediately at spawn"),
				*Next > World->GetTimeSeconds());
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_SoundResetOddsFlipSpec,
	"WoT.Parity.P1B.SoundResetOddsFlip", P1B_Flags)
bool FP1B_SoundResetOddsFlipSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = P1B_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("real Soldier spawn"), Soldier))
	{
		const TArray<FWoTSoundSlotTimer>* Timers =
			WoTSoundTable::FindTimers(Soldier->SoundTableName);
		const FWoTSoundSlotTimer* Acquired = Timers
			? Timers->FindByPredicate([](const FWoTSoundSlotTimer& Timer)
				{ return Timer.Slot == TEXT("Acquired"); }) : nullptr;
		if (TestNotNull(TEXT("retail Acquired timer row"), Acquired))
		{
			uint32 Seed = 1;
			for (; Seed < 10000; ++Seed)
			{
				WoTRandom::Seed(Seed);
				if (WoTRandom::FRand() >= Acquired->Odds)
				{
					break;
				}
			}
			WoTRandom::Seed(Seed);
			TestFalse(TEXT("chosen first bReset odds check fails"),
				Soldier->PlayVoiceSlot(TEXT("Acquired")));
			TestTrue(TEXT("bReset flips to timed mode even when first odds check fails"),
				Soldier->SoundSlotsSwitchedToTimed.Contains(TEXT("Acquired")));
		}
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_SlotSoundNoiseSpec,
	"WoT.Parity.P1B.SlotSoundNoise", P1B_Flags)
bool FP1B_SlotSoundNoiseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Listener = P1B_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTNPCCharacter* Speaker = P1B_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(-500, 0, 100), 2);
	if (TestNotNull(TEXT("listener"), Listener)
		&& TestNotNull(TEXT("slot-sound speaker"), Speaker))
	{
		CastChecked<AWoTNPCController>(Listener->GetController())
			->SetBrainStateForG20Test(EWoTBrainState::Waiting);
		Listener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
		WoTRandom::Seed(0x51A7);
		TestTrue(TEXT("real NPC bark slot selects and plays"),
			Speaker->PlayVoiceSlot(TEXT("SeeEnemy")));
		TestEqual(TEXT("SetupAndPlaySound emits loudness-one AI noise"),
			Listener->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
			static_cast<AActor*>(Speaker));
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_HealZoneHundredCapSpec,
	"WoT.Parity.P1B.HealZoneHundredCap", P1B_Flags)
bool FP1B_HealZoneHundredCapSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1B_Spawn());
	AWoTNPCCharacter* Sister = P1B_SpawnCreature(
		*World, TEXT("Sister"), FVector(0, 0, 50), 1);
	if (TestNotNull(TEXT("heal zone"), Zone)
		&& TestNotNull(TEXT("real Sister troop"), Sister))
	{
		Zone->Collision->SetBoxExtent(FVector(500));
		Zone->bHealZone = true;
		Zone->HealPerSec = 20;
		Sister->Health = 200;
		WoTTest::TickWorld(World, 0.1f);
		TestEqual(TEXT("WOTPawn PainTimer skips healing at or above 100"),
			Sister->Health, 200);
		Sister->Health = 95;
		WoTTest::TickWorld(World, 1.1f);
		TestEqual(TEXT("NPC healing is capped at retail's hard 100"),
			Sister->Health, 100);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_NonWaterViewFogSpec,
	"WoT.Parity.P1B.NonWaterViewFog", P1B_Flags)
bool FP1B_NonWaterViewFogSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Player = P1B_SpawnPlayer(
		*World, FVector(0, 0, 50), 1, &Controller);
	AWoTZoneVolume* Abyss = World->SpawnActor<AWoTZoneVolume>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1B_Spawn());
	if (TestNotNull(TEXT("non-water abyss zone"), Abyss)
		&& TestNotNull(TEXT("possessed player"), Player)
		&& TestNotNull(TEXT("local controller"), Controller))
	{
		Abyss->Collision->SetBoxExtent(FVector(500));
		Abyss->bWaterZone = false;
		Abyss->ViewFog = FVector::OneVector;
		Abyss->ViewFlash = FVector::ZeroVector;
		WoTTest::TickWorld(World, 1.0f);
		TestTrue(TEXT("any head zone contributes its authored ViewFog"),
			Controller->CurrentViewFlashFog.Equals(FVector::OneVector, 0.01f));
		TestTrue(TEXT("non-water zone keeps neutral ViewFlash scale"),
			FMath::IsNearlyEqual(Controller->CurrentViewFlashScale, 1.0f, 0.01f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_SpearResetDelaySpec,
	"WoT.Parity.P1B.SpearResetDelay", P1B_Flags)
bool FP1B_SpearResetDelaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		FVector::ZeroVector, FRotator::ZeroRotator, P1B_Spawn());
	AWoTCharacter* Enemy = P1B_SpawnPlayer(*World, FVector(52, 0, 0), 2);
	if (TestNotNull(TEXT("real spear"), Spear)
		&& TestNotNull(TEXT("touching enemy"), Enemy))
	{
		Spear->Team = 1;
		Spear->TrapTouch(Enemy);
		WoTTest::PlaceWithoutOverlapEvents(Enemy, FVector(500, 0, 0));
		WoTTest::TickWorld(World, 0.55f);
		WoTTest::TickWorld(World, 2.05f);
		TestEqual(TEXT("spear began its retract"), Spear->GetPhase(),
			EWoTSpearPhase::Reseting);
		WoTTest::TickWorld(World, 1.05f);
		TestEqual(TEXT("TweenAnim .25 plus Timer 1 keeps it disarmed for 1.25 seconds"),
			Spear->GetPhase(), EWoTSpearPhase::Reseting);
		WoTTest::TickWorld(World, 0.25f);
		TestEqual(TEXT("spear eventually re-arms"), Spear->GetPhase(),
			EWoTSpearPhase::Waiting);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_BounceableSettlesFlatSpec,
	"WoT.Parity.P1B.BounceableSettlesFlat", P1B_Flags)
bool FP1B_BounceableSettlesFlatSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -20), FRotator::ZeroRotator, P1B_Spawn());
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor);
	FloorBox->SetBoxExtent(FVector(1000, 1000, 20));
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(FloorBox);
	FloorBox->RegisterComponent();
	AWoTSpewTrigger* Trigger = World->SpawnActor<AWoTSpewTrigger>(
		FVector(0, 0, 200), FRotator::ZeroRotator, P1B_Spawn());
	if (TestNotNull(TEXT("real SpewTrigger"), Trigger))
	{
		FWoTSpewThing& Thing = Trigger->Things.AddDefaulted_GetRef();
		Thing.Count = 1;
		Thing.MinSpeed = Thing.MaxSpeed = 0.0f;
		Thing.CollisionRadius = 8.0f;
		Thing.CollisionHeight = 8.0f;
		Thing.MinFirstHitSpeed = 5000.0f;
		Thing.MinBounceSpeed = 5000.0f;
		Thing.SpeedDampeningRatio = 0.0f;
		Thing.RotationDampeningRatio = 0.0f;
		Trigger->bCollision = true;
		Trigger->SpewCount = 1;
		Trigger->WoTTrigger(nullptr, nullptr);
		Trigger->Tick(0.0f);
		AWoTSpewChunk* Chunk = nullptr;
		for (TActorIterator<AWoTSpewChunk> It(World); It; ++It)
		{
			Chunk = *It;
			break;
		}
		if (TestNotNull(TEXT("trigger spawned a bounceable chunk"), Chunk))
		{
			Chunk->SetActorRotation(FRotator(37, 19, 53));
			WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(0, 0, 30));
			Chunk->Tick(0.3f);
			TestFalse(TEXT("slow first impact ends the bounce"),
				Chunk->IsBouncingForTest());
			TestTrue(TEXT("settled rubble lies flat"),
				FMath::IsNearlyZero(Chunk->GetActorRotation().Pitch, 0.1f)
				&& FMath::IsNearlyZero(Chunk->GetActorRotation().Roll, 0.1f));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_ActorRotatorCopySpec,
	"WoT.Parity.P1B.ActorRotatorCopy", P1B_Flags)
bool FP1B_ActorRotatorCopySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	P1B_AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	FWoTRuntimeClassDefaultRecord& Decoration = P1B_AddClass(
		Classes, TEXT("Engine.Decoration"), TEXT("Engine.Actor"));
	Decoration.Properties.Add(P1B_ObjectProperty(TEXT("Mesh"), TEXT("WOT.TestMesh")));
	P1B_AddClass(Classes, TEXT("WOT.Decoration"), TEXT("Engine.Decoration"));
	P1B_AddClass(Classes, TEXT("Legend.ActorRotator"), TEXT("Engine.Actor"));
	TArray<FWoTRuntimeWorldActor> Actors;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FWoTRuntimeWorldActor& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = FString::Printf(TEXT("P1B.Decoration%d"), Index);
		Actor.ClassIdentity = TEXT("WOT.Decoration");
		Actor.Properties.Add(P1B_NameProperty(TEXT("Tag"), TEXT("Orbit")));
	}
	FWoTRuntimeWorldActor& Template = Actors.AddDefaulted_GetRef();
	Template.Identity = TEXT("P1B.ActorRotator0");
	Template.ClassIdentity = TEXT("Legend.ActorRotator");
	Template.Properties.Add(P1B_NameProperty(TEXT("Event"), TEXT("Orbit")));
	for (const TCHAR* Flag : {TEXT("bFaceIn"), TEXT("bFaceOut"),
		TEXT("bBounceRadius"), TEXT("bBounceRotationRate"),
		TEXT("bBounceHeight")})
	{
		Template.Properties.Add(P1B_ValueProperty(Flag,
			EWoTRuntimeWorldPropertyType::Boolean, uint8(1)));
	}
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (TestTrue(TEXT("real projector accepts rotator fixture"),
		P1B_Project(*World, MoveTemp(Actors), MoveTemp(Classes), Projection, Error)))
	{
		TArray<AWoTActorRotator*> Rotators;
		for (AActor* Actor : Projection.DeferredActors)
		{
			if (AWoTActorRotator* Rotator = Cast<AWoTActorRotator>(Actor))
			{
				Rotators.Add(Rotator);
			}
		}
		Rotators.Sort([](const AWoTActorRotator& A, const AWoTActorRotator& B)
			{ return A.GetName() < B.GetName(); });
		TestEqual(TEXT("non-long-distance template creates one rotator per actor"),
			Rotators.Num(), 3);
		if (Rotators.Num() == 3)
		{
			TestTrue(TEXT("the first initialized template keeps authored flags"),
				Rotators[0]->bFaceIn && Rotators[0]->bFaceOut
				&& Rotators[0]->bBounceRadius
				&& Rotators[0]->bBounceRotationRate
				&& Rotators[0]->bBounceHeight);
			for (int32 Index = 1; Index < Rotators.Num(); ++Index)
			{
				TestFalse(TEXT("ActorRotator.Copy clones use false face/bounce defaults"),
					Rotators[Index]->bFaceIn || Rotators[Index]->bFaceOut
					|| Rotators[Index]->bBounceRadius
					|| Rotators[Index]->bBounceRotationRate
					|| Rotators[Index]->bBounceHeight);
			}
		}
	}
	else
	{
		AddError(Error);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_DragonFlyInstanceRateSpec,
	"WoT.Parity.P1B.DragonFlyInstanceRate", P1B_Flags)
bool FP1B_DragonFlyInstanceRateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	P1B_AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	FWoTRuntimeClassDefaultRecord& Decoration = P1B_AddClass(
		Classes, TEXT("Engine.Decoration"), TEXT("Engine.Actor"));
	Decoration.Properties.Add(P1B_ObjectProperty(TEXT("Mesh"), TEXT("WOT.TestMesh")));
	P1B_AddClass(Classes, TEXT("WoTDecorations.DragonFly"),
		TEXT("Engine.Decoration"));
	FWoTRuntimeWorldActor Fly;
	Fly.Identity = TEXT("P1B.DragonFly0");
	Fly.ClassIdentity = TEXT("WoTDecorations.DragonFly");
	Fly.Properties.Add(P1B_ValueProperty(TEXT("MinAnimRate"),
		EWoTRuntimeWorldPropertyType::Float, 2.0f));
	Fly.Properties.Add(P1B_ValueProperty(TEXT("MaxAnimRate"),
		EWoTRuntimeWorldPropertyType::Float, 4.0f));
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (TestTrue(TEXT("real projector accepts DragonFly fixture"),
		P1B_Project(*World, {Fly}, MoveTemp(Classes), Projection, Error)))
	{
		AWoTDecoration* Projected = nullptr;
		for (AActor* Actor : Projection.DeferredActors)
		{
			if ((Projected = Cast<AWoTDecoration>(Actor))) break;
		}
		if (TestNotNull(TEXT("projected DragonFly"), Projected))
		{
			TestEqual(TEXT("instance MinAnimRate survives PreBeginPlay projection"),
				Projected->MinAnimRate, 2.0f);
			TestEqual(TEXT("instance MaxAnimRate survives PreBeginPlay projection"),
				Projected->MaxAnimRate, 4.0f);
		}
	}
	else AddError(Error);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_TeleporterWithEffectsSilentSpec,
	"WoT.Parity.P1B.TeleporterWithEffectsSilent", P1B_Flags)
bool FP1B_TeleporterWithEffectsSilentSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	P1B_AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	P1B_AddClass(Classes, TEXT("Engine.Teleporter"), TEXT("Engine.Actor"));
	P1B_AddClass(Classes, TEXT("Legend.TeleporterWithEffects"),
		TEXT("Engine.Teleporter"));
	TArray<FWoTRuntimeWorldActor> Actors;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FWoTRuntimeWorldActor& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = FString::Printf(TEXT("P1B.Teleporter%d"), Index);
		Actor.ClassIdentity = TEXT("Legend.TeleporterWithEffects");
		const float Location[3] = {static_cast<float>(Index * 5000), 0, 100};
		Actor.Properties.Add(P1B_ValueProperty(TEXT("Location"),
			EWoTRuntimeWorldPropertyType::Struct, Location, TEXT("Vector")));
	}
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (TestTrue(TEXT("real projector accepts effect-teleporter fixture"),
		P1B_Project(*World, MoveTemp(Actors), MoveTemp(Classes), Projection, Error)))
	{
		TArray<AWoTTeleporter*> Teleporters;
		for (AActor* Actor : Projection.DeferredActors)
		{
			if (AWoTTeleporter* Teleporter = Cast<AWoTTeleporter>(Actor))
			{
				Teleporter->FinishSpawning(Teleporter->GetActorTransform());
				Teleporters.Add(Teleporter);
			}
		}
		Teleporters.Sort([](const AWoTTeleporter& A, const AWoTTeleporter& B)
			{ return A.GetActorLocation().X < B.GetActorLocation().X; });
		if (TestEqual(TEXT("two effect teleporters project"), Teleporters.Num(), 2))
		{
			Teleporters[0]->URL = TEXT("Exit");
			Teleporters[1]->TeleTag = TEXT("Exit");
			AWoTNPCCharacter* Listener = P1B_SpawnCreature(
				*World, TEXT("Trolloc"), FVector(100, 0, 100), 1);
			AWoTNPCCharacter* Traveler = P1B_SpawnCreature(
				*World, TEXT("Trolloc"), FVector(300, 0, 100), 2);
			if (TestNotNull(TEXT("nearby listener"), Listener)
				&& TestNotNull(TEXT("teleporting pawn"), Traveler))
			{
				CastChecked<AWoTNPCController>(Listener->GetController())
					->SetBrainStateForG20Test(EWoTBrainState::Waiting);
				Listener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
				Traveler->EmitNoise(1.0f);
				TestEqual(TEXT("hearing control acquires this pawn noise"),
					Listener->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
					static_cast<AActor*>(Traveler));
				WoTTest::TickWorld(World, 0.3f);
				CastChecked<AWoTNPCController>(Listener->GetController())
					->SetBrainStateForG20Test(EWoTBrainState::Waiting);
				Listener->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
				Traveler->SetActorLocation(FVector(20, 0, 100), false, nullptr,
					ETeleportType::TeleportPhysics);
				Teleporters[0]->NotifyActorBeginOverlap(Traveler);
				TestTrue(TEXT("real effect teleporter moved the pawn"),
					Traveler->GetActorLocation().X > 4000.0f);
				TestFalse(TEXT("TeleporterWithEffects does not call base MakeNoise"),
					Listener->Brain->IsGoalValid(EWoTGoalIndex::Threat));
			}
		}
	}
	else AddError(Error);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_DistanceFadeRISpec,
	"WoT.Parity.P1B.DistanceFadeRI", P1B_Flags)
bool FP1B_DistanceFadeRISpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	P1B_AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	FWoTRuntimeClassDefaultRecord& Decoration = P1B_AddClass(
		Classes, TEXT("Engine.Decoration"), TEXT("Engine.Actor"));
	Decoration.Properties.Add(P1B_ObjectProperty(TEXT("Mesh"), TEXT("WOT.TestMesh")));
	P1B_AddClass(Classes, TEXT("WOT.twotriangles"), TEXT("Engine.Decoration"));
	FWoTRuntimeWorldActor Sheet;
	Sheet.Identity = TEXT("Arena_06.twotriangles0");
	Sheet.ClassIdentity = TEXT("WOT.twotriangles");
	Sheet.Properties.Add(P1B_ObjectProperty(TEXT("RenderIteratorClass"),
		TEXT("Legend.DistanceFadeRI")));
	Sheet.Properties.Add(P1B_ValueProperty(TEXT("VisibilityRadius"),
		EWoTRuntimeWorldPropertyType::Float, 512.0f));
	Sheet.Properties.Add(P1B_ValueProperty(TEXT("VisibilityHeight"),
		EWoTRuntimeWorldPropertyType::Float, 256.0f));
	Sheet.Properties.Add(P1B_ValueProperty(TEXT("ScaleGlow"),
		EWoTRuntimeWorldPropertyType::Float, 0.75f));
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (TestTrue(TEXT("real projector accepts DistanceFadeRI fixture"),
		P1B_Project(*World, {Sheet}, MoveTemp(Classes), Projection, Error)))
	{
		AWoTDecoration* Projected = nullptr;
		for (AActor* Actor : Projection.DeferredActors)
		{
			if ((Projected = Cast<AWoTDecoration>(Actor))) break;
		}
		if (TestNotNull(TEXT("projected Arena_06 glow sheet"), Projected))
		{
			Projected->FinishSpawning(Projected->GetActorTransform());
			AWoTCharacter* Viewer = P1B_SpawnPlayer(
				*World, FVector(512, 0, 0), 1);
			WoTTest::TickWorld(World, 0.1f);
			TestTrue(TEXT("engine visibility radius culls the sheet at 512 uu"),
				!Projected->Mesh->IsVisible());
			TestTrue(TEXT("DistanceFadeRI is fully transparent at 512 uu"),
				FMath::IsNearlyZero(Projected->ScaleGlow, 0.01f));
			if (Viewer)
			{
				WoTTest::PlaceWithoutOverlapEvents(Viewer, FVector(12, 0, 0));
				WoTTest::TickWorld(World, 0.1f);
				TestTrue(TEXT("sheet fades in near the viewer"),
					Projected->Mesh->IsVisible()
					&& Projected->ScaleGlow > 0.7f);
			}
		}
	}
	else AddError(Error);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_FocusOnWaypointSpec,
	"WoT.Parity.P1B.FocusOnWaypoint", P1B_Flags)
bool FP1B_FocusOnWaypointSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = P1B_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1);
	AWoTCharacter* Target = P1B_SpawnPlayer(
		*World, FVector(900, 400, 100), 2);
	AWoTNPCController* Controller = Soldier
		? Cast<AWoTNPCController>(Soldier->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = Soldier
		? Cast<UWoTTestBrainComponent>(Soldier->Brain) : nullptr;
	if (TestNotNull(TEXT("production Soldier"), Soldier)
		&& TestNotNull(TEXT("real controller"), Controller)
		&& TestNotNull(TEXT("forced-query brain"), Brain)
		&& TestNotNull(TEXT("threat"), Target))
	{
		Brain->bForceVisible = false;
		Brain->bForceReachable = false;
		Brain->bForcePathable = true;
		Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
		Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
		Controller->Tick(0.1f);
		const FVector Waypoint = Controller->GetMovementManager().Destination;
		TestFalse(TEXT("pathable handler selected a route waypoint"),
			Waypoint.IsNearlyZero());
		TestTrue(TEXT("FocusSelectorOnWaypoint faces the selected waypoint"),
			Controller->GetFocalPoint().Equals(Waypoint, 0.1f));
		TestFalse(TEXT("routed guard no longer focuses through the wall on its target"),
			Controller->GetFocalPoint().Equals(Target->GetActorLocation(), 0.1f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_ApproachingWaypointSpec,
	"WoT.Parity.P1B.ApproachingWaypoint", P1B_Flags)
bool FP1B_ApproachingWaypointSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		FVector(0, 0, 0), FRotator::ZeroRotator, P1B_Spawn());
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor);
	FloorBox->SetBoxExtent(FVector(2000, 2000, 10));
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetRootComponent(FloorBox);
	FloorBox->RegisterComponent();
	AWoTNPCCharacter* Soldier = P1B_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 54), 1);
	AWoTCharacter* Target = P1B_SpawnPlayer(
		*World, FVector(900, 0, 54), 2);
	AWoTNPCController* Controller = Soldier
		? Cast<AWoTNPCController>(Soldier->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = Soldier
		? Cast<UWoTTestBrainComponent>(Soldier->Brain) : nullptr;
	if (TestNotNull(TEXT("production Soldier"), Soldier)
		&& TestNotNull(TEXT("real controller"), Controller)
		&& TestNotNull(TEXT("forced-query brain"), Brain)
		&& TestNotNull(TEXT("unreachable threat"), Target))
	{
		Brain->bForceVisible = true;
		Brain->bForceReachable = true;
		Brain->bForcePathable = true;
		Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
		FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
		const FVector GoalLocation = Target->GetActorLocation();
		const FVector ObserverLocation = Soldier->GetActorLocation();
		Threat.Cache.Stamp(EWoTGoalCacheItem::VisibleFail,
			GoalLocation, ObserverLocation);
		Threat.Cache.Stamp(EWoTGoalCacheItem::ReachableFail,
			GoalLocation, ObserverLocation);
		Threat.Cache.Stamp(EWoTGoalCacheItem::PathableFail,
			GoalLocation, ObserverLocation);
		Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
		const FVector Start = Soldier->GetActorLocation();
		Controller->Tick(0.05f);
		TestEqual(TEXT("failed spatial queries select the UnNavigable band"),
			Controller->GetTrackingProximity(), EWoTGoalProximity::UnNavigable);
		TestTrue(TEXT("UnNavigable uses WaypointSelectorApproaching"),
			Controller->GetTrackingBands().Band(EWoTGoalProximity::UnNavigable)
				.MovementBinding.WaypointSelector.ToString().EndsWith(
					TEXT("WaypointSelectorApproaching")));
		WoTTest::TickWorld(World, 1.0f, 0.05f);
		const FWoTGoal& Waypoint = Brain->GetGoal(EWoTGoalIndex::CurrentWaypoint);
		TestTrue(TEXT("WaypointSelectorApproaching authors a trace waypoint"),
			Waypoint.HasVector()
			&& !Waypoint.GoalVector.Equals(Start, 1.0f));
		TestTrue(TEXT("UnNavigable handler steps toward the threat"),
			Soldier->GetActorLocation().X > Start.X + 1.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_NPCMovementAnimationSpec,
	"WoT.Parity.P1B.NPCMovementAnimation", P1B_Flags)
bool FP1B_NPCMovementAnimationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Trolloc = P1B_SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	AWoTNPCController* Controller = Trolloc
		? Cast<AWoTNPCController>(Trolloc->GetController()) : nullptr;
	if (TestNotNull(TEXT("production Trolloc"), Trolloc)
		&& TestNotNull(TEXT("real controller"), Controller))
	{
		Trolloc->Brain->SetWaitingGoal(FVector(1000, 0, 100),
			FRotator::ZeroRotator);
		Controller->TransitionOnGoalPriority(true);
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Controller->Tick(0.05f);
			Trolloc->GetCharacterMovement()->Velocity = FVector(110, 0, 0);
			Trolloc->Tick(0.05f);
		}
		TestEqual(TEXT("110 uu/s Trolloc return uses the Walk slot"),
			Trolloc->RepAnim.Sequence, FName(TEXT("Walk")));
		const FWoTAnimTableEntry* Walk = WoTAnimTable::PickSlotAnim(
			Trolloc->AnimTableName, TEXT("Walk"), Trolloc);
		const float BaseRate = WoTAnimTable::ResolveRate(Walk);
		TestTrue(TEXT("walk loop rate is scaled by intended/base walking speed"),
			Walk && FMath::IsNearlyEqual(Trolloc->RepAnim.Rate,
				BaseRate * Controller->GetIntendedMovementSpeedForAnimation()
				/ Trolloc->AnimationBaseWalkingSpeed, 0.05f));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_NPCInactiveAnimationSpec,
	"WoT.Parity.P1B.NPCInactiveAnimation", P1B_Flags)
bool FP1B_NPCInactiveAnimationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = P1B_SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("production Soldier"), Soldier))
	{
		uint32 Seed = 1;
		FName Expected = NAME_None;
		for (; Seed < 10000; ++Seed)
		{
			WoTRandom::Seed(Seed);
			if (const FWoTAnimTableEntry* Pick = WoTAnimTable::PickSlotAnim(
				Soldier->AnimTableName, TEXT("Wait"), Soldier))
			{
				Expected = Pick->AnimName;
				if (Expected != TEXT("Breath")) break;
			}
		}
		TestFalse(TEXT("fixture chose a non-Breath Wait animation"),
			Expected.IsNone() || Expected == TEXT("Breath"));
		WoTRandom::Seed(Seed);
		Soldier->Tick(0.05f);
		TestEqual(TEXT("PlayInactiveAnimation selects from the Wait slot"),
			Soldier->RepAnim.Sequence, Expected);
		const uint8 FirstToken = Soldier->RepAnim.PlayToken;
		Soldier->Tick(10.0f);
		TestTrue(TEXT("each completed inactivity loop picks again"),
			Soldier->RepAnim.PlayToken != FirstToken);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1B_NPCInactiveSoundSpec,
	"WoT.Parity.P1B.NPCInactiveSound", P1B_Flags)
bool FP1B_NPCInactiveSoundSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Archer = P1B_SpawnCreature(
		*World, TEXT("Archer"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("production Archer"), Archer))
	{
		Archer->SoundSlotNextTime.Add(TEXT("WaitingRandom"), 0.0f);
		TSet<FName> WaitingSounds;
		if (const TArray<FWoTSoundTableEntry>* Table =
			WoTSoundTable::Find(Archer->SoundTableName))
		{
			for (const FWoTSoundTableEntry& Entry : *Table)
			{
				if (Entry.Slot == TEXT("WaitingRandom"))
				{
					WaitingSounds.Add(Entry.SoundSubPath);
				}
			}
		}
		TestTrue(TEXT("Archer has retail WaitingRandom entries"),
			WaitingSounds.Num() > 0);
		WoTRandom::Seed(0x1D1E);
		Archer->Tick(0.05f);
		Archer->Tick(10.0f);
		bool bPlayedWaitingRandom = false;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			bPlayedWaitingRandom |= WaitingSounds.Contains(It->SoundSubPath);
		}
		TestTrue(TEXT("WaitingIdle completion calls the real WaitingRandom slot"),
			bPlayedWaitingRandom);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
