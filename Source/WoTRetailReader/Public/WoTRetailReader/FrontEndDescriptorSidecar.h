// Canonical entry descriptor publication, separate from the nineteen worlds.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/FrontEndWorld.h"

/** Seal the entry descriptor to its cache build identity. ProductionDescriptorSha256
 * carries the cache content label; the parameter and sidecar field keep their
 * names for format compatibility. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<uint8>>
WriteSealedFrontEndDescriptorSidecar(
	const FWoTFrontEndDescriptor& Descriptor,
	const FWoTSha256& InstallerSha256,
	const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256);

/** Parse a sealed sidecar against an expected build identity.
 * ExpectedProductionDescriptorSha256 is the cache content label the manifest
 * recorded; the name is kept for format compatibility. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTFrontEndDescriptor>
ParseSealedFrontEndDescriptorSidecar(
	TConstArrayView<uint8> Bytes,
	const FWoTSha256& ExpectedInstallerSha256,
	const FWoTSha256& ExpectedContractSha256,
	const FWoTSha256& ExpectedProductionDescriptorSha256,
	const FWoTSha256& ExpectedDescriptorRoot);

#if WITH_AUTOMATION_TESTS
/** Observe actual sealed parser calls until the final returned token owner is
 * destroyed; final destruction drains callbacks already in flight. */
WOTRETAILREADER_API TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTObserveFrontEndDescriptorParsesForTesting(
	TFunction<void()> Observer);
#endif
