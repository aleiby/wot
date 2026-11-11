#include "WoTCitadelTeamOverview.h"

#include "WoTScoreboard.h"

namespace
{
	FWoTHUDDrawCmd& CitadelOverviewAddText(
		TArray<FWoTHUDDrawCmd>& Commands, const FString& Text,
		float X, float Y, const FLinearColor& Tint,
		EWoTTextAlignment Alignment = EWoTTextAlignment::Left)
	{
		FWoTHUDDrawCmd& Command = Commands.AddDefaulted_GetRef();
		Command.Text = Text;
		Command.FontId = FName(TEXT("F_WOTReg14"));
		Command.X = X;
		Command.Y = Y;
		Command.Tint = Tint;
		Command.TextAlignment = Alignment;
		return Command;
	}
}

TArray<FWoTHUDDrawCmd> FWoTCitadelTeamOverview::Layout(
	const FWoTCitadelTeamOverviewSnapshot& Snapshot, int32 LogicalWidth,
	int32 LogicalHeight, float FontLineHeight,
	const FWoTScoreboardLayoutOptions& Options) const
{
	TArray<FWoTHUDDrawCmd> Commands;
	const FLinearColor White = FLinearColor::White;
	const int32 LineHeight = FMath::Max(FMath::TruncToInt(FontLineHeight), 0);
	const float CenterX = static_cast<float>(LogicalWidth) * 0.5f;

	// CitadelTeamOverview inherits ArenaScoreBoard.DrawHeader unchanged.
	const int32 ClockSeconds = Snapshot.bDisplayRemaining
		? FMath::Max(Snapshot.RemainingTime, 0) : Snapshot.ElapsedTime;
	TArray<FString> Headers;
	for (const FString& Header : {Snapshot.MapName,
		FString(Snapshot.bDisplayRemaining
			? TEXT("Remaining Time: ") : TEXT("Elapsed Time: ")) +
			FWoTArenaScoreboard::FormatTime(ClockSeconds)})
	{
		if (!Header.IsEmpty())
		{
			Headers.Add(Header);
		}
	}
	// The legend's Y: a quarter of the board, moved down only when the
	// options keep header lines that would reach it (as the scoreboards do).
	const int32 LegendY = Options.bKeepHeaders
		? FMath::Max(LogicalHeight / 4, LineHeight * 4 + Headers.Num() * LineHeight + 1)
		: LogicalHeight / 4;
	const int32 HeaderLimit = LegendY - LineHeight;
	int32 HeaderY = LineHeight * 4;
	for (const FString& Header : Headers)
	{
		if (HeaderY < HeaderLimit)
		{
			CitadelOverviewAddText(Commands, Header, CenterX,
				static_cast<float>(HeaderY), White, EWoTTextAlignment::Center);
			HeaderY += LineHeight;
		}
	}

	// CitadelTeamOverview.defaultproperties: index 0 retains Arena's LeftThird;
	// indices 1..3 become LeftThird+16, Center+8, RightThird respectively.
	const FWoTScoreboardDisplayInfo DisplayInfo[ColumnCount] = {
		{ EWoTScoreboardJustification::LeftThird, 0, true },
		{ EWoTScoreboardJustification::LeftThird, 16, true },
		{ EWoTScoreboardJustification::Center, 8, true },
		{ EWoTScoreboardJustification::RightThird, 0, true }
	};
	int32 X[ColumnCount];
	for (int32 Column = 0; Column < ColumnCount; ++Column)
	{
		X[Column] = FWoTArenaScoreboard::JustifiedX(
			DisplayInfo[Column], LogicalWidth);
	}

	const FString Legend[ColumnCount] = {
		Snapshot.TeamLegend, FString(), Snapshot.PlayersLegend,
		Snapshot.StatusLegend
	};
	for (int32 Column = 0; Column < ColumnCount; ++Column)
	{
		if (!Legend[Column].IsEmpty())
		{
			CitadelOverviewAddText(Commands, Legend[Column],
				static_cast<float>(X[Column]), static_cast<float>(LegendY), White);
		}
	}

	const int32 TeamCount = FMath::Min(Snapshot.Teams.Num(), MaxTeams);
	for (int32 Team = 0; Team < TeamCount; ++Team)
	{
		const FWoTCitadelTeamOverviewRow& Row = Snapshot.Teams[Team];
		const float Y = static_cast<float>(LegendY + (Team + 1) * SlotSpacing);
		const FLinearColor Tint = Row.bHighlight ? White
			: FLinearColor::FromSRGBColor(FColor(120, 120, 120));
		const FString TeamDescription = Row.Description.IsEmpty()
			? FString::Printf(TEXT("Team #%d"), Team + 1) : Row.Description;

		CitadelOverviewAddText(Commands,
			FString::Printf(TEXT("%d)"), Team + 1),
			static_cast<float>(X[0]), Y, Tint);
		CitadelOverviewAddText(Commands, TeamDescription,
			static_cast<float>(X[1]), Y, Tint);
		CitadelOverviewAddText(Commands,
			FString::Printf(TEXT("%d (%d)"), Row.NumPlayers, Row.MaxTeamSize),
			static_cast<float>(X[2]), Y, Tint);
		CitadelOverviewAddText(Commands, Row.Status,
			static_cast<float>(X[3]), Y, Tint);
	}

	// ShowScores draws this only after at least one replicated team exists and every
	// GetTeamStatus result exactly equals BattleInfo.default.ReadyStr. Canvas.CurY is
	// one font line below the last row after its final DrawText.
	if (TeamCount > 0 && Snapshot.bAllTeamsReady &&
		!Snapshot.BeginBattleText.IsEmpty())
	{
		const float BeginY = static_cast<float>(
			LegendY + TeamCount * SlotSpacing + LineHeight);
		CitadelOverviewAddText(Commands, Snapshot.BeginBattleText,
			CenterX, BeginY, White, EWoTTextAlignment::Center);
	}
	return Commands;
}

FWoTScoreboardBoard FWoTCitadelTeamOverview::LayoutBoard(
	const FWoTCitadelTeamOverviewSnapshot& Snapshot, int32 CanvasSizeX,
	int32 CanvasSizeY, float FontLineHeight, int32 TextScale) const
{
	return FWoTScoreboardBoardChoice::Choose(CanvasSizeX, CanvasSizeY,
		FontLineHeight, TextScale, /*bHasCompactDensity*/ false, 0.0f,
		[&](int32 Width, int32 Height, const FWoTScoreboardLayoutOptions& Options)
		{
			return Layout(Snapshot, Width, Height, FontLineHeight, Options);
		});
}
