#include "WoTKeychainAccess.h"

#if PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#include <Security/Security.h>
#endif

namespace
{
// Security.framework status codes, spelled out so Classify stays portable.
constexpr int32 WoTKeychainSuccess = 0;              // errSecSuccess
constexpr int32 WoTKeychainItemNotFound = -25300;    // errSecItemNotFound
constexpr int32 WoTKeychainAuthFailed = -25293;      // errSecAuthFailed
constexpr int32 WoTKeychainNoInteraction = -25308;   // errSecInteractionNotAllowed

#if WITH_AUTOMATION_TESTS
TFunction<EWoTKeychainAccess()>& WoTKeychainProbeOverride()
{
	static TFunction<EWoTKeychainAccess()> Override;
	return Override;
}
#endif
}

namespace WoTKeychainAccess
{
EWoTKeychainAccess Classify(int32 OSStatus)
{
	switch (OSStatus)
	{
	case WoTKeychainSuccess:
	case WoTKeychainItemNotFound:
		return EWoTKeychainAccess::NoPrompt;
	case WoTKeychainAuthFailed:
	case WoTKeychainNoInteraction:
		return EWoTKeychainAccess::WillPrompt;
	default:
		return EWoTKeychainAccess::Unknown;
	}
}

EWoTKeychainAccess Probe(const FString& AccountId, FString* OutDetail)
{
#if WITH_AUTOMATION_TESTS
	if (WoTKeychainProbeOverride())
	{
		const EWoTKeychainAccess Access = WoTKeychainProbeOverride()();
		if (OutDetail)
		{
			*OutDetail = FString::Printf(TEXT("access=%s status=override"),
				LexToString(Access));
		}
		return Access;
	}
#endif
#if PLATFORM_MAC
	if (AccountId.IsEmpty())
	{
		if (OutDetail)
		{
			*OutDetail = TEXT("access=unknown status=no-account");
		}
		return EWoTKeychainAccess::Unknown;
	}
	// The interaction switch is process-wide: probe only from the game thread
	// (the front end), never beside the EOS SDK's own keychain reads.
	if (!ensure(IsInGameThread()))
	{
		if (OutDetail)
		{
			*OutDetail = TEXT("access=unknown status=off-game-thread");
		}
		return EWoTKeychainAccess::Unknown;
	}
	CFStringRef Account = CFStringCreateWithCString(kCFAllocatorDefault,
		TCHAR_TO_UTF8(*AccountId), kCFStringEncodingUTF8);
	const void* Keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
		kSecReturnData, kSecMatchLimit, kSecUseAuthenticationUI};
	const void* Values[] = {kSecClassGenericPassword,
		CFSTR("com.epicgames.sdk.connect.access_token"), Account,
		kCFBooleanTrue, kSecMatchLimitOne, kSecUseAuthenticationUIFail};
	CFDictionaryRef Query = CFDictionaryCreate(kCFAllocatorDefault, Keys, Values,
		UE_ARRAY_COUNT(Keys), &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	// The login keychain is a legacy file keychain: its ACL dialog honours the
	// process-wide interaction switch, not only kSecUseAuthenticationUIFail.
	// Nothing else touches the keychain while the front end probes (sign-in
	// starts only after this returns), and the previous value is restored.
	// The query runs only if UI really is off: a dialog here is the very
	// thing the probe exists to prevent.
	Boolean bInteractionWasAllowed = true;
	CFTypeRef Data = nullptr;
	OSStatus Status = errSecInteractionNotAllowed;
	bool bQueried = false;
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (SecKeychainGetUserInteractionAllowed(&bInteractionWasAllowed) == errSecSuccess
		&& SecKeychainSetUserInteractionAllowed(false) == errSecSuccess)
	{
		Status = SecItemCopyMatching(Query, &Data);
		bQueried = true;
		SecKeychainSetUserInteractionAllowed(bInteractionWasAllowed);
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	if (Data != nullptr)
	{
		CFRelease(Data);
	}
	CFRelease(Query);
	CFRelease(Account);
	if (!bQueried)
	{
		if (OutDetail)
		{
			*OutDetail = TEXT("access=unknown status=interaction-switch-failed");
		}
		return EWoTKeychainAccess::Unknown;
	}
	const EWoTKeychainAccess Access = Classify(static_cast<int32>(Status));
	if (OutDetail)
	{
		*OutDetail = FString::Printf(TEXT("access=%s status=%d"),
			LexToString(Access), static_cast<int32>(Status));
	}
	return Access;
#else
	if (OutDetail)
	{
		*OutDetail = TEXT("access=no-prompt status=not-applicable");
	}
	return EWoTKeychainAccess::NoPrompt;
#endif
}

const TCHAR* LexToString(EWoTKeychainAccess Access)
{
	switch (Access)
	{
	case EWoTKeychainAccess::NoPrompt: return TEXT("no-prompt");
	case EWoTKeychainAccess::WillPrompt: return TEXT("will-prompt");
	case EWoTKeychainAccess::Unknown:
	default: return TEXT("unknown");
	}
}

#if WITH_AUTOMATION_TESTS
void SetProbeOverrideForTesting(TFunction<EWoTKeychainAccess()> Override)
{
	WoTKeychainProbeOverride() = MoveTemp(Override);
}
#endif
}
