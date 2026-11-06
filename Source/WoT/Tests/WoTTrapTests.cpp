// Automation specs for the WoTTraps port — the AWoTTrap base. The runtime trap-actor
// spine every trap inherits: team gate (IsInactive), collision Hide/Show, destroyed-event routing,
// and multi-actor identity. Single-process, no LAN, no map.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestTrap.h"
#include "../Traps/WoTTrap.h"
#include "../Traps/WoTTrapBlocker.h"
#include "../Traps/WoTTrapBurst.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTWall.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTAlarm.h"
#include "WoTTestAIClasses.h"   // AWoTTestNPCCharacter (forced-nav test brain) for the fan-out case
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../WoTMover.h"
#include "../WoTCharacter.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTSeal.h"
#include "../WoTCharacterMovement.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTEvents.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTParticleSprayer.h"
#include "../WoTCollisionBlocker.h"

#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "Engine/DamageEvents.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

namespace
{
	bool TrapSpecMaterialParentChainContains(const UMaterialInterface* Material,
		const UMaterialInterface* ExpectedParent)
	{
		const UMaterialInterface* Current = Material;
		while (IsValid(Current))
		{
			if (Current == ExpectedParent)
			{
				return true;
			}
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
			Current = Instance ? Instance->Parent : nullptr;
		}
		return false;
	}

	// Minimal ticking Standalone world with the project game mode (so InitPlayerState creates the
	// AWoTPlayerState) — supplied by the shared standalone fixture.
	FActorSpawnParameters TrapAlwaysSpawnParams()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	// Fixed 1/60 steps (UWorld::Tick clamps a coarse delta to 0.4s, so a lifespan can't be advanced
	// in one big tick — the repo's known gotcha). GFrameCounter MUST advance per step: FTimerManager
	// early-outs if it already ticked "this frame", so without it every step reads as one frame and
	// the lifespan timer never progresses.
}

