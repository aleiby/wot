#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "WoTGameState.generated.h"

// Port of GameReplicationInfo's match-clock fields (Engine/GameReplicationInfo.uc).
// The countdown is client-predicted exactly as the original: RemainingTime /
// ElapsedTime replicate once (bNetInitial) and both server and clients then
// decrement locally in Tick. giCombatBase polls RemainingTime from AWoTGameMode::Timer.
UCLASS()
class AWoTGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AWoTGameState();

	// Seconds left in the match (giCombatBase seeds it to 60*TimeLimit). Counts down
	// through zero into the negative GameRestartDelay window before the map rotates.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Match") float RemainingTime = 0.0f;
	// Seconds since match start (shown when there is no TimeLimit).
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Match") float ElapsedTime = 0.0f;
	// Show the remaining-time countdown (TimeLimit > 0); else the elapsed clock.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Match") bool bDisplayRemaining = false;
	// Freeze the clock (e.g. pre-match / intermission) — halts both counters.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Match") bool bStopCountDown = false;

	// GameReplicationInfo.Tick: advance the match clock unless it is frozen.
	virtual void Tick(float DeltaTime) override;

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
