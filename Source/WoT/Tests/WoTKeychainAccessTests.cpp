// macOS keychain prompt probe: the pure classification of a no-UI
// SecItemCopyMatching status (the platform probe feeds it the real one).
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Online/WoTKeychainAccess.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTKeychainAccessClassifySpec,
	"WoT.Internet.Keychain.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTKeychainAccessClassifySpec::RunTest(const FString&)
{
	TestEqual(TEXT("readable without asking"),
		WoTKeychainAccess::Classify(0), EWoTKeychainAccess::NoPrompt);
	TestEqual(TEXT("no item yet: the SDK creates it without asking"),
		WoTKeychainAccess::Classify(-25300), EWoTKeychainAccess::NoPrompt);
	// Measured 2026-10-04: a different code identity reading the EOS item from
	// the login keychain with UI disabled answers errSecAuthFailed.
	TestEqual(TEXT("legacy keychain ACL would ask"),
		WoTKeychainAccess::Classify(-25293), EWoTKeychainAccess::WillPrompt);
	TestEqual(TEXT("data-protection keychain would ask"),
		WoTKeychainAccess::Classify(-25308), EWoTKeychainAccess::WillPrompt);
	TestEqual(TEXT("any other answer is undecided"),
		WoTKeychainAccess::Classify(-34018), EWoTKeychainAccess::Unknown);
	TestEqual(TEXT("lex names"), FString(WoTKeychainAccess::LexToString(
		EWoTKeychainAccess::WillPrompt)), FString(TEXT("will-prompt")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTKeychainAccessPlatformSpec,
	"WoT.Internet.Keychain.PlatformProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTKeychainAccessPlatformSpec::RunTest(const FString&)
{
	// An account no product uses: never an item, so never a dialog.
	FString Detail;
	const EWoTKeychainAccess Access = WoTKeychainAccess::Probe(
		TEXT("00000000000000000000000000000000"), &Detail);
	TestEqual(TEXT("an absent item needs no prompt"), Access,
		EWoTKeychainAccess::NoPrompt);
	TestTrue(TEXT("the probe explains itself"), Detail.StartsWith(TEXT("access=")));
	return !HasAnyErrors();
}

#endif
