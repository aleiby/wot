#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTTracerProjectile.generated.h"

class AWoTSeal;

// Port of Angreal.AngrealTracerProjectile (exports/script/Angreal/
// AngrealTracerProjectile.uc): a SeekingProjectile that homes on the nearest
// Seal, following it as it is picked up or dropped, and leaves a fiery trail.
// bInfo/non-damaging — it just reaches the seal and fizzles.
//
// Spec defaults: Acceleration 150, Speed 1, MaxSpeed 500, bDesperateSeeker,
// !bTakeShortcuts, ReachTolerance 20, !bNotifiesDestination, LifeSpan 0,
// collision 16x16.
//
// The TracerSprayer/LoopTR presentation and the original accelerating
// SetRotation(Rotation+Spin) roll are live on the projectile.
UCLASS()
class AWoTTracerProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The dust-mote trail (6a-iii): follows the eye; expiry leaves the motes
	// hanging 15s (their authored LifeSpan) with emission stopped.
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Trail;
	virtual void Destroyed() override;

	// The tracer eye (AngrealTracerProjectile.uc: Mesh Tracer, LoopAnim 'Tracer'
	// at 0.1; 6a-ii).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	virtual void BeginPlay() override;
	AWoTTracerProjectile();

	// The seal we are currently tracking (may be carried by a pawn or lying in
	// the world). Tracked so we can re-follow it across pickup/drop.
	UPROPERTY() TObjectPtr<AWoTSeal> TrackingSeal;

	// SpinRate=(Roll=65000) in the original (UE1 rotator units/sec; 65536 = 360).
	UPROPERTY(VisibleAnywhere, Category = "WoT|Tracer") float SpinAccelerationRollDegPerSec2 = 0.0f;
	float GetSpinRollDegPerSec() const { return SpinRollDegPerSec; }

	virtual void SetDestination(AActor* Dest) override;
	virtual void NotifySeekerBounced(AWoTPathNode* OnNode) override;
	virtual void Tick(float DeltaTime) override;

private:
	// Seal.uc 'Pickup'-state re-targeting, the original's 1Hz Timer(): keep the
	// destination pointed at the seal whether it is carried or dropped.
	void RetargetSeal();
	float RetargetTimer = 0.0f;
	float SpinRollDegPerSec = 0.0f;

	static AWoTSeal* FindCarriedSeal(AActor* PawnActor);
};
