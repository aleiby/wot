#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WoTMatchParticipant.generated.h"

class AController;
class AWoTCharacter;

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UWoTMatchParticipant : public UInterface
{
	GENERATED_BODY()
};

class WOT_API IWoTMatchParticipant
{
	GENERATED_BODY()

public:
	virtual void OnParticipantPawnDied(AWoTCharacter* DeadPawn, float RespawnDelaySeconds) = 0;
	virtual void OnMatchEnded() = 0;
};

// A player-state-bearing AI participant, excluding ordinary NPC controllers.
WOT_API bool IsWoTBotParticipant(const AController* Controller);
