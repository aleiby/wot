#pragma once

#include "CoreMinimal.h"

class AActor;
class AWoTCharacter;

// The item behind a killing blow, for the optional onboarding death recap.
// The server reads it from the blow's damage causer as the pawn dies and the
// game mode sends it to the victim alone (AWoTPlayerController::
// ClientKilledBy). Presentation only: the inspection is read-only and nothing
// in the simulation consults the answer. The original never names the item
// in its death line (giWOT.uc:127-158), so this is an addition, not parity.
namespace WoTKillingItem
{
	/** The shipped script class name (the hand-table key the onboarding guide
	    rows use) of the item behind a killing blow of DamageType dealt to
	    Victim through DamageCauser, or NAME_None when no item dealt it or the
	    port cannot tell which one did:
	    - an item (an angreal's own blow, or a leech or reflector naming its
	      source angreal as causer): that item, except that a self-inflicted
	      blow from one of the victim's own tainted items names Taint
	      (AngrealInventory.uc:306/315);
	    - an angreal projectile, or a piece it spawned: its source angreal;
	    - a fire wall's slab, a spear, a portcullis's mover: the item that
	      places the trap;
	    - the Mashadar a Guardian summons: Guardian;
	    - a Balefire blow, which carries no causer
	      (AngrealInvBalefire.uc:316-322): Balefire.
	    Falls, drowning, zones, map movers, pits, staircases and summoned
	    creatures answer NAME_None. */
	WOT_API FName Resolve(const AWoTCharacter& Victim, FName DamageType,
		const AActor* DamageCauser);
}
