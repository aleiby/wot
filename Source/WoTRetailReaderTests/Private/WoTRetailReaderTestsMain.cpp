// Standalone automation and canonical contract round-trip runner.
#include "RequiredProgramMainCPPInclude.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/Contract.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/KeyRing.h"

#if PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#define WOT_RETAIL_DUP _dup
#define WOT_RETAIL_DUP2 _dup2
#define WOT_RETAIL_READ _read
#define WOT_RETAIL_WRITE _write
#else
#include <unistd.h>
#define WOT_RETAIL_DUP dup
#define WOT_RETAIL_DUP2 dup2
#define WOT_RETAIL_READ read
#define WOT_RETAIL_WRITE write
#endif

IMPLEMENT_APPLICATION(WoTRetailReaderTests, "WoTRetailReaderTests");

namespace
{
constexpr uint8 RetailOpcodeEvaluate = 0;
constexpr uint8 RetailOpcodeShutdown = 1;
constexpr uint8 RetailOpcodeEvaluateContractPayload = 2;
constexpr uint8 RetailOpcodeEvaluateSignedContract = 3;
constexpr uint8 RetailStatusAccepted = 0;
constexpr uint8 RetailStatusRejected = 1;
int32 GRetailProtocolOut = -1;

bool RetailReadExactly(uint8* Buffer, int32 Count)
{
	int32 TotalRead = 0;
	while (TotalRead < Count)
	{
		const int32 Chunk = static_cast<int32>(WOT_RETAIL_READ(0, Buffer + TotalRead,
			static_cast<unsigned int>(Count - TotalRead)));
		if (Chunk <= 0)
		{
			return false;
		}
		TotalRead += Chunk;
	}
	return true;
}

bool RetailWriteExactly(const uint8* Buffer, int32 Count)
{
	int32 TotalWritten = 0;
	while (TotalWritten < Count)
	{
		const int32 Chunk = static_cast<int32>(WOT_RETAIL_WRITE(GRetailProtocolOut,
			Buffer + TotalWritten, static_cast<unsigned int>(Count - TotalWritten)));
		if (Chunk <= 0)
		{
			return false;
		}
		TotalWritten += Chunk;
	}
	return true;
}

bool RetailRespond(uint8 Status, TConstArrayView<uint8> Payload)
{
	uint8 Header[5];
	Header[0] = Status;
	const uint32 Length = static_cast<uint32>(Payload.Num());
	FMemory::Memcpy(Header + 1, &Length, sizeof(Length));
	return RetailWriteExactly(Header, sizeof(Header))
		&& (Payload.Num() == 0 || RetailWriteExactly(Payload.GetData(), Payload.Num()));
}

bool RetailReject(const FWoTBootstrapError& Error)
{
	const FString Text = Error.ToString();
	const FTCHARToUTF8 Utf8(*Text);
	return RetailRespond(RetailStatusRejected, TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

bool RetailRejectContract(const FWoTBootstrapError& Error)
{
	const FString Text = FString(LexToString(Error.Code)) + TEXT(": ") + Error.Path;
	const FTCHARToUTF8 Utf8(*Text);
	return RetailRespond(RetailStatusRejected, TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FWoTKeyRing RetailSyntheticContractRing()
{
	FWoTKeyRing Ring;
	Ring.Format = TEXT("wot-release-key-ring-v1");
	Ring.Epoch = 1;
	FWoTReleaseKey Key;
	Key.KeyId = TEXT("cache-contract-test");
	Key.FirstSequence = 0;
	Key.LastSequence = MAX_uint64;
	TWoTBootstrapResult<FWoTEd25519PublicKey> PublicKey =
		FWoTEd25519PublicKey::FromHex(
			TEXT("b5128532471f945652bbb0dd21a322bfdf814165b62f29caf59770926e346026"));
	if (!PublicKey.IsError())
	{
		Key.PublicKey = PublicKey.GetValue();
	}
	Ring.Keys.Add(MoveTemp(Key));
	return Ring;
}

int32 RunRetailArtifactRoundTrip()
{
#if PLATFORM_WINDOWS
	_setmode(0, _O_BINARY);
	_setmode(GRetailProtocolOut, _O_BINARY);
#endif
	for (;;)
	{
		uint8 Header[5];
		if (!RetailReadExactly(Header, sizeof(Header)))
		{
			return 0;
		}
		uint32 Length = 0;
		FMemory::Memcpy(&Length, Header + 1, sizeof(Length));
		if (Header[0] == RetailOpcodeShutdown)
		{
			return 0;
		}
		const uint8 Opcode = Header[0];
		if ((Opcode != RetailOpcodeEvaluate
				&& Opcode != RetailOpcodeEvaluateContractPayload
				&& Opcode != RetailOpcodeEvaluateSignedContract)
			|| Length > 4 * 1024 * 1024 + 1)
		{
			return 2;
		}
		TArray<uint8> Candidate;
		Candidate.SetNumUninitialized(static_cast<int32>(Length));
		if (Length > 0 && !RetailReadExactly(Candidate.GetData(), Candidate.Num()))
		{
			return 2;
		}
		if (Opcode == RetailOpcodeEvaluateContractPayload)
		{
			TWoTBootstrapResult<FWoTRetailContract> Contract =
				ParseRetailContractPayload(Candidate);
			if (Contract.IsError())
			{
				if (!RetailRejectContract(Contract.GetError()))
				{
					return 2;
				}
				continue;
			}
		}
		else if (Opcode == RetailOpcodeEvaluateSignedContract)
		{
			FWoTKeyRing Ring = RetailSyntheticContractRing();
			TWoTBootstrapResult<FWoTRetailContract> Contract =
				ParseRetailContract(Candidate, Ring);
			if (Contract.IsError())
			{
				if (!RetailRejectContract(Contract.GetError()))
				{
					return 2;
				}
				continue;
			}
		}
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Candidate);
		if (Parsed.IsError())
		{
			if (!RetailReject(Parsed.GetError()))
			{
				return 2;
			}
			continue;
		}
		TWoTBootstrapResult<TArray<uint8>> Written = WriteCanonicalJson(Parsed.GetValue());
		if (Written.IsError())
		{
			if (!RetailReject(Written.GetError()))
			{
				return 2;
			}
			continue;
		}
		if (!RetailRespond(RetailStatusAccepted, Written.GetValue()))
		{
			return 2;
		}
	}
}

bool RetailMatchesPrefix(const FString& FullPath, const FString& Prefix)
{
	return FullPath == Prefix
		|| FullPath.StartsWith(Prefix + TEXT("."), ESearchCase::CaseSensitive);
}

FString RetailEscapeReportString(const FString& Value)
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
		else if (Character < 0x20 || Character > 0x7e)
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

struct FRetailSpecOutcome
{
	FString FullTestPath;
	FString DisplayName;
	bool bPassed = false;
	int32 Errors = 0;
	int32 Warnings = 0;
	double Duration = 0.0;
};

bool WriteRetailAutomationReport(
	const FString& ReportPath, const TArray<FRetailSpecOutcome>& Outcomes)
{
	int32 Succeeded = 0;
	int32 SucceededWithWarnings = 0;
	int32 Failed = 0;
	double TotalDuration = 0.0;
	for (const FRetailSpecOutcome& Outcome : Outcomes)
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
		+ RetailEscapeReportString(FPlatformProcess::ComputerName())
		+ TEXT("\", \"platform\": \"")
		+ RetailEscapeReportString(FPlatformProperties::PlatformName()) + TEXT("\"}],\n");
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
	for (int32 OutcomeIndex = 0; OutcomeIndex < Outcomes.Num(); ++OutcomeIndex)
	{
		const FRetailSpecOutcome& Outcome = Outcomes[OutcomeIndex];
		Json += TEXT("\t\t{");
		Json += FString::Printf(TEXT("\"testDisplayName\": \"%s\", "),
			*RetailEscapeReportString(Outcome.DisplayName));
		Json += FString::Printf(TEXT("\"fullTestPath\": \"%s\", "),
			*RetailEscapeReportString(Outcome.FullTestPath));
		Json += FString::Printf(TEXT("\"state\": \"%s\", "),
			Outcome.bPassed ? TEXT("Success") : TEXT("Fail"));
		Json += FString::Printf(TEXT("\"duration\": %.3f, \"warnings\": %d, "
			"\"errors\": %d, \"entries\": []}"), Outcome.Duration,
			Outcome.Warnings, Outcome.Errors);
		Json += (OutcomeIndex + 1 < Outcomes.Num()) ? TEXT(",\n") : TEXT("\n");
	}
	Json += TEXT("\t]\n}\n");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	return FFileHelper::SaveStringToFile(Json, *ReportPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

int32 RunRetailSuite(const FString& FilterExpression, const FString& ReportPath)
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
	TArray<FRetailSpecOutcome> Outcomes;
	TSet<FString> MatchedPrefixes;
	for (const FAutomationTestInfo& Info : Available)
	{
		const FString FullTestPath = Info.GetFullTestPath();
		bool bWanted = false;
		for (const FString& Prefix : Prefixes)
		{
			if (RetailMatchesPrefix(FullTestPath, Prefix))
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
		FRetailSpecOutcome Outcome;
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
	if (!ReportPath.IsEmpty() && !WriteRetailAutomationReport(ReportPath, Outcomes))
	{
		UE_LOG(LogWoTBootstrap, Error, TEXT("could not write report %s"), *ReportPath);
		return 2;
	}
	if (Outcomes.Num() == 0)
	{
		UE_LOG(LogWoTBootstrap, Error, TEXT("filter '%s' selected no test out of %d"),
			*FilterExpression, Available.Num());
		return 2;
	}
	for (const FString& Prefix : Prefixes)
	{
		if (!MatchedPrefixes.Contains(Prefix))
		{
			UE_LOG(LogWoTBootstrap, Error, TEXT("filter prefix '%s' selected no test"),
				*Prefix);
			return 2;
		}
	}
	int32 FailedCount = 0;
	for (const FRetailSpecOutcome& Outcome : Outcomes)
	{
		FailedCount += Outcome.bPassed ? 0 : 1;
	}
	UE_LOG(LogWoTBootstrap, Display, TEXT("%d/%d specs passed for '%s'"),
		Outcomes.Num() - FailedCount, Outcomes.Num(), *FilterExpression);
	return FailedCount == 0 ? 0 : 1;
}

bool FindRetailOption(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name, FString& OutValue)
{
	const FString Flag = FString(TEXT("--")) + Name;
	const FString FlagEquals = Flag + TEXT("=");
	for (int32 ArgIndex = 1; ArgIndex < ArgC; ++ArgIndex)
	{
		const FString Argument = ArgV[ArgIndex];
		if (Argument == Flag)
		{
			OutValue = ArgIndex + 1 < ArgC ? ArgV[ArgIndex + 1] : TEXT("");
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

bool HasRetailFlag(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name)
{
	const FString Flag = FString(TEXT("--")) + Name;
	for (int32 ArgIndex = 1; ArgIndex < ArgC; ++ArgIndex)
	{
		if (FString(ArgV[ArgIndex]) == Flag)
		{
			return true;
		}
	}
	return false;
}

bool ValidateRetailProductionDescriptor(const FString& DescriptorPath)
{
	TArray<uint8> Bytes;
	if (DescriptorPath.IsEmpty()
		|| !FFileHelper::LoadFileToArray(Bytes, *DescriptorPath))
	{
		UE_LOG(LogWoTBootstrap, Error,
			TEXT("production descriptor does not exist: %s"), *DescriptorPath);
		return false;
	}
	FWoTBootstrapStatus Validated =
		ValidateCacheProductionDescriptorForTesting(Bytes);
	if (Validated.IsError())
	{
		UE_LOG(LogWoTBootstrap, Error,
			TEXT("production descriptor is not the reviewed input: %s"),
			*Validated.GetError().ToString());
		return false;
	}
	return true;
}
}  // namespace

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	GRetailProtocolOut = WOT_RETAIL_DUP(1);
	if (GRetailProtocolOut >= 0)
	{
		WOT_RETAIL_DUP2(2, 1);
	}
	const int32 PreInitResult = GEngineLoop.PreInit(ArgC, ArgV);
	if (PreInitResult != 0)
	{
		return PreInitResult;
	}
	int32 ExitCode = 0;
	if (HasRetailFlag(ArgC, ArgV, TEXT("artifact-roundtrip")))
	{
		ExitCode = GRetailProtocolOut < 0 ? 2 : RunRetailArtifactRoundTrip();
	}
	else
	{
		FString Filter;
		if (!FindRetailOption(ArgC, ArgV, TEXT("suite"), Filter) || Filter.IsEmpty())
		{
			UE_LOG(LogWoTBootstrap, Error,
				TEXT("usage: WoTRetailReaderTests --suite <filter>[+<filter>] "
					"[--report <index.json>] [--production-descriptor <json>] | "
					"--artifact-roundtrip; entry reader: --suite "
					"WoT.RetailReader.FrontEndWorld+WoT.RetailReader.BSPWorld"));
			ExitCode = 2;
		}
		else
		{
			FString DescriptorPath;
			const bool bHasDescriptor = FindRetailOption(
				ArgC, ArgV, TEXT("production-descriptor"), DescriptorPath);
			if (bHasDescriptor && !ValidateRetailProductionDescriptor(DescriptorPath))
			{
				ExitCode = 2;
			}
			else
			{
				// The suite loads exactly this path; an empty value deliberately
				// selects its checked-in fallback for no-flag invocations.
				SetCacheProductionDescriptorPathForTesting(
					bHasDescriptor ? DescriptorPath : FString());
				FString ReportPath;
				FindRetailOption(ArgC, ArgV, TEXT("report"), ReportPath);
				ExitCode = RunRetailSuite(Filter, ReportPath);
			}
		}
	}
	FEngineLoop::AppPreExit();
	FEngineLoop::AppExit();
	return ExitCode;
}
