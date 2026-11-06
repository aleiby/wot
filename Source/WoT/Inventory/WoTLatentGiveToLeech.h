#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTLatentGiveToLeech.generated.h"

// Port of WOT.LatentGiveToLeech (Leech) — after AffectResolution seconds hands
// the specified Inventory item to our host pawn, gated by the game-mode
// PickupQuery, then self-destroys. AngrealInventory.GoEmpty spawns one to give
// an emptied NPC weave back to its caster after NPCRespawnTime.
// Spec: exports/script/WOT/LatentGiveToLeech.uc (all).
//
// UE5 mapping: retail's leech is an Actor attached to (owned by) the host pawn;
// the port's UWoTLeech is a UActorComponent on that pawn (Leech framework,
// PORT_NOTES). "Pawn(Owner)" in AffectHost therefore reads GetOwner(). The
// retail LifeSpan=10000 safety net (LatentGiveToLeech.uc:51) is intentionally
// dropped: the component self-destroys via AffectHost/Unattach at
// AffectResolution, and if the host dies first the component dies with it (see
// OnComponentDestroyed) — both cleanup paths are covered without a timed
// fallback.
UCLASS()
class UWoTLatentGiveToLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTLatentGiveToLeech();

	// May be none (LatentGiveToLeech.uc:20).
	UPROPERTY() TSubclassOf<class AWoTInventory> ItemType;
	// If set, overrides use of ItemType (LatentGiveToLeech.uc:21).
	UPROPERTY() TObjectPtr<class AWoTInventory> Item;

	virtual void AffectHost(int32 Iterations = 0) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
};
