#include "../../WoTMacUninstall.h"

#if PLATFORM_MAC

#include "../../WoTBootstrapSubsystem.h"
#include "../../WoTPayloadSubsystem.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Mac/CocoaThread.h"
#include "Mac/MacSystemIncludes.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTBootstrapCore/TestOverrides.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"
#include "WoTUserDataRoot.h"

#include <cstdlib>

#import <AppKit/AppKit.h>
#if __has_include(<Security/SecTranslocate.h>)
#include <Security/SecTranslocate.h>
#else
extern "C" CFURLRef SecTranslocateCreateOriginalPathForURL(
	CFURLRef TranslocatedPath, CFErrorRef* Error);
#endif

namespace
{
constexpr NSInteger GWoTMacUninstallMenuTag = 0x574f5455;
constexpr int32 GWoTMacUninstallMaximumSizeEntries = 100000;
constexpr double GWoTMacUninstallSizeDeadlineSeconds = 2.0;
constexpr TCHAR GWoTMacUninstallResolutionFailure[] =
	TEXT("Original app resolution failed; nothing was removed.");
constexpr TCHAR GWoTMacUninstallAuthenticationFailure[] =
	TEXT("The game data folder could not be authenticated; nothing was removed.");
constexpr TCHAR GWoTMacUninstallOtherProcessFailure[] =
	TEXT("Quit the other copy of WoT Reborn first");
constexpr TCHAR GWoTMacUninstallShutdownFailure[] =
	TEXT("The running game could not release its data; nothing was removed.");

struct FWoTMacUninstallSize
{
	uint64 Bytes = 0;
	int32 Entries = 0;
	bool bBounded = false;
};

struct FWoTMacUninstallHeldDirectory
{
	FString Path;
	FString Leaf;
	FWoTNativeFileIdentity Identity;
	FWoTNativeDirectoryHandle Parent;
	FWoTNativeDirectoryHandle Directory;
};

struct FWoTMacUninstallWork
{
	FString DataRoot;
	FString RuntimeBundle;
	FString OriginalBundle;
	FString UpdateDirectory;
	FString Answer;
	FString FailureReason;
	TArray<FString> RemovedPaths;
	TArray<FString> RemainingPaths;
	FWoTMacUninstallHeldDirectory HeldDataRoot;
	FWoTMacUninstallHeldDirectory HeldUpdateDirectory;
	TUniquePtr<FWoTProfileSession> Session;
	bool bProfileReadyForFinalRemoval = false;
};

id GWoTMacUninstallMenuObserver = nil;
NSObject* GWoTMacUninstallMenuTarget = nil;
bool GWoTMacUninstallTestCommandStarted = false;
bool GWoTMacUninstallInProgress = false;
FDelegateHandle GWoTMacUninstallTestStartHandle;
TSharedPtr<FWoTMacUninstallWork> GWoTMacUninstallExitHold;
#if WITH_AUTOMATION_TESTS
bool GWoTMacUninstallSuppressExitForTesting = false;
bool GWoTMacUninstallSuppressAlertForTesting = false;
bool GWoTMacUninstallAssumeShutdownReadyForTesting = false;
bool GWoTMacUninstallExitRequestedForTesting = false;
int32 GWoTMacUninstallShutdownCountForTesting = 0;
#endif

void WoTMacUninstallSetMenuEnabled(bool bEnabled)
{
	NSMenu* MainMenu = [NSApp mainMenu];
	if (MainMenu == nil || [MainMenu numberOfItems] == 0)
	{
		return;
	}
	NSMenu* AppMenu = [[MainMenu itemAtIndex:0] submenu];
	for (NSMenuItem* Item in [AppMenu itemArray])
	{
		if ([Item tag] == GWoTMacUninstallMenuTag)
		{
			[Item setEnabled:bEnabled ? YES : NO];
		}
	}
}

FString WoTMacUninstallExpectedDataRoot()
{
	@autoreleasepool
	{
		NSArray<NSString*>* Paths = NSSearchPathForDirectoriesInDomains(
			NSApplicationSupportDirectory, NSUserDomainMask, YES);
		if (Paths == nil || [Paths count] != 1)
		{
			return FString();
		}
		NSString* Support = [Paths objectAtIndex:0];
		FString Root(UTF8_TO_TCHAR(Support.fileSystemRepresentation));
		Root = FPaths::Combine(Root, WoTUserDataRootLeafName());
		FPaths::NormalizeDirectoryName(Root);
		if (Root.IsEmpty() || FPaths::IsRelative(Root)
			|| !FPaths::CollapseRelativeDirectories(Root))
		{
			return FString();
		}
		return Root;
	}
}

bool WoTMacUninstallAuthenticatedRootMatches(
	const FString& Expected, const FString& Authenticated)
{
	return !Expected.IsEmpty() && Expected == Authenticated;
}

bool WoTMacUninstallMakeDirectoryWritable(
	const FWoTNativeDirectoryHandle& Directory)
{
	TWoTBootstrapResult<uint32> Mode = QueryNativeMode(Directory);
	return !Mode.IsError()
		&& (!((Mode.GetValue() & 0300u) != 0300u)
			|| !SetNativeMode(Directory, Mode.GetValue() | 0700u).IsError());
}

bool WoTMacUninstallEmptyDirectory(
	const FWoTNativeDirectoryHandle& Directory)
{
	if (!WoTMacUninstallMakeDirectoryWritable(Directory))
	{
		return false;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Directory);
	if (Listed.IsError())
	{
		return false;
	}
	for (const FWoTNativeDirectoryEntry& Entry : Listed.GetValue())
	{
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
				OpenNativeDirectoryAtNoFollow(Directory, Entry.Name);
			if (Child.IsError())
			{
				return false;
			}
			const FWoTNativeFileIdentity Identity =
				Child.GetValue().GetIdentity();
			if (!WoTMacUninstallEmptyDirectory(Child.GetValue()))
			{
				return false;
			}
			TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
				Child.GetValue().QueryCurrentIdentity();
			if (Current.IsError()
				|| !Current.GetValue().IsSameObject(Identity))
			{
				return false;
			}
			Child.GetValue() = FWoTNativeDirectoryHandle();
			if (RemoveNativeEntryAtNoFollowIfIdentity(
					Directory, Entry.Name, EWoTTreeEntryType::Directory,
					Current.GetValue()).IsError())
			{
				return false;
			}
		}
		else if (RemoveNativeEntryAtNoFollow(
				Directory, Entry.Name, Entry.Type).IsError())
		{
			return false;
		}
	}
	return true;
}

