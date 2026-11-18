// Headless behavioral specs for G26/G27 mover groups, encroachment, locks,
// attachment, and the remaining script mover FSMs.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../WoTAttachMover.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTDecoration.h"
#include "../WoTElevatorMover.h"
#include "../WoTFragment.h"
#include "../WoTGradualMover.h"
#include "../WoTHUD.h"
#include "../WoTMixMover.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTStageTrigger.h"

#include "Engine/Engine.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

namespace
{
	AWoTMover* DeferredGroupMover(UWorld* World, const FVector& Location,
		FName Tag, bool bSlave = false)
	{
		if (!World)
		{
			return nullptr;
		}
		AWoTMover* Mover = World->SpawnActorDeferred<AWoTMover>(
			AWoTMover::StaticClass(), FTransform(FRotator::ZeroRotator, Location));
		if (!Mover)
		{
			return nullptr;
		}
		Mover->KeyPos = {FVector::ZeroVector, FVector(120.0f, 0.0f, 0.0f)};
		Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
		Mover->MoveTime = 1.0f;
		Mover->StayOpenTime = 1.0f;
		Mover->InitialState = EWoTMoverState::TriggerToggle;
		Mover->TagName = Tag;
		Mover->bSlave = bSlave;
		return Mover;
	}

	bool FinishDeferredMover(AWoTMover* Mover, const FVector& Location)
	{
		if (!Mover)
		{
			return false;
		}
		Mover->FinishSpawning(FTransform(FRotator::ZeroRotator, Location));
		return true;
	}

	void ConfigureBlockingCube(AWoTMover* Mover)
	{
		if (!Mover || !Mover->Mesh)
		{
			return;
		}
		UStaticMesh* Cube = LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		Mover->Mesh->SetStaticMesh(Cube);
		Mover->Mesh->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	}

