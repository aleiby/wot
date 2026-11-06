// Deterministic cache coordination and completed build result.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Progress.h"

class FWoTRetailPackage;

/** Authenticated texture authority bound to one ordered font-v2 atlas page. */
struct FWoTFontAtlasPageRecord
{
	uint32 Ordinal = 0;
	FString TextureLogicalId;
	FString SourceIdentity;
	uint32 Width = 0;
	uint32 Height = 0;
	FString Format;
	FWoTSha256 SourceSha256;
	uint32 CharacterCount = 0;
};

/** Complete font-v2 glyph metric, independent of UE's legacy UFont layout. */
struct FWoTFontGlyphRecord
{
	uint32 Codepoint = 0;
	uint32 Page = 0;
	FIntRect AtlasRect;
	int32 Advance = 0;
	int32 BearingX = 0;
	int32 BearingY = 0;
};

struct FWoTFontCharacterMapRecord
{
	uint32 Codepoint = 0;
	uint32 Glyph = 0;
};

struct FWoTFontKerningRecord
{
	uint32 LeftCodepoint = 0;
	uint32 RightCodepoint = 0;
	int32 Adjustment = 0;
};

/** Canonical semantic representation carried by every font-v2 object. */
struct FWoTFontV2Record
{
	FString SourceExport;
	uint32 CharactersPerPage = 0;
	uint32 Baseline = 0;
	uint32 LineHeight = 0;
	TArray<FWoTFontAtlasPageRecord> Pages;
	TArray<FWoTFontCharacterMapRecord> CharacterMap;
	TArray<FWoTFontGlyphRecord> Glyphs;
	TArray<FWoTFontKerningRecord> Kerning;
};

struct FWoTCacheBuildResult
{
	FWoTCacheManifestV3 Manifest;
	TArray<uint8> CanonicalManifest;
	FWoTSha256 ManifestSha256;
	FWoTCacheProgressSnapshot Progress;

};

/** Complete structural identity of a parsed contract, with unambiguous framing. */
WOTRETAILREADER_API FWoTSha256 ComputeRetailContractIdentity(
	const FWoTRetailContract& Contract);

/** Parse the production-descriptor pin compiled into CacheBuilder.cpp. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTSha256>
GetCacheExpectedProductionDescriptorSha256();

/** Exact reviewed contract/descriptor pair that requires entry-sidecar authority. */
WOTRETAILREADER_API bool IsCurrentProductionCacheBuildIdentity(
	const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256);

namespace WoT::Retail::Detail
{
TWoTBootstrapResult<FWoTCacheBuildResult> BuildRetailCache(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);
}

#if WITH_AUTOMATION_TESTS
/** A normalized importer boundary used only to exercise the coordinator. */
struct FWoTCacheImporterForTesting
{
	FString Stage;
	TArray<FString> Importers;
	TFunction<TWoTBootstrapResult<TArray<FWoTCacheBlob>>(
		IWoTObjectWriter&, IWoTProgressSink&)> Run;
};

WOTRETAILREADER_API TWoTBootstrapResult<FWoTCacheBuildResult>
BuildRetailCacheForTesting(const FWoTSha256& InstallerSha256,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress,
	TConstArrayView<uint8> CanonicalProductionDescriptor,
	const TMap<FString, uint64>& BoundedSourceSizes,
	TArray<FWoTCacheImporterForTesting>& Importers);

WOTRETAILREADER_API FWoTBootstrapStatus ValidateCacheProductionDescriptorForTesting(
	TConstArrayView<uint8> CanonicalProductionDescriptor);

/** The standalone test runner publishes its exact command-line descriptor here. */
WOTRETAILREADER_API void SetCacheProductionDescriptorPathForTesting(FString Path);

/** Load the runner-published descriptor, or the checked-in default when unset. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<uint8>>
LoadCacheProductionDescriptorForTesting();

/** Serialize the canonical cross-language artifact without adding a second JSON builder. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<uint8>>
WriteCacheSyntheticParityArtifactForTesting(
	const FWoTCacheBuildResult& Build,
	const TMap<FString, FWoTSha256>& WorldEventRoots,
	const FWoTSha256& WorldSetRoot,
	TConstArrayView<FString> RejectedFieldPaths);
#endif
