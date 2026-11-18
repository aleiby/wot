#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/ProfileSession.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTProfileSessionSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

class FWoTProfileSessionFixture final
{
public:
	FWoTProfileSessionFixture()
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(), TEXT("Profile Session Tests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT("Data")));
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root), true);
	}

	~FWoTProfileSessionFixture()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Root), false, true);
	}

	FString Root;
};

FString WoTProfileSessionRunner()
{
	FString BinaryDirectory = FPaths::ConvertRelativePathToFull(
		FPlatformProcess::BaseDir());
	FPaths::NormalizeDirectoryName(BinaryDirectory);
#if PLATFORM_WINDOWS
	const FString Sibling = FPaths::Combine(
		BinaryDirectory, TEXT("WoTBootstrapCoreTests.exe"));
	if (IFileManager::Get().FileExists(*Sibling)) return Sibling;
	return FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Binaries/Win64/WoTBootstrapCoreTests.exe"));
#elif PLATFORM_MAC
	FString Runner = FPaths::Combine(
		BinaryDirectory, TEXT("WoTBootstrapCoreTests"));
	if (IFileManager::Get().FileExists(*Runner)) return Runner;
	Runner = FPaths::Combine(BinaryDirectory,
		TEXT("WoTBootstrapCoreTests.app/Contents/MacOS/WoTBootstrapCoreTests"));
	if (IFileManager::Get().FileExists(*Runner)) return Runner;
	Runner = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		BinaryDirectory,
		TEXT("../../../WoTBootstrapCoreTests.app/Contents/MacOS/WoTBootstrapCoreTests")));
	if (IFileManager::Get().FileExists(*Runner)) return Runner;
	Runner = FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Binaries/Mac/WoTBootstrapCoreTests"));
	if (!IFileManager::Get().FileExists(*Runner))
	{
		Runner = FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Binaries/Mac/WoTBootstrapCoreTests.app/Contents/MacOS/WoTBootstrapCoreTests"));
	}
	return Runner;
#else
	return FString();
#endif
}

bool RunStartupProbe(const FString& Runner, const FString& Root,
	const FString& UserDirectory, const FString& Accessed,
	int32& OutExitCode)
{
	uint32 ChildPid = 0;
	const FString Arguments = FString::Printf(
		TEXT("-UserDir=\"%s\" --profile-session-startup-probe=\"%s\" --accessed=\"%s\""),
		*UserDirectory, *Root, *Accessed);
	FProcHandle Child = FPlatformProcess::CreateProc(
		*Runner, *Arguments, false, false, false, &ChildPid, 0,
		*FPaths::GetPath(Runner), nullptr);
	if (!Child.IsValid()) return false;
	FPlatformProcess::WaitForProc(Child);
	const bool bRead = FPlatformProcess::GetProcReturnCode(
		Child, &OutExitCode);
	FPlatformProcess::CloseProc(Child);
	return bRead;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProfileSessionExclusionSpec,
	"WoT.ProfileSession.SharedExclusiveExclusion",
	WoTProfileSessionSpecFlags)
bool FWoTProfileSessionExclusionSpec::RunTest(const FString&)
{
	FWoTProfileSessionFixture Fixture;
	auto FirstResult = AcquireWoTProfileSessionShared(Fixture.Root);
	auto SecondResult = AcquireWoTProfileSessionShared(Fixture.Root);
	if (!TestTrue(TEXT("two owners may hold SHARED concurrently"),
		!FirstResult.IsError() && !SecondResult.IsError()))
	{
		return false;
	}
	FWoTProfileSession First = MoveTemp(FirstResult).GetValue();
	FWoTProfileSession Second = MoveTemp(SecondResult).GetValue();
	auto Refused = First.TryAcquireExclusive();
	TestTrue(TEXT("another SHARED holder refuses EXCLUSIVE without error"),
		!Refused.IsError() && !Refused.GetValue());
	TestTrue(TEXT("refusal restores SHARED"), First.IsShared());

	Second = FWoTProfileSession();
	auto Exclusive = First.TryAcquireExclusive();
	TestTrue(TEXT("the sole holder acquires EXCLUSIVE"),
		!Exclusive.IsError() && Exclusive.GetValue());
	auto BlockedShared = AcquireWoTProfileSessionShared(Fixture.Root);
	TestTrue(TEXT("EXCLUSIVE blocks a new game session"),
		BlockedShared.IsError());
	if (BlockedShared.IsError())
	{
		TestEqual(TEXT("blocked startup is classified as contention"),
			BlockedShared.GetError().Code,
			EWoTBootstrapError::ServiceUnavailable);
		TestEqual(TEXT("blocked startup has the close/retry message"),
			BlockedShared.GetError().Message,
			FString(WoTProfileSessionContentionMessage()));
	}
	TestFalse(TEXT("EXCLUSIVE restores to SHARED"),
		First.RestoreShared().IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProfileSessionRootLockSpec,
	"WoT.ProfileSession.LockLivesAtDataRoot",
	WoTProfileSessionSpecFlags)
bool FWoTProfileSessionRootLockSpec::RunTest(const FString&)
{
	FWoTProfileSessionFixture Fixture;
	auto Acquired = AcquireWoTProfileSessionShared(Fixture.Root);
	if (!TestFalse(TEXT("SHARED creates the Data root"), Acquired.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("the sole created profile entry is root session.lock"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			Fixture.Root, WoTProfileSessionLockLeaf())));
	TestFalse(TEXT("the obsolete UpdaterState lock is absent"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			Fixture.Root, TEXT("UpdaterState"), WoTProfileSessionLockLeaf())));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProfileSessionFailurePolicySpec,
	"WoT.ProfileSession.ContentionAndUnwritableAreDistinct",
	WoTProfileSessionSpecFlags)
