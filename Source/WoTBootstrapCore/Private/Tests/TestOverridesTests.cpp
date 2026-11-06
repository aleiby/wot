// Specs for the single strict loopback gate shared by the game and updater.
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/TestOverrides.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTTestOverridesSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

class FWoTScopedTestAnnouncementUrl final
{
public:
	explicit FWoTScopedTestAnnouncementUrl(const TCHAR* Value)
		: Previous(FPlatformMisc::GetEnvironmentVariable(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL")))
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"), Value);
	}

	~FWoTScopedTestAnnouncementUrl()
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"), *Previous);
	}

private:
	FString Previous;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTStrictTestLoopbackOverrideSpec,
	"WoT.BootstrapCore.TestOverrides.StrictLoopbackGate",
	WoTTestOverridesSpecFlags)

bool FWoTStrictTestLoopbackOverrideSpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TCHAR* Accepted[] = {
		TEXT("http://127.0.0.1:1/a"),
		TEXT("http://127.0.0.1:65535/path/to/announcement.json"),
	};
	for (const TCHAR* Value : Accepted)
	{
		FWoTScopedTestAnnouncementUrl Environment(Value);
		TestTrue(*FString::Printf(TEXT("accepts %s"), Value),
			WoTIsTestLoopbackOverrideActive());
	}

	const TCHAR* Rejected[] = {
		TEXT(""), TEXT("https://127.0.0.1:1234/a"),
		TEXT("http://localhost:1234/a"),
		TEXT("http://127.0.0.1/a"), TEXT("http://127.0.0.1:0/a"),
		TEXT("http://127.0.0.1:65536/a"), TEXT("http://127.0.0.1:+1/a"),
		TEXT("http://127.0.0.1:4294967297/a"),
		TEXT("http://127.0.0.1:999999999999999999999999/a"),
		TEXT("http://user@127.0.0.1:1234/a"),
		TEXT("http://127.0.0.1:1234/"),
		TEXT("http://127.0.0.1:1234/a?query"),
		TEXT("http://127.0.0.1:1234/a#fragment"),
		TEXT("http://127.0.0.1:1234/a\\b"),
	};
	for (const TCHAR* Value : Rejected)
	{
		FWoTScopedTestAnnouncementUrl Environment(Value);
		TestFalse(*FString::Printf(TEXT("rejects %s"), Value),
			WoTIsTestLoopbackOverrideActive());
	}
	return true;
}

#endif
