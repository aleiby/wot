#include "WoTAngrealInvFork.h"

#include "WoTForkRTSReflector.h"

AWoTAngrealInvFork::AWoTAngrealInvFork()
{
	BindStatusIcons(TEXT("I_Fork"), TEXT("M_Fork"));
	// AngrealInvFork defaultproperties
	Duration = 20.0f;
	DurationType = EWoTDurationType::DT_Lifespan;
	ReflectorClasses = { UWoTForkRTSReflector::StaticClass() };
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bRare = true;
	bDefensive = true;
	bCombat = true;
	MaxInitialCharges = 3;
	MaxCharges = 5;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateFK.ActivateFK"));
	MaxChargesInGroup = 3;
	MinChargeGroupInterval = 3.0f;   // AngrealInvFork.uc:94
}
