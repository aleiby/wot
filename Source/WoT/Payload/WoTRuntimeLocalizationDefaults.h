// Cache-v3 localization/default data loaded through the one verified reader.
#pragma once

#include "CoreMinimal.h"
#include "WoTNeutralResources.h"
#include "WoTRuntimeRegistry.h"

class WOT_API FWoTRuntimeLocalizationDefaults final
{
public:
	FWoTBootstrapStatus LoadLocalizationFirstUse(
		const FWoTNeutralResourceId& Resource);
	FWoTBootstrapStatus LoadClassDefaultsFirstUse(
		const FWoTNeutralResourceId& Resource);
#if WITH_AUTOMATION_TESTS
	/** Exercise the exact first-use validator with an already verified frame. */
	FWoTBootstrapStatus LoadClassDefaultsFirstUseObjectForTesting(
		FWoTRuntimeVerifiedObject&& Object);
#endif

	TConstArrayView<uint8> LocalizationBytes() const
	{
		return Localization.IsSet()
			? Localization->Payload() : TConstArrayView<uint8>();
	}
	TConstArrayView<uint8> ClassDefaultBytes() const
	{
		return ClassDefaults.IsSet()
			? ClassDefaults->Payload() : TConstArrayView<uint8>();
	}

private:
	FWoTBootstrapStatus LoadVerifiedClassDefaults(
		FWoTRuntimeVerifiedObject&& Object);
	TOptional<FWoTRuntimeVerifiedObject> Localization;
	TOptional<FWoTRuntimeVerifiedObject> ClassDefaults;
};
