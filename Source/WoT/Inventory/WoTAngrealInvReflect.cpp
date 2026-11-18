#include "WoTAngrealInvReflect.h"

#include "WoTReturnToSenderReflector.h"

AWoTAngrealInvReflect::AWoTAngrealInvReflect()
{
	BindStatusIcons(TEXT("I_Reflect"), TEXT("M_Reflect"));
	// AngrealInvReflect defaultproperties
	Duration = 5.0f;
	DurationType = EWoTDurationType::DT_Lifespan;
	ReflectorClasses = { UWoTReturnToSenderReflector::StaticClass() };
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
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateRF.ActivateRF"));
	MaxChargesInGroup = 2;
	MinChargeGroupInterval = 3.0f;   // AngrealInvReflect.uc:120
}