TWoTBootstrapResult<FWoTMacUninstallHeldDirectory>
WoTMacUninstallOpenHeldDirectory(const FString& AbsolutePath)
{
	using FResult =
		TWoTBootstrapResult<FWoTMacUninstallHeldDirectory>;
	FString ParentPath;
	FString Leaf;
	FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
		AbsolutePath, ParentPath, Leaf);
	if (Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Parent =
		OpenNativeDirectoryPathNoFollow(ParentPath);
	if (Parent.IsError())
	{
		return FResult::Error(MoveTemp(Parent).GetError());
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Type =
		QueryNativeEntryTypeAtNoFollow(Parent.GetValue(), Leaf);
	if (Type.IsError() || !Type.GetValue().IsSet()
		|| Type.GetValue().GetValue() != EWoTTreeEntryType::Directory)
	{
		return FResult::Error(Type.IsError()
			? MoveTemp(Type).GetError()
			: MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the uninstall target is not a no-follow directory"),
				AbsolutePath));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryAtNoFollow(Parent.GetValue(), Leaf);
	if (Root.IsError())
	{
		return FResult::Error(MoveTemp(Root).GetError());
	}
	FWoTMacUninstallHeldDirectory Held;
	Held.Path = AbsolutePath;
	Held.Leaf = MoveTemp(Leaf);
	Held.Identity = Root.GetValue().GetIdentity();
	Held.Parent = MoveTemp(Parent).GetValue();
	Held.Directory = MoveTemp(Root).GetValue();
	return FResult::Success(MoveTemp(Held));
}

bool WoTMacUninstallRemoveHeldDirectory(
	FWoTMacUninstallHeldDirectory& Held)
{
	if (!Held.Parent.IsValid() || !Held.Directory.IsValid()
		|| !WoTMacUninstallEmptyDirectory(Held.Directory))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
		Held.Directory.QueryCurrentIdentity();
	if (Current.IsError()
		|| !Current.GetValue().IsSameObject(Held.Identity))
	{
		return false;
	}
	Held.Directory = FWoTNativeDirectoryHandle();
	return !RemoveNativeEntryAtNoFollowIfIdentity(
		Held.Parent, Held.Leaf, EWoTTreeEntryType::Directory,
		Current.GetValue()).IsError();
}

void WoTMacUninstallMeasureDirectory(
	const FWoTNativeDirectoryHandle& Directory, double Deadline,
	FWoTMacUninstallSize& Size)
{
	if (Size.bBounded || FPlatformTime::Seconds() >= Deadline
		|| Size.Entries >= GWoTMacUninstallMaximumSizeEntries)
	{
		Size.bBounded = true;
		return;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Directory);
	if (Listed.IsError())
	{
		Size.bBounded = true;
		return;
	}
	for (const FWoTNativeDirectoryEntry& Entry : Listed.GetValue())
	{
		if (++Size.Entries > GWoTMacUninstallMaximumSizeEntries
			|| FPlatformTime::Seconds() >= Deadline)
		{
			Size.bBounded = true;
			return;
		}
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
				OpenNativeDirectoryAtNoFollow(Directory, Entry.Name);
			if (Child.IsError())
			{
				Size.bBounded = true;
				continue;
			}
			WoTMacUninstallMeasureDirectory(
				Child.GetValue(), Deadline, Size);
		}
		else if (Entry.Type == EWoTTreeEntryType::File)
		{
			TWoTBootstrapResult<FWoTNativeFileHandle> File =
				OpenNativeFileAtReadNoFollow(Directory, Entry.Name);
			if (File.IsError())
			{
				Size.bBounded = true;
				continue;
			}
			Size.Bytes += File.GetValue().GetIdentity().Size;
		}
	}
}

FWoTMacUninstallSize WoTMacUninstallMeasureRoot(const FString& RootPath)
{
	FWoTMacUninstallSize Size;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryPathNoFollow(RootPath);
	if (Root.IsError())
	{
		Size.bBounded = true;
		return Size;
	}
	WoTMacUninstallMeasureDirectory(Root.GetValue(),
		FPlatformTime::Seconds() + GWoTMacUninstallSizeDeadlineSeconds, Size);
	return Size;
}

FString WoTMacUninstallAbbreviateHome(const FString& Path)
{
	FString Home = FPlatformProcess::UserHomeDir();
	FPaths::NormalizeDirectoryName(Home);
	if (Path == Home)
	{
		return TEXT("~");
	}
	if (Path.StartsWith(Home + TEXT("/"), ESearchCase::CaseSensitive))
	{
		return TEXT("~") + Path.RightChop(Home.Len());
	}
	return Path;
}

