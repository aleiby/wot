#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "../RetailData/WoTRetailFamilies.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif

/**
 * Specs that Reset or Install fixture sets into a retail family leave the
 * process-wide registries changed for every later spec. On scope exit this
 * rebuilds the real families from the reference packages (warm cache).
 */
struct FWoTRetailFamiliesRestoreScope final
{
	~FWoTRetailFamiliesRestoreScope()
	{
#if !WOT_GUARDED_BOOTSTRAP
		WoTRetailFamiliesResetActivationForTesting();
		WoTDevRetailFamiliesResetForTesting();
		WoTDevRetailFamiliesEnsureActivated();
#endif
	}
};

#endif // WITH_AUTOMATION_TESTS