// The team/editing gate + collision Hide/Show + identity helpers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapBaseSpec,
	"WoT.Trap.Base", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapBaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestTrap* Trap = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	if (!Trap)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Trap->Team = 5;

	// A possessed pawn carrying a team on its PlayerState (the IsInactive team source).
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->InitPlayerState();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	PC->Possess(Pawn);
	AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>();
	if (!PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// --- IsInactive team gate ---
	PS->Team = 5;
	TestTrue(TEXT("inactive for a same-team pawn"), Trap->IsInactive(Pawn));
	PS->Team = 6;
	TestFalse(TEXT("active for an enemy-team pawn"), Trap->IsInactive(Pawn));
	Trap->bEditing = true;
	TestTrue(TEXT("inactive while editing (never fires mid-placement)"), Trap->IsInactive(Pawn));
	Trap->bEditing = false;
	TestFalse(TEXT("a non-pawn actor is not a trigger"), Trap->IsInactive(Trap));

	// --- NPC team gate: retail NPCs mirror AssignedTeam into their PRI (WOTPawn.uc:429), so
	// Trap.uc's PlayerReplicationInfo.Team read gates them too. Without that path
	// an NPC with no PlayerState reads team 255 and springs its own citadel's traps.
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	if (TestNotNull(TEXT("NPC spawned"), NPC))
	{
		NPC->AssignedTeam = 5;
		TestTrue(TEXT("inactive for a same-team NPC (own troops don't spring it)"),
			Trap->IsInactive(NPC));
		NPC->AssignedTeam = 6;
		TestFalse(TEXT("active for an enemy-team NPC"), Trap->IsInactive(NPC));
	}

	// --- Hide/Show couple visibility with collision ---
	Trap->HideTrap();
	TestFalse(TEXT("hidden trap has no collision"), Trap->GetActorEnableCollision());
	TestTrue(TEXT("hidden trap is hidden"), Trap->IsHidden());
	Trap->ShowTrap();
	TestTrue(TEXT("shown trap collides again"), Trap->GetActorEnableCollision());
	TestFalse(TEXT("shown trap is visible"), Trap->IsHidden());

	// --- SameLogicalActor identity (self / owned-by / shared base) ---
	TestTrue(TEXT("identical actor is the same logical trap"), Trap->SameLogicalActor(Trap));

	AWoTTestTrap* Owned = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	Owned->SetOwner(Trap);
	TestTrue(TEXT("an owned actor is the same logical trap"), Trap->SameLogicalActor(Owned));

	// Two slabs sharing a manager (GetBaseResource → Owner) are the same logical trap.
	AWoTTestTrap* Manager = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	AWoTTestTrapSlab* SlabA = World->SpawnActor<AWoTTestTrapSlab>(TrapAlwaysSpawnParams());
	AWoTTestTrapSlab* SlabB = World->SpawnActor<AWoTTestTrapSlab>(TrapAlwaysSpawnParams());
	SlabA->SetOwner(Manager);
	SlabB->SetOwner(Manager);
	TestTrue(TEXT("two slabs of one manager share a base resource"), SlabA->SameLogicalActor(SlabB));

	AWoTTestTrap* Unrelated = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	TestFalse(TEXT("an unrelated trap is a different logical trap"), Trap->SameLogicalActor(Unrelated));

	// --- DamageActor virtual dispatch through the base pointer ---
	AWoTTrap* AsBase = Trap;
	AsBase->DamageActor(Pawn);
	TestEqual(TEXT("DamageActor dispatches to the subclass"), Trap->DamageActorCalls, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// DestroyedTrigger fans DestroyedEvent out through UWoTEventSubsystem — authority only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapDestroyedEventSpec,
	"WoT.Trap.DestroyedEvent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapDestroyedEventSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A sink registered under "TrapGone" (deferred so its TagName is set before BeginPlay registers).
	AWoTTestTrapEventSink* Sink = World->SpawnActorDeferred<AWoTTestTrapEventSink>(
		AWoTTestTrapEventSink::StaticClass(), FTransform::Identity);
	Sink->TagName = TEXT("TrapGone");
	Sink->FinishSpawning(FTransform::Identity);

	AWoTTestTrap* Trap = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	Trap->DestroyedEvent = TEXT("TrapGone");

	// Authority: firing reaches the registered sink.
	Trap->DestroyedTrigger(nullptr, nullptr);
	TestEqual(TEXT("destroyed event reaches the tagged sink"), Sink->TriggerCount, 1);

	// A proxy trap fires nothing (world-state change is authority-only).
	Trap->SetRole(ROLE_SimulatedProxy);
	Trap->DestroyedTrigger(nullptr, nullptr);
	TestEqual(TEXT("a proxy trap does not fire the destroyed event"), Sink->TriggerCount, 1);
	Trap->SetRole(ROLE_Authority);

	// An unset DestroyedEvent fires nothing.
	AWoTTestTrap* Silent = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	Silent->DestroyedTrigger(nullptr, nullptr);
	TestEqual(TEXT("an unset destroyed event fires nothing"), Sink->TriggerCount, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// TrapBlocker forwards its overlaps to the owning trap: damaging → DamageActor, else → TrapTouch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapBlockerSpec,
	"WoT.Trap.Blocker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapBlockerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestTrap* Owner = World->SpawnActor<AWoTTestTrap>(TrapAlwaysSpawnParams());
	AWoTTrapBlocker* Blocker = World->SpawnActor<AWoTTrapBlocker>(TrapAlwaysSpawnParams());
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	if (!Owner || !Blocker || !Victim)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Blocker->SetOwner(Owner);

	// Non-damaging blocker relays the touch (the spear's trigger), no damage.
	Blocker->SetBlocking(/*bCollide*/true, /*bBlock*/false, /*bDamage*/false, 8.0f, 8.0f);
	Blocker->OnBlockerOverlap(nullptr, Victim, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("non-damaging blocker relays TrapTouch"), Owner->TrapTouchCalls, 1);
	TestEqual(TEXT("non-damaging blocker deals no damage"), Owner->DamageActorCalls, 0);

	// Damaging blocker calls the owner's DamageActor (pass -1 to skip the resize).
	Blocker->SetBlocking(/*bCollide*/true, /*bBlock*/false, /*bDamage*/true, -1.0f, -1.0f);
	Blocker->OnBlockerOverlap(nullptr, Victim, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("damaging blocker calls DamageActor"), Owner->DamageActorCalls, 1);
	TestEqual(TEXT("damaging blocker does not relay a second touch"), Owner->TrapTouchCalls, 1);

	// An owner-less blocker is inert (no crash).
	AWoTTrapBlocker* Orphan = World->SpawnActor<AWoTTrapBlocker>(TrapAlwaysSpawnParams());
	Orphan->SetBlocking(true, false, true, 4.0f, 4.0f);
	Orphan->OnBlockerOverlap(nullptr, Victim, nullptr, 0, false, FHitResult());
	TestTrue(TEXT("an owner-less blocker overlap is inert"), true);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Staircase is a mover TRIGGER: an armed (Active) staircase fires its Mover on an enemy touch (and
// re-arms after the dwell); it does nothing inactive, on a teammate, or as a proxy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTStaircaseSpec,
	"WoT.Trap.Staircase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTStaircaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Fresh {door, staircase, pawn} per case (a sprung door stays open through the dwell, so cases
	// must not share one). Returns whether the staircase sprang its tagged mover (+Z rise).
	int32 TagN = 0;
	auto StairSprings = [&](uint8 StairTeam, uint8 PawnTeam, bool bActive, bool bProxy) -> bool
	{
		const FName Tag(*FString::Printf(TEXT("SC%d"), TagN++));

		// A door tagged `Tag` that rises +Z100 on trigger (deferred so KeyPos/TagName precede BeginPlay).
		const FTransform X(FRotator::ZeroRotator, FVector(0, 0, 100));
		AWoTMover* Door = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), X);
		Door->InitialState = EWoTMoverState::TriggerOpenTimed;
		Door->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
		Door->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
		Door->MoveTime = 0.3f;
		Door->TagName = Tag;
		Door->FinishSpawning(X);
		const double BaseZ = Door->GetActorLocation().Z;

		// The staircase trigger linked to that door (MoverTag set before BeginPlay resolves it).
		AWoTStaircase* Stair = World->SpawnActorDeferred<AWoTStaircase>(
			AWoTStaircase::StaticClass(), FTransform::Identity);
		Stair->MoverTag = Tag;
		Stair->Team = StairTeam;
		Stair->FinishSpawning(FTransform::Identity);
		if (bProxy) { Stair->SetRole(ROLE_SimulatedProxy); }
		if (bActive) { Stair->Activate(); }

		// A pawn carrying PawnTeam on its PlayerState.
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 300), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = PawnTeam; }

		// Touch from inside the staircase's UE1 cylinder: team/state gates decide.
		WoTTest::PlaceWithoutOverlapEvents(Pawn, Stair->GetActorLocation());
		Stair->OnStaircaseOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
		WoTTest::TickWorld(World, 30);   // 0.5s > MoveTime
		return Door->GetActorLocation().Z > BaseZ + 50.0;
	};

	TestTrue(TEXT("an active staircase springs its stairs on an enemy touch"),
		StairSprings(/*StairTeam*/1, /*PawnTeam*/2, /*bActive*/true, /*bProxy*/false));
	TestFalse(TEXT("a staircase does not spring for a same-team pawn"),
		StairSprings(1, 1, true, false));
	TestFalse(TEXT("an inactive staircase does not spring"),
		StairSprings(1, 2, false, false));
	TestFalse(TEXT("a proxy staircase does not spring (authority-gated)"),
		StairSprings(1, 2, true, true));

	// IsTrapDeployed tracks the Active state.
	AWoTStaircase* S = World->SpawnActor<AWoTStaircase>(TrapAlwaysSpawnParams());
	TestFalse(TEXT("staircase starts not deployed (Inactive)"), S->IsTrapDeployed());
	S->Activate();
	TestTrue(TEXT("staircase deployed once Active"), S->IsTrapDeployed());
	S->Deactivate();
	TestFalse(TEXT("staircase not deployed after Deactivate"), S->IsTrapDeployed());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapCursorCylinderSpec,
	"WoT.Trap.CursorCylinder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapCursorCylinderSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params = TrapAlwaysSpawnParams();
	AWoTPortcullis* Portcullis = World->SpawnActor<AWoTPortcullis>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	AWoTStaircase* Staircase = World->SpawnActor<AWoTStaircase>(
		FVector(1000.0f, 0.0f, 0.0f), FRotator::ZeroRotator, Params);
	Portcullis->Activate();
	Staircase->Activate();
	Portcullis->BeginEditingResource(1);
	Staircase->BeginEditingResource(1);

	auto TraceShape = [this, World](AActor* Trap,
		UWoTFlatCylinderComponent* Flat, float Radius, float HalfHeight,
		const TCHAR* Label)
	{
		const FVector Center = Trap->GetActorLocation();
		FCollisionQueryParams Query(SCENE_QUERY_STAT(TrapCursorCylinder), true);
		FHitResult Hit;
		const float OutsideCylinderZ = HalfHeight + Radius * 0.5f;
		const bool bBroadEmptyHit = World->LineTraceSingleByChannel(Hit,
			Center + FVector(-Radius * 2.0f, 0.0f, OutsideCylinderZ),
			Center + FVector( Radius * 2.0f, 0.0f, OutsideCylinderZ),
			ECC_Visibility, Query);
		TestFalse(FString::Printf(TEXT("%s visibility passes inside the broad capsule but above the retail cylinder"), Label),
			bBroadEmptyHit && Hit.GetActor() == Trap);

		Hit = FHitResult();
		const bool bCylinderHit = World->LineTraceSingleByChannel(Hit,
			Center + FVector(-Radius * 2.0f, 0.0f, 0.0f),
			Center + FVector( Radius * 2.0f, 0.0f, 0.0f),
			ECC_Visibility, Query);
		TestTrue(FString::Printf(TEXT("%s visibility hits the retail flat cylinder"), Label),
			bCylinderHit && Hit.GetActor() == Trap);

		TestNotNull(FString::Printf(TEXT("%s exact cursor cylinder exists"), Label), Flat);
		if (!Flat)
		{
			return;
		}
		const double Diagonal = Radius / FMath::Sqrt(2.0);
		FHitResult ExactHit;
		TestTrue(FString::Printf(TEXT("%s diagonal ray just inside true radius hits"), Label),
			Flat->ExactLineTrace(
				Center + FVector(Diagonal * 0.999, Diagonal * 0.999, HalfHeight * 2.0),
				Center + FVector(Diagonal * 0.999, Diagonal * 0.999, -HalfHeight * 2.0),
				ExactHit));
		ExactHit = FHitResult();
		TestFalse(FString::Printf(TEXT("%s diagonal ray just outside true radius misses"), Label),
			Flat->ExactLineTrace(
				Center + FVector(Diagonal * 1.001, Diagonal * 1.001, HalfHeight * 2.0),
				Center + FVector(Diagonal * 1.001, Diagonal * 1.001, -HalfHeight * 2.0),
				ExactHit));
	};

	TraceShape(Portcullis, Portcullis->GetCursorTraceCylinder(),
		Portcullis->GetUE1TriggerRadius(),
		Portcullis->GetUE1TriggerHalfHeight(), TEXT("Portcullis"));
	TraceShape(Staircase, Staircase->GetCursorTraceCylinder(),
		Staircase->GetUE1TriggerRadius(),
		Staircase->GetUE1TriggerHalfHeight(), TEXT("Staircase"));

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// WallSlab: authority damage decrements the replicated WithstandDamage, the stage derives from it
// (the port's replacement for the un-replicated native Skin), and it destroys at zero.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWallSlabSpec,
	"WoT.Trap.WallSlab", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWallSlabSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// WithstandDamage must replicate (all-clients) — the stage is derived from it, so this is what
	// carries the damage feedback to clients. Assert CPF_Net via reflection (the safe check; do NOT
	// re-walk GetLifetimeReplicatedProps on a CDO — that trips the push-model assertion).
	const FProperty* WD = FindFProperty<FProperty>(AWoTWallSlab::StaticClass(), TEXT("WithstandDamage"));
	TestTrue(TEXT("WithstandDamage replicates"), WD && WD->HasAnyPropertyFlags(CPF_Net));

	auto DamageSlab = [&](AWoTWallSlab* Slab, float Amount)
	{
		FHitResult Hit;
		Hit.ImpactPoint = Slab->GetActorLocation();
		const FPointDamageEvent Ev(Amount, Hit, FVector::ForwardVector, nullptr);
		Slab->TakeDamage(Amount, Ev, nullptr, nullptr);
	};

	// Full → stage 0; damage across the 100/50 thresholds → stages 1, 2; past 0 → destroyed.
	AWoTWallSlab* Slab = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	TestEqual(TEXT("full slab is stage 0"), Slab->GetStage(), 0);
	DamageSlab(Slab, 60.0f);   // 150 → 90
	TestEqual(TEXT("WithstandDamage decremented"), Slab->WithstandDamage, 90);
	TestEqual(TEXT("stage 1 below 2/3"), Slab->GetStage(), 1);
	DamageSlab(Slab, 50.0f);   // 90 → 40
	TestEqual(TEXT("stage 2 below 1/3"), Slab->GetStage(), 2);
	DamageSlab(Slab, 50.0f);   // 40 → -10 → destroyed
	TestFalse(TEXT("slab destroyed at zero withstand"), IsValid(Slab));

	// Authority gate: a proxy slab takes no damage.
	AWoTWallSlab* Proxy = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	Proxy->SetRole(ROLE_SimulatedProxy);
	DamageSlab(Proxy, 60.0f);
	TestEqual(TEXT("a proxy slab does not self-apply damage"), Proxy->WithstandDamage, 150);

	// Editing gate: a slab being edited takes no damage.
	AWoTWallSlab* Editing = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	Editing->bEditing = true;
	DamageSlab(Editing, 60.0f);
	TestEqual(TEXT("an editing slab takes no damage"), Editing->WithstandDamage, 150);

	// Stage derivation from the (replicated) value alone — what a client computes on OnRep.
	AWoTWallSlab* D = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	D->WithstandDamage = 101; TestEqual(TEXT("101 → stage 0"), D->GetStage(), 0);
	D->WithstandDamage = 100; TestEqual(TEXT("100 → stage 1"), D->GetStage(), 1);
	D->WithstandDamage = 51;  TestEqual(TEXT("51 → stage 1"),  D->GetStage(), 1);
	D->WithstandDamage = 50;  TestEqual(TEXT("50 → stage 2"),  D->GetStage(), 2);
	D->WithstandDamage = 0;   TestEqual(TEXT("0 → destroyed stage"), D->GetStage(), 3);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Wall is a server-only, invisible manager; destroying it tears down the slabs it owns (only those).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWallManagerSpec,
	"WoT.Trap.Wall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWallManagerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTWall* Wall = World->SpawnActor<AWoTWall>(TrapAlwaysSpawnParams());
	TestNotNull(TEXT("wall spawned"), Wall);
	TestFalse(TEXT("the wall manager is server-only (not replicated)"), Wall->GetIsReplicated());
	TestTrue(TEXT("the wall manager is invisible"), Wall->IsHidden());

	AWoTWallSlab* Owned1 = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	AWoTWallSlab* Owned2 = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	AWoTWallSlab* Foreign = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	Owned1->SetOwner(Wall);
	Owned2->SetOwner(Wall);

	Wall->Destroy();
	WoTTest::TickWorld(World, 2);
	TestFalse(TEXT("owned slab 1 torn down with the wall"), IsValid(Owned1));
	TestFalse(TEXT("owned slab 2 torn down with the wall"), IsValid(Owned2));
	TestTrue(TEXT("a foreign slab is not torn down"), IsValid(Foreign));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Portcullis: an inverted drop-gate. Starts RAISED; an active enemy touch DROPS it once; it survives