FString WoTMacUninstallResolveOriginalBundle(const FString& RuntimeBundle)
{
	FString Normalized = RuntimeBundle;
	FPaths::NormalizeDirectoryName(Normalized);
	if (!WoTIsRunningTreeTranslocated(Normalized))
	{
		return Normalized;
	}
	@autoreleasepool
	{
		NSString* Path = [NSString stringWithUTF8String:TCHAR_TO_UTF8(*Normalized)];
		NSURL* URL = Path != nil
			? [NSURL fileURLWithPath:Path isDirectory:YES] : nil;
		if (URL == nil)
		{
			return FString();
		}
		CFErrorRef Error = nullptr;
		CFURLRef Original = SecTranslocateCreateOriginalPathForURL(
			(CFURLRef)URL, &Error);
		if (Error != nullptr)
		{
			CFRelease(Error);
		}
		if (Original == nullptr)
		{
			return FString();
		}
		NSURL* OriginalURL = (NSURL*)Original;
		FString Result(UTF8_TO_TCHAR(
			OriginalURL.path.fileSystemRepresentation));
		CFRelease(Original);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}
}

bool WoTMacUninstallCanPrepareShutdown()
{
#if WITH_AUTOMATION_TESTS
	// Headless editor automation has no game instance carrying the Bootstrap and
	// Payload subsystems; the production-refusal spec drives the drain path itself.
	if (GWoTMacUninstallAssumeShutdownReadyForTesting)
	{
		return true;
	}
#endif
	if (GEngine == nullptr)
	{
		return false;
	}
	bool bFoundGameInstance = false;
	bool bFoundBootstrap = false;
	bool bFoundPayload = false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UGameInstance* Instance = Context.OwningGameInstance;
		if (Instance == nullptr)
		{
			continue;
		}
		bFoundGameInstance = true;
		if (UWoTBootstrapSubsystem* Bootstrap =
			Instance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			bFoundBootstrap = true;
		}
		if (UWoTPayloadSubsystem* Payload =
			Instance->GetSubsystem<UWoTPayloadSubsystem>())
		{
			bFoundPayload = true;
		}
	}
	return bFoundGameInstance && bFoundBootstrap && bFoundPayload;
}

void WoTMacUninstallPrepareShutdown()
{
#if WITH_AUTOMATION_TESTS
	++GWoTMacUninstallShutdownCountForTesting;
#endif
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UGameInstance* Instance = Context.OwningGameInstance;
		if (Instance == nullptr)
		{
			continue;
		}
		if (UWoTBootstrapSubsystem* Bootstrap =
			Instance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			Bootstrap->ShutdownForUninstall();
		}
		if (UWoTPayloadSubsystem* Payload =
			Instance->GetSubsystem<UWoTPayloadSubsystem>())
		{
			Payload->ShutdownForUninstall();
		}
	}
}

void WoTMacUninstallAddRemaining(TArray<FString>& Paths,
	const FString& Path)
{
	if (!Path.IsEmpty())
	{
		Paths.AddUnique(Path);
	}
}

void WoTMacUninstallMarkAllRemaining(FWoTMacUninstallWork& Work)
{
	WoTMacUninstallAddRemaining(Work.RemainingPaths, Work.DataRoot);
	WoTMacUninstallAddRemaining(Work.RemainingPaths, Work.UpdateDirectory);
	WoTMacUninstallAddRemaining(Work.RemainingPaths,
		Work.OriginalBundle.IsEmpty()
			? Work.RuntimeBundle : Work.OriginalBundle);
}

void WoTMacUninstallPreflightFailure(
	FWoTMacUninstallWork& Work, FString Reason)
{
	Work.FailureReason = MoveTemp(Reason);
	WoTMacUninstallMarkAllRemaining(Work);
}

bool WoTMacUninstallPreflight(
	FWoTMacUninstallWork& Work, const FString& AuthenticatedDataRoot,
	TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle,
	TFunction<bool()> CanPrepareShutdown)
{
	TOptional<FString> Original = ResolveOriginalBundle(Work.RuntimeBundle);
	if (!Original.IsSet() || Original.GetValue().IsEmpty())
	{
		WoTMacUninstallPreflightFailure(
			Work, GWoTMacUninstallResolutionFailure);
		return false;
	}
	Work.OriginalBundle = Original.GetValue();
	FPaths::NormalizeDirectoryName(Work.OriginalBundle);
	Work.UpdateDirectory = FPaths::Combine(
		FPaths::GetPath(Work.OriginalBundle), TEXT(".wot-update"));

	if (!WoTMacUninstallAuthenticatedRootMatches(
			Work.DataRoot, AuthenticatedDataRoot))
	{
		WoTMacUninstallPreflightFailure(
			Work, GWoTMacUninstallAuthenticationFailure);
		return false;
	}
	TWoTBootstrapResult<FWoTMacUninstallHeldDirectory> HeldData =
		WoTMacUninstallOpenHeldDirectory(Work.DataRoot);
	if (HeldData.IsError())
	{
		WoTMacUninstallPreflightFailure(Work,
			FString(GWoTMacUninstallAuthenticationFailure) + TEXT(" ")
			+ HeldData.GetError().ToString());
		return false;
	}
	Work.HeldDataRoot = MoveTemp(HeldData).GetValue();
	if (!Work.Session.IsValid() || !Work.Session->IsShared()
		|| Work.Session->DataRoot() != Work.DataRoot
		|| !Work.Session->DataRootIdentity().IsSameObject(
			Work.HeldDataRoot.Identity))
	{
		WoTMacUninstallPreflightFailure(Work,
			TEXT("The game data session gate is unavailable or does not match the authenticated profile; nothing was removed."));
		return false;
	}
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Root = ResolveUpdaterStateRoot(
		Work.DataRoot, FPaths::GetPath(Work.OriginalBundle),
		FPaths::GetCleanFilename(Work.OriginalBundle));
	if (Root.IsError())
	{
		WoTMacUninstallPreflightFailure(Work,
			TEXT("The updater workspace path is unsafe; nothing was removed. ")
			+ Root.GetError().ToString());
		return false;
	}
	Work.UpdateDirectory = Root.GetValue().UpdateDir;
	bool bCreated = false;
	auto Update = OpenOrCreateCanonicalNativeDirectoryNoFollow(Work.UpdateDirectory, bCreated);
	if (Update.IsError())
	{
		WoTMacUninstallPreflightFailure(Work, TEXT("The updater workspace is unsafe; nothing was removed."));
		return false;
	}
	TWoTBootstrapResult<FWoTMacUninstallHeldDirectory> HeldUpdate =
		WoTMacUninstallOpenHeldDirectory(Work.UpdateDirectory);
	if (HeldUpdate.IsError())
	{
		WoTMacUninstallPreflightFailure(Work,
			HeldUpdate.IsError()
				? TEXT("The locked updater workspace could not be reopened; nothing was removed. ")
					+ HeldUpdate.GetError().ToString()
				: TEXT("The updater workspace name changed while it was locked; nothing was removed."));
		return false;
	}
	Work.HeldUpdateDirectory = MoveTemp(HeldUpdate).GetValue();
	if (!CanPrepareShutdown())
	{
		WoTMacUninstallPreflightFailure(
			Work, GWoTMacUninstallShutdownFailure);
		return false;
	}
	return true;
}

