#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTDecreaseChargesLeech.generated.h"

// Which rarity of held artifact this leech drains.
enum class EWoTChargeRarity : uint8 { Common, Uncommon, Rare };

// Port of the Decrease{Common,Uncommon,Rare}ChargesLeech family
// (exports/script/Angreal/*ChargesLeech.uc) — the charge-drain leeches installed
// by AngrealDecayProjectile. Each AffectHost pass removes ONE charge from every
// held AngrealInventory of the matching rarity (the original ignores the
// coalesced Iterations count — one charge per interval), and when an artifact
// hits zero it UnCasts + GoEmpty. If the host is gone/dead the leech detaches.
//
// The three shipped classes are identical but for the rarity checked and their
// AffectResolution default (Common 1s / Uncommon 3s / Rare 5s), so the loop lives
// once in this base and the concretes below only set defaults (PORT_NOTES).
UCLASS(Abstract)
class UWoTDecreaseChargesLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTDecreaseChargesLeech();

	// The rarity of artifact drained (set per concrete).
	EWoTChargeRarity TargetRarity = EWoTChargeRarity::Common;

	virtual void AffectHost(int32 Iterations = 0) override;

protected:
	bool MatchesRarity(const class AWoTAngrealInventory* Angreal) const;
};

// DecreaseCommonChargesLeech — drains common artifacts every 1s. The original
// also spawned an AttachedSound follower (DecaySound=Angreal.Decay.LeechDC,
// AttachedSound.uc byte defaults 64/192); the base-leech ambient hum carries it.
UCLASS()
class UWoTDecreaseCommonChargesLeech : public UWoTDecreaseChargesLeech
{
	GENERATED_BODY()

public:
	UWoTDecreaseCommonChargesLeech()
	{
		TargetRarity = EWoTChargeRarity::Common;
		AffectResolution = 1.0f;
		AmbientSoundPath = FName(TEXT("Angreal/LeechDC"));
		AmbientSoundRadius = 64;
		AmbientSoundVolume = 192;
	}
};

// DecreaseUncommonChargesLeech — drains uncommon artifacts every 3s.
UCLASS()
class UWoTDecreaseUncommonChargesLeech : public UWoTDecreaseChargesLeech
{
	GENERATED_BODY()

public:
	UWoTDecreaseUncommonChargesLeech()
	{
		TargetRarity = EWoTChargeRarity::Uncommon;
		AffectResolution = 3.0f;
	}
};

// DecreaseRareChargesLeech — drains rare artifacts every 5s.
UCLASS()
class UWoTDecreaseRareChargesLeech : public UWoTDecreaseChargesLeech
{
	GENERATED_BODY()

public:
	UWoTDecreaseRareChargesLeech()
	{
		TargetRarity = EWoTChargeRarity::Rare;
		AffectResolution = 5.0f;
	}
};
