#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvAirBurst.generated.h"

// Port of Angreal.AngrealInvAirBurst — the Air Pulse ter'angreal. Spec:
// exports/script/Angreal/AngrealInvAirBurst.uc. A rapid-fire (RoundsPerMinute
// 120) melee-range weave: each cast sweeps a short beam of air in front of the
// caster (RangeX x RangeY box), dealing distance-scaled damage and knocking back
// authored pushable decorations caught in it. The authored target list is preserved, while
// only carcasses and bounceable/breakable decorations receive the push.
//
// Presentation: the ActivateAB cast sound and the AirBlastProj/AirBlastBall
// flying ripple (AWoTAirBlastFX) are bound. The self-Kickback recoil is
// commented OUT in the shipped Cast() (AngrealInvAirBurst.uc:71) — no recoil is
// parity. SprayerBlower (:114-116) now applies its class-default 1.0 s impulse.
// It acts on nearby local world particle plumes through their gravity seat.
// The commented-out AirBurstSprayer/AirBurstVisual "NEW" block remains absent; it
// never shipped.
UCLASS()
class AWoTAngrealInvAirBurst : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvAirBurst();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float RangeX = 120.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float RangeY = 20.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float MaxDamage = 20.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float MinDamage = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float MomentumTransfer = 20000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") float LargestCollisionRadius = 250.0f;
	// Offset of the blast effect from the caster, UNROTATED (the shipped spawn is
	// Owner.Location + EffectOffset, AngrealInvAirBurst.uc:121,351).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AirBurst") FVector EffectOffset = FVector(0, 0, 20);

	virtual void CastArtifact() override;   // Cast()
	virtual float GetMaxRange() override { return RangeX; }
	// IsAffectable (AngrealInvAirBurst.uc:286-307): production class-family
	// selector shared by Burst and parity callers.
	bool IsAffectable(const AActor* Actor) const;

private:
	void Burst();
	float NextCastTime = 0.0f;
};
