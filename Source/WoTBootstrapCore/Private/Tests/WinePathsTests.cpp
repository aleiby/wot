// Specs for the Wine path helpers the installer search uses on Linux/Steam Deck.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/WinePaths.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTWinePathsSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWineUnixDownloadsPathSpec,
	"WoT.BootstrapCore.Wine.UnixDownloadsPath", WoTWinePathsSpecFlags)

bool FWoTWineUnixDownloadsPathSpec::RunTest(const FString&)
{
	TestEqual(TEXT("absolute HOME"), WoTWineUnixDownloadsPath(TEXT("/home/player")),
		FString(TEXT("/home/player/Downloads")));
	TestEqual(TEXT("HOME with a trailing slash"),
		WoTWineUnixDownloadsPath(TEXT("/home/player/")),
		FString(TEXT("/home/player/Downloads")));
	TestEqual(TEXT("no HOME"), WoTWineUnixDownloadsPath(TEXT("")), FString());
	TestEqual(TEXT("a relative HOME is ignored"),
		WoTWineUnixDownloadsPath(TEXT("home/player")), FString());
	TestEqual(TEXT("a HOME of / is ignored"), WoTWineUnixDownloadsPath(TEXT("/")), FString());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWineDosPathOutsideWineSpec,
	"WoT.BootstrapCore.Wine.DosPathOutsideWine", WoTWinePathsSpecFlags)

bool FWoTWineDosPathOutsideWineSpec::RunTest(const FString&)
{
	ON_SCOPE_EXIT { WoTSetRunningUnderWineForTesting(TOptional<bool>()); };
	WoTSetRunningUnderWineForTesting(false);
	TestEqual(TEXT("outside Wine there is no DOS mapping for a Unix path"),
		WoTWineDosPathForUnixPath(TEXT("/home/player/Downloads")), FString());
	TestEqual(TEXT("an empty path never maps"), WoTWineDosPathForUnixPath(TEXT("")),
		FString());
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
