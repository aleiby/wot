// Development-only construction of production class-default cache objects.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/Contract.h"

#if !WOT_GUARDED_BOOTSTRAP

struct FWoTDevelopmentClassDefaultObject
{
	FString LogicalId;
	TArray<uint8> ObjectBytes;
	bool bLoadedFromCache = false;
};

/** Shared read-only source cache for development retail decoders. */
class WOTRETAILREADER_API FWoTDevelopmentPackageCache final
{
public:
	explicit FWoTDevelopmentPackageCache(FStringView ReferenceOriginalRoot);
	TWoTBootstrapResult<TSharedPtr<const TArray<uint8>>> Load(
		FStringView SourcePath);

private:
	FString Root;
	TMap<FString, TSharedPtr<const TArray<uint8>>> Packages;
};

/** Build every production class_default fact from a read-only System root. */
WOTRETAILREADER_API
TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>>
WoTBuildClassDefaultObjectsForDevelopment(
	const FWoTRetailContract& Contract,
	FStringView ReferenceSystemRoot,
	FStringView CacheRoot,
	bool bForceRebuild = false);

/** Same build using the package cache shared with lazy texture/mesh lookups. */
WOTRETAILREADER_API
TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>>
WoTBuildClassDefaultObjectsForDevelopment(
	const FWoTRetailContract& Contract,
	FWoTDevelopmentPackageCache& SourceCache,
	FStringView CacheRoot,
	bool bForceRebuild = false);

#if WITH_AUTOMATION_TESTS
/** Stable cache namespace over every package that may supply reflected schema. */
WOTRETAILREADER_API FString
WoTDevelopmentClassDefaultInputSetDigestForTesting(
	const TMap<FString, TArray<uint8>>& Inputs);
#endif

#endif // !WOT_GUARDED_BOOTSTRAP
