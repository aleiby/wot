// Windows link-refusing native handles.
//
// Same rule as the macOS file: owned-object opens pass
// FILE_FLAG_OPEN_REPARSE_POINT, so a symlink, junction, or mount point is opened
// AS the reparse point and rejected. The one following open below is private to
// canonicalizing a directory above an owned root and is accepted only after an
// identity-equal canonical all-components-no-follow reopen.
//
// Windows has no Win32 `openat`, so relative descent uses the native NtCreateFile
// entry point with OBJECT_ATTRIBUTES.RootDirectory set to the directory HANDLE we
// already hold. Each child therefore comes from that exact parent object even if an
// attacker gives the parent's old path to a different directory during the walk.
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ReparseClassification.h"

#include "../CanonicalNativeDirectoryPlatform.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTInstallerSourceReparse, Log, All);

// FILE_DISPOSITION_INFO's member is written as `DeleteFile` in WinBase.h, but
// fileapi.h's A/W dispatch macro is in effect when that struct is parsed, so it
// is really compiled as DeleteFileW (DeleteFileA without UNICODE). UE's
// PostWindowsApi.h then undefines the macro -- and windows.h has already been
// pulled in through CoreMinimal above, so it is undefined before any line of
// this file is compiled. The member therefore cannot be named portably here at
// all: `.DeleteFile` is not a member, and `.DeleteFileW` only is one under
// UNICODE. FILE_DISPOSITION_INFO is a documented single-BOOLEAN structure, so
// mirror it under a name of our own; the static_assert fails loudly if that
// ever stops being true.
struct FWoTFileDispositionInfo
{
	BOOLEAN DeleteFile;
};
static_assert(sizeof(FWoTFileDispositionInfo) == sizeof(FILE_DISPOSITION_INFO),
	"FILE_DISPOSITION_INFO is no longer a single BOOLEAN");

struct FWoTNativeHandleState
{
	void* Handle = INVALID_HANDLE_VALUE;
	/** Accepted data-reparse leaves retain Handle for no-follow validation and
	 * read only through this separately identity-checked filter handle. */
	void* DataHandle = INVALID_HANDLE_VALUE;
	FWoTNativeFileIdentity Identity;
	bool bInstallerSource = false;
	bool bInstallerDataReparse = false;
	bool bInstallerOnlineOnlyAtOpen = false;
	/** Opened absolutely as a drive root ("Z:/"): under Wine its mapping reparse
	 * point is the directory it stands for. Every re-query keeps this context. */
	bool bDriveRoot = false;
	/** Under Wine: the object itself is a mount point (its volume differs from
	 * its parent directory's). Kept for every re-query, like bDriveRoot. */
	bool bVolumeBoundary = false;

	/** The original spelling, retained only for diagnostics and executable reporting. */
	FString Path;

	~FWoTNativeHandleState()
	{
		if (Handle != INVALID_HANDLE_VALUE && Handle != nullptr)
		{
			::CloseHandle(static_cast<HANDLE>(Handle));
			Handle = INVALID_HANDLE_VALUE;
		}
		if (DataHandle != INVALID_HANDLE_VALUE && DataHandle != nullptr)
		{
			::CloseHandle(static_cast<HANDLE>(DataHandle));
			DataHandle = INVALID_HANDLE_VALUE;
		}
	}
};

struct FWoTNativeFileLockState
{
	void* Handle = INVALID_HANDLE_VALUE;
	OVERLAPPED Range = {};
	bool bLocked = false;

	~FWoTNativeFileLockState()
	{
		if (Handle != INVALID_HANDLE_VALUE && Handle != nullptr)
		{
			if (bLocked)
			{
				(void)::UnlockFileEx(static_cast<HANDLE>(Handle), 0, 1, 0, &Range);
			}
			(void)::CloseHandle(static_cast<HANDLE>(Handle));
		}
	}
};

struct FWoTNativeHandleAccess
{
	template<class HandleType>
	static TUniquePtr<FWoTNativeHandleState>& State(HandleType& Handle)
	{
		return Handle.State;
	}

	template<class HandleType>
	static const TUniquePtr<FWoTNativeHandleState>& State(const HandleType& Handle)
	{
		return Handle.State;
	}

	static void SetReportedPath(FWoTNativeExecutableIdentity& Handle, FString&& Path)
	{
		Handle.ReportedPath = MoveTemp(Path);
	}

	static TUniquePtr<FWoTNativeFileLockState>& LockState(FWoTNativeFileLock& Lock)
	{
		return Lock.State;
	}
};

namespace
{
#if WITH_AUTOMATION_TESTS
FWoTNativeInstallerSourceAttributeQueryForTesting
	GWoTInstallerSourceAttributeQueryForTesting;
FWoTNativeInstallerSourceReadObserverForTesting
	GWoTInstallerSourceReadObserverForTesting;
#endif

FWoTBootstrapError SystemError(const TCHAR* What, uint32 LastError, FStringView Path)
{
	FWoTBootstrapError Error = MakeWoTBootstrapError(EWoTBootstrapError::Io,
		FString::Printf(TEXT("%s failed with Windows error %u"), What, LastError),
		FString(Path));
	Error.NativeCode = LastError;
	return Error;
}

bool IsPermissionError(uint32 Error)
{
	return Error == ERROR_ACCESS_DENIED || Error == ERROR_WRITE_PROTECT
		|| Error == ERROR_NETWORK_ACCESS_DENIED;
}

FWoTBootstrapStatus SystemStatus(const TCHAR* What, uint32 LastError, FStringView Path)
{
	return FWoTBootstrapStatus::Error(SystemError(What, LastError, Path));
}

FWoTBootstrapStatus RequireSingleLeaf(FStringView Leaf)
{
	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(Leaf); Safe.IsError())
	{
		return Safe;
	}
	const FString LeafString(Leaf);
	if (LeafString.Contains(TEXT("/")) || LeafString.Contains(TEXT("\\")))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("a handle-relative mutation requires exactly one path component"),
			LeafString));
	}
	return FWoTBootstrapStatus::Success();
}

using FWoTNtCreateFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
	PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using FWoTNtSetInformationFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID,
	ULONG, ULONG /*FILE_INFORMATION_CLASS*/);
using FWoTRtlNtStatusToDosError = ULONG(WINAPI*)(NTSTATUS);

// NT FILE_RENAME_INFORMATION. Declared here rather than via winbase's
// FILE_RENAME_INFO so the Flags union member exists regardless of the SDK's
// NTDDI floor; the layouts are identical.
struct FWoTNtFileRenameInformation
{
	union
	{
		BOOLEAN ReplaceIfExists;  // FileRenameInformation (10)
		ULONG Flags;              // FileRenameInformationEx (65)
	};
	HANDLE RootDirectory;
	ULONG FileNameLength;
	WCHAR FileName[1];
};
constexpr ULONG WoTNtFileRenameInformationClass = 10;
constexpr ULONG WoTNtFileRenameInformationExClass = 65;
constexpr ULONG WoTNtRenameReplaceIfExists = 0x1;
constexpr ULONG WoTNtRenamePosixSemantics = 0x2;

// A directory rename is refused with STATUS_ACCESS_DENIED while any process
// holds a handle on anything below that directory, whatever its sharing mode;
// no rename flag avoids the check. Spelled out because ntstatus.h cannot be
// included beside windows.h without redefinitions.
constexpr NTSTATUS WoTStatusAccessDenied = static_cast<NTSTATUS>(0xC0000022L);
// Backoff for that refusal: the first wait, the cap each wait doubles up to, and
// the total time spent waiting before the rename fails as it did on the first try.
constexpr double WoTDirectoryRenameFirstWaitSeconds = 0.05;
constexpr double WoTDirectoryRenameMaxWaitSeconds = 1.0;
constexpr double WoTDirectoryRenameRetryBudgetSeconds = 10.0;
// The pre-existing retry of a replace-rename that hit a sharing violation.
constexpr int32 WoTReplaceRenameSharingRetries = 19;
constexpr float WoTReplaceRenameSharingWaitSeconds = 0.05f;

#if WITH_AUTOMATION_TESTS
TFunction<void(int32)> GWoTNativeDirectoryRenameRetryObserverForTesting;
TFunction<void(int32)> GWoTNativeReplaceRenameRetryObserverForTesting;
TAtomic<int64> GWoTNativeDirectoryRenameRetryBudgetMsForTesting(-1);
#endif

double WoTDirectoryRenameRetryBudget()
{
#if WITH_AUTOMATION_TESTS
	const int64 OverrideMs = GWoTNativeDirectoryRenameRetryBudgetMsForTesting.Load();
	if (OverrideMs >= 0)
	{
		return static_cast<double>(OverrideMs) / 1000.0;
	}
#endif
	return WoTDirectoryRenameRetryBudgetSeconds;
}

FWoTNtCreateFile ResolveNtCreateFile()
{
	const HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
	return Ntdll == nullptr ? nullptr : reinterpret_cast<FWoTNtCreateFile>(
		::GetProcAddress(Ntdll, "NtCreateFile"));
}

FWoTNtSetInformationFile ResolveNtSetInformationFile()
{
	const HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
	return Ntdll == nullptr ? nullptr : reinterpret_cast<FWoTNtSetInformationFile>(
		::GetProcAddress(Ntdll, "NtSetInformationFile"));
}

uint32 NtStatusToWindowsError(NTSTATUS Status)
{
	const HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
	const FWoTRtlNtStatusToDosError Convert = Ntdll == nullptr ? nullptr
		: reinterpret_cast<FWoTRtlNtStatusToDosError>(
			::GetProcAddress(Ntdll, "RtlNtStatusToDosError"));
	return Convert == nullptr ? static_cast<uint32>(Status) : Convert(Status);
}

constexpr uint32 WoTReparseTagNameSurrogateBit = 0x20000000u;
constexpr uint32 WoTReparseTagDedup = 0x80000013u;
constexpr uint32 WoTReparseTagWof = 0x80000017u;
constexpr uint32 WoTReparseTagAppExecLink = 0x8000001bu;
constexpr uint32 WoTReparseTagCloud = 0x9000001au;
constexpr uint32 WoTReparseTagCloudVariantMask = 0xffff0fffu;
constexpr uint32 WoTFileAttributeRecallOnOpen = 0x00040000u;
constexpr uint32 WoTFileAttributeRecallOnDataAccess = 0x00400000u;
constexpr uint32 WoTInstallerSourceNonLocalAttributes = FILE_ATTRIBUTE_OFFLINE
	| WoTFileAttributeRecallOnOpen | WoTFileAttributeRecallOnDataAccess;

EWoTInstallerSourceReparseTagClass ClassifyInstallerSourceReparseTag(
	uint32 ReparseTag)
{
	if (ReparseTag == WoTReparseTagWof || ReparseTag == WoTReparseTagDedup
		|| (ReparseTag & WoTReparseTagCloudVariantMask) == WoTReparseTagCloud)
	{
		return EWoTInstallerSourceReparseTagClass::DataContent;
	}
	if ((ReparseTag & WoTReparseTagNameSurrogateBit) != 0
		|| ReparseTag == WoTReparseTagAppExecLink)
	{
		return EWoTInstallerSourceReparseTagClass::NameSurrogate;
	}
	return EWoTInstallerSourceReparseTagClass::Unknown;
}

const TCHAR* InstallerSourceReparseClassName(
	EWoTInstallerSourceReparseTagClass Class)
{
	switch (Class)
	{
	case EWoTInstallerSourceReparseTagClass::DataContent:
		return TEXT("data-content");
	case EWoTInstallerSourceReparseTagClass::NameSurrogate:
		return TEXT("name-surrogate");
	case EWoTInstallerSourceReparseTagClass::Unknown:
	default:
		return TEXT("unknown");
	}
}

bool QueryReparseTag(HANDLE Handle, FStringView Path, uint32& OutTag,
	TOptional<FWoTBootstrapError>& OutError)
{
	FILE_ATTRIBUTE_TAG_INFO TagInfo = {};
	if (!::GetFileInformationByHandleEx(
		Handle, FileAttributeTagInfo, &TagInfo, sizeof(TagInfo)))
	{
		OutError = SystemError(
			TEXT("GetFileInformationByHandleEx(FileAttributeTagInfo)"),
			::GetLastError(), Path);
		return false;
	}
	OutTag = TagInfo.ReparseTag;
	return true;
}

void LogInstallerSourceReparseRefusal(uint32 ReparseTag,
	EWoTInstallerSourceReparseTagClass Class, FStringView Path)
{
	UE_LOG(LogWoTInstallerSourceReparse, Warning,
		TEXT("installer source refused reparse tag=0x%08X class=%s path=%s"),
		ReparseTag, InstallerSourceReparseClassName(Class), *FString(Path));
}

