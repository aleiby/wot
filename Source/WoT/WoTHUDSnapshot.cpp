#include "WoTHUDSnapshot.h"

#include "WoTHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"

namespace
{
/** The HUD's own unit: its icons are 64 pixels, its notches 8. */
constexpr float WoTHUDLayout_IconUnit = 64.0f;
constexpr float WoTHUDLayout_Padding = 8.0f;
constexpr float WoTHUDLayout_Gap = 8.0f;
/** The reading board keeps the scoreboard's 640-unit column design. */
constexpr int32 WoTHUDLayout_BoardWidth = 640;

const AWoTHUD& WoTHUDLayout_Resolver()
{
	return *GetDefault<AWoTHUD>();
}

/** A command's drawn extent in its section's units. Texture sizes the
 * headless runtime cannot read fall back to the HUD's icon unit. */
FBox2D WoTHUDLayout_CommandBounds(EWoTHUDSection Section, const FWoTHUDDrawCmd& Command,
	int32 FontWidth)
{
	const float Scale = Command.Scale == 0.0f ? 1.0f : Command.Scale;
	FVector2D Size(0.0, 0.0);
	FVector2D Min(Command.X, Command.Y);
	if (!Command.Text.IsEmpty() || Section == EWoTHUDSection::Messages
		|| Section == EWoTHUDSection::TeamPrompt || Section == EWoTHUDSection::PlayerID)
	{
		if (const UFont* Font = WoTHUDLayout_Resolver().ResolveCommandFont(Command, FontWidth))
		{
			const FString Measured = Command.Text.IsEmpty() ? FString(TEXT(" ")) : Command.Text;
			Size = FVector2D(Font->GetStringSize(*Measured), Font->GetStringHeightSize(*Measured))
				* Scale;
		}
		if (Command.bCenterX || Command.TextAlignment == EWoTTextAlignment::Center)
		{
			Min.X -= Size.X * 0.5;
		}
		else if (Command.TextAlignment == EWoTTextAlignment::Right)
		{
			Min.X -= Size.X;
		}
	}
	else
	{
		const UTexture2D* Texture = WoTHUDLayout_Resolver().ResolveCommandTexture(Section, Command);
		const float Width = Texture && Texture->GetSizeX() > 0 ? Texture->GetSizeX() : WoTHUDLayout_IconUnit;
		const float Height = Texture && Texture->GetSizeY() > 0 ? Texture->GetSizeY() : WoTHUDLayout_IconUnit;
		Size = FVector2D(Width, Height) * Scale;
	}
	return FBox2D(Min, Min + Size);
}

FBox2D WoTHUDLayout_SectionBounds(const FWoTHUDSection& Section, int32 FontWidth)
{
	FBox2D Bounds(ForceInit);
	for (const FWoTHUDDrawCmd& Command : Section.Commands)
	{
		Bounds += WoTHUDLayout_CommandBounds(Section.Section, Command, FontWidth);
	}
	return Bounds;
}

/** The section moved by Offset and drawn with unit factors. */
FWoTHUDSection WoTHUDLayout_Moved(const FWoTHUDSection& Section, const FVector2D& Offset)
{
	FWoTHUDSection Moved = Section;
	if (Moved.Path == EWoTHUDDrawPath::Scaled)
	{
		Moved.Path = EWoTHUDDrawPath::Unscaled;
	}
	Moved.BoardScale = 1;
	for (FWoTHUDDrawCmd& Command : Moved.Commands)
	{
		Command.X += static_cast<float>(Offset.X);
		Command.Y += static_cast<float>(Offset.Y);
	}
	return Moved;
}

/** The largest whole scale at which Extent fits Size (at least 1). */
int32 WoTHUDLayout_FitScale(const FIntPoint& Size, const FVector2D& Extent)
{
	if (Extent.X <= 0.0 || Extent.Y <= 0.0)
	{
		return 1;
	}
	return FMath::Max(1, FMath::Min(FMath::FloorToInt32(Size.X / Extent.X),
		FMath::FloorToInt32(Size.Y / Extent.Y)));
}

float WoTHUDLayout_LineHeight(int32 FontWidth)
{
	FWoTHUDDrawCmd Probe;
	Probe.FontId = FName(TEXT("F_WOTReg14"));
	const UFont* Font = WoTHUDLayout_Resolver().ResolveCommandFont(Probe, FontWidth);
	return Font ? static_cast<float>(Font->GetStringHeightSize(TEXT("TEST"))) : 14.0f;
}

FWoTHUDLayoutResult WoTHUDLayout_Compact(const FWoTHUDSnapshot& Snapshot,
	const FWoTHUDLayoutOptions& Options)
{
	FWoTHUDLayoutResult Result;
	const int32 FontWidth = FMath::Max(Snapshot.CanvasSize.X, 640);
	float StripBottom = WoTHUDLayout_Padding;
	float StripRight = WoTHUDLayout_Padding;
	// The strip: health, then the status icons, then the warning and flash.
	for (const EWoTHUDSection Part : {EWoTHUDSection::Health, EWoTHUDSection::StatusIcons,
		EWoTHUDSection::Warning, EWoTHUDSection::Flash})
	{
		const FWoTHUDSection* Section = Snapshot.FindSection(Part);
		if (!Section)
		{
			continue;
		}
		const FBox2D Bounds = WoTHUDLayout_SectionBounds(*Section, FontWidth);
		const FVector2D Offset(StripRight - Bounds.Min.X, WoTHUDLayout_Padding - Bounds.Min.Y);
		Result.Sections.Add(WoTHUDLayout_Moved(*Section, Offset));
		StripRight += Bounds.GetSize().X + WoTHUDLayout_Gap;
		StripBottom = FMath::Max(StripBottom, WoTHUDLayout_Padding + static_cast<float>(Bounds.GetSize().Y));
	}
	float Width = StripRight;
	// The groups below, keeping the fan and the slide where they sit above
	// the bar.
	FBox2D HandBounds(ForceInit);
	for (const EWoTHUDSection Part : {EWoTHUDSection::CurrentHand, EWoTHUDSection::AddedItems,
		EWoTHUDSection::Hands})
	{
		if (const FWoTHUDSection* Section = Snapshot.FindSection(Part))
		{
			HandBounds += WoTHUDLayout_SectionBounds(*Section, FontWidth);
		}
	}
	float Bottom = StripBottom;
	if (HandBounds.bIsValid)
	{
		const FVector2D Offset(WoTHUDLayout_Padding - HandBounds.Min.X,
			StripBottom + WoTHUDLayout_Gap - HandBounds.Min.Y);
		for (const EWoTHUDSection Part : {EWoTHUDSection::CurrentHand,
			EWoTHUDSection::AddedItems, EWoTHUDSection::Hands})
		{
			if (const FWoTHUDSection* Section = Snapshot.FindSection(Part))
			{
				Result.Sections.Add(WoTHUDLayout_Moved(*Section, Offset));
			}
		}
		Bottom = StripBottom + WoTHUDLayout_Gap + static_cast<float>(HandBounds.GetSize().Y);
		Width = FMath::Max(Width, WoTHUDLayout_Padding * 2.0f + static_cast<float>(HandBounds.GetSize().X));
	}
	// Then every text line, left-aligned, in the flat HUD's order.
	float LineY = Bottom + WoTHUDLayout_Gap;
	for (const EWoTHUDSection Part : {EWoTHUDSection::Progress, EWoTHUDSection::TeamPrompt,
		EWoTHUDSection::Messages, EWoTHUDSection::PlayerID})
	{
		const FWoTHUDSection* Section = Snapshot.FindSection(Part);
		if (!Section)
		{
			continue;
		}
		FWoTHUDSection Lines;
		Lines.Section = Part;
		Lines.Path = EWoTHUDDrawPath::TextItems;
		for (const FWoTHUDDrawCmd& Command : Section->Commands)
		{
			FWoTHUDDrawCmd Line = Command;
			Line.bCenterX = false;
			Line.TextAlignment = EWoTTextAlignment::Left;
			if (Line.Style == EWoTHUDStyle::Normal)
			{
				Line.Style = EWoTHUDStyle::Masked;
			}
			Line.X = WoTHUDLayout_Padding;
			Line.Y = LineY;
			const FBox2D Bounds = WoTHUDLayout_CommandBounds(Part, Line, FontWidth);
			LineY += static_cast<float>(Bounds.GetSize().Y);
			Width = FMath::Max(Width, WoTHUDLayout_Padding * 2.0f + static_cast<float>(Bounds.GetSize().X));
			Lines.Commands.Add(MoveTemp(Line));
		}
		if (!Lines.Commands.IsEmpty())
		{
			Result.Sections.Add(MoveTemp(Lines));
		}
	}
	const FVector2D Extent(Width, LineY + WoTHUDLayout_Padding);
	const int32 Scale = WoTHUDLayout_FitScale(Options.Size, Extent);
	for (FWoTHUDSection& Section : Result.Sections)
	{
		Section.BoardScale = Scale;
	}
	Result.Extent = FIntPoint(FMath::CeilToInt32(Extent.X * Scale),
		FMath::CeilToInt32(Extent.Y * Scale));
	return Result;
}

FWoTHUDLayoutResult WoTHUDLayout_Reading(const FWoTHUDSnapshot& Snapshot,
	const FWoTHUDLayoutOptions& Options)
{
	FWoTHUDLayoutResult Result;
	// The board units: at least the scoreboard's 640-unit design wide.
	const int32 Scale = FMath::Max(1, Options.Size.X / WoTHUDLayout_BoardWidth);
	const int32 Width = FMath::Max(Options.Size.X / Scale, 1);
	const int32 Height = FMath::Max(Options.Size.Y / Scale, 1);
	const int32 FontWidth = Options.Size.X;
	const float LineHeight = WoTHUDLayout_LineHeight(FontWidth);
	Result.Extent = FIntPoint(Width * Scale, Height * Scale);
	auto AddSection = [&Result, Scale](EWoTHUDSection Part, EWoTHUDDrawPath Path,
		TArray<FWoTHUDDrawCmd>&& Commands)
	{
		FWoTHUDSection& Added = Result.Sections.AddDefaulted_GetRef();
		Added.Section = Part;
		Added.Path = Path;
		Added.BoardScale = Scale;
		Added.Commands = MoveTemp(Commands);
	};
	if (Snapshot.bScoreboard)
	{
		const FWoTArenaScoreboard ArenaBoard;
		const FWoTCitadelScoreboard CitadelBoard;
		const FWoTArenaScoreboard& Board = Snapshot.bCitadelScoreboard
			? static_cast<const FWoTArenaScoreboard&>(CitadelBoard) : ArenaBoard;
		bool bIconsAvailable = true;
		for (const FName TextureId : FWoTArenaScoreboard::AllIconTextureIds())
		{
			FWoTHUDDrawCmd Probe;
			Probe.TexId = TextureId;
			bIconsAvailable &= WoTHUDLayout_Resolver().ResolveCommandTexture(
				EWoTHUDSection::Scoreboard, Probe) != nullptr;
		}
		// Every row, in the board's own order, a page at a time.
		FWoTCitadelScoreboard SortedCitadel;
		FWoTArenaScoreboard SortedArena;
		FWoTArenaScoreboard& Sorted = Snapshot.bCitadelScoreboard
			? static_cast<FWoTArenaScoreboard&>(SortedCitadel) : SortedArena;
		Sorted.Players = Snapshot.Scoreboard.Players;
		Sorted.Sort();
		const int32 Total = Sorted.Players.Num();
		auto LayoutPage = [&](int32 First, int32 Count)
		{
			FWoTScoreboardSnapshot Page = Snapshot.Scoreboard;
			Page.Players.Reset();
			for (int32 Index = First; Index < FMath::Min(First + Count, Total); ++Index)
			{
				Page.Players.Add(Sorted.Players[Index]);
			}
			FWoTScoreboardLayoutOptions LayoutOptions;
			LayoutOptions.bKeepHeaders = true;
			return Board.Layout(Page, Width, Height, LineHeight, Snapshot.bStandalone,
				bIconsAvailable, LayoutOptions);
		};
		int32 RowsPerPage = FMath::Max(Total, 1);
		while (RowsPerPage > 1 && FWoTScoreboardBoardChoice::Bottom(
			LayoutPage(0, RowsPerPage), LineHeight,
			static_cast<float>(FWoTArenaScoreboard::IconSize))
			> static_cast<float>(Height) - LineHeight)
		{
			--RowsPerPage;
		}
		Result.PageCount = FMath::Max(1, FMath::DivideAndRoundUp(Total, RowsPerPage));
		Result.Page = FMath::Clamp(Options.Page, 0, Result.PageCount - 1);
		AddSection(EWoTHUDSection::Scoreboard, EWoTHUDDrawPath::Unscaled,
			LayoutPage(Result.Page * RowsPerPage, RowsPerPage));
		if (Result.PageCount > 1)
		{
			TArray<FWoTHUDDrawCmd> Footer;
			FWoTHUDDrawCmd& Line = Footer.AddDefaulted_GetRef();
			Line.Text = FString::Printf(TEXT("%d / %d"), Result.Page + 1, Result.PageCount);
			Line.FontId = FName(TEXT("F_WOTReg14"));
			Line.X = static_cast<float>(Width) * 0.5f;
			Line.Y = static_cast<float>(Height) - LineHeight;
			Line.TextAlignment = EWoTTextAlignment::Center;
			AddSection(EWoTHUDSection::Scoreboard, EWoTHUDDrawPath::Unscaled, MoveTemp(Footer));
		}
		return Result;
	}
	if (Snapshot.bTeamOverview)
	{
		const FWoTCitadelTeamOverview Overview;
		FWoTScoreboardLayoutOptions LayoutOptions;
		LayoutOptions.bKeepHeaders = true;
		AddSection(EWoTHUDSection::TeamOverview, EWoTHUDDrawPath::Unscaled,
			Overview.Layout(Snapshot.TeamOverview, Width, Height, LineHeight, LayoutOptions));
		return Result;
	}
	// Every message in full, wrapped to the panel.
	FWoTHUDDrawCmd Probe;
	Probe.FontId = FName(TEXT("F_WOTReg14"));
	const UFont* Font = WoTHUDLayout_Resolver().ResolveCommandFont(Probe, FontWidth);
	TArray<FWoTHUDDrawCmd> Lines;
	float LineY = WoTHUDLayout_Padding;
	for (const FWoTHUDMessage& Message : Snapshot.Messages)
	{
		const TArray<FString> Wrapped = AWoTHUD::WrapMessageLines(Message.Text,
			static_cast<float>(Width) - 2.0f * WoTHUDLayout_Padding,
			[Font](const FString& Text)
			{
				return Font ? static_cast<float>(Font->GetStringSize(*Text)) : 0.0f;
			});
		for (const FString& Text : Wrapped)
		{
			FWoTHUDDrawCmd& Line = Lines.AddDefaulted_GetRef();
			Line.Text = Text;
			Line.FontId = Probe.FontId;
			Line.X = WoTHUDLayout_Padding;
			Line.Y = LineY;
			Line.Style = EWoTHUDStyle::Translucent;
			Line.Tint = AWoTHUD::FadedHUDColor(Message.Opacity);
			LineY += LineHeight;
		}
	}
	if (!Lines.IsEmpty())
	{
		AddSection(EWoTHUDSection::Messages, EWoTHUDDrawPath::TextItems, MoveTemp(Lines));
	}
	return Result;
}
}

FWoTHUDLayoutResult FWoTHUDLayout::Arrange(const FWoTHUDSnapshot& Snapshot,
	const FWoTHUDLayoutOptions& Options)
{
	switch (Options.Layout)
	{
	case EWoTHUDLayout::Compact:
		return WoTHUDLayout_Compact(Snapshot, Options);
	case EWoTHUDLayout::Reading:
		return WoTHUDLayout_Reading(Snapshot, Options);
	case EWoTHUDLayout::Flat:
	default:
	{
		FWoTHUDLayoutResult Result;
		Result.Sections = Snapshot.Sections;
		Result.Extent = Snapshot.CanvasSize * Snapshot.Upscale;
		return Result;
	}
	}
}

void FWoTHUDLayout::Draw(UCanvas& Canvas, TConstArrayView<FWoTHUDSection> Sections)
{
	WoTHUDLayout_Resolver().DrawSections(Canvas, Sections);
}
