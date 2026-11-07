#include "WoTUpdaterCore/UpdateState.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr TCHAR WoTUpdateStateLeaf[] = TEXT("update-state.json");
constexpr uint64 WoTUpdateStateMaxBytes = 1024;

FWoTBootstrapError WoTUpdateStateError(FString Message, FString Path = FString())
{
	return MakeWoTBootstrapError(EWoTBootstrapError::Io,
		MoveTemp(Message), MoveTemp(Path));
}

FString WoTUpdateStatePath(const FWoTUpdaterStateRoot& Root)
{
	return FPaths::Combine(Root.StateDir, WoTUpdateStateLeaf);
}

TOptional<FString> WoTValidateUpdateStateRoot(const FWoTUpdaterStateRoot& Root)
{
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
		ResolveUpdaterUserStateRoot(Root.UserStateRoot);
	if (Resolved.IsError()) return Resolved.GetError().ToString();
	if (Root.UserStateRoot != Resolved.GetValue().UserStateRoot
		|| Root.StateDir != Resolved.GetValue().StateDir)
	{
		return FString(TEXT("the updater state path does not derive from its data root"));
	}
	return {};
}

bool WoTConsumeAscii(TConstArrayView<uint8> Bytes, int32& Index,
	const ANSICHAR* Expected)
{
	while (*Expected != '\0')
	{
		if (Index >= Bytes.Num()
			|| Bytes[Index++] != static_cast<uint8>(*Expected++))
		{
			return false;
		}
	}
	return true;
}

bool WoTConsumeUint64(TConstArrayView<uint8> Bytes, int32& Index,
	uint64& OutValue)
{
	if (Index >= Bytes.Num() || Bytes[Index] < '0' || Bytes[Index] > '9')
	{
		return false;
	}
	if (Bytes[Index] == '0')
	{
		++Index;
		OutValue = 0;
		return Index >= Bytes.Num() || Bytes[Index] < '0' || Bytes[Index] > '9';
	}
	uint64 Value = 0;
	while (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
	{
		const uint64 Digit = static_cast<uint64>(Bytes[Index++] - '0');
		if (Value > (MAX_uint64 - Digit) / 10) return false;
		Value = Value * 10 + Digit;
	}
	OutValue = Value;
	return true;
}

bool WoTParseUpdateState(TConstArrayView<uint8> Bytes, FWoTUpdateStateRecord& OutState)
{
	int32 Index = 0;
	FWoTUpdateStateRecord Parsed;
	if (!WoTConsumeAscii(Bytes, Index, "{\"key_revocation_floor\":")
		|| !WoTConsumeUint64(Bytes, Index, Parsed.KeyRevocationFloor)
		|| !WoTConsumeAscii(Bytes, Index, ",\"newest_announcement_sequence\":")
		|| !WoTConsumeUint64(Bytes, Index, Parsed.NewestAnnouncementSequence)
		|| !WoTConsumeAscii(Bytes, Index, "}") || Index != Bytes.Num())
	{
		return false;
	}
	OutState = Parsed;
	return true;
}

TArray<uint8> WoTEncodeUpdateState(const FWoTUpdateStateRecord& State)
{
	const FString Text = FString::Printf(
		TEXT("{\"key_revocation_floor\":%llu,\"newest_announcement_sequence\":%llu}"),
		State.KeyRevocationFloor, State.NewestAnnouncementSequence);
	TArray<uint8> Bytes;
	Bytes.Reserve(Text.Len());
	for (const TCHAR Character : Text) Bytes.Add(static_cast<uint8>(Character));
	return Bytes;
}

void WoTSetUpdateStateDiagnostic(FString* OutDiagnostic, FString Diagnostic)
{
	if (OutDiagnostic != nullptr) *OutDiagnostic = MoveTemp(Diagnostic);
}

FWoTBootstrapStatus WoTUpdateStateSaveFailure(const FWoTBootstrapError& Cause,
	FString* OutDiagnostic, const FString& Path)
{
	const FString Diagnostic = FString::Printf(
		TEXT("update state could not be saved: %s"), *Cause.ToString());
	WoTSetUpdateStateDiagnostic(OutDiagnostic, Diagnostic);
	return FWoTBootstrapStatus::Error(WoTUpdateStateError(Diagnostic, Path));
}
}  // namespace

