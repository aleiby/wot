#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTAngrealInventory.h"
#include "WoTLeech.h"
#include "GameFramework/Pawn.h"
#include "WoTIgnoreElementReflector.generated.h"

// Port of Angreal.IgnoreElementReflector + its 5 concretes (Ignore{Fire,Water,
// Air,Earth,Spirit}ElementReflector). Specs: exports/script/Angreal/
// Ignore*ElementReflector.uc. The elemental-shield effect: throw away all damage
// AND all angreal effects composed of the shield's element.
//
// - TakeDamage: negate (return 0) when the incoming AEFWS DamageType contains the
//   ignored element (DamageTypeContains).
// - ProcessEffect: block an effect whose SourceAngreal carries the element
//   (IgnoresEffect -> DamageTypeContains(GetDamageType(Source), element)).
//
// Presentation binds ShieldParticleMesh/deflect feedback, install appearance,
// and the TriggerEvent lifecycle through the production dispatch paths.
UCLASS(Abstract)
class UWoTIgnoreElementReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	// IgnoreElementReflector.uc:187-188 — highest damage priority (runs before the
	// hit-point Personal Shield at 225); a recast replaces the old element shield.
	UWoTIgnoreElementReflector()
	{
		Priority = 255;
		bRemoveExisting = true;
	}

	// AEFWS element name recognized by DamageTypeContains: Air/Earth/Fire/Water/Spirit.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") FName IgnoredElement;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") FName ImpactConfig;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") FName DeflectSoundPath;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield") FName TriggerEvent;

	virtual int32 ModifyDamage(int32 Damage, FName WoTDamageType) override
	{
		bStoppedLastDamage =
			AWoTAngrealInventory::DamageTypeContains(WoTDamageType, IgnoredElement);
		if (bStoppedLastDamage) { SpawnImpactFeedback(); }
		return bStoppedLastDamage ? 0 : Damage;
	}
	virtual bool StopsDamageChain(int32 /*DamageAfterMe*/) const override
	{
		return bStoppedLastDamage;
	}

	// IgnoreElementReflector.uc:96 — ProcessEffect passes an effect through (does
	// NOT block) if it's beneficial (!bDeleterious) OR self-sourced; so we only
	// block a same-element effect that is deleterious AND not from our own artifact.
	virtual bool IgnoresEffect(const AWoTAngrealInventory* Source,
	                           bool bEffectDeleterious) const override
	{
		return bEffectDeleterious && Source != SourceAngreal
			&& AWoTAngrealInventory::DamageTypeContains(
				AWoTAngrealInventory::GetDamageType(Source), IgnoredElement);
	}

	// IgnoreElementReflector.uc:33-46 — Install runs RemoveDeleteriousEffects
	// (:141-176): raising the shield strips every deleterious, removable leech AND
	// reflector whose SourceAngreal matches the element (InvIsIgnored, overridden
	// per element as bElement<X> — which GetDamageType+DamageTypeContains
	// reproduces; NO bFromProjectile filter, unlike Swap/Shift) — an Air Shield
	// frees a Whirlwind victim mid-lift. Base Install() is non-virtual, so the
	// cleanse hangs off OnRegister, which RegisterComponent() inside base Install
	// reaches only when the install actually lands (blocked installs bail first).
	virtual void OnRegister() override
	{
		Super::OnRegister();
		APawn* Host = Cast<APawn>(GetOwner());
		if (!Host || GetOwnerRole() != ROLE_Authority)
		{
			return;
		}
		auto Matches = [this](const AWoTAngrealInventory* Src)
		{
			return IsValid(Src) && AWoTAngrealInventory::DamageTypeContains(
				AWoTAngrealInventory::GetDamageType(Src), IgnoredElement);
		};
		TArray<UWoTLeech*> Leeches;
		Host->GetComponents<UWoTLeech>(Leeches);
		for (UWoTLeech* L : Leeches)
		{
			if (IsValid(L) && L->bDeleterious && L->bRemovable && Matches(L->SourceAngreal))
			{
				L->Unattach();
			}
		}
		TArray<UWoTReflector*> Reflectors;
		Host->GetComponents<UWoTReflector>(Reflectors);
		for (UWoTReflector* R : Reflectors)
		{
			if (R != this && IsValid(R) && R->bDeleterious && R->bRemovable
				&& Matches(R->SourceAngreal))
			{
				R->Uninstall();
			}
		}
		OnInstalledFeedback();
	}
	virtual void OnUnregister() override;
	virtual void OnEffectIgnoredFeedback() override { SpawnImpactFeedback(); }

protected:
	bool bStoppedLastDamage = false;
	void SpawnImpactFeedback();
	void OnInstalledFeedback();
};

UCLASS()
class UWoTIgnoreFireElementReflector : public UWoTIgnoreElementReflector
{
	GENERATED_BODY()
public:
	UWoTIgnoreFireElementReflector()
	{
		IgnoredElement = TEXT("Fire"); ImpactConfig = TEXT("FireShieldVisual");
		DeflectSoundPath = TEXT("Angreal/DeflectFS"); TriggerEvent = TEXT("ElementalFireTriggered");
	}
};

UCLASS()
class UWoTIgnoreWaterElementReflector : public UWoTIgnoreElementReflector
{
	GENERATED_BODY()
public:
	UWoTIgnoreWaterElementReflector()
	{
		IgnoredElement = TEXT("Water"); ImpactConfig = TEXT("WaterShieldVisual");
		DeflectSoundPath = TEXT("Angreal/DeflectWS"); TriggerEvent = TEXT("ElementalWaterTriggered");
	}

	// IgnoreWaterElementReflector.uc:94-105: the water shield also eats exactly
	// 'drowned' — breath damage carries no AEFWS letters, so the base element
	// test alone would pass it through.
	virtual int32 ModifyDamage(int32 Damage, FName WoTDamageType) override
	{
		if (WoTDamageType == FName(TEXT("Drowned")))
		{
			bStoppedLastDamage = true;
			SpawnImpactFeedback();
			return 0;
		}
		return Super::ModifyDamage(Damage, WoTDamageType);
	}
};

UCLASS()
class UWoTIgnoreAirElementReflector : public UWoTIgnoreElementReflector
{
	GENERATED_BODY()
public:
	UWoTIgnoreAirElementReflector()
	{
		IgnoredElement = TEXT("Air"); ImpactConfig = TEXT("AirShieldVisual");
		DeflectSoundPath = TEXT("Angreal/DeflectAS"); TriggerEvent = TEXT("ElementalAirTriggered");
	}
};

UCLASS()
class UWoTIgnoreEarthElementReflector : public UWoTIgnoreElementReflector
{
	GENERATED_BODY()
public:
	UWoTIgnoreEarthElementReflector()
	{
		IgnoredElement = TEXT("Earth"); ImpactConfig = TEXT("EarthShieldVisual");
		DeflectSoundPath = TEXT("Angreal/DeflectES"); TriggerEvent = TEXT("ElementalEarthTriggered");
	}
};

UCLASS()
class UWoTIgnoreSpiritElementReflector : public UWoTIgnoreElementReflector
{
	GENERATED_BODY()
public:
	UWoTIgnoreSpiritElementReflector()
	{
		IgnoredElement = TEXT("Spirit"); ImpactConfig = TEXT("SpiritShieldVisual");
		DeflectSoundPath = TEXT("Angreal/DeflectSS"); TriggerEvent = TEXT("ElementalSpiritTriggered");
	}
};
