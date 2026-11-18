#pragma once

#include "CoreMinimal.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerStart.h"
#include "WoTPlayerStart.generated.h"

// Port of PlayerStart.TeamNumber (UE1 PlayerStart carried a team tag; UE5's APlayerStart does
// not). Placed per team area in the Battle maps (map data); giCombatBase.FindPlayerStart weights
// spawns toward a player's own team area via TeamNumber. Defaults 0 like the retail byte
// in UE1's PlayerStart — T3D omits default-valued props, so a start with
// no TeamNumber line (Battle2_01's team-0 starts) IS team 0; 255 is the explicit neutral tag.
UCLASS()
class AWoTPlayerStart : public APlayerStart
{
	GENERATED_BODY()

public:
	AWoTPlayerStart(const FObjectInitializer& ObjectInitializer =
		FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		// Engine.PlayerStart defaultproperties: CollisionRadius/Height=40.
		// UE5 APlayerStart is 40x92, so restore the retail cylinder explicitly.
		GetCapsuleComponent()->InitCapsuleSize(40.0f, 40.0f);
	}

	UPROPERTY(EditAnywhere, Category = "WoT|Battle") uint8 TeamNumber = 0;
};
