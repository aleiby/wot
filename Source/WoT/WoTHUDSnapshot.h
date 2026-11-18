#pragma once

#include "CoreMinimal.h"
#include "WoTCitadelTeamOverview.h"
#include "WoTHUDDrawCmd.h"
#include "WoTScoreboard.h"

class UCanvas;

// The player's HUD as data (H5). The HUD advances its presentation state once
// per local-player frame, then describes everything it shows as this
// read-only snapshot: the draw commands of each part, placed on its own
// logical canvas, plus the values a destination may present differently. The
// flat HUD draws the snapshot's sections on its Canvas; a destination arranges
// the same snapshot (FWoTHUDLayout) and draws it on a canvas of its own, so
// switching destinations changes only the layout. Plain types only.

/** One part of the HUD, in the order the flat HUD draws them. */
enum class EWoTHUDSection : uint8
{
	/** The health icon and number, and the seeker frame. */
	Health,
	/** Active effects with their timers. */
	StatusIcons,
	/** The crosshair, or the citadel editor's cursor. */
	Reticle,
	/** The selected group's slots while a new selection is shown. */
	CurrentHand,
	/** A newly added artifact sliding into its group. */
	AddedItems,
	/** The groups: each artifact's icon, charges and key, and the slot notches. */
	Hands,
	/** The connection and travel progress lines. */
	Progress,
	/** The side choice on a Battle map. */
	TeamPrompt,
	/** Hand, left, right, centre, subtitle and positioned messages. */
	Messages,
	/** The name of the player in the aim. */
	PlayerID,
	/** The flash texture (the slow-frame warning). */
	Flash,
	/** The warning icon. */
	Warning,
	/** The arena or citadel scoreboard. */
	Scoreboard,
	/** The citadel team overview. */
	TeamOverview
};

/** How a destination submits a section's commands. */
enum class EWoTHUDDrawPath : uint8
{
	/** Icons at X,Y (Scale times their size) and text runs aligned by
	 * TextAlignment/bCenterX, through the canvas's 640x480 factors (a canvas
	 * smaller than 640x480 shrinks them to fit). */
	Scaled,
	/** The same with unit factors. */
	Unscaled,
	/** Text runs as canvas text items at their final positions: Style picks
	 * the blend and bCenterX centres on whole pixels. */
	TextItems
};

struct FWoTHUDSection
{
	EWoTHUDSection Section = EWoTHUDSection::Health;
	EWoTHUDDrawPath Path = EWoTHUDDrawPath::Scaled;
	/** Canvas pixels per command unit: the section draws under this integer
	 * scale (the scoreboards' doubled board, a destination's own scale). */
	int32 BoardScale = 1;
	TArray<FWoTHUDDrawCmd> Commands;
};

enum class EWoTHUDMessageKind : uint8
{
	Hand,
	Left,
	Right,
	Center,
	Subtitle,
	Generic
};

/** A message on screen, as the flat HUD draws it. */
struct FWoTHUDMessage
{
	EWoTHUDMessageKind Kind = EWoTHUDMessageKind::Hand;
	FString Text;
	/** 0..1: the message's intensity and fade. */
	float Opacity = 1.0f;
};

/** An active effect's icon and timer, as the flat HUD shows them. */
struct FWoTHUDStatus
{
	FName Icon;
	bool bGood = false;
	/** The timer's step, 0 (just begun) to 18 (nearly over): which of the flat
	 * HUD's 19 timer frames (T32_01 to T32_19) it draws. Exact durations are
	 * not exposed, so a destination shows exactly what the flat HUD shows. */
	int32 TimerStep = 0;
	static constexpr int32 TimerSteps = 19;
};

/** What the player's HUD shows after its update this frame. Nothing hidden or
 * debug-only: a part the HUD does not draw now is absent. */
struct FWoTHUDSnapshot
{
	/** Counts the HUD's updates, one per local-player frame while the HUD
	 * draws; zero before the first. */
	uint64 Serial = 0;
	/** World time of the update. */
	double TimeSeconds = 0.0;
	/** The HUD shows: not hidden, travelling, or behind the in-game menu or
	 * help. While false nothing is drawn and message time does not run. */
	bool bVisible = false;
	/** The flat HUD's logical canvas the sections are placed on, its integer
	 * upscale on the real canvas, and the text scale its text draws at. */
	FIntPoint CanvasSize = FIntPoint::ZeroValue;
	int32 Upscale = 1;
	int32 TextScale = 1;

	/** The health number while it shows. */
	bool bHealth = false;
	int32 Health = 0;
	/** The selected artifact (its shipped class name) and its charges, while
	 * the groups show; Charges is -1 for an artifact without charges. */
	FName Artifact;
	int32 Charges = -1;
	TArray<FWoTHUDStatus> Status;
	TArray<FWoTHUDMessage> Messages;
	TArray<FWoTProgressEntry> Progress;
	FString TeamPrompt;
	FString PlayerID;
	float PlayerIDOpacity = 0.0f;
	FName FlashTexture;
	float FlashOpacity = 0.0f;
	bool bWarning = false;

	/** The scoreboard while it shows: every player (unsorted; the boards sort),
	 * which board, and whether the game is standalone (no ping column). */
	bool bScoreboard = false;
	bool bCitadelScoreboard = false;
	bool bStandalone = false;
	FWoTScoreboardSnapshot Scoreboard;
	/** The citadel team overview while it shows. */
	bool bTeamOverview = false;
	FWoTCitadelTeamOverviewSnapshot TeamOverview;

	/** Everything the flat HUD draws, part by part in its draw order, placed
	 * on CanvasSize. */
	TArray<FWoTHUDSection> Sections;

	const FWoTHUDSection* FindSection(EWoTHUDSection Section) const
	{
		return Sections.FindByPredicate([Section](const FWoTHUDSection& Entry)
		{
			return Entry.Section == Section;
		});
	}
};

enum class EWoTHUDLayout : uint8
{
	/** The flat HUD: the sections where the flat HUD draws them. */
	Flat,
	/** A compact panel: health, groups, status and warnings in a strip,
	 * then the messages, progress, team prompt and player ID as lines. No
	 * reticle and no boards. */
	Compact,
	/** A reading panel: the scoreboard or team overview, a page at a time,
	 * else every message in full. */
	Reading
};

struct FWoTHUDLayoutOptions
{
	EWoTHUDLayout Layout = EWoTHUDLayout::Flat;
	/** The destination canvas in pixels (Compact and Reading). */
	FIntPoint Size = FIntPoint(640, 480);
	/** Reading: the scoreboard page to show, clamped to the pages there are. */
	int32 Page = 0;
};

struct FWoTHUDLayoutResult
{
	TArray<FWoTHUDSection> Sections;
	/** The arranged content's extent in canvas pixels. */
	FIntPoint Extent = FIntPoint::ZeroValue;
	/** Reading: the page shown and how many there are. */
	int32 Page = 0;
	int32 PageCount = 1;
};

struct WOT_API FWoTHUDLayout
{
	/** Arranges a snapshot for a destination. Flat returns its sections as
	 * the flat HUD draws them; a mode switch changes only this step. */
	static FWoTHUDLayoutResult Arrange(const FWoTHUDSnapshot& Snapshot,
		const FWoTHUDLayoutOptions& Options);
	/** Draws arranged sections on a canvas the caller owns (a destination's
	 * own render target), resolving fonts and textures as the HUD does. The
	 * flat HUD draws its own Canvas the same way. */
	static void Draw(UCanvas& Canvas, TConstArrayView<FWoTHUDSection> Sections);
};
