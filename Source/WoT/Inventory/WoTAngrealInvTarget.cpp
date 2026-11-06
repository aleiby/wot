#include "WoTAngrealInvTarget.h"

#include "WoTAutoTargetReflector.h"

AWoTAngrealInvTarget::AWoTAngrealInvTarget()
{
	BindStatusIcons(TEXT("I_Targeting"), TEXT("M_Targeting"));
	// AngrealInvTarget defaultproperties
	Duration = 15.0f;
	DurationType = EWoTDurationType::DT_Lifespan;
	ReflectorClasses = { UWoTAutoTargetReflector::StaticClass() };
	bElementAir = true;
	bElementSpirit = true;
	bRare = true;
	bOffensive = true;
	bCombat = true;
	MinInitialCharges = 1;
	MaxInitialCharges = 2;
	MaxCharges = 5;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateTA.ActivateTA"));
	MinChargeGroupInterval = 5.0f;   // AngrealInvTarget.uc:41
	InventoryGroup = 66;
}
