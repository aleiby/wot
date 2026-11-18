#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTShieldReflector.generated.h"

// Port of Angreal.ShieldReflector (expands Reflector) — the Personal Shield's
// effect. Spec: exports/script/Angreal/ShieldReflector.uc. A hit-point shield:
// each hit splits its damage, sending ShieldReduceDamagePct to a ShieldHitPoints
// pool and the rest to the player; when the pool is exhausted the overflow
// spills to the player and the shield breaks (uninstalls). No time limit — it's
// worn away by damage (the launcher installs it with Duration 0 = permanent).
//
// ShieldHitPoints lives on the reflector (the original kept it on the pawn for
// the HUD); GetDuration exposes that folded pool to the status countdown.
// Absorbed/reduced hits emit ShieldDeflectSound.
//
// Recast over a worn shield: retail Install fills the PAWN's pool to 50 and only
// then runs Reflector.Install's bRemoveExisting, whose UnInstall of the old
// shield zeroes that same pawn pool (ShieldReflector.uc:33-44, :61-70,
// Reflector.uc:233-245). The new shield is left with 0 points: it absorbs
// nothing and breaks on the next hit. Install reproduces that ordering.
UCLASS()
class UWoTShieldReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTShieldReflector();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") int32 ShieldReduceDamagePct = 50;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") int32 MaxShieldHitPoints = 50;
	UPROPERTY(Replicated) int32 ShieldHitPoints = 50;

	virtual void Install(APawn* NewHost) override;
	virtual int32 ModifyDamage(int32 Damage, FName WoTDamageType) override;
	virtual float GetDuration() const override
	{
		return static_cast<float>(ShieldHitPoints);
	}
	virtual bool StopsDamageChain(int32 /*DamageAfterMe*/) const override
	{
		return bStoppedLastDamage;
	}
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// Alternate which side gets the rounding remainder (evens out over hits).
	bool bCalcOrder = false;
	bool bStoppedLastDamage = false;
};
