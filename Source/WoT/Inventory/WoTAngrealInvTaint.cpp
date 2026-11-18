#include "WoTAngrealInvTaint.h"

#include "../Projectiles/WoTTaintProjectile.h"

AWoTAngrealInvTaint::AWoTAngrealInvTaint()
{
	BindStatusIcons(TEXT("I_Taint"), TEXT("M_Taint"));
	// AngrealInvTaint defaultproperties
	ProjectileClass = AWoTTaintProjectile::StaticClass();
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bRare = true;
	bOffensive = true;
	bCombat = true;
	MaxInitialCharges = 2;
	MaxCharges = 10;
	bTargetsFriendlies = false;
	MinChargeGroupInterval = 8.0f;
	InventoryGroup = 63;
}
