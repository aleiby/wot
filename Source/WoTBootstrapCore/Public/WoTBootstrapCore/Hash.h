// SHA-256 over the engine's statically linked OpenSSL.
//
// One-shot for in-memory artifacts, streaming for the multi-gigabyte installer and
// release archives that must never be held in memory at once.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

WOTBOOTSTRAPCORE_API FWoTSha256 Sha256(TConstArrayView<uint8> Bytes);

#if WITH_AUTOMATION_TESTS
/**
 * Register one independently owned automation callback.
 *
 * The returned shared pointer is the registration token. Moving transfers one
 * ownership reference; copying deliberately shares it, and only destruction
 * or Reset of the final owner unregisters and drains invocations already in
 * flight. Final-owner reset from the callback itself deactivates immediately;
 * the last callback-exit path performs deferred bookkeeping/event cleanup so
 * self-unregister never waits on itself. WaitForDrain is an automation-only
 * seam whose implementation must not return before its supplied event fires;
 * the default performs the real platform-event wait.
 */
template <typename... TArgs>
TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTRegisterAutomationObserverForTesting(
	FCriticalSection& Guard,
	TArray<TSharedPtr<TFunction<void(TArgs...)>, ESPMode::ThreadSafe>>& Entries,
	TSet<const void*>& ActiveEntries,
	TMap<const void*, int32>& InFlight,
	TMap<const void*, TMap<uint32, int32>>& ActiveThreads,
	TMap<const void*, FEvent*>& DrainedEvents,
	TFunction<void(TArgs...)> Callback,
	TFunction<void()> OnDrainEntered = {},
	TFunction<void(FEvent*)> WaitForDrain = {})
{
	check(Callback);
	if (!Callback)
	{
		return {};
	}
	const TSharedRef<TFunction<void(TArgs...)>, ESPMode::ThreadSafe> Entry =
		MakeShared<TFunction<void(TArgs...)>, ESPMode::ThreadSafe>(
			MoveTemp(Callback));
	// Direct ownership is intentional: a last-lived token must not depend on
	// the engine's process-global event pool during static teardown.
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	FEvent* Drained = FPlatformProcess::CreateSynchEvent(true);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	check(Drained != nullptr);
	if (Drained == nullptr)
	{
		return {};
	}
	const void* Key = &Entry.Get();
	{
		FScopeLock Lock(&Guard);
		Entries.Add(Entry);
		ActiveEntries.Add(Key);
		InFlight.Add(Key, 0);
		ActiveThreads.Add(Key);
		DrainedEvents.Add(Key, Drained);
	}
	return TSharedPtr<uint8, ESPMode::ThreadSafe>(MakeShareable(
		new uint8(0),
		[&Guard, &Entries, &ActiveEntries, &InFlight, &ActiveThreads,
			&DrainedEvents, Entry, Key, Drained,
			OnDrainEntered = MoveTemp(OnDrainEntered),
			WaitForDrain = MoveTemp(WaitForDrain)](uint8* Token)
		{
			bool bDeferredSelfCleanup = false;
			{
				FScopeLock Lock(&Guard);
				const TMap<uint32, int32>* Threads = ActiveThreads.Find(Key);
				int32* Count = InFlight.Find(Key);
				const bool bCurrentCallback = Threads != nullptr
					&& Threads->Contains(FPlatformTLS::GetCurrentThreadId());
				const int32 RemovedActive = ActiveEntries.Remove(Key);
				const int32 RemovedEntry = Entries.RemoveSingleSwap(Entry);
				check(RemovedActive == 1);
				check(RemovedEntry == 1);
				check(Count != nullptr);
				if (bCurrentCallback)
				{
					check(Count != nullptr && *Count > 0);
					// A negative count denotes deactivated self-unregister. Every
					// callback exit advances it toward zero; the last one owns cleanup.
					*Count = -*Count;
					bDeferredSelfCleanup = true;
				}
				else if (Count != nullptr && *Count == 0)
				{
					Drained->Trigger();
				}
			}
			if (OnDrainEntered)
			{
				OnDrainEntered();
			}
			if (bDeferredSelfCleanup)
			{
				delete Token;
				return;
			}
			if (WaitForDrain)
			{
				WaitForDrain(Drained);
			}
			else
			{
				Drained->Wait();
			}
			{
				FScopeLock Lock(&Guard);
				check(InFlight.FindRef(Key) == 0);
				const int32 RemovedInFlight = InFlight.Remove(Key);
				const int32 RemovedThreads = ActiveThreads.Remove(Key);
				const int32 RemovedDrained = DrainedEvents.Remove(Key);
				check(RemovedInFlight == 1);
				check(RemovedThreads == 1);
				check(RemovedDrained == 1);
			}
			delete Drained;
			delete Token;
		}));
}