// partial damage but on DESTRUCTION re-raises + fires DestroyedEvent WITHOUT being destroyed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPortcullisSpec,
	"WoT.Trap.Portcullis", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPortcullisSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// WithstandDamage replicates for parity (no client consumer — the gate motion rides Phase).
	const FProperty* WD = FindFProperty<FProperty>(
		AWoTPortcullisMover::StaticClass(), TEXT("WithstandDamage"));
	TestTrue(TEXT("WithstandDamage replicates"), WD && WD->HasAnyPropertyFlags(CPF_Net));

	// A gate = a PortcullisMover (KeyPos: alpha 0 = down/blocking Z100, alpha 1 = raised Z200) + the
	// Portcullis trigger that owns it (resolved by tag in BeginPlay).
	int32 TagN = 0;
	auto MakeGate = [&](uint8 PortTeam, bool bActivate) -> AWoTPortcullis*
	{
		const FName Tag(*FString::Printf(TEXT("PC%d"), TagN++));
		const FTransform X(FRotator::ZeroRotator, FVector(0, 0, 100));
		AWoTPortcullisMover* M = World->SpawnActorDeferred<AWoTPortcullisMover>(
			AWoTPortcullisMover::StaticClass(), X);
		M->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
		M->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
		M->MoveTime = 0.3f;
		M->TagName = Tag;
		M->FinishSpawning(X);   // BeginPlay → HideMover → raised (Z200)

		AWoTPortcullis* P = World->SpawnActorDeferred<AWoTPortcullis>(
			AWoTPortcullis::StaticClass(), FTransform::Identity);
		P->MoverTag = Tag;
		P->Team = PortTeam;
		P->FinishSpawning(FTransform::Identity);   // BeginPlay finds + owns the mover
		if (bActivate) { P->Activate(); }
		return P;
	};
	auto MakePawn = [&](uint8 Team) -> AWoTCharacter*
	{
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 300), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = Team; }
		// Stand under the arch (inside the gate's UE1 cylinder) without an engine
		// overlap, so the spec's explicit touch is the only one delivered.
		WoTTest::PlaceWithoutOverlapEvents(Pawn, FVector(0, 0, 60));
		return Pawn;
	};
	auto DamageMover = [&](AWoTPortcullisMover* M, float Amt)
	{
		FHitResult Hit;
		Hit.ImpactPoint = M->GetActorLocation();
		const FPointDamageEvent Ev(Amt, Hit, FVector::ForwardVector, nullptr);
		M->TakeDamage(Amt, Ev, nullptr, nullptr);
	};
	auto GateRaised = [](AWoTPortcullis* P) { return P->GetMover()->GetActorLocation().Z > 150.0; };

	// --- Lifecycle on one active gate ---
	AWoTPortcullis* Port = MakeGate(/*PortTeam*/1, /*bActivate*/true);
	TestTrue(TEXT("portcullis starts raised (open)"), GateRaised(Port));

	// An enemy walking under drops the gate.
	Port->OnPortcullisOverlap(nullptr, MakePawn(2), nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 30);   // > MoveTime
	TestFalse(TEXT("an active enemy touch drops the gate"), GateRaised(Port));

	// Partial damage: the gate survives, withstand decrements, it stays down.
	AWoTPortcullisMover* M = Port->GetMover();
	DamageMover(M, 50.0f);   // 120 → 70
	TestTrue(TEXT("gate survives partial damage"), IsValid(M));
	TestEqual(TEXT("withstand decremented"), M->WithstandDamage, 70);
	TestFalse(TEXT("gate stays down after partial damage"), GateRaised(Port));

	// Destruction: fires DestroyedEvent, re-raises, and does NOT destroy the actor.
	AWoTTestTrapEventSink* Sink = World->SpawnActorDeferred<AWoTTestTrapEventSink>(
		AWoTTestTrapEventSink::StaticClass(), FTransform::Identity);
	Sink->TagName = TEXT("PortGone");
	Sink->FinishSpawning(FTransform::Identity);
	Port->DestroyedEvent = TEXT("PortGone");
	DamageMover(M, 100.0f);   // 70 → -30 → destroyed path
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("gate re-raises on destruction"), GateRaised(Port));
	TestTrue(TEXT("the mover actor is NOT destroyed (it re-opens)"), IsValid(M));
	TestEqual(TEXT("destroyed event fired through the owner trap"), Sink->TriggerCount, 1);

	// --- Gates: same-team, inactive ---
	AWoTPortcullis* Friendly = MakeGate(1, /*bActivate*/true);
	Friendly->OnPortcullisOverlap(nullptr, MakePawn(1), nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 30);
	TestTrue(TEXT("a same-team touch does not drop the gate"), GateRaised(Friendly));

	AWoTPortcullis* Idle = MakeGate(1, /*bActivate*/false);
	Idle->OnPortcullisOverlap(nullptr, MakePawn(2), nullptr, 0, false, FHitResult());
	WoTTest::TickWorld(World, 30);
	TestTrue(TEXT("an inactive portcullis does not drop"), GateRaised(Idle));

	// IsTrapDeployed tracks Active.
	AWoTPortcullis* Toggle = MakeGate(1, false);
	TestFalse(TEXT("portcullis starts not deployed"), Toggle->IsTrapDeployed());
	Toggle->Activate();
	TestTrue(TEXT("portcullis deployed once Active"), Toggle->IsTrapDeployed());
	Toggle->Deactivate();
	TestFalse(TEXT("portcullis not deployed after Deactivate"), Toggle->IsTrapDeployed());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Spear: a Waiting→Activating→Extended→Reseting FSM. A trigger touch springs it; during the thrust it
