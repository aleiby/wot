#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "../Updater/WoTInstallerLocations.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr EAutomationTestFlags InstallerLocationsSpec_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

struct FInstallerLocationsSpec_EnvironmentScope
{
	explicit FInstallerLocationsSpec_EnvironmentScope(const FString& Value)
		: Previous(FPlatformMisc::GetEnvironmentVariable(
			TEXT("WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR")))
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR"), *Value);
	}
	~FInstallerLocationsSpec_EnvironmentScope()
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_DOWNLOADS_DIR"), *Previous);
	}
	FString Previous;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstallerLocationsCandidatesSpec,
	"WoT.Bootstrap.InstallerLocations.Candidates",
	InstallerLocationsSpec_Flags)

bool FInstallerLocationsCandidatesSpec::RunTest(const FString&)
{
	const TArray<FString> Candidates =
		WoTDownloadsInstallerCandidates(TEXT("/d"));
	TestEqual(TEXT("there are exactly ten browser names"),
		Candidates.Num(), 10);
	if (Candidates.Num() == 10)
	{
		TestEqual(TEXT("the pinned name is first"),
			FPaths::GetCleanFilename(Candidates[0]),
			FString(WoT::Bootstrap::InstallerName));
		TestEqual(TEXT("the first duplicate is second"),
			FPaths::GetCleanFilename(Candidates[1]),
			FString(TEXT("setup_the_wheel_of_time_333b_(54846) (1).exe")));
		TestEqual(TEXT("the ninth duplicate is last"),
			FPaths::GetCleanFilename(Candidates[9]),
			FString(TEXT("setup_the_wheel_of_time_333b_(54846) (9).exe")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstallerLocationsTestSeamSpec,
	"WoT.Bootstrap.InstallerLocations.TestSeam",
	InstallerLocationsSpec_Flags)

bool FInstallerLocationsTestSeamSpec::RunTest(const FString&)
{
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-installer-locations-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeDirectoryName(Root);
	if (!TestTrue(TEXT("the seam directory is created"),
		IFileManager::Get().MakeDirectory(*Root, true)))
	{
		return false;
	}
	FInstallerLocationsSpec_EnvironmentScope Environment(Root);
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Canonical =
		OpenCanonicalNativeDirectoryNoFollow(Root);
	if (!TestFalse(TEXT("the seam directory canonicalizes"),
			Canonical.IsError()))
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	TestEqual(TEXT("the override replaces platform discovery"),
		WoTResolveDownloadsDirectory(),
		Canonical.GetValue().CanonicalPath);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return !HasAnyErrors();
}

#endif
