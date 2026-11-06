// Exception-free UE wrapper around the callback-only innoextract C ABI.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Error.h"

/** The only extraction destination: callers decide what storage, if any, receives bytes. */
class IWoTByteSink
{
public:
	virtual ~IWoTByteSink() = default;
	virtual FWoTBootstrapStatus Consume(TConstArrayView<uint8> Bytes) = 0;
};

struct FWoTInnoEntryDescription
{
	uint32 AdapterIndex = 0;
	FString Name;
	uint64 Size = 0;
	FString ChecksumType;
	FString Checksum;
	FString Compression;
};

/** Internal package-parser owner; all C allocations die with this object. */
class WOTRETAILREADER_API FWoTInnoExtractor final
{
public:
	static TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>> Open(
		TArray<uint8>&& ApprovedInput);
	static TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>> OpenWithApprovedDigest(
		TArray<uint8>&& ApprovedInput, const FWoTSha256& ApprovedSha256);

	~FWoTInnoExtractor();
	FWoTInnoExtractor(FWoTInnoExtractor&& Other);
	FWoTInnoExtractor& operator=(FWoTInnoExtractor&& Other);
	FWoTInnoExtractor(const FWoTInnoExtractor&) = delete;
	FWoTInnoExtractor& operator=(const FWoTInnoExtractor&) = delete;

	const TArray<FWoTInnoEntryDescription>& Entries() const;
	const FString& Description() const;
	const FWoTSha256& CachedInputSha256() const;
	FWoTBootstrapStatus Extract(uint32 AdapterIndex, uint64 ExpectedSize,
		IWoTByteSink& Sink) const;

private:
	struct FImpl;
	explicit FWoTInnoExtractor(TUniquePtr<FImpl>&& InImpl);
	TUniquePtr<FImpl> Impl;
};
