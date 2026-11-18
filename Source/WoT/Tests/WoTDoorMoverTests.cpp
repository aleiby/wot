// Headless behavioral specs for DoorMover.uc + DoorTrigger.uc.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTDoorMover.h"
#include "../WoTDoorTrigger.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

namespace
{
	void ConfigureDoorBlockingCube(AWoTDoorMover* Door)
	{
		if (!Door || !Door->Mesh)
		{
			return;
		}
		Door->Mesh->SetStaticMesh(LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Door->Mesh->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	}

	AWoTDoorMover* SpawnDoorMoverForSpec(UWorld* World, FName Tag,
		const FVector& ForwardKey, const FVector& BackwardKey)
	{
		AWoTDoorMover* Door = World ? World->SpawnActorDeferred<AWoTDoorMover>(
			AWoTDoorMover::StaticClass(), FTransform::Identity) : nullptr;
		if (!Door)
		{
			return nullptr;
		}
		Door->KeyPos = {FVector::ZeroVector, ForwardKey, BackwardKey};
		Door->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
		                FRotator::ZeroRotator};
		Door->MoveTime = 0.2f;
		Door->StayOpenTime = 0.1f;
		Door->TagName = Tag;
		Door->FinishSpawning(FTransform::Identity);
		return Door;
	}

	AWoTDoorTrigger* SpawnDoorTriggerForSpec(UWorld* World, FName Event,
		EWoTDoorDirection Direction)
	{
		const FTransform FarAway(FRotator::ZeroRotator, FVector(10000, 0, 0));
		AWoTDoorTrigger* Trigger = World ? World->SpawnActorDeferred<AWoTDoorTrigger>(
			AWoTDoorTrigger::StaticClass(), FarAway) : nullptr;
		if (!Trigger)
		{
			return nullptr;
		}
		Trigger->EventName = Event;
		Trigger->TriggerType = EWoTTriggerType::PawnProximity;
		Trigger->Direction = Direction;
		Trigger->FinishSpawning(FarAway);
		return Trigger;
	}

	void FireDoorTriggerForSpec(AWoTDoorTrigger* Trigger, APawn* Pawn)
	{
		if (Trigger && Pawn)
		{
			// One explicit touch delivered from inside the trigger's UE1
			// cylinder, then the pawn returns to where the spec put it.
			const FVector Home = Pawn->GetActorLocation();
			WoTTest::PlaceWithoutOverlapEvents(Pawn, Trigger->GetActorLocation());
			Trigger->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
			WoTTest::PlaceWithoutOverlapEvents(Pawn, Home);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorForwardSpec,
	"WoT.Mover.Door.Forward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorForwardSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = SpawnDoorMoverForSpec(
		World, TEXT("ForwardDoor"), FVector(100, 0, 0), FVector(0, 100, 0));
	AWoTDoorTrigger* Trigger = SpawnDoorTriggerForSpec(
		World, TEXT("ForwardDoor"), EWoTDoorDirection::Forward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("trigger"), Trigger) ||
		!TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FireDoorTriggerForSpec(Trigger, Pawn);
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("forward trigger selects key one"), Door->GetOpenKey(), 1);
	TestEqual(TEXT("door dwells after forward opening"), Door->GetDoorPhase(),
		EWoTDoorPhase::Dwelling);
	TestTrue(TEXT("forward opening reaches key one"),
		Door->GetActorLocation().Equals(FVector(100, 0, 0), 1.0f));
	WoTTest::TickWorld(World, 20);
	TestEqual(TEXT("forward sequence returns idle"), Door->GetDoorPhase(),
		EWoTDoorPhase::Idle);
	TestTrue(TEXT("forward sequence returns to key zero"),
		Door->GetActorLocation().IsNearlyZero(1.0f));

