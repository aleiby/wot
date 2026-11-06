// macOS link-refusing native handles.
//
// The rule this file exists to enforce: never let the kernel resolve a symlink on
// our behalf. Owned-object opens use O_NOFOLLOW and every descent goes through
// `openat` from an already-held descriptor. The one following open below is
// private to canonicalizing a directory above an owned root; its handle identity
// must match a canonical all-components-no-follow reopen before it is trusted.
#include "WoTBootstrapCore/NativeHandles.h"

#include "../CanonicalNativeDirectoryPlatform.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

struct FWoTNativeHandleState
{
	int Descriptor = -1;
	FWoTNativeFileIdentity Identity;

	~FWoTNativeHandleState()
	{
		if (Descriptor >= 0)
		{
			close(Descriptor);
			Descriptor = -1;
		}
	}
};

struct FWoTNativeFileLockState
{
	int Descriptor = -1;
	bool bLocked = false;

	~FWoTNativeFileLockState()
	{
		if (Descriptor >= 0)
		{
			if (bLocked)
			{
				(void)flock(Descriptor, LOCK_UN);
			}
			(void)close(Descriptor);
		}
	}
};

#if WITH_AUTOMATION_TESTS
FString GWoTNativeDirectoryEnumerationRemovalLeafForTesting;
#endif

/** Grants the free functions here access to the handles' single state pointer. */
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
FWoTBootstrapError SystemError(const TCHAR* What, int ErrorNumber, FStringView Path)
{
	FWoTBootstrapError Error = MakeWoTBootstrapError(
		ErrorNumber == ELOOP ? EWoTBootstrapError::UnsafePath : EWoTBootstrapError::Io,
		FString::Printf(TEXT("%s failed: %hs"), What, strerror(ErrorNumber)),
		FString(Path));
	Error.NativeCode = static_cast<uint32>(ErrorNumber);
	return Error;
}

FWoTBootstrapStatus SystemStatus(const TCHAR* What, int ErrorNumber, FStringView Path)
{
	return FWoTBootstrapStatus::Error(SystemError(What, ErrorNumber, Path));
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
			FString(Leaf)));
	}
	return FWoTBootstrapStatus::Success();
}

TOptional<FWoTBootstrapError> IdentityFromDescriptor(int Descriptor,
	FStringView Path, FWoTNativeFileIdentity& OutIdentity)
{
	struct stat Status = {};
	if (fstat(Descriptor, &Status) != 0)
	{
		return SystemError(TEXT("fstat"), errno, Path);
	}

	OutIdentity.VolumeId = static_cast<uint64>(Status.st_dev);
	OutIdentity.FileIdLow = static_cast<uint64>(Status.st_ino);
	OutIdentity.FileIdHigh = 0;
	OutIdentity.Size = static_cast<uint64>(Status.st_size);
	// Change time, not modification time: touching content or metadata both move
	// it, and unlike mtime it cannot be set backwards by the writer.
	OutIdentity.ChangeState = static_cast<uint64>(Status.st_ctimespec.tv_sec) * 1000000000ull
		+ static_cast<uint64>(Status.st_ctimespec.tv_nsec);
	OutIdentity.LinkCount = static_cast<uint32>(Status.st_nlink);

	if (S_ISDIR(Status.st_mode))
	{
		OutIdentity.Type = EWoTTreeEntryType::Directory;
	}
	else if (S_ISLNK(Status.st_mode))
	{
		OutIdentity.Type = EWoTTreeEntryType::SymbolicLink;
	}
	else if (S_ISREG(Status.st_mode))
	{
		OutIdentity.Type = EWoTTreeEntryType::File;
	}
	else
	{
		// Sockets, FIFOs, and devices are not tree content. Reading one could block
		// forever or have side effects, so they are refused at the door.
		return MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the object is not a regular file, directory, or symlink"), FString(Path));
	}
	return {};
}

TWoTBootstrapResult<int> OpenAbsolute(FStringView Path, int ExtraFlags)
{
	using FResult = TWoTBootstrapResult<int>;
	const FString Absolute(Path);
	if (!Absolute.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("an absolute path is required"), Absolute));
	}

	const FTCHARToUTF8 Utf8(*Absolute);
	// O_NOFOLLOW applies to the FINAL component only; intermediate components of an
	// absolute root are the caller's trust assumption, which is why every path
	// BELOW a root is walked with openat instead.
	const int Descriptor = open(Utf8.Get(),
		O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | ExtraFlags);
	if (Descriptor < 0)
	{
		return FResult::Error(SystemError(TEXT("open"), errno, Absolute));
	}
	return FResult::Success(Descriptor);
}

