#include "../WoTMacUninstall.h"

#if WITH_AUTOMATION_TESTS && PLATFORM_MAC

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ProfileSession.h"

namespace
{
class FWoTMacUninstallFixture
{
public:
	FWoTMacUninstallFixture()
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Mac Uninstall Tests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FPaths::NormalizeDirectoryName(Root);
		DataRoot = FPaths::Combine(Root, TEXT("profile/Library/Application Support/WoT Reborn"));
		Bundle = FPaths::Combine(Root, TEXT("Install With Spaces/WoT Reborn.app"));
		Update = FPaths::Combine(FPaths::GetPath(Bundle), TEXT(".wot-update"));
		Installer = FPaths::Combine(Root, TEXT("Downloads/setup_wot.exe"));
		ProfileSibling = FPaths::Combine(
			FPaths::GetPath(DataRoot), TEXT("neighbor-sentinel.txt"));
		IFileManager::Get().MakeDirectory(*DataRoot, true);
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Bundle, TEXT("Contents/MacOS")), true);
		IFileManager::Get().MakeDirectory(*Update, true);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Installer), true);
		FFileHelper::SaveStringToFile(TEXT("payload"),
			*FPaths::Combine(DataRoot, TEXT("payload.bin")));
		FFileHelper::SaveStringToFile(TEXT("game"),
			*FPaths::Combine(Bundle, TEXT("Contents/MacOS/WoT-Mac-Test")));
		FFileHelper::SaveStringToFile(TEXT("temporary"),
			*FPaths::Combine(Update, TEXT("temporary.bin")));
		FFileHelper::SaveStringToFile(TEXT("installer"), *Installer);
		FFileHelper::SaveStringToFile(TEXT("keep"), *ProfileSibling);
	}

	~FWoTMacUninstallFixture()
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}

	bool Recycle(const FString& Path)
	{
		Recycled = Path;
		return IFileManager::Get().DeleteDirectory(*Path, false, true);
	}

	FString Root;
	FString DataRoot;
	FString Bundle;
	FString Update;
	FString Installer;
	FString ProfileSibling;
	FString Recycled;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallMenuIdempotentSpec,
	"WoT.MacUninstall.MenuInstallsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallMenuIdempotentSpec::RunTest(const FString&)
{
	WoTRemoveMacUninstallMenuItemForTesting();
	WoTInstallMacUninstallMenuItemForTesting();
	WoTInstallMacUninstallMenuItemForTesting();
	TestEqual(TEXT("one tagged menu item"),
		WoTMacUninstallMenuItemCountForTesting(), 1);
	WoTRemoveMacUninstallMenuItemForTesting();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallCancelSpec,
	"WoT.MacUninstall.CancelRemovesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallCancelSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	bool bResolverCalled = false;
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(false, Fixture.DataRoot,
			Fixture.DataRoot, Fixture.Bundle,
			[&bResolverCalled](const FString&) -> TOptional<FString>
			{
				bResolverCalled = true;
				return {};
			},
			[](const FString&) { return false; });
	TestFalse(TEXT("cancel does not resolve the app"), bResolverCalled);
	TestTrue(TEXT("data remains"), IFileManager::Get().DirectoryExists(*Fixture.DataRoot));
	TestTrue(TEXT("update temp remains"), IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestTrue(TEXT("app remains"), IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	TestEqual(TEXT("nothing removed"), Result.RemovedPaths.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallConfirmSpec,
	"WoT.MacUninstall.ConfirmRemovesDataAndUpdater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallConfirmSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(true, Fixture.DataRoot,
			Fixture.DataRoot, Fixture.Bundle,
			[&Fixture](const FString&) { return TOptional<FString>(Fixture.Bundle); },
			[&Fixture](const FString& Path) { return Fixture.Recycle(Path); });
	TestFalse(TEXT("data removed"), IFileManager::Get().DirectoryExists(*Fixture.DataRoot));
	TestFalse(TEXT("root session lock is unlinked with the empty Data root"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			Fixture.DataRoot, WoTProfileSessionLockLeaf())));
	TestFalse(TEXT("update temp removed"), IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestFalse(TEXT("app recycled"), IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	TestTrue(TEXT("downloaded installer sibling survives"),
		IFileManager::Get().FileExists(*Fixture.Installer));
	TestTrue(TEXT("profile-root sibling sentinel survives"),
		IFileManager::Get().FileExists(*Fixture.ProfileSibling));
	TestEqual(TEXT("original app selected"), Fixture.Recycled, Fixture.Bundle);
	TestEqual(TEXT("no remaining paths"), Result.RemainingPaths.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallSymlinkSpec,
	"WoT.MacUninstall.SymlinkedRootRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallSymlinkSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	const FString LinkParent = FPaths::Combine(Fixture.Root, TEXT("linked profile"));
	const FString LinkPath = FPaths::Combine(LinkParent, TEXT("WoT Reborn"));
	const FString Target = FPaths::Combine(Fixture.Root, TEXT("must survive"));
	IFileManager::Get().MakeDirectory(*LinkParent, true);
	IFileManager::Get().MakeDirectory(*Target, true);
	FFileHelper::SaveStringToFile(TEXT("keep"), *FPaths::Combine(Target, TEXT("keep.bin")));
	auto Parent = OpenNativeDirectoryPathNoFollow(LinkParent);
	TestFalse(TEXT("link parent opens"), Parent.IsError());
	if (!Parent.IsError())
	{
		TestFalse(TEXT("root link created"), CreateNativeSymbolicLinkAtNoFollow(
			Parent.GetValue(), TEXT("WoT Reborn"), Target).IsError());
	}
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(true, LinkPath, LinkPath, Fixture.Bundle,
			[&Fixture](const FString&) { return TOptional<FString>(Fixture.Bundle); },
			[&Fixture](const FString& Path) { return Fixture.Recycle(Path); });
	TestTrue(TEXT("symlinked root is reported"), Result.RemainingPaths.Contains(LinkPath));
	TestTrue(TEXT("target survives"), IFileManager::Get().FileExists(
		*FPaths::Combine(Target, TEXT("keep.bin"))));
	TestTrue(TEXT("updater survives refused root"),
		IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestTrue(TEXT("app survives refused root"),
		IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	if (!Parent.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(Parent.GetValue(), TEXT("WoT Reborn"),
			EWoTTreeEntryType::SymbolicLink);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallOtherSessionSpec,
	"WoT.MacUninstall.OtherSessionRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallOtherSessionSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	TWoTBootstrapResult<FWoTProfileSession> OtherSession =
		AcquireWoTProfileSessionShared(Fixture.DataRoot);
	TestFalse(TEXT("other session acquires shared"), OtherSession.IsError());
	if (OtherSession.IsError()) return false;
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(true, Fixture.DataRoot,
			Fixture.DataRoot, Fixture.Bundle,
			[&Fixture](const FString&) { return TOptional<FString>(Fixture.Bundle); },
			[&Fixture](const FString& Path) { return Fixture.Recycle(Path); });
	TestEqual(TEXT("other-copy refusal is exact"), Result.FailureReason,
		FString(TEXT("Quit the other copy of WoT Reborn first")));
	TestTrue(TEXT("data survives other process"),
		IFileManager::Get().DirectoryExists(*Fixture.DataRoot));
	TestTrue(TEXT("updater survives other process"),
		IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestTrue(TEXT("app survives other process"),
		IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	TestTrue(TEXT("bundle was not recycled"), Fixture.Recycled.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallProductionRefusalSpec,
	"WoT.MacUninstall.ProductionContentionExitsAfterDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallProductionRefusalSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	WoTResetProcessProfileSessionForTesting();
	ON_SCOPE_EXIT
	{
		WoTResetProcessProfileSessionForTesting();
		WoTRemoveMacUninstallMenuItemForTesting();
	};
	FWoTBootstrapStatus GameSession =
		WoTAcquireProcessProfileSessionShared(Fixture.DataRoot);
	TestFalse(TEXT("game session acquires shared"), GameSession.IsError());
	if (GameSession.IsError()) return false;
	TWoTBootstrapResult<FWoTProfileSession> OtherSession =
		AcquireWoTProfileSessionShared(Fixture.DataRoot);
	TestFalse(TEXT("other session acquires shared"), OtherSession.IsError());
	if (OtherSession.IsError()) return false;
	WoTInstallMacUninstallMenuItemForTesting();

	const FWoTMacUninstallProductionFlowTestResult Result =
		WoTRunMacUninstallProductionRefusalForTesting(
			Fixture.DataRoot, Fixture.DataRoot, Fixture.Bundle,
			[&Fixture](const FString&)
			{
				return TOptional<FString>(Fixture.Bundle);
			});
	TestEqual(TEXT("other-copy refusal is exact"), Result.FailureReason,
		FString(TEXT("Quit the other copy of WoT Reborn first")));
	TestTrue(TEXT("drained refusal remains in progress until exit"),
		Result.bInProgress);
	TestFalse(TEXT("uninstall menu stays disabled while exit is pending"),
		Result.bMenuEnabled);
	TestFalse(TEXT("a drained game does not resume its process session"),
		Result.bProcessSessionShared);
	TestFalse(TEXT("real subsystem shutdown is committed before contention"),
		Result.bSubsystemsAlive);
	TestTrue(TEXT("a drained refusal requests immediate exit"),
		Result.bExitRequested);
	TestTrue(TEXT("data survives production refusal"),
		IFileManager::Get().DirectoryExists(*Fixture.DataRoot));
	TestTrue(TEXT("updater survives production refusal"),
		IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestTrue(TEXT("app survives production refusal"),
		IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallResolutionFailureSpec,
	"WoT.MacUninstall.TranslocationResolutionFailureRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallResolutionFailureSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(true, Fixture.DataRoot,
			Fixture.DataRoot, Fixture.Bundle,
			[](const FString&) { return TOptional<FString>(); },
			[&Fixture](const FString& Path) { return Fixture.Recycle(Path); });
	TestTrue(TEXT("resolution failure is explicit"),
		Result.FailureReason.Contains(TEXT("resolution failed")));
	TestTrue(TEXT("data survives resolution failure"),
		IFileManager::Get().DirectoryExists(*Fixture.DataRoot));
	TestTrue(TEXT("updater survives resolution failure"),
		IFileManager::Get().DirectoryExists(*Fixture.Update));
	TestTrue(TEXT("app survives resolution failure"),
		IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	TestTrue(TEXT("bundle was not recycled"), Fixture.Recycled.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUninstallTranslocationSpec,
	"WoT.MacUninstall.TranslocatedUsesOriginal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWoTMacUninstallTranslocationSpec::RunTest(const FString&)
{
	FWoTMacUninstallFixture Fixture;
	const FString Translocated = FPaths::Combine(
		Fixture.Root, TEXT("AppTranslocation/random/d/WoT Reborn.app"));
	FString ResolverInput;
	const FWoTMacUninstallTestResult Result =
		WoTRunMacUninstallFilesystemForTesting(true, Fixture.DataRoot,
			Fixture.DataRoot, Translocated,
			[&](const FString& Path)
			{
				ResolverInput = Path;
				return TOptional<FString>(Fixture.Bundle);
			},
			[&Fixture](const FString& Path) { return Fixture.Recycle(Path); });
	TestEqual(TEXT("runtime path queried"), ResolverInput, Translocated);
	TestEqual(TEXT("original app recycled"), Fixture.Recycled, Fixture.Bundle);
	TestFalse(TEXT("original removed"), IFileManager::Get().DirectoryExists(*Fixture.Bundle));
	TestEqual(TEXT("no remaining paths"), Result.RemainingPaths.Num(), 0);
	return true;
}

#endif
