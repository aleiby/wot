#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTAbsorbReflector.generated.h"

// Port of Angreal.AbsorbReflector (expands Reflector) — the Absorb effect. Spec:
// exports/script/Angreal/AbsorbReflector.uc. A one-shot: the next weave that
// strikes the host has its firing artifact snatched out of the attacker's grasp
// and placed in the host's inventory (with the spent charge refunded), and the
// hit is cancelled. The projectile-hit path (NotifyHitByAngrealProjectile) is
// folded into AWoTGenericProjectile::ProcessTouch via OnHitByProjectile.
//
// The effect also consumes deleterious non-projectile ProcessEffects and, at
// install time, the first removable deleterious non-projectile leech/reflector.
// NPC sources keep their artifact while the host gets a fresh class-default copy.
// Deferred presentation rider: the AbsorbVisual mesh ghost.
UCLASS()
class UWoTAbsorbReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTAbsorbReflector();
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Absorb") FName NonStealableTypes[2];
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Absorb") FName UnAffectedProjs[7];
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<class USoundBase> InvokeSound;

	virtual void Install(APawn* NewHost) override;
	virtual bool ConsumesEffect(const FWoTDamageEffectSpec& Effect) override;
	virtual bool OnHitByProjectile(class AWoTAngrealProjectile* Proj) override;
	bool IsStealable(const class AWoTAngrealInventory* A) const;
	bool IsAffectable(const AActor* Other) const;
	virtual bool IsProjectileAffectable(const AActor* Proj) const override
	{
		return IsAffectable(Proj);
	}
	virtual bool BuildTouchReflectorDescription(
		FWoTTouchReflectorDescription& Out) const override;

private:
	bool StealArtifact(class AWoTAngrealInventory* A);
	void DoCoolEffect() const;
};
