// Link-refusing native handles.
//
// Everything here exists because a PATH IS NOT AN OBJECT. Between checking a path
// and using it, the name can be pointed at something else — a symlink, a reparse
// point, a hard link, or an entirely different file dropped in by a rename. Every
// ordinary function below therefore opens objects without following a link,
// captures a path-independent identity from the open handle, and hands back a
// handle rather than a path. The owned-root canonicalizer is the explicit
// exception: it follows ancestors above the root once, then proves a no-follow
// canonical reopen names the same object. Later checks describe the object
// actually held, even after its name has been given away.
//
// The `At` variants walk one relative component at a time from an already-held
// directory, which is what keeps an attacker from swapping a mid-path directory
// after the prefix was validated.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

/** One name observed while enumerating an already-held directory. */
struct FWoTNativeDirectoryEntry
{
	FString Name;
	EWoTTreeEntryType Type = EWoTTreeEntryType::File;
};

/**
 * An existing directory resolved from the object opened at the launch path.
 *
 * CanonicalPath comes from the first held handle, never from textual path
 * normalization. Directory is a second, all-components-no-follow open of that
 * canonical path whose native identity was proven equal to the first handle.
 */
struct FWoTCanonicalNativeDirectory
{
	FString CanonicalPath;
	FWoTNativeDirectoryHandle Directory;
	/** Set by open-or-create so creation durability can flush the held parent. */
	FWoTNativeDirectoryHandle ParentDirectory;
};

enum class EWoTNativeFileLockMode : uint8
{
	Shared,
	Exclusive,
};

struct FWoTNativeFileLockState;

/**
 * A byte-range/flock lease whose ownership follows the handle, not a thread.
 *
 * This is intentionally move-only. In particular, Win64 uses LockFileEx rather
 * than a mutex, so moving or destroying a cache permit on another thread is safe.
 */
class WOTBOOTSTRAPCORE_API FWoTNativeFileLock final
{
public:
	FWoTNativeFileLock();
	~FWoTNativeFileLock();
	FWoTNativeFileLock(FWoTNativeFileLock&& Other);
	FWoTNativeFileLock& operator=(FWoTNativeFileLock&& Other);

	FWoTNativeFileLock(const FWoTNativeFileLock&) = delete;
	FWoTNativeFileLock& operator=(const FWoTNativeFileLock&) = delete;

	bool IsValid() const;

	/** Explicitly unlocks and closes; a failure is reported, never discarded. */
	FWoTBootstrapStatus Release();

private:
	friend struct FWoTNativeHandleAccess;
	TUniquePtr<FWoTNativeFileLockState> State;
};

/** Opens an absolute path's final component without following a link. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle> OpenNativeFileReadNoFollow(
	FStringView CanonicalAbsolutePath);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenNativeDirectoryNoFollow(
	FStringView CanonicalAbsolutePath);

/**
 * Opens an absolute path from its filesystem root one component at a time.
 *
 * Unlike the final-component-only entry points above, these reject a
 * symlink/reparse point anywhere between the volume root and the returned
 * object. Use these when the absolute prefix itself is security-sensitive.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle>
	OpenNativeFilePathReadNoFollow(FStringView CanonicalAbsolutePath);
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeDirectoryHandle>
	OpenNativeDirectoryPathNoFollow(FStringView CanonicalAbsolutePath);

/**
 * Follow ancestors above an owned root once, obtain their canonical spelling
 * from the opened directory handle, then re-open the canonical path no-follow.
 * The final component is never followed. A replacement between the two opens
 * is refused by native identity.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
	OpenCanonicalNativeDirectoryNoFollow(FStringView LaunchAbsolutePath);

/**
 * Canonicalizes a directory wholly above an owned root. The launch path may
 * itself end in a link/junction; the canonical reopen does not follow links
 * and must identify the same directory. Never use this for the owned root.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
	OpenCanonicalNativeDirectoryFollowing(FStringView AncestorAbsolutePath);

/**
 * Splits an absolute native directory path without losing its filesystem root.
 * In particular, `D:/WoTState` becomes parent `D:/`, never drive-relative `D:`.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus SplitNativePathParentAndLeaf(
	FStringView AbsolutePath, FString& OutParent, FString& OutLeaf);

/**
 * Opens, or creates exactly one owned-root leaf beneath a canonical held parent.
 * Ancestors above the owned root may canonicalize through links; the owned leaf
 * itself is opened/created handle-relatively and is never followed.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
	OpenOrCreateCanonicalNativeDirectoryNoFollow(
		FStringView OwnedRootAbsolutePath, bool& bOutCreated);

#if WITH_AUTOMATION_TESTS
/** Test seam for replacing the name after the first handle/path capture. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
	OpenCanonicalNativeDirectoryNoFollowForTesting(
		FStringView LaunchAbsolutePath, TFunction<void()> BeforeCanonicalReopen);
#endif

/**
 * Opens a relative path beneath a held directory, one component at a time.
 *
 * The relative path must be tree-relative and non-escaping: no absolute prefix, no
 * `..`, no empty or `.` component, and no drive or UNC prefix. A signed manifest
 * naming such a path is refused rather than sanitized — sanitizing would mean
 * deciding what the signer meant.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle> OpenNativeFileAtReadNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView CanonicalRelativePath);

/**
 * Looks up ONE leaf beneath Directory without listing the directory and without
 * following a link: empty when the leaf does not exist, otherwise its type (a
 * link of any kind reports SymbolicLink). Leaf goes through RequireSingleLeaf;
 * any other failure is Io.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<TOptional<EWoTTreeEntryType>>
	QueryNativeEntryTypeAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);

#if PLATFORM_WINDOWS
/** Installer-only classification. Generic owned-tree identity still treats
 * every reparse point as SymbolicLink. */