FWoTUpdateStateRecordReadResult ReadWoTUpdateState(const FWoTUpdaterStateRoot& Root)
{
	FWoTUpdateStateRecordReadResult Result;
	if (TOptional<FString> Invalid = WoTValidateUpdateStateRoot(Root);
		Invalid.IsSet())
	{
		Result.Diagnostic = MoveTemp(Invalid.GetValue());
		return Result;
	}
	if (!IFileManager::Get().DirectoryExists(*Root.StateDir))
	{
		Result.Diagnostic = TEXT("update state is missing");
		Result.bMissing = true;
		return Result;
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Directory =
		OpenNativeDirectoryNoFollow(Root.StateDir);
	if (Directory.IsError())
	{
		Result.Diagnostic = FString::Printf(TEXT("update state is unreadable: %s"),
			*Directory.GetError().ToString());
		return Result;
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(Directory.GetValue(), WoTUpdateStateLeaf);
	if (Found.IsError())
	{
		Result.Diagnostic = FString::Printf(TEXT("update state is unreadable: %s"),
			*Found.GetError().ToString());
		return Result;
	}
	if (!Found.GetValue().IsSet())
	{
		Result.Diagnostic = TEXT("update state is missing");
		Result.bMissing = true;
		return Result;
	}
	if (Found.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		Result.Diagnostic = TEXT("update state is not a regular file");
		return Result;
	}

	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenNativeFileAtReadNoFollow(Directory.GetValue(), WoTUpdateStateLeaf);
	if (File.IsError())
	{
		Result.Diagnostic = FString::Printf(TEXT("update state is unreadable: %s"),
			*File.GetError().ToString());
		return Result;
	}
	const uint64 CapturedSize = File.GetValue().GetIdentity().Size;
	if (CapturedSize > WoTUpdateStateMaxBytes)
	{
		Result.Diagnostic = TEXT("update state exceeds the 1 KiB limit");
		return Result;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(CapturedSize));
	uint64 TotalRead = 0;
	while (TotalRead < CapturedSize)
	{
		TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(File.GetValue(),
			TotalRead, MakeArrayView(Bytes).Slice(
				static_cast<int32>(TotalRead), Bytes.Num() - static_cast<int32>(TotalRead)));
		if (Read.IsError())
		{
			Result.Diagnostic = FString::Printf(TEXT("update state is unreadable: %s"),
				*Read.GetError().ToString());
			return Result;
		}
		if (Read.GetValue() == 0)
		{
			Result.Diagnostic = TEXT("update state changed while it was being read");
			return Result;
		}
		TotalRead += Read.GetValue();
	}
	if (!WoTParseUpdateState(Bytes, Result.State))
	{
		Result.State = FWoTUpdateStateRecord{};
		Result.Diagnostic = TEXT("update state is malformed");
	}
	return Result;
}

FWoTBootstrapStatus MergeAndWriteWoTUpdateState(
	const FWoTUpdaterStateRoot& Root, FWoTUpdateStateRecord& InOutState,
	FString* OutDiagnostic)
{
	WoTSetUpdateStateDiagnostic(OutDiagnostic, FString());
	if (TOptional<FString> Invalid = WoTValidateUpdateStateRoot(Root);
		Invalid.IsSet())
	{
		const FWoTBootstrapError Error = WoTUpdateStateError(
			MoveTemp(Invalid.GetValue()), WoTUpdateStatePath(Root));
		return WoTUpdateStateSaveFailure(Error, OutDiagnostic,
			WoTUpdateStatePath(Root));
	}

	const FWoTUpdateStateRecordReadResult Disk = ReadWoTUpdateState(Root);
	InOutState.KeyRevocationFloor = FMath::Max(
		InOutState.KeyRevocationFloor, Disk.State.KeyRevocationFloor);
	InOutState.NewestAnnouncementSequence = FMath::Max(
		InOutState.NewestAnnouncementSequence,
		Disk.State.NewestAnnouncementSequence);
	// A record that does not exist yet is the normal first run, not a problem
	// to report (callers log OutDiagnostic as a save failure).
	if (!Disk.Diagnostic.IsEmpty() && !Disk.bMissing)
	{
		WoTSetUpdateStateDiagnostic(OutDiagnostic, Disk.Diagnostic);
	}

	bool bUserCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> User =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(
			Root.UserStateRoot, bUserCreated);
	if (User.IsError())
	{
		return WoTUpdateStateSaveFailure(User.GetError(), OutDiagnostic,
			WoTUpdateStatePath(Root));
	}
	bool bStateCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> StateDirectory =
		OpenOrCreateNativeDirectoryAtNoFollow(User.GetValue().Directory,
			TEXT("UpdaterState"), bStateCreated);
	if (StateDirectory.IsError())
	{
		return WoTUpdateStateSaveFailure(StateDirectory.GetError(), OutDiagnostic,
			WoTUpdateStatePath(Root));
	}

	const FString TemporaryLeaf = FString::Printf(TEXT("update-state.%u.%s.tmp"),
		FPlatformProcess::GetCurrentProcessId(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TWoTBootstrapResult<FWoTNativeFileHandle> Temporary =
		CreateNativeFileAtWriteNoFollow(StateDirectory.GetValue(), TemporaryLeaf);
	if (Temporary.IsError())
	{
		return WoTUpdateStateSaveFailure(Temporary.GetError(), OutDiagnostic,
			WoTUpdateStatePath(Root));
	}
	const TArray<uint8> Bytes = WoTEncodeUpdateState(InOutState);
	if (FWoTBootstrapStatus Written =
			WriteNativeFile(Temporary.GetValue(), Bytes); Written.IsError())
	{
		Temporary.GetValue() = FWoTNativeFileHandle();
		RemoveNativeEntryAtNoFollow(StateDirectory.GetValue(), TemporaryLeaf,
			EWoTTreeEntryType::File);
		return WoTUpdateStateSaveFailure(Written.GetError(), OutDiagnostic,
			WoTUpdateStatePath(Root));
	}
	Temporary.GetValue() = FWoTNativeFileHandle();
	if (FWoTBootstrapStatus Renamed = RenameNativeEntryAtNoFollow(
			StateDirectory.GetValue(), TemporaryLeaf, StateDirectory.GetValue(),
			WoTUpdateStateLeaf, true); Renamed.IsError())
	{
		RemoveNativeEntryAtNoFollow(StateDirectory.GetValue(), TemporaryLeaf,
			EWoTTreeEntryType::File);
		return WoTUpdateStateSaveFailure(Renamed.GetError(), OutDiagnostic,
			WoTUpdateStatePath(Root));
	}
	return FWoTBootstrapStatus::Success();
}
