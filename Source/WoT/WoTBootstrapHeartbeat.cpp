#include "WoTBootstrapHeartbeat.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Updater/WoTGameBootstrapWorker.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr double WoTBootstrapHeartbeatInitialOpenRetrySeconds = 1.0;
constexpr double WoTBootstrapHeartbeatMaximumOpenRetrySeconds = 60.0;
constexpr int32 WoTBootstrapHeartbeatRetainedFileCount = 20;
constexpr TCHAR WoTBootstrapHeartbeatPrefix[] =
	TEXT("bootstrap-heartbeat-v1.");
constexpr TCHAR WoTBootstrapHeartbeatSuffix[] = TEXT(".jsonl");

FString WoTBootstrapHeartbeatNewFileName()
{
	return FString::Printf(TEXT("%s%u.%s%s"),
		WoTBootstrapHeartbeatPrefix,
		FPlatformProcess::GetCurrentProcessId(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower(),
		WoTBootstrapHeartbeatSuffix);
}

struct FWoTBootstrapHeartbeatState
{
	FCriticalSection Lock;
	FString FileName = WoTBootstrapHeartbeatNewFileName();
	bool bRetentionAttempted = false;
	bool bOpenAttempted = false;
	double NextOpenAttemptSeconds = 0.0;
	double OpenRetryDelaySeconds =
		WoTBootstrapHeartbeatInitialOpenRetrySeconds;
#if WITH_AUTOMATION_TESTS
	FString RootOverrideForTesting;
	bool bFailNextOpenForTesting = false;
#endif
};

FWoTBootstrapHeartbeatState& WoTBootstrapHeartbeatStateSingleton()
{
	static FWoTBootstrapHeartbeatState State;
	return State;
}

void WoTBootstrapHeartbeatScheduleOpenRetry(
	FWoTBootstrapHeartbeatState& State)
{
	State.bOpenAttempted = true;
	State.NextOpenAttemptSeconds =
		FPlatformTime::Seconds() + State.OpenRetryDelaySeconds;
	State.OpenRetryDelaySeconds = FMath::Min(
		State.OpenRetryDelaySeconds * 2.0,
		WoTBootstrapHeartbeatMaximumOpenRetrySeconds);
}

void WoTBootstrapHeartbeatResetOpenRetry(
	FWoTBootstrapHeartbeatState& State)
{
	State.bOpenAttempted = false;
	State.NextOpenAttemptSeconds = 0.0;
	State.OpenRetryDelaySeconds =
		WoTBootstrapHeartbeatInitialOpenRetrySeconds;
}

FString WoTBootstrapHeartbeatEscape(FStringView Value)
{
	FString Escaped;
	Escaped.Reserve(Value.Len());
	for (const TCHAR Char : Value)
	{
		switch (Char)
		{
		case TEXT('"'): Escaped += TEXT("\\\""); break;
		case TEXT('\\'): Escaped += TEXT("\\\\"); break;
		case TEXT('\n'): Escaped += TEXT("\\n"); break;
		case TEXT('\r'): Escaped += TEXT("\\r"); break;
		case TEXT('\t'): Escaped += TEXT("\\t"); break;
		default:
			if (Char < TEXT(' '))
			{
				Escaped += FString::Printf(TEXT("\\u%04x"),
					static_cast<uint32>(Char));
			}
			else
			{
				Escaped.AppendChar(Char);
			}
			break;
		}
	}
	return Escaped;
}

FString WoTBootstrapHeartbeatLine(
	FStringView Event, EWoTBootstrapPhase Phase, uint64 OperationEpoch,
	TOptional<uint64> Completed, TOptional<uint64> Total,
	FStringView Detail, FStringView TimestampUtc, uint32 ProcessId)
{
	// Keys stay alphabetical so the line set is canonical and diffable.
	FString Line = TEXT("{");
	if (Completed.IsSet())
	{
		Line += FString::Printf(TEXT("\"completed\":%llu,"),
			static_cast<unsigned long long>(Completed.GetValue()));
	}
	if (!Detail.IsEmpty())
	{
		Line += FString::Printf(TEXT("\"detail\":\"%s\","),
			*WoTBootstrapHeartbeatEscape(Detail));
	}
	Line += FString::Printf(
		TEXT("\"epoch\":%llu,\"event\":\"%s\",\"phase\":\"%s\",\"pid\":%u,"),
		static_cast<unsigned long long>(OperationEpoch),
		*WoTBootstrapHeartbeatEscape(Event),
		WoTBootstrapPhaseName(Phase), ProcessId);
	if (Total.IsSet())
	{
		Line += FString::Printf(TEXT("\"total\":%llu,"),
			static_cast<unsigned long long>(Total.GetValue()));
	}
	Line += FString::Printf(TEXT("\"ts_utc\":\"%s\"}\n"),
		*WoTBootstrapHeartbeatEscape(TimestampUtc));
	return Line;
}

struct FWoTBootstrapHeartbeatCandidate
{
	FString Name;
	FWoTNativeFileIdentity Identity;
};

bool WoTBootstrapHeartbeatHasProcessFileName(const FString& Name)
{
	if (!Name.StartsWith(WoTBootstrapHeartbeatPrefix, ESearchCase::CaseSensitive)
		|| !Name.EndsWith(WoTBootstrapHeartbeatSuffix, ESearchCase::CaseSensitive))
		return false;
	int32 Index = UE_ARRAY_COUNT(WoTBootstrapHeartbeatPrefix) - 1;
	if (Index >= Name.Len() || Name[Index] < TEXT('1') || Name[Index] > TEXT('9'))
		return false;
	uint32 Pid = 0;
	while (Index < Name.Len() && Name[Index] >= TEXT('0') && Name[Index] <= TEXT('9'))
	{
		const uint32 Digit = Name[Index++] - TEXT('0');
		if (Pid > (MAX_uint32 - Digit) / 10) return false;
		Pid = Pid * 10 + Digit;
	}
	if (Index >= Name.Len() || Name[Index++] != TEXT('.')) return false;
	if (Name.Len() - Index != 32 + UE_ARRAY_COUNT(WoTBootstrapHeartbeatSuffix) - 1)
		return false;
	for (int32 End = Index + 32; Index < End; ++Index)
	{
		const TCHAR Char = Name[Index];
		if (!((Char >= TEXT('0') && Char <= TEXT('9'))
			|| (Char >= TEXT('a') && Char <= TEXT('f'))
			|| (Char >= TEXT('A') && Char <= TEXT('F')))) return false;
	}
	return true;
}

void WoTBootstrapHeartbeatRetainNewest(
	const FWoTNativeDirectoryHandle& Directory,
	const FString& CurrentFileName)
{
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Enumerated =
		EnumerateNativeDirectory(Directory);
	if (Enumerated.IsError())
	{
		return;
	}

	TArray<FWoTBootstrapHeartbeatCandidate> Candidates;
	for (const FWoTNativeDirectoryEntry& Entry : Enumerated.GetValue())
	{
		if (Entry.Type != EWoTTreeEntryType::File
			|| Entry.Name.Equals(CurrentFileName, ESearchCase::IgnoreCase)
			|| !WoTBootstrapHeartbeatHasProcessFileName(Entry.Name))
		{
			continue;
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
			OpenNativeFileAtReadNoFollow(Directory, Entry.Name);
		if (!Opened.IsError())
		{
			FWoTBootstrapHeartbeatCandidate& Candidate =
				Candidates.Emplace_GetRef();
			Candidate.Name = Entry.Name;
			Candidate.Identity = Opened.GetValue().GetIdentity();
		}
	}

	Candidates.Sort([](const FWoTBootstrapHeartbeatCandidate& Left,
		const FWoTBootstrapHeartbeatCandidate& Right)
	{
		if (Left.Identity.ChangeState != Right.Identity.ChangeState)
		{
			return Left.Identity.ChangeState > Right.Identity.ChangeState;
		}
		return Left.Name.Compare(Right.Name, ESearchCase::CaseSensitive) > 0;
	});

	const int32 PriorFilesToKeep =
		FMath::Max(0, WoTBootstrapHeartbeatRetainedFileCount - 1);
	for (int32 Index = PriorFilesToKeep; Index < Candidates.Num(); ++Index)
	{
		(void)RemoveNativeEntryAtNoFollowIfIdentity(
			Directory, Candidates[Index].Name, EWoTTreeEntryType::File,
			Candidates[Index].Identity);
	}
}

bool WoTBootstrapHeartbeatOpenForAppend(
	FWoTBootstrapHeartbeatState& State,
	FWoTNativeFileHandle& OutFile, uint64& OutOffset)
{
	if (State.bOpenAttempted
		&& FPlatformTime::Seconds() < State.NextOpenAttemptSeconds)
	{
		return false;
	}
#if WITH_AUTOMATION_TESTS
	if (State.bFailNextOpenForTesting)
	{
		State.bFailNextOpenForTesting = false;
		WoTBootstrapHeartbeatScheduleOpenRetry(State);
		return false;
	}
#endif

	FString InstallRoot;
#if WITH_AUTOMATION_TESTS
	InstallRoot = State.RootOverrideForTesting;
#endif
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> OpenedRoot =
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory>::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the heartbeat root is unavailable")));
#if WOT_GUARDED_BOOTSTRAP
	if (InstallRoot.IsEmpty())
	{
		OpenedRoot = WoTReopenResolvedUserStateRoot();
		if (OpenedRoot.IsError())
		{
			WoTBootstrapHeartbeatScheduleOpenRetry(State);
			return false;
		}
	}
#endif
	if (!InstallRoot.IsEmpty())
	{
		FString Root = FPaths::ConvertRelativePathToFull(InstallRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (FPaths::IsRelative(Root)
			|| !FPaths::CollapseRelativeDirectories(Root))
		{
			WoTBootstrapHeartbeatScheduleOpenRetry(State);
			return false;
		}
		bool bCreatedRoot = false;
		OpenedRoot = OpenOrCreateCanonicalNativeDirectoryNoFollow(
			Root, bCreatedRoot);
		(void)bCreatedRoot;
	}
	if (OpenedRoot.IsError())
	{
		WoTBootstrapHeartbeatScheduleOpenRetry(State);
		return false;
	}

	bool bCreatedState = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> StateDirectory =
		OpenOrCreateNativeDirectoryAtNoFollow(
			OpenedRoot.GetValue().Directory, TEXT("UpdaterState"), bCreatedState);
	(void)bCreatedState;
	if (StateDirectory.IsError())
	{
		WoTBootstrapHeartbeatScheduleOpenRetry(State);
		return false;
	}
	bool bCreatedFile = false;
	TWoTBootstrapResult<FWoTNativeFileHandle> OpenedFile =
		OpenOrCreateNativeFileAtReadWriteNoFollow(
			StateDirectory.GetValue(), State.FileName, bCreatedFile);
	(void)bCreatedFile;
	if (OpenedFile.IsError())
	{
		WoTBootstrapHeartbeatScheduleOpenRetry(State);
		return false;
	}

	FWoTNativeFileHandle File = MoveTemp(OpenedFile).GetValue();
	uint64 Offset = File.GetIdentity().Size;
	if (!State.bRetentionAttempted)
	{
		State.bRetentionAttempted = true;
		WoTBootstrapHeartbeatRetainNewest(
			StateDirectory.GetValue(), State.FileName);
	}
	if (Offset > 0)
	{
		uint8 LastByte = 0;
		const TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(
			File, Offset - 1, TArrayView<uint8>(&LastByte, 1));
		if (Read.IsError() || Read.GetValue() != 1)
		{
			WoTBootstrapHeartbeatScheduleOpenRetry(State);
			return false;
		}
		if (LastByte != '\n')
		{
			const uint8 Newline = '\n';
			if (WriteNativeFileAt(File, Offset,
					TConstArrayView<uint8>(&Newline, 1)).IsError())
			{
				WoTBootstrapHeartbeatScheduleOpenRetry(State);
				return false;
			}
			++Offset;
		}
	}

	OutFile = MoveTemp(File);
	OutOffset = Offset;
	WoTBootstrapHeartbeatResetOpenRetry(State);
	return true;
}
} // namespace

FString WoTBootstrapHeartbeatFileName()
{
	FWoTBootstrapHeartbeatState& State = WoTBootstrapHeartbeatStateSingleton();
	FScopeLock Lock(&State.Lock);
	return State.FileName;
}

const TCHAR* WoTBootstrapPhaseName(EWoTBootstrapPhase Phase)
{
	switch (Phase)
	{
	case EWoTBootstrapPhase::EstablishingIdentity:
		return TEXT("EstablishingIdentity");
	case EWoTBootstrapPhase::CheckingGameUpdates:
		return TEXT("CheckingGameUpdates");
	case EWoTBootstrapPhase::DownloadingUpdate:
		return TEXT("DownloadingUpdate");
	case EWoTBootstrapPhase::CheckingOriginalGameData:
		return TEXT("CheckingOriginalGameData");
	case EWoTBootstrapPhase::OriginalGameNeeded:
		return TEXT("OriginalGameNeeded");
	case EWoTBootstrapPhase::ExtractingOriginalGame:
		return TEXT("ExtractingOriginalGame");
	case EWoTBootstrapPhase::BuildingContent:
		return TEXT("BuildingContent");
	case EWoTBootstrapPhase::VerifyingContent:
		return TEXT("VerifyingContent");
	case EWoTBootstrapPhase::StartingGame:
		return TEXT("StartingGame");
	case EWoTBootstrapPhase::Ready:
		return TEXT("Ready");
	case EWoTBootstrapPhase::Fatal:
	default:
		return TEXT("Fatal");
	}
}

bool WoTRecordBootstrapHeartbeat(
	FStringView Event, EWoTBootstrapPhase Phase, uint64 OperationEpoch,
	TOptional<uint64> Completed, TOptional<uint64> Total,
	FStringView Detail)
{
	FWoTBootstrapHeartbeatState& State = WoTBootstrapHeartbeatStateSingleton();
	FScopeLock Lock(&State.Lock);
	FWoTNativeFileHandle File;
	uint64 Offset = 0;
	if (!WoTBootstrapHeartbeatOpenForAppend(State, File, Offset))
	{
		return false;
	}

	const FString Line = WoTBootstrapHeartbeatLine(
		Event, Phase, OperationEpoch, Completed, Total, Detail,
		FDateTime::UtcNow().ToIso8601(),
		FPlatformProcess::GetCurrentProcessId());
	const FTCHARToUTF8 Utf8(*Line);
	const TConstArrayView<uint8> Bytes(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return !WriteNativeFileAt(File, Offset, Bytes).IsError();
}

#if WITH_AUTOMATION_TESTS
void SetWoTBootstrapHeartbeatRootForTesting(FString InstallRoot)
{
	FWoTBootstrapHeartbeatState& State = WoTBootstrapHeartbeatStateSingleton();
	FScopeLock Lock(&State.Lock);
	State.RootOverrideForTesting = MoveTemp(InstallRoot);
	State.FileName = WoTBootstrapHeartbeatNewFileName();
	State.bRetentionAttempted = false;
	WoTBootstrapHeartbeatResetOpenRetry(State);
	State.bFailNextOpenForTesting = false;
}

void FailNextWoTBootstrapHeartbeatOpenForTesting()
{
	FWoTBootstrapHeartbeatState& State = WoTBootstrapHeartbeatStateSingleton();
	FScopeLock Lock(&State.Lock);
	State.bFailNextOpenForTesting = true;
}

FString WoTBootstrapHeartbeatLineForTesting(
	FStringView Event, EWoTBootstrapPhase Phase, uint64 OperationEpoch,
	TOptional<uint64> Completed, TOptional<uint64> Total,
	FStringView Detail, FStringView TimestampUtc, uint32 ProcessId)
{
	return WoTBootstrapHeartbeatLine(Event, Phase, OperationEpoch,
		Completed, Total, Detail, TimestampUtc, ProcessId);
}
#endif
