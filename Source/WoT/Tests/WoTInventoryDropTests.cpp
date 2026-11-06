// Class automation specs for the G05/G06/G07 gameplay-parity wave: the exact
// Engine.Inventory drop arc (AActor::physFalling non-pawn path + Pickup.Landed),
// the ValidTouch player gate, the giMPBattle teammate targeting filter, and the
// AngrealSpawnPool respawn-delay scaling.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTBattleGameMode.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerState.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	UWorld* CreateDropSpecWorld(bool bBattle = false)
	{
		return bBattle
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();
	}

	AWoTCharacter* SpawnDropSpecPlayer(UWorld* World, const FVector& Location,
		uint8 Team)
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->GetPlayerState<AWoTPlayerState>()->Team = Team;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		PC->Possess(Pawn);
		return Pawn;
	}

	FWoTInventoryFallState MakeFallState(const FVector& Velocity)
	{
		FWoTInventoryFallState State;
		State.Velocity = Velocity;
		return State;
	}

	// Default zone: no water, engine gravity, no current.
	void DefaultZone(const FVector&, FWoTInventoryFallZone& Zone)
	{
		Zone = FWoTInventoryFallZone();
	}

	bool NoHitTrace(const FVector&, const FVector&, FHitResult&)
	{
		return false;
	}
}

// UE1's physFalling non-pawn arithmetic, pinned exactly: the
// average-velocity move, the 2*avg-old end-velocity refinement, the sub-tick
// split, the apex split, and the terminal clamp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryFallIntegrationSpec,
	"WoT.Inventory.Drop.FallIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryFallIntegrationSpec::RunTest(const FString&)
{
	// One 0.1s tick from rest under (0,0,-950): move at the average velocity
	// -47.5, end at the refined 2*avg - old = -95 (exact free-fall).
	FWoTInventoryFallState State = MakeFallState(FVector::ZeroVector);
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, DefaultZone, NoHitTrace);
	TestEqual(TEXT("0.1s displacement is avg-velocity"), float(State.Location.Z), -4.75f, 0.01f);
	TestEqual(TEXT("0.1s end velocity is refined"), float(State.Velocity.Z), -95.0f, 0.01f);

	// A 0.5s frame splits into 0.1 sub-ticks (timeTick = min(0.1, r*0.5) while
	// r > 0.1): five exact ticks — v -475, z -118.75 (== 0.5*g*t^2).
	State = MakeFallState(FVector::ZeroVector);
	AWoTInventory::StepFalling(State, 0.5f, 100.0f, 0.0f, DefaultZone, NoHitTrace);
	TestEqual(TEXT("0.5s velocity"), float(State.Velocity.Z), -475.0f, 0.05f);
	TestEqual(TEXT("0.5s displacement"), float(State.Location.Z), -118.75f, 0.05f);

	// Terminal velocity: the refined end velocity clamps to the UE1
	// ZoneInfo default 2500 (no shipped WoT map authors it).
	State = MakeFallState(FVector(0, 0, -2490.0f));
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, DefaultZone, NoHitTrace);
	TestEqual(TEXT("terminal clamp"), float(State.Velocity.Z), -2500.0f, 0.01f);

	// Apex split: +30 up flips within the tick;
	// part = 30/47.5 splits the tick, the remainder integrates down.
	State = MakeFallState(FVector(0, 0, 30.0f));
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, DefaultZone, NoHitTrace);
	TestEqual(TEXT("apex fraction recorded"), State.ApexFraction, 30.0f / 47.5f, 0.001f);
	TestEqual(TEXT("post-apex velocity"), float(State.Velocity.Z), -65.0f, 0.05f);
	TestEqual(TEXT("post-apex displacement"), float(State.Location.Z), -1.75f, 0.05f);
	return true;
}

