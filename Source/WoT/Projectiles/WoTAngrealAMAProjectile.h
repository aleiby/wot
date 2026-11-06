#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "WoTAngrealAMAProjectile.generated.h"

// Port of Angreal.AngrealAMAProjectile (expands GenericProjectile) — the Aura of
// Unraveling's delivery: a lobbed land-mine. Spec:
// exports/script/Angreal/AngrealAMAProjectile.uc.
//
// It falls (PHYS_Falling), and on landing PLACES itself (does not explode), then
// runs Priming -> Waiting. Tripping is the retail GLOBAL Touch
// (AngrealAMAProjectile.uc:33-43): any pawn or projectile that touches the mine's
// own 12uu collision, in any state, plants a full-size AWoTAMAPearl dome where
// the mine sits. The 150uu TriggerRadius is never applied: it lives in
// Waiting.BeginPlay (:111-114), and BeginPlay is a spawn-time event that never
// runs after GotoState('Waiting'), so the shipped mine keeps CollisionRadius 12.
// A spawn-time overlap (the caster the mine is launched from) is not a touch:
// UE1 sends no Touch at spawn and a cylinder exit is not a hit.
//
// The pickup mesh, landing alignment, Fading dissolve, and sounds are bound.
UCLASS()
class AWoTAngrealAMAProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:

	// The aura pearl ring (AngrealAMAProjectile.uc:178 binds the PICKUP mesh
	// AngrealAntiMagicAuraPickup, as shipped; 6a-ii).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	virtual void BeginPlay() override;
	AWoTAngrealAMAProjectile();

	// How close a pawn must get to trip a placed, armed mine.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float TriggerRadius = 150.0f;
	// How long the mine sits inert after placement before it can be tripped.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float PrimeTime = 0.0f;

	// Land-and-place instead of exploding; arm after priming; plant the dome.
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;
	// AngrealAMAProjectile.uc:49 declares ProcessTouch as an empty no-op: the
	// mine can't be absorbed/reflected/forked and never explodes on touch — only
	// the global Touch below reacts.
	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override {}
	// AngrealAMAProjectile.uc:33-43 (global, authority): a Pawn or Projectile
	// touching the mine plants the dome. Driven by the mine's own collision.
	void Touch(AActor* Other);
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Test seam / internal: freeze in place and arm after PrimeTime.
	void Place(const FVector& HitNormal = FVector::UpVector);
	bool IsPlaced() const { return bPlaced; }
	bool IsArmed() const { return bArmed; }
	bool IsFading() const { return bFading; }

private:
	UFUNCTION() void Arm();
	void PlantDome();
	UFUNCTION() void OnMineBeginOverlap(UPrimitiveComponent* Comp, AActor* Other,
		UPrimitiveComponent* OtherComp, int32 BodyIndex, bool bFromSweep,
		const FHitResult& Hit);
	UFUNCTION() void OnMineEndOverlap(UPrimitiveComponent* Comp, AActor* Other,
		UPrimitiveComponent* OtherComp, int32 BodyIndex);
	// Actors already overlapping the mine when it spawned (measured in BeginPlay,
	// before UpdateInitialOverlaps reports them): never a touch until they have
	// left and come back.
	TSet<TWeakObjectPtr<AActor>> SpawnOverlaps;
	UFUNCTION() void OnRep_Fading();
	void BeginFadingPresentation();

	bool bPlaced = false;
	bool bArmed = false;
	bool bTriggered = false;   // one dome per mine (guard against multi-pawn polls)
	UPROPERTY(ReplicatedUsing = OnRep_Fading) bool bFading = false;
	FTimerHandle PrimeTimer;
};