// impales (DamageAmount + a momentum shove); it deals no damage once held/retracting, then re-arms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpearSpec,
	"WoT.Trap.Spear", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpearSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto MakePawnAt = [&](uint8 Team, const FVector& Loc) -> AWoTCharacter*
	{
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();   // else the CMC ignores movement (incl. LaunchCharacter)
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = Team; }
		return Pawn;
	};

	// Spear at the origin (Team 1); an enemy (Team 2) at +X so the momentum shove pushes it +X. (The
	// same-team IsInactive gate is proven in WoT.Trap.Base — plain base delegation here.)
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		FVector::ZeroVector, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	Spear->Team = 1;
	TestEqual(TEXT("spear starts Waiting"), (int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Waiting);

	AWoTCharacter* Enemy = MakePawnAt(2, FVector(52, 0, 0));
	Spear->TrapTouch(Enemy);
	TestEqual(TEXT("an enemy trigger touch springs the spear (Activating)"),
		(int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Activating);

	// Impale during Activating: DamageAmount off Health + the momentum shove to the live-pawn
	// spec math (Engine.Pawn TakeDamage): momentum = 20000*Dir; a WALKING victim's Z floors
	// at 0.4*|momentum| = 8000; then /Mass(175) is ADDED → ~114 uu/s along the line + ~46 up.
	const int32 H0 = Enemy->Health;
	// The Z-floor is a PHYS_Walking-only spec term, and the floorless test world spawns
	// pawns falling — make the victim a walking pawn (as a real floor-standing victim is).
	Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Enemy->SetActorLocation(FVector(51, 0, 0), false, nullptr,
		ETeleportType::TeleportPhysics);
	Spear->DamageActor(Enemy);   // post-movement Touch delivery; parity spec covers native routing
	TestEqual(TEXT("impale deals DamageAmount"), Enemy->Health, H0 - 5);
	const FVector ShoveVel = Enemy->GetCharacterMovement()->Velocity;
	TestTrue(FString::Printf(TEXT("shove adds 20000/175 ~ 114 uu/s along the spear line (vx %.1f)"),
		ShoveVel.X), FMath::Abs(ShoveVel.X - 20000.0f / 175.0f) < 15.0f);
	TestTrue(FString::Printf(TEXT("a walking victim pops up (0.4*|momentum|/mass ~ +46; vz %.1f)"),
		ShoveVel.Z), ShoveVel.Z > 20.0f);
	TestEqual(TEXT("the shared momentum pipeline enters falling"),
		(int32)Enemy->GetCharacterMovement()->MovementMode.GetValue(),
		(int32)MOVE_Falling);

	// The thrust window closes → Extended → no more damage there.
	WoTTest::TickWorld(World, 40);   // > ThrustTime (0.5s)
	TestEqual(TEXT("the spear extends after the thrust"),
		(int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Extended);
	const int32 H1 = Enemy->Health;
	Spear->DamageActor(Enemy);
	TestEqual(TEXT("no impale while extended"), Enemy->Health, H1);

	// Extended (2s) → Reseting → Waiting (1.25s), re-armed.
	WoTTest::TickWorld(World, 130);   // > ExtendedTime (2s)
	TestEqual(TEXT("the spear retracts (Reseting)"),
		(int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Reseting);
	WoTTest::TickWorld(World, 80);    // > ResetTime (1.25s)
	TestEqual(TEXT("the spear re-arms (Waiting)"),
		(int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Waiting);

	// A pawn appearing over the shaft springs the trap, but the collision toggle that
	// begins the thrust is not itself a retail Touch event. A later movement contact owns
	// the impale (covered by WoT.Parity.A.SpearMovementTouch).
	TestEqual(TEXT("the raw character CDO carries no gameplay-health fallback"),
		GetDefault<AWoTCharacter>()->MaxHealth, 0);
	AWoTCharacter* Stander = MakePawnAt(2, FVector(52, 0, 0));   // over the tip (51.2uu out)
	TestEqual(TEXT("appearing in the reach springs the spear by pure contact"),
		(int32)Spear->GetPhase(), (int32)EWoTSpearPhase::Activating);
	TestEqual(TEXT("arming collision does not synthesize an impale"),
		Stander->Health, Stander->MaxHealth);

	// A proxy spear does not spring (authority-gated).
	AWoTSpear* ProxySpear = World->SpawnActor<AWoTSpear>(
		FVector(500, 0, 0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	ProxySpear->SetRole(ROLE_SimulatedProxy);
	ProxySpear->TrapTouch(MakePawnAt(2, FVector(600, 0, 0)));
	TestEqual(TEXT("a proxy spear does not spring"),
		(int32)ProxySpear->GetPhase(), (int32)EWoTSpearPhase::Waiting);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Pit: a false floor. AWoTPit spawns its AWoTPitTrap surface and claims a free pre-placed
// AWoTPitOpening; touching the surface teleports the victim to the opening (horizontal offset from
// the trap center + view rotation preserved) — the shaft below the opening and fall damage do the
// hurting. Touching the opening climbs back out the same way. A resting Seal disables the pit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPitSpec,
	"WoT.Trap.Pit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto MakePawnAt = [&](uint8 Team, const FVector& Loc) -> AWoTCharacter*
	{
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = Team; }
		return Pawn;
	};

	// A pre-placed shaft mouth, deep below and far away (as the Battle maps place them).
	AWoTPitOpening* Opening = World->SpawnActor<AWoTPitOpening>(
		FVector(4000, 0, -800), FRotator::ZeroRotator, TrapAlwaysSpawnParams());

	// Spawning the pit (Team 1) spawns + links the surface and claims the opening.
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector::ZeroVector, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	Pit->Team = 1;
	AWoTPitTrap* Trap = Pit->ThePitTrap;
	TestNotNull(TEXT("the pit spawns its trap surface"), Trap);
	if (!Trap) { WoTTest::DestroySpecWorld(World); return false; }
	TestTrue(TEXT("the pit claims the free opening (both links)"),
		Opening->ThePitTrap == Trap && Trap->ThePitOpening == Opening);
	TestTrue(TEXT("the pit owns both ends"),
		Trap->GetOwner() == Pit && Opening->GetOwner() == Pit);

	// "Deploy": move the surface to its spot and expose it (the editor placement path is deferred).
	Trap->SetActorLocation(FVector(1000, 0, 0));
	Trap->ShowTrap();

	// NOTE: spawn/SetActorLocation fire REAL overlap notifications synchronously in the test
	// world — so this spec drives the pit the honest way: spawn each pawn CLEAR of the volumes
	// (pad r50, mouth r128), set its team first, then step it in with SetActorLocation and let
	// the engine overlap deliver the Touch. Direct handler calls are reserved for probes that
	// must not physically move (the JustAccepted bounce guard).

	// An enemy steps onto the pad, off-center, looking somewhere distinctive: lands at the
	// opening with the SAME horizontal offset and view yaw (PitTrap Touch -> PitOpening.Accept).
	AWoTCharacter* Enemy = MakePawnAt(2, FVector(1100, 20, 5));
	Enemy->SetActorRotation(FRotator(0, 123, 0));
	Enemy->SetActorLocation(FVector(1010, 20, 5));   // step in -> the overlap fires the fall
	TestTrue(TEXT("falling in lands at the opening, offset preserved"),
		Enemy->GetActorLocation().Equals(FVector(4010, 20, -800), 1.0));
	TestTrue(TEXT("view rotation rides through the teleport"),
		FMath::Abs(FRotator::NormalizeAxis(Enemy->GetActorRotation().Yaw - 123.0)) < 1.0);

	// Still inside the mouth: a repeat arrival overlap must not bounce them back (JustAccepted).
	Opening->NotifyActorBeginOverlap(Enemy);
	TestTrue(TEXT("no arrival bounce at the opening"),
		Enemy->GetActorLocation().Equals(FVector(4010, 20, -800), 1.0));

	// They fall down the shaft (leaving the mouth re-arms it), then climb the vines back up:
	// re-entering the mouth returns them to the trap surface, offset from the OPENING preserved
	// (PitOpening Touch -> PitTrap.Accept).
	Enemy->SetActorLocation(FVector(4400, -15, -790));   // off the mouth -> end-overlap re-arms
	Enemy->SetActorLocation(FVector(4100, -15, -795));   // climb back in -> the overlap fires
	TestTrue(TEXT("climbing out returns to the trap surface, offset preserved"),
		Enemy->GetActorLocation().Equals(FVector(1100, -15, 0), 1.0));

	// Same-team walk-over never springs it (Pit.IsInactive via the owner).
	AWoTCharacter* Friendly = MakePawnAt(1, FVector(1100, 0, 5));
	Friendly->SetActorLocation(FVector(1005, 0, 5));
	TestTrue(TEXT("same-team walk-over is safe"),
		Friendly->GetActorLocation().Equals(FVector(1005, 0, 5), 1.0));

	// A Seal resting on the cover holds the pit open; removing it (and re-entering — Touch only
	// re-fires on re-entry, as in UE1) re-arms.
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		FVector(1000, 0, 0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	AWoTCharacter* Enemy2 = MakePawnAt(2, FVector(1108, -6, 5));
	Enemy2->SetActorLocation(FVector(1008, -6, 5));
	TestTrue(TEXT("a resting Seal disables the pit"),
		Enemy2->GetActorLocation().Equals(FVector(1008, -6, 5), 1.0));
	Seal->Destroy();
	Enemy2->SetActorLocation(FVector(1108, -6, 5));   // step off
	Enemy2->SetActorLocation(FVector(1008, -6, 5));   // step back on
	TestTrue(TEXT("seal removed -> the pit springs again"),
		Enemy2->GetActorLocation().Equals(FVector(4008, -6, -800), 1.0));

	// A second pit finds no free opening: inert (surface destroyed), the pit actor survives.
	AWoTPit* Pit2 = World->SpawnActor<AWoTPit>(
		FVector(2000, 0, 0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	TestNull(TEXT("no free opening -> no trap surface"), Pit2->ThePitTrap.Get());
	TestTrue(TEXT("the placed pit actor survives"), IsValid(Pit2));

	// Destroying a pit frees its opening; the next pit claims it.
	Pit->Destroy();
	TestTrue(TEXT("destroying the pit releases the opening"), Opening->ThePitTrap == nullptr);
	AWoTPit* Pit3 = World->SpawnActor<AWoTPit>(
		FVector(3000, 0, 0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	TestTrue(TEXT("a later pit claims the freed opening"),
		Pit3->ThePitTrap != nullptr && Opening->ThePitTrap == Pit3->ThePitTrap);

	// A proxy surface never teleports (authority gate).
	AWoTPitTrap* Trap3 = Pit3->ThePitTrap;
	Trap3->ShowTrap();
	Trap3->SetRole(ROLE_SimulatedProxy);
	AWoTCharacter* Enemy3 = MakePawnAt(2, FVector(3100, 3, 5));
	Enemy3->SetActorLocation(FVector(3004, 3, 5));   // step in -> role gate holds
	TestTrue(TEXT("a proxy pit surface does not teleport"),
		Enemy3->GetActorLocation().Equals(FVector(3004, 3, 5), 1.0));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Fall damage — the Pit's damage half (WOTPlayer.Landed): players take 'fell' damage on hard
// landings (impact velocity <= -1100), gib below -2000; NPCs never do.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFallDamageSpec,
	"WoT.Trap.FallDamage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFallDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto MakePawnAt = [&](uint8 Team, const FVector& Loc) -> AWoTCharacter*
	{
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = Team; }
		return Pawn;
	};

	// A real landing always carries the floor normal; a zero normal would
	// take the retail ladder branch (HitNormal.Z <= 0.001, WOTPlayer.uc:2366).
	FHitResult Hit;
	Hit.Normal = FVector::UpVector;
	Hit.ImpactNormal = FVector::UpVector;

	// Above the -1100 threshold: bruise-free (a plain hard jump landing).
	AWoTCharacter* P = MakePawnAt(1, FVector(0, 0, 100));
	const int32 H0 = P->Health;
	P->GetCharacterMovement()->Velocity = FVector(0, 0, -1000);
	P->Landed(Hit);
	TestEqual(TEXT("landing at -1000 is damage-free"), P->Health, H0);

	// The formula band: -0.15 * (VZ + 1050), UnrealScript float->int truncation at TakeDamage.
	P->GetCharacterMovement()->Velocity = FVector(0, 0, -1500);
	P->Landed(Hit);
	TestEqual(TEXT("landing at -1500 costs trunc(67.5) = 67"), P->Health, H0 - 67);

	// The gib band (< -2000): Health + 250 — always lethal.
	AWoTCharacter* Q = MakePawnAt(2, FVector(100, 0, 100));
	Q->GetCharacterMovement()->Velocity = FVector(0, 0, -2500);
	Q->Landed(Hit);
	TestTrue(TEXT("landing at -2500 kills outright"), Q->Health <= 0);

	// NPC fall damage — WOTPawn.Landed (WOTPawn.uc:1074-1096): the same -0.15*(VZ+1050)
	// core, gated at MinLandedDamageVelocity-850 = -1700 (vs the player's -1100) and
	// gibbed below FatalLandedDamageVelocity -2000. (The shipped .uc does apply fall
	// damage to NPCs.)
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(200, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	const int32 NH0 = NPC->Health;
	NPC->GetCharacterMovement()->Velocity = FVector(0, 0, -1600);
	NPC->Landed(Hit);
	TestEqual(TEXT("an NPC landing at -1600 is damage-free (its gate is -1700, not -1100)"),
		NPC->Health, NH0);
	NPC->GetCharacterMovement()->Velocity = FVector(0, 0, -1800);
	NPC->Landed(Hit);
	TestEqual(TEXT("an NPC landing at -1800 costs trunc(112.5) = 112"), NPC->Health, NH0 - 112);

	AWoTNPCCharacter* NPC2 = World->SpawnActor<AWoTNPCCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	NPC2->GetCharacterMovement()->Velocity = FVector(0, 0, -2100);
	NPC2->Landed(Hit);
	TestTrue(TEXT("an NPC landing below -2000 gibs (Health+250)"), NPC2->Health <= 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// FireWall: a burning barricade line. The slab burns touchers for 8 Fire on entry and every 0.5s
// while they stand in the flame, consuming itself by the same amount each burn; Health 0 destroys
// it. (Pawns get MOVE_None while parked in the flame — the test world has no floor, and gravity
// would drop them out of the volume during the interval ticks.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFireWallSpec,
	"WoT.Trap.FireWall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFireWallSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto MakePawnAt = [&](uint8 Team, const FVector& Loc) -> AWoTCharacter*
	{
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
		PC->Possess(Pawn);
		if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = Team; }
		return Pawn;
	};

	// The manager (Team 1) + a slab it owns, as the deferred deploy would spawn it.
	AWoTFireWall* Wall = World->SpawnActor<AWoTFireWall>(
		FVector::ZeroVector, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	Wall->Team = 1;
	FActorSpawnParameters SlabSP = TrapAlwaysSpawnParams();
	SlabSP.Owner = Wall;
	AWoTFireWallSlab* Slab = World->SpawnActor<AWoTFireWallSlab>(
		AWoTFireWallSlab::StaticClass(), FVector(1000, 0, 0), FRotator::ZeroRotator, SlabSP);
	TestEqual(TEXT("the flame starts with full fuel"), Slab->Health, 450);

	// An enemy walks into the flame (volume at z+40): burned 8 Fire on entry, the flame eats 8.
	AWoTCharacter* Enemy = MakePawnAt(2, FVector(1100, 0, 40));
	const int32 H0 = Enemy->Health;
	Enemy->SetActorLocation(FVector(1010, 0, 40));   // step in -> the overlap burns
	TestEqual(TEXT("entry burns the toucher for 8"), Enemy->Health, H0 - 8);
	TestEqual(TEXT("the flame consumes itself by 8"), Slab->Health, 442);

	// Standing in it: re-burned every DamageInterval (0.5s).
	Enemy->GetCharacterMovement()->SetMovementMode(MOVE_None);
	WoTTest::TickWorld(World, 35);   // > one 0.5s interval
	TestEqual(TEXT("the interval re-burns the stander"), Enemy->Health, H0 - 16);
	TestEqual(TEXT("the flame keeps consuming itself"), Slab->Health, 434);

	// Stepping out stops the burning.
	Enemy->SetActorLocation(FVector(1100, 0, 40));
	WoTTest::TickWorld(World, 35);
	TestEqual(TEXT("out of the flame -> no more burns"), Enemy->Health, H0 - 16);
	TestEqual(TEXT("the flame stops consuming"), Slab->Health, 434);

	// Same-team crossing is free (the owner trap's gate).
	AWoTCharacter* Friendly = MakePawnAt(1, FVector(1100, 60, 40));
	const int32 FH0 = Friendly->Health;
	Friendly->SetActorLocation(FVector(1010, 6, 40));
	TestEqual(TEXT("same-team crossing is unburned"), Friendly->Health, FH0);
	TestEqual(TEXT("a friendly does not consume the flame"), Slab->Health, 434);

	// Attacked to 0 -> destroyed.
	FWoTDamageEvent Hit;
	Hit.WoTDamageType = TEXT("hurt");
	Slab->TakeDamage(1000, Hit, Enemy->GetController(), nullptr);
	TestFalse(TEXT("fuel 0 by damage -> the slab is destroyed"), IsValid(Slab));

	// The item contract says crossings consume the wall; verify that self-damage
	// can destroy the slab too (FireWallInventory.uc:18).
	AWoTFireWallSlab* Slab2 = World->SpawnActor<AWoTFireWallSlab>(
		AWoTFireWallSlab::StaticClass(), FVector(2000, 0, 0), FRotator::ZeroRotator, SlabSP);
	Slab2->Health = 16;
	AWoTCharacter* Enemy2 = MakePawnAt(2, FVector(2100, 0, 40));
	const int32 H2 = Enemy2->Health;
	Enemy2->SetActorLocation(FVector(2010, 0, 40));   // entry burn: fuel 16 -> 8
	Enemy2->GetCharacterMovement()->SetMovementMode(MOVE_None);
	WoTTest::TickWorld(World, 35);                     // interval burn: fuel 8 -> 0 -> destroyed
	TestEqual(TEXT("both burns landed on the crosser"), Enemy2->Health, H2 - 16);
	TestFalse(TEXT("the last burn stamps out the fire"), IsValid(Slab2));

	// Hidden slabs neither burn nor take damage.
	AWoTFireWallSlab* Slab3 = World->SpawnActor<AWoTFireWallSlab>(
		AWoTFireWallSlab::StaticClass(), FVector(3000, 0, 0), FRotator::ZeroRotator, SlabSP);
	Slab3->HideTrap();
	const int32 H3 = Enemy2->Health;
	Slab3->DamageActor(Enemy2);
	TestEqual(TEXT("a hidden flame does not burn"), Enemy2->Health, H3);
	Slab3->TakeDamage(100, Hit, Enemy2->GetController(), nullptr);
	TestEqual(TEXT("a hidden flame takes no damage"), Slab3->Health, 450);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// TrapBurst is a fire-and-forget cosmetic: it self-destructs after its lifespan.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapBurstSpec,
	"WoT.Trap.Burst", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapBurstSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTrapBurst* Burst = World->SpawnActor<AWoTTrapBurst>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	TestNotNull(TEXT("burst spawned"), Burst);
	TestTrue(TEXT("burst replicates (server-spawned, seen on clients)"), Burst->GetIsReplicated());

	// Tick past the burst lifespan (~1s) — it destroys itself.
	WoTTest::TickWorld(World, 75);
	TestFalse(TEXT("burst self-destructed after its lifespan"), IsValid(Burst));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The Alarm ring FSM (Alarm.uc Waiting state): the player-only touch gate with ring-for-everyone
// team semantics, the AlarmSounded fan-out, the Sound window's Bump gate + re-arm, and the
// TakeDamage ring path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAlarmSpec,
	"WoT.Trap.Alarm", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAlarmSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	if (!Alarm)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Alarm->Team = 1;

	// A player pawn on the SAME team as the alarm — the touch gate reads IsInactive(None), so a
	// same-team player still rings it. Spawned CLEAR of the alarm capsule: spawn-overlap fires
	// NotifyActorBeginOverlap → Touch and would ring before the gate cases run.
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->InitPlayerState();
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(200, 0, 50), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	PC->Possess(Player);
	if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>())
	{
		PS->Team = 1;
	}

	// An idle NPC guard in fan-out range (SoundRadius*16 = 4080uu); auto-possessed on spawn.
	AWoTTestNPCCharacter* Guard = World->SpawnActor<AWoTTestNPCCharacter>(
		FVector(300, 0, 0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	Guard->GetCharacterMovement()->SetMovementMode(MOVE_Flying);   // no floor headless
	WoTTest::TickWorld(World, 2);

	// --- Touch gates (Alarm.uc:68-84) ---
	Alarm->Touch(Guard);
	TestFalse(TEXT("an NPC touch does not ring (players only)"), Alarm->bRung);

	Alarm->bEditing = true;
	Alarm->Touch(Player);
	TestFalse(TEXT("no ring while being edited (IsInactive gate)"), Alarm->bRung);
	Alarm->bEditing = false;

	Alarm->Touch(Player);
	TestTrue(TEXT("a player touch rings — same team included (ring for everyone)"), Alarm->bRung);

	// --- Fan-out: the guard's controller adopted the rung alarm as its intermediate goal ---
	TestTrue(TEXT("AlarmSounded fanned out to the guard"),
	         Guard->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	TestEqual(TEXT("the guard's goal is the alarm"),
	          Guard->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
	          static_cast<AActor*>(Alarm));

	// --- Sound window (Alarm.uc:90-94): Bump disabled for AlarmDelay, Touch stays live ---
	TestFalse(TEXT("bump disabled while the gong sounds"), Alarm->IsBumpEnabled());
	Alarm->bRung = false;
	Alarm->Bump(Player);
	TestFalse(TEXT("a bump mid-window does not re-ring"), Alarm->bRung);
	Alarm->Touch(Player);
	TestTrue(TEXT("a fresh touch mid-window re-rings (only Bump is disabled)"), Alarm->bRung);

	// The re-ring restarted the window; advance past AlarmDelay (3s at 1/60 steps).
	WoTTest::TickWorld(World, 3 * 60 + 10);
	TestTrue(TEXT("bump re-enabled after AlarmDelay"), Alarm->IsBumpEnabled());
	Alarm->bRung = false;
	Alarm->Bump(Player);
	TestTrue(TEXT("a bump re-rings once re-armed"), Alarm->bRung);

	// --- TakeDamage → Touch(InstigatedBy): shooting the alarm rings it (player instigator) ---
	Alarm->bRung = false;
	const FWoTDamageEvent DamageEvent;
	Alarm->TakeDamage(10.0f, DamageEvent, PC, nullptr);
	TestTrue(TEXT("damaging the alarm rings it (TakeDamage → Touch)"), Alarm->bRung);

	// --- Overlap entry drives Touch (UE1 Touch): a player walking into the alarm rings it ---
	Alarm->bRung = false;
	Player->SetActorLocation(Alarm->GetActorLocation() + FVector(20, 0, 50));
	TestTrue(TEXT("walking into the alarm rings it (overlap → Touch)"), Alarm->bRung);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The trap visual binds. Every runtime trap actor resolves its imported
// mesh at BeginPlay (AWoTTrap::BindTrapVisual); the WallSlab swaps its stage skin off
// the replicated WithstandDamage; the Spear presentation follows the replicated Phase;
// the TrapBurst one-shots 'Explo'. Needs the imported trap meshes + MI_TrapWall2/3
// (import_mesh.py per mesh + import_trap_skins.py).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapVisualsSpec,
	"WoT.Trap.Visuals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTrapVisualsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto VisualOf = [](AActor* A) -> UWoTVertexAnimComponent*
	{
		return A ? A->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr;
	};
	auto BoundMeshName = [&](AActor* A) -> FString
	{
		UWoTVertexAnimComponent* V = VisualOf(A);
		USkeletalMesh* M = V ? V->GetSkeletalMeshAsset() : nullptr;
		return M ? M->GetName() : FString();
	};

	// --- Every trap binds its mesh + UE1 DrawScale at BeginPlay. ---
	AWoTWallSlab* Slab = World->SpawnActor<AWoTWallSlab>(TrapAlwaysSpawnParams());
	TestEqual(TEXT("WallSlab binds its mesh"), BoundMeshName(Slab), FString(TEXT("WallSlab")));
	if (UWoTVertexAnimComponent* V = VisualOf(Slab))
	{
		// The world-size invariant: the meshmap scale is BAKED into the
		// geometry, so the mesh's long axis x component scale must fill the shipped
		// 28x142 blocking cylinder (component scale = the authored DrawScale, 1.0).
		// Longest-extent form: the GLB->Interchange axis remap shuffles which local
		// axis is "up".
		const FVector Extent = V->GetSkeletalMeshAsset()->GetBounds().BoxExtent
		                       * V->GetRelativeScale3D();
		TestTrue(FString::Printf(
		             TEXT("WallSlab visual fills its 142uu cylinder (extent %s)"),
		             *Extent.ToString()),
		         FMath::Abs(Extent.GetMax() - 71.0f) < 5.0f);
	}
	AWoTFireWallSlab* FireSlab = World->SpawnActor<AWoTFireWallSlab>(TrapAlwaysSpawnParams());
	TestEqual(TEXT("FireWallSlab binds the log base"),
	          BoundMeshName(FireSlab), FString(TEXT("FWBaseLogs")));
	if (UWoTVertexAnimComponent* V = VisualOf(FireSlab))
	{
		// FireWallSlab.uc authors DrawScale=1.2: baked extents x 1.2 = the shipped
		// CollisionRadius 19 (longest axis — the logs lie flat).
		const FVector Extent = V->GetSkeletalMeshAsset()->GetBounds().BoxExtent
		                       * V->GetRelativeScale3D();
		TestTrue(FString::Printf(TEXT("FWBaseLogs fills its 19uu radius (extent %s)"),
		                         *Extent.ToString()),
		         FMath::Abs(Extent.GetMax() - 19.0f) < 2.0f);
	}
	// --- 6b-ii: the FireWall flame bundle — 3 local sprayers, verbatim configs,
	// Health-ratio scaling, staggered fade on destroy. ---
	AWoTParticleSprayer* Body = nullptr;    // FireWallSprayer1 (Volume 7)
	AWoTParticleSprayer* Smoke = nullptr;   // FireWallSprayer2 (modulated)
	AWoTParticleSprayer* Jet = nullptr;     // FireWallSprayer3 (Volume 8)
	int32 FlameCount = 0;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (It->GetOwner() != FireSlab)
		{
			continue;
		}
		++FlameCount;
		if (It->bModulateOnly)          { Smoke = *It; }
		else if (It->Gravity.Z > 100.0f) { Jet = *It; }
		else                             { Body = *It; }
	}
	TestEqual(TEXT("three flame sprayers spawned locally"), FlameCount, 3);
	if (Body && Smoke && Jet)
	{
		TestEqual(TEXT("flame body Volume 7 (FireWallSprayer1)"), Body->Volume, 7.0f);
		TestEqual(TEXT("flame body MinVolume 3"), Body->MinVolume, 3.0f);
		TestEqual(TEXT("smoke Volume 1.25 (FireWallSprayer2)"), Smoke->Volume, 1.25f);
		TestEqual(TEXT("jet Volume 8 (FireWallSprayer3)"), Jet->Volume, 8.0f);
		TestEqual(TEXT("body templates (NumTemplates=2 — the third .uc row is unused)"),
		          Body->Templates.Num(), 2);
		TestTrue(TEXT("body flipbook materials resolved (import_flame_fx.py)"),
		         Body->AddMaterials.Num() == 2 && Body->AddMaterials[0] != nullptr
		         && Body->ModMaterials[1] != nullptr);
		TestTrue(TEXT("smoke material resolved"),
		         Smoke->ModMaterials.Num() == 1 && Smoke->ModMaterials[0] != nullptr);
		TestTrue(TEXT("smoke sinks, jet rises (shipped gravities)"),
		         Smoke->Gravity.Z == -20.0f && Jet->Gravity.Z == 110.0f);

		// Half the fuel → everything halves (Volume, MinVolume, glow/scale bounds).
		FWoTDamageEvent Burn;
		Burn.WoTDamageType = TEXT("Sliced");
		FireSlab->TakeDamage(225.0f, Burn, nullptr, nullptr);   // 450 → 225
		TestEqual(TEXT("half health halves the body volume"), Body->Volume, 3.5f);
		TestEqual(TEXT("half health halves MinVolume"), Body->MinVolume, 1.5f);
		TestEqual(TEXT("half health halves the jet volume"), Jet->Volume, 4.0f);
		TestEqual(TEXT("template glow scales (body T0 MinScaleGlow .85 x .5)"),
		          Body->Templates[0].MinScaleGlow, 0.425f);
		TestEqual(TEXT("template draw scale scales (body T0 MaxDrawScale 2 x .5)"),
		          Body->Templates[0].MaxDrawScale, 1.0f);

		// Destroy → the flames stop emitting and fade staggered (1.5/1.7/1.0).
		FireSlab->Destroy();
		TestTrue(TEXT("destroyed slab stops the flames"),
		         !Body->bOn && !Smoke->bOn && !Jet->bOn);
		TestEqual(TEXT("body fades over 1.5s"), Body->GetLifeSpan(), 1.5f);
		TestEqual(TEXT("smoke fades over 1.7s"), Smoke->GetLifeSpan(), 1.7f);
		TestEqual(TEXT("jet fades over 1.0s"), Jet->GetLifeSpan(), 1.0f);
	}

	TestEqual(TEXT("Pit binds the false floor"),
	          BoundMeshName(World->SpawnActor<AWoTPit>(TrapAlwaysSpawnParams())),
	          FString(TEXT("Pit")));
	TestEqual(TEXT("Alarm binds the gong"),
	          BoundMeshName(World->SpawnActor<AWoTAlarm>(TrapAlwaysSpawnParams())),
	          FString(TEXT("Alarm")));

	// --- WallSlab stage skins: base at full, then the B3 texture override for
	// TrapWall2/TrapWall3 (Wall's commented-out Stage1 line keeps the base skin). ---
	auto SlabSkin = [&]() -> FString
	{
		UWoTVertexAnimComponent* V = VisualOf(Slab);
		if (!V)
		{
			return FString();
		}
		const FSoftObjectPath* Skin = V->GetRuntimeSkinOverrides().Find(0);
		return Skin ? Skin->ToString() : FString();
	};
	auto DamageSlab = [&](float Amount)
	{
		FHitResult Hit;
		Hit.ImpactPoint = Slab->GetActorLocation();
		const FPointDamageEvent Ev(Amount, Hit, FVector::ForwardVector, nullptr);
		Slab->TakeDamage(Amount, Ev, nullptr, nullptr);
	};
	TestEqual(TEXT("a full slab has no stage override"), SlabSkin(), FString());
	DamageSlab(60.0f);   // 150 → 90: stage 1
	TestEqual(TEXT("stage 1 uses the TrapWall2 B3 texture"), SlabSkin(),
		FString(TEXT("/Game/WoT/Textures/WoTTraps/TrapWall2.TrapWall2")));
	DamageSlab(50.0f);   // 90 → 40: stage 2
	TestEqual(TEXT("stage 2 uses the TrapWall3 B3 texture"), SlabSkin(),
		FString(TEXT("/Game/WoT/Textures/WoTTraps/TrapWall3.TrapWall3")));

	// --- Spear: presentation follows the Phase FSM. ---
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(TrapAlwaysSpawnParams());
	TestEqual(TEXT("Spear binds its mesh"), BoundMeshName(Spear), FString(TEXT("Spear")));
	UWoTVertexAnimComponent* SpearV = VisualOf(Spear);
	if (!TestNotNull(TEXT("spear visual"), SpearV))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("Waiting holds the retracted frame"), SpearV->IsAnimating());

	// Spring it: the trap is neutral (255) and an unpossessed pawn reads 255 — put the
	// spear on a real team so the pawn counts as an enemy.
	Spear->Team = 5;
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(500, 500, 100), FRotator::ZeroRotator,
		TrapAlwaysSpawnParams());
	Spear->TrapTouch(Victim);
	TestEqual(TEXT("touch springs the spear"), Spear->GetPhase(), EWoTSpearPhase::Activating);
	TestTrue(TEXT("Activating plays the thrust"), SpearV->IsAnimating());
	TestEqual(TEXT("the thrust sequence"), SpearV->GetCurrentSequence(), FName(TEXT("Activating")));

	WoTTest::TickWorld(World, 35);   // past ThrustTime 0.5
	TestEqual(TEXT("thrust dwell ends Extended"), Spear->GetPhase(), EWoTSpearPhase::Extended);
	TestFalse(TEXT("Extended holds the tip out (frame-stopped)"), SpearV->IsAnimating());
	TestTrue(TEXT("held on the thrust's end pose"), SpearV->GetAnimFrame() > 0.9f);

	WoTTest::TickWorld(World, 2 * 60 + 10);   // past ExtendedTime 2
	TestEqual(TEXT("hold ends Reseting"), Spear->GetPhase(), EWoTSpearPhase::Reseting);
	WoTTest::TickWorld(World, 75 + 10);       // past ResetTime 1.25
	TestEqual(TEXT("reset re-arms Waiting"), Spear->GetPhase(), EWoTSpearPhase::Waiting);
	TestFalse(TEXT("re-armed retracted frame"), SpearV->IsAnimating());
	TestEqual(TEXT("back on the retracted pose"), SpearV->GetAnimFrame(), 0.0f);

	// --- TrapBurst: a replicated one-shot playing 'Explo' on spawn. ---
	AWoTTrapBurst* Burst = World->SpawnActor<AWoTTrapBurst>(TrapAlwaysSpawnParams());
	TestEqual(TEXT("TrapBurst binds its mesh"), BoundMeshName(Burst), FString(TEXT("Trapburst")));
	TestTrue(TEXT("the burst plays on spawn"), VisualOf(Burst)->IsAnimating());
	TestEqual(TEXT("the burst sequence"),
	          VisualOf(Burst)->GetCurrentSequence(), FName(TEXT("Explo")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPitModulatedVisualSpec,
	"WoT.Trap.PitModulatedVisual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPitModulatedVisualSpec::RunTest(const FString&)
{
	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	if (!TestNotNull(TEXT("committed Pit modulate master loads"), ModulateMaster))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(TrapAlwaysSpawnParams());
	UWoTVertexAnimComponent* PitVisual = Pit
		? Pit->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr;
	UMaterialInterface* PitMaterial = PitVisual ? PitVisual->GetMaterial(0) : nullptr;
	TestNotNull(TEXT("Pit visual binds slot 0"), PitMaterial);
	TestNotNull(TEXT("Pit slot 0 is a dynamic material instance"),
		Cast<UMaterialInstanceDynamic>(PitMaterial));
	TestTrue(TEXT("Pit slot 0 is rooted at M_WoT_MeshModulate"),
		TrapSpecMaterialParentChainContains(PitMaterial, ModulateMaster));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityASpearMovementTouchSpec,
	"WoT.Parity.A.SpearMovementTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTParityASpearMovementTouchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(FVector::ZeroVector, FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	Spear->Team = 1;
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->SetAsLocalPlayerController(); PC->InitPlayerState();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector(100,100,0), FRotator::ZeroRotator, TrapAlwaysSpawnParams());
	PC->Possess(Pawn);
	if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) { PS->Team = 2; }
	Pawn->SetActorLocation(FVector(52,0,0));
	const int32 HealthAtActivation = Pawn->Health;
	TestEqual(TEXT("real blocker contact activates the spear"), Spear->GetPhase(), EWoTSpearPhase::Activating);
	TestEqual(TEXT("collision re-enable does not synthesize an impale"), HealthAtActivation, Pawn->MaxHealth);
	// Movement in the activation frame must not be swallowed while collision-enable
	// callbacks are being filtered.
	Pawn->SetActorLocation(FVector(51,0,0)); WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	TestEqual(TEXT("a later movement-generated blocker touch impales once"), Pawn->Health, HealthAtActivation - 5);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