// physFalling's water branch: fluid friction damps the old velocity,
// buoyancy scales gravity; the 2*avg-old doubling only applies while still
// accelerating downward (V.Z < OldV.Z) or after upward motion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryFallWaterSpec,
	"WoT.Inventory.Drop.FallWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryFallWaterSpec::RunTest(const FString&)
{
	auto WaterZone = [](const FVector&, FWoTInventoryFallZone& Zone)
	{
		Zone = FWoTInventoryFallZone();
		Zone.bWater = true;   // FluidFriction keeps the 1.2 LevelInfo default
	};

	// Sinking: V0=-100 → avg -123.5, refined -147; displacement -12.35.
	FWoTInventoryFallState State = MakeFallState(FVector(0, 0, -100.0f));
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, WaterZone, NoHitTrace);
	TestEqual(TEXT("water sink velocity"), float(State.Velocity.Z), -147.0f, 0.05f);
	TestEqual(TEXT("water sink displacement"), float(State.Location.Z), -12.35f, 0.05f);

	// Neutral buoyancy (Buoyancy == Mass): gravity term zero, friction only —
	// avg -76; -76 is NOT < -100 and OldV.Z < 0, so the refine doubling is
	// skipped and the end velocity IS the damped average.
	State = MakeFallState(FVector(0, 0, -100.0f));
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 100.0f, WaterZone, NoHitTrace);
	TestEqual(TEXT("buoyant water velocity undoubles"), float(State.Velocity.Z), -76.0f, 0.05f);
	return true;
}

// physFalling's hit handling: a floor hit (Normal.Z > 0.7) lands with the
// back-derived contact velocity; a wall hit slides the remaining delta along
// the wall and refines velocity from actual displacement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryFallHitSpec,
	"WoT.Inventory.Drop.FallLandingAndSlide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryFallHitSpec::RunTest(const FString&)
{
	// Floor at half the move: land, velocity = (loc-old)/(t*HitTime) = -47.5.
	auto FloorTrace = [](const FVector& Start, const FVector& End, FHitResult& Hit)
	{
		if (End.Z < Start.Z)
		{
			Hit.Time = 0.5f;
			Hit.Normal = FVector::UpVector;
			return true;
		}
		return false;
	};
	FWoTInventoryFallState State = MakeFallState(FVector::ZeroVector);
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, DefaultZone, FloorTrace);
	TestTrue(TEXT("floor hit lands"), State.bLanded);
	TestFalse(TEXT("landing stops the fall"), State.bFalling);
	TestEqual(TEXT("landing normal"), float(State.LandedNormal.Z), 1.0f, 0.001f);
	TestEqual(TEXT("landing back-derived velocity"), float(State.Velocity.Z), -47.5f, 0.01f);
	TestEqual(TEXT("landing stop position"), float(State.Location.Z), -2.375f, 0.01f);

	// Vertical wall at half the move: slide the projected remainder, keep
	// falling; end velocity refines from actual displacement (-50, 0, -195).
	int32 Hits = 0;
	auto WallTrace = [&Hits](const FVector&, const FVector&, FHitResult& Hit)
	{
		if (Hits++ == 0)
		{
			Hit.Time = 0.5f;
			Hit.Normal = FVector(1, 0, 0);
			return true;
		}
		return false;
	};
	State = MakeFallState(FVector(-100.0f, 0, -100.0f));
	AWoTInventory::StepFalling(State, 0.1f, 100.0f, 0.0f, DefaultZone, WallTrace);
	TestFalse(TEXT("a wall hit does not land"), State.bLanded);
	TestTrue(TEXT("still falling after the slide"), State.bFalling);
	TestEqual(TEXT("slide X stops at the wall"), float(State.Location.X), -5.0f, 0.01f);
	TestEqual(TEXT("slide Z keeps the projected delta"), float(State.Location.Z), -14.75f, 0.01f);
	TestEqual(TEXT("post-slide velocity X"), float(State.Velocity.X), -50.0f, 0.05f);
	TestEqual(TEXT("post-slide velocity Z"), float(State.Velocity.Z), -195.0f, 0.05f);
	return true;
}