TOptional<FWoTBootstrapError> IdentityFromHandle(HANDLE Handle, FStringView Path,
	FWoTNativeFileIdentity& OutIdentity, bool bAllowInstallerDataReparse = false,
	bool* bOutInstallerDataReparse = nullptr,
	const FWoTReparseComponentFacts* Context = nullptr)
{
	if (bOutInstallerDataReparse != nullptr)
	{
		*bOutInstallerDataReparse = false;
	}
	FILE_ID_INFO IdInfo = {};
	if (!::GetFileInformationByHandleEx(Handle, FileIdInfo, &IdInfo, sizeof(IdInfo)))
	{
		return SystemError(TEXT("GetFileInformationByHandleEx(FileIdInfo)"),
			::GetLastError(), Path);
	}

	BY_HANDLE_FILE_INFORMATION Info = {};
	if (!::GetFileInformationByHandle(Handle, &Info))
	{
		return SystemError(TEXT("GetFileInformationByHandle"), ::GetLastError(), Path);
	}

	FILE_BASIC_INFO BasicInfo = {};
	if (!::GetFileInformationByHandleEx(Handle, FileBasicInfo, &BasicInfo, sizeof(BasicInfo)))
	{
		return SystemError(TEXT("GetFileInformationByHandleEx(FileBasicInfo)"),
			::GetLastError(), Path);
	}

	OutIdentity.VolumeId = static_cast<uint64>(IdInfo.VolumeSerialNumber);
	// FILE_ID_128 is the only id that is stable on ReFS; the 64-bit index is not.
	uint64 Low = 0;
	uint64 High = 0;
	FMemory::Memcpy(&Low, IdInfo.FileId.Identifier, sizeof(Low));
	FMemory::Memcpy(&High, IdInfo.FileId.Identifier + sizeof(Low), sizeof(High));
	OutIdentity.FileIdLow = Low;
	OutIdentity.FileIdHigh = High;

	OutIdentity.Size = (static_cast<uint64>(Info.nFileSizeHigh) << 32)
		| static_cast<uint64>(Info.nFileSizeLow);
	// ChangeTime moves for content and metadata alike and, unlike LastWriteTime,
	// cannot be set backwards by the writer.
	OutIdentity.ChangeState = static_cast<uint64>(BasicInfo.ChangeTime.QuadPart);
	OutIdentity.LinkCount = static_cast<uint32>(Info.nNumberOfLinks);

	if ((Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		if (bAllowInstallerDataReparse)
		{
			uint32 ReparseTag = 0;
			TOptional<FWoTBootstrapError> TagError;
			if (!QueryReparseTag(Handle, Path, ReparseTag, TagError))
			{
				return TagError;
			}
			const EWoTInstallerSourceReparseTagClass Class =
				ClassifyInstallerSourceReparseTag(ReparseTag);
			if (Class == EWoTInstallerSourceReparseTagClass::DataContent
				&& (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				OutIdentity.Type = EWoTTreeEntryType::File;
				if (bOutInstallerDataReparse != nullptr)
				{
					*bOutInstallerDataReparse = true;
				}
			}
			else
			{
				LogInstallerSourceReparseRefusal(ReparseTag, Class, Path);
				FWoTBootstrapError Error = MakeWoTBootstrapError(
					EWoTBootstrapError::UnsafePath,
					FString::Printf(TEXT("installer source reparse tag 0x%08X (%s) is refused"),
						ReparseTag, InstallerSourceReparseClassName(Class)),
					FString(Path));
				Error.NativeCode = ReparseTag;
				return Error;
			}
		}
		else
		{
			// Generic identity rule: opened AS the reparse point, every tag is a
			// link -- except, under Wine only, the drive root and the mount points
			// Wine presents for the Unix filesystem (a directory at a drive root
			// or volume boundary carrying a measured Wine mapping tag). Native
			// Windows never queries the tag here and keeps every tag a link.
			FWoTReparseComponentFacts Facts;
			Facts.bReparse = true;
			Facts.bDirectory = (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			Facts.bWine = WoTIsRunningUnderWine();
			if (Context != nullptr)
			{
				Facts.bDriveRoot = Context->bDriveRoot;
				Facts.bVolumeBoundary = Context->bVolumeBoundary;
			}
			if (Facts.bWine && Facts.bDirectory && (Facts.bDriveRoot || Facts.bVolumeBoundary))
			{
				TOptional<FWoTBootstrapError> TagError;
				if (!QueryReparseTag(Handle, Path, Facts.Tag, TagError))
				{
					return TagError;
				}
			}
			OutIdentity.Type = WoTClassifyPathComponent(Facts);
		}
	}
	else if ((Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		OutIdentity.Type = EWoTTreeEntryType::Directory;
	}
	else
	{
		OutIdentity.Type = EWoTTreeEntryType::File;
	}
	return {};
}

bool HandleMatchesObjectId(HANDLE Handle,
	const FWoTNativeFileIdentity& Expected, FStringView Path,
	TOptional<FWoTBootstrapError>& OutError)
{
	FILE_ID_INFO IdInfo = {};
	if (!::GetFileInformationByHandleEx(
			Handle, FileIdInfo, &IdInfo, sizeof(IdInfo)))
	{
		OutError = SystemError(
			TEXT("GetFileInformationByHandleEx(installer data FileIdInfo)"),
			::GetLastError(), Path);
		return false;
	}
	uint64 Low = 0;
	uint64 High = 0;
	FMemory::Memcpy(&Low, IdInfo.FileId.Identifier, sizeof(Low));
	FMemory::Memcpy(&High, IdInfo.FileId.Identifier + sizeof(Low), sizeof(High));
	return static_cast<uint64>(IdInfo.VolumeSerialNumber) == Expected.VolumeId
		&& Low == Expected.FileIdLow && High == Expected.FileIdHigh;
}

TWoTBootstrapResult<HANDLE> OpenInstallerDataById(
	HANDLE HeldParent, const FWoTNativeFileIdentity& Identity, FStringView Path)
{
	using FResult = TWoTBootstrapResult<HANDLE>;
	FILE_ID_DESCRIPTOR Descriptor = {};
	Descriptor.dwSize = sizeof(Descriptor);
	Descriptor.Type = ExtendedFileIdType;
	FMemory::Memcpy(Descriptor.ExtendedFileId.Identifier,
		&Identity.FileIdLow, sizeof(Identity.FileIdLow));
	FMemory::Memcpy(Descriptor.ExtendedFileId.Identifier + sizeof(Identity.FileIdLow),
		&Identity.FileIdHigh, sizeof(Identity.FileIdHigh));
	// OpenFileById defines hVolumeHint as a handle to any file on the same
	// volume. The already-held parent is therefore the least-authority hint and
	// keeps this reopen bound to the validated directory walk.
	const HANDLE Data = ::OpenFileById(HeldParent, &Descriptor, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		FILE_FLAG_BACKUP_SEMANTICS);
	const uint32 OpenError = Data == INVALID_HANDLE_VALUE
		? ::GetLastError() : ERROR_SUCCESS;
	if (Data == INVALID_HANDLE_VALUE)
	{
		return FResult::Error(SystemError(
			TEXT("OpenFileById(installer data)"), OpenError, Path));
	}
	TOptional<FWoTBootstrapError> IdentityError;
	if (!HandleMatchesObjectId(Data, Identity, Path, IdentityError))
	{
		(void)::CloseHandle(Data);
		if (IdentityError.IsSet())
		{
			return FResult::Error(MoveTemp(IdentityError.GetValue()));
		}
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the filter-following installer data handle is not the validated object"),
			FString(Path)));
	}
	return FResult::Success(Data);
}

HANDLE DataHandleForRead(const FWoTNativeHandleState& State)
{
	return State.DataHandle != INVALID_HANDLE_VALUE && State.DataHandle != nullptr
		? static_cast<HANDLE>(State.DataHandle)
		: static_cast<HANDLE>(State.Handle);
}

FWoTBootstrapError InstallerOnlineOnlyError(FStringView Path)
{
	return MakeWoTBootstrapError(EWoTBootstrapError::ServiceUnavailable,
		FString::Printf(TEXT("The installer at %s is an online-only file. Make it "
			"available on this device (in File Explorer, right-click it and choose "
			"'Always keep on this device'), then start WoT Reborn again."),
			*FString(Path)), FString(Path));
}

TOptional<FWoTBootstrapError> RequireInstallerSourceLocalBeforeRead(
	const FWoTNativeFileHandle& File);

TWoTBootstrapResult<HANDLE> OpenPathNoFollow(FStringView Path, bool bDirectory)
{
	using FResult = TWoTBootstrapResult<HANDLE>;

	const FString Full(Path);
	const DWORD Flags = FILE_FLAG_OPEN_REPARSE_POINT
		| (bDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0);

	// FILE_SHARE_DELETE is included deliberately: holding a file must not stop a
	// concurrent rename or delete. Detecting that it happened is the job of the
	// identity checks, not of locking the filesystem against the rest of the system.
	const HANDLE Handle = ::CreateFileW(*Full, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, Flags, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return FResult::Error(SystemError(TEXT("CreateFileW"), ::GetLastError(), Full));
	}
	return FResult::Success(Handle);
}

TWoTBootstrapResult<HANDLE> OpenOneRelative(const FWoTNativeHandleState& Root,
	FStringView Leaf, ACCESS_MASK Access, ULONG Disposition, ULONG Options,
	bool* bOutCreated = nullptr)
{
	using FResult = TWoTBootstrapResult<HANDLE>;
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FWoTNtCreateFile NtCreateFile = ResolveNtCreateFile();
	if (NtCreateFile == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("NtCreateFile is unavailable from ntdll"), FString(Leaf)));
	}
	FString LeafString(Leaf);
	UNICODE_STRING Name = {};
	Name.Buffer = const_cast<PWSTR>(*LeafString);
	Name.Length = static_cast<USHORT>(LeafString.Len() * sizeof(TCHAR));
	Name.MaximumLength = Name.Length;
	OBJECT_ATTRIBUTES Attributes = {};
	Attributes.Length = sizeof(Attributes);
	Attributes.RootDirectory = static_cast<HANDLE>(Root.Handle);
	Attributes.ObjectName = &Name;
	Attributes.Attributes = OBJ_CASE_INSENSITIVE;
	IO_STATUS_BLOCK IoStatus = {};
	HANDLE Handle = INVALID_HANDLE_VALUE;
	const NTSTATUS Status = NtCreateFile(&Handle, Access, &Attributes, &IoStatus,
		nullptr, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, Disposition,
		Options | FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT,
		nullptr, 0);
	if (Status < 0)
	{
		return FResult::Error(SystemError(TEXT("NtCreateFile"),
			NtStatusToWindowsError(Status), Leaf));
	}
	if (bOutCreated != nullptr)
	{
		// NtCreateFile reports FILE_CREATED as information value 2. Use the ABI
		// value directly because some Windows SDK winternl.h versions omit its name.
		*bOutCreated = IoStatus.Information == 2;
	}
	return FResult::Success(Handle);
}

/** Exactly a drive letter, a colon and one separator: a drive root opened absolutely. */
bool IsDriveRootPath(FStringView Path)
{
	return Path.Len() == 3 && FChar::IsAlpha(Path[0]) && Path[1] == TEXT(':')
		&& (Path[2] == TEXT('/') || Path[2] == TEXT('\\'));
}

/**
 * Under Wine only: whether an absolutely opened reparse directory is a mount
 * point, i.e. its volume differs from its parent path's. Native Windows never
 * asks (and keeps every reparse point a link); any failed query answers false.
 */
bool WineVolumeBoundaryForAbsolutePath(HANDLE Handle, FStringView Path)
{
	if (!WoTIsRunningUnderWine() || IsDriveRootPath(Path))
	{
		return false;
	}
	FILE_ATTRIBUTE_TAG_INFO Tag = {};
	constexpr DWORD ReparseDirectory = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY;
	if (!::GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Tag, sizeof(Tag))
		|| (Tag.FileAttributes & ReparseDirectory) != ReparseDirectory)
	{
		return false;
	}
	FString Parent = FPaths::GetPath(FString(Path));
	if (Parent.Len() == 2 && Parent[1] == TEXT(':'))
	{
		Parent += TEXT("/");
	}
	if (Parent.IsEmpty())
	{
		return false;
	}
	// The parent is opened by path after the child is held. That is enough
	// here: it is consulted only under Wine and only for a reparse directory,
	// and a junction object lives on its parent's volume, so only a genuine
	// mount point differs. Swapping the parent in between needs a same-user
	// process, which under Proton can already change the install directly.
	const HANDLE ParentHandle = ::CreateFileW(*Parent, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (ParentHandle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	FILE_ID_INFO ParentId = {};
	FILE_ID_INFO ChildId = {};
	const bool bKnown =
		::GetFileInformationByHandleEx(ParentHandle, FileIdInfo, &ParentId, sizeof(ParentId))
		&& ::GetFileInformationByHandleEx(Handle, FileIdInfo, &ChildId, sizeof(ChildId));
	::CloseHandle(ParentHandle);
	return bKnown && ParentId.VolumeSerialNumber != ChildId.VolumeSerialNumber;
}

template<class HandleType>
HANDLE HeldHandle(const HandleType& Held)
{
	return static_cast<HANDLE>(FWoTNativeHandleAccess::State(Held)->Handle);
}

TOptional<FWoTBootstrapError> RejectLink(const FWoTNativeFileIdentity& Identity,
	FStringView Path, HANDLE Handle)
{
	if (Identity.Type == EWoTTreeEntryType::SymbolicLink)
	{
		FWoTBootstrapError Error = MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the path component is a reparse point"), FString(Path));
		// The tag names the kind of link in a field report (0xA000000C symbolic
		// link, 0xA0000003 junction or mount point). A failed query keeps 0.
		uint32 Tag = 0;
		TOptional<FWoTBootstrapError> TagError;
		if (QueryReparseTag(Handle, Path, Tag, TagError))
		{
			Error.NativeCode = Tag;
		}
		return Error;
	}
	return {};
}

template<class HandleType>
TWoTBootstrapResult<HandleType> AdoptHandle(HANDLE Handle, FStringView Path,
	bool bInstallerSource = false, HANDLE DataHandle = INVALID_HANDLE_VALUE,
	bool bInstallerOnlineOnlyAtOpen = false, bool bDriveRoot = false,
	bool bVolumeBoundary = false)
{
	using FResult = TWoTBootstrapResult<HandleType>;

	TUniquePtr<FWoTNativeHandleState> State = MakeUnique<FWoTNativeHandleState>();
	State->Handle = Handle;
	State->DataHandle = DataHandle;
	State->Path = FString(Path);
	State->bInstallerSource = bInstallerSource;
	State->bInstallerOnlineOnlyAtOpen = bInstallerOnlineOnlyAtOpen;
	State->bDriveRoot = bDriveRoot;
	State->bVolumeBoundary = bVolumeBoundary;
	FWoTReparseComponentFacts Context;
	Context.bDriveRoot = bDriveRoot;
	Context.bVolumeBoundary = bVolumeBoundary;
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromHandle(Handle, Path, State->Identity,
				bInstallerSource, &State->bInstallerDataReparse, &Context); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (State->bInstallerDataReparse
		&& (DataHandle == INVALID_HANDLE_VALUE || DataHandle == nullptr)
		&& !bInstallerOnlineOnlyAtOpen)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("an accepted installer data reparse point has no filter-following handle"),
			FString(Path)));
	}

	HandleType Result;
	FWoTNativeHandleAccess::State(Result) = MoveTemp(State);
	return FResult::Success(MoveTemp(Result));
}

template<class HandleType>
TWoTBootstrapResult<HandleType> DuplicateHeldHandle(const HandleType& Source)
{
	using FResult = TWoTBootstrapResult<HandleType>;

	const TUniquePtr<FWoTNativeHandleState>& State = FWoTNativeHandleAccess::State(Source);
	if (!State.IsValid() || State->Handle == INVALID_HANDLE_VALUE)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the handle is not open")));
	}

	HANDLE Duplicate = INVALID_HANDLE_VALUE;
	if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(State->Handle),
			// 0, not FALSE: HideWindowsPlatformTypes.h undefines TRUE/FALSE
			// (:23,:27), and every use in this file is below that include.
			::GetCurrentProcess(), &Duplicate, 0, 0, DUPLICATE_SAME_ACCESS))
	{
		return FResult::Error(SystemError(TEXT("DuplicateHandle"), ::GetLastError(),
			State->Path));
	}

	HANDLE DataDuplicate = INVALID_HANDLE_VALUE;
	if (State->DataHandle != INVALID_HANDLE_VALUE && State->DataHandle != nullptr
		&& !::DuplicateHandle(::GetCurrentProcess(),
			static_cast<HANDLE>(State->DataHandle), ::GetCurrentProcess(),
			&DataDuplicate, 0, 0, DUPLICATE_SAME_ACCESS))
	{
		const uint32 Error = ::GetLastError();
		(void)::CloseHandle(Duplicate);
		return FResult::Error(SystemError(
			TEXT("DuplicateHandle(installer data)"), Error, State->Path));
	}
	TWoTBootstrapResult<HandleType> Adopted = AdoptHandle<HandleType>(
		Duplicate, State->Path, State->bInstallerSource, DataDuplicate,
		State->bInstallerOnlineOnlyAtOpen, State->bDriveRoot, State->bVolumeBoundary);
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (FWoTNativeHandleAccess::State(Adopted.GetValue())->Identity != State->Identity)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the held object changed identity while it was open")));
	}
	return Adopted;
}

/** Walk one relative path beneath a held directory, relative to each held HANDLE. */
TWoTBootstrapResult<HANDLE> OpenRelativeThroughComponents(
	const FWoTNativeHandleState& Root, FStringView RelativePath,
	bool bFinalIsDirectory, bool bAllowFinalInstallerDataReparse = false,
	HANDLE* OutInstallerDataHandle = nullptr,
	bool* bOutInstallerOnlineOnly = nullptr,
	bool* bOutFinalVolumeBoundary = nullptr)
{
	using FResult = TWoTBootstrapResult<HANDLE>;
	if (bOutFinalVolumeBoundary != nullptr)
	{
		*bOutFinalVolumeBoundary = false;
	}
	if (OutInstallerDataHandle != nullptr)
	{
		*OutInstallerDataHandle = INVALID_HANDLE_VALUE;
	}
	if (bOutInstallerOnlineOnly != nullptr)
	{
		*bOutInstallerOnlineOnly = false;
	}

	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(RelativePath); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}

	TArray<FString> Components;
	FString(RelativePath).ParseIntoArray(Components, TEXT("/"), true);

	const FWoTNtCreateFile NtCreateFile = ResolveNtCreateFile();
	if (NtCreateFile == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("NtCreateFile is unavailable from ntdll"), FString(RelativePath)));
	}

	HANDLE Current = INVALID_HANDLE_VALUE;
	if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(Root.Handle),
			::GetCurrentProcess(), &Current, 0, 0, DUPLICATE_SAME_ACCESS))
	{
		return FResult::Error(SystemError(TEXT("DuplicateHandle"), ::GetLastError(),
			RelativePath));
	}

	// Under Wine a component whose volume differs from its parent's is one of
	// the mount points Wine presents for the Unix filesystem. Native Windows
	// never needs the volumes. An unknown volume is never a boundary.
	const bool bTrackVolumes = WoTIsRunningUnderWine();
	uint64 ParentVolume = 0;
	bool bParentVolumeKnown = false;
	if (bTrackVolumes)
	{
		FILE_ID_INFO RootId = {};
		bParentVolumeKnown = ::GetFileInformationByHandleEx(
			Current, FileIdInfo, &RootId, sizeof(RootId)) != 0;
		ParentVolume = RootId.VolumeSerialNumber;
	}

	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		const bool bFinal = Index == Components.Num() - 1;
		const bool bDirectory = !bFinal || bFinalIsDirectory;
		FString& Component = Components[Index];
		UNICODE_STRING Name = {};
		Name.Buffer = const_cast<PWSTR>(*Component);
		Name.Length = static_cast<USHORT>(Component.Len() * sizeof(TCHAR));
		Name.MaximumLength = Name.Length;
		OBJECT_ATTRIBUTES Attributes = {};
		Attributes.Length = sizeof(Attributes);
		Attributes.RootDirectory = Current;
		Attributes.ObjectName = &Name;
		Attributes.Attributes = OBJ_CASE_INSENSITIVE;

		IO_STATUS_BLOCK IoStatus = {};
		HANDLE Next = INVALID_HANDLE_VALUE;
		const ULONG Options = FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT
			| FILE_OPEN_FOR_BACKUP_INTENT
			| (bDirectory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
		const NTSTATUS Status = NtCreateFile(&Next, GENERIC_READ | SYNCHRONIZE,
			&Attributes, &IoStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
			Options, nullptr, 0);
		if (Status < 0)
		{
			::CloseHandle(Current);
			Current = INVALID_HANDLE_VALUE;
			return FResult::Error(SystemError(TEXT("NtCreateFile"),
				NtStatusToWindowsError(Status), Component));
		}

		FWoTReparseComponentFacts Context;
		uint64 NextVolume = 0;
		bool bNextVolumeKnown = false;
		if (bTrackVolumes)
		{
			FILE_ID_INFO NextId = {};
			bNextVolumeKnown = ::GetFileInformationByHandleEx(
				Next, FileIdInfo, &NextId, sizeof(NextId)) != 0;
			NextVolume = NextId.VolumeSerialNumber;
			Context.bVolumeBoundary = bParentVolumeKnown && bNextVolumeKnown
				&& NextVolume != ParentVolume;
		}

		FWoTNativeFileIdentity Identity;
		bool bInstallerDataReparse = false;
		if (TOptional<FWoTBootstrapError> Error =
				IdentityFromHandle(Next, Component, Identity,
					bFinal && bAllowFinalInstallerDataReparse,
					&bInstallerDataReparse, &Context); Error.IsSet())
		{
			::CloseHandle(Current);
			Current = INVALID_HANDLE_VALUE;
			::CloseHandle(Next);
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> LinkError = RejectLink(Identity, Component, Next);
			LinkError.IsSet())
		{
			::CloseHandle(Current);
			Current = INVALID_HANDLE_VALUE;
			::CloseHandle(Next);
			return FResult::Error(MoveTemp(LinkError.GetValue()));
		}
		if (bFinal && bOutFinalVolumeBoundary != nullptr)
		{
			*bOutFinalVolumeBoundary = Context.bVolumeBoundary;
		}
		if (!bFinal && Identity.Type != EWoTTreeEntryType::Directory)
		{
			::CloseHandle(Current);
			Current = INVALID_HANDLE_VALUE;
			::CloseHandle(Next);
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("a mid-path component is not a directory"), Component));
		}

		if (bFinal)
		{
			if (bInstallerDataReparse && OutInstallerDataHandle != nullptr)
			{
				// A following open can itself recall a Cloud Files placeholder.
				// Inspect attributes on the no-follow object first and retain only
				// that validation handle when the player must localize it.
				FILE_BASIC_INFO Basic = {};
				if (!::GetFileInformationByHandleEx(
						Next, FileBasicInfo, &Basic, sizeof(Basic)))
				{
					const uint32 AttributeError = ::GetLastError();
					::CloseHandle(Current);
					Current = INVALID_HANDLE_VALUE;
					::CloseHandle(Next);
					return FResult::Error(SystemError(
						TEXT("GetFileInformationByHandleEx(installer locality)"),
						AttributeError, RelativePath));
				}
				const bool bOnlineOnly =
					(static_cast<uint32>(Basic.FileAttributes)
						& WoTInstallerSourceNonLocalAttributes) != 0;
				if (bOutInstallerOnlineOnly != nullptr)
				{
					*bOutInstallerOnlineOnly = bOnlineOnly;
				}
				if (!bOnlineOnly)
				{
					TWoTBootstrapResult<HANDLE> Data = OpenInstallerDataById(
						Current, Identity, RelativePath);
					if (Data.IsError())
					{
						::CloseHandle(Current);
						Current = INVALID_HANDLE_VALUE;
						::CloseHandle(Next);
						return FResult::Error(MoveTemp(Data).GetError());
					}
					*OutInstallerDataHandle = Data.GetValue();
				}
			}
			::CloseHandle(Current);
			Current = INVALID_HANDLE_VALUE;
			return FResult::Success(Next);
		}
		::CloseHandle(Current);
		Current = Next;
		ParentVolume = NextVolume;
		bParentVolumeKnown = bNextVolumeKnown;
	}

	if (Current != INVALID_HANDLE_VALUE)
	{
		::CloseHandle(Current);
	}
	return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
		TEXT("the relative path resolved to nothing"), FString(RelativePath)));
}

