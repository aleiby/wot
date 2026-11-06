#include "WoTAndilayRootLeech.h"

#include "../WoTCharacter.h"

#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UWoTAndilayRootLeech::UWoTAndilayRootLeech()
{
	// AndilayRootLeech.uc defaultproperties (:113-125). The retail LifeSpan
	// value and its Tick-time charge-ratio rewrite exist only to drive the HUD
	// icon; GetInitialDuration/GetDuration expose that surface without a gameplay
	// lifespan, so the component lives until its charges are spent or its host
	// dies (the two gameplay teardown paths in AffectHost).
	MaxAffectResolution = 0.8f;
	MinAffectResolution = 0.2f;
	MaxHealSpeed = 400.0f;
	HealingAmount = 1;
	MaxCharges = 100;
	AffectResolution = 10000.0f;
	bDisplayIcon = true;
	bRemovable = false;
	bSingular = true;
	Lifespan = 0.0f;
}

void UWoTAndilayRootLeech::GiveCharges(int32 Amount)
{
	NumCharges = FMath::Min(NumCharges + Amount, MaxCharges);
	PeakCharges = FMath::Max(PeakCharges, NumCharges);
}

float UWoTAndilayRootLeech::GetDuration() const
{
	// AndilayRootLeech.uc:101-107 rewrites LifeSpan to this ratio solely for
	// IconInfo. Keep gameplay lifespan disabled and expose the same HUD surface.
	return PeakCharges > 0
		? 10000.0f * static_cast<float>(NumCharges) / static_cast<float>(PeakCharges)
		: 0.0f;
}

int32 UWoTAndilayRootLeech::GetCharges()
{
	const int32 Charges = FMath::Min(NumCharges, HealingAmount);
	NumCharges -= Charges;
	return Charges;
}

void UWoTAndilayRootLeech::AffectHost(int32 /*Iterations*/)
{
	AWoTCharacter* Host = Cast<AWoTCharacter>(GetOwner());
	if (!IsValid(Host) || Host->Health <= 0 || NumCharges <= 0)
	{
		Unattach();
		return;
	}

	// AndilayRootLeech.uc:34-41: GetCharges() is INSIDE this gate. A full-health
	// host therefore consumes no charge; only a wounded host takes the 1-HP
	// trickle and advances toward depletion.
	if (Host->Health < Host->MaxHealth)
	{
		Host->IncreaseHealth(GetCharges());
		if (NumCharges <= 0)
		{
			Unattach();
		}
	}
}

void UWoTAndilayRootLeech::TickComponent(
	float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	if (const AActor* Host = GetOwner())
	{
		// AndilayRootLeech.uc:98-100. Deliberately UNCLAMPED: a host faster
		// than MaxHealSpeed heals more slowly than MaxAffectResolution.
		const float Scalar = MaxHealSpeed > 0.0f
			? Host->GetVelocity().Size() / MaxHealSpeed : 0.0f;
		AffectResolution = MinAffectResolution
			+ Scalar * (MaxAffectResolution - MinAffectResolution);
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UWoTAndilayRootLeech::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWoTAndilayRootLeech, NumCharges);
}
