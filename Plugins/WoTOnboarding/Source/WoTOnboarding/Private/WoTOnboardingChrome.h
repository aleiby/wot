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
// scale box that draws WindowScale screen pixels per unit. The guide fills
// GuideScreenFill of the largest 960x540 window that fits the screen, at any
// fraction, so its text reads at the screen's size. The cards draw at the
// largest whole number (1 or 2) at which that window fits, or, on a screen
// smaller than that, the fraction that fits. The faces are the published
// bitmap strikes for PixelScaleFor (1 or 2); without them, engine faces of
// about the same size stand in.
//
// Steam Deck legibility: Valve's compatibility checklist rejects text under
// 9 px tall at 1280x800 and recommends 12 px. The Deck draws the cards at
// 1x and the guide a little larger, and the smallest original face they use
// is 14 px tall. Font() still lifts every face, the engine stand-ins
// included, to DeckMinimumFontSize(), so no onboarding line draws under 12 px
// on the Deck's screen whatever the published faces carry.
namespace WoTOnboardingChrome
{
	/** The height, in physical pixels at 1280x800, that every onboarding
	    line meets: Valve's recommendation (its hard floor is 9). */
	constexpr float DeckMinimumTextPixels = 12.0f;

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

	/** A text block in one of Faces' faces; it follows them when they
	    refresh for a new strike scale. */
	TSharedRef<STextBlock> MakeText(const TSharedRef<FWoTOnboardingFaces>& Faces,
		const FString& Text, EFace Face, const FLinearColor& Color,
		ETextJustify::Type Justification = ETextJustify::Left, bool bWrap = false);
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

	/** From the widget's own geometry. True when the strike scale changed,
	    so the faces need resolving again. */
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
};

/** What one onboarding window draws its text with: the chrome resolved when
    it opened and its faces at the current strike scale. */
class FWoTOnboardingFaces
{
public:
	explicit FWoTOnboardingFaces(const FWoTOnboardingMenuChrome& InChrome);

	/** Resolves every face at PixelScale. */
	void Refresh(int32 PixelScale);

	const FSlateFontInfo& Get(WoTOnboardingChrome::EFace Face) const
	{
		return Fonts[static_cast<int32>(Face)];
	}

	/** True when Face draws the published strike rather than an engine
	    stand-in, for text whose characters only the original face maps
	    (F_Charge's infinity mark). */
	bool IsPublished(WoTOnboardingChrome::EFace Face) const
	{
		return bPublished[static_cast<int32>(Face)];
	}

	const FWoTOnboardingMenuChrome& GetChrome() const { return Chrome; }

private:
	FWoTOnboardingMenuChrome Chrome;
	FSlateFontInfo Fonts[static_cast<int32>(WoTOnboardingChrome::EFace::Count)];
	bool bPublished[static_cast<int32>(WoTOnboardingChrome::EFace::Count)] = {};
};