// The NTSTATUS values a single-leaf lookup reads as "no such entry". Spelled out
// because ntstatus.h cannot be included beside windows.h without redefinitions.
constexpr NTSTATUS WoTStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS WoTStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003AL);
constexpr NTSTATUS WoTStatusDeletePending = static_cast<NTSTATUS>(0xC0000056L);

/**
 * Opens one leaf beneath Root for attribute reads only and hands back the raw
 * NTSTATUS, because SystemError flattens every failure to Io and a caller that
 * must tell "absent" from "unreadable" cannot decide on error text. The open
 * shares READ|WRITE|DELETE like every other open here, and an attribute-only
 * access mask takes no part in share-mode checks, so it never blocks a writer's
 * rename-over or a delete and is never blocked by a writer that shares nothing.
 * Neither FILE_DIRECTORY_FILE nor FILE_NON_DIRECTORY_FILE: a file or a
 * directory opens alike.
 */
TWoTBootstrapResult<NTSTATUS> OpenOneRelativeForAttributes(
	const FWoTNativeHandleState& Root, FStringView Leaf, HANDLE& OutHandle)
{
	using FResult = TWoTBootstrapResult<NTSTATUS>;
	OutHandle = INVALID_HANDLE_VALUE;
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FWoTNtCreateFile NtCreateFile = ResolveNtCreateFile();
	if (NtCreateFile == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("NtCreateFile is unavailable from ntdll"), FString(Leaf)));
	}
	FString LeafString(Leaf);
	UNICODE_STRING Name = {};
	Name.Buffer = const_cast<PWSTR>(*LeafString);
	Name.Length = static_cast<USHORT>(LeafString.Len() * sizeof(TCHAR));
	Name.MaximumLength = Name.Length;
	OBJECT_ATTRIBUTES Attributes = {};
	Attributes.Length = sizeof(Attributes);
	Attributes.RootDirectory = static_cast<HANDLE>(Root.Handle);
	Attributes.ObjectName = &Name;
	Attributes.Attributes = OBJ_CASE_INSENSITIVE;
	IO_STATUS_BLOCK IoStatus = {};
	HANDLE Handle = INVALID_HANDLE_VALUE;
	const NTSTATUS Status = NtCreateFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
		&Attributes, &IoStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
		FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT
			| FILE_OPEN_FOR_BACKUP_INTENT,
		nullptr, 0);
	if (Status >= 0)
	{
		OutHandle = Handle;
	}
	return FResult::Success(Status);
}
}  // namespace

EWoTInstallerSourceReparseTagClass WoTClassifyInstallerSourceReparseTag(
	uint32 ReparseTag)
{
	return ClassifyInstallerSourceReparseTag(ReparseTag);
}

bool WoTInstallerSourceAttributesAreFullyLocal(uint32 FileAttributes)
{
	return (FileAttributes & WoTInstallerSourceNonLocalAttributes) == 0;
}

TWoTBootstrapResult<uint32> QueryNativeInstallerSourceFileAttributes(
	const FWoTNativeFileHandle& File)
{
	using FResult = TWoTBootstrapResult<uint32>;
	if (!File.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the installer source handle is not open")));
	}
	const FWoTNativeHandleState& State = *FWoTNativeHandleAccess::State(File);
	if (!State.bInstallerSource)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the held file is not an installer source"), State.Path));
	}
#if WITH_AUTOMATION_TESTS
	if (GWoTInstallerSourceAttributeQueryForTesting)
	{
		return GWoTInstallerSourceAttributeQueryForTesting(File);
	}
#endif
	FILE_BASIC_INFO Information = {};
	if (!::GetFileInformationByHandleEx(static_cast<HANDLE>(State.Handle),
			FileBasicInfo, &Information, sizeof(Information)))
	{
		return FResult::Error(SystemError(
			TEXT("GetFileInformationByHandleEx(installer FileBasicInfo)"),
			::GetLastError(), State.Path));
	}
	return FResult::Success(static_cast<uint32>(Information.FileAttributes));
}

TWoTBootstrapResult<bool> QueryNativeVolumeSupportsWof(
	const FWoTNativeDirectoryHandle& Directory)
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the directory handle is not open")));
	}
	WCHAR FileSystemName[MAX_PATH + 1] = {};
	DWORD FileSystemFlags = 0;
	const FWoTNativeHandleState& State =
		*FWoTNativeHandleAccess::State(Directory);
	if (!::GetVolumeInformationByHandleW(static_cast<HANDLE>(State.Handle),
			nullptr, 0, nullptr, nullptr, &FileSystemFlags,
			FileSystemName, UE_ARRAY_COUNT(FileSystemName)))
	{
		return FResult::Error(SystemError(
			TEXT("GetVolumeInformationByHandleW"), ::GetLastError(), State.Path));
	}
	if (FCString::Stricmp(FileSystemName, TEXT("NTFS")) != 0
		|| (FileSystemFlags & FILE_FILE_COMPRESSION) == 0)
	{
		return FResult::Success(false);
	}
	USHORT CompressionFormat = 0;
	DWORD CompressionBytes = 0;
	if (!::DeviceIoControl(static_cast<HANDLE>(State.Handle),
			FSCTL_GET_COMPRESSION, nullptr, 0, &CompressionFormat,
			sizeof(CompressionFormat), &CompressionBytes, nullptr))
	{
		const uint32 CompressionError = ::GetLastError();
		if (CompressionError == ERROR_INVALID_FUNCTION
			|| CompressionError == ERROR_NOT_SUPPORTED)
		{
			return FResult::Success(false);
		}
		return FResult::Error(SystemError(
			TEXT("DeviceIoControl(FSCTL_GET_COMPRESSION)"),
			CompressionError, State.Path));
	}
	const HMODULE WofUtil = ::LoadLibraryExW(
		TEXT("wofutil.dll"), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (WofUtil == nullptr)
	{
		return FResult::Success(false);
	}
	using FWofShouldCompressBinaries = BOOL (WINAPI*)(LPCWSTR, PULONG);
	using FWofGetDriverVersion = HRESULT (WINAPI*)(HANDLE, ULONG, PULONG);
	const FWofShouldCompressBinaries ShouldCompressBinaries =
		reinterpret_cast<FWofShouldCompressBinaries>(
			::GetProcAddress(WofUtil, "WofShouldCompressBinaries"));
	const FWofGetDriverVersion GetDriverVersion =
		reinterpret_cast<FWofGetDriverVersion>(
			::GetProcAddress(WofUtil, "WofGetDriverVersion"));
	if (ShouldCompressBinaries == nullptr || GetDriverVersion == nullptr)
	{
		(void)::FreeLibrary(WofUtil);
		return FResult::Success(false);
	}
	WCHAR VolumeRoot[MAX_PATH + 1] = {};
	if (!::GetVolumePathNameW(*State.Path, VolumeRoot, UE_ARRAY_COUNT(VolumeRoot)))
	{
		const uint32 VolumePathError = ::GetLastError();
		(void)::FreeLibrary(WofUtil);
		return FResult::Error(SystemError(
			TEXT("GetVolumePathNameW(WOF capability)"),
			VolumePathError, State.Path));
	}
	ULONG PreferredAlgorithm = 0;
	if (!ShouldCompressBinaries(VolumeRoot, &PreferredAlgorithm))
	{
		(void)::FreeLibrary(WofUtil);
		return FResult::Success(false);
	}
	constexpr ULONG WofProviderFile = 2;
	ULONG Version = 0;
	const HRESULT DriverResult = GetDriverVersion(
		static_cast<HANDLE>(State.Handle), WofProviderFile, &Version);
	(void)::FreeLibrary(WofUtil);
	if (DriverResult == S_OK)
	{
		return FResult::Success(true);
	}
	if (DriverResult == HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION))
	{
		return FResult::Success(false);
	}
	FWoTBootstrapError Error = MakeWoTBootstrapError(EWoTBootstrapError::Io,
		TEXT("WofGetDriverVersion failed while querying WOF volume support"),
		State.Path);
	Error.NativeCode = static_cast<uint32>(DriverResult);
	return FResult::Error(MoveTemp(Error));
}

#if WITH_AUTOMATION_TESTS
void WoTSetNativeInstallerSourceAttributeQueryForTesting(
	FWoTNativeInstallerSourceAttributeQueryForTesting InQuery)
{
	GWoTInstallerSourceAttributeQueryForTesting = MoveTemp(InQuery);
}

void WoTSetNativeInstallerSourceReadObserverForTesting(
	FWoTNativeInstallerSourceReadObserverForTesting InObserver)
{
	GWoTInstallerSourceReadObserverForTesting = MoveTemp(InObserver);
}
#endif

namespace
{
TOptional<FWoTBootstrapError> RequireInstallerSourceLocalBeforeRead(
	const FWoTNativeFileHandle& File)
{
	const FWoTNativeHandleState& State = *FWoTNativeHandleAccess::State(File);
	if (!State.bInstallerSource)
	{
		return {};
	}
	TWoTBootstrapResult<uint32> Attributes =
		QueryNativeInstallerSourceFileAttributes(File);
	if (Attributes.IsError())
	{
		return MoveTemp(Attributes).GetError();
	}
	if (!WoTInstallerSourceAttributesAreFullyLocal(Attributes.GetValue()))
	{
		return InstallerOnlineOnlyError(State.Path);
	}
	if (State.bInstallerOnlineOnlyAtOpen)
	{
		return InstallerOnlineOnlyError(State.Path);
	}
	return {};
}
} // namespace

// --- FWoTNativeFileLock ------------------------------------------------------

FWoTNativeFileLock::FWoTNativeFileLock() = default;

FWoTNativeFileLock::~FWoTNativeFileLock()
{
	if (State.IsValid())
	{
		FWoTBootstrapStatus Released = Release();
		ensureMsgf(!Released.IsError(), TEXT("native file-lock release failed: %s"),
			Released.IsError() ? *Released.GetError().ToString() : TEXT(""));
	}
}

FWoTNativeFileLock::FWoTNativeFileLock(FWoTNativeFileLock&& Other) = default;

FWoTNativeFileLock& FWoTNativeFileLock::operator=(FWoTNativeFileLock&& Other)
{
	if (this != &Other)
	{
		if (State.IsValid())
		{
			FWoTBootstrapStatus Released = Release();
			ensureMsgf(!Released.IsError(), TEXT("native file-lock release failed: %s"),
				Released.IsError() ? *Released.GetError().ToString() : TEXT(""));
		}
		State = MoveTemp(Other.State);
	}
	return *this;
}

bool FWoTNativeFileLock::IsValid() const
{
	return State.IsValid() && State->Handle != INVALID_HANDLE_VALUE
		&& State->Handle != nullptr && State->bLocked;
}

FWoTBootstrapStatus FWoTNativeFileLock::Release()
{
	if (!State.IsValid())
	{
		return FWoTBootstrapStatus::Success();
	}
	const HANDLE Handle = static_cast<HANDLE>(State->Handle);
	const bool bWasLocked = State->bLocked;
	State->Handle = INVALID_HANDLE_VALUE;
	State->bLocked = false;
	const bool bUnlocked = !bWasLocked || ::UnlockFileEx(Handle, 0, 1, 0, &State->Range) != 0;
	const uint32 UnlockError = bUnlocked ? ERROR_SUCCESS : ::GetLastError();
	const bool bClosed = ::CloseHandle(Handle) != 0;
	const uint32 CloseError = bClosed ? ERROR_SUCCESS : ::GetLastError();
	State.Reset();
	if (!bUnlocked)
	{
		return SystemStatus(TEXT("UnlockFileEx"), UnlockError, TEXT(""));
	}
	if (!bClosed)
	{
		return SystemStatus(TEXT("CloseHandle"), CloseError, TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

// --- FWoTNativeFileHandle ----------------------------------------------------

FWoTNativeFileHandle::FWoTNativeFileHandle() = default;
FWoTNativeFileHandle::~FWoTNativeFileHandle() = default;
FWoTNativeFileHandle::FWoTNativeFileHandle(FWoTNativeFileHandle&& Other) = default;
FWoTNativeFileHandle& FWoTNativeFileHandle::operator=(FWoTNativeFileHandle&& Other) = default;

bool FWoTNativeFileHandle::IsValid() const
{
	return State.IsValid() && State->Handle != INVALID_HANDLE_VALUE;
}

const FWoTNativeFileIdentity& FWoTNativeFileHandle::GetIdentity() const
{
	check(State.IsValid());
	return State->Identity;
}

TWoTBootstrapResult<FWoTNativeFileIdentity> FWoTNativeFileHandle::QueryCurrentIdentity() const
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileIdentity>;
	if (!IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the file handle is not open")));
	}
	FWoTNativeFileIdentity Identity;
	FWoTReparseComponentFacts Context;
	Context.bDriveRoot = State->bDriveRoot;
	Context.bVolumeBoundary = State->bVolumeBoundary;
	if (TOptional<FWoTBootstrapError> Error = IdentityFromHandle(
			static_cast<HANDLE>(State->Handle), State->Path, Identity,
			State->bInstallerDataReparse, nullptr, &Context); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	return FResult::Success(Identity);
}

TWoTBootstrapResult<FWoTNativeFileHandle> FWoTNativeFileHandle::DuplicateForRead() const
{
	return DuplicateHeldHandle<FWoTNativeFileHandle>(*this);
}

TWoTBootstrapResult<TArray<uint8>> FWoTNativeFileHandle::ReadAll() const
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (!IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the file handle is not open")));
	}
	if (State->Identity.Type != EWoTTreeEntryType::File)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("only a regular file can be read")));
	}
	if (TOptional<FWoTBootstrapError> Local =
			RequireInstallerSourceLocalBeforeRead(*this); Local.IsSet())
	{
		return FResult::Error(MoveTemp(Local.GetValue()));
	}
	if (State->Identity.Size > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("the file is too large to fit in a byte array"), State->Path));
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(State->Identity.Size));
	uint64 Offset = 0;
	while (Offset < State->Identity.Size)
	{
		OVERLAPPED Overlapped = {};
		Overlapped.Offset = static_cast<DWORD>(Offset & 0xFFFFFFFFull);
		Overlapped.OffsetHigh = static_cast<DWORD>(Offset >> 32);

		const uint64 Remaining = State->Identity.Size - Offset;
		const DWORD Request = static_cast<DWORD>(
			Remaining > 0x10000000ull ? 0x10000000ull : Remaining);
		DWORD Read = 0;
		// Offsets are supplied explicitly so reading never disturbs a shared file
		// pointer, matching the macOS pread path.
	#if WITH_AUTOMATION_TESTS
		if (State->bInstallerSource && GWoTInstallerSourceReadObserverForTesting)
		{
			GWoTInstallerSourceReadObserverForTesting();
		}
	#endif
		if (!::ReadFile(DataHandleForRead(*State), Bytes.GetData() + Offset,
				Request, &Read, &Overlapped))
		{
			return FResult::Error(SystemError(TEXT("ReadFile"), ::GetLastError(),
				State->Path));
		}
		if (Read == 0)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
				TEXT("the file ended before its recorded size")));
		}
		Offset += Read;
	}
	return FResult::Success(MoveTemp(Bytes));
}

