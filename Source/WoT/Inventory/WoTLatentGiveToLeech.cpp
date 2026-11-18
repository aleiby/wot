#include "WoTLatentGiveToLeech.h"

#include "WoTInventory.h"
#include "../WoTGameMode.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"

UWoTLatentGiveToLeech::UWoTLatentGiveToLeech()
{
	// LatentGiveToLeech.uc defaultproperties (:47-52).
	AffectResolution = 15.0f;   // :49 — GoEmpty overrides this with NPCRespawnTime.
	bRemovable = false;         // :50
	// LifeSpan=10000 (:51) intentionally omitted — see the header note.
}

void UWoTLatentGiveToLeech::AffectHost(int32 /*Iterations*/)
{
	// LatentGiveToLeech.uc:24-45. "Pawn(Owner)" == our host pawn (the component
	// owner). AffectHost only fires from UWoTLeech::Tick with a live authority
	// owner, so the host is valid here; the host-died path runs via
	// OnComponentDestroyed instead.
	if (APawn* HostPawn = Cast<APawn>(GetOwner()))
	{
		if (Item != nullptr || ItemType != nullptr)
		{
			if (Item == nullptr)
			{
				Item = GetWorld()->SpawnActor<AWoTInventory>(ItemType);
			}

			if (IsValid(Item))
			{
				bool bAllowed = true;
				if (const AWoTGameMode* Game =
				        GetWorld()->GetAuthGameMode<AWoTGameMode>())
				{
					bAllowed = Game->PickupQuery(HostPawn, Item);
				}
				if (bAllowed)
				{
					Item->GiveTo(HostPawn);
				}
				else
				{
					Item->Destroy();
				}
			}
			// Handed off (given or destroyed) — clear so the teardown path below
			// never double-frees a now-inventory-held item.
			Item = nullptr;
		}
	}

	// UnAttach(); Destroy(); — Unattach() destroys this component (LatentGiveToLeech.uc:43-44).
	Unattach();
}

void UWoTLatentGiveToLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// If we are torn down before AffectHost hands the item off — the host pawn
	// died before NPCRespawnTime, so the component model destroys us with it —
	// the emptied weave would otherwise be orphaned in the world. Destroy it
	// without granting. AffectHost clears Item once handled, so a normal
	// completion leaves Item null and never re-destroys the live pickup.
	if (IsValid(Item))
	{
		Item->Destroy();
		Item = nullptr;
	}
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}
