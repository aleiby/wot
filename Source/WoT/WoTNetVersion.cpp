#include "WoTNetVersion.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Crc.h"
#include "Misc/NetworkVersion.h"
#include "Templates/TypeHash.h"

namespace WoTNetVersion
{
	namespace
	{
		// UE's base computation SHARES the override result's checksum cache
		// (NetworkVersion.cpp:224-260: the cache is consulted before the delegate
		// and the no-override path also writes it). Reading the base through the
		// raw API therefore both returns a stale PINNED value when one is cached
		// and leaves a BASE value cached that later bypasses the override
		// (review-caught pin bypass). Bracketing with invalidation keeps the base
		// pure and leaves the shared cache untouched by our read.
		uint32 GetUncachedBaseVersion()
		{
			FNetworkVersion::InvalidateNetworkChecksum();
			const uint32 Base = FNetworkVersion::GetLocalNetworkVersion(false);
			FNetworkVersion::InvalidateNetworkChecksum();
			return Base;
		}
	}

	uint32 ComputeFor(const FString& ReleaseCommit)
	{
		// Release commits are lowercase hexadecimal. TCHAR CRC over that identical
		// ASCII string, followed by HashCombine's fixed uint32 mix, is stable on
		// every supported platform.
		return HashCombine(
			GetUncachedBaseVersion(),
			FCrc::StrCrc32(*ReleaseCommit));
	}

	FString GetReleaseCommit()
	{
		FString ReleaseCommit;
		if (GConfig)
		{
			GConfig->GetString(
				TEXT("WoTNetVersion"), TEXT("ReleaseCommit"), ReleaseCommit, GGameIni);
		}
		return ReleaseCommit;
	}

	uint32 Current()
	{
		const FString ReleaseCommit = GetReleaseCommit();
		if (ReleaseCommit.IsEmpty())
		{
			return GetUncachedBaseVersion();
		}
		return ComputeFor(ReleaseCommit);
	}

	void Bind()
	{
		FNetworkVersion::GetLocalNetworkVersionOverride.BindStatic(&Current);
		// Make the override effective even if another startup path asked UE for
		// the base checksum before the game module bound its delegate.
		FNetworkVersion::InvalidateNetworkChecksum();
	}
}