// --- FWoTNativeDirectoryHandle -----------------------------------------------

FWoTNativeDirectoryHandle::FWoTNativeDirectoryHandle() = default;
FWoTNativeDirectoryHandle::~FWoTNativeDirectoryHandle() = default;
FWoTNativeDirectoryHandle::FWoTNativeDirectoryHandle(FWoTNativeDirectoryHandle&& Other) = default;
FWoTNativeDirectoryHandle& FWoTNativeDirectoryHandle::operator=(
	FWoTNativeDirectoryHandle&& Other) = default;

bool FWoTNativeDirectoryHandle::IsValid() const
{
	return State.IsValid() && State->Handle != INVALID_HANDLE_VALUE;
}

const FWoTNativeFileIdentity& FWoTNativeDirectoryHandle::GetIdentity() const
{
	check(State.IsValid());
	return State->Identity;
}

TWoTBootstrapResult<FWoTNativeFileIdentity> FWoTNativeDirectoryHandle::QueryCurrentIdentity() const
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileIdentity>;
	if (!IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	FWoTNativeFileIdentity Identity;
	FWoTReparseComponentFacts Context;
	Context.bDriveRoot = State->bDriveRoot;
	Context.bVolumeBoundary = State->bVolumeBoundary;
	if (TOptional<FWoTBootstrapError> Error = IdentityFromHandle(
			static_cast<HANDLE>(State->Handle), State->Path, Identity, false, nullptr,
			&Context); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	return FResult::Success(Identity);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> FWoTNativeDirectoryHandle::DuplicateForRead() const
{
	return DuplicateHeldHandle<FWoTNativeDirectoryHandle>(*this);
}

// --- FWoTNativeExecutableIdentity --------------------------------------------

FWoTNativeExecutableIdentity::FWoTNativeExecutableIdentity() = default;
FWoTNativeExecutableIdentity::~FWoTNativeExecutableIdentity() = default;
FWoTNativeExecutableIdentity::FWoTNativeExecutableIdentity(
	FWoTNativeExecutableIdentity&& Other) = default;
FWoTNativeExecutableIdentity& FWoTNativeExecutableIdentity::operator=(
	FWoTNativeExecutableIdentity&& Other) = default;

bool FWoTNativeExecutableIdentity::IsValid() const
{
	return State.IsValid() && State->Handle != INVALID_HANDLE_VALUE;
}

const FWoTNativeFileIdentity& FWoTNativeExecutableIdentity::GetIdentity() const
{
	check(State.IsValid());
	return State->Identity;
}

TWoTBootstrapResult<FWoTNativeFileIdentity>
	FWoTNativeExecutableIdentity::QueryCurrentIdentity() const
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileIdentity>;
	if (!IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the executable handle is not open")));
	}
	FWoTNativeFileIdentity Identity;
	FWoTReparseComponentFacts Context;
	Context.bDriveRoot = State->bDriveRoot;
	Context.bVolumeBoundary = State->bVolumeBoundary;
	if (TOptional<FWoTBootstrapError> Error = IdentityFromHandle(
			static_cast<HANDLE>(State->Handle), State->Path, Identity, false, nullptr,
			&Context); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	return FResult::Success(Identity);
}

const FString& FWoTNativeExecutableIdentity::GetReportedPath() const
{
	return ReportedPath;
}

// --- entry points ------------------------------------------------------------

TWoTBootstrapResult<FWoTNativeFileHandle> OpenNativeFileReadNoFollow(
	FStringView CanonicalAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;

	TWoTBootstrapResult<HANDLE> Handle = OpenPathNoFollow(CanonicalAbsolutePath, false);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Adopted =
		AdoptHandle<FWoTNativeFileHandle>(Handle.GetValue(), CanonicalAbsolutePath, false,
			INVALID_HANDLE_VALUE, false, IsDriveRootPath(CanonicalAbsolutePath),
			WineVolumeBoundaryForAbsolutePath(Handle.GetValue(), CanonicalAbsolutePath));
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (TOptional<FWoTBootstrapError> LinkError = RejectLink(
			Adopted.GetValue().GetIdentity(), CanonicalAbsolutePath,
			HeldHandle(Adopted.GetValue())); LinkError.IsSet())
	{
		return FResult::Error(MoveTemp(LinkError.GetValue()));
	}
	return Adopted;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenNativeDirectoryNoFollow(
	FStringView CanonicalAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;

	TWoTBootstrapResult<HANDLE> Handle = OpenPathNoFollow(CanonicalAbsolutePath, true);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Adopted =
		AdoptHandle<FWoTNativeDirectoryHandle>(Handle.GetValue(), CanonicalAbsolutePath, false,
			INVALID_HANDLE_VALUE, false, IsDriveRootPath(CanonicalAbsolutePath),
			WineVolumeBoundaryForAbsolutePath(Handle.GetValue(), CanonicalAbsolutePath));
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (TOptional<FWoTBootstrapError> LinkError = RejectLink(
			Adopted.GetValue().GetIdentity(), CanonicalAbsolutePath,
			HeldHandle(Adopted.GetValue())); LinkError.IsSet())
	{
		return FResult::Error(MoveTemp(LinkError.GetValue()));
	}
	if (Adopted.GetValue().GetIdentity().Type != EWoTTreeEntryType::Directory)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the path is not a directory"), FString(CanonicalAbsolutePath)));
	}
	return Adopted;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle>
WoTOpenNativeDirectoryFollowingForCanonicalization(
	FStringView LaunchAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	const FString Full(LaunchAbsolutePath);
	const bool bDrivePath = Full.Len() >= 3 && FChar::IsAlpha(Full[0])
		&& Full[1] == TEXT(':')
		&& (Full[2] == TEXT('/') || Full[2] == TEXT('\\'));
	const bool bUncPath = Full.StartsWith(
		TEXT("//"), ESearchCase::CaseSensitive)
		|| Full.StartsWith(TEXT("\\\\"), ESearchCase::CaseSensitive);
	if (!bDrivePath && !bUncPath)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("an absolute ancestor path is required"), Full));
	}
	const HANDLE Handle = ::CreateFileW(*Full, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return FResult::Error(SystemError(
			TEXT("CreateFileW(canonical ancestor)"), ::GetLastError(), Full));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Adopted =
		AdoptHandle<FWoTNativeDirectoryHandle>(Handle, Full);
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (Adopted.GetValue().GetIdentity().Type
		!= EWoTTreeEntryType::Directory)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the canonical ancestor is not a directory"), Full));
	}
	return Adopted;
}

TWoTBootstrapResult<FString> QueryNativeDirectoryPath(
	const FWoTNativeDirectoryHandle& Directory)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	const FWoTNativeHandleState& State =
		*FWoTNativeHandleAccess::State(Directory);
#if WITH_AUTOMATION_TESTS
	extern FString GWoTFailDirectoryFinalPathQueryForTesting;
	if (!GWoTFailDirectoryFinalPathQueryForTesting.IsEmpty()
		&& FString(State.Path).Equals(
			GWoTFailDirectoryFinalPathQueryForTesting,
			ESearchCase::IgnoreCase))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the held directory has no DOS or UNC path"), State.Path));
	}
#endif
	constexpr DWORD Flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
	DWORD Capacity = ::GetFinalPathNameByHandleW(
		static_cast<HANDLE>(State.Handle), nullptr, 0, Flags);
	if (Capacity == 0)
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW"),
			::GetLastError(), State.Path));
	}
	TArray<WCHAR> Buffer;
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		Buffer.SetNumUninitialized(static_cast<int32>(Capacity) + 1);
		const DWORD Length = ::GetFinalPathNameByHandleW(
			static_cast<HANDLE>(State.Handle), Buffer.GetData(),
			static_cast<DWORD>(Buffer.Num()), Flags);
		if (Length == 0)
		{
			return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW"),
				::GetLastError(), State.Path));
		}
		if (Length >= static_cast<DWORD>(Buffer.Num()))
		{
			Capacity = Length;
			continue;
		}

		FString Path(static_cast<int32>(Length), Buffer.GetData());
		if (Path.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::CaseSensitive))
		{
			Path = TEXT("//") + Path.RightChop(8);
		}
		else if (Path.StartsWith(TEXT("\\\\?\\"), ESearchCase::CaseSensitive))
		{
			Path = Path.RightChop(4);
		}
		Path.ReplaceInline(TEXT("\\"), TEXT("/"),
			ESearchCase::CaseSensitive);
		const bool bDrivePath = Path.Len() >= 3 && FChar::IsAlpha(Path[0])
			&& Path[1] == TEXT(':') && Path[2] == TEXT('/');
		const bool bUncPath = Path.StartsWith(
			TEXT("//"), ESearchCase::CaseSensitive)
			&& !Path.StartsWith(TEXT("//?/"), ESearchCase::CaseSensitive);
		if (!bDrivePath && !bUncPath)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsupportedProtocol,
				TEXT("the held directory has no DOS or UNC path"), Path));
		}
		return FResult::Success(MoveTemp(Path));
	}
	return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
		TEXT("the held directory path changed length repeatedly"), State.Path));
}

namespace
{
#if WITH_AUTOMATION_TESTS
TAtomic<bool> GWoTNativeVolumeGuidPathFailsForTesting{false};
#endif

/** "\\\\?\\Volume{...}\\" for the volume holding Handle. */
TWoTBootstrapResult<FString> VolumeGuidRootOfHandle(HANDLE Handle)
{
	using FResult = TWoTBootstrapResult<FString>;
	constexpr DWORD Flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
	const DWORD Capacity = ::GetFinalPathNameByHandleW(Handle, nullptr, 0, Flags);
	if (Capacity == 0)
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(volume)"),
			::GetLastError(), TEXT("held directory")));
	}
	TArray<WCHAR> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(Capacity) + 1);
	const DWORD Length = ::GetFinalPathNameByHandleW(
		Handle, Buffer.GetData(), static_cast<DWORD>(Buffer.Num()), Flags);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num()))
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(volume)"),
			::GetLastError(), TEXT("held directory")));
	}
	FString VolumePath(static_cast<int32>(Length), Buffer.GetData());
	const int32 GuidEnd = VolumePath.Find(
		TEXT("}\\"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (!VolumePath.StartsWith(TEXT("\\\\?\\Volume{"),
			ESearchCase::CaseSensitive) || GuidEnd == INDEX_NONE)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the held directory has no volume-GUID path"), VolumePath));
	}
	VolumePath.LeftInline(GuidEnd + 2, EAllowShrinking::No);
	return FResult::Success(MoveTemp(VolumePath));
}

/** The held directory's own DOS path with a trailing separator ("Z:\\home\\...\\"). */
TWoTBootstrapResult<FString> DosDirectoryOfHandle(HANDLE Handle)
{
	using FResult = TWoTBootstrapResult<FString>;
	constexpr DWORD Flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
	const DWORD Capacity = ::GetFinalPathNameByHandleW(Handle, nullptr, 0, Flags);
	if (Capacity == 0)
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(dos)"),
			::GetLastError(), TEXT("held directory")));
	}
	TArray<WCHAR> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(Capacity) + 1);
	const DWORD Length = ::GetFinalPathNameByHandleW(
		Handle, Buffer.GetData(), static_cast<DWORD>(Buffer.Num()), Flags);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num()))
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(dos)"),
			::GetLastError(), TEXT("held directory")));
	}
	FString Path(static_cast<int32>(Length), Buffer.GetData());
	if (Path.StartsWith(TEXT("\\\\?\\"), ESearchCase::CaseSensitive) && Path.Len() > 6
		&& FChar::IsAlpha(Path[4]) && Path[5] == TEXT(':'))
	{
		Path.RightChopInline(4, EAllowShrinking::No);
	}
	if (!Path.EndsWith(TEXT("\\"), ESearchCase::CaseSensitive))
	{
		Path += TEXT("\\");
	}
	return FResult::Success(MoveTemp(Path));
}
}  // namespace

TWoTBootstrapResult<uint64> QueryNativeDirectoryAvailableBytes(
	const FWoTNativeDirectoryHandle& Directory)
{
	using FResult = TWoTBootstrapResult<uint64>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	const HANDLE Handle = static_cast<HANDLE>(
		FWoTNativeHandleAccess::State(Directory)->Handle);
	TWoTBootstrapResult<FString> Root = VolumeGuidRootOfHandle(Handle);
#if WITH_AUTOMATION_TESTS
	if (GWoTNativeVolumeGuidPathFailsForTesting.Load())
	{
		Root = TWoTBootstrapResult<FString>::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the held directory has no volume-GUID path (forced)")));
	}
#endif
	if (Root.IsError())
	{
		// Under Wine (Proton) the Unix drive Z: has no volume GUID: the GUID
		// query fails with error 4390 on every Z: handle, measured on a Steam
		// Deck. Its DOS path answers GetDiskFreeSpaceExW with the real mount's
		// free space, and this value only budgets a preflight, so the held
		// directory's own DOS path is enough there. Native Windows keeps the
		// GUID path and fails as before.
		if (!WoTIsRunningUnderWine())
		{
			return FResult::Error(MoveTemp(Root).GetError());
		}
		TWoTBootstrapResult<FString> Dos = DosDirectoryOfHandle(Handle);
		if (Dos.IsError())
		{
			return FResult::Error(MoveTemp(Root).GetError());
		}
		Root = MoveTemp(Dos);
	}
	ULARGE_INTEGER Available = {};
	if (!::GetDiskFreeSpaceExW(*Root.GetValue(), &Available, nullptr, nullptr))
	{
		return FResult::Error(SystemError(TEXT("GetDiskFreeSpaceExW"),
			::GetLastError(), Root.GetValue()));
	}
	return FResult::Success(static_cast<uint64>(Available.QuadPart));
}

TWoTBootstrapResult<FString> QueryNativeFilePath(
	const FWoTNativeFileHandle& File)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (!File.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the file handle is not open")));
	}
	const FWoTNativeHandleState& State = *FWoTNativeHandleAccess::State(File);
	constexpr DWORD Flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
	DWORD Capacity = ::GetFinalPathNameByHandleW(
		static_cast<HANDLE>(State.Handle), nullptr, 0, Flags);
	if (Capacity == 0)
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(file)"),
			::GetLastError(), State.Path));
	}
	TArray<WCHAR> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(Capacity) + 1);
	const DWORD Length = ::GetFinalPathNameByHandleW(
		static_cast<HANDLE>(State.Handle), Buffer.GetData(),
		static_cast<DWORD>(Buffer.Num()), Flags);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num()))
	{
		return FResult::Error(SystemError(TEXT("GetFinalPathNameByHandleW(file)"),
			::GetLastError(), State.Path));
	}
	FString Path(static_cast<int32>(Length), Buffer.GetData());
	if (Path.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::CaseSensitive))
	{
		Path = TEXT("//") + Path.RightChop(8);
	}
	else if (Path.StartsWith(TEXT("\\\\?\\"), ESearchCase::CaseSensitive))
	{
		Path = Path.RightChop(4);
	}
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	return FResult::Success(MoveTemp(Path));
}

TWoTBootstrapResult<FWoTNativeFileHandle> OpenNativeFileAtReadNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView CanonicalRelativePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	bool bFinalVolumeBoundary = false;
	TWoTBootstrapResult<HANDLE> Handle = OpenRelativeThroughComponents(
		*FWoTNativeHandleAccess::State(Directory), CanonicalRelativePath, false,
		false, nullptr, nullptr, &bFinalVolumeBoundary);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	return AdoptHandle<FWoTNativeFileHandle>(Handle.GetValue(), CanonicalRelativePath,
		false, INVALID_HANDLE_VALUE, false, false, bFinalVolumeBoundary);
}