	const FProperty* State = FindFProperty<FProperty>(
		AWoTDoorMover::StaticClass(), TEXT("DoorPhase"));
	TestTrue(TEXT("door phase replicates"), State && State->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorBackwardSpec,
	"WoT.Mover.Door.Backward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorBackwardSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = SpawnDoorMoverForSpec(
		World, TEXT("BackwardDoor"), FVector(100, 0, 0), FVector(0, 100, 0));
	AWoTDoorTrigger* Trigger = SpawnDoorTriggerForSpec(
		World, TEXT("BackwardDoor"), EWoTDoorDirection::Backward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("trigger"), Trigger) ||
		!TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FireDoorTriggerForSpec(Trigger, Pawn);
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("backward trigger selects key two"), Door->GetOpenKey(), 2);
	TestTrue(TEXT("backward opening reaches key two"),
		Door->GetActorLocation().Equals(FVector(0, 100, 0), 1.0f));
	WoTTest::TickWorld(World, 20);
	TestTrue(TEXT("backward sequence returns to key zero"),
		Door->GetActorLocation().IsNearlyZero(1.0f));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorLockSpec,
	"WoT.Mover.Door.LockGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorLockSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = World ? World->SpawnActorDeferred<AWoTDoorMover>(
		AWoTDoorMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("door"), Door))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Door->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(0, 100, 0)};
	Door->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
	                FRotator::ZeroRotator};
	Door->MoveTime = 0.2f;
	Door->StayOpenTime = 1.0f;
	Door->TagName = TEXT("LockedDoor");
	Door->bLockable = true;
	Door->FinishSpawning(FTransform::Identity);
	AWoTDoorTrigger* Trigger = SpawnDoorTriggerForSpec(
		World, TEXT("LockedDoor"), EWoTDoorDirection::Forward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("trigger"), Trigger) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FireDoorTriggerForSpec(Trigger, Pawn);
	WoTTest::TickWorld(World, 15);
	TestEqual(TEXT("keyless pawn leaves the locked door idle"), Door->GetDoorPhase(),
		EWoTDoorPhase::Idle);
	TestTrue(TEXT("keyless pawn leaves the locked door at key zero"),
		Door->GetActorLocation().IsNearlyZero(0.1f));

	const FTransform KeyTransform(FRotator::ZeroRotator, FVector(5000, 0, 0));
	AWoTAngrealInvFireball* Key = World ?
		World->SpawnActorDeferred<AWoTAngrealInvFireball>(
			AWoTAngrealInvFireball::StaticClass(), KeyTransform) : nullptr;
	if (!TestNotNull(TEXT("key"), Key))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Key->bHeldItem = true;
	Key->EventName = TEXT("LockedDoor");
	Key->FinishSpawning(KeyTransform);
	Pawn->InventoryComponent->AddInventory(Key);
	FireDoorTriggerForSpec(Trigger, Pawn);
	WoTTest::TickWorld(World, 8);
	TestTrue(TEXT("a matching inventory key opens the door"),
		Door->GetActorLocation().X > 50.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorTriggerCampSpec,
	"WoT.Mover.Door.TriggerCampNoRefire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorTriggerCampSpec::RunTest(const FString&)
{
	// DoorMover.uc:85-104 re-arms its triggers with Enable('Touch'): a pawn still
	// standing in the volume gets NO fresh Touch — it must exit and re-enter. A
	// camping pawn must not cycle the door forever. This spec drives the trigger
	// through REAL engine overlaps (spawn/SetActorLocation deliver them
	// synchronously in the test world) so a re-enable that re-ran UpdateOverlaps
	// would be caught.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = SpawnDoorMoverForSpec(
		World, TEXT("CampDoor"), FVector(100, 0, 0), FVector(0, 100, 0));
	AWoTDoorTrigger* Trigger = SpawnDoorTriggerForSpec(
		World, TEXT("CampDoor"), EWoTDoorDirection::Forward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(12000, 0, 100), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("trigger"), Trigger) ||
		!TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Trigger->Capsule->SetCapsuleSize(120.0f, 120.0f);
	// The floorless test world would let gravity pull the camper out of the
	// volume mid-spec; the camp must be positional, not physical.
	Pawn->GetCharacterMovement()->DisableMovement();

	Pawn->SetActorLocation(Trigger->GetActorLocation());   // step in -> Touch
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("stepping into the trigger opens the door"), Door->GetDoorPhase(),
		EWoTDoorPhase::Opening);

	// Full cycle (0.2s open + 0.1s dwell + 0.2s close) with the pawn camping
	// inside; FinishClosing re-arms the trigger while it still overlaps.
	WoTTest::TickWorld(World, 40);
	TestEqual(TEXT("re-arming under a camping pawn does not re-fire"),
		Door->GetDoorPhase(), EWoTDoorPhase::Idle);
	WoTTest::TickWorld(World, 20);
	TestEqual(TEXT("the door stays shut while the pawn keeps camping"),
		Door->GetDoorPhase(), EWoTDoorPhase::Idle);
	TestTrue(TEXT("the camped door rests at key zero"),
		Door->GetActorLocation().IsNearlyZero(1.0f));

	// Exit + re-enter delivers a fresh Touch: the door opens again.
	Pawn->SetActorLocation(FVector(12000, 0, 100));
	WoTTest::TickWorld(World, 2);
	Pawn->SetActorLocation(Trigger->GetActorLocation());
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("exit and re-enter re-fires the trigger"), Door->GetDoorPhase(),
		EWoTDoorPhase::Opening);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorDirectionalFallbackSpec,
	"WoT.Mover.Door.DirectionalFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorDirectionalFallbackSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* PlainMover = World ? World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("plain mover"), PlainMover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlainMover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	PlainMover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	PlainMover->MoveTime = 0.2f;
	PlainMover->StayOpenTime = 1.0f;
	PlainMover->InitialState = EWoTMoverState::TriggerOpenTimed;
	PlainMover->TagName = TEXT("PlainEvent");
	PlainMover->FinishSpawning(FTransform::Identity);

	const FTransform RelayTransform(FRotator::ZeroRotator, FVector(9000, 0, 0));
	AWoTTrigger* Relay = World ? World->SpawnActorDeferred<AWoTTrigger>(
		AWoTTrigger::StaticClass(), RelayTransform) : nullptr;
	if (!TestNotNull(TEXT("relay"), Relay))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Relay->TagName = TEXT("DirectionalEvent");
	Relay->EventName = TEXT("PlainEvent");
	Relay->FinishSpawning(RelayTransform);
	AWoTDoorTrigger* Directional = SpawnDoorTriggerForSpec(
		World, TEXT("DirectionalEvent"), EWoTDoorDirection::Backward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("directional trigger"), Directional) ||
		!TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FireDoorTriggerForSpec(Directional, Pawn);
	WoTTest::TickWorld(World, 8);
	TestTrue(TEXT("non-door target receives ordinary WoTTrigger fallback"),
		PlainMover->GetActorLocation().X > 50.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorLockPresentationSpec,
	"WoT.Door.Lock.IsUnlockedPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorLockPresentationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = World ? World->SpawnActorDeferred<AWoTDoorMover>(
		AWoTDoorMover::StaticClass(), FTransform::Identity) : nullptr;
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTPlayerController* PC = World ?
		World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("pawn"), Pawn) ||
		!TestNotNull(TEXT("player controller"), PC))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->Possess(Pawn);
	PC->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD());
	if (!TestNotNull(TEXT("player HUD"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	HUD->HandMessage.Message = TEXT("Sentinel");
	Door->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(0, 100, 0)};
	Door->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
		FRotator::ZeroRotator};
	Door->MoveTime = 0.1f;
	Door->StayOpenTime = 0.05f;
	Door->TagName = TEXT("PresentedDoor");
	Door->bLockable = true;
	Door->bStayUnlocked = true;
	Door->LockedSoundPath = TEXT("Spec/DoorLocked");
	Door->UnLockSoundPath = TEXT("Spec/DoorUnlock");
	Door->LockedMessage = TEXT("");
	Door->UnLockMessage = TEXT("");
	Door->FinishSpawning(FTransform::Identity);

	Door->WoTTriggerDirectional(true, Pawn, Pawn);
	TestEqual(TEXT("keyless door presents locked edge"), Door->LastLockSound,
		FName(TEXT("Spec/DoorLocked")));
	TestEqual(TEXT("authored empty locked message is suppressed"),
		HUD->HandMessage.Message, FString(TEXT("Sentinel")));
	const uint8 LockedCounter = Door->LockPresentationCounter;

	AWoTAngrealInvFireball* Key = World->SpawnActor<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), FVector(5000, 0, 0),
		FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("key"), Key))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Key->EventName = TEXT("PresentedDoor");
	Pawn->InventoryComponent->AddInventory(Key);
	// The PRODUCTION AddInventory path just messaged the acquisition ("You got
	// the Fireball ter'angreal") to this HUD; re-seed the sentinel so the assert
	// below isolates the DOOR's (suppressed) unlock message.
	HUD->HandMessage.Message = TEXT("Sentinel");
	Door->WoTTriggerDirectional(true, Pawn, Pawn);
	TestEqual(TEXT("key use presents unlock edge"), Door->LastLockSound,
		FName(TEXT("Spec/DoorUnlock")));
	TestEqual(TEXT("authored empty unlock message is suppressed"),
		HUD->HandMessage.Message, FString(TEXT("Sentinel")));
	TestEqual(TEXT("unlock increments the presentation nonce"),
		Door->LockPresentationCounter, static_cast<uint8>(LockedCounter + 1));
	const uint8 UnlockCounter = Door->LockPresentationCounter;
	WoTTest::TickWorld(World, 20);
	Pawn->InventoryComponent->DeleteInventory(Key);
	Door->WoTTriggerDirectional(true, Pawn, Pawn);
	TestEqual(TEXT("stay-unlocked path opens silently without retaining the key"),
		Door->LockPresentationCounter, UnlockCounter);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorGroupReturnPairSpec,
	"WoT.Door.Group.ReturnPair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorGroupReturnPairSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto DeferredDoor = [World](const FVector& Location) -> AWoTDoorMover*
	{
		AWoTDoorMover* Door = World ? World->SpawnActorDeferred<AWoTDoorMover>(
			AWoTDoorMover::StaticClass(), FTransform(FRotator::ZeroRotator, Location))
			: nullptr;
		if (!Door)
		{
			return nullptr;
		}
		Door->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(0, 100, 0)};
		Door->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
			FRotator::ZeroRotator};
		Door->MoveTime = 0.2f;
		Door->StayOpenTime = 0.05f;
		Door->TagName = TEXT("BigDoors01");
		return Door;
	};
	AWoTDoorMover* A = DeferredDoor(FVector::ZeroVector);
	AWoTDoorMover* B = DeferredDoor(FVector(0, 300, 0));
	ConfigureDoorBlockingCube(A);
	ConfigureDoorBlockingCube(B);
	AWoTDoorTrigger* Trigger = SpawnDoorTriggerForSpec(
		World, TEXT("BigDoors01"), EWoTDoorDirection::Forward);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("first door"), A) || !TestNotNull(TEXT("second door"), B) ||
		!TestNotNull(TEXT("door trigger"), Trigger) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	A->FinishSpawning(FTransform::Identity);
	B->FinishSpawning(FTransform(FRotator::ZeroRotator, FVector(0, 300, 0)));
	A->WoTTriggerDirectional(true, Pawn, Pawn);
	B->WoTTriggerDirectional(true, Pawn, Pawn);
	TestFalse(TEXT("opening doors mask the matching DoorTrigger"),
		Trigger->IsTriggerEnabled());
	WoTTest::TickWorld(World, 17);
	TestEqual(TEXT("first door is closing"), A->GetDoorPhase(), EWoTDoorPhase::Closing);
	TestEqual(TEXT("second door is closing"), B->GetDoorPhase(), EWoTDoorPhase::Closing);
	Pawn->SetActorLocation(FVector(50, 0, 0), false);
	const FVector ABefore = A->GetActorLocation();
	const FVector BBefore = B->GetActorLocation();
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("blocked pair leader reopens"), A->GetDoorPhase(),
		EWoTDoorPhase::Opening);
	TestEqual(TEXT("blocked pair follower reopens"), B->GetDoorPhase(),
		EWoTDoorPhase::Opening);
	TestTrue(TEXT("leader reversal is pose-continuous"),
		A->GetActorLocation().Equals(ABefore, 0.1f));
	// The follower may legally advance up to ONE tick of travel (100uu *
	// (1/60)/0.2 = 8.3uu) inside the reversal tick — actor tick order decides
	// whether it moved before the leader's encroach dispatch. One-tick tolerance
	// still catches a snap-to-phase-start (a ~90uu jump at this alpha).
	TestTrue(TEXT("follower reversal is pose-continuous"),
		B->GetActorLocation().Equals(BBefore, 10.0f));
	A->MakeGroupStop();
	TestEqual(TEXT("group stop freezes first door"), A->GetDoorPhase(),
		EWoTDoorPhase::Stopped);
	TestEqual(TEXT("group stop freezes second door"), B->GetDoorPhase(),
		EWoTDoorPhase::Stopped);
	TestTrue(TEXT("group stop re-arms matching DoorTriggers"),
		Trigger->IsTriggerEnabled());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoorSoundEdgesSpec,
	"WoT.Door.SoundEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDoorSoundEdgesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDoorMover* Door = SpawnDoorMoverForSpec(
		World, TEXT("SoundDoor"), FVector(100, 0, 0), FVector(0, 100, 0));
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Door->MoveTime = 0.1f;
	Door->StayOpenTime = 0.1f;
	Door->OpeningSoundPath = TEXT("Spec/Opening");
	Door->OpenedSoundPath = TEXT("Spec/Opened");
	Door->ClosingSoundPath = TEXT("Spec/Closing");
	Door->ClosedSoundPath = TEXT("Spec/Closed");
	Door->MoveAmbientSoundPath = TEXT("Spec/MoveLoop");
	const int32 InitialEdgeCount = Door->PhaseSoundPlayCount;
	Door->WoTTriggerDirectional(true, Pawn, Pawn);
	TestEqual(TEXT("opening edge"), Door->LastPhaseSound, FName(TEXT("Spec/Opening")));
	TestTrue(TEXT("opening loop"), Door->bMoveLoopOn);
	TestEqual(TEXT("opening call site plays one edge"), Door->PhaseSoundPlayCount,
		InitialEdgeCount + 1);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("opened edge"), Door->LastPhaseSound, FName(TEXT("Spec/Opened")));
	TestFalse(TEXT("dwell is silent"), Door->bMoveLoopOn);
	TestEqual(TEXT("opened call site plays one edge"), Door->PhaseSoundPlayCount,
		InitialEdgeCount + 2);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("closing edge"), Door->LastPhaseSound, FName(TEXT("Spec/Closing")));
	TestTrue(TEXT("closing loop"), Door->bMoveLoopOn);
	TestEqual(TEXT("closing call site plays one edge"), Door->PhaseSoundPlayCount,
		InitialEdgeCount + 3);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("closed edge"), Door->LastPhaseSound, FName(TEXT("Spec/Closed")));
	TestFalse(TEXT("closed pose is silent"), Door->bMoveLoopOn);
	TestEqual(TEXT("closed call site plays one edge"), Door->PhaseSoundPlayCount,
		InitialEdgeCount + 4);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