void WoTMacUninstallDeleteState(FWoTMacUninstallWork& Work)
{
	// Shutdown released every local snapshot before EXCLUSIVE was attempted.
	// Delete ContentCache with the rest of Data; a second cache lock no longer
	// exists and would only self-contend with this profile owner.
	FWoTBootstrapStatus ProfileDeleted = Work.Session.IsValid()
		? Work.Session->DeleteProfileContentsExceptSessionLock()
		: FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the exclusive profile session owner is unavailable"),
			Work.DataRoot));
	if (!ProfileDeleted.IsError())
	{
		// The root-level held lock remains until the bundle has been recycled.
		// Final unlink happens immediately before exit.
		Work.bProfileReadyForFinalRemoval = true;
		Work.RemovedPaths.Add(Work.DataRoot);
	}
	else
	{
		Work.RemainingPaths.Add(Work.DataRoot);
		Work.FailureReason = TEXT("Uninstall incomplete: ")
			+ ProfileDeleted.GetError().ToString();
	}
	if (WoTMacUninstallRemoveHeldDirectory(Work.HeldUpdateDirectory))
	{
		Work.RemovedPaths.Add(Work.UpdateDirectory);
	}
	else
	{
		Work.RemainingPaths.Add(Work.UpdateDirectory);
	}
}

void WoTMacUninstallWriteTestResult(const FWoTMacUninstallWork& Work)
{
#if WOT_BOOTSTRAP_TEST_IDENTITIES
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("wot-uninstall-result-v1"));
	Object->SetStringField(TEXT("answer"), Work.Answer);
	Object->SetStringField(TEXT("failure_reason"), Work.FailureReason);
	auto ToJson = [](const TArray<FString>& Paths)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Path : Paths)
		{
			Values.Add(MakeShared<FJsonValueString>(Path));
		}
		return Values;
	};
	Object->SetArrayField(TEXT("removed_paths"), ToJson(Work.RemovedPaths));
	Object->SetArrayField(TEXT("remaining_paths"), ToJson(Work.RemainingPaths));
	FString Text;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (FJsonSerializer::Serialize(Object, Writer))
	{
		const FString ResultPath = FPaths::Combine(
			FPaths::GetPath(Work.DataRoot), TEXT("wot-uninstall-result.json"));
		(void)FFileHelper::SaveStringToFile(Text, *ResultPath);
	}
#endif
}

void WoTMacUninstallShowResult(const FWoTMacUninstallWork& Work)
{
	WoTMacUninstallWriteTestResult(Work);
	if (!Work.RemainingPaths.IsEmpty())
	{
	#if WITH_AUTOMATION_TESTS
		if (GWoTMacUninstallSuppressAlertForTesting)
		{
			return;
		}
	#endif
	#if !WOT_BOOTSTRAP_TEST_IDENTITIES
		@autoreleasepool
		{
			NSAlert* Alert = [[NSAlert alloc] init];
			[Alert setAlertStyle:NSAlertStyleWarning];
			const FString Message = Work.FailureReason.IsEmpty()
				? TEXT("Uninstall incomplete") : Work.FailureReason;
			[Alert setMessageText:[NSString stringWithUTF8String:
				TCHAR_TO_UTF8(*Message)]];
			FString Paths = FString::Join(Work.RemainingPaths, TEXT("\n"));
			[Alert setInformativeText:[NSString stringWithUTF8String:
				TCHAR_TO_UTF8(*Paths)]];
			[Alert addButtonWithTitle:@"OK"];
			[Alert runModal];
		#if !__has_feature(objc_arc)
			[Alert release];
		#endif
		}
	#endif
	}
}

void WoTMacUninstallRequestExit()
{
#if WITH_AUTOMATION_TESTS
	if (GWoTMacUninstallSuppressExitForTesting)
	{
		GWoTMacUninstallExitRequestedForTesting = true;
		return;
	}
#endif
	// The committed path may already have unlinked Data/session.lock. Bypass
	// ordinary engine teardown so no late cache, preferences, EOS, or quit-swap
	// work can recreate or write below the removed profile root.
	std::_Exit(EXIT_SUCCESS);
}

void WoTMacUninstallFinishCommitted(TSharedRef<FWoTMacUninstallWork> Work)
{
	WoTMacUninstallShowResult(*Work);
	if (Work->bProfileReadyForFinalRemoval && Work->Session.IsValid())
	{
		Work->HeldDataRoot = FWoTMacUninstallHeldDirectory();
		const FWoTBootstrapStatus FinalRemoval =
			Work->Session->DeleteSessionLockAndEmptyProfileRoot();
		if (FinalRemoval.IsError())
		{
			FPlatformMisc::LowLevelOutputDebugStringf(
				TEXT("WoT uninstall final profile removal failed: %s\n"),
				*FinalRemoval.GetError().ToString());
		}
	}
	// Keep the accepted session.lock-unlink-to-exit window to this immediate call.
	WoTMacUninstallRequestExit();
}

