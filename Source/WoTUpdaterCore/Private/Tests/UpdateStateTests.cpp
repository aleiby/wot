// Real-path specs for the two-field best-effort updater state.
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTUpdaterCore/UpdateState.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTUpdateStateSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

class FWoTUpdateStateSpecRoot final
{
public:
	FWoTUpdateStateSpecRoot()
	{
		Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
			TEXT("UpdaterState"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Path);
		IFileManager::Get().MakeDirectory(*Path, true);
	}

	~FWoTUpdateStateSpecRoot()
	{
		IFileManager::Get().DeleteDirectory(*Path, false, true);
	}

	FWoTUpdateStateSpecRoot(const FWoTUpdateStateSpecRoot&) = delete;
	FWoTUpdateStateSpecRoot& operator=(const FWoTUpdateStateSpecRoot&) = delete;

	FString Path;
};

bool WoTUpdateStateSpecResolve(FAutomationTestBase& Test,
	const FWoTUpdateStateSpecRoot& Fixture, FWoTUpdaterStateRoot& OutRoot)
{
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
		ResolveUpdaterUserStateRoot(Fixture.Path);
	if (!Test.TestFalse(TEXT("the updater state root resolves"), Resolved.IsError()))
	{
		if (Resolved.IsError()) Test.AddError(Resolved.GetError().ToString());
		return false;
	}
	OutRoot = MoveTemp(Resolved).GetValue();
	return true;
}

