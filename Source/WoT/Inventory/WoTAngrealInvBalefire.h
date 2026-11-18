#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvBalefire.generated.h"

// Port of Angreal.AngrealInvBalefire (expands AngrealInventory) — the Balefire
// ter'angreal: a hold-to-charge, release-to-fire hitscan "bar of white light"
// that wipes what it touches from the timeline. Spec:
// exports/script/Angreal/AngrealInvBalefire.uc.
//
// Hold-to-charge maps onto the existing press->CastArtifact / release->
// UnCastArtifact input path (WoTCharacter Fire binding). On release the weave
// fires instantly along the caster's view for StreakLength, striking every pawn
// whose body lies within the beam cylinder (DidIndeedHit — in front, within the
// victim's capsule half-extents). Damage and caster recoil scale with charge:
// Scalar = min(held / PrimeTime, 1). Released before half-prime, it auto-finishes
// to half charge after the remaining time (the UnrealScript FinishCharging state
// -> a timer). The beam pierces everything (no wall stop, no far cap) — parity
// with the original's infinite-forward cylinder test.
//
// Balefire "breaks all rules": pawn damage goes through
// AWoTCharacter::TakeRawDamage, bypassing only ProcessEffect interception. The
// complete TakeDamage reflector fold still runs, so Personal Shield can blunt it.
// The damage type is the literal name 'Balefire' (not an AEFWS element combo).
//
// Presentation binds BalefireCorona, BalefireLine, decals, HitBF, and the
// timeline-removal burst. Extant non-pawn targets are handled; LegionProjectile
// remains absent and fenced in PORT_NOTES. Victim/caster momentum is preserved.
UCLASS()
class AWoTAngrealInvBalefire : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvBalefire();

	virtual void CastArtifact() override;    // Cast(): begin charging
	virtual void UnCastArtifact() override;  // UnCast(): release / fire
	virtual void Reset() override;
	virtual void Failed() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// defaultproperties (simulation subset).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Balefire") float BalefireDamage = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Balefire") float PrimeTime = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Balefire") float StreakLength = 8000.0f;

	// PlaceDecals (6a-iv): the 500uu-stepped scorch walk along the fired beam.
	void PlaceBeamDecals(FVector From, const FVector& Direction, float Limit);
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Balefire") float Kickback = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Balefire") float VisualOffset = 50.0f;
	UPROPERTY(ReplicatedUsing = OnRep_Casting) bool bCasting = false;
	UPROPERTY(Transient) TObjectPtr<class AWoTBalefireCorona> GatherEffect;
	UFUNCTION() void OnRep_Casting();

private:
	// FinishCharging state: re-enter UnCastArtifact once the prime completes.
	void FinishCharging();
	// The actual hitscan: recoil + cylinder sweep + charge spend, scaled by charge.
	void Fire(float Scalar);
	void ReconcileCastingPresentation();
	void SweepBalefiredCarcasses();

	float CastTime = 0.0f;
	bool bCheckForCarcasses = false;
	FTimerHandle FinishChargingTimer;
};