void WoTMacUninstallReleaseRefusedWork(FWoTMacUninstallWork& Work)
{
	Work.HeldUpdateDirectory = FWoTMacUninstallHeldDirectory();
	Work.HeldDataRoot = FWoTMacUninstallHeldDirectory();
	if (Work.Session.IsValid() && Work.Session->IsExclusive())
	{
		FWoTBootstrapStatus Shared = Work.Session->RestoreShared();
		if (Shared.IsError())
		{
			Work.FailureReason +=
				TEXT(" The game data session gate could not be restored: ")
				+ Shared.GetError().ToString();
		}
	}
}

void WoTMacUninstallFinishRefused(TSharedRef<FWoTMacUninstallWork> Work)
{
	WoTMacUninstallReleaseRefusedWork(*Work);
	WoTMacUninstallShowResult(*Work);
	const bool bRestored = WoTRestoreProcessProfileSession(
		MoveTemp(Work->Session));
	if (!bRestored)
	{
		FPlatformMisc::LowLevelOutputDebugString(
			TEXT("WoT uninstall refusal could not restore the shared profile session gate\n"));
		std::_Exit(EXIT_FAILURE);
	}
	GWoTMacUninstallInProgress = false;
	WoTMacUninstallSetMenuEnabled(true);
}

void WoTMacUninstallFinishDrainedRefusal(
	TSharedRef<FWoTMacUninstallWork> Work)
{
	// The game has already shut down its workers, EOS platform and payload
	// snapshots. Keep the restored SHARED hold only until this immediate exit;
	// resuming a half-drained game would be less safe than asking for a retry.
	WoTMacUninstallShowResult(*Work);
	WoTMacUninstallRequestExit();
}

void WoTMacUninstallRecycleOriginal(
	TSharedRef<FWoTMacUninstallWork> Work)
{
	@autoreleasepool
	{
		NSString* Path = [NSString stringWithUTF8String:
			TCHAR_TO_UTF8(*Work->OriginalBundle)];
		NSURL* URL = Path != nil
			? [NSURL fileURLWithPath:Path isDirectory:YES] : nil;
		if (URL == nil)
		{
			Work->RemainingPaths.Add(Work->OriginalBundle);
			WoTMacUninstallFinishCommitted(Work);
			return;
		}
		[[NSWorkspace sharedWorkspace] recycleURLs:@[URL]
			completionHandler:^(NSDictionary<NSURL*, NSURL*>*, NSError* Error)
			{
				if (Error == nil)
				{
					Work->RemovedPaths.Add(Work->OriginalBundle);
				}
				else
				{
					Work->RemainingPaths.Add(Work->OriginalBundle);
				}
				WoTMacUninstallFinishCommitted(Work);
			}];
	}
}

void WoTMacUninstallRunConfirmedFlow(
	TSharedRef<FWoTMacUninstallWork> Work,
	const FString& AuthenticatedDataRoot,
	TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle,
	bool bSynchronousRefusal)
{
	Work->Session = WoTTakeProcessProfileSession();
	if (!WoTMacUninstallPreflight(*Work, AuthenticatedDataRoot,
			MoveTemp(ResolveOriginalBundle),
			[]() { return WoTMacUninstallCanPrepareShutdown(); }))
	{
		if (bSynchronousRefusal)
		{
			MainThreadCall(^{ WoTMacUninstallFinishRefused(Work); },
				true, NSDefaultRunLoopMode);
		}
		else
		{
			MainThreadCall(^{ WoTMacUninstallFinishRefused(Work); },
				false, NSDefaultRunLoopMode);
		}
		return;
	}

	// Authentication and every path check are complete. Drain local users before
	// dropping SHARED, then try EXCLUSIVE without waiting. A contended uninstall
	// removes nothing and exits the already-drained game.
	WoTMacUninstallPrepareShutdown();
	TWoTBootstrapResult<bool> Exclusive = Work->Session.IsValid()
		? Work->Session->TryAcquireExclusive()
		: TWoTBootstrapResult<bool>::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the process profile session is unavailable"), Work->DataRoot));
	if (Exclusive.IsError() || !Exclusive.GetValue())
	{
		WoTMacUninstallPreflightFailure(*Work,
			Exclusive.IsError()
				? TEXT("The game data session could not be acquired safely; nothing was removed. ")
					+ Exclusive.GetError().ToString()
				: FString(GWoTMacUninstallOtherProcessFailure));
		if (bSynchronousRefusal)
		{
			MainThreadCall(^{ WoTMacUninstallFinishDrainedRefusal(Work); },
				true, NSDefaultRunLoopMode);
		}
		else
		{
			MainThreadCall(^{ WoTMacUninstallFinishDrainedRefusal(Work); },
				false, NSDefaultRunLoopMode);
		}
		return;
	}

	// EXCLUSIVE is the irreversible commit point. Deletion/recycling failures
	// are partial results and the process always exits.
	GWoTMacUninstallExitHold = Work;
	Async(EAsyncExecution::ThreadPool, [Work]()
	{
		WoTMacUninstallDeleteState(*Work);
		MainThreadCall(^{
			if (Work->RemainingPaths.Contains(Work->OriginalBundle))
			{
				WoTMacUninstallFinishCommitted(Work);
			}
			else
			{
				WoTMacUninstallRecycleOriginal(Work);
			}
		}, false, NSDefaultRunLoopMode);
	});
}

