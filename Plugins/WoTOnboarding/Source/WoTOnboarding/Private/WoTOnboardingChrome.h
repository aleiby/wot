#pragma once

#include "CoreMinimal.h"
#include "WoTOnboardingSeams.h"

// The chrome every onboarding widget draws with: the retail menu chrome the
// game module publishes through the seam, or, while that is unavailable,
// engine default faces and flat panels — never retail-looking, always usable.
namespace WoTOnboardingChrome
{
	/** The published retail chrome, else the fallback. Resolved per call:
	    the retail faces are revocable payload data. */
	FWoTOnboardingMenuChrome Resolve();

	/** True for the retail chrome (its brushes are already tinted; the
	    fallback's are white and need a tint). */
	bool IsRetail(const FWoTOnboardingMenuChrome& Chrome);
}
