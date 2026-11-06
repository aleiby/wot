// Bounded cache-v3 import of package registries, .int files, and class defaults.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportRegistryLocalizationDefaults(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

#if WITH_AUTOMATION_TESTS
struct FWoTRemainingSourceBytesForTesting
{
	FString SourcePath;
	TArray<uint8> Bytes;
};

WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportRegistryLocalizationDefaultsSourcesForTesting(
	TConstArrayView<FWoTRemainingSourceBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);
#endif
