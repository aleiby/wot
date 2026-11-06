// Atomic cache-v3 import of the required multiplayer BSP/world set.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/Package.h"

struct FWoTWorldSetResult
{
	TArray<FWoTCacheBlob> Objects;
	TArray<FWoTWorldDescriptor> Worlds;
	FWoTSha256 SetRoot;
};

/** Bind every descriptor slot and world/BSP output association to an
 * independently parsed retail contract. */
WOTRETAILREADER_API FWoTBootstrapStatus
ValidateWorldDescriptorContractAssociations(
	TConstArrayView<FWoTWorldDescriptor> Descriptors,
	const FWoTRetailContract& Contract);

WOTRETAILREADER_API TWoTBootstrapResult<FWoTWorldSetResult>
ImportRequiredWorldSet(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

/** Import the single entry map without making it a multiplayer descriptor slot. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTWorldSetResult>
ImportFrontEndWorld(const FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

#if WITH_AUTOMATION_TESTS
/** A source-only test seam; production always streams the held installer. */
struct FWoTWorldPackageBytesForTesting
{
	FString SourcePath;
	TArray<uint8> Bytes;
};

WOTRETAILREADER_API TWoTBootstrapResult<FWoTWorldSetResult>
ImportRequiredWorldSetPackageBytesForTesting(
	TConstArrayView<FWoTWorldPackageBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

/** Exercise the dedicated entry importer against genuine package bytes. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTWorldSetResult>
ImportFrontEndWorldPackageBytesForTesting(
	TConstArrayView<FWoTWorldPackageBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

/** Exercises the production attachment-cycle validator without fabricating UE packages. */
WOTRETAILREADER_API FWoTBootstrapStatus ValidateWorldAttachmentsForTesting(
	TConstArrayView<int32> ParentByActor);

/** Feeds a minimal name table and property byte stream through the production
 * BSP world's strict property decoder. */
WOTRETAILREADER_API bool ReadWorldPropertiesFixtureForTesting(
	TConstArrayView<FString> Names, TConstArrayView<uint8> Bytes);

/**
 * Translate the shared synthetic JSON matrix into the production semantic
 * closure records. An error result deliberately carries no completed set.
 */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTWorldSetResult>
ValidateWorldSemanticFixtureForTesting(TConstArrayView<uint8> CanonicalBytes);
#endif