enum class EWoTInstallerSourceReparseTagClass : uint8
{
	DataContent,
	NameSurrogate,
	Unknown,
};

struct FWoTInstallerSourceEntryInfo
{
	EWoTTreeEntryType Type = EWoTTreeEntryType::File;
	uint32 ReparseTag = 0;
	bool bRefusedReparsePoint = false;
};

/** Looks up one installer leaf. WOF/dedup/cloud data tags remain files;
 * name-surrogate and unknown tags are refused and retain their numeric tag. */
WOTBOOTSTRAPCORE_API
TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>>
	QueryNativeInstallerSourceEntryAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);

/** Opens the terminal installer data object without following any ancestor.
 * Only allowlisted data-content reparse tags may pass the terminal boundary. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle>
	OpenNativeInstallerSourceFileAtReadNoFollow(
		const FWoTNativeDirectoryHandle& Directory,
		FStringView CanonicalRelativePath);

WOTBOOTSTRAPCORE_API EWoTInstallerSourceReparseTagClass
	WoTClassifyInstallerSourceReparseTag(uint32 ReparseTag);

/** True only when none of Windows' recall/offline attributes can make a read
 * synchronously hydrate the installer through a Cloud Files provider. */
WOTBOOTSTRAPCORE_API bool WoTInstallerSourceAttributesAreFullyLocal(
	uint32 FileAttributes);

/** Reads FILE_BASIC_INFO from the held no-follow installer handle. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint32>
	QueryNativeInstallerSourceFileAttributes(
		const FWoTNativeFileHandle& File);

/** WOF's compact.exe /EXE mode is supported only where the filesystem reports
 * both NTFS semantics and file compression support. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool>
	QueryNativeVolumeSupportsWof(
		const FWoTNativeDirectoryHandle& Directory);

#if WITH_AUTOMATION_TESTS
using FWoTNativeInstallerSourceAttributeQueryForTesting =
	TFunction<TWoTBootstrapResult<uint32>(const FWoTNativeFileHandle&)>;
using FWoTNativeInstallerSourceReadObserverForTesting = TFunction<void()>;

/** Replaces only the held-handle attribute query; empty restores native I/O. */
WOTBOOTSTRAPCORE_API void
	WoTSetNativeInstallerSourceAttributeQueryForTesting(
		FWoTNativeInstallerSourceAttributeQueryForTesting InQuery);

/** Observes each native ReadFile call made through an installer-source handle. */
WOTBOOTSTRAPCORE_API void WoTSetNativeInstallerSourceReadObserverForTesting(
	FWoTNativeInstallerSourceReadObserverForTesting InObserver);
#endif
#endif

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenNativeDirectoryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView CanonicalRelativePath);

/**
 * Handle-relative mutation primitives.
 *
 * Every name argument is exactly one safe component. These functions never
 * reconstruct an absolute pathname and never follow a link/reparse point.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle>
	CreateNativeFileAtWriteNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);

/**
 * Proves whether a held directory can receive a new regular file without ever
 * deleting an entry by name. Windows creates the unique leaf delete-on-close;
 * macOS asks the held descriptor for effective-user write access and creates
 * no entry at all. Permission/read-only failures are a successful `false`.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool>
	ProbeNativeDirectoryWritableNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf);

#if PLATFORM_WINDOWS
/**
 * Non-mutating access check for the actual activation operations on a held
 * child tree. The held install root must accept a renamed directory; the live
 * tree must be renameable; and every descendant must be recursively deletable.
 * Descendants are opened relative to held parents with DELETE unless the held
 * parent grants FILE_DELETE_CHILD. Reparse entries are checked but not entered.
 * No entry is created, renamed, or removed.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool>
	CheckNativeDirectoryMutationAccessAtNoFollow(
		const FWoTNativeDirectoryHandle& Parent, FStringView Leaf,
		TFunctionRef<FWoTBootstrapStatus()> BetweenEntries =
			[]() { return FWoTBootstrapStatus::Success(); });
#endif

#if WITH_AUTOMATION_TESTS
/** Runs while the Windows delete-on-close probe handle is still held. The Mac
 * implementation performs no create and therefore never invokes the hook. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool>
	ProbeNativeDirectoryWritableNoFollowForTesting(
		const FWoTNativeDirectoryHandle& Directory, FStringView UniqueLeaf,
		TFunction<void(FStringView)> AfterCreate);

#if PLATFORM_WINDOWS
/** One-shot native create failure injected at the next writable probe. */
WOTBOOTSTRAPCORE_API void WoTSetNativeDirectoryWritableProbeErrorForTesting(
	uint32 NativeCode);

