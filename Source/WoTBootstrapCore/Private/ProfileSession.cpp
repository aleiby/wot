#include "WoTBootstrapCore/ProfileSession.h"

#include "Misc/Paths.h"

#if PLATFORM_MAC
#include <cerrno>
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr TCHAR GWoTProfileSessionLockLeaf[] = TEXT("session.lock");
constexpr TCHAR GWoTProfileSessionContentionMessage[] =
	TEXT("Close other copies of WoT Reborn and try again.");

TUniquePtr<FWoTProfileSession> GWoTProcessProfileSession;

FWoTBootstrapError WoTProfileSessionError(
	EWoTBootstrapError Code, FString Message, FString Path = FString(),
	uint32 NativeCode = 0)
{
	FWoTBootstrapError Error =
		MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
	Error.NativeCode = NativeCode;
	return Error;
}

bool WoTProfileSessionIsContention(const FWoTBootstrapError& Error)
{
#if PLATFORM_WINDOWS
	return Error.NativeCode == ERROR_LOCK_VIOLATION
		|| Error.NativeCode == ERROR_SHARING_VIOLATION;
#elif PLATFORM_MAC
	return Error.NativeCode == EWOULDBLOCK || Error.NativeCode == EAGAIN;
#else
	(void)Error;
	return false;
#endif
}

template <typename HandleType>
FWoTBootstrapStatus WoTRequireProfileOwnership(
	const HandleType& Handle, FStringView Label)
{
#if PLATFORM_MAC
	TWoTBootstrapResult<bool> Owned = IsNativeOwnedByCurrentUser(Handle);
	if (Owned.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Owned).GetError());
	}
	if (!Owned.GetValue())
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %.*s is not owned by the current user"),
				Label.Len(), Label.GetData())));
	}
#else
	(void)Handle;
	(void)Label;
#endif
	return FWoTBootstrapStatus::Success();
}

template <typename HandleType>
FWoTBootstrapStatus WoTSecureProfileObject(
	const HandleType& Handle, uint32 Mode, FStringView Label)
{
	if (FWoTBootstrapStatus Owned = WoTRequireProfileOwnership(Handle, Label);
		Owned.IsError())
	{
		return Owned;
	}
#if PLATFORM_MAC
	return SetNativeMode(Handle, Mode);
#else
	(void)Mode;
	return FWoTBootstrapStatus::Success();
#endif
}

FWoTBootstrapStatus WoTRemoveProfileTreeEntry(
	const FWoTNativeDirectoryHandle& Parent,
	const FWoTNativeDirectoryEntry& Entry)
{
	if (Entry.Type != EWoTTreeEntryType::Directory)
	{
		return RemoveNativeEntryAtNoFollow(Parent, Entry.Name, Entry.Type);
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
		OpenNativeDirectoryAtNoFollow(Parent, Entry.Name);
	if (Child.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Child).GetError());
	}
	if (FWoTBootstrapStatus Secured = WoTSecureProfileObject(
			Child.GetValue(), 0700u, TEXTVIEW("profile directory"));
		Secured.IsError())
	{
		return Secured;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(Child.GetValue());
	if (Entries.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	}
	for (const FWoTNativeDirectoryEntry& Nested : Entries.GetValue())
	{
		if (FWoTBootstrapStatus Removed = WoTRemoveProfileTreeEntry(
				Child.GetValue(), Nested); Removed.IsError())
		{
			return Removed;
		}
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
		Child.GetValue().QueryCurrentIdentity();
	if (Current.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Current).GetError());
	}
	const FWoTNativeFileIdentity Identity = Current.GetValue();
	Child.GetValue() = FWoTNativeDirectoryHandle();
	return RemoveNativeEntryAtNoFollowIfIdentity(
		Parent, Entry.Name, EWoTTreeEntryType::Directory, Identity);
}
} // namespace

const TCHAR* WoTProfileSessionLockLeaf()
{
	return GWoTProfileSessionLockLeaf;
}

const TCHAR* WoTProfileSessionContentionMessage()
{
	return GWoTProfileSessionContentionMessage;
}

FWoTProfileSession::FWoTProfileSession() = default;
FWoTProfileSession::~FWoTProfileSession() = default;
FWoTProfileSession::FWoTProfileSession(FWoTProfileSession&& Other) = default;
FWoTProfileSession& FWoTProfileSession::operator=(
	FWoTProfileSession&& Other) = default;

bool FWoTProfileSession::IsShared() const
{
	return Mode == EMode::Shared && Lock.IsValid();
}

bool FWoTProfileSession::IsExclusive() const
{
	return Mode == EMode::Exclusive && Lock.IsValid();
}