void WoTMacUninstallConfirm(const FString& ExpectedDataRoot,
	const FString& AuthenticatedDataRoot, const FString& RuntimeBundle)
{
	AsyncTask(ENamedThreads::GameThread,
		[ExpectedDataRoot, AuthenticatedDataRoot, RuntimeBundle]()
	{
		TSharedRef<FWoTMacUninstallWork> Work =
			MakeShared<FWoTMacUninstallWork>();
		Work->DataRoot = ExpectedDataRoot;
		Work->RuntimeBundle = RuntimeBundle;
		Work->Answer = TEXT("confirm");
			WoTMacUninstallRunConfirmedFlow(Work, AuthenticatedDataRoot,
			[](const FString& Bundle) -> TOptional<FString>
			{
				FString Original = WoTMacUninstallResolveOriginalBundle(Bundle);
				return Original.IsEmpty()
					? TOptional<FString>()
					: TOptional<FString>(MoveTemp(Original));
			},
				false);
	});
}

FString WoTMacUninstallRuntimeBundle()
{
	@autoreleasepool
	{
		NSString* Path = [[NSBundle mainBundle] bundlePath];
		FString Result(Path != nil
			? UTF8_TO_TCHAR(Path.fileSystemRepresentation) : TEXT(""));
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}
}

void WoTMacUninstallBegin(TOptional<bool> TestAnswer)
{
	if (GWoTMacUninstallInProgress)
	{
		return;
	}
	GWoTMacUninstallInProgress = true;
	WoTMacUninstallSetMenuEnabled(false);
	const FString ExpectedDataRoot = WoTMacUninstallExpectedDataRoot();
	const FString AuthenticatedDataRoot = WoTUserDataRoot();
	const FString RuntimeBundle = WoTMacUninstallRuntimeBundle();
	Async(EAsyncExecution::ThreadPool,
			[ExpectedDataRoot, AuthenticatedDataRoot, RuntimeBundle, TestAnswer]()
		{
			const FWoTMacUninstallSize Size =
					WoTMacUninstallMeasureRoot(ExpectedDataRoot);
			MainThreadCall(^{
				bool bConfirm = TestAnswer.Get(false);
				if (!TestAnswer.IsSet())
				{
					@autoreleasepool
					{
						NSByteCountFormatter* Formatter =
							[[NSByteCountFormatter alloc] init];
						NSString* ByteText = [Formatter stringFromByteCount:
							static_cast<long long>(Size.Bytes)];
						FString DisplaySize(UTF8_TO_TCHAR(
							ByteText.UTF8String));
						if (Size.bBounded)
						{
							DisplaySize = TEXT("at least ") + DisplaySize;
						}
						const FString Info = FString::Printf(
							TEXT("This removes the game's data (%s) from %s and moves WoT Reborn to the Trash. Your GOG installer download is not touched."),
							*DisplaySize,
								*WoTMacUninstallAbbreviateHome(ExpectedDataRoot));
						NSAlert* Alert = [[NSAlert alloc] init];
						[Alert setAlertStyle:NSAlertStyleWarning];
						[Alert setMessageText:@"Uninstall WoT Reborn?"];
						[Alert setInformativeText:[NSString stringWithUTF8String:
							TCHAR_TO_UTF8(*Info)]];
						NSButton* Uninstall = [Alert addButtonWithTitle:@"Uninstall"];
						NSButton* Cancel = [Alert addButtonWithTitle:@"Cancel"];
						[Uninstall setHasDestructiveAction:YES];
						[Uninstall setKeyEquivalent:@""];
						[Cancel setKeyEquivalent:@"\r"];
						bConfirm = [Alert runModal] == NSAlertFirstButtonReturn;
					#if !__has_feature(objc_arc)
						[Alert release];
						[Formatter release];
					#endif
					}
				}
				if (bConfirm)
				{
						WoTMacUninstallConfirm(ExpectedDataRoot,
							AuthenticatedDataRoot, RuntimeBundle);
				}
					else
				{
						if (TestAnswer.IsSet())
						{
							FWoTMacUninstallWork Work;
							Work.DataRoot = ExpectedDataRoot;
							Work.RuntimeBundle = RuntimeBundle;
							Work.OriginalBundle = WoTMacUninstallResolveOriginalBundle(
								RuntimeBundle);
							Work.UpdateDirectory = FPaths::Combine(
								FPaths::GetPath(Work.OriginalBundle), TEXT(".wot-update"));
							Work.Answer = TEXT("cancel");
							WoTMacUninstallMarkAllRemaining(Work);
							WoTMacUninstallWriteTestResult(Work);
						}
						GWoTMacUninstallInProgress = false;
						WoTMacUninstallSetMenuEnabled(true);
				}
			}, false, NSDefaultRunLoopMode);
		});
}

void WoTMacUninstallEnsureMenuItem()
{
	@autoreleasepool
	{
		NSMenu* MainMenu = [NSApp mainMenu];
		if (MainMenu == nil || [MainMenu numberOfItems] == 0)
		{
			return;
		}
		NSMenu* AppMenu = [[MainMenu itemAtIndex:0] submenu];
		if (AppMenu == nil)
		{
			return;
		}
		NSInteger Found = 0;
		for (NSInteger Index = [AppMenu numberOfItems] - 1; Index >= 0; --Index)
		{
			if ([[AppMenu itemAtIndex:Index] tag] == GWoTMacUninstallMenuTag)
			{
				++Found;
				if (Found > 1)
				{
					[AppMenu removeItemAtIndex:Index];
				}
			}
		}
		if (Found != 0)
		{
			return;
		}
		NSInteger QuitIndex = [AppMenu numberOfItems];
		for (NSInteger Index = [AppMenu numberOfItems] - 1; Index >= 0; --Index)
		{
			NSMenuItem* Candidate = [AppMenu itemAtIndex:Index];
			if ([Candidate action] == @selector(terminate:))
			{
				QuitIndex = Index;
				break;
			}
		}
		NSMenuItem* Item = [[NSMenuItem alloc]
			initWithTitle:@"Uninstall WoT Reborn…"
			action:@selector(wotUninstall:) keyEquivalent:@""];
		[Item setTag:GWoTMacUninstallMenuTag];
		[Item setTarget:GWoTMacUninstallMenuTarget];
		[Item setEnabled:GWoTMacUninstallInProgress ? NO : YES];
		[AppMenu insertItem:Item atIndex:QuitIndex];
	#if !__has_feature(objc_arc)
		[Item release];
	#endif
	}
}
}

