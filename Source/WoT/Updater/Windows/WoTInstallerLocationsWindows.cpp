// Windows Downloads folder for the installer probe.
#include "../WoTInstallerLocations.h"

#include "Misc/Paths.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include <KnownFolders.h>
#include "Windows/HideWindowsPlatformTypes.h"

FString WoTPlatformDownloadsDirectory()
{
	// The user's Downloads folder as the shell records it (relocations included).
	// It reads the registry and does NOT follow USERPROFILE/LOCALAPPDATA
	// overrides, so funnel and spec runs must never reach this: shared code
	// substitutes WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR in test-identity builds and
	// returns empty, never this folder, when that override is unresolvable.
	PWSTR RawPath = nullptr;
	const HRESULT Result = ::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT,
		nullptr, &RawPath);
	FString Path;
	if (SUCCEEDED(Result) && RawPath != nullptr)
	{
		Path = FString(RawPath);
	}
	// Freed whether or not the call succeeded, per the API contract.
	::CoTaskMemFree(RawPath);
	FPaths::NormalizeDirectoryName(Path);
	return Path;
}
