// Lease-bound descriptor registry for every non-derived runtime retail resource.
#pragma once

#include "CoreMinimal.h"
#include "WoTNeutralResources.h"
#include "WoTVerifiedBlobReader.h"
#include "WoTBootstrapCore/CanonicalJson.h"

class FWoTCacheGenerationLease;
class UWoTPayloadSubsystem;

class WOT_API FWoTRuntimeVerifiedObject final
{
public:
	FWoTRuntimeVerifiedObject() = default;
	FWoTRuntimeVerifiedObject(FWoTRuntimeVerifiedObject&&) = default;
	FWoTRuntimeVerifiedObject& operator=(FWoTRuntimeVerifiedObject&&) = default;
	FWoTRuntimeVerifiedObject(const FWoTRuntimeVerifiedObject&) = delete;
	FWoTRuntimeVerifiedObject& operator=(const FWoTRuntimeVerifiedObject&) = delete;

	const FWoTCanonicalJsonValue& Header() const { return ParsedHeader; }
	TConstArrayView<uint8> Payload() const
	{
		const TConstArrayView<uint8> All = Blob.Bytes();
		return All.RightChop(PayloadOffset);
	}

private:
	FWoTImmutableVerifiedBlob Blob;
	FWoTCanonicalJsonValue ParsedHeader;
	int32 PayloadOffset = 0;

	friend class FWoTRuntimeRegistry;
};

class WOT_API FWoTRuntimeRegistry final
{
public:
	static FWoTRuntimeRegistry& Get();

	FWoTRuntimeRegistry(const FWoTRuntimeRegistry&) = delete;
	FWoTRuntimeRegistry& operator=(const FWoTRuntimeRegistry&) = delete;

	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Read(
		const FWoTNeutralResourceId& Resource,
		uint64 MaximumDecodedBytes) const;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> ReadObject(
		const FWoTNeutralResourceId& Resource,
		uint64 MaximumDecodedBytes, FStringView ExpectedEncoding,
		FStringView ExpectedKind,
		FStringView RebuildOnlyEncoding = FStringView(),
		FStringView AlternateExpectedEncoding = FStringView(),
		FStringView SecondAlternateExpectedEncoding = FStringView(),
		FStringView ThirdAlternateExpectedEncoding = FStringView(),
		FStringView FourthAlternateExpectedEncoding = FStringView()) const;
#if !WOT_GUARDED_BOOTSTRAP
	/** Validate an in-memory developer object through the runtime frame parser. */
	static TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
		DecodeObjectBytesForDevelopment(TArray<uint8> Bytes,
			FStringView ExpectedEncoding, FStringView ExpectedKind,
			FStringView RebuildOnlyEncoding = FStringView(),
			FStringView AlternateExpectedEncoding = FStringView());
#endif
#if WITH_AUTOMATION_TESTS
	static TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
		DecodeObjectBytesForTesting(TArray<uint8> Bytes,
			FStringView ExpectedEncoding, FStringView ExpectedKind,
			FStringView RebuildOnlyEncoding = FStringView(),
			FStringView AlternateExpectedEncoding = FStringView());
#endif
	/**
	 * Enumerate the exact logical objects authorized by the active lease for a
	 * semantic domain. The result is ordinal-sorted so domains such as
	 * procedural textures cannot accidentally inherit manifest-map iteration
	 * order as simulation order.
	 */
	TArray<FWoTNeutralResourceId> ListResources(
		EWoTNeutralResourceKind Kind) const;
	/** Snapshot the exact manifest descriptor authorized for one semantic id. */
	TOptional<FWoTCacheBlobDescriptor> FindDescriptor(
		const FWoTNeutralResourceId& Resource) const;
	/** True only while this registry reads from the exact shared cache state. */
	bool SharesSnapshotWith(const FWoTCacheGenerationLease& Lease) const;

private:
	FWoTRuntimeRegistry() = default;
#if WITH_AUTOMATION_TESTS || !WOT_GUARDED_BOOTSTRAP
	static TWoTBootstrapResult<FWoTRuntimeVerifiedObject>
		DecodeUntrustedObjectBytes(TArray<uint8> Bytes,
			FStringView ExpectedEncoding, FStringView ExpectedKind,
			FStringView RebuildOnlyEncoding,
			FStringView AlternateExpectedEncoding);
#endif
	FWoTBootstrapStatus BindVerifiedGeneration(
		const FWoTCacheGenerationLease& Lease);
	FWoTBootstrapStatus RegisterDescriptor(
		const FWoTNeutralResourceId& Resource,
		const FWoTCacheBlobDescriptor& Descriptor);
	void Reset();

	mutable FCriticalSection Guard;
	const FWoTCacheGenerationLease* ActiveLease = nullptr;
	TMap<FString, FWoTCacheBlobDescriptor> Descriptors;

	friend class UWoTPayloadSubsystem;
#if WITH_AUTOMATION_TESTS
	/** The guarded cache-decode gate binds a lease without a game instance. */
	friend class FWoTGuardedCacheDecodeAccess;
#endif
};
