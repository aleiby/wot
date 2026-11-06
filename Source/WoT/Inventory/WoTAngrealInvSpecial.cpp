#include "WoTAngrealInvSpecial.h"

AWoTAngrealInvSpecial::AWoTAngrealInvSpecial()
{
	BindStatusIcons(TEXT("I_Special"), TEXT("M_Special"));
	// AngrealInvSpecial defaultproperties
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bRare = true;
	ChargeCost = 0;
	InventoryGroup = 65;
}
