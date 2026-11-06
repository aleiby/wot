#include "WoTAngrealInvSoulBarb.h"

#include "../Projectiles/WoTSoulBarbProjectile.h"

AWoTAngrealInvSoulBarb::AWoTAngrealInvSoulBarb()
{
	BindStatusIcons(TEXT("I_SoulBarb"), TEXT("M_SoulBarb"));
	// AngrealInvSoulBarb defaultproperties
	ProjectileClass = AWoTSoulBarbProjectile::StaticClass();
	DurationType = EWoTDurationType::DT_Lifespan;
	bElementSpirit = true;
	bCommon = true;
	bOffensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 9.0f;
	bTargetsFriendlies = false;
	MinChargeGroupInterval = 20.0f;
	InventoryGroup = 56;
}
