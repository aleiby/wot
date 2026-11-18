// Fixtures for the native-handle specs.
//
// The factory creates only the SHAPES the specs need — ordinary files and
// directories, symlinks, hard links, a rename that replaces a name, and an object
// deleted while still held. It never constructs platform handle state: the specs
// open every fixture through the production entry points, so what is tested is the
// real open path and not a parallel one that happens to agree with it.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

#if WITH_AUTOMATION_TESTS

// Several names below are also windows.h A/W-dispatch MACROS (CreateDirectory ->
// CreateDirectoryW, and so on). Any translation unit that has seen windows.h --
// directly or transitively, which changes as headers move -- would otherwise
// rewrite these DECLARATIONS and every CALL to the ...W spelling, while a
// translation unit that has not seen it keeps the plain spelling. The two then
// disagree at LINK time and never at compile time, so it presents as
// "unresolved external symbol ...W" in whichever direction the include graph
// happens to fall. Undefining them HERE, in the shared header, makes every
// consumer agree regardless of what else it includes; #undef of an
// undefined macro is legal, so this is a no-op off Windows.
#undef CreateDirectory
#undef CreateSymbolicLink
#undef CreateHardLink
#undef CreateFile
#undef DeleteFile
#undef MoveFileEx

// Exported, because the definitions live in this module while WoTRetailReader's
// specs reach the declarations through PrivateIncludePaths. A Program target
// links every module into one image and resolves them either way; a modular
// editor build puts each module in its own DLL, where an unexported definition
// is invisible and the link fails on all ten names at once.
namespace WoTNativeHandleTestFactory
{
/** A private temporary directory. Returns an absolute path, or empty on failure. */
WOTBOOTSTRAPCORE_API FString CreateTemporaryRoot();

WOTBOOTSTRAPCORE_API void RemoveTree(const FString& AbsolutePath);

WOTBOOTSTRAPCORE_API bool WriteFile(const FString& AbsolutePath, TConstArrayView<uint8> Bytes);

/** Creates a sparse regular file with the requested logical size. */
WOTBOOTSTRAPCORE_API bool CreateSparseFile(const FString& AbsolutePath, uint64 Size);

WOTBOOTSTRAPCORE_API bool CreateDirectory(const FString& AbsolutePath);

/**
 * Creates a symbolic link.
 *
 * Returns false when the platform refuses — on Windows an unprivileged process
 * cannot create one without developer mode, so specs must skip rather than fail.
 */
WOTBOOTSTRAPCORE_API bool CreateSymbolicLink(const FString& LinkAbsolutePath, const FString& Target);

/** Mac directory symlink / Win64 directory junction used only as an ancestor. */
WOTBOOTSTRAPCORE_API bool CreateDirectoryAncestorAlias(
	const FString& LinkAbsolutePath, const FString& Target);

/** Creates a second name for an existing file. */
WOTBOOTSTRAPCORE_API bool CreateHardLink(const FString& LinkAbsolutePath, const FString& ExistingAbsolutePath);

/** Atomically replaces `AbsolutePath` with a different file. */
WOTBOOTSTRAPCORE_API bool ReplaceByRename(const FString& AbsolutePath, const FString& ReplacementAbsolutePath);

/** Gives a held directory's old name to ReplacementAbsolutePath. */
WOTBOOTSTRAPCORE_API bool ReplaceDirectoryByRename(const FString& AbsolutePath,
	const FString& ReplacementAbsolutePath);

/** Unlinks a name while any open handle keeps the bytes readable. */
WOTBOOTSTRAPCORE_API bool RemoveName(const FString& AbsolutePath);

/**
 * Writes metadata the way the OS does, leaving content, entries, size and link
 * count alone so only ChangeState moves: a named alternate data stream on Win64
 * (as Zone.Identifier/SmartScreen are), an extended attribute on Mac (as the
 * launch-time provenance/macl xattrs are). Works on files and directories.
 * Returns false if the OS refuses the write.
 */
WOTBOOTSTRAPCORE_API bool ChangeMetadataOnly(const FString& AbsolutePath);

/**
 * Returns once the clock the file system stamps ChangeState from has passed
 * ChangeState, so the next mutation is guaranteed a different one. NTFS stamps
 * from the coarse system time (one ~15.6 ms tick), so a write in the same tick
 * as the capture leaves ChangeState equal; APFS ctime is in nanoseconds and
 * returns at once. Capped near 200 ms; false if the cap was hit.
 */
WOTBOOTSTRAPCORE_API bool WaitPastChangeStateTick(uint64 ChangeState);
}  // namespace WoTNativeHandleTestFactory

#endif  // WITH_AUTOMATION_TESTS
