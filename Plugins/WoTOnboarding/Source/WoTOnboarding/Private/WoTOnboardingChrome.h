#pragma once

#include "CoreMinimal.h"
#include "WoTOnboardingSeams.h"

// The chrome every onboarding widget draws with: the retail menu chrome the
// game module publishes through the seam, or, while that is unavailable,
// engine default faces and flat panels (never retail-looking, always usable).
//
// Steam Deck legibility: Valve's compatibility checklist rejects text under
// 9 px tall at 1280x800 and recommends 12 px. Resolve() lifts every face to
// at least DeckMinimumFontSize(), so no onboarding line draws under 12 px on
// the Deck's screen whatever the published chrome carries. The widgets draw
// with the title, body and italic faces; the menu's small face is too small
// for that screen, so they leave it alone.
namespace WoTOnboardingChrome
{
	/** The height, in physical pixels at 1280x800, that every onboarding
	    line meets: Valve's recommendation (its hard floor is 9). */
	constexpr float DeckMinimumTextPixels = 12.0f;

	/** The published retail chrome, else the fallback, with every face at
	    least DeckMinimumFontSize(). Resolved per call: the retail faces are
	    revocable payload data. */
	FWoTOnboardingMenuChrome Resolve();

	/** True for the retail chrome (its brushes are already tinted; the
	    fallback's are white and need a tint). */
	bool IsRetail(const FWoTOnboardingMenuChrome& Chrome);

	/** The Slate layout scale at Width x Height on the project's UI DPI
	    curve at the default UI scale (about 0.74 at 1280x800). */
	float LayoutScaleAt(int32 Width, int32 Height);

	/** Physical pixels Font's em covers under LayoutScale. Slate renders a
	    font size as points at 96 DPI. */
	float PhysicalPixels(const FSlateFontInfo& Font, float LayoutScale);

	/** The smallest font size that draws DeckMinimumTextPixels tall at
	    1280x800. */
	float DeckMinimumFontSize();
}
