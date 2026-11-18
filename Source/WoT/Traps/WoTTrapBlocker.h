#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTTrapBlocker.generated.h"

class UCapsuleComponent;

// Port of WoTTraps/TrapBlocker.uc (a Keypoint) — the Spear's collision proxy. An invisible cylinder
// spaced along the spear's reach that forwards its contacts to the owning trap: a non-damaging
// blocker relays the touch (the trigger), a damaging blocker calls the owner's DamageActor. The
// original's NoDamage/CanDamage states collapse to a single bDamage flag (shared-enum FSM would be
// overkill for a two-way branch). Both original states redirect Bump → Touch, so a BLOCKING
// contact (the extended shaft) delivers exactly like an overlap — NotifyHit is that Bump path
// here. Placed + driven by the Spear (Slice 3); no replication of its own.
UCLASS()
class AWoTTrapBlocker : public AActor
{
	GENERATED_BODY()

public:
	AWoTTrapBlocker();

	// TrapBlocker.SetBlocking: resize (pass -1 radius/height to skip), set collision, and pick the
	// damage vs relay behavior. Mirrors the original's SetCollisionSize + SetCollision + GotoState.
	// bBlock maps to a Block response (the shape default overlaps everything — an enabled-physics
	// state alone never stops a pawn).
	void SetBlocking(bool bCollide, bool bBlock, bool bDamage,
	                 float MyCollisionRadius, float MyCollisionHeight);

	// The overlap entry (public so a spec can drive it directly, like AWoTTrigger::OnOverlap — a
	// headless overlap is awkward to synthesize). Forwards to the owning AWoTTrap: DamageActor when
	// damaging, else TrapTouch.
	UFUNCTION()
	void OnBlockerOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                      UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                      bool bFromSweep, const FHitResult& Hit);

	// UE1 Bump — a pawn pressing against a BLOCKING blocker. Both TrapBlocker.uc states
	// redirect Bump into Touch, so this routes identically to the overlap.
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;

private:
	// The shared routing (overlap / hit): DamageActor when damaging, else the
	// trap's trigger touch.
	void Dispatch(AActor* Other);

	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<UCapsuleComponent> Capsule;

	// The CanDamage (true) vs NoDamage (false) branch.
	bool bDamage = false;
};
