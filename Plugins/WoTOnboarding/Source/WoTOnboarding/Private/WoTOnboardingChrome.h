#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Widgets/Text/STextBlock.h"
#include "WoTOnboardingSeams.h"

struct FGeometry;
class FWoTOnboardingFaces;

// How every onboarding widget draws: the original game's window look.
//
// Window units. The widgets lay out in the original's 1x pixels inside a
// scale box that draws WindowScale screen pixels per unit. The cards draw at
// the largest whole number (1 or 2) at which a 960x540 window fits the
// screen, or, on a screen smaller than that, the fraction that fits. The
// guide fills GuideScreenFill of the largest 960x540 window that fits, at any
// fraction, so it uses the screen's room.
//
// Whole pixels. The published faces are bitmap strikes at every whole step
// from 1 to MaxPixelScale, and a strike sized for its step covers exactly one
// screen pixel per strike pixel, whatever the window scale. The cards use the
// strike for PixelScaleFor (1 or 2). The guide draws its titles and labels at
// the step nearest its scale (BaseStepFor), so they keep about their original
// size, and its reading text (the detail pane's lines and facts) at about
// ReadingTextScale times that: the largest step from ReadingStepsFor at which
// every entry fits the pane without scrolling, never less than one step above
// the labels. On a screen too small for a 1x guide there are no whole pixels
// to keep, so the guide draws the 1x strike at its original size and its
// reading text ReadingTextScale larger. Without the published faces, engine
// faces of about the same size stand in.
//
// Steam Deck legibility: Valve's compatibility checklist rejects text under
// 9 px tall at 1280x800 and recommends 12 px. The Deck draws the cards at 1x
// and the guide at about 1.27 with its labels at 1x and its reading text at
// 2x, and the smallest original face they use is 14 px tall. Font() still
// lifts every card face, the engine stand-ins included, to
// DeckMinimumFontSize(). Wherever the guide draws at 1x or more, GuideFont()
// keeps each of its lines at least DeckMinimumTextPixels tall on the screen
// it draws on: a published face too small moves up a whole step, and an
// engine stand-in grows to fit.
namespace WoTOnboardingChrome
{
	/** The height, in physical pixels at 1280x800, that every onboarding
	    line meets: Valve's recommendation (its hard floor is 9). */
	constexpr float DeckMinimumTextPixels = 12.0f;

	/** The largest whole step the guide draws a face at. */
	constexpr int32 MaxPixelScale = FWoTOnboardingMenuChrome::MaxPixelScale;

	/** About how much larger than its original size the guide's reading
	    text draws. */
	constexpr float ReadingTextScale = 1.5f;

	/** Which size a guide line draws at: Base for the title, labels and gem
	    labels; Reading for the detail pane's lines and facts. */
	enum class ETextRole : uint8 { Base, Reading };

	/** The whole step for the guide's base text at WindowScale: the nearest
	    whole number, 1 to MaxPixelScale. */
	int32 BaseStepFor(float WindowScale);

	/** The whole steps the guide's reading text may use at WindowScale: from
	    one step above the base to the first step at or above ReadingTextScale
	    times the window scale, both capped at MaxPixelScale. Both are 1 below
	    a 1x window scale. */
	void ReadingStepsFor(float WindowScale, int32& OutLowest, int32& OutHighest);

	/** The guide window in window units, also the reference size for card scale. */
	constexpr float WindowWidth = 960.0f;
	constexpr float WindowHeight = 540.0f;

	// The guide fills most of the screen, leaving a thin margin of the game.
	constexpr float GuideScreenFill = 0.95f;

	enum class EWindowScaleRule : uint8
	{
		WholeStep,
		GuideFill
	};

	/** The original faces the widgets draw: F_WOTReg30 titles, F_WOTReg14
	    body text and labels, F_WOTIta14 quotes, F_Element gem labels, and
	    F_Charge for the charge count (its ':' glyph is the item-info
	    window's infinity mark). */
	enum class EFace : uint8
	{
		Title,
		Body,
		Italic,
		Element,
		Charge,
		Count
	};

	/** Face for a guide drawn at WindowScale: the published strike at Step,
	    sized so each strike pixel covers one screen pixel and moved up a whole
	    step while a line would draw under DeckMinimumTextPixels; else an
	    engine face of about the same size, lifted to that floor. Below a 1x
	    window scale, the 1x strike at its original size, ReadingTextScale
	    larger for Reading. */
	FSlateFontInfo GuideFont(const FWoTOnboardingMenuChrome& Chrome, EFace Face,
		int32 Step, float WindowScale, ETextRole Role = ETextRole::Base);

	/** The published faces, else an empty chrome (no strikes, no binder).
	    Resolved per call: the faces are revocable payload data. */
	FWoTOnboardingMenuChrome Resolve();

	/** True when the chrome carries the published faces. */
	bool IsRetail(const FWoTOnboardingMenuChrome& Chrome);

	/** The strike scale (1 or 2) for a screen PhysicalSize pixels large: the
	    largest whole number at which a WindowWidth x WindowHeight window
	    fits, and 1 on a screen too small for even that. */
	int32 PixelScaleFor(const FVector2D& PhysicalSize);

	/** Screen pixels per window unit: whole steps for cards, or the guide
	    fill fraction of the scale that fits the screen. */
	float WindowScaleFor(const FVector2D& PhysicalSize,
		EWindowScaleRule Rule = EWindowScaleRule::WholeStep);