TWoTBootstrapResult<int> OpenRelativeThroughComponents(int RootDescriptor,
	FStringView RelativePath, int FinalExtraFlags)
{
	using FResult = TWoTBootstrapResult<int>;

	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(RelativePath); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}

	TArray<FString> Components;
	FString(RelativePath).ParseIntoArray(Components, TEXT("/"), true);

	int Current = dup(RootDescriptor);
	if (Current < 0)
	{
		return FResult::Error(SystemError(TEXT("dup"), errno, RelativePath));
	}

	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		const bool bFinal = Index == Components.Num() - 1;
		const int Flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
			| (bFinal ? FinalExtraFlags : O_DIRECTORY);

		const FTCHARToUTF8 Utf8(*Components[Index]);
		// One component at a time: validating a whole path once would leave every
		// directory in it swappable between the check and the use.
		const int Next = openat(Current, Utf8.Get(), Flags);
		const int OpenError = errno;
		close(Current);
		if (Next < 0)
		{
			return FResult::Error(SystemError(TEXT("openat"), OpenError, Components[Index]));
		}
		Current = Next;
	}
	return FResult::Success(Current);
}

template<class HandleType>
TWoTBootstrapResult<HandleType> AdoptDescriptor(int Descriptor, FStringView Path)
{
	using FResult = TWoTBootstrapResult<HandleType>;

	TUniquePtr<FWoTNativeHandleState> State = MakeUnique<FWoTNativeHandleState>();
	State->Descriptor = Descriptor;
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromDescriptor(Descriptor, Path, State->Identity); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	// Metadata opens are nonblocking so a hostile FIFO/device cannot stall before
	// fstat classifies it. Regular files are safe to read, so restore blocking
	// semantics before returning the handle used by the one bounded read.
	if (State->Identity.Type == EWoTTreeEntryType::File)
	{
		const int Flags = fcntl(Descriptor, F_GETFL);
		if (Flags < 0 || fcntl(Descriptor, F_SETFL, Flags & ~O_NONBLOCK) != 0)
		{
			return FResult::Error(SystemError(TEXT("fcntl"), errno, Path));
		}
	}

	HandleType Handle;
	FWoTNativeHandleAccess::State(Handle) = MoveTemp(State);
	return FResult::Success(MoveTemp(Handle));
}

template<class HandleType>
TWoTBootstrapResult<HandleType> DuplicateHandle(const HandleType& Source)
{
	using FResult = TWoTBootstrapResult<HandleType>;

	const TUniquePtr<FWoTNativeHandleState>& State = FWoTNativeHandleAccess::State(Source);
	if (!State.IsValid() || State->Descriptor < 0)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the handle is not open")));
	}

	const int Duplicate = dup(State->Descriptor);
	if (Duplicate < 0)
	{
		return FResult::Error(SystemError(TEXT("dup"), errno, TEXT("")));
	}

	TWoTBootstrapResult<HandleType> Adopted =
		AdoptDescriptor<HandleType>(Duplicate, TEXT(""));
	if (Adopted.IsError())
	{
		return Adopted;
	}
	// A duplicate must describe the SAME object. If the identity moved between the
	// original open and now, the caller's assumptions are already void.
	if (FWoTNativeHandleAccess::State(Adopted.GetValue())->Identity != State->Identity)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the held object changed identity while it was open")));
	}
	return Adopted;
}
}  // namespace

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
	return State.IsValid() && State->Descriptor >= 0 && State->bLocked;
}