FWoTBootstrapStatus FWoTProfileSession::RevalidatePath() const
{
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> ReopenedRoot =
		OpenNativeDirectoryNoFollow(RootPath);
	if (ReopenedRoot.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(ReopenedRoot).GetError());
	}
	if (!ReopenedRoot.GetValue().GetIdentity().IsSameObject(RootIdentity))
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the profile root changed while its session lock was held"),
			RootPath));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> ReopenedFile =
		OpenNativeFileAtReadNoFollow(
			ReopenedRoot.GetValue(), GWoTProfileSessionLockLeaf);
	if (ReopenedFile.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(ReopenedFile).GetError());
	}
	const FWoTNativeFileIdentity& ReopenedIdentity =
		ReopenedFile.GetValue().GetIdentity();
	if (ReopenedIdentity.LinkCount != 1
		|| !ReopenedIdentity.IsSameObject(FileIdentity))
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the profile session lock name changed while its lock was held"),
			FPaths::Combine(RootPath, GWoTProfileSessionLockLeaf)));
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTProfileSession::AcquireLock(
	EWoTNativeFileLockMode NativeMode, EMode NewMode)
{
	TWoTBootstrapResult<FWoTNativeFileLock> Acquired =
		AcquireNativeFileLock(File, NativeMode, false);
	if (Acquired.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Acquired).GetError());
	}
	Lock = MoveTemp(Acquired).GetValue();
	Mode = NewMode;
	FWoTBootstrapStatus Revalidated = RevalidatePath();
	if (Revalidated.IsError())
	{
		(void)Lock.Release();
		Mode = EMode::None;
		return Revalidated;
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<bool> FWoTProfileSession::TryAcquireExclusive()
{
	using FResult = TWoTBootstrapResult<bool>;
	if (!IsShared())
	{
		return FResult::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the profile session is not held shared"), RootPath));
	}
	if (FWoTBootstrapStatus Revalidated = RevalidatePath();
		Revalidated.IsError())
	{
		return FResult::Error(MoveTemp(Revalidated).GetError());
	}
	if (FWoTBootstrapStatus Released = Lock.Release(); Released.IsError())
	{
		Mode = EMode::None;
		return FResult::Error(MoveTemp(Released).GetError());
	}
	Mode = EMode::None;

	FWoTBootstrapStatus Exclusive = AcquireLock(
		EWoTNativeFileLockMode::Exclusive, EMode::Exclusive);
	if (!Exclusive.IsError())
	{
		return FResult::Success(true);
	}
	FWoTBootstrapError ExclusiveError = MoveTemp(Exclusive).GetError();
	// Another upgrader may have won after both processes dropped SHARED. Restore
	// nonblocking too; callers treat a lost lifetime hold as a fatal refusal.
	FWoTBootstrapStatus Restored = AcquireLock(
		EWoTNativeFileLockMode::Shared, EMode::Shared);
	if (Restored.IsError())
	{
		return FResult::Error(WoTProfileSessionError(
			EWoTBootstrapError::Io,
			TEXT("the shared profile session could not be restored: ")
				+ Restored.GetError().ToString(), RootPath));
	}
	return WoTProfileSessionIsContention(ExclusiveError)
		? FResult::Success(false)
		: FResult::Error(MoveTemp(ExclusiveError));
}

FWoTBootstrapStatus FWoTProfileSession::RestoreShared()
{
	if (IsShared())
	{
		return FWoTBootstrapStatus::Success();
	}
	if (!IsExclusive())
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the profile session is not held exclusive"), RootPath));
	}
	if (FWoTBootstrapStatus Released = Lock.Release(); Released.IsError())
	{
		Mode = EMode::None;
		return Released;
	}
	Mode = EMode::None;
	return AcquireLock(EWoTNativeFileLockMode::Shared, EMode::Shared);
}

FWoTBootstrapStatus
FWoTProfileSession::DeleteProfileContentsExceptSessionLock()
{
	if (!IsExclusive())
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("profile deletion requires the exclusive session owner"),
			RootPath));
	}
	if (FWoTBootstrapStatus Revalidated = RevalidatePath();
		Revalidated.IsError())
	{
		return Revalidated;
	}
	if (FWoTBootstrapStatus Secured = WoTSecureProfileObject(
			RootDirectory, 0700u, TEXTVIEW("profile root")); Secured.IsError())
	{
		return Secured;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(RootDirectory);
	if (Entries.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	}
	for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
	{
		if (Entry.Name == GWoTProfileSessionLockLeaf)
		{
			if (Entry.Type != EWoTTreeEntryType::File)
			{
				return FWoTBootstrapStatus::Error(WoTProfileSessionError(
					EWoTBootstrapError::UnsafePath,
					TEXT("the held session.lock name is no longer a file"),
					RootPath));
			}
			continue;
		}
		if (FWoTBootstrapStatus Removed = WoTRemoveProfileTreeEntry(
				RootDirectory, Entry); Removed.IsError())
		{
			return Removed;
		}
	}
	return RevalidatePath();
}

