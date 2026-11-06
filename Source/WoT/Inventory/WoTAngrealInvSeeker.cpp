#include "WoTAngrealInvSeeker.h"

#include "../Projectiles/WoTSeekerProjectile.h"

AWoTAngrealInvSeeker::AWoTAngrealInvSeeker()
{
	BindStatusIcons(TEXT("I_Seeker"), TEXT("M_Seeker"));
	// AngrealInvSeeker defaultproperties
	ProjectileClass = AWoTSeekerProjectile::StaticClass();
	bElementFire = true;
	bElementAir = true;
	bUncommon = true;
	bOffensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 10;
	MaxCharges = 20;
	Priority = 2.0f;
	bTargetsFriendlies = false;
	MaxChargesInGroup = 3;
	MinChargeGroupInterval = 5.0f;
	InventoryGroup = 52;
}