TWoTBootstrapResult<FWoTNativeFileHandle>
OpenNativeInstallerSourceFileAtReadNoFollow(
	const FWoTNativeDirectoryHandle& Directory,
	FStringView CanonicalRelativePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	HANDLE DataHandle = INVALID_HANDLE_VALUE;
	bool bOnlineOnlyAtOpen = false;
	bool bFinalVolumeBoundary = false;
	TWoTBootstrapResult<HANDLE> Handle = OpenRelativeThroughComponents(
		*FWoTNativeHandleAccess::State(Directory), CanonicalRelativePath,
		false, true, &DataHandle, &bOnlineOnlyAtOpen, &bFinalVolumeBoundary);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	FString DiagnosticPath = FPaths::Combine(
		FWoTNativeHandleAccess::State(Directory)->Path,
		FString(CanonicalRelativePath));
	FPaths::NormalizeFilename(DiagnosticPath);
	return AdoptHandle<FWoTNativeFileHandle>(
		Handle.GetValue(), DiagnosticPath, true, DataHandle,
		bOnlineOnlyAtOpen, false, bFinalVolumeBoundary);
}

TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> QueryNativeEntryTypeAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<TOptional<EWoTTreeEntryType>>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	HANDLE Handle = INVALID_HANDLE_VALUE;
	TWoTBootstrapResult<NTSTATUS> Opened = OpenOneRelativeForAttributes(
		*FWoTNativeHandleAccess::State(Directory), Leaf, Handle);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	const NTSTATUS Status = Opened.GetValue();
	// DELETE_PENDING is a name whose object is already on its way out (a browser
	// replacing a placeholder, a player deleting the file): absent, not broken.
	if (Status == WoTStatusObjectNameNotFound || Status == WoTStatusObjectPathNotFound
		|| Status == WoTStatusDeletePending)
	{
		return FResult::Success(TOptional<EWoTTreeEntryType>());
	}
	if (Status < 0)
	{
		return FResult::Error(SystemError(TEXT("NtCreateFile(attributes)"),
			NtStatusToWindowsError(Status), Leaf));
	}

	FILE_ATTRIBUTE_TAG_INFO TagInfo = {};
	const bool bQueried = ::GetFileInformationByHandleEx(
		Handle, FileAttributeTagInfo, &TagInfo, sizeof(TagInfo)) != 0;
	const DWORD QueryError = bQueried ? ERROR_SUCCESS : ::GetLastError();
	::CloseHandle(Handle);
	if (!bQueried)
	{
		return FResult::Error(SystemError(
			TEXT("GetFileInformationByHandleEx(FileAttributeTagInfo)"), QueryError, Leaf));
	}
	// The identity rule: opened AS the reparse point, any reparse point is a link.
	const EWoTTreeEntryType Type =
		(TagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
			? EWoTTreeEntryType::SymbolicLink
			: (TagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
				? EWoTTreeEntryType::Directory : EWoTTreeEntryType::File;
	return FResult::Success(TOptional<EWoTTreeEntryType>(Type));
}

TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>>
QueryNativeInstallerSourceEntryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult =
		TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	HANDLE Handle = INVALID_HANDLE_VALUE;
	TWoTBootstrapResult<NTSTATUS> Opened = OpenOneRelativeForAttributes(
		*FWoTNativeHandleAccess::State(Directory), Leaf, Handle);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	const NTSTATUS Status = Opened.GetValue();
	if (Status == WoTStatusObjectNameNotFound
		|| Status == WoTStatusObjectPathNotFound || Status == WoTStatusDeletePending)
	{
		return FResult::Success(TOptional<FWoTInstallerSourceEntryInfo>());
	}
	if (Status < 0)
	{
		return FResult::Error(SystemError(TEXT("NtCreateFile(installer attributes)"),
			NtStatusToWindowsError(Status), Leaf));
	}

	FILE_ATTRIBUTE_TAG_INFO TagInfo = {};
	const bool bQueried = ::GetFileInformationByHandleEx(
		Handle, FileAttributeTagInfo, &TagInfo, sizeof(TagInfo)) != 0;
	const DWORD QueryError = bQueried ? ERROR_SUCCESS : ::GetLastError();
	::CloseHandle(Handle);
	if (!bQueried)
	{
		return FResult::Error(SystemError(
			TEXT("GetFileInformationByHandleEx(installer FileAttributeTagInfo)"),
			QueryError, Leaf));
	}

	FWoTInstallerSourceEntryInfo Info;
	if ((TagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		Info.ReparseTag = TagInfo.ReparseTag;
		const EWoTInstallerSourceReparseTagClass Class =
			ClassifyInstallerSourceReparseTag(Info.ReparseTag);
		if (Class == EWoTInstallerSourceReparseTagClass::DataContent
			&& (TagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			Info.Type = EWoTTreeEntryType::File;
		}
		else
		{
			Info.Type = EWoTTreeEntryType::SymbolicLink;
			Info.bRefusedReparsePoint = true;
			LogInstallerSourceReparseRefusal(Info.ReparseTag, Class, Leaf);
		}
	}
	else
	{
		Info.Type = (TagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
			? EWoTTreeEntryType::Directory : EWoTTreeEntryType::File;
	}
	return FResult::Success(
		TOptional<FWoTInstallerSourceEntryInfo>(MoveTemp(Info)));
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenNativeDirectoryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView CanonicalRelativePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	bool bFinalVolumeBoundary = false;
	TWoTBootstrapResult<HANDLE> Handle = OpenRelativeThroughComponents(
		*FWoTNativeHandleAccess::State(Directory), CanonicalRelativePath, true,
		false, nullptr, nullptr, &bFinalVolumeBoundary);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	return AdoptHandle<FWoTNativeDirectoryHandle>(Handle.GetValue(), CanonicalRelativePath,
		false, INVALID_HANDLE_VALUE, false, false, bFinalVolumeBoundary);
}

TWoTBootstrapResult<TPair<FString, FString>>
SplitAbsolutePathForNoFollowWalk(FStringView CanonicalAbsolutePath)
{
	using FResult = TWoTBootstrapResult<TPair<FString, FString>>;
	FString Path(CanonicalAbsolutePath);
	Path.ReplaceInline(TEXT("\\"), TEXT("/"),
		ESearchCase::CaseSensitive);
	FString Root;
	FString Relative;
	if (Path.Len() >= 4 && FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':') && Path[2] == TEXT('/'))
	{
		Root = Path.Left(3);
		Relative = Path.RightChop(3);
	}
	else if (Path.StartsWith(TEXT("//"), ESearchCase::CaseSensitive)
		&& !Path.StartsWith(TEXT("//?/"), ESearchCase::CaseSensitive))
	{
		const int32 ServerEnd = Path.Find(
			TEXT("/"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, 2);
		const int32 ShareEnd = ServerEnd == INDEX_NONE ? INDEX_NONE
			: Path.Find(TEXT("/"), ESearchCase::CaseSensitive,
				ESearchDir::FromStart, ServerEnd + 1);
		if (ServerEnd > 2 && ShareEnd > ServerEnd + 1)
		{
			Root = Path.Left(ShareEnd);
			Relative = Path.RightChop(ShareEnd + 1);
		}
	}
	if (Root.IsEmpty() || Relative.IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("an absolute path below a drive/share root is required"),
			Path));
	}
	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(Relative);
		Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	return FResult::Success(
		TPair<FString, FString>(MoveTemp(Root), MoveTemp(Relative)));
}

TWoTBootstrapResult<FWoTNativeFileHandle>
OpenNativeFilePathReadNoFollow(FStringView CanonicalAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	TWoTBootstrapResult<TPair<FString, FString>> Split =
		SplitAbsolutePathForNoFollowWalk(CanonicalAbsolutePath);
	if (Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Split.GetValue().Key);
	if (Root.IsError())
	{
		return FResult::Error(MoveTemp(Root).GetError());
	}
	return OpenNativeFileAtReadNoFollow(
		Root.GetValue(), Split.GetValue().Value);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle>
OpenNativeDirectoryPathNoFollow(FStringView CanonicalAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	TWoTBootstrapResult<TPair<FString, FString>> Split =
		SplitAbsolutePathForNoFollowWalk(CanonicalAbsolutePath);
	if (Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Split.GetValue().Key);
	if (Root.IsError())
	{
		return FResult::Error(MoveTemp(Root).GetError());
	}
	return OpenNativeDirectoryAtNoFollow(
		Root.GetValue(), Split.GetValue().Value);
}

TWoTBootstrapResult<FWoTNativeFileHandle> CreateNativeFileAtWriteNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	TWoTBootstrapResult<HANDLE> Handle = OpenOneRelative(
		*FWoTNativeHandleAccess::State(Directory), Leaf,
		GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE, FILE_CREATE,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	return AdoptHandle<FWoTNativeFileHandle>(Handle.GetValue(), Leaf);
}

namespace
{
#if WITH_AUTOMATION_TESTS
TAtomic<uint32> GWoTNativeDirectoryWritableProbeErrorForTesting(0);
TFunction<FWoTBootstrapStatus()> GWoTNativeDirectoryMutationWalkHookForTesting;
#endif

TWoTBootstrapResult<bool> ProbeNativeDirectoryWritableNoFollowInternal(
	const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf,
	const TFunction<void(FStringView)>& AfterCreate)
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	// FILE_CREATE is the handle-relative NtCreateFile spelling of CREATE_NEW;
	// FILE_DELETE_ON_CLOSE is the native equivalent of
	// FILE_FLAG_DELETE_ON_CLOSE. Cleanup is attached to this object handle, so a
	// racing rename cannot redirect it to a replacement at the original name.
	auto CreateProbe = [&]() -> TWoTBootstrapResult<HANDLE>
	{
	#if WITH_AUTOMATION_TESTS
		if (const uint32 Injected =
				GWoTNativeDirectoryWritableProbeErrorForTesting.Exchange(0);
			Injected != 0)
		{
			return TWoTBootstrapResult<HANDLE>::Error(SystemError(
				TEXT("NtCreateFile(writable probe)"), Injected, UniqueLeaf));
		}
	#endif
		return OpenOneRelative(
			*FWoTNativeHandleAccess::State(Directory), UniqueLeaf,
			DELETE | SYNCHRONIZE, FILE_CREATE,
			FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
				| FILE_DELETE_ON_CLOSE);
	};
	TWoTBootstrapResult<HANDLE> Created = CreateProbe();
	if (Created.IsError())
	{
		FWoTBootstrapError Error = MoveTemp(Created).GetError();
		return IsPermissionError(Error.NativeCode)
			? FResult::Success(false)
			: FResult::Error(MoveTemp(Error));
	}
	if (AfterCreate)
	{
		AfterCreate(UniqueLeaf);
	}
	if (!::CloseHandle(Created.GetValue()))
	{
		return FResult::Error(SystemError(
			TEXT("CloseHandle(delete-on-close probe)"), ::GetLastError(),
			UniqueLeaf));
	}
	return FResult::Success(true);
}
} // namespace

TWoTBootstrapResult<bool> ProbeNativeDirectoryWritableNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf)
{
	return ProbeNativeDirectoryWritableNoFollowInternal(
		Directory, UniqueLeaf, {});
}

TWoTBootstrapResult<bool> CheckNativeDirectoryMutationAccessAtNoFollow(
	const FWoTNativeDirectoryHandle& Parent, FStringView Leaf,
	TFunctionRef<FWoTBootstrapStatus()> BetweenEntries)
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!Parent.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the parent directory handle is not open")));
	}

	auto HeldGrants = [](const FWoTNativeDirectoryHandle& Directory,
		DWORD Access, FStringView Path) -> FResult
	{
		// AccessCheck evaluates the current effective token against the security
		// descriptor of this exact held object. ReOpenFile cannot be used here:
		// Windows will not let it grant rights beyond the original read handle.
		const HANDLE Handle = static_cast<HANDLE>(
			FWoTNativeHandleAccess::State(Directory)->Handle);
		const SECURITY_INFORMATION SecurityInformation =
			OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
			| DACL_SECURITY_INFORMATION;
		DWORD SecurityBytes = 0;
		(void)::GetKernelObjectSecurity(
			Handle, SecurityInformation, nullptr, 0, &SecurityBytes);
		if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || SecurityBytes == 0)
		{
			return FResult::Error(SystemError(
				TEXT("GetKernelObjectSecurity(size)"), ::GetLastError(), Path));
		}
		TArray<uint8> Security;
		Security.SetNumUninitialized(static_cast<int32>(SecurityBytes));
		if (!::GetKernelObjectSecurity(Handle, SecurityInformation,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(Security.GetData()),
			SecurityBytes, &SecurityBytes))
		{
			return FResult::Error(SystemError(
				TEXT("GetKernelObjectSecurity"), ::GetLastError(), Path));
		}

		HANDLE EffectiveToken = nullptr;
		if (!::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE,
			1, &EffectiveToken))
		{
			const uint32 ThreadTokenError = ::GetLastError();
			if (ThreadTokenError != ERROR_NO_TOKEN
				|| !::OpenProcessToken(::GetCurrentProcess(),
					TOKEN_QUERY | TOKEN_DUPLICATE, &EffectiveToken))
			{
				return FResult::Error(SystemError(TEXT("Open effective token"),
					ThreadTokenError == ERROR_NO_TOKEN ? ::GetLastError()
						: ThreadTokenError, Path));
			}
		}
		ON_SCOPE_EXIT { ::CloseHandle(EffectiveToken); };
		HANDLE ImpersonationToken = nullptr;
		if (!::DuplicateToken(
			EffectiveToken, SecurityImpersonation, &ImpersonationToken))
		{
			return FResult::Error(SystemError(
				TEXT("DuplicateToken"), ::GetLastError(), Path));
		}
		ON_SCOPE_EXIT { ::CloseHandle(ImpersonationToken); };

		GENERIC_MAPPING Mapping = {
			FILE_GENERIC_READ, FILE_GENERIC_WRITE,
			FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
		DWORD DesiredAccess = Access;
		::MapGenericMask(&DesiredAccess, &Mapping);
		PRIVILEGE_SET Privileges = {};
		DWORD PrivilegeBytes = sizeof(Privileges);
		DWORD GrantedAccess = 0;
		BOOL bGranted = 0;
		if (!::AccessCheck(
			reinterpret_cast<PSECURITY_DESCRIPTOR>(Security.GetData()),
			ImpersonationToken, DesiredAccess, &Mapping, &Privileges,
			&PrivilegeBytes, &GrantedAccess, &bGranted))
		{
			return FResult::Error(SystemError(
				TEXT("AccessCheck"), ::GetLastError(), Path));
		}
		return FResult::Success(bGranted != 0);
	};

	auto PermissionOrError = [](TWoTBootstrapResult<HANDLE>&& Opened) -> FResult
	{
		if (!Opened.IsError())
		{
			if (!::CloseHandle(Opened.GetValue()))
			{
				return FResult::Error(SystemError(
					TEXT("CloseHandle(delete access check)"),
					::GetLastError(), TEXT("")));
			}
			return FResult::Success(true);
		}
		FWoTBootstrapError Error = MoveTemp(Opened).GetError();
		// NTFS evaluates the DACL before share access, so a sharing violation
		// means DELETE was granted and only a held handle is in the way: the
		// running game's own paks and working directory (opened without
		// FILE_SHARE_DELETE) while it probes. They are closed by the time the
		// launcher activates the staged tree. Treating it as missing permission
		// put every install where the user lacks FILE_DELETE_CHILD (an inherited
		// Modify-only drive ACL) into notify-only updates.
		if (Error.NativeCode == ERROR_SHARING_VIOLATION)
		{
			return FResult::Success(true);
		}
		return IsPermissionError(Error.NativeCode) ? FResult::Success(false)
			: FResult::Error(MoveTemp(Error));
	};

	auto OpenDirectoryForWalk = [](const FWoTNativeDirectoryHandle& Directory,
		FStringView ChildLeaf, bool bNeedDelete)
		-> TWoTBootstrapResult<FWoTNativeDirectoryHandle>
	{
		using FDirectoryResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
		constexpr ACCESS_MASK WalkAccess =
			FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
		TWoTBootstrapResult<HANDLE> Opened = OpenOneRelative(
			*FWoTNativeHandleAccess::State(Directory), ChildLeaf,
			WalkAccess | (bNeedDelete ? DELETE : 0),
			FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
		if (Opened.IsError() && bNeedDelete
			&& Opened.GetError().NativeCode == ERROR_SHARING_VIOLATION)
		{
			// DELETE is proven (see PermissionOrError): a held handle such as the
			// running game's working directory refused only the share. Reopen
			// without it so the walk can still prove the descendants.
			Opened = OpenOneRelative(
				*FWoTNativeHandleAccess::State(Directory), ChildLeaf, WalkAccess,
				FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
		}
		if (Opened.IsError())
		{
			return FDirectoryResult::Error(MoveTemp(Opened).GetError());
		}
		FDirectoryResult Adopted =
			AdoptHandle<FWoTNativeDirectoryHandle>(Opened.GetValue(), ChildLeaf);
		if (!Adopted.IsError()
			&& Adopted.GetValue().GetIdentity().Type
				!= EWoTTreeEntryType::Directory)
		{
			return FDirectoryResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("a recursive delete access check found a reparse point where a directory was enumerated"),
				FString(ChildLeaf)));
		}
		return MoveTemp(Adopted);
	};

	// Activation adds a renamed directory to the install root. It can remove
	// each source name either through FILE_DELETE_CHILD on that held root or
	// through DELETE on the source object itself.
	TWoTBootstrapResult<bool> CanAddRenamedTree = HeldGrants(
		Parent, FILE_ADD_SUBDIRECTORY, Leaf);
	if (CanAddRenamedTree.IsError() || !CanAddRenamedTree.GetValue())
	{
		return MoveTemp(CanAddRenamedTree);
	}
	TWoTBootstrapResult<bool> InstallCanDeleteChild = HeldGrants(
		Parent, FILE_DELETE_CHILD, Leaf);
	if (InstallCanDeleteChild.IsError())
	{
		return MoveTemp(InstallCanDeleteChild);
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Live = OpenDirectoryForWalk(
		Parent, Leaf, !InstallCanDeleteChild.GetValue());
	if (Live.IsError())
	{
		FWoTBootstrapError Error = MoveTemp(Live).GetError();
		return IsPermissionError(Error.NativeCode) ? FResult::Success(false)
			: FResult::Error(MoveTemp(Error));
	}

	struct FWalkFrame
	{
		FWoTNativeDirectoryHandle Directory;
		TArray<FWoTNativeDirectoryEntry> Entries;
		int32 NextEntry = 0;
		bool bParentCanDeleteChild = false;

		FWalkFrame(FWoTNativeDirectoryHandle&& InDirectory,
			TArray<FWoTNativeDirectoryEntry>&& InEntries,
			bool bInParentCanDeleteChild)
			: Directory(MoveTemp(InDirectory))
			, Entries(MoveTemp(InEntries))
			, bParentCanDeleteChild(bInParentCanDeleteChild)
		{
		}
	};

	TArray<FWalkFrame> Stack;
	auto PushDirectory = [&](FWoTNativeDirectoryHandle&& Directory) -> FResult
	{
		TWoTBootstrapResult<bool> ParentCanDeleteChild = HeldGrants(
			Directory, FILE_DELETE_CHILD, Leaf);
		if (ParentCanDeleteChild.IsError())
		{
			return MoveTemp(ParentCanDeleteChild);
		}
		TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
			EnumerateNativeDirectory(Directory);
		if (Entries.IsError())
		{
			FWoTBootstrapError Error = MoveTemp(Entries).GetError();
			return IsPermissionError(Error.NativeCode) ? FResult::Success(false)
				: FResult::Error(MoveTemp(Error));
		}
		Stack.Emplace(MoveTemp(Directory), MoveTemp(Entries).GetValue(),
			ParentCanDeleteChild.GetValue());
		return FResult::Success(true);
	};

	FResult EnteredLive = PushDirectory(MoveTemp(Live).GetValue());
	if (EnteredLive.IsError() || !EnteredLive.GetValue())
	{
		return MoveTemp(EnteredLive);
	}

	bool bVisitedEntry = false;
	while (!Stack.IsEmpty())
	{
		FWalkFrame& Current = Stack.Last();
		if (Current.NextEntry >= Current.Entries.Num())
		{
			// Destroying the completed frame closes this directory before its
			// parent resumes, so the explicit stack retains only the active DFS path.
			Stack.Pop(EAllowShrinking::No);
			continue;
		}

		if (bVisitedEntry)
		{
		#if WITH_AUTOMATION_TESTS
			if (GWoTNativeDirectoryMutationWalkHookForTesting)
			{
				if (FWoTBootstrapStatus Hooked =
						GWoTNativeDirectoryMutationWalkHookForTesting();
					Hooked.IsError())
				{
					return FResult::Error(MoveTemp(Hooked).GetError());
				}
			}
		#endif
			if (FWoTBootstrapStatus Continued = BetweenEntries();
				Continued.IsError())
			{
				return FResult::Error(MoveTemp(Continued).GetError());
			}
		}
		bVisitedEntry = true;
		FWoTNativeDirectoryEntry Entry =
			MoveTemp(Current.Entries[Current.NextEntry++]);

		// A real directory must be entered to prove its descendants. A reparse
		// directory was enumerated as SymbolicLink and is checked only as an entry.
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
				OpenDirectoryForWalk(Current.Directory, Entry.Name,
					!Current.bParentCanDeleteChild);
			if (Child.IsError())
			{
				FWoTBootstrapError Error = MoveTemp(Child).GetError();
				return IsPermissionError(Error.NativeCode)
					? FResult::Success(false)
					: FResult::Error(MoveTemp(Error));
			}
			FResult EnteredChild = PushDirectory(MoveTemp(Child).GetValue());
			if (EnteredChild.IsError() || !EnteredChild.GetValue())
			{
				return MoveTemp(EnteredChild);
			}
		}
		else if (!Current.bParentCanDeleteChild)
		{
			FResult Deletable = PermissionOrError(OpenOneRelative(
				*FWoTNativeHandleAccess::State(Current.Directory), Entry.Name,
				DELETE | SYNCHRONIZE, FILE_OPEN,
				FILE_SYNCHRONOUS_IO_NONALERT));
			if (Deletable.IsError() || !Deletable.GetValue())
			{
				return MoveTemp(Deletable);
			}
		}
	}
	return FResult::Success(true);
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<bool> ProbeNativeDirectoryWritableNoFollowForTesting(
	const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf,
	TFunction<void(FStringView)> AfterCreate)
{
	return ProbeNativeDirectoryWritableNoFollowInternal(
		Directory, UniqueLeaf, AfterCreate);
}

