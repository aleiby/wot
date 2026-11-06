#include "../WoTInstallerPicker.h"

#if PLATFORM_MAC

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "Mac/CocoaThread.h"
#include "Mac/MacApplication.h"
#include "Mac/MacSystemIncludes.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#ifndef WOT_BOOTSTRAP_TEST_IDENTITIES
#define WOT_BOOTSTRAP_TEST_IDENTITIES 0
#endif

namespace
{
#if WITH_AUTOMATION_TESTS
FCriticalSection GWoTInstallerPickerMacTestHooksLock;
TFunction<TOptional<FString>()> GWoTInstallerPickerMacShowPanelForTesting;
TFunction<void(bool)> GWoTInstallerPickerMacObserveModalStateForTesting;
#endif

class FWoTMacScopedSystemModalMode final
{
public:
	explicit FWoTMacScopedSystemModalMode(
		TFunction<void(bool)> InObserveModalState = {})
		: ObserveModalState(MoveTemp(InObserveModalState))
	{
	#if WITH_EDITOR
		FCoreDelegates::PreModal.Broadcast();
	#endif
		MacApplication->SystemModalMode(true);
		if (ObserveModalState)
		{
			ObserveModalState(true);
		}
	}
	~FWoTMacScopedSystemModalMode()
	{
		End();
	}

	void End()
	{
		if (!bActive.Exchange(false))
		{
			return;
		}
		MacApplication->SystemModalMode(false);
	#if WITH_EDITOR
		FCoreDelegates::PostModal.Broadcast();
	#endif
		if (ObserveModalState)
		{
			ObserveModalState(false);
		}
	}

private:
	TFunction<void(bool)> ObserveModalState;
	TAtomic<bool> bActive{true};
};
}

#if WITH_AUTOMATION_TESTS
void WoTSetInstallerPickerMacHooksForTesting(
	TFunction<TOptional<FString>()> ShowPanel,
	TFunction<void(bool)> ObserveModalState)
{
	FScopeLock Lock(&GWoTInstallerPickerMacTestHooksLock);
	GWoTInstallerPickerMacShowPanelForTesting = MoveTemp(ShowPanel);
	GWoTInstallerPickerMacObserveModalStateForTesting =
		MoveTemp(ObserveModalState);
}
#endif

void WoTShowInstallerPicker(const FString& DefaultDirectory,
	TFunction<void(const FString&)> OnPicked)
{
	if (!OnPicked)
	{
		return;
	}
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	const FString Override = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_BOOTSTRAP_TEST_PICKED_INSTALLER"));
	if (!Override.IsEmpty())
	{
		OnPicked(Override);
		return;
	}
#endif
	TSharedRef<TFunction<void(const FString&)>, ESPMode::ThreadSafe> Callback =
		MakeShared<TFunction<void(const FString&)>, ESPMode::ThreadSafe>(
			MoveTemp(OnPicked));
	const FString InitialDirectory = DefaultDirectory;
	TFunction<TOptional<FString>()> ShowPanelForTesting;
	TFunction<void(bool)> ObserveModalState;
	#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&GWoTInstallerPickerMacTestHooksLock);
		ShowPanelForTesting = GWoTInstallerPickerMacShowPanelForTesting;
		ObserveModalState = GWoTInstallerPickerMacObserveModalStateForTesting;
	}
	#endif
	TSharedRef<FWoTMacScopedSystemModalMode, ESPMode::ThreadSafe> ModalMode =
		MakeShared<FWoTMacScopedSystemModalMode, ESPMode::ThreadSafe>(
			MoveTemp(ObserveModalState));
	MainThreadCall(^{
		TOptional<FString> PickedPath;
		@autoreleasepool
		{
			if (ShowPanelForTesting)
			{
				PickedPath = ShowPanelForTesting();
			}
			else
			{
				NSOpenPanel* Panel = [NSOpenPanel openPanel];
				[Panel setCanChooseFiles:YES];
				[Panel setCanChooseDirectories:NO];
				[Panel setAllowsMultipleSelection:NO];
				[Panel setCanCreateDirectories:NO];
				[Panel setAllowedContentTypes:@[
					[UTType typeWithFilenameExtension:@"exe"]]];
				if (!InitialDirectory.IsEmpty())
				{
					NSString* Directory = [NSString stringWithUTF8String:
						TCHAR_TO_UTF8(*InitialDirectory)];
					[Panel setDirectoryURL:[NSURL fileURLWithPath:Directory]];
				}
				if ([Panel runModal] == NSModalResponseOK)
				{
					NSURL* Selected = Panel.URL;
					FString Path(UTF8_TO_TCHAR(
						Selected.path.fileSystemRepresentation));
					FPaths::NormalizeFilename(Path);
					PickedPath = MoveTemp(Path);
				}
				[Panel close];
			}
		}
		AsyncTask(ENamedThreads::GameThread,
			[Callback, ModalMode, PickedPath = MoveTemp(PickedPath)]()
			{
				ModalMode->End();
				if (PickedPath.IsSet())
				{
					(*Callback)(PickedPath.GetValue());
				}
			});
	}, false, NSDefaultRunLoopMode);
}

#endif