@interface FWoTMacUninstallMenuTarget : NSObject
- (void)wotUninstall:(id)sender;
@end

@implementation FWoTMacUninstallMenuTarget
- (void)wotUninstall:(id)sender
{
	(void)sender;
	WoTMacUninstallBegin({});
}
@end

void WoTInstallMacUninstallMenuItem()
{
	#if WOT_BOOTSTRAP_TEST_IDENTITIES
	if (!GWoTMacUninstallTestCommandStarted)
	{
		FString Answer;
		if (FParse::Value(FCommandLine::Get(),
				TEXT("wottestuninstall="), Answer)
			&& (Answer == TEXT("confirm") || Answer == TEXT("cancel")))
		{
			GWoTMacUninstallTestCommandStarted = true;
			const bool bConfirm = Answer == TEXT("confirm");
			GWoTMacUninstallTestStartHandle =
				FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([bConfirm]()
				{
					MainThreadCall(^{ WoTMacUninstallBegin(bConfirm); },
						false, NSDefaultRunLoopMode);
				});
		}
	}
	#endif
	MainThreadCall(^{
		if (GWoTMacUninstallMenuTarget == nil)
		{
			GWoTMacUninstallMenuTarget =
				[[FWoTMacUninstallMenuTarget alloc] init];
		}
		WoTMacUninstallEnsureMenuItem();
		if (GWoTMacUninstallMenuObserver == nil)
		{
			GWoTMacUninstallMenuObserver = [[NSNotificationCenter defaultCenter]
				addObserverForName:NSApplicationDidUpdateNotification
				object:NSApp queue:[NSOperationQueue mainQueue]
				usingBlock:^(NSNotification*)
				{
					WoTMacUninstallEnsureMenuItem();
				}];
		}
	}, false, NSDefaultRunLoopMode);
}

void WoTShutdownMacUninstallMenuItem()
{
	#if WOT_BOOTSTRAP_TEST_IDENTITIES
	if (GWoTMacUninstallTestStartHandle.IsValid())
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(
			GWoTMacUninstallTestStartHandle);
		GWoTMacUninstallTestStartHandle.Reset();
	}
	#endif
	MainThreadCall(^{
		if (GWoTMacUninstallMenuObserver != nil)
		{
			[[NSNotificationCenter defaultCenter]
				removeObserver:GWoTMacUninstallMenuObserver];
			GWoTMacUninstallMenuObserver = nil;
		}
	}, false, NSDefaultRunLoopMode);
}

#if WITH_AUTOMATION_TESTS
FWoTMacUninstallTestResult WoTRunMacUninstallFilesystemForTesting(
	bool bConfirm, const FString& ExpectedDataRoot,
	const FString& AuthenticatedDataRoot, const FString& RuntimeBundle,
	TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle,
	TFunction<bool(const FString&)> RecycleOriginalBundle)
{
	FWoTMacUninstallTestResult Result;
	if (!bConfirm)
	{
		return Result;
	}
	FWoTMacUninstallWork Work;
	Work.DataRoot = ExpectedDataRoot;
	Work.RuntimeBundle = RuntimeBundle;
	Work.Answer = TEXT("confirm");
	TWoTBootstrapResult<FWoTProfileSession> Session =
		AcquireWoTProfileSessionShared(ExpectedDataRoot);
	if (Session.IsError())
	{
		WoTMacUninstallPreflightFailure(Work,
			TEXT("The game data session gate could not be acquired for the uninstall fixture; nothing was removed. ")
			+ Session.GetError().ToString());
		Result.FailureReason = MoveTemp(Work.FailureReason);
		Result.RemainingPaths = MoveTemp(Work.RemainingPaths);
		return Result;
	}
	Work.Session = MakeUnique<FWoTProfileSession>(
		MoveTemp(Session).GetValue());
	if (!WoTMacUninstallPreflight(Work, AuthenticatedDataRoot,
			MoveTemp(ResolveOriginalBundle),
			[]() { return true; }))
	{
		WoTMacUninstallReleaseRefusedWork(Work);
		Result.FailureReason = MoveTemp(Work.FailureReason);
		Result.RemainingPaths = MoveTemp(Work.RemainingPaths);
		return Result;
	}
	TWoTBootstrapResult<bool> Exclusive = Work.Session->TryAcquireExclusive();
	if (Exclusive.IsError() || !Exclusive.GetValue())
	{
		WoTMacUninstallPreflightFailure(Work,
			Exclusive.IsError()
				? TEXT("The game data session could not be acquired safely; nothing was removed. ")
					+ Exclusive.GetError().ToString()
				: FString(GWoTMacUninstallOtherProcessFailure));
		WoTMacUninstallReleaseRefusedWork(Work);
		Result.FailureReason = MoveTemp(Work.FailureReason);
		Result.RemainingPaths = MoveTemp(Work.RemainingPaths);
		return Result;
	}
	WoTMacUninstallDeleteState(Work);
	if (RecycleOriginalBundle(Work.OriginalBundle))
	{
		Work.RemovedPaths.Add(Work.OriginalBundle);
	}
	else
	{
		Work.RemainingPaths.Add(Work.OriginalBundle);
	}
	if (Work.bProfileReadyForFinalRemoval && Work.Session.IsValid())
	{
		Work.HeldDataRoot = FWoTMacUninstallHeldDirectory();
		FWoTBootstrapStatus FinalRemoval =
			Work.Session->DeleteSessionLockAndEmptyProfileRoot();
		if (FinalRemoval.IsError())
		{
			Work.RemovedPaths.Remove(Work.DataRoot);
			Work.RemainingPaths.AddUnique(Work.DataRoot);
			Work.FailureReason = TEXT("Uninstall incomplete: ")
				+ FinalRemoval.GetError().ToString();
		}
	}
	Result.RemovedPaths = MoveTemp(Work.RemovedPaths);
	Result.RemainingPaths = MoveTemp(Work.RemainingPaths);
	Result.FailureReason = MoveTemp(Work.FailureReason);
	return Result;
}

