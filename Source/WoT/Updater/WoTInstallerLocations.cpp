#include "WoTInstallerLocations.h"

#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/NativeHandles.h"

#ifndef WOT_BOOTSTRAP_TEST_IDENTITIES
#define WOT_BOOTSTRAP_TEST_IDENTITIES 0
#endif

namespace
{
FString CanonicalDownloadsDirectory(FString Path)
{
	if (Path.IsEmpty() || FPaths::IsRelative(Path))
	{
		return FString();
	}
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Opened =
		OpenCanonicalNativeDirectoryNoFollow(Path);
	return Opened.IsError() ? FString()
		: MoveTemp(Opened).GetValue().CanonicalPath;
}
}

FString WoTResolveDownloadsDirectory()
{
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	const FString Override = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR"));
	if (!Override.IsEmpty())
	{
		// An explicit but unusable test directory never falls through to the
		// developer's real Downloads folder.
		return CanonicalDownloadsDirectory(Override);
	}
#endif
	return CanonicalDownloadsDirectory(WoTPlatformDownloadsDirectory());
}

TArray<FString> WoTDownloadsInstallerCandidates(
	const FString& DownloadsDirectory)
{
	TArray<FString> Result;
	if (DownloadsDirectory.IsEmpty())
	{
		return Result;
	}
	Result.Reserve(10);
	Result.Add(FPaths::Combine(
		DownloadsDirectory, WoT::Bootstrap::InstallerName));
	const FString Stem = FPaths::GetBaseFilename(
		WoT::Bootstrap::InstallerName, false);
	const FString Extension = FPaths::GetExtension(
		WoT::Bootstrap::InstallerName, true);
	for (int32 Duplicate = 1; Duplicate <= 9; ++Duplicate)
	{
		Result.Add(FPaths::Combine(DownloadsDirectory,
			FString::Printf(TEXT("%s (%d)%s"),
				*Stem, Duplicate, *Extension)));
	}
	for (FString& Path : Result)
	{
		FPaths::NormalizeFilename(Path);
	}
	return Result;
}