/** Installs a hook called only by the mutation walker and only between entries. */
WOTBOOTSTRAPCORE_API void WoTSetNativeDirectoryMutationWalkHookForTesting(
	TFunction<FWoTBootstrapStatus()> Hook);

/** Called on the renaming thread each time RenameNativeEntryAtNoFollow is about
 * to wait and retry a directory rename refused because something below the
 * directory is open. FailedAttempts is 1 on the first refusal. Empty removes it. */
WOTBOOTSTRAPCORE_API void WoTSetNativeDirectoryRenameRetryObserverForTesting(
	TFunction<void(int32 FailedAttempts)> Observer);

/** Replaces the total wait budget of that retry; negative restores the default. */
WOTBOOTSTRAPCORE_API void WoTSetNativeDirectoryRenameRetryBudgetForTesting(
	double Seconds);
#endif
#endif

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle>
	OpenOrCreateNativeFileAtReadWriteNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf, bool& bOutCreated);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeDirectoryHandle>
	CreateNativeDirectoryAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeDirectoryHandle>
	OpenOrCreateNativeDirectoryAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf, bool& bOutCreated);

WOTBOOTSTRAPCORE_API FWoTBootstrapStatus WriteNativeFile(
	FWoTNativeFileHandle& File, TConstArrayView<uint8> Bytes);

/** Resize a held regular file without resolving its path again. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus TruncateNativeFile(
	FWoTNativeFileHandle& File, uint64 Size);

/** Write at an exact byte offset without truncating or changing the caller's
 * notion of a stream cursor. Intended for bounded streaming sinks. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus WriteNativeFileAt(
	FWoTNativeFileHandle& File, uint64 Offset, TConstArrayView<uint8> Bytes);

/** Read up to Buffer.Num() bytes at an exact offset. Zero means EOF. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint64> ReadNativeFileAt(
	const FWoTNativeFileHandle& File, uint64 Offset, TArrayView<uint8> Buffer);

WOTBOOTSTRAPCORE_API FWoTBootstrapStatus FlushNativeFile(
	const FWoTNativeFileHandle& File);

WOTBOOTSTRAPCORE_API FWoTBootstrapStatus FlushNativeDirectory(
	const FWoTNativeDirectoryHandle& Directory);

/** POSIX permission projection of the held object (never queried by path). */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint32> QueryNativeMode(
	const FWoTNativeFileHandle& File);
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint32> QueryNativeMode(
	const FWoTNativeDirectoryHandle& Directory);

#if PLATFORM_MAC
/** True only when the held object itself is owned by the current uid. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool> IsNativeOwnedByCurrentUser(
	const FWoTNativeFileHandle& File);
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool> IsNativeOwnedByCurrentUser(
	const FWoTNativeDirectoryHandle& Directory);
#endif

/** Apply permission bits to the held object itself, never to a later path lookup. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus SetNativeMode(
	const FWoTNativeFileHandle& File, uint32 Mode);
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus SetNativeMode(
	const FWoTNativeDirectoryHandle& Directory, uint32 Mode);

/**
 * Renames one held-parent-relative entry without following links. On Windows a
 * directory rename that is refused because another process holds something
 * below the directory open is retried with backoff for about ten seconds,
 * synchronously, before it fails; a failure's message carries the raw NTSTATUS.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus RenameNativeEntryAtNoFollow(
	const FWoTNativeDirectoryHandle& FromDirectory, FStringView FromLeaf,
	const FWoTNativeDirectoryHandle& ToDirectory, FStringView ToLeaf,
	bool bReplaceExisting);

/** Atomically exchange two held-parent-relative entries without following links. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<bool>
	ExchangeNativeEntriesAtNoFollow(
		const FWoTNativeDirectoryHandle& DirectoryA, FStringView LeafA,
		const FWoTNativeDirectoryHandle& DirectoryB, FStringView LeafB);

/** Create/read a link object relative to a held parent without following it. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus CreateNativeSymbolicLinkAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	FStringView Target);
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FString>
	ReadNativeSymbolicLinkAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint32>
	QueryNativeSymbolicLinkModeAtNoFollow(
		const FWoTNativeDirectoryHandle& Directory, FStringView Leaf);

/**
 * Publishes the already-held, already-verified source object without replacement.
 *
 * VerifiedIdentity is mutation-sensitive: the held object must remain a
 * single-link regular file with that full identity, and FromLeaf must still name
 * it immediately before publication. The returned handle is a fresh no-follow
 * open of ToLeaf, proven to be the same held object and a single-link regular
 * file. The rename itself may advance platform change time, so callers that bind
 * content to a mutation-sensitive snapshot re-hash the returned handle. A
 * destination collision is an error and neither object is replaced.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileHandle>
	PublishHeldNativeFileAtNoReplace(
		const FWoTNativeFileHandle& HeldSource,
		const FWoTNativeFileIdentity& VerifiedIdentity,
		const FWoTNativeDirectoryHandle& FromDirectory, FStringView FromLeaf,
		const FWoTNativeDirectoryHandle& ToDirectory, FStringView ToLeaf);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>>
	EnumerateNativeDirectory(const FWoTNativeDirectoryHandle& Directory);

#if WITH_AUTOMATION_TESTS && PLATFORM_MAC
/** Removes the named entry after readdir reports it and before enumeration
 * queries its type. One-shot seam for the vanished-entry regression spec. */