// DropFrom (shipped Engine.u bytecode: bCollideWorld/bHeldItem set first, then
// PHYS_Falling + BecomePickup + DeleteInventory + Pickup@Dropped) and
// Inventory.Pickup.Landed (flatten pitch only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropLifecycleSpec,
	"WoT.Inventory.Drop.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryDropLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = CreateDropSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvFireball* Item = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInventory* Held = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("pickup held"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Held->DropFrom(FVector(50, 0, 100));
	TestEqual(TEXT("dropped item re-enters Pickup"),
		Held->GetItemState(), EWoTInventoryState::Pickup);
	TestFalse(TEXT("dropped item clears bHeldItem (shipped NEW line)"), Held->bHeldItem);
	TestEqual(TEXT("dropped item never respawns"), Held->RespawnTime, 0.0f);
	TestTrue(TEXT("dropped item is falling"), Held->IsFalling());
	TestFalse(TEXT("drop removed it from the inventory"),
		Inv->Items.Contains(Held));
	TestTrue(TEXT("a dropped item replicates movement (UE1 proxy staging)"),
		Held->IsReplicatingMovement());

	// No floor in the spec world: gravity pulls it down through ticked frames.
	// A dropped item does NOT spin while falling (Pickup@Dropped bypasses the
	// Begin-label PHYS_Rotating block).
	const float StartZ = Held->GetActorLocation().Z;
	const float StartYaw = float(Held->GetActorRotation().Yaw);
	WoTTest::TickWorld(World, 0.25f);
	TestTrue(TEXT("dropped item falls under zone gravity"),
		Held->GetActorLocation().Z < StartZ - 10.0f);
	TestEqual(TEXT("no pickup spin while dropped"),
		float(Held->GetActorRotation().Yaw), StartYaw, 0.01f);

	// Landed flattens pitch only (Engine.Inventory) and ends the fall
	// at rest — no PHYS_Rotating resume for a dropped item.
	Held->SetActorRotation(FRotator(30.0f, 40.0f, 10.0f));
	Held->Landed(FVector::UpVector);
	TestEqual(TEXT("landing flattens pitch"), float(Held->GetActorRotation().Pitch), 0.0f, 0.1f);
	TestEqual(TEXT("landing keeps yaw"), float(Held->GetActorRotation().Yaw), 40.0f, 0.1f);
	TestFalse(TEXT("landing ends the fall"), Held->IsFalling());
	const float RestYaw = float(Held->GetActorRotation().Yaw);
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("no spin at rest after landing"),
		float(Held->GetActorRotation().Yaw), RestYaw, 0.01f);

	// Collected again: the dropped lifecycle ends with the pickup.
	TestTrue(TEXT("dropped item can be re-collected"), Held->TryPickupBy(Pawn));
	TestFalse(TEXT("re-pickup ends drop-scoped movement replication"),
		Held->IsReplicatingMovement());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Engine.Inventory's bIsPlayer gate: shipped NPCs never set bIsPlayer, so an
// NPC pawn (which does carry an inventory component for its caster loadout)
// must not consume a world pickup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryNPCPickupGateSpec,
	"WoT.Inventory.Drop.NPCCannotPickup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryNPCPickupGateSpec::RunTest(const FString&)
{
	UWorld* World = CreateDropSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTAngrealInvFireball* Item = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator, Params);
	WoTTest::TickWorld(World, 0.05f);

	TestFalse(TEXT("an NPC pawn cannot pick up (bIsPlayer gate)"),
		Item->TryPickupBy(NPC));
	TestEqual(TEXT("the pickup stays in the world"),
		Item->GetItemState(), EWoTInventoryState::Pickup);

	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(600, 0, 100), FRotator::ZeroRotator, Params);
	TestTrue(TEXT("a player pawn still picks up"), Item->TryPickupBy(Player));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// AngrealInventory.uc:814-830: in the citadel game a !bTargetsFriendlies
