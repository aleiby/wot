#include "WoTDecreaseChargesLeech.h"

#include "WoTAngrealInventory.h"
#include "WoTInventoryComponent.h"
#include "../WoTCharacter.h"

#include "GameFramework/Pawn.h"

UWoTDecreaseChargesLeech::UWoTDecreaseChargesLeech()
{
	// Common to all three ChargesLeech defaults.
	bDeleterious = true;
}

bool UWoTDecreaseChargesLeech::MatchesRarity(const AWoTAngrealInventory* Angreal) const
{
	if (!Angreal)
	{
		return false;
	}
	switch (TargetRarity)
	{
	case EWoTChargeRarity::Common:   return Angreal->bCommon;
	case EWoTChargeRarity::Uncommon: return Angreal->bUncommon;
	case EWoTChargeRarity::Rare:     return Angreal->bRare;
	}
	return false;
}

void UWoTDecreaseChargesLeech::AffectHost(int32 /*Iterations*/)
{
	// Gate on Pawn(Owner).Health > 0 (original) — a dead/gone host detaches us.
	AWoTCharacter* Host = Cast<AWoTCharacter>(GetOwner());
	if (!IsValid(Host) || !Host->IsAlive())
	{
		Unattach();
		return;
	}

	UWoTInventoryComponent* Inv = Host->FindComponentByClass<UWoTInventoryComponent>();
	if (!Inv)
	{
		return;
	}

	// One charge per matching artifact per pass (the original ignores the
	// coalesced Iterations — one charge per interval).
	// GoEmpty unlinks from Inv->Items. Retail's linked-list walk retains its next
	// pointer across that removal; snapshot the array to preserve the same visit.
	const TArray<TObjectPtr<AWoTInventory>> Items = Inv->Items;
	for (AWoTInventory* Item : Items)
	{
		AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item);
		if (Angreal && MatchesRarity(Angreal))
		{
			Angreal->CurCharges -= 1;
			if (Angreal->CurCharges <= 0)
			{
				Angreal->UnCastArtifact();
				Angreal->GoEmpty();
			}
		}
	}
}
