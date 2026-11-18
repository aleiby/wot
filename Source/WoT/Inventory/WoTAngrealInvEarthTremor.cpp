#include "WoTAngrealInvEarthTremor.h"

#include "../Projectiles/WoTEarthTremorProjectile.h"

AWoTAngrealInvEarthTremor::AWoTAngrealInvEarthTremor()
{
	BindStatusIcons(TEXT("I_EarthTremor"), TEXT("M_EarthTremor"));
	// AngrealInvEarthTremor defaultproperties.
	ProjectileClass = AWoTEarthTremorProjectile::StaticClass();
	bElementEarth = true;
	bUncommon = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 90.0f;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 7.0f;
	MaxChargeUsedInterval = 0.67f;
	MinChargeGroupInterval = 4.0f;
	InventoryGroup = 60;
}
