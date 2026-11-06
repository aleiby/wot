#include "WoTAngrealInvShift.h"

#include "../WoTAppearEffect.h"
#include "../WoTCharacter.h"
#include "../WoTZoneVolume.h"
#include "../WoTUE1Collision.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "WoTLeech.h"
#include "WoTReflector.h"
#include "WoTWhirlwindLeech.h"

#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AWoTAngrealInvShift::AWoTAngrealInvShift()
{
	BindStatusIcons(TEXT("I_Shift"), TEXT("M_Shift"));
	// AngrealInvShift defaultproperties
	ShiftDistance = 160.0f;
	bElementAir = true;
	bElementSpirit = true;
	bUncommon = true;
	bDefensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 6.0f;
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateSF.ActivateSF"));
	MaxChargeUsedInterval = 1.0f;
	MinChargeGroupInterval = 3.0f;
	InventoryGroup = 54;
}

void AWoTAngrealInvShift::CastArtifact()
{
	AWoTCharacter* OwnerPawn = Cast<AWoTCharacter>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	// ShiftEffect.uc:127-136: Destination = Location + ((1,0,0)*ShiftDistance >>
	// ViewRotation) — pitch included, so a downward view aims into the floor.
	const FVector Dest = OwnerPawn->GetActorLocation()
		+ OwnerPawn->GetViewRotation().Vector() * ShiftDistance;

	// ShiftEffect.uc:138-141: Util.ActorFits(Victim, Destination, 1024) tests only
	// BLOCKING ACTORS (signed height, movers skipped — Legend/Util.uc:400-440);
	// world geometry is not consulted here. Then Victim.SetLocation, i.e. UE1
	// FarMoveActor: FindSpot nudges the cylinder out of the world and fails only
	// when no spot fits. TeleportTo is UE5's port of
	// FarMoveActor (FindTeleportSpot + encroachment), so a destination a few uu
	// into the floor lands on it instead of failing the cast.
	if (!UWoTWhirlwindLeech::ActorFits(OwnerPawn, Dest, ActorFitsRadius)
		|| !WoTUE1Collision::TeleportWorldOnly(
			*OwnerPawn, Dest, OwnerPawn->GetActorRotation()))
	{
		Failed();
		return;
	}
	const FVector Arrived = OwnerPawn->GetActorLocation();

	// Any seeker locked onto us loses its target.
	for (TActorIterator<AWoTSeekingProjectile> It(GetWorld()); It; ++It)
	{
		if (It->Destination == OwnerPawn)
		{
			It->SetDestination(nullptr);
		}
	}

	// ShiftOutOfLeechesAndReflectors (WOTUtil.uc): shifting frees you from every
	// removable, deleterious, non-projectile weave latched on you (whirlwind lift,
	// lightning link) — those are destroyed outright. Projectile-installed payloads
	// (Ice/Decay/Taint) stay, exactly as they survive a Swap.
	auto IsBad = [](const auto* E)
	{
		return IsValid(E) && E->bRemovable && E->bDeleterious && !E->bFromProjectile;
	};
	TArray<UWoTLeech*> Leeches;
	OwnerPawn->GetComponents<UWoTLeech>(Leeches);
	for (UWoTLeech* L : Leeches) { if (IsBad(L)) { L->Unattach(); } }
	TArray<UWoTReflector*> Reflectors;
	OwnerPawn->GetComponents<UWoTReflector>(Reflectors);
	for (UWoTReflector* R : Reflectors) { if (IsBad(R)) { R->Uninstall(); } }

	// ShiftEffect.uc:156-159: the arrival rings — SetColors('Blue','Gold') ->
	// Cyan top / Yellow bottom, no fade (bFadeIn=false is the retail SP-only
	// fade opt-out; the port never fades — see AWoTAppearEffect).
	AWoTAppearEffect::SpawnColored(*GetWorld(), OwnerPawn,
		TEXT("Blue"), TEXT("Gold"));

	// Force a fall from wherever we landed — a shift forward/up while walking would
	// otherwise leave the caster floating (ShiftEffect.uc:162-166). Skipped in water.
	if (!AWoTZoneVolume::IsPointInWater(GetWorld(), Arrived))
	{
		if (UCharacterMovementComponent* Move = OwnerPawn->GetCharacterMovement())
		{
			Move->SetMovementMode(MOVE_Falling);
		}
	}

	Super::CastArtifact();   // sound + HUD hook
	UseCharge();
}
