#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBootstrapHeartbeat.h"
#include "../WoTBootstrapViewModel.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr EAutomationTestFlags WoTBootstrapHeartbeatSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FString WoTBootstrapHeartbeatTestRoot(FStringView Label)
{
	const FString LabelText(Label);
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(),
		FString::Printf(TEXT("wot-bootstrap-heartbeat-%s-%s"),
			*LabelText,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatLineSpec,
	"WoT.Bootstrap.Flow.HeartbeatLine",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatLineSpec::RunTest(const FString&)
{
	TestEqual(TEXT("canonical heartbeat line"),
		WoTBootstrapHeartbeatLineForTesting(
			TEXT("progress"), EWoTBootstrapPhase::BuildingContent, 7,
			TOptional<uint64>(128), TOptional<uint64>(5462),
			TEXT("d"), TEXT("2026-07-30T22:00:00.000Z"), 4242),
		FString(TEXT("{\"completed\":128,\"detail\":\"d\",\"epoch\":7,")
			TEXT("\"event\":\"progress\",\"phase\":\"BuildingContent\",")
			TEXT("\"pid\":4242,\"total\":5462,")
			TEXT("\"ts_utc\":\"2026-07-30T22:00:00.000Z\"}\n")));

	TestEqual(TEXT("minimal heartbeat line"),
		WoTBootstrapHeartbeatLineForTesting(
			TEXT("phase-begin"), EWoTBootstrapPhase::EstablishingIdentity, 0,
			{}, {}, {}, TEXT("T"), 1),
		FString(TEXT("{\"epoch\":0,\"event\":\"phase-begin\",")
			TEXT("\"phase\":\"EstablishingIdentity\",\"pid\":1,")
			TEXT("\"ts_utc\":\"T\"}\n")));

	const FString Escaped = WoTBootstrapHeartbeatLineForTesting(
		TEXT("fail"), EWoTBootstrapPhase::Fatal, 1, {}, {},
		TEXT("a\"b\\c\nd"), TEXT("T"), 1);
	TestTrue(TEXT("detail quotes escaped"),
		Escaped.Contains(TEXT("\"detail\":\"a\\\"b\\\\c\\nd\""),
			ESearchCase::CaseSensitive));

	TestEqual(TEXT("phase token: CheckingGameUpdates"),
		FString(WoTBootstrapPhaseName(
			EWoTBootstrapPhase::CheckingGameUpdates)),
		FString(TEXT("CheckingGameUpdates")));
	TestEqual(TEXT("phase token: Ready"),
		FString(WoTBootstrapPhaseName(EWoTBootstrapPhase::Ready)),
		FString(TEXT("Ready")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatAppendSpec,
	"WoT.Bootstrap.Flow.HeartbeatAppend",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatAppendSpec::RunTest(const FString&)
{
	const FString Parent = WoTBootstrapHeartbeatTestRoot(TEXT("append"));
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("heartbeat fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};

	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	const FString FirstLeaf = WoTBootstrapHeartbeatFileName();
	const FString Prefix = FString::Printf(TEXT("bootstrap-heartbeat-v1.%u."),
		FPlatformProcess::GetCurrentProcessId());
	TestTrue(TEXT("run filename carries the process id"),
		FirstLeaf.StartsWith(Prefix, ESearchCase::CaseSensitive));
	TestTrue(TEXT("run filename has the JSONL suffix"),
		FirstLeaf.EndsWith(TEXT(".jsonl"), ESearchCase::CaseSensitive));
	const FString GuidText = FirstLeaf.Mid(
		Prefix.Len(), FirstLeaf.Len() - Prefix.Len() - 6);
	FGuid ParsedGuid;
	TestTrue(TEXT("run filename carries a GUID"),
		FGuid::ParseExact(GuidText, EGuidFormats::Digits, ParsedGuid));

	TestTrue(TEXT("first record appends"), WoTRecordBootstrapHeartbeat(
		TEXT("phase-begin"), EWoTBootstrapPhase::EstablishingIdentity, 0));
	TestTrue(TEXT("second record appends"), WoTRecordBootstrapHeartbeat(
		TEXT("completion"), EWoTBootstrapPhase::EstablishingIdentity, 0,
		{}, {}, TEXT("next=CheckingGameUpdates succeeded=1")));

	const FString FirstPath = FPaths::Combine(
		InstallRoot, TEXT("UpdaterState"), FirstLeaf);
	FString FirstContents;
	if (!TestTrue(TEXT("heartbeat log is readable while the writer lives"),
		FFileHelper::LoadFileToString(FirstContents, *FirstPath)))
	{
		return false;
	}
	TArray<FString> FirstLines;
	FirstContents.ParseIntoArrayLines(FirstLines);
	TestEqual(TEXT("two appends produce two records"), FirstLines.Num(), 2);
	TestTrue(TEXT("first record stays first"), FirstLines.Num() == 2
		&& FirstLines[0].Contains(TEXT("\"event\":\"phase-begin\""),
			ESearchCase::CaseSensitive));
	TestTrue(TEXT("second record stays second"), FirstLines.Num() == 2
		&& FirstLines[1].Contains(TEXT("\"event\":\"completion\""),
			ESearchCase::CaseSensitive));

	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	const FString SecondLeaf = WoTBootstrapHeartbeatFileName();
	TestNotEqual(TEXT("a test-root reset simulates a new process run"),
		SecondLeaf, FirstLeaf);
	TestTrue(TEXT("new run writes its own process file"),
		WoTRecordBootstrapHeartbeat(TEXT("phase-begin"),
			EWoTBootstrapPhase::CheckingGameUpdates, 1));
	FString SecondContents;
	TestTrue(TEXT("new run file is readable"),
		FFileHelper::LoadFileToString(SecondContents, *FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"), SecondLeaf)));
	TArray<FString> SecondLines;
	SecondContents.ParseIntoArrayLines(SecondLines);
	TestEqual(TEXT("new run starts with one record"), SecondLines.Num(), 1);
	FString FirstContentsAfterReset;
	TestTrue(TEXT("the prior run file remains readable"),
		FFileHelper::LoadFileToString(FirstContentsAfterReset, *FirstPath));
	TestEqual(TEXT("the new run does not append to the prior run"),
		FirstContentsAfterReset, FirstContents);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatOpenRetrySpec,
	"WoT.Bootstrap.Flow.HeartbeatOpenRetry",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatOpenRetrySpec::RunTest(const FString&)
{
	const FString Parent = WoTBootstrapHeartbeatTestRoot(TEXT("open-retry"));
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("open-retry fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};

	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	const FString HeartbeatPath = FPaths::Combine(
		InstallRoot, TEXT("UpdaterState"), WoTBootstrapHeartbeatFileName());
	FailNextWoTBootstrapHeartbeatOpenForTesting();
	TestFalse(TEXT("the injected first open failure is surfaced"),
		WoTRecordBootstrapHeartbeat(TEXT("phase-begin"),
			EWoTBootstrapPhase::EstablishingIdentity, 51));
	TestFalse(TEXT("an immediate record is suppressed during backoff"),
		WoTRecordBootstrapHeartbeat(TEXT("retry-too-soon"),
			EWoTBootstrapPhase::EstablishingIdentity, 51));
	FPlatformProcess::Sleep(1.1f);
	TestTrue(TEXT("a later record retries and lands"),
		WoTRecordBootstrapHeartbeat(TEXT("completion"),
			EWoTBootstrapPhase::EstablishingIdentity, 51));

	FString Contents;
	TestTrue(TEXT("the recovered heartbeat is readable"),
		FFileHelper::LoadFileToString(Contents, *HeartbeatPath));
	TestFalse(TEXT("the failed record never landed"),
		Contents.Contains(TEXT("\"event\":\"phase-begin\""),
			ESearchCase::CaseSensitive));
	TestTrue(TEXT("the retry record landed"),
		Contents.Contains(TEXT("\"event\":\"completion\""),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatTornTailSpec,
	"WoT.Bootstrap.Flow.HeartbeatTornTail",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatTornTailSpec::RunTest(const FString&)
{
	const FString Parent = WoTBootstrapHeartbeatTestRoot(TEXT("torn"));
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("torn-tail fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};

	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	const FString HeartbeatPath = FPaths::Combine(
		InstallRoot, TEXT("UpdaterState"), WoTBootstrapHeartbeatFileName());
	TestTrue(TEXT("baseline record appends"), WoTRecordBootstrapHeartbeat(
		TEXT("phase-begin"), EWoTBootstrapPhase::EstablishingIdentity, 0));
	const FString Fragment(TEXT("{\"event\":\"torn"));
	if (!TestTrue(TEXT("a failed-write fragment is present"),
		FFileHelper::SaveStringToFile(Fragment, *HeartbeatPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_Append)))
	{
		return false;
	}

	TestTrue(TEXT("the next record appends after the fragment"),
		WoTRecordBootstrapHeartbeat(TEXT("completion"),
			EWoTBootstrapPhase::CheckingGameUpdates, 1));
	FString Contents;
	if (!TestTrue(TEXT("repaired heartbeat is readable"),
		FFileHelper::LoadFileToString(Contents, *HeartbeatPath)))
	{
		return false;
	}
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines);
	TestEqual(TEXT("the fragment and following record occupy separate lines"),
		Lines.Num(), 3);
	TestTrue(TEXT("the failed fragment is preserved for diagnosis"),
		Lines.Num() == 3 && Lines[1] == Fragment);
	TestTrue(TEXT("the following record is intact"), Lines.Num() == 3
		&& Lines[2].Contains(TEXT("\"event\":\"completion\""),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatRetentionSpec,
	"WoT.Bootstrap.Flow.HeartbeatRetention",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatRetentionSpec::RunTest(const FString&)
{
	const FString Parent = WoTBootstrapHeartbeatTestRoot(TEXT("retention"));
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	const FString StateRoot = FPaths::Combine(InstallRoot, TEXT("UpdaterState"));
	if (!TestTrue(TEXT("retention fixture state root is created"),
		IFileManager::Get().MakeDirectory(*StateRoot, true)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
	};

	TArray<FString> OldLeaves;
	for (int32 Index = 0; Index < 21; ++Index)
	{
		const FString Leaf = FString::Printf(
			TEXT("bootstrap-heartbeat-v1.%d.%s.jsonl"), 1000 + Index,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
		OldLeaves.Add(Leaf);
		if (!TestTrue(TEXT("old process file is created"),
			FFileHelper::SaveStringToFile(TEXT("old\n"),
				*FPaths::Combine(StateRoot, Leaf),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
		{
			return false;
		}
		FPlatformProcess::Sleep(0.005f);
	}

	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	const FString CurrentLeaf = WoTBootstrapHeartbeatFileName();
	TestTrue(TEXT("current run writes while pruning old diagnostics"),
		WoTRecordBootstrapHeartbeat(TEXT("phase-begin"),
			EWoTBootstrapPhase::EstablishingIdentity, 0));
	TArray<FString> Retained;
	IFileManager::Get().FindFiles(Retained,
		*FPaths::Combine(StateRoot, TEXT("bootstrap-heartbeat-v1.*.jsonl")),
		true, false);
	TestEqual(TEXT("at most twenty process files remain"), Retained.Num(), 20);
	TestFalse(TEXT("the oldest process file is removed"),
		IFileManager::Get().FileExists(*FPaths::Combine(StateRoot, OldLeaves[0])));
	TestTrue(TEXT("a recent prior process file remains"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			StateRoot, OldLeaves.Last())));
	TestTrue(TEXT("the current process file is always protected"),
		IFileManager::Get().FileExists(*FPaths::Combine(StateRoot, CurrentLeaf)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapHeartbeatDecoyFilesSpec,
	"WoT.Bootstrap.Flow.HeartbeatDecoyFiles",
	WoTBootstrapHeartbeatSpecFlags)

bool FWoTBootstrapHeartbeatDecoyFilesSpec::RunTest(const FString&)
{
	const FString Root = WoTBootstrapHeartbeatTestRoot(TEXT("decoys"));
	const FString StateRoot = FPaths::Combine(Root, TEXT("UpdaterState"));
	if (!TestTrue(TEXT("decoy fixture exists"),
		IFileManager::Get().MakeDirectory(*StateRoot, true))) return false;
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	const TArray<FString> Decoys = {
		TEXT("bootstrap-heartbeat-v1.backup.jsonl"),
		TEXT("bootstrap-heartbeat-v1.0.0123456789abcdef0123456789abcdef.jsonl"),
		TEXT("bootstrap-heartbeat-v1.-1.0123456789abcdef0123456789abcdef.jsonl"),
		TEXT("bootstrap-heartbeat-v1.01.0123456789abcdef0123456789abcdef.jsonl"),
		TEXT("bootstrap-heartbeat-v1.1.0123456789abcdef0123456789abcdeg.jsonl"),
		TEXT("bootstrap-heartbeat-v1.1.0123456789abcdef0123456789abcde.jsonl"),
		TEXT("bootstrap-heartbeat-v1.1.0123456789abcdef0123456789abcdef0.jsonl"),
		TEXT("bootstrap-heartbeat-v1.1.extra.0123456789abcdef0123456789abcdef.jsonl"),
		TEXT("bootstrap-heartbeat-v1.1.0123456789abcdef0123456789abcdef.jsonl.bak"),
		TEXT("bootstrap-heartbeat-v1.4294967296.0123456789abcdef0123456789abcdef.jsonl")};
	for (const FString& Leaf : Decoys)
		TestTrue(TEXT("decoy written"), FFileHelper::SaveStringToFile(
			TEXT("sentinel\n"), *FPaths::Combine(StateRoot, Leaf)));
	FPlatformProcess::Sleep(0.01f);
	for (int32 Index = 1; Index <= 22; ++Index)
		TestTrue(TEXT("process file written"), FFileHelper::SaveStringToFile(
			TEXT("old\n"), *FPaths::Combine(StateRoot, FString::Printf(
				TEXT("bootstrap-heartbeat-v1.%d.0123456789abcdef0123456789abcdef.jsonl"), Index))));
	SetWoTBootstrapHeartbeatRootForTesting(Root);
	TestTrue(TEXT("retention runs"), WoTRecordBootstrapHeartbeat(
		TEXT("phase-begin"), EWoTBootstrapPhase::EstablishingIdentity, 0));
	for (const FString& Leaf : Decoys)
	{
		FString Contents;
		TestTrue(*Leaf, FFileHelper::LoadFileToString(Contents,
			*FPaths::Combine(StateRoot, Leaf)));
		TestEqual(TEXT("decoy content preserved"), Contents, FString(TEXT("sentinel\n")));
	}
	return !HasAnyErrors();
}

#endif
