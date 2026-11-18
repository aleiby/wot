#include "WoTCitadelGameState.h"

#include "Net/UnrealNetwork.h"

void AWoTCitadelGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// CitadelGameReplicationInfo.replication: reliable if( Role == ROLE_Authority ) SealGoal.
	DOREPLIFETIME(AWoTCitadelGameState, SealGoal);
}
