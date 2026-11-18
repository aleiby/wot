#pragma once

// Test doubles for the NPC/AI specs: the dynamically-created test world has no
// navmesh, so the navigability primitives are forced open — the specs exercise
// the decision layer (goals, cadence, FSM, strikes), not nav queries.

#include "CoreMinimal.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../Projectiles/WoTIKTrailer.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTProjectile.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "WoTTestAIClasses.generated.h"

UCLASS()
class UWoTTestBrainComponent : public UWoTBrainComponent
{
	GENERATED_BODY()

public:
	// Forced navigability primitives (no navmesh headless). Visibility is forced
	// too so the band ladder's proximity classification is fully controllable
	// (the ladder gates Reachable on Visible); default all-open matches the
	// pre-ladder specs that leaned on the real LOS trace succeeding in the open.
	bool bForceReachable = true;
	bool bForceVisible = true;
	bool bForcePathable = false;
	mutable int32 Fix9PathableQueryCount = 0;

protected:
	virtual bool QueryVisible(const FWoTGoal& Goal) const override { return bForceVisible; }
	virtual bool QueryReachable(EWoTGoalIndex Index) const override { return bForceReachable; }
	virtual bool QueryPathable(EWoTGoalIndex Index) const override
	{
		++Fix9PathableQueryCount;
		return bForcePathable;
	}
};

UCLASS()
class AWoTTestNPCCharacter : public AWoTNPCCharacter
{
	GENERATED_BODY()

public:
	AWoTTestNPCCharacter(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer.SetDefaultSubobjectClass<UWoTTestBrainComponent>(TEXT("Brain")))
	{
	}

	bool bCaptureWalkOffVelocityForFix3 = false;
	bool bFix3CapturedWalkOffVelocity = false;
	FVector Fix3WalkOffVelocity = FVector::ZeroVector;
	virtual void OnWalkingOffLedge_Implementation(
		const FVector& PreviousFloorImpactNormal,
		const FVector& PreviousFloorContactNormal,
		const FVector& PreviousLocation, float TimeDelta) override
	{
		Super::OnWalkingOffLedge_Implementation(PreviousFloorImpactNormal,
			PreviousFloorContactNormal, PreviousLocation, TimeDelta);
		if (bCaptureWalkOffVelocityForFix3)
		{
			bFix3CapturedWalkOffVelocity = true;
			Fix3WalkOffVelocity = GetVelocity();
		}
	}
};

// Passive FSM observation: keep every real state entry, including multiple
// transitions within one world tick. Selection and event dispatch stay in Super.
UCLASS()
class AWoTTestStateObservationController : public AWoTNPCController
{
	GENERATED_BODY()
public:
	TArray<EWoTBrainState> StateEntries;
protected:
	virtual void BeginState(EWoTBrainState InState) override
	{
		StateEntries.Add(InState);
		Super::BeginState(InState);
	}
};

// Passive hint observation; custom immutable data only, production dispatch unchanged.
UCLASS()
class AWoTTestMovementObservationController : public AWoTNPCController
{
	GENERATED_BODY()
public:
	TArray<FVector> OffensivePulseLocations;
	TArray<float> OffensivePulseTimes;
	void SetPatternFixture(const FWoTMovementPattern& Pattern)
	{
		// Isolate one handler's timing from Trolloc's throwing-weapon preference
		// flip; the ordinary classifier and notifier still execute unchanged.
		TrackingBands.bPreferVisibility = false;
		TrackingBands.FlipOdds = -1;
		auto& Band = TrackingBands.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)];
		Band.MovementBinding.Pattern = Pattern;
	}
protected:
	virtual void OnNotifierPulse(EWoTNotifierSlot Slot) override
	{
		if (Slot == EWoTNotifierSlot::Offensive && GetPawn())
		{
			OffensivePulseLocations.Add(GetPawn()->GetActorLocation());
			OffensivePulseTimes.Add(GetWorld()->GetTimeSeconds());
		}
		Super::OnNotifierPulse(Slot);
	}
};

// A test NPC whose AUTHORED sound-volume default is not 1.0, so specs can tell
// "0.10 * class default" apart from a hard-coded 0.10 (the Warder duck specs).
UCLASS()
class AWoTTestQuietNPCCharacter : public AWoTTestNPCCharacter
{
	GENERATED_BODY()

public:
	AWoTTestQuietNPCCharacter(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SoundVolumeMultiplier = 0.5f;
	}
};

// A captain NPC (the orders-layer archetype): the forced-primitive test brain +
// the bIsCaptain flag that enables the controller's orders block. AssignedOrders
// is left at the KillIntruder default; specs reconfigure via ChangeOrders /
// DispatchOrders.
UCLASS()
class AWoTTestCaptainNPC : public AWoTTestNPCCharacter
{
	GENERATED_BODY()

public:
	AWoTTestCaptainNPC(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		bIsCaptain = true;
	}
};

