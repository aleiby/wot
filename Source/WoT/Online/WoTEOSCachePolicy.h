#pragma once

#include "CoreMinimal.h"

#if WOT_WITH_EOS && WITH_EOS_SDK
#include "eos_types.h"
#include "WoTBootstrapCore/Error.h"

class IEOSSDKManager;

/**
 * Replaces EOS's platform default cache directory with Data/Online/EOS. The
 * UTF-8 storage remains alive for the whole EOS platform lifetime.
 */
class FWoTEOSCachePolicy final
{
public:
	explicit FWoTEOSCachePolicy(FStringView HeldCacheDirectory);

	void Apply(EOS_Platform_Options& Options) const;
	/** The one registration path used by guarded startup and its integration spec. */
	FDelegateHandle Register(IEOSSDKManager& Manager);
	const FString& Directory() const { return CacheDirectory; }

private:
	FString CacheDirectory;
	TArray<ANSICHAR> CacheDirectoryUtf8;
};

/** The single policy construction and registration path used by game startup. */
FWoTBootstrapStatus WoTRegisterEOSCachePolicyForStartup(
	IEOSSDKManager* Manager, FStringView DataRoot,
	TUniquePtr<FWoTEOSCachePolicy>& OutPolicy, FDelegateHandle& OutHandle);
#endif