FWoTBootstrapStatus FWoTNativeFileLock::Release()
{
	if (!State.IsValid())
	{
		return FWoTBootstrapStatus::Success();
	}
	const int Descriptor = State->Descriptor;
	const bool bWasLocked = State->bLocked;
	State->Descriptor = -1;
	State->bLocked = false;
	const int UnlockResult = bWasLocked ? flock(Descriptor, LOCK_UN) : 0;
	const int UnlockError = errno;
	const int CloseResult = Descriptor >= 0 ? close(Descriptor) : 0;
	const int CloseError = errno;
	State.Reset();
	if (UnlockResult != 0)
	{
		return SystemStatus(TEXT("flock(LOCK_UN)"), UnlockError, TEXT(""));
	}
	if (CloseResult != 0)
	{
		return SystemStatus(TEXT("close"), CloseError, TEXT(""));
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
	return State.IsValid() && State->Descriptor >= 0;
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
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromDescriptor(State->Descriptor, TEXT(""), Identity); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	return FResult::Success(Identity);
}

TWoTBootstrapResult<FWoTNativeFileHandle> FWoTNativeFileHandle::DuplicateForRead() const
{
	return DuplicateHandle<FWoTNativeFileHandle>(*this);
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
	if (State->Identity.Size > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("the file is too large to fit in a byte array")));
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(State->Identity.Size));
	int64 Offset = 0;
	while (Offset < static_cast<int64>(State->Identity.Size))
	{
		// pread, so reading never disturbs a shared file offset and a partial read
		// resumes exactly where it stopped.
		const ssize_t Read = pread(State->Descriptor, Bytes.GetData() + Offset,
			static_cast<size_t>(State->Identity.Size - Offset), Offset);
		if (Read < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return FResult::Error(SystemError(TEXT("pread"), errno, TEXT("")));
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
	return State.IsValid() && State->Descriptor >= 0;
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
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromDescriptor(State->Descriptor, TEXT(""), Identity); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	return FResult::Success(Identity);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> FWoTNativeDirectoryHandle::DuplicateForRead() const
{
	return DuplicateHandle<FWoTNativeDirectoryHandle>(*this);
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
	return State.IsValid() && State->Descriptor >= 0;
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
	if (TOptional<FWoTBootstrapError> Error =
			IdentityFromDescriptor(State->Descriptor, TEXT(""), Identity); Error.IsSet())
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
	TWoTBootstrapResult<int> Descriptor = OpenAbsolute(CanonicalAbsolutePath, 0);
	if (Descriptor.IsError())
	{
		return TWoTBootstrapResult<FWoTNativeFileHandle>::Error(
			MoveTemp(Descriptor).GetError());
	}
	return AdoptDescriptor<FWoTNativeFileHandle>(Descriptor.GetValue(),
		CanonicalAbsolutePath);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenNativeDirectoryNoFollow(
	FStringView CanonicalAbsolutePath)
{
	TWoTBootstrapResult<int> Descriptor = OpenAbsolute(CanonicalAbsolutePath, O_DIRECTORY);
	if (Descriptor.IsError())
	{
		return TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Error(
			MoveTemp(Descriptor).GetError());
	}
	return AdoptDescriptor<FWoTNativeDirectoryHandle>(Descriptor.GetValue(),
		CanonicalAbsolutePath);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle>
WoTOpenNativeDirectoryFollowingForCanonicalization(
	FStringView LaunchAbsolutePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	const FString Absolute(LaunchAbsolutePath);
	if (!Absolute.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("an absolute ancestor path is required"), Absolute));
	}
	const FTCHARToUTF8 Utf8(*Absolute);
	const int Descriptor = open(Utf8.Get(),
		O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
	if (Descriptor < 0)
	{
		return FResult::Error(SystemError(
			TEXT("open(canonical ancestor)"), errno, Absolute));
	}
	return AdoptDescriptor<FWoTNativeDirectoryHandle>(Descriptor, Absolute);
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
	ANSICHAR Buffer[PATH_MAX] = {};
	if (fcntl(FWoTNativeHandleAccess::State(Directory)->Descriptor,
			F_GETPATH, Buffer) != 0)
	{
		return FResult::Error(SystemError(
			TEXT("fcntl(F_GETPATH)"), errno, TEXT("held directory")));
	}
	const FUTF8ToTCHAR Converted(Buffer);
	FString Path(Converted.Length(), Converted.Get());
	if (!Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("F_GETPATH returned a non-absolute directory path"), Path));
	}
	return FResult::Success(MoveTemp(Path));
}

TWoTBootstrapResult<uint64> QueryNativeDirectoryAvailableBytes(
	const FWoTNativeDirectoryHandle& Directory)
{
	using FResult = TWoTBootstrapResult<uint64>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	struct statfs Status = {};
	if (fstatfs(FWoTNativeHandleAccess::State(Directory)->Descriptor,
			&Status) != 0)
	{
		return FResult::Error(SystemError(
			TEXT("fstatfs"), errno, TEXT("held directory")));
	}
	return FResult::Success(static_cast<uint64>(Status.f_bavail)
		* static_cast<uint64>(Status.f_bsize));
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
	ANSICHAR Buffer[PATH_MAX] = {};
	if (fcntl(FWoTNativeHandleAccess::State(File)->Descriptor,
			F_GETPATH, Buffer) != 0)
	{
		return FResult::Error(SystemError(
			TEXT("fcntl(F_GETPATH)"), errno, TEXT("held file")));
	}
	const FUTF8ToTCHAR Converted(Buffer);
	FString Path(Converted.Length(), Converted.Get());
	return Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
		? FResult::Success(MoveTemp(Path))
		: FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("F_GETPATH returned a non-absolute file path"), Path));
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
	TWoTBootstrapResult<int> Descriptor = OpenRelativeThroughComponents(
		FWoTNativeHandleAccess::State(Directory)->Descriptor, CanonicalRelativePath, 0);
	if (Descriptor.IsError())
	{
		return FResult::Error(MoveTemp(Descriptor).GetError());
	}
	return AdoptDescriptor<FWoTNativeFileHandle>(Descriptor.GetValue(), CanonicalRelativePath);
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
	TWoTBootstrapResult<int> Descriptor = OpenRelativeThroughComponents(
		FWoTNativeHandleAccess::State(Directory)->Descriptor, CanonicalRelativePath,
		O_DIRECTORY);
	if (Descriptor.IsError())
	{
		return FResult::Error(MoveTemp(Descriptor).GetError());
	}
	return AdoptDescriptor<FWoTNativeDirectoryHandle>(Descriptor.GetValue(),
		CanonicalRelativePath);
}

