#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"

#if WITH_AUTOMATION_TESTS

class UWoTPayloadSubsystem;
class FWoTCacheGenerationLease;

struct FWoTPayloadActivationTestObject
{
	FString LogicalId;
	FString Schema;
	TArray<uint8> Bytes;
};

/** Frame one importer payload in the canonical cache-v3 object envelope. */
TArray<uint8> WoTPayloadActivationTest_FrameObject(
	FStringView CanonicalHeader, TConstArrayView<uint8> Payload);

/** Publish one exact typed object through the real cache store. */
bool WoTPayloadActivationTest_PublishObject(
	const FString& Root, const FString& LogicalId, const FString& Schema,
	TConstArrayView<uint8> ObjectBytes, FString& OutError);

bool WoTPayloadActivationTest_PublishObjects(
	const FString& Root, TConstArrayView<FWoTPayloadActivationTestObject> Objects,
	FString& OutError);

/**
 * Acquire the published generation and activate the production payload
 * subsystem from that exact verified snapshot.
 */
bool WoTPayloadActivationTest_ActivatePublished(
	UWoTPayloadSubsystem* Subsystem, const FString& Root, FString& OutError);

FWoTBootstrapStatus
WoTPayloadActivationTest_ActivatePublishedForWorldAuthorization(
	UWoTPayloadSubsystem* Subsystem, const FString& Root);

FWoTBootstrapStatus
WoTPayloadActivationTest_ActivateLeaseForWorldAuthorization(
	UWoTPayloadSubsystem* Subsystem, const FWoTCacheGenerationLease& Lease);

#endif // WITH_AUTOMATION_TESTS
