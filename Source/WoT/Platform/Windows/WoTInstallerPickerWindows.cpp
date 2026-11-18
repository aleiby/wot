// Windows "Browse…" for the original game's installer: the shell's
// own file-open dialog, modal on the game thread like the engine's dialogs.
#include "../WoTInstallerPicker.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShObjIdl.h>
#include <ShlObj.h>
#include "Windows/COMPointer.h"
#include "Windows/HideWindowsPlatformTypes.h"

// Only test-identity targets define it; MSVC Shipping rejects an undefined
// macro in #if (C4668), which Clang lets through.
#ifndef WOT_BOOTSTRAP_TEST_IDENTITIES
#define WOT_BOOTSTRAP_TEST_IDENTITIES 0
#endif

void WoTShowInstallerPicker(const FString& DefaultDirectory,
	TFunction<void(const FString&)> OnPicked)
{
	check(IsInGameThread());
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	// Test-identity builds only: specs and funnel cells pick without any UI.
	const FString SeamPath = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"));
	if (!SeamPath.IsEmpty())
	{
		OnPicked(SeamPath);
		return;
	}
#endif

	// The file dialog needs a single-threaded apartment; Slate already made the
	// game thread one, so this only balances a reference count.
	if (!FWindowsPlatformMisc::CoInitialize())
	{
		return;
	}
	ON_SCOPE_EXIT
	{
		FWindowsPlatformMisc::CoUninitialize();
	};

	// Declared after the scope guard so it is released before COM is.
	TComPtr<IFileOpenDialog> Dialog;
	if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&Dialog))))
	{
		return;
	}
	FILEOPENDIALOGOPTIONS Options = 0;
	Dialog->GetOptions(&Options);
	// NOCHANGEDIR: without it the dialog moves the process working directory.
	Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST
		| FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
	const COMDLG_FILTERSPEC FileTypes[] = {{L"GOG installer", L"*.exe"}};
	Dialog->SetFileTypes(UE_ARRAY_COUNT(FileTypes), FileTypes);

	if (!DefaultDirectory.IsEmpty())
	{
		FString StartFolder = DefaultDirectory;
		FPaths::MakePlatformFilename(StartFolder);
		TComPtr<IShellItem> Folder;
		if (SUCCEEDED(::SHCreateItemFromParsingName(*StartFolder, nullptr,
				IID_PPV_ARGS(&Folder))))
		{
			Dialog->SetFolder(Folder);
		}
	}

	const void* Owner = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
		: nullptr;
	// Cancel is a failure HRESULT (ERROR_CANCELLED): the player simply keeps
	// waiting, so nothing is reported.
	if (FAILED(Dialog->Show(static_cast<HWND>(const_cast<void*>(Owner)))))
	{
		return;
	}
	TComPtr<IShellItem> Chosen;
	if (FAILED(Dialog->GetResult(&Chosen)))
	{
		return;
	}
	PWSTR RawPath = nullptr;
	if (FAILED(Chosen->GetDisplayName(SIGDN_FILESYSPATH, &RawPath)) || RawPath == nullptr)
	{
		return;
	}
	FString Picked(RawPath);
	::CoTaskMemFree(RawPath);
	FPaths::NormalizeFilename(Picked);
	OnPicked(Picked);
}