template <typename HandleType>
TWoTBootstrapResult<HandleType> OpenAbsolutePathAllComponentsNoFollow(
	FStringView CanonicalAbsolutePath, int FinalExtraFlags)
{
	using FResult = TWoTBootstrapResult<HandleType>;
	const FString Absolute(CanonicalAbsolutePath);
	if (!Absolute.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
		|| Absolute.Len() <= 1)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("a non-root absolute path is required"), Absolute));
	}
	const FString Relative = Absolute.RightChop(1);
	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(Relative);
		Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const int FilesystemRoot = open("/",
		O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (FilesystemRoot < 0)
	{
		return FResult::Error(SystemError(
			TEXT("open(filesystem root)"), errno, TEXT("/")));
	}
	TWoTBootstrapResult<int> Descriptor = OpenRelativeThroughComponents(
		FilesystemRoot, Relative, FinalExtraFlags);
	close(FilesystemRoot);
	if (Descriptor.IsError())
	{
		return FResult::Error(MoveTemp(Descriptor).GetError());
	}
	return AdoptDescriptor<HandleType>(
		Descriptor.GetValue(), CanonicalAbsolutePath);
}

TWoTBootstrapResult<FWoTNativeFileHandle>
OpenNativeFilePathReadNoFollow(FStringView CanonicalAbsolutePath)
{
	return OpenAbsolutePathAllComponentsNoFollow<FWoTNativeFileHandle>(
		CanonicalAbsolutePath, 0);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle>
OpenNativeDirectoryPathNoFollow(FStringView CanonicalAbsolutePath)
{
	return OpenAbsolutePathAllComponentsNoFollow<FWoTNativeDirectoryHandle>(
		CanonicalAbsolutePath, O_DIRECTORY);
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
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	const int Descriptor = openat(FWoTNativeHandleAccess::State(Directory)->Descriptor,
		LeafUtf8.Get(), O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC
			| O_NOFOLLOW, 0600);
	if (Descriptor < 0)
	{
		return FResult::Error(SystemError(TEXT("openat(create)"), errno, Leaf));
	}
	return AdoptDescriptor<FWoTNativeFileHandle>(Descriptor, Leaf);
}

TWoTBootstrapResult<bool> ProbeNativeDirectoryWritableNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf)
{
	using FResult = TWoTBootstrapResult<bool>;
	(void)UniqueLeaf;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the directory handle is not open")));
	}
	const int Root = FWoTNativeHandleAccess::State(Directory)->Descriptor;
	if (faccessat(Root, ".", W_OK, AT_EACCESS) == 0)
	{
		return FResult::Success(true);
	}
	const int AccessError = errno;
	if (AccessError == EROFS || AccessError == EACCES || AccessError == EPERM)
	{
		return FResult::Success(false);
	}
	return FResult::Error(SystemError(
		TEXT("faccessat(AT_EACCESS)"), AccessError, TEXT(".")));
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<bool> ProbeNativeDirectoryWritableNoFollowForTesting(
	const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf,
	TFunction<void(FStringView)> AfterCreate)
{
	(void)AfterCreate;
	return ProbeNativeDirectoryWritableNoFollow(Directory, UniqueLeaf);
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
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	const int Root = FWoTNativeHandleAccess::State(Directory)->Descriptor;
	int Descriptor = openat(Root, LeafUtf8.Get(),
		O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (Descriptor >= 0)
	{
		bOutCreated = true;
	}
	else if (errno == EEXIST)
	{
		Descriptor = openat(Root, LeafUtf8.Get(),
			O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	}
	if (Descriptor < 0)
	{
		return FResult::Error(SystemError(TEXT("openat(open-or-create)"), errno, Leaf));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Result =
		AdoptDescriptor<FWoTNativeFileHandle>(Descriptor, Leaf);
	if (!Result.IsError())
	{
		const FWoTNativeFileIdentity& Identity = Result.GetValue().GetIdentity();
		if (Identity.Type != EWoTTreeEntryType::File || Identity.LinkCount != 1)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the opened lock object is not a single-link regular file"),
				FString(Leaf)));
		}
	}
	return Result;
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
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	const int Root = FWoTNativeHandleAccess::State(Directory)->Descriptor;
	if (mkdirat(Root, LeafUtf8.Get(), 0700) != 0)
	{
		return FResult::Error(SystemError(TEXT("mkdirat"), errno, Leaf));
	}
	const int Descriptor = openat(Root, LeafUtf8.Get(),
		O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (Descriptor < 0)
	{
		return FResult::Error(SystemError(TEXT("openat(directory)"), errno, Leaf));
	}
	return AdoptDescriptor<FWoTNativeDirectoryHandle>(Descriptor, Leaf);
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenOrCreateNativeDirectoryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf, bool& bOutCreated)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	bOutCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Created =
		CreateNativeDirectoryAtNoFollow(Directory, Leaf);
	if (!Created.IsError())
	{
		bOutCreated = true;
		return Created;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryAtNoFollow(Directory, Leaf);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	return Opened;
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
	if (ftruncate(FWoTNativeHandleAccess::State(File)->Descriptor,
			static_cast<off_t>(Size)) != 0)
	{
		return SystemStatus(TEXT("ftruncate"), errno, TEXT(""));
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
	uint64 WrittenTotal = 0;
	while (WrittenTotal < static_cast<uint64>(Bytes.Num()))
	{
		const ssize_t Written = pwrite(
			FWoTNativeHandleAccess::State(File)->Descriptor,
			Bytes.GetData() + WrittenTotal,
			static_cast<size_t>(static_cast<uint64>(Bytes.Num()) - WrittenTotal),
			static_cast<off_t>(Offset + WrittenTotal));
		if (Written < 0 && errno == EINTR)
		{
			continue;
		}
		if (Written <= 0)
		{
			return SystemStatus(TEXT("pwrite"), errno, TEXT(""));
		}
		WrittenTotal += static_cast<uint64>(Written);
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
	for (;;)
	{
		const ssize_t Read = pread(FWoTNativeHandleAccess::State(File)->Descriptor,
			Buffer.GetData(), static_cast<size_t>(Buffer.Num()),
			static_cast<off_t>(Offset));
		if (Read < 0 && errno == EINTR)
		{
			continue;
		}
		if (Read < 0)
		{
			return FResult::Error(SystemError(TEXT("pread"), errno, TEXT("")));
		}
		return FResult::Success(static_cast<uint64>(Read));
	}
}

FWoTBootstrapStatus FlushNativeFile(const FWoTNativeFileHandle& File)
{
	if (!File.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the file handle is not open")));
	}
	if (fcntl(FWoTNativeHandleAccess::State(File)->Descriptor, F_FULLFSYNC) != 0)
	{
		return SystemStatus(TEXT("fcntl(F_FULLFSYNC)"), errno, TEXT(""));
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
	if (fsync(FWoTNativeHandleAccess::State(Directory)->Descriptor) != 0)
	{
		return SystemStatus(TEXT("fsync"), errno, TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

template <typename HandleType>
TWoTBootstrapResult<uint32> QueryModeFromHeldMacHandle(const HandleType& Handle)
{
	using FResult = TWoTBootstrapResult<uint32>;
	if (!Handle.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the native handle is not open")));
	}
	struct stat Information = {};
	if (fstat(FWoTNativeHandleAccess::State(Handle)->Descriptor, &Information) != 0)
	{
		return FResult::Error(SystemError(TEXT("fstat(mode)"), errno, TEXT("")));
	}
	return FResult::Success(static_cast<uint32>(Information.st_mode & 0777));
}

template <typename HandleType>
FWoTBootstrapStatus SetModeOnHeldMacHandle(const HandleType& Handle, uint32 Mode)
{
	if (!Handle.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the native handle is not open")));
	}
	if ((Mode & ~0777u) != 0)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the native mode contains non-permission bits")));
	}
	if (fchmod(FWoTNativeHandleAccess::State(Handle)->Descriptor,
			static_cast<mode_t>(Mode)) != 0)
	{
		return SystemStatus(TEXT("fchmod"), errno, TEXT(""));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<uint32> QueryNativeMode(const FWoTNativeFileHandle& File)
{
	return QueryModeFromHeldMacHandle(File);
}

TWoTBootstrapResult<uint32> QueryNativeMode(const FWoTNativeDirectoryHandle& Directory)
{
	return QueryModeFromHeldMacHandle(Directory);
}

template <typename HandleType>
TWoTBootstrapResult<bool> IsHeldMacHandleOwnedByCurrentUser(
	const HandleType& Handle)
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!Handle.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("the native handle is not open")));
	}
	struct stat Information = {};
	if (fstat(FWoTNativeHandleAccess::State(Handle)->Descriptor,
			&Information) != 0)
	{
		return FResult::Error(SystemError(
			TEXT("fstat(owner)"), errno, TEXT("")));
	}
	return FResult::Success(Information.st_uid == getuid());
}

TWoTBootstrapResult<bool> IsNativeOwnedByCurrentUser(
	const FWoTNativeFileHandle& File)
{
	return IsHeldMacHandleOwnedByCurrentUser(File);
}

TWoTBootstrapResult<bool> IsNativeOwnedByCurrentUser(
	const FWoTNativeDirectoryHandle& Directory)
{
	return IsHeldMacHandleOwnedByCurrentUser(Directory);
}

FWoTBootstrapStatus SetNativeMode(const FWoTNativeFileHandle& File, uint32 Mode)
{
	return SetModeOnHeldMacHandle(File, Mode);
}

FWoTBootstrapStatus SetNativeMode(const FWoTNativeDirectoryHandle& Directory, uint32 Mode)
{
	return SetModeOnHeldMacHandle(Directory, Mode);
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
	const FString FromString(FromLeaf);
	const FString ToString(ToLeaf);
	const FTCHARToUTF8 FromUtf8(*FromString);
	const FTCHARToUTF8 ToUtf8(*ToString);
	if (renameatx_np(FWoTNativeHandleAccess::State(FromDirectory)->Descriptor,
			FromUtf8.Get(), FWoTNativeHandleAccess::State(ToDirectory)->Descriptor,
			ToUtf8.Get(), bReplaceExisting ? 0 : RENAME_EXCL) != 0)
	{
		return SystemStatus(TEXT("renameatx_np"), errno, FromLeaf);
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<bool> ExchangeNativeEntriesAtNoFollow(
	const FWoTNativeDirectoryHandle& DirectoryA, FStringView LeafA,
	const FWoTNativeDirectoryHandle& DirectoryB, FStringView LeafB)
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!DirectoryA.IsValid() || !DirectoryB.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("an exchange directory handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(LeafA); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(LeafB); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	const FString LeafAString(LeafA);
	const FString LeafBString(LeafB);
	const FTCHARToUTF8 LeafAUtf8(*LeafAString);
	const FTCHARToUTF8 LeafBUtf8(*LeafBString);
	if (renameatx_np(FWoTNativeHandleAccess::State(DirectoryA)->Descriptor,
			LeafAUtf8.Get(),
			FWoTNativeHandleAccess::State(DirectoryB)->Descriptor,
			LeafBUtf8.Get(), RENAME_SWAP) == 0)
	{
		// The activation layer owns the durability edge and flushes both held
		// parent directories immediately after this atomic namespace operation.
		return FResult::Success(true);
	}
	const int ExchangeError = errno;
	if (ExchangeError == ENOTSUP || ExchangeError == EINVAL)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(
				TEXT("renameatx_np(RENAME_SWAP) is unsupported: %hs"),
				strerror(ExchangeError)), FString(LeafA)));
	}
	return FResult::Error(
		SystemError(TEXT("renameatx_np(RENAME_SWAP)"), ExchangeError, LeafA));
}

FWoTBootstrapStatus CreateNativeSymbolicLinkAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	FStringView Target)
{
	if (!Directory.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("a symbolic-link parent handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
		return Safe;
	const FString LeafString(Leaf);
	const FString TargetString(Target);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	const FTCHARToUTF8 TargetUtf8(*TargetString);
	if (symlinkat(TargetUtf8.Get(),
			FWoTNativeHandleAccess::State(Directory)->Descriptor,
			LeafUtf8.Get()) != 0)
	{
		return SystemStatus(TEXT("symlinkat"), errno, Leaf);
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FString> ReadNativeSymbolicLinkAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("a symbolic-link parent handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
		return FResult::Error(MoveTemp(Safe).GetError());
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	ANSICHAR Buffer[PATH_MAX + 1] = {};
	const ssize_t Count = readlinkat(
		FWoTNativeHandleAccess::State(Directory)->Descriptor,
		LeafUtf8.Get(), Buffer, PATH_MAX);
	if (Count < 0)
		return FResult::Error(SystemError(TEXT("readlinkat"), errno, Leaf));
	if (Count == PATH_MAX)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("a symbolic-link target exceeds PATH_MAX"), FString(Leaf)));
	}
	const FUTF8ToTCHAR Converted(Buffer, static_cast<int32>(Count));
	return FResult::Success(FString::ConstructFromPtrSize(
		Converted.Get(), Converted.Length()));
}

TWoTBootstrapResult<uint32> QueryNativeSymbolicLinkModeAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<uint32>;
	if (!Directory.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("a symbolic-link parent handle is not open")));
	}
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
		return FResult::Error(MoveTemp(Safe).GetError());
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	struct stat Facts = {};
	if (fstatat(FWoTNativeHandleAccess::State(Directory)->Descriptor,
			LeafUtf8.Get(), &Facts, AT_SYMLINK_NOFOLLOW) != 0)
		return FResult::Error(SystemError(TEXT("fstatat"), errno, Leaf));
	if (!S_ISLNK(Facts.st_mode))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the named entry is not a symbolic link"), FString(Leaf)));
	}
	return FResult::Success(static_cast<uint32>(Facts.st_mode & 0777));
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

	// renameatx_np still takes a source name. Hold the object reached by that
	// exact parent-relative name and prove that it is our verified handle before
	// allowing the kernel to consume the name.
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

	const FString FromString(FromLeaf);
	const FString ToString(ToLeaf);
	const FTCHARToUTF8 FromUtf8(*FromString);
	const FTCHARToUTF8 ToUtf8(*ToString);
	if (renameatx_np(FWoTNativeHandleAccess::State(FromDirectory)->Descriptor,
			FromUtf8.Get(), FWoTNativeHandleAccess::State(ToDirectory)->Descriptor,
			ToUtf8.Get(), RENAME_EXCL) != 0)
	{
		return FResult::Error(SystemError(
			TEXT("renameatx_np(held no-replace publish)"), errno, FromLeaf));
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
	// dup() would share the directory stream offset with the long-lived held
	// handle.  After one enumeration, every later enumeration of that handle
	// would therefore begin at EOF and silently report an empty directory.  Open
	// the held directory itself to get an independent open-file description and
	// a fresh cursor for every pass.
	const int EnumerationDescriptor = openat(
		FWoTNativeHandleAccess::State(Directory)->Descriptor, ".",
		O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (EnumerationDescriptor < 0)
	{
		return FResult::Error(SystemError(
			TEXT("openat(directory enumeration)"), errno, TEXT("")));
	}
	DIR* Stream = fdopendir(EnumerationDescriptor);
	if (Stream == nullptr)
	{
		const int Error = errno;
		close(EnumerationDescriptor);
		return FResult::Error(SystemError(TEXT("fdopendir"), Error, TEXT("")));
	}
	TArray<FWoTNativeDirectoryEntry> Entries;
	int ReadError = 0;
	for (;;)
	{
		errno = 0;
		dirent* Entry = readdir(Stream);
		if (Entry == nullptr)
		{
			ReadError = errno;
			break;
		}
		if (strcmp(Entry->d_name, ".") == 0 || strcmp(Entry->d_name, "..") == 0)
		{
			continue;
		}
#if WITH_AUTOMATION_TESTS
		if (!GWoTNativeDirectoryEnumerationRemovalLeafForTesting.IsEmpty())
		{
			const FTCHARToUTF8 RemovalLeafUtf8(
				*GWoTNativeDirectoryEnumerationRemovalLeafForTesting);
			if (strcmp(Entry->d_name, RemovalLeafUtf8.Get()) == 0)
			{
				GWoTNativeDirectoryEnumerationRemovalLeafForTesting.Empty();
				(void)unlinkat(FWoTNativeHandleAccess::State(Directory)->Descriptor,
					Entry->d_name, 0);
			}
		}
#endif
		struct stat Information = {};
		if (fstatat(FWoTNativeHandleAccess::State(Directory)->Descriptor,
				Entry->d_name, &Information, AT_SYMLINK_NOFOLLOW) != 0)
		{
			const int Error = errno;
			if (Error == ENOENT)
			{
				// The name ceased to exist after readdir exposed it. Its absence is
				// the correct result for this pass; every other failure remains fatal.
				continue;
			}
			closedir(Stream);
			return FResult::Error(SystemError(TEXT("fstatat"), Error, TEXT("")));
		}
		FWoTNativeDirectoryEntry& Added = Entries.AddDefaulted_GetRef();
		const FUTF8ToTCHAR Converted(Entry->d_name);
		Added.Name = FString(Converted.Get());
		const FTCHARToUTF8 RoundTrip(*Added.Name);
		const int32 OriginalLength = static_cast<int32>(strlen(Entry->d_name));
		if (RoundTrip.Length() != OriginalLength
			|| FMemory::Memcmp(RoundTrip.Get(), Entry->d_name, OriginalLength) != 0)
		{
			closedir(Stream);
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::InvalidUtf8,
				TEXT("a directory entry name is not valid UTF-8")));
		}
		Added.Type = S_ISDIR(Information.st_mode) ? EWoTTreeEntryType::Directory
			: S_ISREG(Information.st_mode) ? EWoTTreeEntryType::File
			: EWoTTreeEntryType::SymbolicLink;
	}
	if (closedir(Stream) != 0 && ReadError == 0)
	{
		return FResult::Error(SystemError(TEXT("closedir"), errno, TEXT("")));
	}
	if (ReadError != 0)
	{
		return FResult::Error(SystemError(TEXT("readdir"), ReadError, TEXT("")));
	}
	return FResult::Success(MoveTemp(Entries));
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
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	struct stat Information = {};
	const FTCHARToUTF8 LeafUtf8(*FString(Leaf));
	if (fstatat(FWoTNativeHandleAccess::State(Directory)->Descriptor,
			LeafUtf8.Get(), &Information, AT_SYMLINK_NOFOLLOW) != 0)
	{
		const int Error = errno;
		if (Error == ENOENT || Error == ENOTDIR)
		{
			return FResult::Success(TOptional<EWoTTreeEntryType>());
		}
		return FResult::Error(SystemError(TEXT("fstatat"), Error, Leaf));
	}
	const EWoTTreeEntryType Type = S_ISREG(Information.st_mode)
		? EWoTTreeEntryType::File
		: S_ISDIR(Information.st_mode) ? EWoTTreeEntryType::Directory
		: EWoTTreeEntryType::SymbolicLink;
	return FResult::Success(TOptional<EWoTTreeEntryType>(Type));
}

