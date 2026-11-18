#pragma once

#include "CoreMinimal.h"
#include "WoTWeaponProjectile.h"
#include "WoTArcherArrow.generated.h"

// The Whitecloak Archer's thin leaf over the shared WOT.WotWeaponProjectile
// layer. G19 extracted the base once the bolt/axe/halberd/shield leaves landed;
// this class now carries only ArcherArrow.uc's CDO values.
// Spec: exports/script/WoTPawns/ArcherArrow.uc + exports/script/WOT/WotWeaponProjectile.uc.
//
// Unlike the angreal weaves this is NOT an AWoTAngrealProjectile — the reflect/
// absorb/fork chain gates on IsA('AngrealProjectile') (ReturnToSenderReflector.uc:
// 207), so an arrow is never intercepted: its ProcessTouch does a plain 'shot'
// TakeDamage (WotWeaponProjectile.uc:178).
UCLASS()
class AWoTArcherArrow : public AWoTWeaponProjectile
{
	GENERATED_BODY()

public:
	AWoTArcherArrow();

protected:
	virtual void PlayExplodeCosmetics(const FVector& HitLocation,
		const FVector& HitNormal) override;

};
