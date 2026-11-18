#include "WoTVerifiedBlobReader.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "WoTCacheLease.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "../WoTBootstrapHeartbeat.h"

namespace
{
#if WITH_AUTOMATION_TESTS
FCriticalSection GWoTVerifiedBlobObserverGuard;
TFunction<void()> GWoTVerifiedBlobBodyReadObserver;
#endif

template <typename T>
TWoTBootstrapResult<T> WoTVerifiedBlobError(EWoTBootstrapError Code,
	const TCHAR* Message, const FString& Path = FString())
{
	return TWoTBootstrapResult<T>::Error(
		MakeWoTBootstrapError(Code, Message, Path));
}

TWoTBootstrapResult<FString> WoTVerifiedBlobObjectName(FStringView LogicalId)
{
	if (LogicalId.IsEmpty() || LogicalId.Len() > 4096)
	{
		return WoTVerifiedBlobError<FString>(EWoTBootstrapError::UnsafePath,
			TEXT("a cache logical id must be non-empty and bounded"), FString(LogicalId));
	}
	const FTCHARToUTF8 Utf8(LogicalId.GetData(), LogicalId.Len());
	const FWoTSha256 IdSha = Sha256(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	return TWoTBootstrapResult<FString>::Success(
		IdSha.ToHex() + TEXT(".blob"));
}
}

#if WITH_AUTOMATION_TESTS
void WoTSetVerifiedBlobBodyReadObserverForTesting(TFunction<void()> Observer)
{
	FScopeLock Lock(&GWoTVerifiedBlobObserverGuard);
	GWoTVerifiedBlobBodyReadObserver = MoveTemp(Observer);
}
#endif

TWoTBootstrapResult<FWoTImmutableVerifiedBlob> ReadVerifiedCacheBlob(
	const FWoTCacheGenerationLease& Lease, const FWoTCacheBlobDescriptor& Descriptor,
	const FWoTCacheBlobReadLimits& Limits)
{
	using FResult = TWoTBootstrapResult<FWoTImmutableVerifiedBlob>;
	if (!Lease.IsValid())
	{
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::Io, TEXT("the cache generation lease is not live"));
	}
	bool bMustRevoke = false;
	bool bBeganRevocation = false;
	bool bContentIntegrityFailure = false;
	bool bRuntimeIntegrityFailure = false;
	FString FailureReason;
	TArray<TFunction<bool()>> RevocationCallbacks;
	auto MarkContentIntegrityFailure = [&](FString Reason)
	{
		bMustRevoke = true;
		bContentIntegrityFailure = true;
		FailureReason = MoveTemp(Reason);
	};
	auto MarkOrdinaryFailure = [&](FString Reason)
	{
		bMustRevoke = true;
		FailureReason = MoveTemp(Reason);
	};
	FResult ReadResult = [&]() -> FResult
	{
	FScopeLock SerializedRead(&Lease.GetVerifiedReadGuard());
	ON_SCOPE_EXIT
	{
		if (bMustRevoke)
		{
			bRuntimeIntegrityFailure = bContentIntegrityFailure;
			bBeganRevocation = bRuntimeIntegrityFailure
				? Lease.BeginRuntimeIntegrityFailureUnderVerifiedReadGuard(
					Descriptor.LogicalId, FailureReason,
					RevocationCallbacks)
				: Lease.BeginRevocationUnderVerifiedReadGuard(
					RevocationCallbacks);
		}
	};
	if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
	{
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::Io,
			TEXT("the cache generation rejects new verified reads during revocation"));
	}
	const FWoTCacheLeaseManifestEntry* Authorized =
		Lease.FindManifestEntry(Descriptor.LogicalId);
	if (Authorized == nullptr || Authorized->Schema != Descriptor.Schema
		|| Authorized->Size != Descriptor.Size
		|| Authorized->Sha256 != Descriptor.Sha256)
	{
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the caller descriptor does not exactly match the leased manifest"),
			Descriptor.LogicalId);
	}
	if (Descriptor.Size > Limits.MaximumBlobBytes
		|| Descriptor.Size > Limits.MaximumAllocationBytes
		|| Descriptor.Size > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::SizeLimit,
			TEXT("the cache blob exceeds its bounded read or allocation limit"),
			Descriptor.LogicalId);
	}
	bool bAllowMemoizedRead = true;
#if WITH_AUTOMATION_TESTS
	bAllowMemoizedRead = !Limits.ImmediatelyBeforeSingleReadForTesting
		&& !Limits.ConcurrentWithSingleReadForTesting
		&& !Limits.ImmediatelyBeforeReadAfterWorkerStartedForTesting
		&& !Limits.BodyReadFailureForTesting;
