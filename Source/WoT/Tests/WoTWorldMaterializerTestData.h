#pragma once

#include "CoreMinimal.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/Contract.h"

#if WITH_AUTOMATION_TESTS

class UWoTPayloadSubsystem;
class FWoTRetailPackage;
struct FWoTFrontEndDescriptor;

constexpr uint8 WoTFrontEndWorldManifestAttackNone = 0;
constexpr uint8 WoTFrontEndWorldManifestAttackMissingWorld = 1;
constexpr uint8 WoTFrontEndWorldManifestAttackMissingBSP = 2;
constexpr uint8 WoTFrontEndWorldManifestAttackWrongWorldFact = 3;
constexpr uint8 WoTFrontEndWorldManifestAttackWrongBSPSchema = 4;
constexpr uint8 WoTFrontEndWorldManifestAttackExtraWorld = 5;
constexpr uint8 WoTFrontEndWorldManifestAttackExtraBSP = 6;

/** Opaque exact descriptor fixture, kept out of this normal-world header's
 * dependency graph. */
const FWoTFrontEndDescriptor*
WoTWorldMaterializerTest_CreateFrontEndAuthority(FString& OutError);
void WoTWorldMaterializerTest_DestroyFrontEndAuthority(
	const FWoTFrontEndDescriptor* Descriptor);

/** Closed synthetic world/BSP contract used by the real cache-operation path. */
FWoTRetailContract WoTWorldMaterializerTest_WorldContract();

/** Operation-compatible builder for the complete nineteen-world fixture. */
TWoTBootstrapResult<FWoTCacheBuildResult>
WoTWorldMaterializerTest_BuildWorldSet(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress);

/** Publish the complete nineteen-record cache fixture used by real route tests. */
bool WoTWorldMaterializerTest_PublishWorldSet(
	const FString& Root, TArray<FWoTWorldDescriptor>& OutDescriptors,
	FString& OutError,
	const FWoTFrontEndDescriptor* FrontEndDescriptor = nullptr,
	uint8 FrontEndAttack = WoTFrontEndWorldManifestAttackNone,
	bool bIncludeLazyRuntimeAnimation = false);

/** Activate the fixture and atomically authorize its exact descriptor set. */
bool WoTWorldMaterializerTest_ActivateAndAuthorize(
	UWoTPayloadSubsystem* Subsystem, const FString& Root,
	TArray<FWoTWorldDescriptor>&& Descriptors, FString& OutError);

/** Framed world-v3 object whose spawn-pool actor authors RespawnTime=NaN. */
TArray<uint8> WoTWorldMaterializerTest_MalformedPoolValueObject();

#endif // WITH_AUTOMATION_TESTS
