// Held-snapshot retail package index and bounded streaming API.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArchiveRules.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/InnoExtractor.h"

/** The complete immutable description paired with its already-open installer handle. */
struct FWoTHeldInstallerSnapshot
{
	FWoTNativeFileHandle Handle;
	uint64 Size = 0;
	FWoTSha256 Sha256;
	FWoTNativeFileIdentity Identity;
};

struct FWoTPackageEntry
{
	uint32 AdapterIndex = 0;
	FString LogicalPath;
	uint64 Size = 0;
	FString ChecksumType;
	FString Checksum;
	FString Compression;
};

class WOTRETAILREADER_API FWoTRetailPackage final
{
public:
	static TWoTBootstrapResult<FWoTRetailPackage> Open(
		FWoTHeldInstallerSnapshot&& Snapshot, const FWoTArchiveLimits& Limits);

	FWoTRetailPackage();
	~FWoTRetailPackage();
	FWoTRetailPackage(FWoTRetailPackage&& Other);
	FWoTRetailPackage& operator=(FWoTRetailPackage&& Other);
	FWoTRetailPackage(const FWoTRetailPackage&) = delete;
	FWoTRetailPackage& operator=(const FWoTRetailPackage&) = delete;

	TWoTBootstrapResult<FWoTPackageEntry> FindExact(FStringView LogicalPath) const;
	FWoTBootstrapStatus StreamExact(const FWoTPackageEntry& Entry,
		IWoTByteSink& Sink) const;
	FWoTBootstrapStatus ReverifyHeldSnapshotAfterExtraction() const;
	FWoTSha256 CachedInputSha256() const;
	uint64 EntryBufferHighWaterBytes() const;

#if WITH_AUTOMATION_TESTS
	using FBeforeReverifyForTesting = void (*)(void* Context);
	void SetBeforeReverifyForTesting(FBeforeReverifyForTesting Callback, void* Context);
	uint64 EntryDecodeCountForTesting() const;
#endif

private:
	struct FImpl;
	explicit FWoTRetailPackage(TUniquePtr<FImpl>&& InImpl);
	TUniquePtr<FImpl> Impl;
};