void WoTSetNativeDirectoryWritableProbeErrorForTesting(uint32 NativeCode)
{
	GWoTNativeDirectoryWritableProbeErrorForTesting.Store(NativeCode);
}

void WoTSetNativeDirectoryMutationWalkHookForTesting(
	TFunction<FWoTBootstrapStatus()> Hook)
{
	GWoTNativeDirectoryMutationWalkHookForTesting = MoveTemp(Hook);
}

void WoTSetNativeDirectoryRenameRetryObserverForTesting(
	TFunction<void(int32 FailedAttempts)> Observer)
{
	GWoTNativeDirectoryRenameRetryObserverForTesting = MoveTemp(Observer);
}

void WoTSetNativeVolumeGuidPathFailsForTesting(bool bFails)
{
	GWoTNativeVolumeGuidPathFailsForTesting.Store(bFails);
}

void WoTSetNativeReplaceRenameRetryObserverForTesting(
	TFunction<void(int32 FailedAttempts)> Observer)
{
	GWoTNativeReplaceRenameRetryObserverForTesting = MoveTemp(Observer);
}

void WoTSetNativeDirectoryRenameRetryBudgetForTesting(double Seconds)
{
	GWoTNativeDirectoryRenameRetryBudgetMsForTesting.Store(Seconds < 0.0
		? -1 : static_cast<int64>(Seconds * 1000.0));
}
#endif

TWoTBootstrapResult<FWoTNativeFileHandle> OpenOrCreateNativeFileAtReadWriteNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf, bool& bOutCreated)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	bOutCreated = false;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	TWoTBootstrapResult<HANDLE> Handle = OpenOneRelative(
		*FWoTNativeHandleAccess::State(Directory), Leaf,
		GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE, FILE_OPEN_IF,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, &bOutCreated);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Adopted =
		AdoptHandle<FWoTNativeFileHandle>(Handle.GetValue(), Leaf);
	if (!Adopted.IsError())
	{
		const FWoTNativeFileIdentity& Identity = Adopted.GetValue().GetIdentity();
		if (Identity.Type != EWoTTreeEntryType::File || Identity.LinkCount != 1)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the opened lock object is not a single-link regular file"),
				FString(Leaf)));
		}
	}
	return Adopted;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> CreateNativeDirectoryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	TWoTBootstrapResult<HANDLE> Handle = OpenOneRelative(
		*FWoTNativeHandleAccess::State(Directory), Leaf,
		GENERIC_READ | DELETE | SYNCHRONIZE, FILE_CREATE,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Adopted =
		AdoptHandle<FWoTNativeDirectoryHandle>(Handle.GetValue(), Leaf);
	if (!Adopted.IsError()
		&& Adopted.GetValue().GetIdentity().Type != EWoTTreeEntryType::Directory)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the created object is not a no-follow directory"), FString(Leaf)));
	}
	return Adopted;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenOrCreateNativeDirectoryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf, bool& bOutCreated)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	bOutCreated = false;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	TWoTBootstrapResult<HANDLE> Handle = OpenOneRelative(
		*FWoTNativeHandleAccess::State(Directory), Leaf,
		GENERIC_READ | DELETE | SYNCHRONIZE, FILE_OPEN_IF,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, &bOutCreated);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Adopted =
		AdoptHandle<FWoTNativeDirectoryHandle>(Handle.GetValue(), Leaf);
	if (!Adopted.IsError()
		&& Adopted.GetValue().GetIdentity().Type != EWoTTreeEntryType::Directory)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the opened object is not a no-follow directory"), FString(Leaf)));
	}
	return Adopted;
}

FWoTBootstrapStatus WriteNativeFile(FWoTNativeFileHandle& File,
	TConstArrayView<uint8> Bytes)
{
	if (FWoTBootstrapStatus Truncated = TruncateNativeFile(File, 0);
		Truncated.IsError())
	{
		return Truncated;
	}
	return WriteNativeFileAt(File, 0, Bytes);
}

FWoTBootstrapStatus TruncateNativeFile(FWoTNativeFileHandle& File, uint64 Size)
{
	if (!File.IsValid() || File.GetIdentity().Type != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("only a held regular file can be truncated")));
	}
	if (Size > static_cast<uint64>(TNumericLimits<int64>::Max()))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("the requested native file size exceeds the platform offset range")));
	}
	const HANDLE Handle = static_cast<HANDLE>(
		FWoTNativeHandleAccess::State(File)->Handle);
	FILE_END_OF_FILE_INFO End = {};
	End.EndOfFile.QuadPart = static_cast<LONGLONG>(Size);
	if (!::SetFileInformationByHandle(Handle, FileEndOfFileInfo, &End, sizeof(End)))
	{
		return SystemStatus(TEXT("SetFileInformationByHandle(FileEndOfFileInfo)"),
			::GetLastError(), TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus WriteNativeFileAt(FWoTNativeFileHandle& File, uint64 Offset,
	TConstArrayView<uint8> Bytes)
{
	if (!File.IsValid() || File.GetIdentity().Type != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("only a held regular file can be written")));
	}
	if (Offset > static_cast<uint64>(TNumericLimits<int64>::Max())
		|| static_cast<uint64>(Bytes.Num())
			> static_cast<uint64>(TNumericLimits<int64>::Max()) - Offset)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("the native file write exceeds the platform offset range")));
	}
	const HANDLE Handle = static_cast<HANDLE>(
		FWoTNativeHandleAccess::State(File)->Handle);
	uint64 WrittenTotal = 0;
	while (WrittenTotal < static_cast<uint64>(Bytes.Num()))
	{
		const DWORD Amount = static_cast<DWORD>(FMath::Min<uint64>(
			static_cast<uint64>(Bytes.Num()) - WrittenTotal,
			64ull * 1024ull * 1024ull));
		const uint64 WriteOffset = Offset + WrittenTotal;
		OVERLAPPED Overlapped = {};
		Overlapped.Offset = static_cast<DWORD>(WriteOffset & 0xFFFFFFFFull);
		Overlapped.OffsetHigh = static_cast<DWORD>(WriteOffset >> 32);
		DWORD Written = 0;
		if (!::WriteFile(Handle, Bytes.GetData() + WrittenTotal, Amount,
				&Written, &Overlapped))
		{
			return SystemStatus(TEXT("WriteFile"), ::GetLastError(), TEXT(""));
		}
		if (Written == 0)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io,
				TEXT("WriteFile made no progress on a held offset write")));
		}
		WrittenTotal += Written;
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<uint64> ReadNativeFileAt(const FWoTNativeFileHandle& File,
	uint64 Offset, TArrayView<uint8> Buffer)
{
	using FResult = TWoTBootstrapResult<uint64>;
	if (!File.IsValid() || File.GetIdentity().Type != EWoTTreeEntryType::File)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("only a held regular file can be read")));
	}
	if (Offset > static_cast<uint64>(TNumericLimits<int64>::Max()))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("the native file read exceeds the platform offset range")));
	}
	if (TOptional<FWoTBootstrapError> Local =
			RequireInstallerSourceLocalBeforeRead(File); Local.IsSet())
	{
		return FResult::Error(MoveTemp(Local.GetValue()));
	}
	const FWoTNativeHandleState& State = *FWoTNativeHandleAccess::State(File);
	const HANDLE Handle = DataHandleForRead(State);
	OVERLAPPED Overlapped = {};
	Overlapped.Offset = static_cast<DWORD>(Offset & 0xFFFFFFFFull);
	Overlapped.OffsetHigh = static_cast<DWORD>(Offset >> 32);
	DWORD Read = 0;
	const DWORD Amount = static_cast<DWORD>(FMath::Min<int64>(
		Buffer.Num(), static_cast<int64>(MAX_uint32)));
#if WITH_AUTOMATION_TESTS
	if (State.bInstallerSource && GWoTInstallerSourceReadObserverForTesting)
	{
		GWoTInstallerSourceReadObserverForTesting();
	}
#endif
	if (!::ReadFile(Handle, Buffer.GetData(), Amount, &Read, &Overlapped))
	{
		return FResult::Error(SystemError(TEXT("ReadFile"),
			::GetLastError(), State.Path));
	}
	return FResult::Success(static_cast<uint64>(Read));
}

