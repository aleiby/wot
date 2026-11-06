#include "WoTScoreboard.h"

namespace
{
	const FName ScoreboardColorTextureIds[FWoTArenaScoreboard::ColorTextureCount] = {
		TEXT("HeadsBase"), TEXT("HeadsBlack"), TEXT("HeadsBlue"),
		TEXT("HeadsRed"), TEXT("HeadsGreen")
	};
	const FName ScoreboardPlayerTextureIds[FWoTArenaScoreboard::PlayerTextureCount] = {
		TEXT("HeadsAesSedai"), TEXT("HeadsForsaken"), TEXT("HeadsHound"),
		TEXT("HeadsWC")
	};

	constexpr int32 ScoreboardSort(EWoTScoreboardSortField Field)
	{
		return static_cast<int32>(Field);
	}

	FWoTHUDDrawCmd& ScoreboardAddText(TArray<FWoTHUDDrawCmd>& Commands,
		const FString& Text, float X, float Y, const FLinearColor& Tint,
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

	FWoTHUDDrawCmd& ScoreboardAddTexture(TArray<FWoTHUDDrawCmd>& Commands,
		FName TextureId, float X, float Y, EWoTHUDStyle Style,
		const FLinearColor& Tint)
	{
		FWoTHUDDrawCmd& Command = Commands.AddDefaulted_GetRef();
		Command.TexId = TextureId;
		Command.X = X;
		Command.Y = Y;
		Command.Scale = 1.0f;
		Command.Style = Style;
		Command.Tint = Tint;
		return Command;
	}
}

FWoTArenaScoreboard::FWoTArenaScoreboard()
{
	SortOrder.SetNumZeroed(SortHistorySize);
	SortOrder[0] = ScoreboardSort(EWoTScoreboardSortField::Ping);
	SortOrder[1] = ScoreboardSort(EWoTScoreboardSortField::PacketLoss);
	SortOrder[2] = -ScoreboardSort(EWoTScoreboardSortField::TimeOnServer);
	SortOrder[3] = ScoreboardSort(EWoTScoreboardSortField::Score);
}

void FWoTArenaScoreboard::SortBy(int32 SortType)
{
	const int32 Field = FMath::Abs(SortType);
	TArray<int32> Active;
	for (int32 Existing : SortOrder)
	{
		if (Existing != ScoreboardSort(EWoTScoreboardSortField::End) &&
			FMath::Abs(Existing) != Field)
		{
			Active.Add(Existing);
		}
	}
	if (Active.Num() >= SortHistorySize)
	{
		Active.RemoveAt(0);
	}
	Active.Add(SortType);
	SortOrder.Init(ScoreboardSort(EWoTScoreboardSortField::End), SortHistorySize);
	for (int32 Index = 0; Index < Active.Num(); ++Index)
	{
		SortOrder[Index] = Active[Index];
	}
}

void FWoTArenaScoreboard::Sort()
{
	for (int32 Index = 0;
		Index < SortOrder.Num() && Index < SortHistorySize &&
		SortOrder[Index] != ScoreboardSort(EWoTScoreboardSortField::End);
		++Index)
	{
		SortUsing(SortOrder[Index]);
	}
}

void FWoTArenaScoreboard::SortUsing(int32 SortType)
{
	const int32 Count = FMath::Min(Players.Num(), MaxPlayers);
	for (int32 I = 0; I < Count - 1; ++I)
	{
		for (int32 J = Count - 1; J > I; --J)
		{
			if (IsGreater(J, J - 1, SortType))
			{
				Players.Swap(J, J - 1);
			}
		}
	}
}

int32 FWoTArenaScoreboard::FieldValue(const FWoTScoreboardPlayer& Player,
	EWoTScoreboardSortField Field)
{
	switch (Field)
	{
	case EWoTScoreboardSortField::Ping: return Player.Ping;
	case EWoTScoreboardSortField::PacketLoss: return Player.PacketLoss;
	case EWoTScoreboardSortField::Kills: return Player.Kills;
	case EWoTScoreboardSortField::Deaths: return Player.Deaths;
	case EWoTScoreboardSortField::Suicides: return Player.Suicides;
	case EWoTScoreboardSortField::Score: return Player.Score;
	case EWoTScoreboardSortField::TimeOnServer: return Player.TimeOnServer / 60;
	case EWoTScoreboardSortField::Color: return Player.Color;
	case EWoTScoreboardSortField::PlayerType: return Player.PlayerType;
	default: return 0;
	}
}

bool FWoTArenaScoreboard::IsGreater(int32 A, int32 B, int32 SortType) const
{
	if (A == B || !Players.IsValidIndex(A) || !Players.IsValidIndex(B))
	{
		return false;
	}

	const EWoTScoreboardSortField Field =
		static_cast<EWoTScoreboardSortField>(FMath::Abs(SortType));
	if (Field == EWoTScoreboardSortField::Name)
	{
		const int32 Comparison = Players[A].Name.Compare(Players[B].Name,
			ESearchCase::CaseSensitive);
		return SortType < 0 ? Comparison < 0 : Comparison > 0;
	}
	if (Field < EWoTScoreboardSortField::Ping ||
		Field > EWoTScoreboardSortField::PlayerType)
	{
		return HandleSortTypeException(A, B, SortType);
	}

	const int32 ValueA = FieldValue(Players[A], Field);
	const int32 ValueB = FieldValue(Players[B], Field);
	return SortType < 0 ? ValueA < ValueB : ValueA > ValueB;
}

bool FWoTArenaScoreboard::HandleSortTypeException(
	int32 /*A*/, int32 /*B*/, int32 /*SortType*/) const
{
	return false;
}

FString FWoTArenaScoreboard::FormatTime(int32 TimeSeconds, bool bNoSeconds)
{
	const int32 SecondsTotal = FMath::Max(TimeSeconds, 0);
	const int32 MinutesTotal = SecondsTotal / 60;
	const int32 Hours = MinutesTotal / 60;
	const int32 Minutes = MinutesTotal - Hours * 60;
	const int32 Seconds = SecondsTotal - MinutesTotal * 60;
	return bNoSeconds
		? FString::Printf(TEXT("%02d:%02d"), Hours, Minutes)
		: FString::Printf(TEXT("%02d:%02d:%02d"), Hours, Minutes, Seconds);
}

int32 FWoTArenaScoreboard::JustifiedX(
	const FWoTScoreboardDisplayInfo& DisplayInfo, int32 LogicalWidth)
{
	int32 X = 0;
	switch (DisplayInfo.Justification)
	{
	case EWoTScoreboardJustification::Right: X = LogicalWidth; break;
	case EWoTScoreboardJustification::Center: X = LogicalWidth / 2; break;
	case EWoTScoreboardJustification::LeftQuarter: X = LogicalWidth / 4; break;
	case EWoTScoreboardJustification::RightQuarter: X = LogicalWidth / 4 * 3; break;
	case EWoTScoreboardJustification::LeftThird: X = LogicalWidth / 3; break;
	case EWoTScoreboardJustification::RightThird: X = LogicalWidth / 3 * 2; break;
	case EWoTScoreboardJustification::Left:
	default: break;
	}
	return X + DisplayInfo.Offset;
}

FName FWoTArenaScoreboard::ColorTextureId(uint8 Color)
{
	const int32 Index = Color < ColorTextureCount ? Color : 0;
	return ScoreboardColorTextureIds[Index];
}

FName FWoTArenaScoreboard::PlayerTextureId(uint8 PlayerType)
{
	const int32 Index = PlayerType < PlayerTextureCount ? PlayerType : 0;
	return ScoreboardPlayerTextureIds[Index];
}

TArray<FName> FWoTArenaScoreboard::AllIconTextureIds()
{
	TArray<FName> TextureIds;
	TextureIds.Reserve(ColorTextureCount + PlayerTextureCount);
	for (const FName TextureId : ScoreboardColorTextureIds)
	{
		TextureIds.Add(TextureId);
	}
	for (const FName TextureId : ScoreboardPlayerTextureIds)
	{
		TextureIds.Add(TextureId);
	}
	return TextureIds;
}

TArray<FWoTHUDDrawCmd> FWoTArenaScoreboard::Layout(
	const FWoTScoreboardSnapshot& Snapshot, int32 LogicalWidth,
	int32 LogicalHeight, float FontLineHeight, bool bStandalone,
	bool bIconsAvailable) const
{
	TArray<FWoTHUDDrawCmd> Commands;
	const FLinearColor White = FLinearColor::White;
	const int32 LineHeight = FMath::Max(FMath::TruncToInt(FontLineHeight), 0);
	const int32 HeaderLimit = LogicalHeight / 4 - LineHeight;
	int32 HeaderY = LineHeight * 4;
	const float CenterX = static_cast<float>(LogicalWidth) * 0.5f;

	auto AddHeader = [&](const FString& Text)
	{
		if (!Text.IsEmpty() && HeaderY < HeaderLimit)
		{
			ScoreboardAddText(Commands, Text, CenterX,
				static_cast<float>(HeaderY), White, EWoTTextAlignment::Center);
			HeaderY += LineHeight;
		}
	};

	AddHeader(Snapshot.MapName);
	const int32 ClockSeconds = Snapshot.bDisplayRemaining
		? FMath::Max(Snapshot.RemainingTime, 0) : Snapshot.ElapsedTime;
	AddHeader(FString(Snapshot.bDisplayRemaining
		? TEXT("Remaining Time: ") : TEXT("Elapsed Time: ")) +
		FormatTime(ClockSeconds));
	AddHeader(Snapshot.VictoryMessage);

	int32 X[ColumnCount];
	LayoutColumns(LogicalWidth, X);

	const int32 LegendY = LogicalHeight / 4;
	LayoutLegend(Commands, X, static_cast<float>(LegendY));

	FWoTArenaScoreboard Sorted = *this;
	Sorted.Players = Snapshot.Players;
	if (Sorted.Players.Num() > MaxPlayers)
	{
		Sorted.Players.SetNum(MaxPlayers);
	}
	Sorted.Sort();

	for (int32 Row = 0; Row < Sorted.Players.Num(); ++Row)
	{
		LayoutPlayerRow(Commands, Sorted.Players[Row], X,
			static_cast<float>(LegendY + (Row + 1) * 20), bStandalone,
			bIconsAvailable, FontLineHeight);
	}
	return Commands;
}

void FWoTArenaScoreboard::LayoutColumns(int32 LogicalWidth, int32* OutX)
{
	const FWoTScoreboardDisplayInfo DisplayInfo[ColumnCount] = {
		{ EWoTScoreboardJustification::LeftThird, 0, true },
		{ EWoTScoreboardJustification::LeftQuarter, -72, true },
		{ EWoTScoreboardJustification::LeftQuarter, -48, true },
		{ EWoTScoreboardJustification::Center, -48, true },
		{ EWoTScoreboardJustification::RightThird, -56, true },
		{ EWoTScoreboardJustification::RightQuarter, 32, true }
	};
	for (int32 Column = 0; Column < ColumnCount; ++Column)
	{
		OutX[Column] = JustifiedX(DisplayInfo[Column], LogicalWidth);
	}
}

void FWoTArenaScoreboard::LayoutLegend(TArray<FWoTHUDDrawCmd>& Commands,
	const int32* X, float Y)
{
	const FString Legend[ColumnCount] = {
		TEXT("Time"), TEXT(""), TEXT("Name"), TEXT("Ping/PL"),
		TEXT("Kills/Deaths/Suicides"), TEXT("Score")
	};
	for (int32 Column = 0; Column < ColumnCount; ++Column)
	{
		// The icon column is enabled for rows but deliberately has no legend label.
		if (!Legend[Column].IsEmpty())
		{
			ScoreboardAddText(Commands, Legend[Column],
				static_cast<float>(X[Column]), Y, FLinearColor::White);
		}
	}
}

void FWoTArenaScoreboard::LayoutPlayerRow(TArray<FWoTHUDDrawCmd>& Commands,
	const FWoTScoreboardPlayer& Player, const int32* X, float Y,
	bool bStandalone, bool bIconsAvailable, float FontLineHeight) const
{
	const FLinearColor Tint = Player.bHighlight ? FLinearColor::White
		: FLinearColor::FromSRGBColor(FColor(120, 120, 120));

	ScoreboardAddText(Commands, FormatTime(Player.TimeOnServer, true),
		static_cast<float>(X[0]), Y, Tint);
	if (bIconsAvailable)
	{
		// ArenaScoreBoard.uc:145-159: Canvas.DrawColor is the ROW color when
		// DrawPlayerElements runs, and DrawIcon draws through it — retail dims a
		// non-highlighted row's color swatch + head exactly like the row's text.
		const float IconY = Y - (IconSize - FontLineHeight);
		ScoreboardAddTexture(Commands, ColorTextureId(Player.Color),
			static_cast<float>(X[1]), IconY, EWoTHUDStyle::Normal, Tint);
		ScoreboardAddTexture(Commands, PlayerTextureId(Player.PlayerType),
			static_cast<float>(X[1]), IconY, EWoTHUDStyle::Masked, Tint);
	}
	ScoreboardAddText(Commands, Player.Name, static_cast<float>(X[2]), Y, Tint);
	if (!bStandalone)
	{
		const FString Loss = Player.PacketLoss >= 0
			? FString::FromInt(Player.PacketLoss) : FString(TEXT("--"));
		ScoreboardAddText(Commands,
			FString::Printf(TEXT("(%d/%s)"), Player.Ping, *Loss),
			static_cast<float>(X[3]), Y, Tint);
	}
	ScoreboardAddText(Commands,
		FString::Printf(TEXT("%d/%d/%d"), Player.Kills,
			Player.Deaths, Player.Suicides), static_cast<float>(X[4]), Y, Tint);
	ScoreboardAddText(Commands, FString::FromInt(Player.Score),
		static_cast<float>(X[5] + 24), Y, Tint, EWoTTextAlignment::Right);
}

FWoTCitadelScoreboard::FWoTCitadelScoreboard()
{
	// CitadelScoreBoard defaultproperties: SortOrder(4)=11, SortOrder(5)=13 — the
	// UE1 fixed-array override keeps the inherited 0-3 and replaces exactly these
	// two slots. Newest entries dominate the stable passes, so rows group per team
	// ordered by team score.
	SortOrder[4] = ScoreboardSort(EWoTScoreboardSortField::Team);
	SortOrder[5] = ScoreboardSort(EWoTScoreboardSortField::TeamScore);
}

bool FWoTCitadelScoreboard::HandleSortTypeException(int32 A, int32 B,
	int32 SortType) const
{
	// CitadelScoreBoard.uc:144-184.
	const EWoTScoreboardSortField Field =
		static_cast<EWoTScoreboardSortField>(FMath::Abs(SortType));
	int32 ValueA = 0;
	int32 ValueB = 0;
	switch (Field)
	{
	case EWoTScoreboardSortField::Team:
		ValueA = Players[A].Team;
		ValueB = Players[B].Team;
		break;
	case EWoTScoreboardSortField::TeamSealCount:
		ValueA = Players[A].TeamSealCount;
		ValueB = Players[B].TeamSealCount;
		break;
	case EWoTScoreboardSortField::TeamScore:
		ValueA = Players[A].TeamScore;
		ValueB = Players[B].TeamScore;
		break;
	default:
		return FWoTArenaScoreboard::HandleSortTypeException(A, B, SortType);
	}
	return SortType < 0 ? ValueA < ValueB : ValueA > ValueB;
}

FString FWoTCitadelScoreboard::TeamDescription(
	const FWoTScoreboardSnapshot& Snapshot, uint8 Team)
{
	const FString* Found = Snapshot.TeamDescriptions.Find(Team);
	return (Found && !Found->IsEmpty())
		? *Found
		: FString::Printf(TEXT("Team #%d"), Team + 1);
}

TArray<FWoTHUDDrawCmd> FWoTCitadelScoreboard::Layout(
	const FWoTScoreboardSnapshot& Snapshot, int32 LogicalWidth,
	int32 LogicalHeight, float FontLineHeight, bool bStandalone,
	bool bIconsAvailable) const
{
	TArray<FWoTHUDDrawCmd> Commands;
	const FLinearColor White = FLinearColor::White;
	const int32 LineHeight = FMath::Max(FMath::TruncToInt(FontLineHeight), 0);
	const int32 HeaderLimit = LogicalHeight / 4 - LineHeight;
	int32 HeaderY = LineHeight * 4;
	const float CenterX = static_cast<float>(LogicalWidth) * 0.5f;

	auto AddHeader = [&](const FString& Text)
	{
		if (!Text.IsEmpty() && HeaderY < HeaderLimit)
		{
			ScoreboardAddText(Commands, Text, CenterX,
				static_cast<float>(HeaderY), White, EWoTTextAlignment::Center);
			HeaderY += LineHeight;
		}
	};

	FWoTCitadelScoreboard Sorted = *this;
	Sorted.Players = Snapshot.Players;
	if (Sorted.Players.Num() > MaxPlayers)
	{
		Sorted.Players.SetNum(MaxPlayers);
	}
	Sorted.Sort();

	// CitadelScoreBoard.uc:29-36 authors this banner from slot 0, but every later
	// row clears VictoryMessage after ArenaScoreBoard.DrawHeader has already read
	// the previous frame. The port deliberately computes the authored result before
	// the header so multiplayer can show it; PORT_NOTES records this divergence.
	FString VictoryMessage = Snapshot.VictoryMessage;
	if (VictoryMessage.IsEmpty() && Sorted.Players.Num() > 0
		&& Snapshot.SealGoal > 0
		&& Sorted.Players[0].TeamSealCount >= Snapshot.SealGoal)
	{
		VictoryMessage = FString::Printf(TEXT("[[ Victory goes to %s ]]"),
			*TeamDescription(Snapshot, Sorted.Players[0].Team));
	}

	AddHeader(Snapshot.MapName);
	// DrawAdditionalHeaderElements (uc:187-203), shipped WoT.int NumSealsText.
	AddHeader(FString::Printf(TEXT("Number of seals to win: %d"),
		Snapshot.SealGoal));
	const int32 ClockSeconds = Snapshot.bDisplayRemaining
		? FMath::Max(Snapshot.RemainingTime, 0) : Snapshot.ElapsedTime;
	AddHeader(FString(Snapshot.bDisplayRemaining
		? TEXT("Remaining Time: ") : TEXT("Elapsed Time: ")) +
		FormatTime(ClockSeconds));
	// ArenaScoreBoard.DrawHeader draws VictoryMessage after the clock (uc:380-386).
	AddHeader(VictoryMessage);

	int32 X[ColumnCount];
	LayoutColumns(LogicalWidth, X);

	// DrawLegend override (uc:107-111): no top-of-board legend — each team block
	// carries its own (DrawTeamHeader, uc:50-87).
	constexpr int32 CitadelSlotSpacing = 14;
	// Arena starts at ClipY/4, then Citadel.DrawLegend subtracts SlotSpacing*2.
	int32 RunningY = LogicalHeight / 4 - CitadelSlotSpacing * 2;
	uint8 PrevTeam = 255;
	for (int32 Row = 0; Row < Sorted.Players.Num(); ++Row)
	{
		const FWoTScoreboardPlayer& Player = Sorted.Players[Row];
		// ArenaScoreBoard.ShowScores advances SlotSpacing before DrawPlayerElements.
		RunningY += CitadelSlotSpacing;
		if (Player.Team != PrevTeam)
		{
			RunningY += LineHeight;   // space (uc:66)
			if (Player.Team != 255)
			{
				ScoreboardAddText(Commands, FString::Printf(
						TEXT("%s    Seals: %d    Score: %d"),
						*TeamDescription(Snapshot, Player.Team),
						Player.TeamSealCount, Player.TeamScore),
					CenterX, static_cast<float>(RunningY), White,
					EWoTTextAlignment::Center);
			}
			RunningY += LineHeight;
			RunningY += LineHeight;   // space (uc:79)
			LayoutLegend(Commands, X, static_cast<float>(RunningY));
			RunningY += LineHeight;
		}
		PrevTeam = Player.Team;
		LayoutPlayerRow(Commands, Player, X, static_cast<float>(RunningY),
			bStandalone, bIconsAvailable, FontLineHeight);
	}
	return Commands;
}