	/** WindowScaleFor a Width x Height screen. */
	float WindowScaleAt(int32 Width, int32 Height);

	/** Physical pixels Font's em covers in a window drawn at WindowScale.
	    Slate renders a font size as points at 96 DPI. */
	float PhysicalPixels(const FSlateFontInfo& Font, float WindowScale);

	/** The smallest font size that draws DeckMinimumTextPixels tall in a
	    window on the Deck's 1280x800 screen. */
	float DeckMinimumFontSize();

	/** Face at PixelScale: the published strike when the chrome answers one,
	    else an engine face of about the original's size; either way at
	    least DeckMinimumFontSize(). */
	FSlateFontInfo Font(const FWoTOnboardingMenuChrome& Chrome, EFace Face,
		int32 PixelScale);

	/** The face's gold at full draw level (255). */
	FLinearColor TextColor(EFace Face);

	/** The face's gold at half draw level (127), for dim labels. */
	FLinearColor LabelColor(EFace Face);

	/** A text block in one of Faces' faces, from its Role's set; it follows
	    them when they refresh for a new strike scale. A FixedWrapWidth above
	    zero wraps at that many window units rather than at the allotted
	    width, so an offscreen measurement wraps as the drawn text does. */
	TSharedRef<STextBlock> MakeText(const TSharedRef<FWoTOnboardingFaces>& Faces,
		const FString& Text, EFace Face, const FLinearColor& Color,
		ETextJustify::Type Justification = ETextJustify::Left, bool bWrap = false,
		ETextRole Role = ETextRole::Base, float FixedWrapWidth = 0.0f);
}

/** The scale an onboarding window draws at, tracked from the geometry its
    widget is given. Initialize it from the viewport when the window opens
    (Slate lays a widget out before its first tick), then Update it every
    tick. */
struct FWoTOnboardingWindowScale
{
	explicit FWoTOnboardingWindowScale(WoTOnboardingChrome::EWindowScaleRule InRule)
		: Rule(InRule) {}

	/** From the viewport's size in pixels and its UI DPI scale. A zero size
	    keeps the 1x defaults. */
	void Initialize(const FVector2D& PhysicalSize, float DpiScale);

	/** From the widget's own geometry. True when the strike changes or the
	    window scale has moved more than 1e-4 (relative) from the scale the
	    last true return reported, so the faces need resolving again. */
	bool Update(const FGeometry& AllottedGeometry);

	int32 GetPixelScale() const { return PixelScale; }
	float GetWindowScale() const { return WindowScale; }

	/** The scale box's scale: window units to the widget's own units. */
	float SlateScale() const { return WindowScale / FMath::Max(DpiScale, 0.01f); }

private:
	WoTOnboardingChrome::EWindowScaleRule Rule;
	float DpiScale = 1.0f;
	int32 PixelScale = 1;
	float WindowScale = 1.0f;
	float ReportedWindowScale = 1.0f;
};

/** What one onboarding window draws its text with: the chrome resolved when
    it opened and its faces at the current strike scale, with a second set
    for the guide's reading text. */
class FWoTOnboardingFaces
{
public:
	explicit FWoTOnboardingFaces(const FWoTOnboardingMenuChrome& InChrome);

	/** Resolves every face at PixelScale, for a card. */
	void Refresh(int32 PixelScale);

	/** Resolves every face for a guide drawn at InWindowScale (GuideFont):
	    the base set at InBaseStep and the reading set at InReadingStep. */
	void RefreshGuide(int32 InBaseStep, int32 InReadingStep, float InWindowScale);
	int32 GetBaseStep() const { return BaseStep; }
	int32 GetReadingStep() const { return ReadingStep; }
	float GetWindowScale() const { return WindowScale; }

	const FSlateFontInfo& Get(WoTOnboardingChrome::EFace Face,
		WoTOnboardingChrome::ETextRole Role = WoTOnboardingChrome::ETextRole::Base) const
	{
		return Role == WoTOnboardingChrome::ETextRole::Reading
			? ReadingFonts[static_cast<int32>(Face)] : Fonts[static_cast<int32>(Face)];
	}

	/** True when Face draws the published strike rather than an engine
	    stand-in, for text whose characters only the original face maps
	    (F_Charge's infinity mark). */
	bool IsPublished(WoTOnboardingChrome::EFace Face,
		WoTOnboardingChrome::ETextRole Role = WoTOnboardingChrome::ETextRole::Base) const
	{
		return Role == WoTOnboardingChrome::ETextRole::Reading
			? bReadingPublished[static_cast<int32>(Face)] : bPublished[static_cast<int32>(Face)];
	}

	const FWoTOnboardingMenuChrome& GetChrome() const { return Chrome; }

private:
	int32 BaseStep = 1;
	int32 ReadingStep = 1;
	float WindowScale = 1.0f;
	FWoTOnboardingMenuChrome Chrome;
	FSlateFontInfo ReadingFonts[static_cast<int32>(WoTOnboardingChrome::EFace::Count)];
	FSlateFontInfo Fonts[static_cast<int32>(WoTOnboardingChrome::EFace::Count)];
	bool bPublished[static_cast<int32>(WoTOnboardingChrome::EFace::Count)] = {};
	bool bReadingPublished[static_cast<int32>(WoTOnboardingChrome::EFace::Count)] = {};
};
