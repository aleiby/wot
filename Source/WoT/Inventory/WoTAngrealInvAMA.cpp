#include "WoTAngrealInvAMA.h"

#include "../Projectiles/WoTAngrealAMAProjectile.h"

AWoTAngrealInvAMA::AWoTAngrealInvAMA()
{
	BindStatusIcons(TEXT("I_AntiMagicAura"), TEXT("M_AntiMagicAura"));
	// AngrealInvAMA defaultproperties.
	ProjectileClass = AWoTAngrealAMAProjectile::StaticClass();
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bUncommon = true;
	bDefensive = true;
	MaxInitialCharges = 3;
	MaxCharges = 5;
	MinChargeGroupInterval = 4.0f;
	InventoryGroup = 67;
	// DurationType=DT_Lifespan — the effect-duration calc is deferred (the dome's
	// own Duration governs it here).
}