/** Dispatch one real instrumentation event to every live registration. */
template <typename... TArgs>
void WoTBroadcastAutomationObserverForTesting(
	FCriticalSection& Guard,
	const TArray<TSharedPtr<TFunction<void(TArgs...)>, ESPMode::ThreadSafe>>& Entries,
	const TSet<const void*>& ActiveEntries,
	TMap<const void*, int32>& InFlight,
	TMap<const void*, TMap<uint32, int32>>& ActiveThreads,
	TMap<const void*, FEvent*>& DrainedEvents,
	TArgs... Args)
{
	TArray<TSharedPtr<TFunction<void(TArgs...)>, ESPMode::ThreadSafe>> Snapshot;
	{
		FScopeLock Lock(&Guard);
		Snapshot = Entries;
	}
	for (const TSharedPtr<TFunction<void(TArgs...)>, ESPMode::ThreadSafe>& Entry
		: Snapshot)
	{
		TFunction<void(TArgs...)> Callback;
		const void* Key = Entry.Get();
		{
			FScopeLock Lock(&Guard);
			if (!ActiveEntries.Contains(Key))
			{
				continue;
			}
			int32* Count = InFlight.Find(Key);
			TMap<uint32, int32>* Threads = ActiveThreads.Find(Key);
			check(Count != nullptr && Threads != nullptr);
			++(*Count);
			++Threads->FindOrAdd(FPlatformTLS::GetCurrentThreadId());
			Callback = *Entry;
		}
		Callback(Args...);
		FEvent* DeferredSelfDrained = nullptr;
		{
			FScopeLock Lock(&Guard);
			int32* Count = InFlight.Find(Key);
			TMap<uint32, int32>* Threads = ActiveThreads.Find(Key);
			FEvent* const* Drained = DrainedEvents.Find(Key);
			check(Count != nullptr && *Count != 0
				&& Threads != nullptr && Drained != nullptr);
			int32* ThreadCount = Threads->Find(FPlatformTLS::GetCurrentThreadId());
			check(ThreadCount != nullptr && *ThreadCount > 0);
			if (--(*ThreadCount) == 0)
			{
				Threads->Remove(FPlatformTLS::GetCurrentThreadId());
			}
			if (*Count < 0)
			{
				if (++(*Count) == 0)
				{
					check(!ActiveEntries.Contains(Key));
					DeferredSelfDrained = *Drained;
					const int32 RemovedInFlight = InFlight.Remove(Key);
					const int32 RemovedThreads = ActiveThreads.Remove(Key);
					const int32 RemovedDrained = DrainedEvents.Remove(Key);
					check(RemovedInFlight == 1);
					check(RemovedThreads == 1);
					check(RemovedDrained == 1);
				}
			}
			else if (--(*Count) == 0 && !ActiveEntries.Contains(Key))
			{
				(*Drained)->Trigger();
			}
		}
		delete DeferredSelfDrained;
	}
}

/** Observe actual one-shot SHA-256 calls in automation code paths. */
WOTBOOTSTRAPCORE_API TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTObserveSha256ForTesting(
	TFunction<void(TConstArrayView<uint8>)> Observer,
	TFunction<void()> OnDrainEntered = {},
	TFunction<void(FEvent*)> WaitForDrain = {});
#endif

/**
 * Incremental SHA-256.
 *
 * Move-only: an OpenSSL digest context is a live resource, and duplicating one
 * would silently fork a hash that callers believe is a single stream.
 */
class WOTBOOTSTRAPCORE_API FWoTSha256Hasher final
{
public:
	FWoTSha256Hasher();
	~FWoTSha256Hasher();

	FWoTSha256Hasher(FWoTSha256Hasher&& Other);
	FWoTSha256Hasher& operator=(FWoTSha256Hasher&& Other);

	FWoTSha256Hasher(const FWoTSha256Hasher&) = delete;
	FWoTSha256Hasher& operator=(const FWoTSha256Hasher&) = delete;

	void Update(TConstArrayView<uint8> Bytes);

	/** Finishes the digest. Calling Update afterwards is a programming error. */
	FWoTSha256 Finish();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
