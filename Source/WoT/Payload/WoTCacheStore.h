// One disposable current cache protected by the process profile session.
#pragma once
#include "CoreMinimal.h"
#include "WoTCacheLease.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTRetailReader/CacheBuilder.h"

struct FWoTCacheStoreState;
class WOT_API FWoTCacheStore final : public IWoTObjectWriter
{
public:
	~FWoTCacheStore();
	FWoTCacheStore(FWoTCacheStore&&);
	FWoTCacheStore& operator=(FWoTCacheStore&&);
	FWoTCacheStore(const FWoTCacheStore&) = delete;
	FWoTCacheStore& operator=(const FWoTCacheStore&) = delete;
	static TWoTBootstrapResult<FWoTCacheStore> Open(FString ContentCacheRoot);
	TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> TryOpenCurrent(
		const FWoTCacheGenerationBuildIdentity& Expected, IWoTProgressSink* Progress = nullptr) const;
	FWoTBootstrapStatus BeginBuild(const FWoTProfileSession& Session);
	FWoTBootstrapStatus Publish(const FWoTCacheBuildResult& Build);
	FWoTBootstrapStatus DeleteCache(const FWoTProfileSession& Session);
	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256& Sha256, uint64 MaximumBytes) override;
#if WITH_AUTOMATION_TESTS
	static TWoTBootstrapResult<FWoTCacheStore> ForTesting(FString Root);
#endif
private:
	explicit FWoTCacheStore(TUniquePtr<FWoTCacheStoreState>&& InState);
	TUniquePtr<FWoTCacheStoreState> State;
};
