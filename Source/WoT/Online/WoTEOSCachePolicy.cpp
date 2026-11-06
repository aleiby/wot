#include "WoTEOSCachePolicy.h"

#if WOT_WITH_EOS && WITH_EOS_SDK

#include "IEOSSDKManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/ProfileSession.h"

FWoTEOSCachePolicy::FWoTEOSCachePolicy(FStringView HeldCacheDirectory)
	: CacheDirectory(HeldCacheDirectory)
{
	FPaths::NormalizeDirectoryName(CacheDirectory);
	const FTCHARToUTF8 Converted(*CacheDirectory);
	CacheDirectoryUtf8.SetNumUninitialized(Converted.Length() + 1);
	if (Converted.Length() > 0)
	{
		FMemory::Memcpy(CacheDirectoryUtf8.GetData(), Converted.Get(),
			Converted.Length());
	}
	CacheDirectoryUtf8[Converted.Length()] = '\0';
}

void FWoTEOSCachePolicy::Apply(EOS_Platform_Options& Options) const
{
	Options.CacheDirectory = CacheDirectoryUtf8.GetData();
}

FDelegateHandle FWoTEOSCachePolicy::Register(IEOSSDKManager& Manager)
{
	return Manager.OnPreCreatePlatform.AddRaw(
		this, &FWoTEOSCachePolicy::Apply);
}

FWoTBootstrapStatus WoTRegisterEOSCachePolicyForStartup(
	IEOSSDKManager* Manager, FStringView DataRoot,
	TUniquePtr<FWoTEOSCachePolicy>& OutPolicy, FDelegateHandle& OutHandle)
{
	OutHandle.Reset();
	OutPolicy.Reset();
	if (Manager == nullptr)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the EOS SDK manager is unavailable")));
	}
	const FWoTProfileSession* Session = WoTGetProcessProfileSession();
	FString NormalizedRoot(DataRoot);
	FPaths::NormalizeDirectoryName(NormalizedRoot);
	if (Session == nullptr || !Session->IsShared()
		|| Session->DataRoot() != NormalizedRoot)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the EOS cache directory requires the held shared profile session"),
			NormalizedRoot));
	}
	FString CacheDirectory = FPaths::Combine(
		NormalizedRoot, TEXT("Online"), TEXT("EOS"));
	FPaths::NormalizeDirectoryName(CacheDirectory);
	if (!IFileManager::Get().MakeDirectory(*CacheDirectory, true)
		|| !IFileManager::Get().DirectoryExists(*CacheDirectory))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the EOS cache directory could not be created"),
			CacheDirectory));
	}
	TUniquePtr<FWoTEOSCachePolicy> Policy =
		MakeUnique<FWoTEOSCachePolicy>(CacheDirectory);
	FDelegateHandle Handle = Policy->Register(*Manager);
	if (!Handle.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the EOS cache policy delegate could not be registered")));
	}
	OutPolicy = MoveTemp(Policy);
	OutHandle = Handle;
	return FWoTBootstrapStatus::Success();
}

#endif