FWoTBootstrapStatus FlushNativeFile(const FWoTNativeFileHandle& File)
{
	if (!File.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the file handle is not open")));
	}
	if (!::FlushFileBuffers(static_cast<HANDLE>(FWoTNativeHandleAccess::State(File)->Handle)))
	{
		return SystemStatus(TEXT("FlushFileBuffers"), ::GetLastError(), TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FlushNativeDirectory(const FWoTNativeDirectoryHandle& Directory)
{
	if (!Directory.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the directory handle is not open")));
	}
	// NTFS/ReFS do not expose a supported FlushFileBuffers contract for directory
	// handles. Relative renames are performed on the held directory and file data
	// is flushed first; this marker is the platform's checked barrier point.
	return FWoTBootstrapStatus::Success();
}

template <typename HandleType>
TWoTBootstrapResult<uint32> QueryModeFromHeldWindowsHandle(const HandleType& Handle)
{
	using FResult = TWoTBootstrapResult<uint32>;
	if (!Handle.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the native handle is not open")));
	}
	FILE_BASIC_INFO Information = {};
	if (!::GetFileInformationByHandleEx(
			static_cast<HANDLE>(FWoTNativeHandleAccess::State(Handle)->Handle),
			FileBasicInfo, &Information, sizeof(Information)))
	{
		return FResult::Error(SystemError(
			TEXT("GetFileInformationByHandleEx(FileBasicInfo)"),
			::GetLastError(), TEXT("")));
	}
	const bool bReadOnly =
		(Information.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
	const bool bDirectory =
		(Information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	return FResult::Success(bDirectory
		? (bReadOnly ? 0555u : 0755u)
		: (bReadOnly ? 0444u : 0644u));
}

template <typename HandleType>
FWoTBootstrapStatus SetModeOnHeldWindowsHandle(const HandleType& Handle, uint32 Mode)
{
	if (!Handle.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the native handle is not open")));
	}
	const bool bDirectory =
		Handle.GetIdentity().Type == EWoTTreeEntryType::Directory;
	const uint32 WritableMode = bDirectory ? 0755u : 0644u;
	const uint32 ReadOnlyMode = bDirectory ? 0555u : 0444u;
	if (Mode != WritableMode && Mode != ReadOnlyMode)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the signed POSIX mode has no exact Win64 filesystem projection")));
	}
	const HANDLE Held =
		static_cast<HANDLE>(FWoTNativeHandleAccess::State(Handle)->Handle);
	// NtCreateFile with an EMPTY relative name, not ReOpenFile: ReOpenFile is
	// refused with ERROR_ACCESS_DENIED for DIRECTORY handles in every flag
	// combination (measured; files succeed in all of them), and the
	// staged tree's signed 0755/0555 directories go through here. An empty
	// ObjectName relative to the held handle reopens the SAME object with new
	// access at NT level -- no pathname is ever resolved, so the no-follow
	// guarantee is untouched, and the identity revalidation below still
	// compares the volume/file-index tuple exactly as before.
	const FWoTNtCreateFile NtCreateFileEntry = ResolveNtCreateFile();
	if (NtCreateFileEntry == nullptr)
	{
		return SystemStatus(TEXT("GetProcAddress(NtCreateFile)"),
			ERROR_PROC_NOT_FOUND, TEXT(""));
	}
	UNICODE_STRING EmptyName = {};
	OBJECT_ATTRIBUTES Attributes = {};
	Attributes.Length = sizeof(Attributes);
	Attributes.RootDirectory = Held;
	Attributes.ObjectName = &EmptyName;
	Attributes.Attributes = OBJ_CASE_INSENSITIVE;
	IO_STATUS_BLOCK ReopenStatus = {};
	HANDLE Native = INVALID_HANDLE_VALUE;
	const NTSTATUS Reopened = NtCreateFileEntry(&Native,
		FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
		&Attributes, &ReopenStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT
			| FILE_OPEN_FOR_BACKUP_INTENT,
		nullptr, 0);
	if (Reopened < 0 || Native == INVALID_HANDLE_VALUE)
	{
		return SystemStatus(TEXT("NtCreateFile(empty-name metadata reopen)"),
			NtStatusToWindowsError(Reopened), TEXT(""));
	}

	// ReOpenFile changes access on the already-held object; it never resolves the
	// diagnostic path retained in FWoTNativeHandleState. Still compare the exact
	// Win32 volume/file-index tuple before granting the metadata handle authority
	// to mutate, matching the held-object revalidation done by fchmod on macOS.
	BY_HANDLE_FILE_INFORMATION HeldInformation = {};
	BY_HANDLE_FILE_INFORMATION ReopenedInformation = {};
	const bool bHeldInformation =
		::GetFileInformationByHandle(Held, &HeldInformation) != 0;
	const uint32 HeldInformationError =
		bHeldInformation ? ERROR_SUCCESS : ::GetLastError();
	const bool bReopenedInformation =
		::GetFileInformationByHandle(Native, &ReopenedInformation) != 0;
	const uint32 ReopenedInformationError =
		bReopenedInformation ? ERROR_SUCCESS : ::GetLastError();
	if (!bHeldInformation || !bReopenedInformation)
	{
		(void)::CloseHandle(Native);
		return SystemStatus(TEXT("GetFileInformationByHandle(mode revalidation)"),
			!bHeldInformation ? HeldInformationError : ReopenedInformationError,
			TEXT(""));
	}
	if (HeldInformation.dwVolumeSerialNumber
			!= ReopenedInformation.dwVolumeSerialNumber
		|| HeldInformation.nFileIndexHigh != ReopenedInformation.nFileIndexHigh
		|| HeldInformation.nFileIndexLow != ReopenedInformation.nFileIndexLow)
	{
		(void)::CloseHandle(Native);
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the metadata-capable reopen is not the held filesystem object")));
	}

	FILE_BASIC_INFO Information = {};
	if (!::GetFileInformationByHandleEx(
			Native, FileBasicInfo, &Information, sizeof(Information)))
	{
		const uint32 Error = ::GetLastError();
		(void)::CloseHandle(Native);
		return SystemStatus(TEXT("GetFileInformationByHandleEx(FileBasicInfo)"),
			Error, TEXT(""));
	}
	if (Mode == ReadOnlyMode)
	{
		Information.FileAttributes |= FILE_ATTRIBUTE_READONLY;
	}
	else
	{
		Information.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
	}
	// FILE_ATTRIBUTE_NORMAL means "no other attributes" and must stand alone.
	// Conversely, zero means "do not change attributes" to FileBasicInfo.
	if ((Information.FileAttributes & ~FILE_ATTRIBUTE_NORMAL) != 0)
	{
		Information.FileAttributes &= ~FILE_ATTRIBUTE_NORMAL;
	}
	else
	{
		Information.FileAttributes = FILE_ATTRIBUTE_NORMAL;
	}
	const bool bSet = ::SetFileInformationByHandle(
		Native, FileBasicInfo, &Information, sizeof(Information)) != 0;
	const uint32 SetError = bSet ? ERROR_SUCCESS : ::GetLastError();
	const bool bClosed = ::CloseHandle(Native) != 0;
	const uint32 CloseError = bClosed ? ERROR_SUCCESS : ::GetLastError();
	if (!bSet)
	{
		return SystemStatus(TEXT("SetFileInformationByHandle(FileBasicInfo)"),
			SetError, TEXT(""));
	}
	if (!bClosed)
	{
		return SystemStatus(TEXT("CloseHandle(metadata reopen)"),
			CloseError, TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<uint32> QueryNativeMode(const FWoTNativeFileHandle& File)
{
	return QueryModeFromHeldWindowsHandle(File);
}

TWoTBootstrapResult<uint32> QueryNativeMode(const FWoTNativeDirectoryHandle& Directory)
{
	return QueryModeFromHeldWindowsHandle(Directory);
}

FWoTBootstrapStatus SetNativeMode(const FWoTNativeFileHandle& File, uint32 Mode)
{
	return SetModeOnHeldWindowsHandle(File, Mode);
}

FWoTBootstrapStatus SetNativeMode(const FWoTNativeDirectoryHandle& Directory, uint32 Mode)
{
	return SetModeOnHeldWindowsHandle(Directory, Mode);
}

FWoTBootstrapStatus RenameNativeEntryAtNoFollow(
	const FWoTNativeDirectoryHandle& FromDirectory, FStringView FromLeaf,
	const FWoTNativeDirectoryHandle& ToDirectory, FStringView ToLeaf,
	bool bReplaceExisting)
{
	if (!FromDirectory.IsValid() || !ToDirectory.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("a rename directory handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(FromLeaf); Safe.IsError())
	{
		return Safe;
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(ToLeaf); Safe.IsError())
	{
		return Safe;
	}
	// FILE_READ_ATTRIBUTES only to tell a directory from a file below; an
	// attribute-only access takes no part in share-mode checks.
	TWoTBootstrapResult<HANDLE> Source = OpenOneRelative(
		*FWoTNativeHandleAccess::State(FromDirectory), FromLeaf,
		DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT);
	if (Source.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Source).GetError());
	}
	// A failed query leaves the rename on the file path: no access-denied retry.
	bool bSourceIsDirectory = false;
	{
		FILE_BASIC_INFO Basic = {};
		if (::GetFileInformationByHandleEx(Source.GetValue(), FileBasicInfo,
				&Basic, sizeof(Basic)))
		{
			bSourceIsDirectory = (Basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}
	}
	// NtSetInformationFile, not SetFileInformationByHandle: the Win32 layer
	// rejects any FILE_RENAME_INFO whose RootDirectory is non-NULL with
	// ERROR_INVALID_PARAMETER (measured: leaf+RootDirectory fails
	// err=87 through Win32 in every shape, and succeeds at NT level), and a
	// full-path rename would reintroduce exactly the destination-path
	// re-resolution this module exists to prevent. The replace case uses
	// FileRenameInformationEx with POSIX semantics so a destination that a
	// reader transiently holds open is still replaced -- the semantics
	// rename(2) gives on macOS, and the known MoveFileEx failure mode.
	const FWoTNtSetInformationFile NtSetInformationFile = ResolveNtSetInformationFile();
	if (NtSetInformationFile == nullptr)
	{
		(void)::CloseHandle(Source.GetValue());
		return SystemStatus(TEXT("GetProcAddress(NtSetInformationFile)"),
			ERROR_PROC_NOT_FOUND, FromLeaf);
	}
	const FString ToString(ToLeaf);
	const DWORD NameBytes = static_cast<DWORD>(ToString.Len() * sizeof(TCHAR));
	TArray<uint8> Storage;
	Storage.SetNumZeroed(
		static_cast<int32>(sizeof(FWoTNtFileRenameInformation) + NameBytes));
	FWoTNtFileRenameInformation* Rename =
		reinterpret_cast<FWoTNtFileRenameInformation*>(Storage.GetData());
	if (bReplaceExisting)
	{
		Rename->Flags = WoTNtRenameReplaceIfExists | WoTNtRenamePosixSemantics;
	}
	else
	{
		Rename->ReplaceIfExists = 0;
	}
	Rename->RootDirectory = static_cast<HANDLE>(FWoTNativeHandleAccess::State(ToDirectory)->Handle);
	Rename->FileNameLength = NameBytes;
	FMemory::Memcpy(Rename->FileName, *ToString, NameBytes);
	// Two transient refusals by other processes are retried; every other
	// failure, including an existing destination, returns at once. POSIX needs
	// neither because rename(2) does not consult other holders.
	//
	// A replace-rename can transiently collide with an external scanner
	// (Defender/indexer) holding the freshly written target WITHOUT
	// FILE_SHARE_DELETE - every handle this module opens shares DELETE, so a
	// sharing violation here is outside interference, observed
	// deterministically on the resume checkpoint's first replace (Windows
	// error 32; Win64 funnel kill-resume-build, 2026-08-05). Retry briefly,
	// then fail closed.
	//
	// A directory rename is refused with STATUS_ACCESS_DENIED (Windows error 5)
	// while any handle is open on anything below the directory, whatever its
	// sharing mode. Callers close their own handles below a directory before
	// publishing it, so the holder is another process: an antivirus scan, the
	// search indexer, a sync client, an Explorer window or a tool walking the
	// tree, each of which usually lets go moments after the files were written.
	// Wait with doubling backoff, synchronously on the calling thread, until the
	// budget is spent, then fail as the first attempt would have. A file has
	// nothing below it, so a file rename never takes this retry.
	const double StartSeconds = FPlatformTime::Seconds();
	const double RetryBudgetSeconds = WoTDirectoryRenameRetryBudget();
	double NextWaitSeconds = WoTDirectoryRenameFirstWaitSeconds;
	int32 SharingRetries = 0;
	int32 AccessDeniedRetries = 0;
	NTSTATUS Status = 0;
	uint32 RenameError = ERROR_SUCCESS;
	bool bRenamed = false;
	for (;;)
	{
		IO_STATUS_BLOCK IoStatus = {};
		Status = NtSetInformationFile(Source.GetValue(), &IoStatus,
			Storage.GetData(), static_cast<ULONG>(Storage.Num()),
			bReplaceExisting ? WoTNtFileRenameInformationExClass
				: WoTNtFileRenameInformationClass);
		bRenamed = Status >= 0;
		RenameError = bRenamed ? ERROR_SUCCESS : NtStatusToWindowsError(Status);
		if (bRenamed)
		{
			break;
		}
		// Under Wine (Proton) the replace has no POSIX semantics: a target that
		// one of our own readers transiently holds is refused with access
		// denied instead of being replaced. That holder lets go within
		// milliseconds, exactly like the scanner behind a sharing violation, so
		// a file replace takes the same bounded retry. Native Windows keeps
		// treating access denied on a file as final.
		const bool bTransientHolder = RenameError == ERROR_SHARING_VIOLATION
			|| (RenameError == ERROR_ACCESS_DENIED && !bSourceIsDirectory
				&& WoTIsRunningUnderWine());
		if (bReplaceExisting && bTransientHolder
			&& SharingRetries < WoTReplaceRenameSharingRetries)
		{
			++SharingRetries;
#if WITH_AUTOMATION_TESTS
			if (GWoTNativeReplaceRenameRetryObserverForTesting)
			{
				GWoTNativeReplaceRenameRetryObserverForTesting(SharingRetries);
			}
#endif
			FPlatformProcess::Sleep(WoTReplaceRenameSharingWaitSeconds);
			continue;
		}
		if (bSourceIsDirectory && Status == WoTStatusAccessDenied)
		{
			const double RemainingSeconds =
				RetryBudgetSeconds - (FPlatformTime::Seconds() - StartSeconds);
			if (RemainingSeconds > 0.0)
			{
				++AccessDeniedRetries;
#if WITH_AUTOMATION_TESTS
				if (GWoTNativeDirectoryRenameRetryObserverForTesting)
				{
					GWoTNativeDirectoryRenameRetryObserverForTesting(AccessDeniedRetries);
				}
#endif
				FPlatformProcess::Sleep(static_cast<float>(
					FMath::Min(NextWaitSeconds, RemainingSeconds)));
				NextWaitSeconds = FMath::Min(
					NextWaitSeconds * 2.0, WoTDirectoryRenameMaxWaitSeconds);
				continue;
			}
		}
		break;
	}
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	const bool bClosed = ::CloseHandle(Source.GetValue()) != 0;
	if (!bRenamed)
	{
		// The raw status tells apart the NTSTATUS values that all map to one
		// Windows error (access denied, delete pending, cannot delete, ...).
		FWoTBootstrapError Error = SystemError(
			TEXT("NtSetInformationFile(FileRenameInformation)"), RenameError, FromLeaf);
		Error.Message += FString::Printf(TEXT(" (NTSTATUS 0x%08X; %d retries over %d ms)"),
			static_cast<uint32>(Status), SharingRetries + AccessDeniedRetries,
			static_cast<int32>(ElapsedSeconds * 1000.0 + 0.5));
		return FWoTBootstrapStatus::Error(MoveTemp(Error));
	}
	if (!bClosed)
	{
		return SystemStatus(TEXT("CloseHandle"), ::GetLastError(), FromLeaf);
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<bool> ExchangeNativeEntriesAtNoFollow(
	const FWoTNativeDirectoryHandle& DirectoryA, FStringView LeafA,
	const FWoTNativeDirectoryHandle& DirectoryB, FStringView LeafB)
{
	using FResult = TWoTBootstrapResult<bool>;
	(void)DirectoryA;
	(void)LeafA;
	(void)DirectoryB;
	(void)LeafB;
	return FResult::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::UnsupportedProtocol,
		TEXT("atomic native entry exchange is unsupported on Win64")));
}

FWoTBootstrapStatus CreateNativeSymbolicLinkAtNoFollow(
	const FWoTNativeDirectoryHandle&, FStringView, FStringView)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::UnsupportedProtocol,
		TEXT("archive symbolic-link creation is unsupported on Win64")));
}

TWoTBootstrapResult<FString> ReadNativeSymbolicLinkAtNoFollow(
	const FWoTNativeDirectoryHandle&, FStringView)
{
	return TWoTBootstrapResult<FString>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::UnsupportedProtocol,
		TEXT("archive symbolic-link reading is unsupported on Win64")));
}

TWoTBootstrapResult<uint32> QueryNativeSymbolicLinkModeAtNoFollow(
	const FWoTNativeDirectoryHandle&, FStringView)
{
	return TWoTBootstrapResult<uint32>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::UnsupportedProtocol,
		TEXT("archive symbolic-link modes are unsupported on Win64")));
}

TWoTBootstrapResult<FWoTNativeFileHandle> PublishHeldNativeFileAtNoReplace(
	const FWoTNativeFileHandle& HeldSource,
	const FWoTNativeFileIdentity& VerifiedIdentity,
	const FWoTNativeDirectoryHandle& FromDirectory, FStringView FromLeaf,
	const FWoTNativeDirectoryHandle& ToDirectory, FStringView ToLeaf)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileHandle>;
	if (!HeldSource.IsValid() || !FromDirectory.IsValid() || !ToDirectory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("a held-file publication handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(FromLeaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(ToLeaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}

	auto MutationError = [&]()
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the held publication source changed after verification"),
			FString(FromLeaf));
	};
	TWoTBootstrapResult<FWoTNativeFileIdentity> HeldBefore =
		HeldSource.QueryCurrentIdentity();
	if (HeldBefore.IsError())
	{
		return FResult::Error(MoveTemp(HeldBefore).GetError());
	}
	if (HeldBefore.GetValue() != VerifiedIdentity
		|| HeldBefore.GetValue().Type != EWoTTreeEntryType::File
		|| HeldBefore.GetValue().LinkCount != 1)
	{
		return FResult::Error(MutationError());
	}

	// The rename itself is handle-bound, but the source name is part of the
	// acquisition contract too. Refuse if a competitor displaced .part while the
	// verified handle remained open under another name.
	TWoTBootstrapResult<FWoTNativeFileHandle> NamedSource =
		OpenNativeFileAtReadNoFollow(FromDirectory, FromLeaf);
	if (NamedSource.IsError())
	{
		return FResult::Error(MutationError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> NamedIdentity =
		NamedSource.GetValue().QueryCurrentIdentity();
	if (NamedIdentity.IsError())
	{
		return FResult::Error(MoveTemp(NamedIdentity).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> HeldImmediatelyBefore =
		HeldSource.QueryCurrentIdentity();
	if (HeldImmediatelyBefore.IsError())
	{
		return FResult::Error(MoveTemp(HeldImmediatelyBefore).GetError());
	}
	if (NamedIdentity.GetValue() != VerifiedIdentity
		|| HeldImmediatelyBefore.GetValue() != VerifiedIdentity)
	{
		return FResult::Error(MutationError());
	}

	const FString ToString(ToLeaf);
	const DWORD NameBytes = static_cast<DWORD>(ToString.Len() * sizeof(TCHAR));
	TArray<uint8> Storage;
	Storage.SetNumZeroed(
		static_cast<int32>(sizeof(FWoTNtFileRenameInformation) + NameBytes));
	FWoTNtFileRenameInformation* Rename =
		reinterpret_cast<FWoTNtFileRenameInformation*>(Storage.GetData());
	Rename->ReplaceIfExists = 0;
	Rename->RootDirectory = static_cast<HANDLE>(
		FWoTNativeHandleAccess::State(ToDirectory)->Handle);
	Rename->FileNameLength = NameBytes;
	FMemory::Memcpy(Rename->FileName, *ToString, NameBytes);
	// NtSetInformationFile, not SetFileInformationByHandle, for the same
	// reason RenameNativeEntryAtNoFollow documents: the Win32 layer rejects
	// any FILE_RENAME_INFO whose RootDirectory is non-NULL with
	// ERROR_INVALID_PARAMETER (87), in every shape. Probed live on Windows —
	// this exact call failed the acquisition suite until routed through the
	// NT level.
	const FWoTNtSetInformationFile NtSetInformationFile =
		ResolveNtSetInformationFile();
	if (NtSetInformationFile == nullptr)
	{
		return FResult::Error(SystemError(
			TEXT("GetProcAddress(NtSetInformationFile)"),
			ERROR_PROC_NOT_FOUND, FromLeaf));
	}
	IO_STATUS_BLOCK IoStatus = {};
	const NTSTATUS RenameStatus = NtSetInformationFile(
		static_cast<HANDLE>(FWoTNativeHandleAccess::State(HeldSource)->Handle),
		&IoStatus, Storage.GetData(), static_cast<ULONG>(Storage.Num()),
		WoTNtFileRenameInformationClass);
	if (RenameStatus < 0)
	{
		return FResult::Error(SystemError(
			TEXT("NtSetInformationFile(FileRenameInformation held no-replace publish)"),
			NtStatusToWindowsError(RenameStatus), FromLeaf));
	}

	TWoTBootstrapResult<FWoTNativeFileIdentity> HeldAfter =
		HeldSource.QueryCurrentIdentity();
	if (HeldAfter.IsError())
	{
		return FResult::Error(MoveTemp(HeldAfter).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Published =
		OpenNativeFileAtReadNoFollow(ToDirectory, ToLeaf);
	if (Published.IsError())
	{
		return FResult::Error(MoveTemp(Published).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> PublishedIdentity =
		Published.GetValue().QueryCurrentIdentity();
	if (PublishedIdentity.IsError())
	{
		return FResult::Error(MoveTemp(PublishedIdentity).GetError());
	}
	if (!HeldAfter.GetValue().IsSameObject(VerifiedIdentity)
		|| HeldAfter.GetValue().Type != EWoTTreeEntryType::File
		|| HeldAfter.GetValue().LinkCount != 1
		|| PublishedIdentity.GetValue() != HeldAfter.GetValue())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the canonical name is not the held publication source"),
			FString(ToLeaf)));
	}
	return Published;
}

TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> EnumerateNativeDirectory(
	const FWoTNativeDirectoryHandle& Directory)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	TArray<FWoTNativeDirectoryEntry> Entries;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(64 * 1024);
	bool bRestart = true;
	for (;;)
	{
		const FILE_INFO_BY_HANDLE_CLASS InformationClass = bRestart
			? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
		if (!::GetFileInformationByHandleEx(
				static_cast<HANDLE>(FWoTNativeHandleAccess::State(Directory)->Handle),
				InformationClass, Buffer.GetData(), static_cast<DWORD>(Buffer.Num())))
		{
			const uint32 Error = ::GetLastError();
			if (Error == ERROR_NO_MORE_FILES)
			{
				break;
			}
			return FResult::Error(SystemError(
				TEXT("GetFileInformationByHandleEx(directory)"), Error, TEXT("")));
		}
		bRestart = false;
		FILE_ID_BOTH_DIR_INFO* Current =
			reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(Buffer.GetData());
		for (;;)
		{
			const int32 CharacterCount = static_cast<int32>(
				Current->FileNameLength / sizeof(WCHAR));
			const FString Name(CharacterCount, Current->FileName);
			if (Name != TEXT(".") && Name != TEXT(".."))
			{
				FWoTNativeDirectoryEntry& Added = Entries.AddDefaulted_GetRef();
				Added.Name = Name;
				Added.Type = (Current->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
					? EWoTTreeEntryType::SymbolicLink
					: (Current->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
						? EWoTTreeEntryType::Directory : EWoTTreeEntryType::File;
			}
			if (Current->NextEntryOffset == 0)
			{
				break;
			}
			Current = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(
				reinterpret_cast<uint8*>(Current) + Current->NextEntryOffset);
		}
	}
	return FResult::Success(MoveTemp(Entries));
}


namespace
{
FWoTBootstrapStatus RemoveNativeEntryAtNoFollowInternal(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	EWoTTreeEntryType ExpectedType,
	const FWoTNativeFileIdentity* ExpectedIdentity)
{
	if (!Directory.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the directory handle is not open")));
	}
	TWoTBootstrapResult<HANDLE> Entry = OpenOneRelative(
		*FWoTNativeHandleAccess::State(Directory), Leaf,
		DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
		FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT);
	if (Entry.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Entry).GetError());
	}
	FWoTNativeFileIdentity Identity;
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromHandle(Entry.GetValue(), Leaf, Identity); Error.IsSet())
	{
		::CloseHandle(Entry.GetValue());
		return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
	}
	if (Identity.Type != ExpectedType)
	{
		::CloseHandle(Entry.GetValue());
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the entry changed type before handle-relative removal"), FString(Leaf)));
	}
	if (ExpectedIdentity != nullptr && Identity != *ExpectedIdentity)
	{
		::CloseHandle(Entry.GetValue());
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the entry identity changed before handle-relative removal"),
			FString(Leaf)));
	}
	// POSIX-semantics delete first: unlink(2) parity requires that (a) a 0444
	// member in a writable parent is deletable despite its READONLY-attribute
	// projection, and (b) the NAME leaves the directory immediately even while
	// other handles hold the object -- plain disposition keeps the name until
	// the last close, so a parent removal right after deleting a held child
	// fails ERROR_DIR_NOT_EMPTY where POSIX succeeds. Probed: the Ex form with
	// POSIX_SEMANTICS|IGNORE_READONLY removes a read-only, still-held file's
	// name at once and the parent deletes cleanly. The plain-disposition path
	// below remains as the fallback for filesystems without the Ex class.
	struct FWoTFileDispositionInfoEx
	{
		DWORD Flags;
	};
	constexpr DWORD WoTDispositionFlagDelete = 0x1;
	constexpr DWORD WoTDispositionFlagPosixSemantics = 0x2;
	constexpr DWORD WoTDispositionFlagIgnoreReadonly = 0x10;
	FWoTFileDispositionInfoEx DispositionEx = {};
	DispositionEx.Flags = WoTDispositionFlagDelete
		| WoTDispositionFlagPosixSemantics | WoTDispositionFlagIgnoreReadonly;
	bool bDeleted = ::SetFileInformationByHandle(Entry.GetValue(),
		static_cast<FILE_INFO_BY_HANDLE_CLASS>(21) /*FileDispositionInfoEx*/,
		&DispositionEx, sizeof(DispositionEx)) != 0;
	uint32 DeleteError = bDeleted ? ERROR_SUCCESS : ::GetLastError();
	FWoTFileDispositionInfo Disposition = {};
	Disposition.DeleteFile = 1;
	if (!bDeleted)
	{
		bDeleted = ::SetFileInformationByHandle(Entry.GetValue(),
			FileDispositionInfo, &Disposition, sizeof(Disposition)) != 0;
		DeleteError = bDeleted ? ERROR_SUCCESS : ::GetLastError();
	}
	if (!bDeleted && DeleteError == ERROR_ACCESS_DENIED)
	{
		// POSIX unlink parity: a 0444 member in a writable parent is deletable,
		// but its Windows projection sets FILE_ATTRIBUTE_READONLY and the
		// disposition delete refuses read-only objects. Clear the attribute
		// through this same handle (FILE_WRITE_ATTRIBUTES is grantable on
		// read-only objects) and retry once; a genuine denial still surfaces.
		FILE_BASIC_INFO Basic = {};
		if (::GetFileInformationByHandleEx(Entry.GetValue(), FileBasicInfo,
				&Basic, sizeof(Basic))
			&& (Basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
		{
			Basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
			if (Basic.FileAttributes == 0)
			{
				Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
			}
			Basic.CreationTime.QuadPart = 0;
			Basic.LastAccessTime.QuadPart = 0;
			Basic.LastWriteTime.QuadPart = 0;
			Basic.ChangeTime.QuadPart = 0;
			if (::SetFileInformationByHandle(Entry.GetValue(), FileBasicInfo,
					&Basic, sizeof(Basic)))
			{
				bDeleted = ::SetFileInformationByHandle(Entry.GetValue(),
					FileDispositionInfo, &Disposition, sizeof(Disposition)) != 0;
				DeleteError = bDeleted ? ERROR_SUCCESS : ::GetLastError();
			}
		}
	}
	const bool bClosed = ::CloseHandle(Entry.GetValue()) != 0;
	if (!bDeleted)
	{
		return SystemStatus(TEXT("SetFileInformationByHandle(FileDispositionInfo)"),
			DeleteError, Leaf);
	}
	if (!bClosed)
	{
		return SystemStatus(TEXT("CloseHandle"), ::GetLastError(), Leaf);
	}
	return FWoTBootstrapStatus::Success();
}
} // namespace

FWoTBootstrapStatus RemoveNativeEntryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	EWoTTreeEntryType ExpectedType)
{
	return RemoveNativeEntryAtNoFollowInternal(
		Directory, Leaf, ExpectedType, nullptr);
}

FWoTBootstrapStatus RemoveNativeEntryAtNoFollowIfIdentity(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	EWoTTreeEntryType ExpectedType,
	const FWoTNativeFileIdentity& ExpectedIdentity)
{
	return RemoveNativeEntryAtNoFollowInternal(
		Directory, Leaf, ExpectedType, &ExpectedIdentity);
}

TWoTBootstrapResult<FWoTNativeFileLock> AcquireNativeFileLock(
	const FWoTNativeFileHandle& File, EWoTNativeFileLockMode Mode, bool bWait)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileLock>;
	if (!File.IsValid() || File.GetIdentity().Type != EWoTTreeEntryType::File
		|| File.GetIdentity().LinkCount != 1)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("a file lock requires a held single-link regular file")));
	}
	HANDLE Duplicate = INVALID_HANDLE_VALUE;
	if (!::DuplicateHandle(::GetCurrentProcess(),
			static_cast<HANDLE>(FWoTNativeHandleAccess::State(File)->Handle),
			::GetCurrentProcess(), &Duplicate, 0, 0, DUPLICATE_SAME_ACCESS))
	{
		return FResult::Error(SystemError(TEXT("DuplicateHandle"), ::GetLastError(),
			TEXT("")));
	}
	TUniquePtr<FWoTNativeFileLockState> State = MakeUnique<FWoTNativeFileLockState>();
	State->Handle = Duplicate;
	const DWORD Flags = (Mode == EWoTNativeFileLockMode::Exclusive
		? LOCKFILE_EXCLUSIVE_LOCK : 0) | (bWait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
	if (!::LockFileEx(Duplicate, Flags, 0, 1, 0, &State->Range))
	{
		return FResult::Error(SystemError(TEXT("LockFileEx"), ::GetLastError(), TEXT("")));
	}
	State->bLocked = true;
	FWoTNativeFileLock Result;
	FWoTNativeHandleAccess::LockState(Result) = MoveTemp(State);
	return FResult::Success(MoveTemp(Result));
}

