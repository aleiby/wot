#pragma once

#include "CoreMinimal.h"
#include "WoTCreatureTypes.generated.h"

// The two authored creature-classification columns, split into their own header so
// non-AI code (AWoTCharacter::SummonFaction) can name EWoTCreatureFaction without
// pulling in WoTCreatureData.h -> WoTNPCCharacter.h -> WoTCharacter.h (a cycle).

// Tier is the TROOP-TABLE ROLE (WOTPlayer.PlayerTroops, WOTPlayer.uc:7264), NOT the
// class layer — Sitter and Questioner are Champion-tier troops whose classes sit on
// Captain/Grunt (so they do NOT inherit Champion's no-morale / no-Automatic fold).
// New authored column (no source field — policy-port-map.md §6/§10).
UENUM()
enum class EWoTCreatureTier : uint8
{
	Grunt,
	Captain,
	Champion
};

// Faction is the troop-table PLAYER (WOTPlayer.uc:7264) + the DisguiseIcon folder.
// New authored column (no source field).
UENUM()
enum class EWoTCreatureFaction : uint8
{
	AesSedai,
	Forsaken,
	Whitecloak,
	Hound
};
