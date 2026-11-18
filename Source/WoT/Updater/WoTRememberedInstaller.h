// The one installer a player chose through Browse that built the content cache,
// remembered in the player's data folder so a later rebuild need not ask again.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Error.h"

/** The record's leaf under `<UserStateRoot>/UpdaterState`. */
inline constexpr TCHAR WoTRememberedInstallerLeaf[] =
	TEXT("remembered-installer.json");

/** True only for a path already in the canonical form an acquired installer
 * reports: absolute, '/'-separated, no empty, `.` or `..` component, no
 * control character, bounded length, and on Windows a drive or UNC path whose
 * components are plain names (no stream, device or wildcard syntax). */
bool WoTIsRememberableInstallerPath(FStringView Path);

/** Canonical JSON `{"format":"wot-remembered-installer-v1","path":...}`.
 * Refuses a path WoTIsRememberableInstallerPath rejects. */
TWoTBootstrapResult<TArray<uint8>> WoTEncodeRememberedInstallerRecord(
	FStringView CanonicalInstallerPath);

/** The record is untrusted input: exactly the canonical bytes of one v1
 * record, or an error. Returns the recorded path. */
TWoTBootstrapResult<FString> WoTParseRememberedInstallerRecord(
	TConstArrayView<uint8> Bytes);

struct FWoTRememberedInstallerRead
{
	/** Set only for a present, well-formed record. */
	TOptional<FString> CanonicalInstallerPath;
	/** Empty when a record was read or none exists; otherwise a short,
	 * path-free token saying why the record was ignored. */
	FString IgnoredReason;
};

/** Never fails: no record is normal, and an unreadable or malformed one is
 * ignored. When ExpectedUserStateRootIdentity is given, a data folder that is
 * no longer that object is not read. Nothing is created or deleted. */
FWoTRememberedInstallerRead WoTReadRememberedInstaller(
	const FString& UserStateRoot,
	const FWoTNativeFileIdentity* ExpectedUserStateRootIdentity);

/** Replaces the record through a unique sibling temporary: write, flush,
 * rename over the old record, flush the directory. A crash leaves either the
 * previous record or the complete new one. */
FWoTBootstrapStatus WoTWriteRememberedInstaller(
	const FString& UserStateRoot,
	const FWoTNativeFileIdentity* ExpectedUserStateRootIdentity,
	FStringView CanonicalInstallerPath);
