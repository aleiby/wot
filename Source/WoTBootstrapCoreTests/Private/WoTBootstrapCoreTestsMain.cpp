// Standalone runner for the WoTBootstrapCore specs and the round-trip protocol.
//
// Two modes, one binary:
//
//   --suite <filter>[+<filter>...]  run the matching automation specs, optionally
//                                   exporting a UE-shaped index.json for
//                                   Tools/dist/validate_automation_report.py
//   --artifact-roundtrip            serve the length-prefixed protocol that
//                                   Tools/dist/artifact_roundtrip.py drives
//
// The runner exits non-zero when a spec fails AND when a filter selects nothing:
// an empty selection is the failure mode a bare exit code cannot distinguish from
// success, and it is exactly how a renamed test silently stops being run.
#include "RequiredProgramMainCPPInclude.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ProfileSession.h"

#if PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#define WOT_DUP _dup
#define WOT_DUP2 _dup2
#define WOT_READ _read
#define WOT_WRITE _write
#else
#include <unistd.h>
#define WOT_DUP dup
#define WOT_DUP2 dup2
#define WOT_READ read
#define WOT_WRITE write
#endif

IMPLEMENT_APPLICATION(WoTBootstrapCoreTests, "WoTBootstrapCoreTests");

namespace
{
constexpr uint8 OpcodeEvaluate = 0;
constexpr uint8 OpcodeShutdown = 1;
constexpr uint8 StatusAccepted = 0;
constexpr uint8 StatusRejected = 1;

/**
 * A private duplicate of the process's original stdout.
 *
 * The engine writes log lines to stdout, and a single stray line would be read as
 * protocol bytes by the driver. So the very first thing main does is duplicate
 * fd 1, then point fd 1 at stderr; frames go to this descriptor and logging goes
 * somewhere harmless.
 */
int32 GProtocolOut = -1;

bool ReadExactly(uint8* Buffer, int32 Count)
{
	int32 Read = 0;
	while (Read < Count)
	{
		const int32 Chunk = static_cast<int32>(
			WOT_READ(0, Buffer + Read, static_cast<unsigned int>(Count - Read)));
		if (Chunk <= 0)
		{
			return false;
		}
		Read += Chunk;
	}
	return true;
}

bool WriteExactly(const uint8* Buffer, int32 Count)
{
	int32 Written = 0;
	while (Written < Count)
	{
		const int32 Chunk = static_cast<int32>(
			WOT_WRITE(GProtocolOut, Buffer + Written,
				static_cast<unsigned int>(Count - Written)));
		if (Chunk <= 0)
		{
			return false;
		}
		Written += Chunk;
	}
	return true;
}

bool Respond(uint8 Status, TConstArrayView<uint8> Payload)
{
	uint8 Header[5];
	Header[0] = Status;
	const uint32 Length = static_cast<uint32>(Payload.Num());
	FMemory::Memcpy(Header + 1, &Length, sizeof(Length));
	if (!WriteExactly(Header, sizeof(Header)))
	{
		return false;
	}
	return Payload.Num() == 0 || WriteExactly(Payload.GetData(), Payload.Num());
}

/** Serve evaluate/shutdown requests until stdin ends or a write fails. */
int32 RunArtifactRoundTrip()
{
#if PLATFORM_WINDOWS
	// Without binary mode the CRT would translate CRLF inside framed payloads.
	_setmode(0, _O_BINARY);
	_setmode(GProtocolOut, _O_BINARY);
#endif

	for (;;)
	{
		uint8 Header[5];
		if (!ReadExactly(Header, sizeof(Header)))
		{
			// Driver closed stdin without a shutdown request; nothing left to do.
			return 0;
		}
		uint32 Length = 0;
		FMemory::Memcpy(&Length, Header + 1, sizeof(Length));
		if (Header[0] == OpcodeShutdown)
		{
			return 0;
		}
		if (Header[0] != OpcodeEvaluate)
		{
			return 2;
		}

		TArray<uint8> Candidate;
		Candidate.SetNumUninitialized(static_cast<int32>(Length));
		if (Length > 0 && !ReadExactly(Candidate.GetData(), Candidate.Num()))
		{
			return 2;
		}

		TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Candidate);
		if (Parsed.IsError())
		{
			const FString Text = Parsed.GetError().ToString();
			const FTCHARToUTF8 Utf8(*Text);
			const TConstArrayView<uint8> Payload(
				reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			if (!Respond(StatusRejected, Payload))
			{
				return 2;
			}
			continue;
		}

		TWoTBootstrapResult<TArray<uint8>> Written = WriteCanonicalJson(Parsed.GetValue());
		if (Written.IsError())
		{
			// Accepted but not re-emittable is a contract violation, not a verdict
			// about the candidate. Report it as a rejection so the driver shows the
			// exact code instead of hanging on a missing frame.
			const FString Text = Written.GetError().ToString();
			const FTCHARToUTF8 Utf8(*Text);
			const TConstArrayView<uint8> Payload(
				reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			if (!Respond(StatusRejected, Payload))
			{
				return 2;
			}
			continue;
		}

		if (!Respond(StatusAccepted, Written.GetValue()))
		{
			return 2;
		}
	}
}

/** UE filter semantics: the exact path, or a dot-delimited descendant of it. */
bool MatchesPrefix(const FString& FullTestPath, const FString& Prefix)
{
	return FullTestPath == Prefix || FullTestPath.StartsWith(Prefix + TEXT("."), ESearchCase::CaseSensitive);
}

// Not "EscapeJsonString": Json/Public/Serialization/JsonWriter.h declares a
// global function of that name that reaches this TU transitively on some
// engine include sets, and the unqualified call is then ambiguous on MSVC.
FString EscapeReportJsonString(const FString& Value)
{
	FString Escaped;
	Escaped.Reserve(Value.Len() + 2);
	for (const TCHAR Character : Value)
	{
		if (Character == TEXT('"') || Character == TEXT('\\'))
		{
			Escaped.AppendChar(TEXT('\\'));
			Escaped.AppendChar(Character);
		}
		else if (Character < 0x20 || Character > 0x7E)
		{
			Escaped += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
		}
		else
		{
			Escaped.AppendChar(Character);
		}
	}
	return Escaped;
}

struct FSpecOutcome
{
	FString FullTestPath;
	FString DisplayName;
	bool bPassed = false;
	int32 Errors = 0;
	int32 Warnings = 0;
	double Duration = 0.0;
};

/**
 * Write the same shape UE's `-ReportExportPath` produces, so one validator gates
 * both editor runs and this Program runner. Only the fields the validator reads
 * are populated; the rest exist to keep the document schema-shaped.
 */
bool WriteAutomationReport(const FString& ReportPath, const TArray<FSpecOutcome>& Outcomes)
{
	int32 Succeeded = 0;
	int32 SucceededWithWarnings = 0;
	int32 Failed = 0;
	double TotalDuration = 0.0;
	for (const FSpecOutcome& Outcome : Outcomes)
	{
		TotalDuration += Outcome.Duration;
		if (!Outcome.bPassed)
		{
			++Failed;
		}
		else if (Outcome.Warnings > 0)
		{
			++SucceededWithWarnings;
		}
		else
		{
			++Succeeded;
		}
	}

	FString Json;
	Json += TEXT("{\n");
	Json += TEXT("\t\"devices\": [{\"deviceName\": \"") + EscapeReportJsonString(FPlatformProcess::ComputerName())
		+ TEXT("\", \"platform\": \"") + EscapeReportJsonString(FPlatformProperties::PlatformName()) + TEXT("\"}],\n");
	Json += FString::Printf(TEXT("\t\"reportCreatedOn\": \"%s\",\n"), *FDateTime::UtcNow().ToString());
	Json += FString::Printf(TEXT("\t\"succeeded\": %d,\n"), Succeeded);
	Json += FString::Printf(TEXT("\t\"succeededWithWarnings\": %d,\n"), SucceededWithWarnings);
	Json += FString::Printf(TEXT("\t\"failed\": %d,\n"), Failed);
	Json += TEXT("\t\"notRun\": 0,\n");
	Json += TEXT("\t\"inProcess\": 0,\n");
	Json += FString::Printf(TEXT("\t\"totalDuration\": %.3f,\n"), TotalDuration);
	Json += TEXT("\t\"comparisonExported\": false,\n");
	Json += TEXT("\t\"comparisonExportDirectory\": \"\",\n");
	Json += TEXT("\t\"tests\": [\n");
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		const FSpecOutcome& Outcome = Outcomes[Index];
		Json += TEXT("\t\t{");
		Json += FString::Printf(TEXT("\"testDisplayName\": \"%s\", "), *EscapeReportJsonString(Outcome.DisplayName));
		Json += FString::Printf(TEXT("\"fullTestPath\": \"%s\", "), *EscapeReportJsonString(Outcome.FullTestPath));
		Json += FString::Printf(TEXT("\"state\": \"%s\", "), Outcome.bPassed ? TEXT("Success") : TEXT("Fail"));
		Json += FString::Printf(TEXT("\"duration\": %.3f, "), Outcome.Duration);
		Json += FString::Printf(TEXT("\"warnings\": %d, "), Outcome.Warnings);
		Json += FString::Printf(TEXT("\"errors\": %d, "), Outcome.Errors);
		Json += TEXT("\"entries\": []}");
		Json += (Index + 1 < Outcomes.Num()) ? TEXT(",\n") : TEXT("\n");
	}
	Json += TEXT("\t]\n}\n");

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	return FFileHelper::SaveStringToFile(Json, *ReportPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

int32 RunSuite(const FString& FilterExpression, const FString& ReportPath)
{
	TArray<FString> Prefixes;
	FilterExpression.ParseIntoArray(Prefixes, TEXT("+"), true);
	for (FString& Prefix : Prefixes)
	{
		Prefix.TrimStartAndEndInline();
	}
	Prefixes.RemoveAll([](const FString& Prefix) { return Prefix.IsEmpty(); });
	if (Prefixes.Num() == 0)
	{
		UE_LOG(LogWoTBootstrap, Error, TEXT("--suite needs at least one filter prefix"));
		return 2;
	}

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	Framework.SetRequestedTestFilter(
		EAutomationTestFlags::ProgramContext | EAutomationTestFlags::ProductFilter);

	TArray<FAutomationTestInfo> Available;
	Framework.GetValidTestNames(Available);

	TArray<FSpecOutcome> Outcomes;
	TSet<FString> MatchedPrefixes;
	for (const FAutomationTestInfo& Info : Available)
	{
		const FString FullTestPath = Info.GetFullTestPath();
		bool bWanted = false;
		for (const FString& Prefix : Prefixes)
		{
			if (MatchesPrefix(FullTestPath, Prefix))
			{
				MatchedPrefixes.Add(Prefix);
				bWanted = true;
			}
		}
		if (!bWanted)
		{
			continue;
		}

		const double Started = FPlatformTime::Seconds();
		Framework.StartTestByName(Info.GetTestName(), 0);
		while (!Framework.ExecuteLatentCommands())
		{
			FPlatformProcess::Sleep(0.0f);
		}
		FAutomationTestExecutionInfo ExecutionInfo;
		const bool bPassed = Framework.StopTest(ExecutionInfo);

		FSpecOutcome Outcome;
		Outcome.FullTestPath = FullTestPath;
		Outcome.DisplayName = Info.GetDisplayName();
		Outcome.bPassed = bPassed;
		Outcome.Errors = ExecutionInfo.GetErrorTotal();
		Outcome.Warnings = ExecutionInfo.GetWarningTotal();
		Outcome.Duration = FPlatformTime::Seconds() - Started;
		Outcomes.Add(Outcome);

		UE_LOG(LogWoTBootstrap, Display, TEXT("%s %s (%d errors)"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), *FullTestPath, Outcome.Errors);
		for (const FAutomationExecutionEntry& Entry : ExecutionInfo.GetEntries())
		{
			if (Entry.Event.Type == EAutomationEventType::Error)
			{
				UE_LOG(LogWoTBootstrap, Error, TEXT("  %s"), *Entry.Event.Message);
			}
		}
	}

	if (!ReportPath.IsEmpty() && !WriteAutomationReport(ReportPath, Outcomes))
	{
		UE_LOG(LogWoTBootstrap, Error, TEXT("could not write report %s"), *ReportPath);
		return 2;
	}

	// An empty selection is a failure, not a pass: it is what a renamed or
	// silently-excluded spec looks like from the outside.
	if (Outcomes.Num() == 0)
	{
		UE_LOG(LogWoTBootstrap, Error,
			TEXT("filter '%s' selected no test out of %d discovered"),
			*FilterExpression, Available.Num());
		return 2;
	}
	for (const FString& Prefix : Prefixes)
	{
		if (!MatchedPrefixes.Contains(Prefix))
		{
			UE_LOG(LogWoTBootstrap, Error, TEXT("filter prefix '%s' selected no test"), *Prefix);
			return 2;
		}
	}

	int32 FailedCount = 0;
	for (const FSpecOutcome& Outcome : Outcomes)
	{
		FailedCount += Outcome.bPassed ? 0 : 1;
	}
	UE_LOG(LogWoTBootstrap, Display, TEXT("%d/%d specs passed for '%s'"),
		Outcomes.Num() - FailedCount, Outcomes.Num(), *FilterExpression);
	return FailedCount == 0 ? 0 : 1;
}

/** Read `--name value` or `--name=value` out of the raw argument vector. */
bool FindOption(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name, FString& OutValue)
{
	const FString Flag = FString(TEXT("--")) + Name;
	const FString FlagEquals = Flag + TEXT("=");
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		const FString Argument = ArgV[Index];
		if (Argument == Flag)
		{
			if (Index + 1 < ArgC)
			{
				OutValue = ArgV[Index + 1];
				return true;
			}
			OutValue.Empty();
			return true;
		}
		if (Argument.StartsWith(FlagEquals, ESearchCase::CaseSensitive))
		{
			OutValue = Argument.RightChop(FlagEquals.Len());
			return true;
		}
	}
	return false;
}

bool HasFlag(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name)
{
	const FString Flag = FString(TEXT("--")) + Name;
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		if (FString(ArgV[Index]) == Flag)
		{
			return true;
		}
	}
	return false;
}

/** Hold the same root-level SHARED profile session as a game process. */
int32 RunProfileSessionProbe(const FString& DataRoot, const FString& ReadyPath,
	const FString& ReleasePath)
{
	if (DataRoot.IsEmpty() || ReadyPath.IsEmpty() || ReleasePath.IsEmpty()) return 2;
	auto Session = AcquireWoTProfileSessionShared(DataRoot);
	if (Session.IsError()) return 2;
	TArray<uint8> Manifest;
	if (!FFileHelper::LoadFileToArray(Manifest,
		*FPaths::Combine(DataRoot, TEXT("ContentCache"), TEXT("current"),
			TEXT("manifest.json")))
		|| Manifest.IsEmpty()) return 2;
	if (!FFileHelper::SaveStringToFile(TEXT("ready"), *ReadyPath)) return 2;
	const double Deadline = FPlatformTime::Seconds() + 20.0;
	while (!FPaths::FileExists(ReleasePath))
	{
		if (FPlatformTime::Seconds() >= Deadline) return 2;
		FPlatformProcess::Sleep(0.01f);
	}
	return 0;
}

/** Model the first guarded child access: acquire SHARED before touching Data. */
int32 RunProfileSessionStartupProbe(
	const FString& DataRoot, const FString& AccessedPath)
{
	if (DataRoot.IsEmpty() || AccessedPath.IsEmpty()) return 2;
	auto Session = AcquireWoTProfileSessionShared(DataRoot);
	if (Session.IsError()) return 1;
	return FFileHelper::SaveStringToFile(TEXT("accessed"), *AccessedPath)
		? 0 : 2;
}
}  // namespace

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	// Before the engine can log a single line: take a private handle on the real
	// stdout and send fd 1 to stderr.
	GProtocolOut = WOT_DUP(1);
	if (GProtocolOut >= 0)
	{
		WOT_DUP2(2, 1);
	}

	const int32 PreInitResult = GEngineLoop.PreInit(ArgC, ArgV);
	if (PreInitResult != 0)
	{
		return PreInitResult;
	}

	int32 ExitCode = 0;
	FString DataRoot;
	if (FindOption(ArgC, ArgV, TEXT("profile-session-startup-probe"), DataRoot))
	{
		FString AccessedPath;
		FindOption(ArgC, ArgV, TEXT("accessed"), AccessedPath);
		ExitCode = RunProfileSessionStartupProbe(DataRoot, AccessedPath);
	}
	else if (FindOption(ArgC, ArgV, TEXT("profile-session-probe"), DataRoot))
	{
		FString ReadyPath;
		FString ReleasePath;
		FindOption(ArgC, ArgV, TEXT("ready"), ReadyPath);
		FindOption(ArgC, ArgV, TEXT("release"), ReleasePath);
		ExitCode = RunProfileSessionProbe(DataRoot, ReadyPath, ReleasePath);
	}
	else if (HasFlag(ArgC, ArgV, TEXT("artifact-roundtrip")))
	{
		if (GProtocolOut < 0)
		{
			ExitCode = 2;
		}
		else
		{
			ExitCode = RunArtifactRoundTrip();
		}
	}
	else
	{
		FString Filter;
		if (!FindOption(ArgC, ArgV, TEXT("suite"), Filter) || Filter.IsEmpty())
		{
			UE_LOG(LogWoTBootstrap, Error,
				TEXT("usage: WoTBootstrapCoreTests --suite <filter>[+<filter>] "
					 "[--report <index.json>] | --artifact-roundtrip"));
			ExitCode = 2;
		}
		else
		{
			FString ReportPath;
			FindOption(ArgC, ArgV, TEXT("report"), ReportPath);
			ExitCode = RunSuite(Filter, ReportPath);
		}
	}

	FEngineLoop::AppPreExit();
	FEngineLoop::AppExit();
	return ExitCode;
}
