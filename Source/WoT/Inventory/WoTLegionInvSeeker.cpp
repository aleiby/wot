#include "WoTLegionInvSeeker.h"

#include "../Projectiles/WoTLegionProjectile.h"

AWoTLegionInvSeeker::AWoTLegionInvSeeker()
{
	// LegionInvSeeker.uc defaultproperties.
	ProjectileClass = AWoTLegionProjectile::StaticClass();   // "WOTPawns.LegionProjectile"
	bElementSpirit = true;
	bUncommon = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 120.0f;
	ChargeCost = 0;                    // infinite use — never drops CurCharges
	NPCRespawnTime = 0.0f;
	MaxChargeUsedInterval = 0.1f;
	MinChargeGroupInterval = 0.0f;
	// Priority = 1.0 is the ProjectileLauncher default (base constructor).
	InventoryGroup = 52;
}
