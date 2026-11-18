// Bounded cache-v3 import of retail textures and sounds.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

#if !WOT_GUARDED_BOOTSTRAP
/** Parsed development package used for lazy, export-scoped texture facts. */
class WOTRETAILREADER_API FWoTDevelopmentTexturePackage final
{
public:
	static TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentTexturePackage>> Parse(
		TArray<uint8> SourceBytes, FStringView SourcePath);
	~FWoTDevelopmentTexturePackage();

	bool FindTexture(FStringView SourceExport, uint32& OutUSize,
		uint32& OutVSize, uint8& OutFormat, FString& OutAnimNext,
		float& OutMinFrameRate, float& OutMaxFrameRate, uint8& OutPrimeCount) const;

private:
	struct FImpl;
	explicit FWoTDevelopmentTexturePackage(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
};
#endif

WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportTextureSound(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

#if WITH_AUTOMATION_TESTS
/** Raw-package seam for an independent developer oracle. It executes the same
 * production texture parser/encoder without manufacturing expected pixels. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportTexturePackageBytesForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress);
#endif
