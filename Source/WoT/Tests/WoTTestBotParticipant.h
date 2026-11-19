#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTMatchParticipant.h"
#include "../WoTPlayerState.h"
#include "WoTTestBotParticipant.generated.h"

// Inert test types follow the existing unguarded UCLASS fixture convention.
UCLASS()
class AWoTTestBotParticipant : public AAIController, public IWoTMatchParticipant
{
	GENERATED_BODY()

public:
	AWoTTestBotParticipant() { bWantsPlayerState = true; }
	virtual void InitPlayerState() override
	{
		if (!PlayerState)
		{
			Super::InitPlayerState();
		}
		if (AWoTPlayerState* State = GetPlayerState<AWoTPlayerState>())
		{
			State->SetIsABot(true);
		}
	}
	int32 PawnDiedCalls = 0;
	int32 MatchEndedCalls = 0;
	float LastRespawnDelay = -1.0f;
	TWeakObjectPtr<AWoTCharacter> LastDeadPawn;
	bool bUnpossessedAtCallback = false;
	virtual void OnParticipantPawnDied(AWoTCharacter* DeadPawn, float Delay) override
	{
		++PawnDiedCalls;
		LastDeadPawn = DeadPawn;
		LastRespawnDelay = Delay;
		bUnpossessedAtCallback = GetPawn() == nullptr && DeadPawn->GetController() == nullptr;
	}
	virtual void OnMatchEnded() override { ++MatchEndedCalls; }
};

UCLASS()
class AWoTTestBotRulesGameMode : public AWoTGameMode
{
	GENERATED_BODY()

public:
	AWoTTestBotRulesGameMode() { PlayerStateClass = AWoTPlayerState::StaticClass(); }
	int32 RestartCalls = 0;
	virtual void RestartGame() override { ++RestartCalls; }
};
