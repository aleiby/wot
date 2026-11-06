#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTProjectileLauncher.generated.h"

class AWoTGenericProjectile;

// Port of Angreal.ProjectileLauncher — the artifact that launches any
// GenericProjectile subclass. Spec: exports/script/Angreal/
// ProjectileLauncher.uc. LaunchProjectileEffect.Invoke (the Invokable
// launch effect) is folded into LaunchProjectile per the deferred-effects
// convention (PORT_NOTES); DefaultTargetingReflector.FindBestTarget is
// folded into GetBestTarget for the same reason. ProjectileClassName's
// DynamicLoadObject decoupling collapses into a direct TSubclassOf (all
// classes live in one C++ module now).
UCLASS(Abstract)
class AWoTProjectileLauncher : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTProjectileLauncher();

	// Repeats casts for as long as input remains held (ProjectileLauncher.uc:32).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launcher") bool bAutoFire = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launcher")
	TSubclassOf<AWoTGenericProjectile> ProjectileClass;

	bool bCasting = false;         // Tracks an active use cycle (ProjectileLauncher.uc:42).
	float NextCastTime = 0.0f;     // Earliest permitted follow-up cast (ProjectileLauncher.uc:45).
	UPROPERTY() TObjectPtr<AActor> BestTarget;   // Most recently selected target (ProjectileLauncher.uc:48).

	virtual void CastArtifact() override;     // Cast()
	virtual void UnCastArtifact() override;   // UnCast()
	virtual void Failed() override;
	virtual void Reset() override;
	virtual void Tick(float DeltaTime) override;

	void AttemptCast();
	virtual void LaunchProjectile();
	virtual AActor* GetBestTarget();
	// FindBestTarget / IsTargetable now live on AWoTAngrealInventory (the base)
	// so non-launcher artifacts can target; inherited unchanged.

	// ---- AI support ----
	virtual bool IsLeadable() override;
	virtual float GetLeadSpeed() override;
	virtual FRotator GetBestTrajectory() override;
	virtual float GetMaxRange() override;
	virtual float GetMinRange() override;
};