FWoTBootstrapStatus FWoTProfileSession::DeleteSessionLockAndEmptyProfileRoot()
{
	if (!IsExclusive())
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("final profile deletion requires the exclusive session owner"),
			RootPath));
	}
	if (FWoTBootstrapStatus Revalidated = RevalidatePath();
		Revalidated.IsError())
	{
		return Revalidated;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(RootDirectory);
	if (Entries.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	}
	if (Entries.GetValue().Num() != 1
		|| Entries.GetValue()[0].Name != GWoTProfileSessionLockLeaf
		|| Entries.GetValue()[0].Type != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the profile contains entries other than the held session lock"),
			RootPath));
	}

	FString ParentPath;
	FString RootLeaf;
	if (FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
			RootPath, ParentPath, RootLeaf); Split.IsError())
	{
		return Split;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Parent =
		OpenNativeDirectoryNoFollow(ParentPath);
	if (Parent.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Parent).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> CurrentFile =
		File.QueryCurrentIdentity();
	if (CurrentFile.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(CurrentFile).GetError());
	}
	if (CurrentFile.GetValue().LinkCount != 1
		|| !CurrentFile.GetValue().IsSameObject(FileIdentity))
	{
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the final session lock changed identity"), RootPath));
	}

	// Accepted residual window: unlink the held lock last, then remove the empty
	// root and exit immediately. No later profile access is permitted.
	if (FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollowIfIdentity(
			RootDirectory, GWoTProfileSessionLockLeaf,
			EWoTTreeEntryType::File, CurrentFile.GetValue()); Removed.IsError())
	{
		return Removed;
	}
	Lock = FWoTNativeFileLock();
	File = FWoTNativeFileHandle();
	Mode = EMode::None;

	TWoTBootstrapResult<FWoTNativeFileIdentity> CurrentRoot =
		RootDirectory.QueryCurrentIdentity();
	if (CurrentRoot.IsError()
		|| !CurrentRoot.GetValue().IsSameObject(RootIdentity))
	{
		return CurrentRoot.IsError()
			? FWoTBootstrapStatus::Error(MoveTemp(CurrentRoot).GetError())
			: FWoTBootstrapStatus::Error(WoTProfileSessionError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the profile root changed before final removal"), RootPath));
	}
	const FWoTNativeFileIdentity RootForRemoval = CurrentRoot.GetValue();
	RootDirectory = FWoTNativeDirectoryHandle();
	return RemoveNativeEntryAtNoFollowIfIdentity(
		Parent.GetValue(), RootLeaf, EWoTTreeEntryType::Directory,
		RootForRemoval);
}

TWoTBootstrapResult<FWoTProfileSession> AcquireWoTProfileSessionShared(
	FStringView DataRoot)
{
	using FResult = TWoTBootstrapResult<FWoTProfileSession>;
	FString RootPath(DataRoot);
	FPaths::NormalizeDirectoryName(RootPath);
	if (RootPath.IsEmpty() || FPaths::IsRelative(RootPath)
		|| !FPaths::CollapseRelativeDirectories(RootPath))
	{
		return FResult::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the profile session requires an absolute safe root"),
			RootPath));
	}
	bool bRootCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Root =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(RootPath, bRootCreated);
	(void)bRootCreated;
	if (Root.IsError())
	{
		return FResult::Error(MoveTemp(Root).GetError());
	}
	RootPath = Root.GetValue().CanonicalPath;
	FPaths::NormalizeDirectoryName(RootPath);
	if (FWoTBootstrapStatus Secured = WoTSecureProfileObject(
			Root.GetValue().Directory, 0700u, TEXTVIEW("profile root"));
		Secured.IsError())
	{
		return FResult::Error(MoveTemp(Secured).GetError());
	}
	bool bFileCreated = false;
	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenOrCreateNativeFileAtReadWriteNoFollow(
			Root.GetValue().Directory, GWoTProfileSessionLockLeaf, bFileCreated);
	(void)bFileCreated;
	if (File.IsError())
	{
		return FResult::Error(MoveTemp(File).GetError());
	}
	if (FWoTBootstrapStatus Secured = WoTSecureProfileObject(
			File.GetValue(), 0600u, TEXTVIEW("profile session lock"));
		Secured.IsError())
	{
		return FResult::Error(MoveTemp(Secured).GetError());
	}

	FWoTProfileSession Session;
	Session.RootPath = MoveTemp(RootPath);
	Session.RootIdentity = Root.GetValue().Directory.GetIdentity();
	Session.RootDirectory = MoveTemp(Root.GetValue().Directory);
	TWoTBootstrapResult<FWoTNativeFileIdentity> CurrentFile =
		File.GetValue().QueryCurrentIdentity();
	if (CurrentFile.IsError())
	{
		return FResult::Error(MoveTemp(CurrentFile).GetError());
	}
	Session.FileIdentity = CurrentFile.GetValue();
	Session.File = MoveTemp(File).GetValue();
	FWoTBootstrapStatus Acquired = Session.AcquireLock(
		EWoTNativeFileLockMode::Shared, FWoTProfileSession::EMode::Shared);
	if (Acquired.IsError())
	{
		if (WoTProfileSessionIsContention(Acquired.GetError()))
		{
			return FResult::Error(WoTProfileSessionError(
				EWoTBootstrapError::ServiceUnavailable,
				GWoTProfileSessionContentionMessage,
				FPaths::Combine(Session.RootPath,
					GWoTProfileSessionLockLeaf)));
		}
		return FResult::Error(MoveTemp(Acquired).GetError());
	}
	// Root/session.lock creation is the lock bootstrap itself. Every other
	// profile mutation, including the writeability probe, happens under SHARED.
	const FString ProbeLeaf = TEXT(".wot-profile-write-probe-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	TWoTBootstrapResult<bool> Writable = ProbeNativeDirectoryWritableNoFollow(
		Session.RootDirectory, ProbeLeaf);
	if (Writable.IsError())
	{
		return FResult::Error(MoveTemp(Writable).GetError());
	}
	if (!Writable.GetValue())
	{
		uint32 PermissionCode = 0;
#if PLATFORM_WINDOWS
		PermissionCode = ERROR_ACCESS_DENIED;
#elif PLATFORM_MAC
		PermissionCode = static_cast<uint32>(EACCES);
#endif
		return FResult::Error(WoTProfileSessionError(
			EWoTBootstrapError::Io, TEXT("the profile root is not writable"),
			Session.RootPath, PermissionCode));
	}
	return FResult::Success(MoveTemp(Session));
}

