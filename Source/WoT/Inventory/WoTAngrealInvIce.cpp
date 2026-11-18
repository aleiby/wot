#include "WoTAngrealInvIce.h"

#include "../Projectiles/WoTIceProjectile.h"

AWoTAngrealInvIce::AWoTAngrealInvIce()
{
	BindStatusIcons(TEXT("I_Freeze"), TEXT("M_Freeze"));
	// AngrealInvIce defaultproperties
	ProjectileClass = AWoTIceProjectile::StaticClass();
	DurationType = EWoTDurationType::DT_Lifespan;
	bElementWater = true;
	bUncommon = true;
	bDefensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	bTargetsFriendlies = false;
	MinChargeGroupInterval = 4.0f;
}
