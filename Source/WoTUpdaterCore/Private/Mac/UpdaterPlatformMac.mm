// AppKit and CoreServices still expose legacy Carbon names that collide with
// UE aliases. Match the engine's Mac system-header workaround locally.
#pragma push_macro("FVector")
#define FVector FVectorWorkaround
#import <AppKit/AppKit.h>
#include <CoreServices/CoreServices.h>
#include <Security/SecStaticCode.h>
#if __has_include(<Security/SecTranslocate.h>)
#include <Security/SecTranslocate.h>
#else
// Current public macOS SDKs export this Security.framework symbol but omit the
// Apple open-source header. Keep the declaration byte-for-byte compatible with
// SecTranslocate.h so the same source also uses the header when an SDK ships it.
extern "C" Boolean SecTranslocateIsTranslocatedURL(
	CFURLRef Path, bool* IsTranslocated, CFErrorRef* Error);
#endif
#pragma pop_macro("FVector")

#include "WoTUpdaterCore/UpdaterPlatform.h"

#include "Misc/Paths.h"

namespace
{
FWoTBootstrapStatus WoTMacPlatformError(EWoTBootstrapError Code,
	FString Message, FString Path = FString())
{
	return FWoTBootstrapStatus::Error(
		MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path)));
}
FString WoTMacPlatformString(NSString* Value)
{
	if (Value == nil) return FString();
	const char* Utf8 = [Value UTF8String];
	return Utf8 != nullptr ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
}

TOptional<FString> WoTMacOutermostAppPath(FStringView RunningTreeRoot)
{
	FString Normalized(RunningTreeRoot);
	FPaths::NormalizeDirectoryName(Normalized);
	if (Normalized.IsEmpty() || FPaths::IsRelative(Normalized)
		|| !FPaths::CollapseRelativeDirectories(Normalized)) return {};

	TArray<FString> Components;
	Normalized.ParseIntoArray(Components, TEXT("/"), true);
	FString Current = TEXT("/");
	for (const FString& Component : Components)
	{
		Current = FPaths::Combine(Current, Component);
		if (Component.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
		{
			return Current;
		}
	}
	return {};
}

}

bool WoTIsRunningTreeTranslocated(const FString& RunningTreeRoot)
{
	const TOptional<FString> AppPath = WoTMacOutermostAppPath(RunningTreeRoot);
	if (!AppPath.IsSet())
	{
		UE_LOG(LogWoTBootstrap, Warning,
			TEXT("App Translocation check failed closed: no outer .app in %s"),
			*RunningTreeRoot);
		return true;
	}

	@autoreleasepool
	{
		const FTCHARToUTF8 PathUtf8(*AppPath.GetValue());
		NSString* PathString = [NSString stringWithUTF8String:PathUtf8.Get()];
		NSURL* Url = PathString != nil
			? [NSURL fileURLWithPath:PathString isDirectory:YES] : nil;
		if (Url == nil)
		{
			UE_LOG(LogWoTBootstrap, Warning,
				TEXT("App Translocation check failed closed: invalid app URL %s"),
				*AppPath.GetValue());
			return true;
		}

		bool bTranslocated = true;
		CFErrorRef Error = nullptr;
		const Boolean Queried = SecTranslocateIsTranslocatedURL(
			(CFURLRef)Url, &bTranslocated, &Error);
		if (!Queried)
		{
			NSString* Description = Error != nullptr
				? [(NSError*)Error localizedDescription] : @"unknown error";
			UE_LOG(LogWoTBootstrap, Warning,
				TEXT("App Translocation check failed closed for %s: %s"),
				*AppPath.GetValue(), *WoTMacPlatformString(Description));
			if (Error != nullptr) CFRelease(Error);
			return true;
		}
		if (Error != nullptr) CFRelease(Error);
		return bTranslocated;
	}
}

FWoTBootstrapStatus WoTVerifyStagedCodeSeal(const FString& StagedTreeRoot)
{
	if (StagedTreeRoot.IsEmpty() || FPaths::IsRelative(StagedTreeRoot))
	{
		return WoTMacPlatformError(EWoTBootstrapError::UnsafePath,
			TEXT("the staged code-seal path is not absolute"), StagedTreeRoot);
	}
	@autoreleasepool
	{
		const FTCHARToUTF8 PathUtf8(*StagedTreeRoot);
		NSString* PathString = [NSString stringWithUTF8String:PathUtf8.Get()];
		NSURL* Url = PathString != nil
			? [NSURL fileURLWithPath:PathString isDirectory:YES] : nil;
		if (Url == nil)
		{
			return WoTMacPlatformError(EWoTBootstrapError::UnsafePath,
				TEXT("the staged code-seal path cannot be represented as a file URL"),
				StagedTreeRoot);
		}
		SecStaticCodeRef StaticCode = nullptr;
		const OSStatus Created = SecStaticCodeCreateWithPath(
			(CFURLRef)Url, kSecCSDefaultFlags, &StaticCode);
		if (Created != errSecSuccess || StaticCode == nullptr)
		{
			return WoTMacPlatformError(EWoTBootstrapError::Crypto,
				Created != errSecSuccess
					? FString::Printf(
						TEXT("SecStaticCodeCreateWithPath failed with OSStatus %d"),
						static_cast<int32>(Created))
					: TEXT("SecStaticCodeCreateWithPath returned no code object"),
				StagedTreeRoot);
		}
		const SecCSFlags Flags = static_cast<SecCSFlags>(
			kSecCSStrictValidate | kSecCSCheckAllArchitectures
			| kSecCSCheckNestedCode);
		const OSStatus Checked = SecStaticCodeCheckValidity(
			StaticCode, Flags, nullptr);
		CFRelease(StaticCode);
		if (Checked != errSecSuccess)
		{
			return WoTMacPlatformError(EWoTBootstrapError::Crypto,
				FString::Printf(
					TEXT("SecStaticCodeCheckValidity failed with OSStatus %d"),
					static_cast<int32>(Checked)), StagedTreeRoot);
		}
		return FWoTBootstrapStatus::Success();
	}
}

FWoTBootstrapStatus WoTRegisterLiveBundle(const FString& LiveTreeRoot)
{
	@autoreleasepool
	{
		const FTCHARToUTF8 PathUtf8(*LiveTreeRoot);
		NSString* PathString = [NSString stringWithUTF8String:PathUtf8.Get()];
		NSURL* Url = PathString != nil ? [NSURL fileURLWithPath:PathString isDirectory:YES] : nil;
		if (Url == nil) return WoTMacPlatformError(EWoTBootstrapError::UnsafePath, TEXT("invalid live bundle URL"));
		const OSStatus Registered = LSRegisterURL((CFURLRef)Url, true);
		return Registered == noErr ? FWoTBootstrapStatus::Success()
			: WoTMacPlatformError(EWoTBootstrapError::Io,
				FString::Printf(TEXT("LSRegisterURL failed with OSStatus %d"), static_cast<int32>(Registered)), LiveTreeRoot);
	}
}
