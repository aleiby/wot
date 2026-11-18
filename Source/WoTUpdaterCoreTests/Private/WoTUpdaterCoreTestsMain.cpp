// Standalone automation runner. The selection and exit-code discipline mirrors
// WoTBootstrapCoreTests: matched success is 0, failures are 1, and CLI/filter
// misuse is 2.
#include "RequiredProgramMainCPPInclude.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"


IMPLEMENT_APPLICATION(WoTUpdaterCoreTests, "WoTUpdaterCoreTests");

namespace
{
bool WoTUpdaterFindOption(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name,
	FString& OutValue);

bool WoTUpdaterMatchesPrefix(const FString& FullTestPath, const FString& Prefix)
{
	return FullTestPath == Prefix
		|| FullTestPath.StartsWith(Prefix + TEXT("."), ESearchCase::CaseSensitive);
}

FString WoTUpdaterEscapeReportJsonString(const FString& Value)
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
			Escaped += FString::Printf(TEXT("\\u%04x"),
				static_cast<uint32>(Character));
		}
		else
		{
			Escaped.AppendChar(Character);
		}
	}
	return Escaped;
}

struct FWoTUpdaterSpecOutcome
{
	FString FullTestPath;
	FString DisplayName;
	bool bPassed = false;
	int32 Errors = 0;
	int32 Warnings = 0;
	double Duration = 0.0;
};

bool WoTUpdaterWriteAutomationReport(const FString& ReportPath,
	const TArray<FWoTUpdaterSpecOutcome>& Outcomes)
{
	int32 Succeeded = 0;
	int32 SucceededWithWarnings = 0;
	int32 Failed = 0;
	double TotalDuration = 0.0;
	for (const FWoTUpdaterSpecOutcome& Outcome : Outcomes)
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
	Json += TEXT("\t\"devices\": [{\"deviceName\": \"")
		+ WoTUpdaterEscapeReportJsonString(FPlatformProcess::ComputerName())
		+ TEXT("\", \"platform\": \"")
		+ WoTUpdaterEscapeReportJsonString(FPlatformProperties::PlatformName())
		+ TEXT("\"}],\n");
	Json += FString::Printf(TEXT("\t\"reportCreatedOn\": \"%s\",\n"),
		*FDateTime::UtcNow().ToString());
	Json += FString::Printf(TEXT("\t\"succeeded\": %d,\n"), Succeeded);
	Json += FString::Printf(TEXT("\t\"succeededWithWarnings\": %d,\n"),
		SucceededWithWarnings);
	Json += FString::Printf(TEXT("\t\"failed\": %d,\n"), Failed);
	Json += TEXT("\t\"notRun\": 0,\n\t\"inProcess\": 0,\n");
	Json += FString::Printf(TEXT("\t\"totalDuration\": %.3f,\n"), TotalDuration);
	Json += TEXT("\t\"comparisonExported\": false,\n");
	Json += TEXT("\t\"comparisonExportDirectory\": \"\",\n\t\"tests\": [\n");
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		const FWoTUpdaterSpecOutcome& Outcome = Outcomes[Index];
		Json += TEXT("\t\t{");
		Json += FString::Printf(TEXT("\"testDisplayName\": \"%s\", "),
			*WoTUpdaterEscapeReportJsonString(Outcome.DisplayName));
		Json += FString::Printf(TEXT("\"fullTestPath\": \"%s\", "),
			*WoTUpdaterEscapeReportJsonString(Outcome.FullTestPath));
		Json += FString::Printf(TEXT("\"state\": \"%s\", "),
			Outcome.bPassed ? TEXT("Success") : TEXT("Fail"));
		Json += FString::Printf(TEXT("\"duration\": %.3f, \"warnings\": %d, "
			"\"errors\": %d, \"entries\": []}"), Outcome.Duration,
			Outcome.Warnings, Outcome.Errors);
		Json += Index + 1 < Outcomes.Num() ? TEXT(",\n") : TEXT("\n");
	}
	Json += TEXT("\t]\n}\n");

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	return FFileHelper::SaveStringToFile(Json, *ReportPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

int32 WoTUpdaterRunSuite(const FString& FilterExpression,
	const FString& ReportPath)
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

	TArray<FWoTUpdaterSpecOutcome> Outcomes;
	TSet<FString> MatchedPrefixes;
	for (const FAutomationTestInfo& Info : Available)
	{
		const FString FullTestPath = Info.GetFullTestPath();
		bool bWanted = false;
		for (const FString& Prefix : Prefixes)
		{
			if (WoTUpdaterMatchesPrefix(FullTestPath, Prefix))
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

		FWoTUpdaterSpecOutcome Outcome;
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

	if (!ReportPath.IsEmpty()
		&& !WoTUpdaterWriteAutomationReport(ReportPath, Outcomes))
	{
		UE_LOG(LogWoTBootstrap, Error, TEXT("could not write report %s"),
			*ReportPath);
		return 2;
	}
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
			UE_LOG(LogWoTBootstrap, Error,
				TEXT("filter prefix '%s' selected no test"), *Prefix);
			return 2;
		}
	}

	int32 FailedCount = 0;
	for (const FWoTUpdaterSpecOutcome& Outcome : Outcomes)
	{
		FailedCount += Outcome.bPassed ? 0 : 1;
	}
	UE_LOG(LogWoTBootstrap, Display, TEXT("%d/%d specs passed for '%s'"),
		Outcomes.Num() - FailedCount, Outcomes.Num(), *FilterExpression);
	return FailedCount == 0 ? 0 : 1;
}

bool WoTUpdaterFindOption(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name,
	FString& OutValue)
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
}  // namespace

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	const int32 PreInitResult = GEngineLoop.PreInit(ArgC, ArgV);
	if (PreInitResult != 0)
	{
		return PreInitResult;
	}
	int32 ExitCode = 0;
	FString Filter;
	if (!WoTUpdaterFindOption(ArgC, ArgV, TEXT("suite"), Filter)
		|| Filter.IsEmpty())
	{
		UE_LOG(LogWoTBootstrap, Error,
			TEXT("usage: WoTUpdaterCoreTests --suite <filter>[+<filter>] "
				 "[--report <index.json>]"));
		ExitCode = 2;
	}
	else
	{
		FString ReportPath;
		WoTUpdaterFindOption(ArgC, ArgV, TEXT("report"), ReportPath);
		ExitCode = WoTUpdaterRunSuite(Filter, ReportPath);
	}

	FEngineLoop::AppPreExit();
	FEngineLoop::AppExit();
	return ExitCode;
}
