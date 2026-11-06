#include "WoTAngrealInvDart.h"

#include "../Projectiles/WoTDartProjectile.h"

AWoTAngrealInvDart::AWoTAngrealInvDart()
{
	BindStatusIcons(TEXT("I_Dartwh"), TEXT("M_Dartwh"));
	// AngrealInvDart defaultproperties
	bAutoFire = true;
	ProjectileClass = AWoTDartProjectile::StaticClass();
	bElementFire = true;
	bElementAir = true;
	bCommon = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 320.0f;
	MinInitialCharges = 20;
	MaxInitialCharges = 70;
	MaxCharges = 100;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 5;
	MaxChargeUsedInterval = 0.0f;
}

void AWoTAngrealInvDart::SetColor(FName Color)
{
	if (Color == TEXT("PC_Red") || Color == TEXT("Red"))
		ConfiguredDartColor = TEXT("Red");
	else if (Color == TEXT("PC_Gold") || Color == TEXT("Yellow")
		|| Color == TEXT("Gold"))
		ConfiguredDartColor = TEXT("Yellow");
	else if (Color == TEXT("PC_Green") || Color == TEXT("Green"))
		ConfiguredDartColor = TEXT("Green");
	else if (Color == TEXT("PC_Purple") || Color == TEXT("Purple"))
		ConfiguredDartColor = TEXT("Purple");
	else
		ConfiguredDartColor = TEXT("Blue");
}
