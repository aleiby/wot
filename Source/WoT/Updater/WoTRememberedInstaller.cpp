#include "WoTRememberedInstaller.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
// Every name here is prefixed: WoT sources share unity translation units.
constexpr TCHAR WoTRememberedInstallerFormat[] =
	TEXT("wot-remembered-installer-v1");
constexpr TCHAR WoTRememberedInstallerStateDirectory[] = TEXT("UpdaterState");
constexpr TCHAR WoTRememberedInstallerWhat[] = TEXT("remembered installer record");
constexpr int32 WoTRememberedInstallerMaxPathChars = 4096;
/** A 4096-unit path is at most 6 bytes per unit once every non-ASCII unit is
 * a \uXXXX escape, plus the fixed keys and format string. */
constexpr uint32 WoTRememberedInstallerMaxRecordBytes = 32 * 1024;
/** Decoded UTF-8 bound for one string: 4096 UTF-16 units, 3 bytes each. */
constexpr uint32 WoTRememberedInstallerMaxStringBytes = 16 * 1024;

FWoTBootstrapError WoTRememberedInstallerError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return MakeWoTBootstrapError(Code, Message);
}

#if PLATFORM_WINDOWS
bool WoTRememberedInstallerIsAsciiLetter(TCHAR Character)
{
	return (Character >= TEXT('A') && Character <= TEXT('Z'))
		|| (Character >= TEXT('a') && Character <= TEXT('z'));
}
#endif

/** One path component: a plain name, never empty, `.` or `..`. On Windows it
 * also carries none of the stream, device-path or wildcard characters and no
 * trailing dot or space (which Windows silently strips, aliasing names). */
bool WoTRememberedInstallerComponentIsPlain(FStringView Component)
{
	if (Component.IsEmpty() || Component == TEXT(".")
		|| Component == TEXT(".."))
	{
		return false;
	}
#if PLATFORM_WINDOWS
	for (const TCHAR Character : Component)
	{
		switch (Character)
		{
		case TEXT('<'):
		case TEXT('>'):
		case TEXT(':'):
		case TEXT('"'):
		case TEXT('|'):
		case TEXT('?'):
		case TEXT('*'):
			return false;
		default:
			break;
		}
	}
	const TCHAR Last = Component[Component.Len() - 1];
	if (Last == TEXT('.') || Last == TEXT(' '))
	{
		return false;
	}
#endif
	return true;
}
} // namespace

bool WoTIsRememberableInstallerPath(FStringView Path)
{
	if (Path.IsEmpty() || Path.Len() > WoTRememberedInstallerMaxPathChars)
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (Character < 0x20 || Character == 0x7f || Character == TEXT('\\'))
		{
			return false;
		}
	}
	int32 FirstComponent = 0;
	int32 MinimumComponents = 1;
#if PLATFORM_WINDOWS
	if (Path.Len() >= 3 && WoTRememberedInstallerIsAsciiLetter(Path[0])
		&& Path[1] == TEXT(':') && Path[2] == TEXT('/'))
	{
		FirstComponent = 3;
	}
	else if (Path.Len() >= 2 && Path[0] == TEXT('/') && Path[1] == TEXT('/'))
	{
		// //server/share/leaf: the server and share are plain components too,
		// which also refuses the //?/ and //./ device namespaces.
		FirstComponent = 2;
		MinimumComponents = 3;
	}
	else
	{
		return false;
	}
#else
	if (Path[0] != TEXT('/'))
	{
		return false;
	}
	FirstComponent = 1;
#endif
	int32 Components = 0;
	int32 ComponentStart = FirstComponent;
	for (int32 Index = FirstComponent; Index <= Path.Len(); ++Index)
	{
		if (Index < Path.Len() && Path[Index] != TEXT('/'))
		{
			continue;
		}
		if (!WoTRememberedInstallerComponentIsPlain(
				Path.Mid(ComponentStart, Index - ComponentStart)))
		{
			return false;
		}
		++Components;
		ComponentStart = Index + 1;
	}
	return Components >= MinimumComponents;
}

