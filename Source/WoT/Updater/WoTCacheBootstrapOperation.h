// One in-process extract/build/verify cache publication operation.
#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTCacheStore.h"
#include "WoTInstallerAcquisition.h"
#include "WoTRetailReader/Contract.h"

class FWoTRetailPackage;

/** Proof returned only after publication and a final held-installer reverify. */
struct FWoTPublishedCacheReceipt
{
	FWoTSha256 InstallerSha256;
	FWoTSha256 ContractSha256;
	FWoTSha256 ProductionDescriptorSha256;
	FWoTSha256 ManifestSha256;
	uint64 CompletedProgressUnits = 0;
	uint64 TotalProgressUnits = 0;
};

class WOT_API FWoTCacheBootstrapOperation final
{
public:
	FWoTCacheBootstrapOperation() = default;

#if WITH_AUTOMATION_TESTS
	using FBuildForTesting = TFunction<TWoTBootstrapResult<FWoTCacheBuildResult>(
		FWoTRetailPackage&, const FWoTRetailContract&, IWoTObjectWriter&,
		IWoTProgressSink&)>;
	explicit FWoTCacheBootstrapOperation(FBuildForTesting InBuildForTesting)
		: BuildForTesting(MoveTemp(InBuildForTesting))
	{
	}
#endif

	TWoTBootstrapResult<FWoTPublishedCacheReceipt> BuildAndPublish(
		FWoTVerifiedInstaller&& Installer, FWoTCacheStore& Store,
		IWoTProgressSink& Progress);
	TWoTBootstrapResult<FWoTPublishedCacheReceipt> BuildAndPublish(
		FWoTRetailPackage&& Package, FWoTCacheStore& Store,
		IWoTProgressSink& Progress);

private:
#if WITH_AUTOMATION_TESTS
	FBuildForTesting BuildForTesting;
#endif
};
