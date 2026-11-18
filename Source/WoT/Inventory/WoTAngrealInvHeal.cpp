#include "WoTAngrealInvHeal.h"

#include "../WoTCharacter.h"

AWoTAngrealInvHeal::AWoTAngrealInvHeal()
{
	BindStatusIcons(TEXT("I_Healing"), TEXT("M_Healing"));
	// AngrealInvHeal defaultproperties
	HealingAmount = 10;
	bElementWater = true;
	bElementAir = true;
	bElementSpirit = true;
	bRare = true;
	bDefensive = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 7;
	MaxCharges = 10;
	Priority = 100.0f;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateHL.ActivateHL"));
	MaxChargesInGroup = 4;
	MinChargesInGroup = 3;
	MinChargeGroupInterval = 6.0f;
	InventoryGroup = 61;
	// GetPriority AI override (only heal when Health < 100-HealingAmount) deferred
	// with the bot-AI pass.
}

void AWoTAngrealInvHeal::CastArtifact()
{
	AWoTCharacter* OwnerPawn = Cast<AWoTCharacter>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	// Full health rejects the cast before healing or spending a charge
	// (AngrealInvHeal.uc:34-38).
	if (OwnerPawn->Health >= 100)
	{
		return;
	}

	OwnerPawn->IncreaseHealth(HealingAmount);   // IncreaseHealthEffect inlined
	Super::CastArtifact();                   // sound + HUD hook
	UseCharge();
}
