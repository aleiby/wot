#include "WoTAngrealInvFireworks.h"

#include "../Projectiles/WoTFireworksProjectile.h"

AWoTAngrealInvFireworks::AWoTAngrealInvFireworks()
{
	BindStatusIcons(TEXT("I_Fireball"), TEXT("M_Fireball"));
	// AngrealInvFireworks defaultproperties (text: Angreal.int [AngrealInvFireworks],
	// whose empty Description/Quote leave the class's own "None" standing).
	ProjectileClass = AWoTFireworksProjectile::StaticClass();
	bElementFire = true;
	bCommon = true;
	bOffensive = true;
	bCombat = true;
	MinInitialCharges = 10;
	MaxInitialCharges = 20;
	InventoryGroup = 51;
}
