#pragma once

#include "CoreMinimal.h"
#include "WoTHUDDrawCmd.h"

struct FWoTCitadelTeamOverviewRow
{
	FString Description;
	int32 NumPlayers = 0;
	int32 MaxTeamSize = 0;
	FString Status;
	bool bHighlight = false;
};

struct FWoTCitadelTeamOverviewSnapshot
{
	FString MapName;
	bool bDisplayRemaining = false;
	int32 RemainingTime = 0;
	int32 ElapsedTime = 0;
	FString TeamLegend;
	FString PlayersLegend;
	FString StatusLegend;
	FString BeginBattleText;
	bool bAllTeamsReady = false;
	TArray<FWoTCitadelTeamOverviewRow> Teams;
};

// Pure CitadelTeamOverview model/layout. The HUD adapter gathers the always-relevant,
// replicated BattleInfo into this snapshot; this class only reproduces the shipped
// ArenaScoreBoard-derived 640-base column layout.
class FWoTCitadelTeamOverview
{
public:
	static constexpr int32 MaxTeams = 4;
	static constexpr int32 ColumnCount = 4;
	static constexpr int32 SlotSpacing = 20;

	TArray<FWoTHUDDrawCmd> Layout(const FWoTCitadelTeamOverviewSnapshot& Snapshot,
		int32 LogicalWidth, int32 LogicalHeight, float FontLineHeight) const;
};
