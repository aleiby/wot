#pragma once

#include "CoreMinimal.h"
#include "../Projectiles/WoTProjectile.h"
#include "WoTTestWallProjectile.generated.h"

// Test-only concrete non-angreal WoTProjectile. Stands in for the unported
// WotWeaponProjectile so the Wall of Air knock-down branch (an incoming
// WOTProjectile that is NOT an AngrealProjectile is knocked to the ground, rather
// than unravelled) can be exercised end-to-end. Inert in play.
UCLASS()
class AWoTTestKnockdownProjectile : public AWoTProjectile
{
	GENERATED_BODY()
};
