#pragma once
#include "../Payload/WoTCacheStore.h"
#if WITH_AUTOMATION_TESTS
// Semantic unit fixture convenience only. Acceptance tests call Open and
// TryOpenCurrent with the compiled production identities themselves.
inline TWoTBootstrapResult<FWoTCacheGenerationLease> WoTAcquireCurrentForTesting(FWoTCacheStore& Store)
{
	using FResult = TWoTBootstrapResult<FWoTCacheGenerationLease>;
	auto Current = Store.TryOpenCurrent({}, nullptr);
	if (Current.IsError()) return FResult::Error(MoveTemp(Current).GetError());
	if (!Current.GetValue().IsSet()) return FResult::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, TEXT("the fixture current cache is absent or invalid")));
	return FResult::Success(MoveTemp(Current.GetValue().GetValue()));
}
inline TWoTBootstrapResult<FWoTCacheGenerationLease> WoTVerifyCurrentForTesting(
	FWoTCacheStore& Store, TConstArrayView<FString> LogicalIds, IWoTProgressSink* Progress = nullptr)
{
	auto Lease = WoTAcquireCurrentForTesting(Store);
	if (Lease.IsError()) return Lease;
	auto Verified = Lease.GetValue().VerifyCriticalClosureForTesting(LogicalIds, Progress);
	if (Verified.IsError()) return TWoTBootstrapResult<FWoTCacheGenerationLease>::Error(MoveTemp(Verified).GetError());
	return Lease;
}
#endif