	AWoTMover* SpawnEncroachMover(UWorld* World, EWoTMoverEncroachType Type,
		int32 Damage = 0, const FVector& Location = FVector::ZeroVector)
	{
		AWoTMover* Mover = DeferredGroupMover(World, Location, TEXT("Encroach"));
		if (!Mover)
		{
			return nullptr;
		}
		Mover->MoveTime = 0.1f;
		Mover->EncroachType = Type;
		Mover->EncroachDamage = Damage;
		ConfigureBlockingCube(Mover);
		FinishDeferredMover(Mover, Location);
		return Mover;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverGroupLeaderElectionSpec,
	"WoT.Mover.Group.LeaderElection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverGroupLeaderElectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("world"), World))
	{
		return false;
	}
	AWoTMover* A = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Pair"));
	AWoTMover* B = DeferredGroupMover(World, FVector(0, 300, 0), TEXT("Pair"));
	AWoTMover* C = DeferredGroupMover(World, FVector(0, 600, 0), TEXT("Odd"));
	if (!TestNotNull(TEXT("first mover"), A) ||
		!TestNotNull(TEXT("second mover"), B) ||
		!TestNotNull(TEXT("odd mover"), C))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FinishDeferredMover(A, FVector::ZeroVector);
	FinishDeferredMover(B, FVector(0, 300, 0));
	FinishDeferredMover(C, FVector(0, 600, 0));

	TestEqual(TEXT("empty ReturnGroup resolves from Tag"), A->ReturnGroup,
		FName(TEXT("Pair")));
	TestEqual(TEXT("the pair shares one elected leader"), B->GetGroupLeader(), A);
	TestEqual(TEXT("the elected leader points to itself"), A->GetGroupLeader(), A);
	TestEqual(TEXT("the odd mover leads itself"), C->GetGroupLeader(), C);
	TestEqual(TEXT("the pair is linked through the follower chain"),
		A->GetGroupFollower(), B);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverGroupReturnSpec,
	"WoT.Mover.Group.ReturnReversesGroup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverGroupReturnSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* A = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Pair"));
	AWoTMover* B = DeferredGroupMover(World, FVector(0, 300, 0), TEXT("Pair"));
	if (!TestNotNull(TEXT("leader"), A) || !TestNotNull(TEXT("follower"), B))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FinishDeferredMover(A, FVector::ZeroVector);
	FinishDeferredMover(B, FVector(0, 300, 0));
	A->WoTTrigger(nullptr, nullptr);
	B->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 18);
	const FVector ABefore = A->GetActorLocation();
	const FVector BBefore = B->GetActorLocation();
	A->MakeGroupReturn();
	TestEqual(TEXT("leader reverses to closing"), A->GetMoverPhase(),
		EWoTMoverPhase::Closing);
	TestEqual(TEXT("follower reverses to closing"), B->GetMoverPhase(),
		EWoTMoverPhase::Closing);
	TestTrue(TEXT("leader pose is continuous at reversal"),
		A->GetActorLocation().Equals(ABefore, 0.1f));
	TestTrue(TEXT("follower pose is continuous at reversal"),
		B->GetActorLocation().Equals(BBefore, 0.1f));
	TestTrue(TEXT("reversal backdates the phase clock"),
		A->GetPhaseStartServerTime() < World->GetTimeSeconds());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverGroupStopSpec,
	"WoT.Mover.Group.StopFreezesGroup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverGroupStopSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* A = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Pair"));
	AWoTMover* B = DeferredGroupMover(World, FVector(0, 300, 0), TEXT("Pair"));
	if (!TestNotNull(TEXT("leader"), A) || !TestNotNull(TEXT("follower"), B))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FinishDeferredMover(A, FVector::ZeroVector);
	FinishDeferredMover(B, FVector(0, 300, 0));
	A->WoTTrigger(nullptr, nullptr);
	B->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 18);
	A->MakeGroupStop();
	const FVector AStopped = A->GetActorLocation();
	const FVector BStopped = B->GetActorLocation();
	const float AAlpha = A->GetStoppedAlpha();
	WoTTest::TickWorld(World, 30);
	TestEqual(TEXT("leader enters replicated stopped phase"), A->GetMoverPhase(),
		EWoTMoverPhase::Stopped);
	TestEqual(TEXT("follower enters replicated stopped phase"), B->GetMoverPhase(),
		EWoTMoverPhase::Stopped);
	TestTrue(TEXT("leader remains at the stopped pose"),
		A->GetActorLocation().Equals(AStopped, 0.1f));
	TestTrue(TEXT("follower remains at the stopped pose"),
		B->GetActorLocation().Equals(BStopped, 0.1f));
	TestTrue(TEXT("stopped alpha is the proxy pose source"), AAlpha > 0.1f && AAlpha < 0.9f);
	const FProperty* AlphaProperty = FindFProperty<FProperty>(
		AWoTMover::StaticClass(), TEXT("StoppedAlpha"));
	TestTrue(TEXT("stopped alpha replicates"),
		AlphaProperty && AlphaProperty->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverLockPresentationSpec,
	"WoT.Mover.Lock.ThrottledLockedPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverLockPresentationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Locked"));
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTPlayerController* PC = World ?
		World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("mover"), Mover) || !TestNotNull(TEXT("pawn"), Pawn) ||
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
	Mover->InitialState = EWoTMoverState::BumpOpenTimed;
	Mover->bLockable = true;
	Mover->LockedSoundPath = TEXT("Spec/Locked");
	Mover->LockedMessage = TEXT("Locked");
	Mover->MinLockSoundTime = 1.0f;
	FinishDeferredMover(Mover, FVector::ZeroVector);
	const FHitResult Hit;
	const uint8 InitialCounter = Mover->LockPresentationCounter;
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, Hit);
	TestEqual(TEXT("retail suppresses a locked bump during the first second"),
		Mover->LockPresentationCounter, InitialCounter);
	TestTrue(TEXT("first-second suppression presents no locked sound"),
		Mover->LastLockSound.IsNone());
	WoTTest::TickWorld(World, 61);
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, Hit);
	const uint8 FirstCounter = Mover->LockPresentationCounter;
	TestEqual(TEXT("first post-throttle bump presents locked sound"), Mover->LastLockSound,
		FName(TEXT("Spec/Locked")));
	TestEqual(TEXT("first keyless bump sends the authored hand message"),
		HUD->HandMessage.Message, FString(TEXT("Locked")));
	TestEqual(TEXT("hand message keeps the authored duration"),
		HUD->HandMessage.LifeSpan, 4.0f);
	HUD->HandMessage.Message = TEXT("Sentinel");
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, Hit);
	TestEqual(TEXT("second bump inside throttle is silent"),
		Mover->LockPresentationCounter, FirstCounter);
	TestEqual(TEXT("message is throttled with the sound"),
		HUD->HandMessage.Message, FString(TEXT("Sentinel")));
	WoTTest::TickWorld(World, 61);
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, Hit);
	TestEqual(TEXT("presentation replays after throttle window"),
		Mover->LockPresentationCounter, static_cast<uint8>(FirstCounter + 1));
	TestEqual(TEXT("message replays after the throttle window"),
		HUD->HandMessage.Message, FString(TEXT("Locked")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverUnlockDelaySpec,
	"WoT.Mover.Lock.UnlockDelayAndLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverUnlockDelaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Locked"));
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("mover"), Mover) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->InitialState = EWoTMoverState::BumpOpenTimed;
	Mover->BumpType = EWoTMoverBumpType::PawnBump;
	Mover->bLockable = true;
	Mover->bStayUnlocked = true;
	Mover->UnLockDuration = 0.1f;
	Mover->MoveTime = 0.03f;
	Mover->StayOpenTime = 0.0f;
	Mover->UnLockSoundPath = TEXT("Spec/Unlock");
	FinishDeferredMover(Mover, FVector::ZeroVector);
	AWoTAngrealInvFireball* Key = World->SpawnActor<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), FVector(1000, 0, 0),
		FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("key"), Key))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Key->EventName = TEXT("Locked");
	Pawn->InventoryComponent->AddInventory(Key);
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, FHitResult());
	TestEqual(TEXT("unlock presents immediately"), Mover->LastLockSound,
		FName(TEXT("Spec/Unlock")));
	TestEqual(TEXT("unlock delay holds the mover closed"), Mover->GetMoverPhase(),
		EWoTMoverPhase::Closed);
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("delay has not elapsed"), Mover->GetMoverPhase(),
		EWoTMoverPhase::Closed);
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("the original unlocking pawn starts the open after delay"),
		Mover->GetMoverPhase(), EWoTMoverPhase::Opening);
	TestTrue(TEXT("stay-unlocked latches"), Mover->bUnlocked);
	const uint8 LatchedCounter = Mover->LockPresentationCounter;
	WoTTest::TickWorld(World, 8);
	Pawn->InventoryComponent->DeleteInventory(Key);
	Mover->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, FHitResult());
	TestEqual(TEXT("latched mover opens silently after the key is gone"),
		Mover->LockPresentationCounter, LatchedCounter);
	TestEqual(TEXT("latched mover starts another open"), Mover->GetMoverPhase(),
		EWoTMoverPhase::Opening);

	AWoTMover* Repeating = DeferredGroupMover(
		World, FVector(0, 500, 0), TEXT("RepeatingLock"));
	AWoTAngrealInvFireball* RepeatingKey = World->SpawnActor<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), FVector(1000, 0, 0),
		FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("non-latching mover"), Repeating) ||
		!TestNotNull(TEXT("non-latching key"), RepeatingKey))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Repeating->InitialState = EWoTMoverState::BumpOpenTimed;
	Repeating->BumpType = EWoTMoverBumpType::PawnBump;
	Repeating->bLockable = true;
	Repeating->bStayUnlocked = false;
	Repeating->MoveTime = 0.03f;
	Repeating->StayOpenTime = 0.0f;
	Repeating->UnLockSoundPath = TEXT("Spec/RepeatingUnlock");
	FinishDeferredMover(Repeating, FVector(0, 500, 0));
	RepeatingKey->EventName = TEXT("RepeatingLock");
	Pawn->InventoryComponent->AddInventory(RepeatingKey);
	Repeating->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, FHitResult());
	const uint8 FirstRepeatingCounter = Repeating->LockPresentationCounter;
	WoTTest::TickWorld(World, 8);
	Repeating->NotifyHit(nullptr, Pawn, nullptr, true, FVector::ZeroVector,
		FVector::UpVector, FVector::ZeroVector, FHitResult());
	TestEqual(TEXT("non-latching lock replays unlock presentation every bump"),
		Repeating->LockPresentationCounter,
		static_cast<uint8>(FirstRepeatingCounter + 1));
	TestFalse(TEXT("non-latching lock never sets bUnlocked"), Repeating->bUnlocked);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverEncroachReturnSpec,
	"WoT.Mover.Encroach.ReturnOnPawnBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverEncroachReturnSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = SpawnEncroachMover(
		World, EWoTMoverEncroachType::ReturnWhenEncroach);
	AWoTMover* Follower = SpawnEncroachMover(
		World, EWoTMoverEncroachType::ReturnWhenEncroach,
		0, FVector(0, 300, 0));
	if (!TestNotNull(TEXT("mover"), Mover) ||
		!TestNotNull(TEXT("group follower"), Follower))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->WoTTrigger(nullptr, nullptr);
	Follower->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("mover reaches open pose before the closing obstruction"),
		Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("blocking pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Mover->WoTTrigger(nullptr, nullptr);
	Follower->WoTTrigger(nullptr, nullptr);
	// The cube's leading face (half-extent 50 from x=120) meets the 17uu pawn
	// capsule at x<=67, i.e. closing alpha <=0.558 -> the third 1/60s tick of the
	// 0.1s close. Tick past it; the reversal keeps Opening live for ~3 more ticks.
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("blocking pawn reopens the closing mover"),
		Mover->GetMoverPhase(), EWoTMoverPhase::Opening);
	TestEqual(TEXT("leader dispatch reopens the whole closing group"),
		Follower->GetMoverPhase(), EWoTMoverPhase::Opening);
	TestTrue(TEXT("return verdict runs the take-hit voice gate"),
		Pawn->NextPainSoundTime > World->GetTimeSeconds());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverEncroachCrushSpec,
	"WoT.Mover.Encroach.CrushKillsPlayerOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverEncroachCrushSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = SpawnEncroachMover(
		World, EWoTMoverEncroachType::CrushWhenEncroach);
	AWoTCharacter* PlayerPawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTCharacter* NPC = World ? World->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 0), FRotator::ZeroRotator) : nullptr;
	APlayerController* PC = World ? World->SpawnActor<APlayerController>() : nullptr;
	if (!TestNotNull(TEXT("mover"), Mover) ||
		!TestNotNull(TEXT("player pawn"), PlayerPawn) ||
		!TestNotNull(TEXT("NPC pawn"), NPC) || !TestNotNull(TEXT("controller"), PC))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->Possess(PlayerPawn);
	const int32 NPCHealth = NPC->Health;
	TestFalse(TEXT("crush verdict lets the mover proceed"),
		Mover->EncroachingOn(PlayerPawn));
	TestTrue(TEXT("player-controlled pawn receives Health+30 Suicided damage"),
		PlayerPawn->Health <= 0);
	TestFalse(TEXT("NPC crush verdict also lets the mover proceed"),
		Mover->EncroachingOn(NPC));
	TestEqual(TEXT("base/NPC KilledBy is the retail no-op"), NPC->Health, NPCHealth);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverEncroachIgnoreDamageSpec,
	"WoT.Mover.Encroach.IgnoreChipsDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverEncroachIgnoreDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = SpawnEncroachMover(
		World, EWoTMoverEncroachType::IgnoreWhenEncroach, 1);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(60, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("mover"), Mover) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 Before = Pawn->Health;
	Mover->WoTTrigger(nullptr, Pawn);
	WoTTest::TickWorld(World, 7);
	TestTrue(TEXT("ignore sweep reaches the target through the pawn"),
		Mover->GetActorLocation().Equals(FVector(120, 0, 0), 0.1f));
	TestTrue(TEXT("real swept encroachment applies EncroachDamage"), Pawn->Health < Before);
	const int32 AfterSweep = Pawn->Health;
	TestFalse(TEXT("direct ignore verdict still does not abort movement"),
		Mover->EncroachingOn(Pawn));
	TestEqual(TEXT("direct EncroachDamage chips once"), Pawn->Health, AfterSweep - 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverEncroachDebrisSpec,
	"WoT.Mover.Encroach.DebrisAndPickupClearing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverEncroachDebrisSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = SpawnEncroachMover(
		World, EWoTMoverEncroachType::IgnoreWhenEncroach);
	AWoTFragment* Fragment = World ? World->SpawnActor<AWoTFragment>(
		FVector(1000, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTAngrealInvFireball* Pickup = World ? World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(1200, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTSeal* Seal = World ? World->SpawnActor<AWoTSeal>(
		FVector(1400, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTCarcass* Carcass = World ? World->SpawnActor<AWoTCarcass>(
		FVector(1600, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTDecoration* Decoration = World ? World->SpawnActor<AWoTDecoration>(
		FVector(1800, 0, 0), FRotator::ZeroRotator) : nullptr;
	AActor* Carrier = World ? World->SpawnActor<AActor>(
		FVector(2000, 0, 0), FRotator::ZeroRotator) : nullptr;
	AWoTAngrealInvFireball* CarriedPickup = World ?
		World->SpawnActor<AWoTAngrealInvFireball>(
			FVector(2000, 0, 0), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("mover"), Mover) || !TestNotNull(TEXT("fragment"), Fragment) ||
		!TestNotNull(TEXT("pickup"), Pickup) || !TestNotNull(TEXT("seal"), Seal) ||
		!TestNotNull(TEXT("carcass"), Carcass) ||
		!TestNotNull(TEXT("decoration"), Decoration) ||
		!TestNotNull(TEXT("non-pawn carrier"), Carrier) ||
		!TestNotNull(TEXT("carried pickup"), CarriedPickup))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("fragment clearing never blocks"), Mover->EncroachingOn(Fragment));
	TestTrue(TEXT("fragment is destroyed"), Fragment->IsActorBeingDestroyed());
	TestFalse(TEXT("world pickup clearing never blocks"), Mover->EncroachingOn(Pickup));
	TestTrue(TEXT("unowned non-seal pickup is destroyed"), Pickup->IsActorBeingDestroyed());
	CarriedPickup->SetOwner(Carrier);
	TestFalse(TEXT("owned pickup clearing never blocks"),
		Mover->EncroachingOn(CarriedPickup));
	TestFalse(TEXT("inventory owned by a non-pawn carrier survives"),
		CarriedPickup->IsActorBeingDestroyed());
	TestFalse(TEXT("seal exemption never blocks"), Mover->EncroachingOn(Seal));
	TestFalse(TEXT("seal survives encroachment"), Seal->IsActorBeingDestroyed());
	TestFalse(TEXT("carcass crushing never blocks"), Mover->EncroachingOn(Carcass));
	TestTrue(TEXT("10000 Crushed gibs the carcass"), Carcass->IsActorBeingDestroyed());
	TestFalse(TEXT("10000 Crushed dispatch to a decoration never blocks"),
		Mover->EncroachingOn(Decoration));
	TestEqual(TEXT("decoration observes the 10000 Crushed damage"),
		Decoration->LastDamageTaken, 10000.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverSlaveSpec,
	"WoT.Mover.SlaveRidesMaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMoverSlaveSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Master = DeferredGroupMover(World, FVector::ZeroVector, TEXT("Ride"));
	AWoTMover* Slave = DeferredGroupMover(World, FVector(0, 150, 0), TEXT("Ride"), true);
	if (!TestNotNull(TEXT("master"), Master) || !TestNotNull(TEXT("slave"), Slave))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FinishDeferredMover(Slave, FVector(0, 150, 0));
	FinishDeferredMover(Master, FVector::ZeroVector);
	TestEqual(TEXT("slave attaches to the same-tag master"),
		Slave->GetAttachParentActor(), static_cast<AActor*>(Master));
	TestTrue(TEXT("slave FSM is inert"), Slave->IsMoverFSMInert());
	Slave->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 10);
	TestEqual(TEXT("slave phase never runs independently"), Slave->GetMoverPhase(),
		EWoTMoverPhase::Closed);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTElevatorMoverKeyframeSpec,
	"WoT.ElevatorMover.KeyframeChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTElevatorMoverKeyframeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTElevatorMover* Mover = World ? World->SpawnActorDeferred<AWoTElevatorMover>(
		AWoTElevatorMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("elevator"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(200, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
		FRotator::ZeroRotator};
	Mover->OpeningSoundPath = TEXT("Spec/Open");
	Mover->ClosedSoundPath = TEXT("Spec/CrossedClosed");
	Mover->FinishSpawning(FTransform::Identity);
	Mover->MoveKeyframe(2, 0.4f);
	Mover->MoveKeyframe(1, 0.1f);
	TestFalse(TEXT("busy MoveKeyframe gate rejects a second command"), Mover->CanMoveKey());
	// The next leg begins in the same tick as the crossed completion edge. Silence
	// its opening cue so LastPhaseSound retains the crossed FinishedClosing edge.
	Mover->OpeningSoundPath = NAME_None;
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("half the total duration reaches the middle key"),
		Mover->GetCurrentKey(), 1);
	TestEqual(TEXT("rising leg uses crossed FinishedClosing sound"),
		Mover->LastPhaseSound, FName(TEXT("Spec/CrossedClosed")));
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("chain reaches requested key"), Mover->GetCurrentKey(), 2);
	TestTrue(TEXT("chain re-arms MoveKeyframe"), Mover->CanMoveKey());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTStageTriggerSpec,
	"WoT.StageTrigger.StageDrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTStageTriggerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTStageTrigger* Stage = World ? World->SpawnActorDeferred<AWoTStageTrigger>(
		AWoTStageTrigger::StaticClass(), FTransform::Identity) : nullptr;
	AWoTElevatorMover* Elevator = World ? World->SpawnActorDeferred<AWoTElevatorMover>(
		AWoTElevatorMover::StaticClass(), FTransform::Identity) : nullptr;
	AWoTDispatcher* Dispatcher = World ? World->SpawnActorDeferred<AWoTDispatcher>(
		AWoTDispatcher::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("stage"), Stage) ||
		!TestNotNull(TEXT("elevator"), Elevator) ||
		!TestNotNull(TEXT("dispatcher"), Dispatcher))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Stage->EventName = TEXT("Lift");
	Stage->TagInc = TEXT("RaiseStage");
	Stage->MaxKeyFrame = 1;
	Stage->DispatcherTags[0] = TEXT("StageDispatcher");
	Elevator->TagName = TEXT("Lift");
	Elevator->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Elevator->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Dispatcher->TagName = TEXT("StageDispatcher");
	Dispatcher->OutEvents = {FName(TEXT("RaiseStage"))};
	Dispatcher->OutDelays = {0.0f};
	Dispatcher->FinishSpawning(FTransform::Identity);
	Stage->FinishSpawning(FTransform::Identity);
	Elevator->FinishSpawning(FTransform::Identity);
	Dispatcher->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("dispatcher out-event increments the stage"), Stage->CurrentStage, 1);
	TestFalse(TEXT("stage drive makes the elevator busy"), Elevator->CanMoveKey());
	TestFalse(TEXT("elevator motion disables linked dispatchers"),
		Dispatcher->IsTriggerEnabled());
	WoTTest::TickWorld(World, 61);
	TestEqual(TEXT("stage drives matching elevator at the one-second literal"),
		Elevator->GetCurrentKey(), 1);
	TestTrue(TEXT("elevator completion re-enables linked dispatchers"),
		Dispatcher->IsTriggerEnabled());
	Dispatcher->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("stage clamps at MaxKeyFrame"), Stage->CurrentStage, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGradualMoverTagSpec,
	"WoT.GradualMover.TagRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGradualMoverTagSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGradualMover* Mover = World ? World->SpawnActorDeferred<AWoTGradualMover>(
		AWoTGradualMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("gradual mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->InitialState = EWoTMoverState::GradualTriggerOpenTimed;
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0),
		FVector(200, 0, 0), FVector(300, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
		FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->OpenTimes = {0.1f, 0.1f, 0.1f, 0, 0, 0};
	Mover->CloseTimes = {0.1f, 0.1f, 0.1f, 0, 0, 0};
	Mover->Tags = {FName(TEXT("First")), FName(TEXT("First")),
		FName(TEXT("Second")), FName(TEXT("Top")), NAME_None, NAME_None};
	Mover->Events = {FName(TEXT("E0")), FName(TEXT("E1")),
		FName(TEXT("E2")), FName(TEXT("E3")), NAME_None, NAME_None};
	Mover->StayOpenTime = 0.1f;
	Mover->FinishSpawning(FTransform::Identity);
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("same-tag legs chain until the first changed tag"),
		Mover->GetCurrentKey(), 2);
	TestEqual(TEXT("tag/event rotate at the reached slot"), Mover->TagName,
		FName(TEXT("Second")));
	Mover->WoTTrigger(nullptr, nullptr);
	// Open leg 2->3 (0.1s) + StayOpenTime dwell (0.1s) + three close legs (0.3s)
	// = 0.5s = 30 ticks; give the chain margin.
	WoTTest::TickWorld(World, 36);
	TestEqual(TEXT("full close resets slot zero tag"), Mover->TagName,
		FName(TEXT("First")));

	AWoTGradualMover* Toggle = World->SpawnActorDeferred<AWoTGradualMover>(
		AWoTGradualMover::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 500, 0)));
	if (!TestNotNull(TEXT("toggle mover"), Toggle))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Toggle->InitialState = EWoTMoverState::GradualTriggerToggle;
	Toggle->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Toggle->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Toggle->OpenTimes = {0.05f, 0, 0, 0, 0, 0};
	Toggle->CloseTimes = {0.05f, 0, 0, 0, 0, 0};
	Toggle->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FVector(0, 500, 0)));
	Toggle->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 4);
	TestTrue(TEXT("toggle latches fully open"), Toggle->bIsFullyOpen);
	Toggle->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 4);
	TestFalse(TEXT("second toggle closes and clears the latch"), Toggle->bIsFullyOpen);

	AWoTGradualMover* Pound = World->SpawnActorDeferred<AWoTGradualMover>(
		AWoTGradualMover::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 800, 0)));
	if (!TestNotNull(TEXT("pound mover"), Pound))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pound->InitialState = EWoTMoverState::GradualTriggerPound;
	Pound->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Pound->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Pound->OpenTimes = {1.0f, 0, 0, 0, 0, 0};
	Pound->CloseTimes = {1.0f, 0, 0, 0, 0, 0};
	Pound->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FVector(0, 800, 0)));
	Pound->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 15);
	const float BeforeReverseX = Pound->GetActorLocation().X;
	Pound->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 1);
	TestEqual(TEXT("pound UnTrigger reverses into the closing phase"),
		Pound->GetGradualPhase(), EWoTGradualPhase::Closing);
	TestTrue(TEXT("pound reversal preserves alpha and moves back from the current pose"),
		Pound->GetActorLocation().X < BeforeReverseX &&
		Pound->GetActorLocation().X > BeforeReverseX - 5.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAttachMoverSpec,
	"WoT.AttachMover.AttachesAndInertsSlaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAttachMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAttachMover* Attach = World ? World->SpawnActorDeferred<AWoTAttachMover>(
		AWoTAttachMover::StaticClass(), FTransform::Identity) : nullptr;
	AWoTMover* Slave = DeferredGroupMover(World, FVector(0, 200, 0), TEXT("Passenger"), true);
	AWoTTrigger* Passenger = World ? World->SpawnActorDeferred<AWoTTrigger>(
		AWoTTrigger::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 300, 0))) : nullptr;
	if (!TestNotNull(TEXT("attach mover"), Attach) ||
		!TestNotNull(TEXT("slave"), Slave) ||
		!TestNotNull(TEXT("non-mover passenger"), Passenger))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Attach->AttachTag = TEXT("Passenger");
	FinishDeferredMover(Slave, FVector(0, 200, 0));
	Passenger->TagName = TEXT("Passenger");
	Passenger->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FVector(0, 300, 0)));
	Attach->FinishSpawning(FTransform::Identity);
	TestEqual(TEXT("AttachMover attaches tagged slave mover"),
		Slave->GetAttachParentActor(), static_cast<AActor*>(Attach));
	TestTrue(TEXT("AttachMover makes attached slave inert"), Slave->IsMoverFSMInert());
	TestEqual(TEXT("AttachMover attaches a non-mover by its UE1 Tag"),
		Passenger->GetAttachParentActor(), static_cast<AActor*>(Attach));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMixMoverSpec,
	"WoT.MixMover.KeyframeAndAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMixMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMixMover* Mix = World ? World->SpawnActorDeferred<AWoTMixMover>(
		AWoTMixMover::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 500, 0))) : nullptr;
	AWoTMover* Slave = DeferredGroupMover(
		World, FVector(0, 700, 0), TEXT("MixPassenger"), true);
	if (!TestNotNull(TEXT("mix mover"), Mix) ||
		!TestNotNull(TEXT("mix slave"), Slave))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mix->AttachTag = TEXT("MixPassenger");
	Mix->TagName = TEXT("ExternalLift");
	Mix->EventName = TEXT("ExternalLiftEvent");
	Mix->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Mix->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mix->OpenTimes = {0.1f, 0, 0, 0, 0, 0};
	Mix->CloseTimes = {0.1f, 0, 0, 0, 0, 0};
	FinishDeferredMover(Slave, FVector(0, 700, 0));
	Mix->FinishSpawning(FTransform(FRotator::ZeroRotator, FVector(0, 500, 0)));
	TestEqual(TEXT("MixMover attaches tagged slave mover"),
		Slave->GetAttachParentActor(), static_cast<AActor*>(Mix));
	TestTrue(TEXT("MixMover makes attached slave inert"), Slave->IsMoverFSMInert());
	TestEqual(TEXT("default/base-state MixMover preserves its authored tag"),
		Mix->TagName, FName(TEXT("ExternalLift")));
	Mix->MoveKeyframe(1);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("MixMover MoveKeyframe reaches its target"), Mix->GetCurrentKey(), 1);
	TestTrue(TEXT("MixMover re-arms its keyframe gate"), Mix->CanMoveKey());

	AWoTMixMover* Sentinel = World->SpawnActorDeferred<AWoTMixMover>(
		AWoTMixMover::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 1100, 0)));
	if (!TestNotNull(TEXT("sentinel mix mover"), Sentinel))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Sentinel->InitialState = EWoTMoverState::GradualTriggerOpenTimed;
	Sentinel->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Sentinel->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Sentinel->OpenTimes = {0.05f, 0.05f, 0, 0, 0, 0};
	Sentinel->CloseTimes = {0.05f, 0.05f, 0, 0, 0, 0};
	Sentinel->StayOpenTime = 1.0f;
	Sentinel->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FVector(0, 1100, 0)));
	Sentinel->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("MixMover full-open key is the NumKeys sentinel"),
		Sentinel->GetCurrentKey(), 2);
	TestTrue(TEXT("sentinel zero slot returns the fully-open pose to base"),
		Sentinel->GetActorLocation().Equals(FVector(0, 1100, 0), 0.1f));

	AWoTMixMover* Pound = World->SpawnActorDeferred<AWoTMixMover>(
		AWoTMixMover::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(0, 900, 0)));
	if (!TestNotNull(TEXT("pound mix mover"), Pound))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pound->InitialState = EWoTMoverState::GradualTriggerPound;
	Pound->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Pound->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Pound->OpenTimes = {0.05f, 0.0f, 0, 0, 0, 0};
	Pound->CloseTimes = {0.05f, 0.0f, 0, 0, 0, 0};
	Pound->StayOpenTime = 0.02f;
	Pound->bTriggerOnceOnly = true;
	Pound->MoveAmbientSoundPath = TEXT("Spec/MixPoundLoop");
	Pound->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FVector(0, 900, 0)));
	Pound->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 15);
	TestEqual(TEXT("MixMover Pound parks after its close dwell"),
		Pound->GetGradualPhase(), EWoTGradualPhase::Spent);
	TestTrue(TEXT("shipped MixMover Pound keeps ambient while parked"),
		Pound->bMoveLoopOn);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
