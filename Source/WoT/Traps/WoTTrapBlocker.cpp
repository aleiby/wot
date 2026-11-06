#include "WoTTrapBlocker.h"

#include "WoTTrap.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

AWoTTrapBlocker::AWoTTrapBlocker()
{
	PrimaryActorTick.bCanEverTick = false;

	// TrapBlocker extends Keypoint (bHidden=True default; TrapBlocker.uc never shows it) —
	// a pure sensing capsule with no visual. Staying hidden also keeps blockers out of the
	// occupancy filter Trap.AnyActorsInArea applies (`!A.bHidden`, Trap.uc:91), so a
	// deployed spear's reach never reads as an occupied area to later placements.
	SetActorHiddenInGame(true);

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	SetRootComponent(Capsule);
	Capsule->InitCapsuleSize(4.0f, 4.0f);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetGenerateOverlapEvents(true);
	Capsule->SetNotifyRigidBodyCollision(true);   // the Bump path (blocking contact)
	Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTTrapBlocker::OnBlockerOverlap);
}

void AWoTTrapBlocker::SetBlocking(bool bCollide, bool bBlock, bool bDamage_,
                                  float MyCollisionRadius, float MyCollisionHeight)
{
	// TrapBlocker.SetBlocking (TrapBlocker.uc:21-36): pass -1 radius/height to skip the resize.
	if (MyCollisionRadius >= 0.0f && MyCollisionHeight >= 0.0f)
	{
		Capsule->SetCapsuleSize(MyCollisionRadius, MyCollisionHeight);
	}
	// The behavior branch first, so any overlap event fired by the collision change below
	// dispatches with the fresh state.
	bDamage = bDamage_;
	// UE1 SetCollision(bCollide, bBlock, bBlock): the blocker either queries (overlap) or is a
	// real barrier. Blocking needs the channel RESPONSE too — the shape default overlaps
	// everything, and QueryAndPhysics alone never stops a pawn.
	Capsule->SetCollisionResponseToAllChannels(bBlock ? ECR_Block : ECR_Overlap);
	Capsule->SetCollisionEnabled(
		bCollide ? (bBlock ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly)
		         : ECollisionEnabled::NoCollision);
}

void AWoTTrapBlocker::OnBlockerOverlap(UPrimitiveComponent* /*Comp*/, AActor* Other,
                                       UPrimitiveComponent* /*OtherComp*/, int32 /*BodyIndex*/,
                                       bool /*bFromSweep*/, const FHitResult& /*Hit*/)
{
	Dispatch(Other);
}

void AWoTTrapBlocker::NotifyHit(UPrimitiveComponent*, AActor* Other,
                                UPrimitiveComponent*, bool, FVector, FVector,
                                FVector, const FHitResult&)
{
	// TrapBlocker.uc: both states' Bump redirects into Touch — a pawn pressing against the
	// blocking shaft delivers like an overlap.
	Dispatch(Other);
}

void AWoTTrapBlocker::Dispatch(AActor* Other)
{
	// CanDamage.Touch → Trap(Owner).DamageActor; NoDamage.Touch → Owner.Touch (the trap's trigger).
	AWoTTrap* OwnerTrap = Cast<AWoTTrap>(GetOwner());
	if (!OwnerTrap || Other == this || Other == OwnerTrap)
	{
		return;
	}
	if (bDamage)
	{
		OwnerTrap->DamageActor(Other);
	}
	else
	{
		OwnerTrap->TrapTouch(Other);
	}
}
