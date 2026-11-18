#include "WoTBattleInfo.h"

#include "WoTCharacter.h"
#include "Net/UnrealNetwork.h"

AWoTBattleInfo::AWoTBattleInfo()
{
	// BattleInfo defaultproperties: bAlwaysRelevant=True; it replicates team state to all.
	bReplicates = true;
	bAlwaysRelevant = true;

	// C++ static arrays are not zero-initialized — start the runtime counters clean (the
	// original's PreBeginPlay asserts them zero at match start).
	for (int32 i = 0; i < 4; ++i)
	{
		NumPlayers[i] = 0;
		NumSeals[i] = 0;
		bEditing[i] = 0;
		bEditingComplete[i] = 0;
	}
}

void AWoTBattleInfo::BeginPlay()
{
	Super::BeginPlay();

	// BattleInfo.PreBeginPlay: validate placed map data. The original asserts (fatal); the
	// port uses ensureMsgf so a half-configured placement during map iteration warns rather
	// than crashing the editor / headless run (divergence, PORT_NOTES). The original's
	// per-team "counter == 0" asserts are vacuous here (the runtime arrays are not
	// editor-exposed, so they are always zero at BeginPlay) — dropped.
	ensureMsgf(NumTeams > 1 && NumTeams <= 4,
		TEXT("BattleInfo %s: NumTeams=%d out of range (expected 2..4)"), *GetName(), NumTeams);
	for (int32 i = 0; i < NumTeams && i < 4; ++i)
	{
		ensureMsgf(TeamClass[i] != nullptr,
			TEXT("BattleInfo %s: TeamClass[%d] unset"), *GetName(), i);
	}
}

void AWoTBattleInfo::ResetTeamSealCount(uint8 Team)
{
	if (Team < 4)
	{
		NumSeals[Team] = 0;
	}
}

void AWoTBattleInfo::IncrementTeamSealCount(uint8 Team)
{
	if (Team < 4)
	{
		NumSeals[Team]++;
	}
}

void AWoTBattleInfo::DecrementTeamSealCount(uint8 Team)
{
	if (Team < 4)
	{
		NumSeals[Team]--;
	}
}

int32 AWoTBattleInfo::GetTeamSealCount(uint8 Team) const
{
	// BattleInfo.GetTeamSealCount: the spectator sentinel holds no seals.
	if (Team == 255 || Team >= 4)
	{
		return 0;
	}
	return NumSeals[Team];
}

FString AWoTBattleInfo::GetTeamDescription(uint8 Team) const
{
	// BattleInfo.GetTeamDescription: spectators have no team; otherwise read the team pawn
	// class's default TeamDescription (Accessed-None-tolerant: "" if the class is unset).
	if (Team == 255)
	{
		return TEXT("Spectator");
	}
	if (Team >= 4 || TeamClass[Team] == nullptr)
	{
		return FString();
	}
	return TeamClass[Team].GetDefaultObject()->TeamDescription;
}

FString AWoTBattleInfo::GetTeamStatus(int32 Team) const
{
	// BattleInfo.GetTeamStatus. Guard the index (UnrealScript tolerated OOB; C++ does not).
	if (Team < 0 || Team >= 4)
	{
		return WaitingStr;
	}
	if (bEditing[Team] == 1)
	{
		return EditingStr;
	}
	else if (!bTeleportersDisabled)
	{
		return PlayingStr;
	}
	else if (bEditingComplete[Team] == 1)
	{
		return ReadyStr;
	}
	else
	{
		return WaitingStr;
	}
}

void AWoTBattleInfo::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// BattleInfo.replication: reliable if( Role == ROLE_Authority ) — all team state to all.
	DOREPLIFETIME(AWoTBattleInfo, NumTeams);
	DOREPLIFETIME(AWoTBattleInfo, TeamClass);
	DOREPLIFETIME(AWoTBattleInfo, NumPlayers);
	DOREPLIFETIME(AWoTBattleInfo, NumSeals);
	DOREPLIFETIME(AWoTBattleInfo, bEditing);
	DOREPLIFETIME(AWoTBattleInfo, bEditingComplete);
	DOREPLIFETIME(AWoTBattleInfo, MaxTeamSize);
	DOREPLIFETIME(AWoTBattleInfo, bTeleportersDisabled);
}
