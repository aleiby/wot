// Process-local bootstrap state shared by guarded runtime behavior owners.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Error.h"

class FWoTCacheGenerationLease;
class UWoTPayloadSubsystem;

/** Dormant until UWoTPayloadSubsystem atomically adopts a verified cache lease. */
WOT_API bool IsWoTPayloadBootstrapActive();

/** False until the authorized world receipt is published. */
WOT_API bool IsWoTBootstrapWorldReady();

/** Controlled readiness notification. A callback must still recheck
 * IsWoTBootstrapWorldReady(); the notification itself conveys no authority. */
DECLARE_MULTICAST_DELEGATE_OneParam(
	FWoTBootstrapWorldReadinessEvent, bool /*bReady*/);
WOT_API FDelegateHandle RegisterWoTBootstrapWorldReadinessListener(
	FWoTBootstrapWorldReadinessEvent::FDelegate&& Listener);
WOT_API void UnregisterWoTBootstrapWorldReadinessListener(
	FDelegateHandle& Handle);

/**
 * Snapshot-lifetime publication is private to the subsystem retaining the
 * verified cache snapshot.
 */
class WOT_API FWoTPayloadActivationPublication final
{
private:
	static FWoTBootstrapStatus PublishWoTPayloadBootstrapActivation(
		const FWoTCacheGenerationLease& Lease);
	static void RevokeWoTPayloadBootstrapActivation(
		const FWoTCacheGenerationLease& Lease);

	friend class UWoTPayloadSubsystem;
};

/** Publishes readiness after the complete front-end/world initialization. */
WOT_API FWoTBootstrapStatus PublishWoTBootstrapWorldReady();

#if WITH_AUTOMATION_TESTS
WOT_API FWoTBootstrapStatus PublishWoTPayloadBootstrapActivationForTesting();
WOT_API FWoTBootstrapStatus PublishWoTBootstrapWorldReadyForTesting();
WOT_API void ResetWoTPayloadActivationForTesting();
WOT_API void WoTSetPayloadLeaseAdoptionObserverForTesting(
	TFunction<void(FString, const FWoTSha256&)> Observer);
#endif
