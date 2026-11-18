#include "WoTAngrealInvChampion.h"

AWoTAngrealInvChampion::AWoTAngrealInvChampion()
{
	BindStatusIcons(TEXT("I_Champion"), TEXT("M_Champion"));
	// AngrealInvChampion defaultproperties (AngrealInvChampion.uc:37-56).
	// AngrealInvChampion.uc:40-41 summon sparkles.
	TopSparkle = TEXT("ZGreenCorona");
	BottomSparkle = TEXT("AYellowCorona");
	bElementSpirit = true;
	bRare = true;
	MaxCharges = 5;
	MinChargeGroupInterval = 10.0f;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateCH.ActivateCH"));
}
