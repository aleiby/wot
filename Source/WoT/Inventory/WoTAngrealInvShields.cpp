#include "WoTAngrealInvShields.h"

#include "WoTIgnoreElementReflector.h"
#include "WoTShieldReflector.h"

#include "GameFramework/Pawn.h"

// Shared defaultproperties across the five elemental shields (all identical but
// for the element, reflector, and title).
namespace
{
	void SetupShield(AWoTReflectorInstaller& S)
	{
		S.Duration = 20.0f;
		// DT_Lifespan on Fire/Air/Earth/Spirit; Water alone ships DT_Shield
		// (AngrealInvWaterShield.uc:25 — a retail data quirk, kept as shipped).
		S.DurationType = EWoTDurationType::DT_Lifespan;
		S.bRare = true;
		S.bDefensive = true;
		S.bCombat = true;
		S.MaxInitialCharges = 3;
		S.MaxCharges = 10;
		// MaxChargesInGroup stays the inherited 1 — only Fire Shield raises it.
		S.InventoryGroup = 64;
		// Title/Description/PickupMessage: Angreal.int [AngrealInv<Element>Shield].
	}
}

AWoTAngrealInvFireShield::AWoTAngrealInvFireShield()
{
	BindStatusIcons(TEXT("I_ElShieldRed"), TEXT("M_ElShieldRed"));
	SetupShield(*this);
	bElementFire = true;
	MaxChargesInGroup = 2;   // AngrealInvFireShield.uc:33 — the only shield with 2
	ReflectorClasses = { UWoTIgnoreFireElementReflector::StaticClass() };
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateFS.ActivateFS"));
}

AWoTAngrealInvWaterShield::AWoTAngrealInvWaterShield()
{
	BindStatusIcons(TEXT("I_ElShieldBlue"), TEXT("M_ElShieldBlue"));
	SetupShield(*this);
	DurationType = EWoTDurationType::DT_Shield;   // AngrealInvWaterShield.uc:25
	bElementWater = true;
	ReflectorClasses = { UWoTIgnoreWaterElementReflector::StaticClass() };
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateWS.ActivateWS"));
}

AWoTAngrealInvAirShield::AWoTAngrealInvAirShield()
{
	BindStatusIcons(TEXT("I_ElShieldWhite"), TEXT("M_ElShieldWhite"));
	SetupShield(*this);
	bElementAir = true;
	ReflectorClasses = { UWoTIgnoreAirElementReflector::StaticClass() };
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateAS.ActivateAS"));
}

AWoTAngrealInvEarthShield::AWoTAngrealInvEarthShield()
{
	BindStatusIcons(TEXT("I_ElShieldGreen"), TEXT("M_ElShieldGreen"));
	SetupShield(*this);
	bElementEarth = true;
	ReflectorClasses = { UWoTIgnoreEarthElementReflector::StaticClass() };
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateES.ActivateES"));
}

AWoTAngrealInvSpiritShield::AWoTAngrealInvSpiritShield()
{
	BindStatusIcons(TEXT("I_ElShieldGold"), TEXT("M_ElShieldGold"));
	SetupShield(*this);
	bElementSpirit = true;
	ReflectorClasses = { UWoTIgnoreSpiritElementReflector::StaticClass() };
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateSS.ActivateSS"));
}

AWoTAngrealInvShield::AWoTAngrealInvShield()
{
	BindStatusIcons(TEXT("I_Shield"), TEXT("M_Shield"));
	// AngrealInvShield (Personal Shield): permanent hit-point shield.
	Duration = 0.0f;   // no time limit — worn away by damage
	DurationType = EWoTDurationType::DT_Shield;
	ReflectorClasses = { UWoTShieldReflector::StaticClass() };
	bElementAir = true;
	bCommon = true;   // AngrealInvShield.uc:74 — rarity (taint/SoulBarb key off it)
	bDefensive = true;
	bCombat = true;
	MaxInitialCharges = 3;
	MaxCharges = 10;
	Priority = 1.0f;               // AngrealInvShield.uc:79 (not the installer's 20)
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateSD.ActivateSD"));
	MinChargeGroupInterval = 4.0f; // AngrealInvShield.uc:81
	InventoryGroup = 53;
}

void AWoTAngrealInvShield::CastArtifact()
{
	// AngrealInvShield.uc:33-43: at a full pool (GetShieldHitPoints ==
	// ShieldReflector.default.MaxShieldHitPoints) the cast Fails — no charge, no
	// reinstall. With no shield installed the pool reads 0, so the cast proceeds.
	if (const APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		const UWoTShieldReflector* Shield =
			OwnerPawn->FindComponentByClass<UWoTShieldReflector>();
		if (Shield && Shield->ShieldHitPoints ==
			GetDefault<UWoTShieldReflector>()->MaxShieldHitPoints)
		{
			Failed();
			return;
		}
	}
	Super::CastArtifact();
}

float AWoTAngrealInvShield::GetPriority() const
{
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const UWoTShieldReflector* Shield = OwnerPawn
		? OwnerPawn->FindComponentByClass<UWoTShieldReflector>() : nullptr;
	return Shield && Shield->ShieldHitPoints > 0 ? 0.0f : Priority;
}
