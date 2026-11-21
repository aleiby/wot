#include "WoTAngrealInvShift.h"
#include "Testing/WoTSoakEvents.h"

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
	WOT_SOAK_BODY_SCOPE("Shift");
	AWoTCharacter* OwnerPawn = Cast<AWoTCharacter>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	// ShiftEffect.uc:129-136: the destination lies ShiftDistance along the view
	// rotation, pitch included, so a downward view aims into the floor.
	const FVector Dest = OwnerPawn->GetActorLocation()
		+ OwnerPawn->GetViewRotation().Vector() * ShiftDistance;

	// ShiftEffect.uc:138-141: Util.ActorFits(Victim, Destination, 1024) tests only
	// blocking actors (signed height, movers skipped; Legend/Util.uc:400-443).
	// Then Victim.SetLocation places the body: it has to fit the level at the
	// destination, which may nudge it out of a floor or wall it only grazes but
	// fits it nowhere in solid space, so a destination under the floor or behind
	// an outer wall fails the cast instead of leaving the body outside the level;
	// and no blocking actor may be where it lands. Only then does the body move.
	UWorld* World = GetWorld();
	FVector Spot = Dest;
	if (!World
		|| !UWoTWhirlwindLeech::ActorFits(OwnerPawn, Dest, ActorFitsRadius)
		|| !WoTUE1Collision::FindSpotLevelOnly(*World,
			OwnerPawn->GetSimpleCollisionCylinderExtent(), Spot, /*bAssumeFit*/ false)
		|| WoTUE1Collision::FindEncroachedBlockingActorAt(*OwnerPawn, Spot)
		|| !OwnerPawn->TeleportTo(Spot, OwnerPawn->GetActorRotation(),
			/*bIsATest*/ false, /*bNoCheck*/ true))
	{
		Failed();
		return;
	}
	WOT_SOAK_BODY_EVENT(OwnerPawn, "Shift placed");
	UWoTZoneIndex::RefreshAfterRelocation(OwnerPawn);
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