#endif
	if (TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Memoized =
		bAllowMemoizedRead ? Lease.FindVerifiedBuffer(Descriptor) : nullptr;
		Memoized.IsValid())
	{
		FWoTImmutableVerifiedBlob Result;
		Result.Buffer = MoveTemp(Memoized);
		return FResult::Success(MoveTemp(Result));
	}

	TWoTBootstrapResult<FString> ObjectName =
		WoTVerifiedBlobObjectName(Descriptor.LogicalId);
	if (ObjectName.IsError())
	{
		return FResult::Error(MoveTemp(ObjectName).GetError());
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> EntryType =
		QueryNativeEntryTypeAtNoFollow(
			Lease.GetBlobDirectory(), ObjectName.GetValue());
	if (EntryType.IsError())
	{
		MarkOrdinaryFailure(EntryType.GetError().Message);
		return FResult::Error(MoveTemp(EntryType).GetError());
	}
	if (!EntryType.GetValue().IsSet())
	{
		MarkContentIntegrityFailure(TEXT("the cache blob is missing"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache blob is missing"), Descriptor.LogicalId);
	}
	if (EntryType.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob is not a single-link regular object"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::UnsafePath,
			TEXT("the cache blob is not a single-link regular object"),
			Descriptor.LogicalId);
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
		OpenNativeFileAtReadNoFollow(Lease.GetBlobDirectory(), ObjectName.GetValue());
	if (Opened.IsError())
	{
		TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Rechecked =
			QueryNativeEntryTypeAtNoFollow(
				Lease.GetBlobDirectory(), ObjectName.GetValue());
		if (!Rechecked.IsError() && !Rechecked.GetValue().IsSet())
		{
			MarkContentIntegrityFailure(TEXT("the cache blob is missing"));
		}
		else
		{
			MarkOrdinaryFailure(Opened.GetError().Message);
		}
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	FWoTNativeFileHandle& File = Opened.GetValue();
	const FWoTNativeFileIdentity OpenIdentity = File.GetIdentity();
	if (OpenIdentity.Type != EWoTTreeEntryType::File || OpenIdentity.LinkCount != 1)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob is not a single-link regular object"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::UnsafePath,
			TEXT("the cache blob is not a single-link regular object"),
			Descriptor.LogicalId);
	}
	if (OpenIdentity.Size != Descriptor.Size)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob size differs from its manifest descriptor"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache blob size differs from its manifest descriptor"),
			Descriptor.LogicalId);
	}

#if WITH_AUTOMATION_TESTS
	if (Limits.ImmediatelyBeforeSingleReadForTesting)
	{
		Limits.ImmediatelyBeforeSingleReadForTesting();
	}
	TFuture<void> ConcurrentMutation;
	if (Limits.ConcurrentWithSingleReadForTesting)
	{
		ConcurrentMutation = Async(EAsyncExecution::Thread,
			[Callback = Limits.ConcurrentWithSingleReadForTesting]()
			{
				Callback();
			});
	}
	if (Limits.ImmediatelyBeforeReadAfterWorkerStartedForTesting)
	{
		Limits.ImmediatelyBeforeReadAfterWorkerStartedForTesting();
	}
#endif

	// Exactly one complete handle read. The same private allocation is measured,
	// hashed, and ultimately exposed through an immutable view; no other bytes are
	// reopened, reread, mapped, or parsed.
#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&GWoTVerifiedBlobObserverGuard);
		if (GWoTVerifiedBlobBodyReadObserver)
		{
			GWoTVerifiedBlobBodyReadObserver();
		}
	}
	WoTObserveCacheSnapshotPhysicalReadForTesting(
		FString::Printf(TEXT("blobs/%s"), *Descriptor.LogicalId));
#endif
	TWoTBootstrapResult<TArray<uint8>> Read = File.ReadAll();
#if WITH_AUTOMATION_TESTS
	if (Limits.BodyReadFailureForTesting)
	{
		TOptional<FWoTBootstrapError> Injected =
			Limits.BodyReadFailureForTesting();
		if (Injected.IsSet())
		{
			Read = TWoTBootstrapResult<TArray<uint8>>::Error(
				MoveTemp(Injected.GetValue()));
		}
	}
#endif
#if WITH_AUTOMATION_TESTS
	if (ConcurrentMutation.IsValid())
	{
		ConcurrentMutation.Wait();
	}
#endif
	if (Read.IsError())
	{
		MarkOrdinaryFailure(Read.GetError().Message);
		return FResult::Error(MoveTemp(Read).GetError());
	}
	TArray<uint8> Buffer = MoveTemp(Read).GetValue();
	if (static_cast<uint64>(Buffer.Num()) != Descriptor.Size)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob read returned a short or overlong object"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache blob read returned a short or overlong object"),
			Descriptor.LogicalId);
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Current = File.QueryCurrentIdentity();
	if (Current.IsError())
	{
		MarkOrdinaryFailure(Current.GetError().Message);
		return FResult::Error(MoveTemp(Current).GetError());
	}
	const FWoTNativeFileIdentity& CurrentIdentity = Current.GetValue();
	if (!CurrentIdentity.IsSameObject(OpenIdentity)
		|| CurrentIdentity.Type != EWoTTreeEntryType::File
		|| CurrentIdentity.LinkCount != 1 || CurrentIdentity.Size != Descriptor.Size)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob mutated during its single verified read"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache blob mutated during its single verified read"),
			Descriptor.LogicalId);
	}
	if (Sha256(Buffer) != Descriptor.Sha256)
	{
		MarkContentIntegrityFailure(
			TEXT("the cache blob digest differs from its manifest descriptor"));
		return WoTVerifiedBlobError<FWoTImmutableVerifiedBlob>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache blob digest differs from its manifest descriptor"),
			Descriptor.LogicalId);
	}

	FWoTImmutableVerifiedBlob Result;
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Shared =
		MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Buffer));
	Lease.RememberVerifiedBuffer(Descriptor, Shared);
	Result.Buffer = MoveTemp(Shared);
	return FResult::Success(MoveTemp(Result));
	}();
	// Corruption revocation invokes synchronous presentation teardown barriers.
	// Do that only after releasing the serialized verified-read scope above.
	if (bBeganRevocation && bRuntimeIntegrityFailure)
	{
		const FString MarkerError = Lease.MarkDamagedBestEffort();
		WoTRecordBootstrapHeartbeat(TEXT("runtime-cache-integrity-failure"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("output=%s reason=%s manifest=%s marker_error=%s"),
				*Descriptor.LogicalId, *FailureReason,
				*Lease.GetManifestSha256().ToHex(), *MarkerError));
	}
	if (bBeganRevocation)
		Lease.FinishRevocation(MoveTemp(RevocationCallbacks));
	return ReadResult;
}
