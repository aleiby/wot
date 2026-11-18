#include "WoTGameState.h"

#include "Net/UnrealNetwork.h"

AWoTGameState::AWoTGameState()
{
	// GameReplicationInfo drives the countdown from Tick (not a 1s Timer): enable it.
	PrimaryActorTick.bCanEverTick = true;
}

void AWoTGameState::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// GameReplicationInfo.Tick — count real seconds. Runs on server AND clients; the
	// initial-only replication of the two counters makes it a client-predicted clock.
	if (!bStopCountDown)
	{
		RemainingTime -= DeltaTime;
		ElapsedTime += DeltaTime;
	}
}

void AWoTGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// GameReplicationInfo.replication: the two counters seed once (bNetInitial), then
	// each machine ticks its own copy; the flags fan out on every update.
	DOREPLIFETIME_CONDITION(AWoTGameState, RemainingTime, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTGameState, ElapsedTime, COND_InitialOnly);
	DOREPLIFETIME(AWoTGameState, bDisplayRemaining);
	DOREPLIFETIME(AWoTGameState, bStopCountDown);
}
