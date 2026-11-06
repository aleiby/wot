#pragma once

#include "CoreMinimal.h"
#include "WoTCharacter.h"
#include "WoTFactionCharacters.generated.h"

// The four MP faction player pawns (WoTPawns.{AesSedai,Forsaken,Hound,Whitecloak} — each a
// WOTPlayer subclass in the original) as thin DATA-ONLY subclasses: behavior is entirely
// AWoTCharacter (which always stood in for all four); each CDO carries what the shipped
// defaultproperties declare — TeamDescription, body mesh, DrawScale, bIsFemale, and the
// summon-troop faction (WOTPlayer.GetTroop reads the player CLASS, WOTPlayer.uc:4076).
// BattleInfo.TeamClass[Team] selects these per team (giMPBattle.JoinTeam); the Arena/base
// game mode selects them from each player's retail ?Class= login option. Base *_Base skins
// ride the imported meshes' own materials; the personal ?Skin= variant axis layers over
// them (AWoTCharacter::PersonalSkin) and each class carries its PlayerColor identity name
// (P02 — both axes live).

UCLASS()
class AWoTAesSedai : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTAesSedai();
};

UCLASS()
class AWoTForsaken : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTForsaken();
};

UCLASS()
class AWoTHound : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTHound();
};

UCLASS()
class AWoTWhitecloak : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTWhitecloak();
};
