#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "WoTRetailBuildContext.h"
#include "Templates/Function.h"

struct WOT_API FWoTRetailFamily
{
	const TCHAR* Name = nullptr;
	TFunction<bool(
		const FWoTRetailBuildContext&,
		FString& OutDigest,
		TFunction<void()>& OutInstall,
		FString& OutError)> Build;
};

/** Every retail-derived registry family available in this build. */
WOT_API TConstArrayView<FWoTRetailFamily> WoTRetailFamilies();

/** Build and pin-check every family before installing any family. */
WOT_API bool WoTActivateRetailFamilies(
	const FWoTRetailBuildContext& Context,
	TConstArrayView<FWoTRetailFamily> Families,
	const TMap<FString, FString>& Pins,
	FString& OutError);

#if WITH_AUTOMATION_TESTS
/** Replace the production activation inputs for one guarded-path spec. */
WOT_API void WoTRetailFamiliesSetActivationInputsForTesting(
	TArray<FWoTRetailFamily> Families,
	TMap<FString, FString> Pins);
/** Clear the process-wide activation result so independent specs can repeat. */
WOT_API void WoTRetailFamiliesResetActivationForTesting();
/** True while a spec has injected its own activation families. */
WOT_API bool WoTRetailFamiliesOverriddenForTesting();
/** Digests computed by the most recent activation attempt. */
WOT_API const TMap<FString, FString>&
	WoTRetailFamilyObservedDigestsForTesting();
#endif