#if WITH_AUTOMATION_TESTS
FString GWoTFailDirectoryFinalPathQueryForTesting;
namespace
{
TAtomic<bool> GWoTFailCurrentExecutableFinalPathQueryForTesting(false);
}

void WoTSetCurrentExecutableFinalPathQueryFailureForTesting(bool bFail)
{
	GWoTFailCurrentExecutableFinalPathQueryForTesting.Store(bFail);
}

void WoTSetDirectoryFinalPathQueryFailureForTesting(FStringView AbsolutePath)
{
	GWoTFailDirectoryFinalPathQueryForTesting = FString(AbsolutePath);
	GWoTFailDirectoryFinalPathQueryForTesting.ReplaceInline(
		TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	FPaths::NormalizeDirectoryName(
		GWoTFailDirectoryFinalPathQueryForTesting);
}

TWoTBootstrapResult<FWoTNativeExecutableIdentity>
OpenNativeExecutableIdentityNoFollowForTesting(FStringView AbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeExecutableIdentity>;
	FString Path(AbsolutePath);
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	TWoTBootstrapResult<HANDLE> Handle = OpenPathNoFollow(Path, false);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> Adopted =
		AdoptHandle<FWoTNativeExecutableIdentity>(Handle.GetValue(), Path);
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (TOptional<FWoTBootstrapError> LinkError =
			RejectLink(Adopted.GetValue().GetIdentity(), Path,
				HeldHandle(Adopted.GetValue())); LinkError.IsSet())
	{
		return FResult::Error(MoveTemp(LinkError.GetValue()));
	}
	FWoTNativeHandleAccess::SetReportedPath(
		Adopted.GetValue(), MoveTemp(Path));
	return Adopted;
}
#endif

TWoTBootstrapResult<FWoTNativeExecutableIdentity>
OpenCurrentExecutableIdentityNoFollow(FString* OutPathResolutionFailureReason)
{
	using FResult = TWoTBootstrapResult<FWoTNativeExecutableIdentity>;
	if (OutPathResolutionFailureReason != nullptr)
	{
		OutPathResolutionFailureReason->Reset();
	}

	TArray<TCHAR> Buffer;
	Buffer.SetNumUninitialized(4096);
	const DWORD Length = ::GetModuleFileNameW(nullptr, Buffer.GetData(),
		static_cast<DWORD>(Buffer.Num()));
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num()))
	{
		return FResult::Error(SystemError(TEXT("GetModuleFileNameW"), ::GetLastError(),
			TEXT("")));
	}
	Buffer[static_cast<int32>(Length)] = TEXT('\0');
	FString Path(Buffer.GetData());
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	const FString LoaderPath = Path;

	// Open the loader-reported name without following its final component, then
	// obtain the canonical DOS/UNC spelling from that same held image handle.
	TWoTBootstrapResult<HANDLE> Handle = OpenPathNoFollow(Path, false);
	if (Handle.IsError())
	{
		return FResult::Error(MoveTemp(Handle).GetError());
	}
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> Adopted =
		AdoptHandle<FWoTNativeExecutableIdentity>(Handle.GetValue(), Path);
	if (Adopted.IsError())
	{
		return Adopted;
	}
	if (TOptional<FWoTBootstrapError> LinkError =
			RejectLink(Adopted.GetValue().GetIdentity(), Path,
				HeldHandle(Adopted.GetValue())); LinkError.IsSet())
	{
		return FResult::Error(MoveTemp(LinkError.GetValue()));
	}
	const HANDLE ImageHandle = static_cast<HANDLE>(
		FWoTNativeHandleAccess::State(Adopted.GetValue())->Handle);
	constexpr DWORD FinalPathFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
	DWORD FinalCapacity = 0;
#if WITH_AUTOMATION_TESTS
	if (!GWoTFailCurrentExecutableFinalPathQueryForTesting.Load())
#endif
	{
		FinalCapacity = ::GetFinalPathNameByHandleW(
			ImageHandle, nullptr, 0, FinalPathFlags);
	}
	if (FinalCapacity == 0)
	{
		if (OutPathResolutionFailureReason != nullptr)
		{
			*OutPathResolutionFailureReason = TEXT("executable-path-unresolved");
		}
		FWoTNativeHandleAccess::SetReportedPath(
			Adopted.GetValue(), FString(LoaderPath));
		return Adopted;
	}
	TArray<WCHAR> FinalBuffer;
	FinalBuffer.SetNumUninitialized(static_cast<int32>(FinalCapacity) + 1);
	const DWORD FinalLength = ::GetFinalPathNameByHandleW(
		ImageHandle, FinalBuffer.GetData(),
		static_cast<DWORD>(FinalBuffer.Num()), FinalPathFlags);
	if (FinalLength == 0
		|| FinalLength >= static_cast<DWORD>(FinalBuffer.Num()))
	{
		if (OutPathResolutionFailureReason != nullptr)
		{
			*OutPathResolutionFailureReason = TEXT("executable-path-unresolved");
		}
		FWoTNativeHandleAccess::SetReportedPath(
			Adopted.GetValue(), FString(LoaderPath));
		return Adopted;
	}
	Path = FString(static_cast<int32>(FinalLength), FinalBuffer.GetData());
	if (Path.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::CaseSensitive))
	{
		Path = TEXT("//") + Path.RightChop(8);
	}
	else if (Path.StartsWith(TEXT("\\\\?\\"), ESearchCase::CaseSensitive))
	{
		Path = Path.RightChop(4);
	}
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	const bool bDrivePath = Path.Len() >= 3 && FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':') && Path[2] == TEXT('/');
	const bool bUncPath = Path.StartsWith(TEXT("//"), ESearchCase::CaseSensitive)
		&& !Path.StartsWith(TEXT("//?/"), ESearchCase::CaseSensitive);
	if (!bDrivePath && !bUncPath)
	{
		if (OutPathResolutionFailureReason != nullptr)
		{
			*OutPathResolutionFailureReason = TEXT("executable-path-unresolved");
		}
		FWoTNativeHandleAccess::SetReportedPath(
			Adopted.GetValue(), FString(LoaderPath));
		return Adopted;
	}
	FWoTNativeHandleAccess::SetReportedPath(Adopted.GetValue(), MoveTemp(Path));
	return Adopted;
}