#if WITH_AUTOMATION_TESTS
void WoTSetNativeDirectoryEnumerationRemovalLeafForTesting(FStringView Leaf)
{
	GWoTNativeDirectoryEnumerationRemovalLeafForTesting = FString(Leaf);
}
#endif

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
	if (FWoTBootstrapStatus Safe = RequireSingleLeaf(Leaf); Safe.IsError())
	{
		return Safe;
	}
	const FString LeafString(Leaf);
	const FTCHARToUTF8 LeafUtf8(*LeafString);
	struct stat Information = {};
	const int Root = FWoTNativeHandleAccess::State(Directory)->Descriptor;
	if (fstatat(Root, LeafUtf8.Get(), &Information, AT_SYMLINK_NOFOLLOW) != 0)
	{
		return SystemStatus(TEXT("fstatat"), errno, Leaf);
	}
	const EWoTTreeEntryType Actual = S_ISDIR(Information.st_mode)
		? EWoTTreeEntryType::Directory
		: S_ISREG(Information.st_mode) ? EWoTTreeEntryType::File
		: EWoTTreeEntryType::SymbolicLink;
	if (Actual != ExpectedType)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the entry changed type before handle-relative removal"), FString(Leaf)));
	}
	if (ExpectedIdentity != nullptr)
	{
		FWoTNativeFileIdentity Identity;
		Identity.VolumeId = static_cast<uint64>(Information.st_dev);
		Identity.FileIdLow = static_cast<uint64>(Information.st_ino);
		Identity.FileIdHigh = 0;
		Identity.Size = static_cast<uint64>(Information.st_size);
		Identity.ChangeState = static_cast<uint64>(Information.st_ctimespec.tv_sec)
			* 1000000000ull + static_cast<uint64>(Information.st_ctimespec.tv_nsec);
		Identity.LinkCount = static_cast<uint32>(Information.st_nlink);
		Identity.Type = Actual;
		if (Identity != *ExpectedIdentity)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the entry identity changed before handle-relative removal"),
				FString(Leaf)));
		}
	}
	if (unlinkat(Root, LeafUtf8.Get(),
			ExpectedType == EWoTTreeEntryType::Directory ? AT_REMOVEDIR : 0) != 0)
	{
		return SystemStatus(TEXT("unlinkat"), errno, Leaf);
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
	const int Duplicate = dup(FWoTNativeHandleAccess::State(File)->Descriptor);
	if (Duplicate < 0)
	{
		return FResult::Error(SystemError(TEXT("dup"), errno, TEXT("")));
	}
	const int Operation = (Mode == EWoTNativeFileLockMode::Exclusive ? LOCK_EX : LOCK_SH)
		| (bWait ? 0 : LOCK_NB);
	if (flock(Duplicate, Operation) != 0)
	{
		const int Error = errno;
		close(Duplicate);
		return FResult::Error(SystemError(TEXT("flock"), Error, TEXT("")));
	}
	FWoTNativeFileLock Result;
	TUniquePtr<FWoTNativeFileLockState> State = MakeUnique<FWoTNativeFileLockState>();
	State->Descriptor = Duplicate;
	State->bLocked = true;
	FWoTNativeHandleAccess::LockState(Result) = MoveTemp(State);
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<FWoTNativeExecutableIdentity>
OpenCurrentExecutableIdentityNoFollow(FString* OutPathResolutionFailureReason)
{
	using FResult = TWoTBootstrapResult<FWoTNativeExecutableIdentity>;
	if (OutPathResolutionFailureReason != nullptr)
	{
		OutPathResolutionFailureReason->Reset();
	}

	// Ask the kernel which vnode backs the Mach-O mapping containing the main
	// executable header. _NSGetExecutablePath is only a name and can be swapped
	// after exec; PROC_PIDREGIONPATHINFO supplies the mapped vnode identity.
	const mach_header* MainHeader = _dyld_get_image_header(0);
	if (MainHeader == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("dyld did not expose the main running-image mapping")));
	}
	proc_regionwithpathinfo Region = {};
	const int RegionBytes = proc_pidinfo(
		getpid(), PROC_PIDREGIONPATHINFO,
		reinterpret_cast<uint64>(MainHeader),
		&Region, sizeof(Region));
	if (RegionBytes != sizeof(Region))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the kernel running-image vnode could not be determined")));
	}
	const vinfo_stat& KernelStat = Region.prp_vip.vip_vi.vi_stat;
	if (!S_ISREG(KernelStat.vst_mode)
		|| Region.prp_vip.vip_path[0] != '/')
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the kernel running-image vnode is not a regular absolute file")));
	}
	const FUTF8ToTCHAR Converted(Region.prp_vip.vip_path);
	FString Path =
		FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());

	const FWoTNativeFileIdentity KernelIdentity = {
		static_cast<uint64>(KernelStat.vst_dev),
		static_cast<uint64>(KernelStat.vst_ino),
		0,
		static_cast<uint64>(KernelStat.vst_size),
		static_cast<uint64>(KernelStat.vst_ctime) * 1000000000ull
			+ static_cast<uint64>(KernelStat.vst_ctimensec),
		static_cast<uint32>(KernelStat.vst_nlink),
		EWoTTreeEntryType::File,
	};
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> Handle =
		OpenAbsolutePathAllComponentsNoFollow<
			FWoTNativeExecutableIdentity>(Path, 0);
	if (Handle.IsError())
	{
		return Handle;
	}
	if (Handle.GetValue().GetIdentity() != KernelIdentity)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the executable name is not the kernel-backed running image"),
			Path));
	}
	FWoTNativeHandleAccess::SetReportedPath(Handle.GetValue(), MoveTemp(Path));
	return Handle;
}