bool WoTMacUninstallMenuEnabledForTesting()
{
	__block bool bEnabled = false;
	MainThreadCall(^{
		NSMenu* MainMenu = [NSApp mainMenu];
		if (MainMenu == nil || [MainMenu numberOfItems] == 0)
		{
			return;
		}
		NSMenu* AppMenu = [[MainMenu itemAtIndex:0] submenu];
		for (NSMenuItem* Item in [AppMenu itemArray])
		{
			if ([Item tag] == GWoTMacUninstallMenuTag)
			{
				bEnabled = [Item isEnabled] == YES;
				return;
			}
		}
	}, true, NSDefaultRunLoopMode);
	return bEnabled;
}

FWoTMacUninstallProductionFlowTestResult
WoTRunMacUninstallProductionRefusalForTesting(
	const FString& ExpectedDataRoot,
	const FString& AuthenticatedDataRoot,
	const FString& RuntimeBundle,
	TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle)
{
	FWoTMacUninstallProductionFlowTestResult Result;
	GWoTMacUninstallExitRequestedForTesting = false;
	GWoTMacUninstallSuppressExitForTesting = true;
	GWoTMacUninstallSuppressAlertForTesting = true;
	GWoTMacUninstallAssumeShutdownReadyForTesting = true;
	const int32 ShutdownCountBefore = GWoTMacUninstallShutdownCountForTesting;
	GWoTMacUninstallInProgress = true;
	MainThreadCall(^{ WoTMacUninstallSetMenuEnabled(false); },
		true, NSDefaultRunLoopMode);

	TSharedRef<FWoTMacUninstallWork> Work =
		MakeShared<FWoTMacUninstallWork>();
	Work->DataRoot = ExpectedDataRoot;
	Work->RuntimeBundle = RuntimeBundle;
	Work->Answer = TEXT("confirm");
	WoTMacUninstallRunConfirmedFlow(Work, AuthenticatedDataRoot,
		MoveTemp(ResolveOriginalBundle), true);

	Result.FailureReason = Work->FailureReason;
	Result.bInProgress = GWoTMacUninstallInProgress;
	Result.bMenuEnabled = WoTMacUninstallMenuEnabledForTesting();
	const FWoTProfileSession* ProcessSession =
		WoTGetProcessProfileSession();
	Result.bProcessSessionShared = ProcessSession != nullptr
		&& ProcessSession->IsShared();
	Result.bSubsystemsAlive =
		GWoTMacUninstallShutdownCountForTesting == ShutdownCountBefore;
	Result.bExitRequested = GWoTMacUninstallExitRequestedForTesting;
	// Drain any main-thread work the refusal queued before the spec returns.
	MainThreadCall(^{}, true, NSDefaultRunLoopMode);
	GWoTMacUninstallSuppressAlertForTesting = false;
	GWoTMacUninstallSuppressExitForTesting = false;
	GWoTMacUninstallAssumeShutdownReadyForTesting = false;
	return Result;
}

void WoTInstallMacUninstallMenuItemForTesting()
{
	MainThreadCall(^{
		if ([NSApp mainMenu] == nil || [[NSApp mainMenu] numberOfItems] == 0)
		{
			NSMenu* Main = [[NSMenu alloc] initWithTitle:@"Main"];
			NSMenuItem* Root = [[NSMenuItem alloc] initWithTitle:@"WoT Reborn"
				action:nil keyEquivalent:@""];
			NSMenu* App = [[NSMenu alloc] initWithTitle:@"WoT Reborn"];
			NSMenuItem* Quit = [[NSMenuItem alloc] initWithTitle:@"Quit"
				action:@selector(terminate:) keyEquivalent:@"q"];
			[App addItem:Quit];
			[Root setSubmenu:App];
			[Main addItem:Root];
			[NSApp setMainMenu:Main];
		#if !__has_feature(objc_arc)
			[Quit release];
			[App release];
			[Root release];
			[Main release];
		#endif
		}
	}, true, NSDefaultRunLoopMode);
	WoTInstallMacUninstallMenuItem();
	MainThreadCall(^{}, true, NSDefaultRunLoopMode);
}

int32 WoTMacUninstallMenuItemCountForTesting()
{
	__block int32 Count = 0;
	MainThreadCall(^{
		NSMenu* AppMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
		for (NSMenuItem* Item in [AppMenu itemArray])
		{
			if ([Item tag] == GWoTMacUninstallMenuTag)
			{
				++Count;
			}
		}
	}, true, NSDefaultRunLoopMode);
	return Count;
}

void WoTRemoveMacUninstallMenuItemForTesting()
{
	MainThreadCall(^{
		// Specs share one editor process with the rest of the suite: leave no
		// NSApplicationDidUpdate observer behind to re-insert the item.
		if (GWoTMacUninstallMenuObserver != nil)
		{
			[[NSNotificationCenter defaultCenter]
				removeObserver:GWoTMacUninstallMenuObserver];
			GWoTMacUninstallMenuObserver = nil;
		}
		NSMenu* AppMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
		for (NSInteger Index = [AppMenu numberOfItems] - 1; Index >= 0; --Index)
		{
			if ([[AppMenu itemAtIndex:Index] tag] == GWoTMacUninstallMenuTag)
			{
				[AppMenu removeItemAtIndex:Index];
			}
		}
	}, true, NSDefaultRunLoopMode);
}
#endif

#endif
