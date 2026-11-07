// Windows Downloads folder for the installer probe.
#include "../WoTInstallerLocations.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/WinePaths.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include <KnownFolders.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
// Under Wine (Proton on Linux and Steam Deck) the shell's Downloads folder is
// an empty folder inside the Wine prefix; the player's browser saves to the
// Linux home's Downloads instead. Map that through Wine's drive Z:.
FString WoTWineLinuxDownloadsDirectory()
{
	if (!WoTIsRunningUnderWine())
	{
		return FString();
	}
	const FString Unix = WoTWineUnixDownloadsPath(
		FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")));
	FString Mapped = WoTWineDosPathForUnixPath(Unix);
	if (Mapped.IsEmpty() || !IFileManager::Get().DirectoryExists(*Mapped))
	{
		return FString();
	}
	FPaths::NormalizeDirectoryName(Mapped);
	return Mapped;
}
}  // namespace

FString WoTPlatformDownloadsDirectory()
{
	if (FString Linux = WoTWineLinuxDownloadsDirectory(); !Linux.IsEmpty())
	{
		return Linux;
	}

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
