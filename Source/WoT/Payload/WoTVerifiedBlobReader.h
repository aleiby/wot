// The single lazy-use boundary for immutable cache payload bytes.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

class FWoTCacheGenerationLease;

/** Manifest-derived identity. LogicalId is an identifier, never an OS path. */
struct FWoTCacheBlobDescriptor
{
	FString LogicalId;
	FString Schema;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

struct FWoTCacheBlobReadLimits
{
	uint64 MaximumBlobBytes = 1024ull * 1024ull * 1024ull;
	uint64 MaximumAllocationBytes = 1024ull * 1024ull * 1024ull;

#if WITH_AUTOMATION_TESTS
	/** Runs after the held no-follow handle is checked and immediately before ReadAll. */
	TFunction<void()> ImmediatelyBeforeSingleReadForTesting;
	/** Runs on a worker concurrently with the sole ReadAll call. */
	TFunction<void()> ConcurrentWithSingleReadForTesting;
	/** Rendezvous after the worker starts and immediately before the sole ReadAll. */
	TFunction<void()> ImmediatelyBeforeReadAfterWorkerStartedForTesting;
	/** Inject an ordinary body-read I/O error without changing the cache bytes. */
	TFunction<TOptional<FWoTBootstrapError>()> BodyReadFailureForTesting;
#endif
};

class WOT_API FWoTImmutableVerifiedBlob final
{
public:
	FWoTImmutableVerifiedBlob() = default;
	FWoTImmutableVerifiedBlob(FWoTImmutableVerifiedBlob&&) = default;
	FWoTImmutableVerifiedBlob& operator=(FWoTImmutableVerifiedBlob&&) = default;
	FWoTImmutableVerifiedBlob(const FWoTImmutableVerifiedBlob&) = delete;
	FWoTImmutableVerifiedBlob& operator=(
		const FWoTImmutableVerifiedBlob&) = delete;

	TConstArrayView<uint8> Bytes() const
	{
		return Buffer.IsValid() ? TConstArrayView<uint8>(*Buffer)
			: TConstArrayView<uint8>();
	}

private:
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Buffer;

	// WOT_API here too: on MSVC the macro expands to __declspec(dllexport/import),
	// so a friend declaration without it and the exported declaration below are
	// the same function with DIFFERENT LINKAGE (C2375). Clang's visibility
	// attribute does not conflict that way, so only the Windows lane sees it.
	friend WOT_API TWoTBootstrapResult<FWoTImmutableVerifiedBlob> ReadVerifiedCacheBlob(
		const FWoTCacheGenerationLease&, const FWoTCacheBlobDescriptor&,
		const FWoTCacheBlobReadLimits&);
	friend class FWoTCacheGenerationLease;
	friend class FWoTRuntimeRegistry;
};

WOT_API TWoTBootstrapResult<FWoTImmutableVerifiedBlob> ReadVerifiedCacheBlob(
	const FWoTCacheGenerationLease& Lease, const FWoTCacheBlobDescriptor& Descriptor,
	const FWoTCacheBlobReadLimits& Limits);

#if WITH_AUTOMATION_TESTS
/** Process-local observer for counting physical blob-body reads in automation. */
WOT_API void WoTSetVerifiedBlobBodyReadObserverForTesting(
	TFunction<void()> Observer);
#endif