TWoTBootstrapResult<TArray<uint8>> WoTEncodeRememberedInstallerRecord(
	FStringView CanonicalInstallerPath)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (!WoTIsRememberableInstallerPath(CanonicalInstallerPath))
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the installer path is not in a form that can be remembered")));
	}
	FWoTCanonicalJsonValue Record = FWoTCanonicalJsonValue::MakeObject();
	if (!Record.AddMember(TEXT("format"),
			FWoTCanonicalJsonValue::MakeString(WoTRememberedInstallerFormat))
		|| !Record.AddMember(TEXT("path"), FWoTCanonicalJsonValue::MakeString(
			FString(CanonicalInstallerPath))))
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::NonCanonicalJson,
			TEXT("the remembered installer record could not be assembled")));
	}
	TWoTBootstrapResult<TArray<uint8>> Bytes = WriteCanonicalJson(Record);
	if (Bytes.IsError())
	{
		return FResult::Error(MoveTemp(Bytes).GetError());
	}
	if (static_cast<uint32>(Bytes.GetValue().Num())
		> WoTRememberedInstallerMaxRecordBytes)
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::SizeLimit,
			TEXT("the remembered installer record exceeds its bound")));
	}
	return Bytes;
}

TWoTBootstrapResult<FString> WoTParseRememberedInstallerRecord(
	TConstArrayView<uint8> Bytes)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (static_cast<uint64>(Bytes.Num()) > WoTRememberedInstallerMaxRecordBytes)
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::SizeLimit,
			TEXT("the remembered installer record exceeds its bound")));
	}
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = WoTRememberedInstallerMaxRecordBytes;
	Limits.MaxDepth = 1;
	Limits.MaxMembers = 2;
	Limits.MaxStringBytes = WoTRememberedInstallerMaxStringBytes;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	const FWoTCanonicalJsonValue& Record = Parsed.GetValue();
	static const TCHAR* const Members[] = {TEXT("format"), TEXT("path")};
	if (TOptional<FWoTBootstrapError> Shape = RequireExactMembers(
			Record, MakeArrayView(Members), WoTRememberedInstallerWhat);
		Shape.IsSet())
	{
		return FResult::Error(MoveTemp(Shape.GetValue()));
	}
	FString Format;
	if (TOptional<FWoTBootstrapError> FormatError = ReadStringField(
			Record, TEXT("format"), WoTRememberedInstallerWhat, Format);
		FormatError.IsSet())
	{
		return FResult::Error(MoveTemp(FormatError.GetValue()));
	}
	if (!Format.Equals(WoTRememberedInstallerFormat, ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the remembered installer record has an unknown format")));
	}
	FString Path;
	if (TOptional<FWoTBootstrapError> PathError = ReadStringField(
			Record, TEXT("path"), WoTRememberedInstallerWhat, Path);
		PathError.IsSet())
	{
		return FResult::Error(MoveTemp(PathError.GetValue()));
	}
	if (!WoTIsRememberableInstallerPath(Path))
	{
		return FResult::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the remembered installer path is not canonical")));
	}
	return FResult::Success(MoveTemp(Path));
}

