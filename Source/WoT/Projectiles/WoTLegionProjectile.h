#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTLegionProjectile.generated.h"

// Port of WoTPawns.LegionProjectile — the Hound champion's homing spirit head.
// A thin leaf over the shared SeekingProjectile homing/replication base
// (WoTSeekingProjectile), carrying only LegionProjectile.uc's CDO deltas plus
// its shootable "VeryDead" put-down. Spec: exports/script/WoTPawns/LegionProjectile.uc.
//
// Divergences (PORT_NOTES):
//   * LegionProjectile.uc:336 Acceleration=20 (NOT the base 16 the SeekingProjectile
//     CDO carries) and :346 LifeSpan=0 (infinite — a seeker chases forever,
//     matching GetMaxRange=MaxInt). The SeekingProjectile base defaults are
//     16 / 300; the leaf overrides win (.uc is the spec).
//   * Retail seeking navigates the level path-node network; the port's base homes via
//     its own direct/NavMesh steering (already noted on AWoTSeekingProjectile).
// The spin/toss, closing glow, sounds, spirit-tail trail, and velocity-facing mesh
// are client presentation attached below the inherited presentation root.
UCLASS()
class AWoTLegionProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:
	AWoTLegionProjectile();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	UPROPERTY(VisibleAnywhere, Category="WoT|Legion")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere, Category="WoT|Legion")
	TObjectPtr<class UPointLightComponent> SpiritLight;
	UPROPERTY(VisibleAnywhere, Category="WoT|Legion")
	TObjectPtr<class UWoTSpriteBillboardComponent> TailSpriteA;
	UPROPERTY(VisibleAnywhere, Category="WoT|Legion")
	TObjectPtr<class UWoTSpriteBillboardComponent> TailSpriteB;

	// LegionProjectile.uc:41,335 — the spirit head can be shot down (bProjTarget).
	// Server-authoritative; the VeryDead Destroy replicates.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Legion") int32 Health = 30;

	// The put-down dive (LegionProjectile.uc:307): Velocity = vect(100,0,-10) * Normal(Velocity).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Legion") float DiveForward = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Legion") float DiveDown = 10.0f;
	// class'util'.static.Fade(Self, 0.5) — the fade-out then destroy (uc:304).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Legion") float VeryDeadFadeTime = 0.5f;

	bool IsVeryDead() const { return bVeryDead; }

	// LegionProjectile.uc:216 TakeDamage: Health -= Damage; VeryDead at <=0. (function,
	// not simulated — server only.)
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;

	// LegionProjectile.uc:313 TargetDestroyed: the victim died -> put ourselves down so
	// we don't hunt its respawn (Legion.Killed cleanup, uc:469-484). GotoState('VeryDead').
	void TargetDestroyed();

	// Legion.Killed (Legion.uc:469-484): a pawn the Legion's spirits were homing on died —
	// TargetDestroyed every spirit whose Destination is that pawn (across ALL Legions, as
	// retail's AllActors scan) so none hunt the respawn. Called from the game mode's kill
	// dispatch (the Level.Game.Killed seam, WoTGameMode::PlayerKilled).
	static void PutDownSeekersTargeting(const UWorld* World, const AActor* Victim);

	// LegionProjectile.uc VeryDead state (uc:247-309) + ProcessTouch (uc:199) + HitWall
	// (uc:187): a downed spirit never explodes — it dives, fades, and is destroyed on
	// landing; a live spirit destroys (no explosion) on any non-pawn/world contact.
	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;

private:
	// state VeryDead (uc:247): stop seeking, dive, fade, destroy. Ignores further
	// ProcessDamage/TakeDamage (uc:249).
	void GotoVeryDead();

	bool bVeryDead = false;
	float TotalRollUnits = 0.0f;
	float CurrentSpinUnits = 2048.0f;
	int32 SpinDirection = 1;
	float RotationRangeUnits = 12288.0f;
	bool bClosingEffect = false;
	float ClosingUntil = 0.0f;
	float TailClock = 0.0f;
};
