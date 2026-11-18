// World descriptor records embedded in the completed cache manifest.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTBootstrapCore/CanonicalJson.h"

/** Encode the ordered nineteen-map set (or an empty importer-fixture set). */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTCanonicalJsonValue>
WriteCacheWorldDescriptors(TConstArrayView<FWoTWorldDescriptor> Descriptors);

/** Strictly parse descriptor records; the containing manifest owns their identity. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>
ParseCacheWorldDescriptors(const FWoTCanonicalJsonValue& Descriptors);