// A caster NPC (the Sister archetype): no melee, one Fireball weave.
UCLASS()
class AWoTTestCasterNPC : public AWoTTestNPCCharacter
{
	GENERATED_BODY()

public:
	AWoTTestCasterNPC(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		MeleeRange = 0.0f;
		MeleeAttacks.Empty();
		Caster = CreateDefaultSubobject<UWoTCasterComponent>(TEXT("Caster"));
		FWoTWeaveSlot Fireball;
		Fireball.WeaveClass = AWoTAngrealInvFireball::StaticClass();
		Fireball.MinInitialCharges = 50;
		Fireball.MaxInitialCharges = 50;
		Caster->Weaves = {Fireball};
	}

	UPROPERTY()
	TObjectPtr<UWoTCasterComponent> Caster;
};

// Drives the production AWoTNPCController defensive notifier on demand so the
// P1C Sister spec observes both detector pulses without relying on timer phase.
UCLASS()
class AWoTP1CDefensivePulseController : public AWoTNPCController
{
	GENERATED_BODY()
public:
	void PulseDefensiveForP1C()
	{
		OnNotifierPulse(EWoTNotifierSlot::Defensive);
	}
};

// A caster NPC with a single DEFENSIVE weave (a Fire Shield) and no offensive weave
// or melee — the DefensiveReflex archetype. Because the only castable item is the
// shield, any cast the spec observes is attributable to the defensive reflex.
UCLASS()
class AWoTTestDefensiveCasterNPC : public AWoTTestNPCCharacter
{
	GENERATED_BODY()

public:
	AWoTTestDefensiveCasterNPC(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		MeleeRange = 0.0f;
		MeleeAttacks.Empty();
		Caster = CreateDefaultSubobject<UWoTCasterComponent>(TEXT("Caster"));
		FWoTWeaveSlot Shield;
		Shield.WeaveClass = AWoTAngrealInvFireShield::StaticClass();
		Shield.MinInitialCharges = 5;
		Shield.MaxInitialCharges = 5;
		Caster->Weaves = {Shield};
	}

	UPROPERTY()
	TObjectPtr<UWoTCasterComponent> Caster;
};

// A concrete seeker for the Myrddraal-teleport spec: AWoTSeekingProjectile is
// Abstract, so the spec needs a spawnable subclass whose Destination it can point
// at the Myrddraal (the DefensivePerimiterCompromisedByOffender trigger).
UCLASS()
class AWoTTestSeeker : public AWoTSeekingProjectile
{
	GENERATED_BODY()
};

// A concrete IK trailer for the trailer-chain spec: AWoTIKTrailer is Abstract, so the
// spec needs a spawnable joint to build and solve a chain against a movable head.
UCLASS()
class AWoTTestIKTrailer : public AWoTIKTrailer
{
	GENERATED_BODY()
};

// A plain spawnable projectile for the defensive-dodge spec: AWoTProjectile is Abstract,
// so the dodge scan (which collects the whole WotProjectile family) needs a concrete leaf
// whose ProjectileMovement velocity the spec can aim at the dodging pawn.
UCLASS()
class AWoTTestDodgeProjectile : public AWoTProjectile
{
	GENERATED_BODY()
};

// A natural collision/damage witness for moving-projectile geometry specs.
// Projectiles reach the ordinary AActor::TakeDamage virtual through overlap;
// no test-only projectile driver is involved.
UCLASS()
class AWoTTestProjectileCollisionTarget : public AActor
{
	GENERATED_BODY()
public:
	AWoTTestProjectileCollisionTarget()
	{
		Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
		SetRootComponent(Box);
		Box->InitBoxExtent(FVector(0.5f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_Pawn);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetGenerateOverlapEvents(true);
	}
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override
	{
		++DamageCount;
		LastDamage = DamageAmount;
		return DamageAmount;
	}
	UPROPERTY() TObjectPtr<UBoxComponent> Box;
	int32 DamageCount = 0;
	float LastDamage = 0.0f;
};

// Public drivers for WotWeaponProjectile's protected state handlers. Automation
// calls the handlers deterministically rather than depending on Chaos overlap
// ordering in an empty headless world.
UCLASS()
class AWoTTestWeaponProjectile : public AWoTWeaponProjectile
{
	GENERATED_BODY()
public:
	void DriveTouch(AActor* Other) { ProcessTouch(Other, Other->GetActorLocation()); }
	void DriveStick() { StickInSomething(); }
};

// A concrete direct weave for the shield-deflect specs: AWoTGenericProjectile is
// Abstract, so the specs need a spawnable leaf whose ProcessTouch they drive against
// a shielding pawn (the ReturnToSender/deflect reflect path).
UCLASS()
class AWoTTestDeflectProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()
public:
	void DriveTouch(AActor* Other) { ProcessTouch(Other, Other->GetActorLocation()); }
};
