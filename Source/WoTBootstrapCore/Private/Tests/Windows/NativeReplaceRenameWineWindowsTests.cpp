// Windows spec for the Wine-only retry of a held small-file replace.
//
// The game saves small files (update state, markers, preferences, notices) by
// renaming a temporary over the old file. Native Windows replaces a target a
// reader holds (POSIX semantics); Wine (Proton) refuses it with access denied
// instead, so under Wine that refusal is retried like a sharing violation while
// native Windows still fails at once. A read-only target produces the same
// refusal (win32 5) natively, and clearing it is the reader letting go.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "../NativeHandleTestFactory.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if WITH_AUTOMATION_TESTS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
constexpr EAutomationTestFlags WoTReplaceRenameWineSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

FString WoTReplaceWindowsPath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	Full.ReplaceInline(TEXT("/"), TEXT("\\"));
	return Full;
}

bool WoTReplaceWrite(const FString& Path, const char* Text)
{
	return WoTNativeHandleTestFactory::WriteFile(Path, TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Text), static_cast<int32>(FCStringAnsi::Strlen(Text))));
}

bool WoTReplaceSetReadOnly(const FString& Path, bool bReadOnly)
{
	return ::SetFileAttributesW(*WoTReplaceWindowsPath(Path),
		bReadOnly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0;
}

FString WoTReplaceRead(const FString& Path)
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *Path);
	return Text;
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeWineHeldReplaceRetriesSpec,
	"WoT.BootstrapCore.Identity.WineHeldReplaceRetries",
	WoTReplaceRenameWineSpecFlags)

bool FWoTNativeWineHeldReplaceRetriesSpec::RunTest(const FString&)
{
	const FString Root = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	const FString Target = Root + TEXT("/update-state.json");
	const FString Temporary = Root + TEXT("/update-state.json.tmp");
	ON_SCOPE_EXIT
	{
		WoTSetRunningUnderWineForTesting(TOptional<bool>());
		WoTSetNativeReplaceRenameRetryObserverForTesting(nullptr);
		(void)WoTReplaceSetReadOnly(Target, false);
		WoTNativeHandleTestFactory::RemoveTree(Root);
	};

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Directory =
		OpenNativeDirectoryNoFollow(Root);
	if (!TestFalse(TEXT("the temporary root opens"), Directory.IsError()))
	{
		if (Directory.IsError()) AddError(Directory.GetError().ToString());
		return false;
	}

	// Under Wine: the refusal is retried, and the save lands once the holder lets go.
	if (!TestTrue(TEXT("the held target and the new contents are written"),
		WoTReplaceWrite(Target, "old") && WoTReplaceWrite(Temporary, "new")
			&& WoTReplaceSetReadOnly(Target, true)))
	{
		return false;
	}
	WoTSetRunningUnderWineForTesting(true);
	int32 WineRetries = 0;
	WoTSetNativeReplaceRenameRetryObserverForTesting([&](int32 FailedAttempts)
	{
		WineRetries = FailedAttempts;
		if (FailedAttempts == 1)
		{
			(void)WoTReplaceSetReadOnly(Target, false);
		}
	});
	FWoTBootstrapStatus Saved = RenameNativeEntryAtNoFollow(Directory.GetValue(),
		TEXT("update-state.json.tmp"), Directory.GetValue(), TEXT("update-state.json"), true);
	if (!TestFalse(TEXT("under Wine a held target is replaced after it is released"),
		Saved.IsError()))
	{
		AddError(Saved.GetError().ToString());
	}
	TestEqual(TEXT("exactly one retry waited for the holder"), WineRetries, 1);
	TestEqual(TEXT("the new contents replaced the old"), WoTReplaceRead(Target), FString(TEXT("new")));

	// Native Windows: the same refusal is final, with no retry.
	if (!TestTrue(TEXT("the native case is set up"),
		WoTReplaceWrite(Temporary, "newer") && WoTReplaceSetReadOnly(Target, true)))
	{
		return false;
	}
	WoTSetRunningUnderWineForTesting(false);
	int32 NativeRetries = 0;
	WoTSetNativeReplaceRenameRetryObserverForTesting([&](int32 FailedAttempts)
	{
		NativeRetries = FailedAttempts;
	});
	FWoTBootstrapStatus Refused = RenameNativeEntryAtNoFollow(Directory.GetValue(),
		TEXT("update-state.json.tmp"), Directory.GetValue(), TEXT("update-state.json"), true);
	if (TestTrue(TEXT("natively the refusal fails the save"), Refused.IsError()))
	{
		TestEqual(TEXT("with win32 access denied"),
			Refused.GetError().NativeCode, static_cast<uint32>(ERROR_ACCESS_DENIED));
	}
	TestEqual(TEXT("natively nothing is retried"), NativeRetries, 0);
	TestEqual(TEXT("the target keeps its contents"), WoTReplaceRead(Target), FString(TEXT("new")));
	return !HasAnyErrors();
}

#endif
