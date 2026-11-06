#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Presentation-only animation (animated light-map recomposition, the
 * light-wave MPC driver, fractal texture simulation, volumetric fog spheres)
 * has nothing to draw on a dedicated server. Every such owner asks this one
 * predicate so the policy, and its test override, live in one place.
 */
WOT_API bool WoTPresentationAnimationEnabled(const UWorld* World);

#if WITH_AUTOMATION_TESTS
/** Force the dedicated-server answer (true) or the client answer (false);
    an unset optional restores the real net-mode check. */
WOT_API void WoTSetPresentationDedicatedServerOverrideForTesting(
	TOptional<bool> bDedicatedServer);
#endif
