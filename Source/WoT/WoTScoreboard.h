#pragma once

#include "CoreMinimal.h"
#include "WoTHUDDrawCmd.h"

// ArenaScoreBoard.uc sort constants. Sort history stores signed values so a
// negative field reverses that comparison exactly as in the shipped script.
enum class EWoTScoreboardSortField : int8
{
	End = 0,
	Name = 1,
	Ping = 2,
	PacketLoss = 3,
	Kills = 4,
	Deaths = 5,
	Suicides = 6,
	Score = 7,
	TimeOnServer = 8,
	Color = 9,
	PlayerType = 10,
	// CitadelScoreBoard.uc:13-15.
	Team = 11,
	TeamSealCount = 12,
	TeamScore = 13
};

enum class EWoTScoreboardJustification : uint8
{
	Left,
	Right,
	Center,
	LeftQuarter,
	RightQuarter,
	LeftThird,
	RightThird
};

struct FWoTScoreboardDisplayInfo
{
	EWoTScoreboardJustification Justification =
		EWoTScoreboardJustification::Left;
	int32 Offset = 0;
	bool bEnabled = true;
};

struct FWoTScoreboardPlayer
{
	FString Name;
	int32 Ping = 0;
	// -1 is the Slice-A no-telemetry sentinel and displays as "--".
	int32 PacketLoss = -1;
	int32 Kills = 0;
	int32 Deaths = 0;
	int32 Suicides = 0;
	int32 Score = 0;
	int32 TimeOnServer = 0;
	uint8 Color = 0;
	uint8 PlayerType = 0;
	bool bHighlight = false;
	// CitadelScoreBoard.InitializePlayer (uc:121-128): per-row team identity/score
	// so the citadel sort keys ride the same stable-sort machinery.
	uint8 Team = 255;
	int32 TeamSealCount = 0;
	int32 TeamScore = 0;
};

struct FWoTScoreboardSnapshot
{
	FString MapName;
	bool bDisplayRemaining = false;
	int32 RemainingTime = 0;
	int32 ElapsedTime = 0;
	FString VictoryMessage;
	TArray<FWoTScoreboardPlayer> Players;
	// Citadel (SB-D3): CitadelGameReplicationInfo.SealGoal + BattleInfo team
	// descriptions, gathered by the HUD for the citadel board only.
	int32 SealGoal = 0;
	TMap<uint8, FString> TeamDescriptions;
};

// Pure Arena scoreboard model/layout. It deliberately knows nothing about a
// world, HUD, canvas, fonts, or texture registries.
class FWoTArenaScoreboard
{
public:
	static constexpr int32 MaxPlayers = 16;
	static constexpr int32 SortHistorySize = 10;
	static constexpr int32 ColumnCount = 6;
	static constexpr int32 IconSize = 16;
	static constexpr int32 ColorTextureCount = 5;
	static constexpr int32 PlayerTextureCount = 4;

	FWoTArenaScoreboard();
	virtual ~FWoTArenaScoreboard() = default;

	TArray<FWoTScoreboardPlayer> Players;
	// Oldest -> newest. Stable passes make the newest entry dominant.
	TArray<int32> SortOrder;

	void SortBy(int32 SortType);
	void Sort();
	void SortUsing(int32 SortType);
	bool IsGreater(int32 A, int32 B, int32 SortType) const;
	virtual bool HandleSortTypeException(int32 A, int32 B, int32 SortType) const;

	static FString FormatTime(int32 TimeSeconds, bool bNoSeconds = false);
	static int32 JustifiedX(const FWoTScoreboardDisplayInfo& DisplayInfo,
		int32 LogicalWidth);
	static FName ColorTextureId(uint8 Color);
	static FName PlayerTextureId(uint8 PlayerType);
	static TArray<FName> AllIconTextureIds();
	virtual TArray<FWoTHUDDrawCmd> Layout(const FWoTScoreboardSnapshot& Snapshot,
		int32 LogicalWidth, int32 LogicalHeight, float FontLineHeight,
		bool bStandalone, bool bIconsAvailable) const;

protected:
	// The shared row renderer (one player line at X columns / row tint) so the
	// citadel layout draws identical rows between its team blocks.
	void LayoutPlayerRow(TArray<FWoTHUDDrawCmd>& Commands,
		const FWoTScoreboardPlayer& Player, const int32* X, float Y,
		bool bStandalone, bool bIconsAvailable, float FontLineHeight) const;
	static void LayoutColumns(int32 LogicalWidth, int32* OutX);
	static void LayoutLegend(TArray<FWoTHUDDrawCmd>& Commands, const int32* X,
		float Y);

private:
	static int32 FieldValue(const FWoTScoreboardPlayer& Player,
		EWoTScoreboardSortField Field);
};

// SB-D3 (G12): the match-time citadel scoreboard (CitadelScoreBoard.uc). Rows
// gain Team/TeamSealCount/TeamScore; the sort history appends SORT_TEAM and
// SORT_TEAM_SCORE (defaultproperties SortOrder(4)=11/(5)=13 — newest dominant,
// so rows group per team ordered by team score); each team block opens with a
// centered "<Desc>    Seals: N    Score: N*10" header + the column legend, the
// top-of-board legend is suppressed (DrawLegend override, uc:107-111), the
// header gains "Number of seals to win: <SealGoal>" between map and clock
// (uc:187-203/315-348); Citadel's 14-UU row stride and initial -28 legend
// adjustment are retained. The authored slot-0 victory banner is deliberately
// made reachable before the header; PORT_NOTES records the retail MP divergence.
class FWoTCitadelScoreboard : public FWoTArenaScoreboard
{
public:
	FWoTCitadelScoreboard();
	virtual bool HandleSortTypeException(int32 A, int32 B,
		int32 SortType) const override;
	virtual TArray<FWoTHUDDrawCmd> Layout(const FWoTScoreboardSnapshot& Snapshot,
		int32 LogicalWidth, int32 LogicalHeight, float FontLineHeight,
		bool bStandalone, bool bIconsAvailable) const override;

	// CitadelScoreBoard.GetTeamDescription (uc:90-104): BattleInfo's description
	// with the "Team #N+1" fallback (also on an empty description).
	static FString TeamDescription(const FWoTScoreboardSnapshot& Snapshot,
		uint8 Team);
};
