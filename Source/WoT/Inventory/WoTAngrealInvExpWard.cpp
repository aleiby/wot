#include "WoTAngrealInvExpWard.h"

#include "../Projectiles/WoTExpWardProjectile.h"

AWoTAngrealInvExpWard::AWoTAngrealInvExpWard()
{
	BindStatusIcons(TEXT("I_LandMine"), TEXT("M_LandMine"));
	// AngrealInvExpWard defaultproperties
	ProjectileClass = AWoTExpWardProjectile::StaticClass();
	bElementFire = true;
	bElementEarth = true;
	bCommon = true;
	bTraps = true;
	RoundsPerMinute = 240.0f;
	MinInitialCharges = 5;
	MaxInitialCharges = 10;
	MaxCharges = 25;
	MaxChargesInGroup = 5;
	MinChargesInGroup = 3;
	MaxChargeUsedInterval = 0.25f;
	MinChargeGroupInterval = 10.0f;
	InventoryGroup = 58;
}