bool FWoTProfileSessionFailurePolicySpec::RunTest(const FString&)
{
	FWoTBootstrapError Contention = MakeWoTBootstrapError(
		EWoTBootstrapError::ServiceUnavailable, TEXT("native contention"));
	const FString ContentionMessage = WoTProfileSessionFailureMessage(
		TEXTVIEW("C:/WoT/Data"), Contention);
	TestEqual(TEXT("contention selects the close/retry message"),
		ContentionMessage, FString(WoTProfileSessionContentionMessage()));
	FWoTBootstrapError Unwritable = MakeWoTBootstrapError(
		EWoTBootstrapError::Io, TEXT("permission denied"));
#if PLATFORM_WINDOWS
	Unwritable.NativeCode = 5;
#endif
	const FString UnwritableMessage = WoTProfileSessionFailureMessage(
		TEXTVIEW("C:/WoT/Data"), Unwritable);
	TestNotEqual(TEXT("an unwritable root is not reported as contention"),
		UnwritableMessage, ContentionMessage);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProfileSessionStartupContentionSpec,
	"WoT.ProfileSession.ExclusiveBlocksChildBeforeProfileAccess",
	WoTProfileSessionSpecFlags)
bool FWoTProfileSessionStartupContentionSpec::RunTest(const FString&)
{
	FWoTProfileSessionFixture Fixture;
	auto Acquired = AcquireWoTProfileSessionShared(Fixture.Root);
	if (!TestFalse(TEXT("parent takes SHARED"), Acquired.IsError())) return false;
	FWoTProfileSession Session = MoveTemp(Acquired).GetValue();
	auto Exclusive = Session.TryAcquireExclusive();
	if (!TestTrue(TEXT("parent takes EXCLUSIVE"),
		!Exclusive.IsError() && Exclusive.GetValue())) return false;
	const FString Runner = WoTProfileSessionRunner();
	if (!TestTrue(TEXT("native runner exists"),
		!Runner.IsEmpty() && IFileManager::Get().FileExists(*Runner))) return false;
	const FString Accessed = FPaths::Combine(
		Fixture.Root, TEXT("child-profile-accessed"));
	const FString ChildUserDirectory = FPaths::Combine(
		FPaths::GetPath(Fixture.Root), TEXT("ChildUserDir"));
	if (!TestTrue(TEXT("child user directory is prepared outside Data"),
		IFileManager::Get().MakeDirectory(*ChildUserDirectory, true))) return false;
	int32 ExitCode = 0;
	if (!TestTrue(TEXT("contended child completes"),
		RunStartupProbe(Runner, Fixture.Root, ChildUserDirectory,
			Accessed, ExitCode))) return false;
	TestEqual(TEXT("contended child refuses startup"), ExitCode, 1);
	TestFalse(TEXT("contended child exits before profile access"),
		IFileManager::Get().FileExists(*Accessed));
	if (!TestFalse(TEXT("parent restores SHARED"),
		Session.RestoreShared().IsError())) return false;
	if (!TestTrue(TEXT("uncontended child completes"),
		RunStartupProbe(Runner, Fixture.Root, ChildUserDirectory,
			Accessed, ExitCode))) return false;
	TestEqual(TEXT("uncontended child starts"), ExitCode, 0);
	TestTrue(TEXT("uncontended child reaches profile access"),
		IFileManager::Get().FileExists(*Accessed));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProfileSessionDeletionOrderSpec,
	"WoT.ProfileSession.UninstallUnlinksLockLast",
	WoTProfileSessionSpecFlags)
bool FWoTProfileSessionDeletionOrderSpec::RunTest(const FString&)
{
	FWoTProfileSessionFixture Fixture;
	auto Acquired = AcquireWoTProfileSessionShared(Fixture.Root);
	if (!TestFalse(TEXT("SHARED acquires"), Acquired.IsError())) return false;
	FWoTProfileSession Session = MoveTemp(Acquired).GetValue();
	const FString Sibling = FPaths::Combine(
		FPaths::GetPath(Fixture.Root), TEXT("installer-sentinel.txt"));
	TestTrue(TEXT("sibling sentinel is created"),
		FFileHelper::SaveStringToFile(TEXT("keep"), *Sibling));
	TestTrue(TEXT("profile content is created"),
		FFileHelper::SaveStringToFile(TEXT("remove"),
			*FPaths::Combine(Fixture.Root, TEXT("preferences.txt"))));
	auto Exclusive = Session.TryAcquireExclusive();
	if (!TestTrue(TEXT("EXCLUSIVE acquires"),
		!Exclusive.IsError() && Exclusive.GetValue()))
	{
		return false;
	}
	if (!TestFalse(TEXT("profile contents are deleted under EXCLUSIVE"),
		Session.DeleteProfileContentsExceptSessionLock().IsError()))
	{
		return false;
	}
	TestTrue(TEXT("session.lock remains held until final removal"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			Fixture.Root, WoTProfileSessionLockLeaf())));
	if (!TestFalse(TEXT("final profile removal succeeds"),
		Session.DeleteSessionLockAndEmptyProfileRoot().IsError()))
	{
		return false;
	}
	TestFalse(TEXT("the Data root is removed after session.lock"),
		IFileManager::Get().DirectoryExists(*Fixture.Root));
	TestTrue(TEXT("the sibling installer sentinel survives"),
		IFileManager::Get().FileExists(*Sibling));
	return !HasAnyErrors();
}

#endif