FWoTBootstrapStatus WoTAcquireProcessProfileSessionShared(FStringView DataRoot)
{
	FString Normalized(DataRoot);
	FPaths::NormalizeDirectoryName(Normalized);
	if (GWoTProcessProfileSession.IsValid())
	{
		if (GWoTProcessProfileSession->IsShared()
			&& GWoTProcessProfileSession->DataRoot() == Normalized)
		{
			return FWoTBootstrapStatus::Success();
		}
		return FWoTBootstrapStatus::Error(WoTProfileSessionError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the process profile session is already owned by another root"),
			Normalized));
	}
	TWoTBootstrapResult<FWoTProfileSession> Acquired =
		AcquireWoTProfileSessionShared(Normalized);
	if (Acquired.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Acquired).GetError());
	}
	GWoTProcessProfileSession = MakeUnique<FWoTProfileSession>(
		MoveTemp(Acquired).GetValue());
	return FWoTBootstrapStatus::Success();
}

TUniquePtr<FWoTProfileSession> WoTTakeProcessProfileSession()
{
	return MoveTemp(GWoTProcessProfileSession);
}

bool WoTRestoreProcessProfileSession(
	TUniquePtr<FWoTProfileSession> Session)
{
	if (GWoTProcessProfileSession.IsValid() || !Session.IsValid()
		|| !Session->IsShared())
	{
		return false;
	}
	GWoTProcessProfileSession = MoveTemp(Session);
	return true;
}

const FWoTProfileSession* WoTGetProcessProfileSession()
{
	return GWoTProcessProfileSession.Get();
}

FString WoTProfileSessionFailureMessage(
	FStringView DataRoot, const FWoTBootstrapError& Error)
{
	if (Error.Code == EWoTBootstrapError::ServiceUnavailable)
	{
		return GWoTProfileSessionContentionMessage;
	}
#if PLATFORM_WINDOWS
	if (Error.NativeCode == ERROR_ACCESS_DENIED
		|| Error.NativeCode == ERROR_WRITE_PROTECT
		|| Error.NativeCode == ERROR_NETWORK_ACCESS_DENIED)
	{
		return FString::Printf(
			TEXT("WoT Reborn can't save its data in %.*s. Move the WoT Reborn folder to a place you can write to, such as your user folder, and start it again."),
			DataRoot.Len(), DataRoot.GetData());
	}
	if (Error.NativeCode == ERROR_DISK_FULL
		|| Error.NativeCode == ERROR_HANDLE_DISK_FULL)
	{
		return FString::Printf(
			TEXT("WoT Reborn can't save its data in %.*s because the drive is full. Free up some space and start it again."),
			DataRoot.Len(), DataRoot.GetData());
	}
#else
	(void)DataRoot;
#endif
	return FString::Printf(
		TEXT("WoT Reborn could not protect its data session. %s"),
		*Error.ToString());
}

#if WITH_AUTOMATION_TESTS
void WoTResetProcessProfileSessionForTesting()
{
	GWoTProcessProfileSession.Reset();
}
#endif
