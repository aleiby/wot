// Bounded cache-v3 import of UE1 fonts and procedural texture inputs.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

#if !WOT_GUARDED_BOOTSTRAP
/** Parsed development package used for lazy, export-scoped procedural
 * texture facts. */
class WOTRETAILREADER_API FWoTDevelopmentProceduralPackage final
{
public:
	static TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentProceduralPackage>> Parse(
		TArray<uint8> SourceBytes, FStringView SourcePath);
	~FWoTDevelopmentProceduralPackage();

	bool FindTexture(FStringView SourceExport, uint32& OutUSize,
		uint32& OutVSize, uint8& OutFormat) const;

private:
	struct FImpl;
	explicit FWoTDevelopmentProceduralPackage(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
};
#endif

WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportFontProcedural(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

#if WITH_AUTOMATION_TESTS
struct FWoTFontV2Record;
struct FWoTRemainingExportForTesting
{
	FString FactIdentity;
	FString CanonicalIdentity;
};

struct FWoTRemainingPackageInspectionForTesting
{
	TArray<FWoTRemainingExportForTesting> Fonts;
	TArray<FWoTRemainingExportForTesting> Textures;
	TArray<FWoTRemainingExportForTesting> Procedurals;
	uint32 ClassCount = 0;
};

/** Loads a sibling package a v2 procedural object references across packages
    (foreign palettes/sources); absent, only the exact source package exists. */
using FWoTRemainingSiblingLoaderForTesting =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;

/** Real-retail pins use the production decoder without wrapping a .u in Inno. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportFontProceduralPackageBytesForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress,
	FWoTRemainingSiblingLoaderForTesting SiblingLoader =
		FWoTRemainingSiblingLoaderForTesting());

WOTRETAILREADER_API TWoTBootstrapResult<FWoTRemainingPackageInspectionForTesting>
InspectRemainingPackageForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath);

/** Pure preflight used to prove hostile records fail before glyph work. */
WOTRETAILREADER_API FWoTBootstrapStatus
ValidateFontV2MaterializationBoundsForTesting(
	const FWoTFontV2Record& Record, uint32& OutVisitedGlyphs);

WOTRETAILREADER_API FWoTBootstrapStatus ComposeFontCodepointForTesting(
	uint32 PageIndex, uint32 CharactersPerPage, uint32 LocalIndex,
	uint32& OutCodepoint);
#endif
