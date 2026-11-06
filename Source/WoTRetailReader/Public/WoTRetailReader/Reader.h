// Public native retail-reader facade.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTRetailReader/CacheBuilder.h"

class FWoTRetailPackage;
struct FWoTRetailContract;
class IWoTObjectWriter;
class IWoTProgressSink;

/** The sole complete definition of the native reader coordinator. */
class WOTRETAILREADER_API FWoTRetailReader final
{
public:
	TWoTBootstrapResult<FWoTCacheBuildResult> Build(
		FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
		IWoTObjectWriter& Writer,
		IWoTProgressSink& Progress);
};
