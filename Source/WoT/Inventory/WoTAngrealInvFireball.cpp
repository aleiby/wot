#include "WoTAngrealInvFireball.h"

#include "../Projectiles/WoTFireballProjectile.h"

AWoTAngrealInvFireball::AWoTAngrealInvFireball()
{
	BindStatusIcons(TEXT("I_Fireball"), TEXT("M_Fireball"));
	// AngrealInvFireball defaultproperties
	ProjectileClass = AWoTFireballProjectile::StaticClass();
	bElementFire = true;
	bCommon = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 90.0f;
	MinInitialCharges = 10;
	MaxInitialCharges = 20;
	MaxCharges = 35;
	Priority = 9.0f;
	MaxChargesInGroup = 6;
	MinChargesInGroup = 3;
	MaxChargeUsedInterval = 0.67f;
	InventoryGroup = 51;
}
