#include "WoTAndilayRoot.h"

#include "WoTAndilayRootLeech.h"
#include "../WoTCharacter.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"

AWoTAndilayRoot::AWoTAndilayRoot()
{
	BindStatusIcons(TEXT("I_AndilayRoot"), TEXT("M_AndilayRoot"));
	DurationType = EWoTDurationType::DT_Lifespan;
	TaintCommonDamage = 0;
	TaintUncommonDamage = 0;
	TaintRareDamage = 0;
	MinInitialCharges = 15;
	MaxInitialCharges = 20;
	Priority = 20.0f;   // inherited LeechAttacher.uc default
	InventoryGroup = 50;
	ActivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/ActivateAR.ActivateAR"));
	PickupSound = TSoftObjectPtr<USoundBase>();
	bRotatingPickup = false;
	Collision->SetSphereRadius(10.0f);
}

UWoTAndilayRootLeech* AWoTAndilayRoot::FindHealingLeech(APawn* Host) const
{
	if (!Host)
	{
		return nullptr;
	}
	TArray<UWoTAndilayRootLeech*> Leeches;
	Host->GetComponents<UWoTAndilayRootLeech>(Leeches);
	for (UWoTAndilayRootLeech* Leech : Leeches)
	{
		if (IsValid(Leech) && Leech->IsRegistered())
		{
			return Leech;
		}
	}
	return nullptr;
}

int32 AWoTAndilayRoot::GetPotentialHealth(const AWoTCharacter* Other) const
{
	if (!Other)
	{
		return 0;
	}
	int32 PotentialHealth = Other->Health;
	TArray<UWoTAndilayRootLeech*> Leeches;
	Other->GetComponents<UWoTAndilayRootLeech>(Leeches);
	for (const UWoTAndilayRootLeech* Leech : Leeches)
	{
		if (IsValid(Leech) && Leech->IsRegistered())
		{
			PotentialHealth += Leech->NumCharges;
		}
	}
	return PotentialHealth;
}

bool AWoTAndilayRoot::ValidTouch(AActor* Other)
{
	const AWoTCharacter* Character = Cast<AWoTCharacter>(Other);
	if (Character)
	{
		// AndilayRoot.uc:33-50 deliberately uses <=, allowing exactly one
		// pickup at full health while rejecting stored healing above the cap.
		// MaxHealth is the authenticated inherited class default stamped on the
		// instance; the native CDO intentionally has no player-health literal.
		if (GetPotentialHealth(Character) > Character->MaxHealth)
		{
			return false;
		}
	}
	return Super::ValidTouch(Other);
}

void AWoTAndilayRoot::GiveTo(APawn* Other)
{
	AWoTCharacter* Character = Cast<AWoTCharacter>(Other);
	if (!Character)
	{
		return;
	}
	if (GetPotentialHealth(Character) > Character->MaxHealth)
	{
		return;   // AndilayRoot.GiveTo repeats the Touch gate.
	}

	Super::GiveTo(Other);
	if (Other != GetOwner())
	{
		return;
	}

	const int32 ChargesToGive = CurCharges;
	UWoTAndilayRootLeech* Leech = FindHealingLeech(Other);
	if (!Leech)
	{
		Leech = NewObject<UWoTAndilayRootLeech>(Other);
		Leech->SetSourceAngreal(this);
		Leech->AttachTo(Other);
		if (!IsValid(Leech) || !Leech->IsRegistered())
		{
			Leech = nullptr;
		}
	}

	// UWoTLeech::AttachTo's bSingular path rejects/destroys an incoming second
	// component; it does not merge. Find the existing component first and top it
	// up explicitly, matching AndilayRoot.GiveTo's iterator + GiveCharges path.
	ChargeCost = CurCharges;
	Super::CastArtifact();
	UseCharge();
	if (IsValid(Leech))
	{
		// Retail tops up only after Cast has spent/emptied the inventory actor.
		Leech->GiveCharges(ChargesToGive);
	}
}
