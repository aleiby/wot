#include "WoTShieldReflector.h"

#include "../WoTSound.h"
#include "../WoTSoundTable.h"

#include "Net/UnrealNetwork.h"

UWoTShieldReflector::UWoTShieldReflector()
{
	// ShieldReflector defaultproperties.
	ShieldReduceDamagePct = 50;
	MaxShieldHitPoints = 50;
	ShieldHitPoints = 50;   // full pool on a first install (see Install for a recast)
	Priority = 225;
	bRemoveExisting = true;
	bDeleterious = false;
}

void UWoTShieldReflector::Install(APawn* NewHost)
{
	// ShieldReflector.Install (uc:33-44): pawn pool = MaxShieldHitPoints, then
	// Super.Install -> bRemoveExisting UnInstalls every same-class reflector,
	// and ShieldReflector.UnInstall (uc:61-70) zeroes the pawn pool again.
	bool bReplacesWornShield = false;
	if (IsValid(NewHost) && bRemoveExisting)
	{
		TArray<UWoTReflector*> Existing;
		NewHost->GetComponents<UWoTReflector>(Existing);
		for (const UWoTReflector* R : Existing)
		{
			bReplacesWornShield |= R != this && IsValid(R) && R->GetClass() == GetClass();
		}
	}
	ShieldHitPoints = MaxShieldHitPoints;
	Super::Install(NewHost);
	// Only if the install really ran (a filtered install never reached
	// bRemoveExisting, so the worn shield and its pool stay) — and the catch-
	// failure branch (Owner != NewHost, uc:47-57) zeroes the pool too.
	if (IsBeingDestroyed() || GetOwner() != NewHost)
	{
		ShieldHitPoints = 0;
	}
	else if (bReplacesWornShield)
	{
		ShieldHitPoints = 0;
	}
}

int32 UWoTShieldReflector::ModifyDamage(int32 Damage, FName /*WoTDamageType*/)
{
	if (UWorld* World = GetWorld(); World && GetOwnerRole() == ROLE_Authority)
	{
		// ShieldReflector.uc:96: every redirected hit plays DeflectSD.
		WoTSound::PlayReplicated(*World, TEXT("Angreal/DeflectSD"),
			GetOwner()->GetActorLocation(), WoTSound::WorldSoundRadius(32));
	}
	// Split the hit between the player and the shield pool; alternate the
	// rounding side each hit (bCalcOrder) exactly as the original.
	bCalcOrder = !bCalcOrder;
	const int32 ReducedShare = Damage * ShieldReduceDamagePct / 100;
	const int32 Remainder = Damage - ReducedShare;
	int32 PlayerDamage = bCalcOrder ? ReducedShare : Remainder;
	const int32 ShieldDamage = bCalcOrder ? Remainder : ReducedShare;

	ShieldHitPoints -= ShieldDamage;
	const bool bShieldDepleted = ShieldHitPoints <= 0;
	if (bShieldDepleted)
	{
		// Overflow (negative pool) spills onto the player; the shield breaks.
		PlayerDamage -= ShieldHitPoints;
		ShieldHitPoints = 0;
	}
	// ShieldReflector.uc:121-125 calls Super only for PlayerDamage > 0; zero-
	// damage pain effects are consumed too while the hit-point shield is active.
	bStoppedLastDamage = PlayerDamage <= 0;
	if (bShieldDepleted)
	{
		Uninstall();
	}

	return PlayerDamage;
}

void UWoTShieldReflector::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWoTShieldReflector, ShieldHitPoints);
}
