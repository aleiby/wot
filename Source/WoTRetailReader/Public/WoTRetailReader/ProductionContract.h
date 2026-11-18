// Verified access to the one compiled production retail-cache contract.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/Contract.h"

/** Parse and authenticate the compiled payload under its compiled verify key. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTRetailContract>
WoTLoadProductionRetailContract();

#if WITH_AUTOMATION_TESTS
/** Process-local parsed-contract seam for spec-scale installer/build fixtures. */
WOTRETAILREADER_API void WoTSetProductionRetailContractForTesting(
	const FWoTRetailContract* Contract);
#endif
