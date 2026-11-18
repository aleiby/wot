#include "WoTAngrealInvDecay.h"

#include "../Projectiles/WoTDecayProjectile.h"

AWoTAngrealInvDecay::AWoTAngrealInvDecay()
{
	BindStatusIcons(TEXT("I_Decay"), TEXT("M_Decay"));
	// AngrealInvDecay defaultproperties
	ProjectileClass = AWoTDecayProjectile::StaticClass();
	DurationType = EWoTDurationType::DT_Lifespan;
	bElementWater = true;
	bElementSpirit = true;
	bRare = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 30.0f;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 3.0f;
	bTargetsFriendlies = false;
	MaxChargeUsedInterval = 2.0f;
	MinChargeGroupInterval = 4.0f;
	InventoryGroup = 57;
}
