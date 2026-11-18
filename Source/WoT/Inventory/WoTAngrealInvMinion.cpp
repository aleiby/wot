#include "WoTAngrealInvMinion.h"

AWoTAngrealInvMinion::AWoTAngrealInvMinion()
{
	BindStatusIcons(TEXT("I_Minion"), TEXT("M_Minion"));
	// AngrealInvMinion defaultproperties (AngrealInvMinion.uc:37-56). Mesh/texture/icon
	// bindings are presentation (deferred); ChargeCost stays the base 1.
	// AngrealInvMinion.uc:40-41 summon sparkles.
	TopSparkle = TEXT("APinkCorona");
	BottomSparkle = TEXT("ABlueCorona");
	bElementSpirit = true;
	bRare = true;
	MaxCharges = 5;
	MinChargeGroupInterval = 10.0f;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateMN.ActivateMN"));
}