// artifact must not target the owner's teammates (illusion redirect pends G31).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryTeammateFilterSpec,
	"WoT.Inventory.Battle.TeammateTargetFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTInventoryTeammateFilterSpec::RunTest(const FString&)
{
	UWorld* World = CreateDropSpecWorld(/*bBattle=*/true);
	AWoTCharacter* Caster = SpawnDropSpecPlayer(World, FVector(0, 0, 100), 2);
	AWoTCharacter* Friendly = SpawnDropSpecPlayer(World, FVector(400, 0, 100), 2);
	AWoTCharacter* Enemy = SpawnDropSpecPlayer(World, FVector(800, 0, 100), 3);
	AWoTCharacter* NoPRI = World->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 100), FRotator::ZeroRotator);

	// Seeker ships bTargetsFriendlies=False (AngrealInvSeeker.uc:40).
	AWoTAngrealInvSeeker* Seeker = World->SpawnActor<AWoTAngrealInvSeeker>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Seeker->GiveTo(Caster);
	TestTrue(TEXT("a teammate is not targetable"), Seeker->IsNotTargetable(Friendly));
	TestFalse(TEXT("an enemy is targetable"), Seeker->IsNotTargetable(Enemy));
	TestFalse(TEXT("a PRI-less pawn skips the filter (retail guard)"),
		Seeker->IsNotTargetable(NoPRI));

	// Retail also guards the OWNER's PRI: a PRI-less owner never filters.
	// Spawned clear of every pawn — a spawn-overlap with the seeker-holding
	// Caster would run the duplicate-absorb path and DESTROY this actor.
	AWoTAngrealInvSeeker* OrphanSeeker = World->SpawnActor<AWoTAngrealInvSeeker>(
		FVector(2400, 0, 100), FRotator::ZeroRotator);
	OrphanSeeker->GiveTo(NoPRI);
	TestFalse(TEXT("a PRI-less owner skips the filter (retail guard)"),
		OrphanSeeker->IsNotTargetable(Friendly));

	// Neutral 255 == 255 is a raw byte match — retail filters it too.
	Caster->GetPlayerState<AWoTPlayerState>()->Team = 255;
	Friendly->GetPlayerState<AWoTPlayerState>()->Team = 255;
	TestTrue(TEXT("two neutral pawns filter as same-team"),
		Seeker->IsNotTargetable(Friendly));

	// bTargetsFriendlies=True (base default) never filters.
	Caster->GetPlayerState<AWoTPlayerState>()->Team = 2;
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Fireball->GiveTo(Caster);
	AWoTCharacter* Friendly2 = SpawnDropSpecPlayer(World, FVector(1600, 0, 100), 2);
	TestFalse(TEXT("bTargetsFriendlies artifacts keep teammates targetable"),
		Fireball->IsNotTargetable(Friendly2));
	WoTTest::DestroySpecWorld(World);

	// Outside giMPBattle the filter never applies (Level.Game.IsA gate).
	UWorld* ArenaWorld = CreateDropSpecWorld();
	AWoTCharacter* ArenaCaster = SpawnDropSpecPlayer(ArenaWorld, FVector(0, 0, 100), 2);
	AWoTCharacter* ArenaMate = SpawnDropSpecPlayer(ArenaWorld, FVector(400, 0, 100), 2);
	AWoTAngrealInvSeeker* ArenaSeeker = ArenaWorld->SpawnActor<AWoTAngrealInvSeeker>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	ArenaSeeker->GiveTo(ArenaCaster);
	TestFalse(TEXT("the filter is Battle-only"),
		ArenaSeeker->IsNotTargetable(ArenaMate));
	WoTTest::DestroySpecWorld(ArenaWorld);
	return true;
}

// AngrealSpawnPool.uc:151: WaitingDelay /= Max(1, NumPlayers / Max(Ideal, 1))
// with INTEGER division, then the 0.3s settle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnPoolDelayScalingSpec,
	"WoT.Inventory.SpawnPool.RespawnDelayScaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpawnPoolDelayScalingSpec::RunTest(const FString&)
{
	TestEqual(TEXT("full pool halves at 2x ideal"),
		AWoTAngrealSpawnPool::ComputeRespawnDelay(80.0f, 4, 8), 40.3f, 0.001f);
	TestEqual(TEXT("below ideal never scales (int division floors to 0 -> Max 1)"),
		AWoTAngrealSpawnPool::ComputeRespawnDelay(80.0f, 4, 3), 80.3f, 0.001f);
	TestEqual(TEXT("integer division floors 5/3 to 1"),
		AWoTAngrealSpawnPool::ComputeRespawnDelay(80.0f, 3, 5), 80.3f, 0.001f);
	TestEqual(TEXT("unauthored ideal (0) divides by player count"),
		AWoTAngrealSpawnPool::ComputeRespawnDelay(80.0f, 0, 6), 80.0f / 6.0f + 0.3f, 0.001f);
	TestEqual(TEXT("zero base keeps only the settle"),
		AWoTAngrealSpawnPool::ComputeRespawnDelay(0.0f, 4, 8), 0.3f, 0.001f);
	return true;
}

#endif   // WITH_AUTOMATION_TESTS
