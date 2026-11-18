#include "WoTPayloadActivation.h"

#include "Misc/ScopeLock.h"
#include "WoTCacheLease.h"

namespace
{
class FWoTTryCriticalSectionGuard final
{
public:
	explicit FWoTTryCriticalSectionGuard(FCriticalSection& InGuard)
		: Guard(InGuard.TryLock() ? &InGuard : nullptr)
	{
	}

	~FWoTTryCriticalSectionGuard()
	{
		if (Guard != nullptr)
		{
			Guard->Unlock();
		}
	}

	FWoTTryCriticalSectionGuard(const FWoTTryCriticalSectionGuard&) = delete;
	FWoTTryCriticalSectionGuard& operator=(
		const FWoTTryCriticalSectionGuard&) = delete;

	bool IsLocked() const { return Guard != nullptr; }

private:
	FCriticalSection* Guard;
};

FCriticalSection WoTPayloadActivationGuard;
const FWoTCacheGenerationLease* WoTPayloadActiveLease = nullptr;
bool bWoTPayloadBootstrapActive = false;
bool bWoTBootstrapWorldReady = false;
FWoTBootstrapWorldReadinessEvent WoTBootstrapWorldReadinessChanged;
#if WITH_AUTOMATION_TESTS
bool bWoTPayloadAutomationLease = false;
FCriticalSection WoTPayloadAdoptionObserverGuard;
TFunction<void(FString, const FWoTSha256&)>
	WoTPayloadAdoptionObserver;
#endif

bool WoTPayloadClearPublication()
{
	const bool bWasWorldReady = bWoTBootstrapWorldReady;
	WoTPayloadActiveLease = nullptr;
	bWoTPayloadBootstrapActive = false;
	bWoTBootstrapWorldReady = false;
#if WITH_AUTOMATION_TESTS
	bWoTPayloadAutomationLease = false;
#endif
	return bWasWorldReady;
}
}

bool IsWoTPayloadBootstrapActive()
{
	// This is a readiness predicate on world/resource load paths. It must never
	// turn absent or contended state into a wait, so fail closed and let the
	// caller return its load error.
	FWoTTryCriticalSectionGuard Lock(WoTPayloadActivationGuard);
	if (!Lock.IsLocked())
	{
		return false;
	}
	return bWoTPayloadBootstrapActive
		&& ((WoTPayloadActiveLease != nullptr
				&& WoTPayloadActiveLease->IsValid())
#if WITH_AUTOMATION_TESTS
			|| bWoTPayloadAutomationLease
#endif
		);
}

bool IsWoTBootstrapWorldReady()
{
	FWoTTryCriticalSectionGuard Lock(WoTPayloadActivationGuard);
	if (!Lock.IsLocked())
	{
		return false;
	}
	return bWoTBootstrapWorldReady && bWoTPayloadBootstrapActive
		&& ((WoTPayloadActiveLease != nullptr
				&& WoTPayloadActiveLease->IsValid())
#if WITH_AUTOMATION_TESTS
			|| bWoTPayloadAutomationLease
#endif
		);
}

FDelegateHandle RegisterWoTBootstrapWorldReadinessListener(
	FWoTBootstrapWorldReadinessEvent::FDelegate&& Listener)
{
	return WoTBootstrapWorldReadinessChanged.Add(MoveTemp(Listener));
}

void UnregisterWoTBootstrapWorldReadinessListener(FDelegateHandle& Handle)
{
	if (Handle.IsValid())
	{
		WoTBootstrapWorldReadinessChanged.Remove(Handle);
		Handle.Reset();
	}
}

FWoTBootstrapStatus FWoTPayloadActivationPublication::
	PublishWoTPayloadBootstrapActivation(
	const FWoTCacheGenerationLease& Lease)
{
	{
		FScopeLock Lock(&WoTPayloadActivationGuard);
		if (bWoTPayloadBootstrapActive || !Lease.IsValid())
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("payload publication requires one matching live generation lease")));
		}
		WoTPayloadActiveLease = &Lease;
		bWoTPayloadBootstrapActive = true;
	}
#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&WoTPayloadAdoptionObserverGuard);
		if (WoTPayloadAdoptionObserver)
		{
			WoTPayloadAdoptionObserver(Lease.GetManifestSha256().ToHex(),
				Lease.GetManifestSha256());
		}
	}
#endif
	return FWoTBootstrapStatus::Success();
}

void FWoTPayloadActivationPublication::
	RevokeWoTPayloadBootstrapActivation(
	const FWoTCacheGenerationLease& Lease)
{
	bool bNotifyDeactivation = false;
	{
		FScopeLock Lock(&WoTPayloadActivationGuard);
		if (WoTPayloadActiveLease == &Lease)
		{
			bNotifyDeactivation = WoTPayloadClearPublication();
		}
	}
	if (bNotifyDeactivation)
	{
		WoTBootstrapWorldReadinessChanged.Broadcast(false);
	}
}

FWoTBootstrapStatus PublishWoTBootstrapWorldReady()
{
	{
		FScopeLock Lock(&WoTPayloadActivationGuard);
		const bool bLeaseLive =
			WoTPayloadActiveLease != nullptr && WoTPayloadActiveLease->IsValid();
		if (!bWoTPayloadBootstrapActive
			|| (!bLeaseLive
#if WITH_AUTOMATION_TESTS
				&& !bWoTPayloadAutomationLease
#endif
				)
			|| bWoTBootstrapWorldReady)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("world authorization was stale, mismatched, or already published")));
		}
		bWoTBootstrapWorldReady = true;
	}
	WoTBootstrapWorldReadinessChanged.Broadcast(true);
	return FWoTBootstrapStatus::Success();
}

#if WITH_AUTOMATION_TESTS
void WoTSetPayloadLeaseAdoptionObserverForTesting(
	TFunction<void(FString, const FWoTSha256&)> Observer)
{
	FScopeLock Lock(&WoTPayloadAdoptionObserverGuard);
	WoTPayloadAdoptionObserver = MoveTemp(Observer);
}

FWoTBootstrapStatus PublishWoTPayloadBootstrapActivationForTesting()
{
	FScopeLock Lock(&WoTPayloadActivationGuard);
	if (bWoTPayloadBootstrapActive)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("test payload publication requires an inactive snapshot state")));
	}
	bWoTPayloadBootstrapActive = true;
	bWoTPayloadAutomationLease = true;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus PublishWoTBootstrapWorldReadyForTesting()
{
	{
		FScopeLock Lock(&WoTPayloadActivationGuard);
		if (!bWoTPayloadBootstrapActive || !bWoTPayloadAutomationLease
			|| bWoTBootstrapWorldReady)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("test world publication requires the active snapshot state")));
		}
		bWoTBootstrapWorldReady = true;
	}
	WoTBootstrapWorldReadinessChanged.Broadcast(true);
	return FWoTBootstrapStatus::Success();
}

void ResetWoTPayloadActivationForTesting()
{
	bool bNotifyDeactivation = false;
	{
		FScopeLock Lock(&WoTPayloadActivationGuard);
		bNotifyDeactivation = WoTPayloadClearPublication();
	}
	if (bNotifyDeactivation)
	{
		WoTBootstrapWorldReadinessChanged.Broadcast(false);
	}
}
#endif
