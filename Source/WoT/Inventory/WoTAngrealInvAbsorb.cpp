#include "WoTAngrealInvAbsorb.h"

#include "WoTAbsorbReflector.h"

AWoTAngrealInvAbsorb::AWoTAngrealInvAbsorb()
{
	BindStatusIcons(TEXT("I_Absorb"), TEXT("M_Absorb"));
	// AngrealInvAbsorb defaultproperties
	Duration = 8.0f;
	DurationType = EWoTDurationType::DT_Lifespan;
	ReflectorClasses = { UWoTAbsorbReflector::StaticClass() };
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bRare = true;
	bDefensive = true;   // AngrealInvAbsorb.uc:41-42 (AI classification)
	bCombat = true;
	MaxInitialCharges = 2;
	MaxCharges = 5;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateABS.ActivateABS"));
	MinChargeGroupInterval = 7.0f;   // AngrealInvAbsorb.uc:46
	InventoryGroup = 50;
}