WOTBOOTSTRAPCORE_API void
	WoTSetNativeDirectoryEnumerationRemovalLeafForTesting(FStringView Leaf);
#endif

/** Query the normalized absolute name of the held directory itself. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FString> QueryNativeDirectoryPath(
	const FWoTNativeDirectoryHandle& Directory);

/** Query the normalized absolute name of the held file itself. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FString> QueryNativeFilePath(
	const FWoTNativeFileHandle& File);

/** Queries free bytes on the volume containing the held directory. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<uint64> QueryNativeDirectoryAvailableBytes(
	const FWoTNativeDirectoryHandle& Directory);

WOTBOOTSTRAPCORE_API FWoTBootstrapStatus RemoveNativeEntryAtNoFollow(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	EWoTTreeEntryType ExpectedType);

/** Removes only if the name still denotes the exact object the caller held. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus RemoveNativeEntryAtNoFollowIfIdentity(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	EWoTTreeEntryType ExpectedType,
	const FWoTNativeFileIdentity& ExpectedIdentity);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileLock> AcquireNativeFileLock(
	const FWoTNativeFileHandle& File, EWoTNativeFileLockMode Mode, bool bWait);

/** Holds the running image open, so later checks describe the executing code. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeExecutableIdentity>
	OpenCurrentExecutableIdentityNoFollow(
		FString* OutPathResolutionFailureReason = nullptr);

#if WITH_AUTOMATION_TESTS && PLATFORM_WINDOWS
/** Forces the final held-image name query itself to fail in a Win64 spec. */
WOTBOOTSTRAPCORE_API void
	WoTSetCurrentExecutableFinalPathQueryFailureForTesting(bool bFail);

/** Fails QueryNativeDirectoryPath only for the named opened directory. */
WOTBOOTSTRAPCORE_API void
	WoTSetDirectoryFinalPathQueryFailureForTesting(FStringView AbsolutePath);

/** Opens a fixture image with the same held/reporting contract as the current
 * process helper, without claiming that the fixture is executing. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeExecutableIdentity>
	OpenNativeExecutableIdentityNoFollowForTesting(FStringView AbsolutePath);
#endif

/**
 * Rejects a relative path that could leave the tree.
 *
 * Exposed so callers and tests share exactly one definition of "safe relative
 * path" rather than each inventing a slightly different one.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus RequireSafeRelativePath(FStringView RelativePath);

/**
 * Re-proves a held file whose identity moved ONLY in ChangeState since Recorded
 * (FWoTNativeFileIdentity::DiffersOnlyInChangeState): the OS writing metadata
 * (macOS provenance/macl xattrs, NTFS Zone.Identifier or SmartScreen streams,
 * ACL/attribute edits) and a same-length rewrite look identical there.
 *
 * Re-reads every byte through the held handle, requires the identity not to
 * move during that read, and accepts only when the size, SHA-256 AND mode still
 * equal the verified ones (a chmod is ChangeState-only too). Returns the identity
 * to use as the new baseline. A wider identity change, an identity that moves
 * during the re-read, or different bytes or mode is CorruptInstallation, exactly
 * as a strict mismatch was before.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTNativeFileIdentity>
	WoTReverifyHeldFileAfterMetadataChurn(const FWoTNativeFileHandle& File,
		const FWoTNativeFileIdentity& Recorded, uint64 VerifiedSize,
		const FWoTSha256& VerifiedSha256, uint32 VerifiedMode,
		FStringView RelativePath);
