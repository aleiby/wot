#include "WoTAngrealInvSwapPlaces.h"

#include "../Projectiles/WoTSwapPlacesProjectile.h"

AWoTAngrealInvSwapPlaces::AWoTAngrealInvSwapPlaces()
{
	BindStatusIcons(TEXT("I_SwapPlaces"), TEXT("M_SwapPlaces"));
	// AngrealInvSwapPlaces defaultproperties.
	ProjectileClass = AWoTSwapPlacesProjectile::StaticClass();
	bElementSpirit = true;
	bUncommon = true;
	bDefensive = true;
	MinInitialCharges = 2;
	MaxInitialCharges = 4;
	MaxCharges = 10;
	// NonTargetableTypes MashadarTrailer / AngrealIllusionProjectile. The native
	// Personal Illusion class now makes the second retail name live; Mashadar's
	// trailer remains a harmless unported nonmatch.
	NonTargetableTypes[0] = TEXT("MashadarTrailer");
	NonTargetableTypes[1] = TEXT("AngrealIllusionProjectile");
	MaxChargesInGroup = 3;
	MinChargeGroupInterval = 3.0f;
	InventoryGroup = 99;
}
