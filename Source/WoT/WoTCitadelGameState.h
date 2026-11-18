#pragma once

#include "CoreMinimal.h"
#include "WoTGameState.h"
#include "WoTCitadelGameState.generated.h"

// Port of CitadelGameReplicationInfo (exports/script/WOT/CitadelGameReplicationInfo.uc).
// The Battle/citadel-siege GameReplicationInfo: a straight extension of the Arena match
// clock (AWoTGameState) that adds the per-team seal target. Wired as AWoTBattleGameMode's
// GameStateClass when that mode lands (citadel block, step 3).
UCLASS()
class AWoTCitadelGameState : public AWoTGameState
{
	GENERATED_BODY()

public:
	// CitadelGameReplicationInfo.SealGoal (defaultproperties: 4): seals a team must place
	// on its SealAltar to win (SealAltar.CheckForWinner counts placed seals against this).
	// config in the original, so a leaf-BP/config tunable here (iron rule 2); replicated
	// reliably from the authority — the citadel scoreboard shows "N of SealGoal".
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "WoT|Match") int32 SealGoal = 4;

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
