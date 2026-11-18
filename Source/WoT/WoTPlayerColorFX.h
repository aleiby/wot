#pragma once

#include "CoreMinimal.h"

class APawn;

// The single presentation seam for the retail PlayerColor axis (P02 landed): a
// player pawn resolves its (appearance-)class CDO PlayerColor (AesSedai PC_Blue /
// Forsaken PC_Red / Hound PC_Purple / Whitecloak PC_Gold), translated to the plain
// color name consumer rows are keyed by; non-players keep the consumer-authored
// fallback, matching retail's per-consumer default cases.
namespace WoTPlayerColorFX
{
	FName Resolve(const APawn* Pawn, FName NonPlayerFallback);
	// AngrealInvDart.GiveTo appearance mapping for non-player owners: only the
	// four faction-player appearances override the otherwise-green NPC fallback.
	// Folded through the same future PlayerColor seam as player owners.
	FName ResolveDart(const APawn* Pawn);
}
