// Program runner for the WoT.Update.PlatformHop loopback suite — the same
// --suite shape as the other standalone automation Program runners.
#include "RequiredProgramMainCPPInclude.h"

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"

IMPLEMENT_APPLICATION(WoTUpdateTransportsTests, "WoTUpdateTransportsTests");

namespace
{
bool WoTUpdateTransportsRunnerFindSuite(
	int32 ArgC, TCHAR* ArgV[], FString& OutSuite)
{
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		const FString Argument(ArgV[Index]);
		if (Argument == TEXT("--suite") && Index + 1 < ArgC)
		{
			OutSuite = ArgV[Index + 1];
			return true;
		}
		if (Argument.StartsWith(TEXT("--suite="), ESearchCase::CaseSensitive))
		{
			OutSuite = Argument.RightChop(8);
			return true;
		}
	}
	return false;
}

bool WoTUpdateTransportsRunnerMatches(
	const FString& FullPath, const FString& Prefix)
{
	return FullPath == Prefix
		|| FullPath.StartsWith(Prefix + TEXT("."), ESearchCase::CaseSensitive);
}

int32 WoTUpdateTransportsRunnerRun(const FString& Suite)
{
	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	Framework.SetRequestedTestFilter(
		EAutomationTestFlags::ProgramContext | EAutomationTestFlags::ProductFilter);
	TArray<FAutomationTestInfo> Available;
	Framework.GetValidTestNames(Available);
	int32 Selected = 0;
	int32 Failed = 0;
	for (const FAutomationTestInfo& Info : Available)
	{
		if (!WoTUpdateTransportsRunnerMatches(Info.GetFullTestPath(), Suite))
		{
			continue;
		}
		++Selected;
		Framework.StartTestByName(Info.GetTestName(), 0);
		while (!Framework.ExecuteLatentCommands())
		{
			FPlatformProcess::Sleep(0.0f);
		}
		FAutomationTestExecutionInfo Execution;
		const bool bPassed = Framework.StopTest(Execution);
		Failed += bPassed ? 0 : 1;
		UE_LOG(LogInit, Display, TEXT("%s %s"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), *Info.GetFullTestPath());
		for (const FAutomationExecutionEntry& Entry : Execution.GetEntries())
		{
			if (Entry.Event.Type == EAutomationEventType::Error)
			{
				UE_LOG(LogInit, Error, TEXT("  %s"), *Entry.Event.Message);
			}
		}
	}
	if (Selected == 0)
	{
		return 2;
	}
	UE_LOG(LogInit, Display, TEXT("%d/%d specs passed for '%s'"),
		Selected - Failed, Selected, *Suite);
	return Failed == 0 ? 0 : 1;
}
} // namespace

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	const int32 PreInit = GEngineLoop.PreInit(ArgC, ArgV);
	if (PreInit != 0)
	{
		return PreInit;
	}
	FString Suite;
	const int32 ExitCode = WoTUpdateTransportsRunnerFindSuite(ArgC, ArgV, Suite)
		&& !Suite.IsEmpty() ? WoTUpdateTransportsRunnerRun(Suite) : 2;
	if (ExitCode == 2)
	{
		UE_LOG(LogInit, Error,
			TEXT("usage: WoTUpdateTransportsTests --suite WoT.Update.PlatformHop"));
	}
	FEngineLoop::AppPreExit();
	FEngineLoop::AppExit();
	return ExitCode;
}
