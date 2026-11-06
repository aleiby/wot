#pragma once

#include "CoreMinimal.h"
#include "WoTRetailBuildContext.h"

#if !WOT_GUARDED_BOOTSTRAP

class FWoTRuntimeClassDefaults;

/** Decode the retail class defaults and install all pinned families once. */
WOT_API void WoTDevRetailFamiliesEnsureActivated();
/** Development/editor view of the same authenticated class-default closure.
 * Guarded builds instead obtain this from the active payload generation. */
WOT_API const FWoTRuntimeClassDefaults* WoTDevRetailClassDefaults();

#if WITH_AUTOMATION_TESTS
WOT_API void WoTDevRetailFamiliesResetForTesting();
WOT_API void WoTDevRetailFamiliesSetCacheRootForTesting(FString CacheRoot);
WOT_API bool WoTDevRetailFamiliesLastDecodeSucceededForTesting();
WOT_API int32 WoTDevRetailFamiliesLastDecodedObjectCountForTesting();
WOT_API int32 WoTDevRetailFamiliesLastCacheHitCountForTesting();
WOT_API double WoTDevRetailFamiliesLastDecodeSecondsForTesting();
WOT_API bool WoTDevRetailFamiliesFindTextureForTesting(
	FStringView Identity, FWoTRetailTextureFacts& Out);
WOT_API bool WoTDevRetailFamiliesFindMeshSequencesForTesting(
	FStringView Identity, TArray<FWoTVertexMeshSequence>& Out);
#endif

#endif // !WOT_GUARDED_BOOTSTRAP