bool WoTUpdateStateSpecWriteText(const FWoTUpdaterStateRoot& Root,
	const FString& Text)
{
	IFileManager::Get().MakeDirectory(*Root.StateDir, true);
	return FFileHelper::SaveStringToFile(Text,
		*FPaths::Combine(Root.StateDir, TEXT("update-state.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateStateReadSpec,
	"WoT.UpdaterCore.UpdateState.ReadDefaultsAndExactUint64",
	WoTUpdateStateSpecFlags)

bool FWoTUpdateStateReadSpec::RunTest(const FString&)
{
	FWoTUpdateStateSpecRoot Fixture;
	FWoTUpdaterStateRoot Root;
	if (!WoTUpdateStateSpecResolve(*this, Fixture, Root)) return false;

	const FWoTUpdateStateRecordReadResult Missing = ReadWoTUpdateState(Root);
	TestEqual(TEXT("missing state defaults the ring floor"),
		Missing.State.KeyRevocationFloor, uint64(0));
	TestEqual(TEXT("missing state defaults the announcement sequence"),
		Missing.State.NewestAnnouncementSequence, uint64(0));
	TestFalse(TEXT("an absent optional record reports a diagnostic"),
		Missing.Diagnostic.IsEmpty());

	const FString Exact = TEXT("{\"key_revocation_floor\":18446744073709551615,")
		TEXT("\"newest_announcement_sequence\":9007199254740993}");
	if (!TestTrue(TEXT("the exact uint64 fixture is written"),
			WoTUpdateStateSpecWriteText(Root, Exact)))
	{
		return false;
	}
	const FWoTUpdateStateRecordReadResult Read = ReadWoTUpdateState(Root);
	if (!Read.Diagnostic.IsEmpty())
	{
		AddError(TEXT("valid update-state diagnostic: ") + Read.Diagnostic);
	}
	TestTrue(TEXT("valid state has no diagnostic"), Read.Diagnostic.IsEmpty());
	TestEqual(TEXT("the full uint64 range is parsed without floating point"),
		Read.State.KeyRevocationFloor, MAX_uint64);
	TestEqual(TEXT("integers above double precision stay exact"),
		Read.State.NewestAnnouncementSequence, uint64(9007199254740993ull));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateStateMalformedSpec,
	"WoT.UpdaterCore.UpdateState.MalformedAndOversizedDefaultWithDiagnostic",
	WoTUpdateStateSpecFlags)

bool FWoTUpdateStateMalformedSpec::RunTest(const FString&)
{
	FWoTUpdateStateSpecRoot Fixture;
	FWoTUpdaterStateRoot Root;
	if (!WoTUpdateStateSpecResolve(*this, Fixture, Root)) return false;

	const FString Cases[] = {
		TEXT("{\"key_revocation_floor\":1.0,\"newest_announcement_sequence\":2}"),
		TEXT("{\"key_revocation_floor\":1,\"newest_announcement_sequence\":2,\"other\":3}"),
		TEXT("{\"key_revocation_floor\":1,\"newest_announcement_sequence\":-1}"),
		TEXT("{\"key_revocation_floor\":1}"),
	};
	for (const FString& Text : Cases)
	{
		TestTrue(TEXT("the malformed fixture is written"),
			WoTUpdateStateSpecWriteText(Root, Text));
		const FWoTUpdateStateRecordReadResult Read = ReadWoTUpdateState(Root);
		TestEqual(TEXT("malformed state defaults the ring floor"),
			Read.State.KeyRevocationFloor, uint64(0));
		TestEqual(TEXT("malformed state defaults the announcement sequence"),
			Read.State.NewestAnnouncementSequence, uint64(0));
		TestFalse(TEXT("malformed state reports a diagnostic"),
			Read.Diagnostic.IsEmpty());
	}

	TestTrue(TEXT("the oversized fixture is written"),
		WoTUpdateStateSpecWriteText(Root, FString::ChrN(1025, TEXT('0'))));
	const FWoTUpdateStateRecordReadResult Oversized = ReadWoTUpdateState(Root);
	TestEqual(TEXT("oversized state defaults the ring floor"),
		Oversized.State.KeyRevocationFloor, uint64(0));
	TestFalse(TEXT("oversized state reports a diagnostic"),
		Oversized.Diagnostic.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateStateMergePublishSpec,
	"WoT.UpdaterCore.UpdateState.MergeMaxAndReplace",
	WoTUpdateStateSpecFlags)

bool FWoTUpdateStateMergePublishSpec::RunTest(const FString&)
{
	FWoTUpdateStateSpecRoot Fixture;
	FWoTUpdaterStateRoot Root;
	if (!WoTUpdateStateSpecResolve(*this, Fixture, Root)) return false;

	FWoTUpdateStateRecord First;
	First.KeyRevocationFloor = 9;
	First.NewestAnnouncementSequence = 4;
	FString Diagnostic;
	FWoTBootstrapStatus Written =
		MergeAndWriteWoTUpdateState(Root, First, &Diagnostic);
	if (!TestFalse(TEXT("the initial state publishes"), Written.IsError()))
	{
		if (Written.IsError()) AddError(Written.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("the initial write reports that no prior record existed"),
		Diagnostic.IsEmpty());

	FWoTUpdateStateRecord Second;
	Second.KeyRevocationFloor = 3;
	Second.NewestAnnouncementSequence = 12;
	Written = MergeAndWriteWoTUpdateState(Root, Second, &Diagnostic);
	if (!TestFalse(TEXT("the merged state replaces the record"), Written.IsError()))
	{
		if (Written.IsError()) AddError(Written.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("the disk ring maximum is merged into memory"),
		Second.KeyRevocationFloor, uint64(9));
	TestEqual(TEXT("the memory announcement maximum is retained"),
		Second.NewestAnnouncementSequence, uint64(12));

	const FWoTUpdateStateRecordReadResult Read = ReadWoTUpdateState(Root);
	TestEqual(TEXT("the merged ring maximum is published"),
		Read.State.KeyRevocationFloor, uint64(9));
	TestEqual(TEXT("the merged announcement maximum is published"),
		Read.State.NewestAnnouncementSequence, uint64(12));

	TArray<FString> Temporaries;
	IFileManager::Get().FindFiles(Temporaries,
		*FPaths::Combine(Root.StateDir, TEXT("update-state.*.tmp")), true, false);
	TestEqual(TEXT("successful publication leaves no sibling temporary"),
		Temporaries.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateStateUnwritableSpec,
	"WoT.UpdaterCore.UpdateState.PublishFailureRetainsMemory",
	WoTUpdateStateSpecFlags)

bool FWoTUpdateStateUnwritableSpec::RunTest(const FString&)
{
	FWoTUpdateStateSpecRoot Fixture;
	FWoTUpdaterStateRoot Root;
	if (!WoTUpdateStateSpecResolve(*this, Fixture, Root)) return false;
	IFileManager::Get().MakeDirectory(*Root.StateDir, true);
	const FString RecordPath =
		FPaths::Combine(Root.StateDir, TEXT("update-state.json"));
	TestTrue(TEXT("a directory blocks record replacement"),
		IFileManager::Get().MakeDirectory(*RecordPath, false));

	FWoTUpdateStateRecord State;
	State.KeyRevocationFloor = 17;
	State.NewestAnnouncementSequence = 23;
	FString Diagnostic;
	const FWoTBootstrapStatus Written =
		MergeAndWriteWoTUpdateState(Root, State, &Diagnostic);
	TestTrue(TEXT("an unwritable record reports a best-effort failure"),
		Written.IsError());
	TestEqual(TEXT("a failed write retains the in-memory ring floor"),
		State.KeyRevocationFloor, uint64(17));
	TestEqual(TEXT("a failed write retains the in-memory announcement maximum"),
		State.NewestAnnouncementSequence, uint64(23));
	TestFalse(TEXT("a failed write supplies a diagnostic"), Diagnostic.IsEmpty());
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