FWoTRememberedInstallerRead WoTReadRememberedInstaller(
	const FString& UserStateRoot,
	const FWoTNativeFileIdentity* ExpectedUserStateRootIdentity)
{
	FWoTRememberedInstallerRead Result;
	if (UserStateRoot.IsEmpty())
	{
		return Result;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> User =
		OpenNativeDirectoryNoFollow(UserStateRoot);
	if (User.IsError())
	{
		Result.IgnoredReason = TEXT("data-folder-unreadable");
		return Result;
	}
	if (ExpectedUserStateRootIdentity != nullptr
		&& !User.GetValue().GetIdentity().IsSameObject(
			*ExpectedUserStateRootIdentity))
	{
		Result.IgnoredReason = TEXT("data-folder-changed");
		return Result;
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> StateType =
		QueryNativeEntryTypeAtNoFollow(
			User.GetValue(), WoTRememberedInstallerStateDirectory);
	if (StateType.IsError())
	{
		Result.IgnoredReason = TEXT("unreadable");
		return Result;
	}
	if (!StateType.GetValue().IsSet())
	{
		return Result;
	}
	if (StateType.GetValue().GetValue() != EWoTTreeEntryType::Directory)
	{
		Result.IgnoredReason = TEXT("state-not-a-directory");
		return Result;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> State =
		OpenNativeDirectoryAtNoFollow(
			User.GetValue(), WoTRememberedInstallerStateDirectory);
	if (State.IsError())
	{
		Result.IgnoredReason = TEXT("unreadable");
		return Result;
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> RecordType =
		QueryNativeEntryTypeAtNoFollow(
			State.GetValue(), WoTRememberedInstallerLeaf);
	if (RecordType.IsError())
	{
		Result.IgnoredReason = TEXT("unreadable");
		return Result;
	}
	if (!RecordType.GetValue().IsSet())
	{
		return Result;
	}
	if (RecordType.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		Result.IgnoredReason = TEXT("not-a-file");
		return Result;
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenNativeFileAtReadNoFollow(State.GetValue(), WoTRememberedInstallerLeaf);
	if (File.IsError())
	{
		Result.IgnoredReason = TEXT("unreadable");
		return Result;
	}
	const uint64 CapturedSize = File.GetValue().GetIdentity().Size;
	if (CapturedSize > WoTRememberedInstallerMaxRecordBytes)
	{
		Result.IgnoredReason = TEXT("too-large");
		return Result;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(CapturedSize));
	uint64 TotalRead = 0;
	while (TotalRead < CapturedSize)
	{
		TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(File.GetValue(),
			TotalRead, MakeArrayView(Bytes).Slice(static_cast<int32>(TotalRead),
				Bytes.Num() - static_cast<int32>(TotalRead)));
		if (Read.IsError())
		{
			Result.IgnoredReason = TEXT("unreadable");
			return Result;
		}
		if (Read.GetValue() == 0)
		{
			Result.IgnoredReason = TEXT("changed-while-read");
			return Result;
		}
		TotalRead += Read.GetValue();
	}
	TWoTBootstrapResult<FString> Parsed =
		WoTParseRememberedInstallerRecord(Bytes);
	if (Parsed.IsError())
	{
		Result.IgnoredReason = TEXT("malformed");
		return Result;
	}
	Result.CanonicalInstallerPath = MoveTemp(Parsed).GetValue();
	return Result;
}

FWoTBootstrapStatus WoTWriteRememberedInstaller(
	const FString& UserStateRoot,
	const FWoTNativeFileIdentity* ExpectedUserStateRootIdentity,
	FStringView CanonicalInstallerPath)
{
	TWoTBootstrapResult<TArray<uint8>> Bytes =
		WoTEncodeRememberedInstallerRecord(CanonicalInstallerPath);
	if (Bytes.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Bytes).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> User =
		OpenNativeDirectoryNoFollow(UserStateRoot);
	if (User.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(User).GetError());
	}
	if (ExpectedUserStateRootIdentity != nullptr
		&& !User.GetValue().GetIdentity().IsSameObject(
			*ExpectedUserStateRootIdentity))
	{
		return FWoTBootstrapStatus::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the data folder is no longer the one this copy started with")));
	}
	bool bStateCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> State =
		OpenOrCreateNativeDirectoryAtNoFollow(User.GetValue(),
			WoTRememberedInstallerStateDirectory, bStateCreated);
	if (State.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(State).GetError());
	}
	(void)bStateCreated;
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Existing =
		QueryNativeEntryTypeAtNoFollow(State.GetValue(), WoTRememberedInstallerLeaf);
	if (Existing.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Existing).GetError());
	}
	if (Existing.GetValue().IsSet()
		&& Existing.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(WoTRememberedInstallerError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the remembered installer record is not a regular file")));
	}
	const FString TemporaryLeaf = FString::Printf(TEXT("%s.%u.%s.tmp"),
		WoTRememberedInstallerLeaf, FPlatformProcess::GetCurrentProcessId(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TWoTBootstrapResult<FWoTNativeFileHandle> Temporary =
		CreateNativeFileAtWriteNoFollow(State.GetValue(), TemporaryLeaf);
	if (Temporary.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Temporary).GetError());
	}
	FWoTBootstrapStatus Written =
		WriteNativeFile(Temporary.GetValue(), Bytes.GetValue());
	if (!Written.IsError())
	{
		Written = FlushNativeFile(Temporary.GetValue());
	}
	Temporary.GetValue() = FWoTNativeFileHandle();
	if (Written.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return Written;
	}
	FWoTBootstrapStatus Published = RenameNativeEntryAtNoFollow(
		State.GetValue(), TemporaryLeaf, State.GetValue(),
		WoTRememberedInstallerLeaf, Existing.GetValue().IsSet());
	if (Published.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return Published;
	}
	return FlushNativeDirectory(State.GetValue());
}
