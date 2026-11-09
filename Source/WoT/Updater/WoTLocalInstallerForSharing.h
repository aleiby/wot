// Where this computer keeps the original installer, for LAN installer sharing
// (spec B5). The search walks the bootstrap sweep's own locations with Inspect
// only: it never hashes, never creates and never deletes anything.
#pragma once

#include "CoreMinimal.h"
#include "WoTInstallerAcquisition.h"

enum class EWoTSharingInstallerSource : uint8
{
	Adjacent,
	Remembered,
	/** <user data root>/OriginalGame/<pinned name>, where a receiver places a
	 * copy it got over the network (written by the receiver, never here). */
	Received,
	Downloads,
};

const TCHAR* LexToString(EWoTSharingInstallerSource Source);

struct FWoTLocalInstallerSearch
{
	/** The bootstrap's retained location beside the game, when it resolved. */
	TOptional<FWoTInstallerLocation> Adjacent;
	/** The adopted user data root and its identity (Remembered, Received).
	 * Empty skips both. */
	FString UserStateRoot;
	TOptional<FWoTNativeFileIdentity> UserStateRootIdentity;
	/** The canonical Downloads folder; empty skips Downloads. */
	FString DownloadsDirectory;
	/** A remembered pick under this folder is skipped (textually, without
	 * touching it). The Mac sharer sets it to Downloads. */
	FString SkipRememberedUnder;
#if WITH_AUTOMATION_TESTS
	/** Small pinned artifact for specs; production always uses the pin. */
	TOptional<FWoTInstallerAcquisitionPins> PinsForTesting;
#endif
};

/** One installer-looking file: the pinned size, singly linked, fully local.
 * Its bytes are unverified until a transfer hashes them. */
struct FWoTLocalInstallerForSharing
{
	EWoTSharingInstallerSource Source = EWoTSharingInstallerSource::Adjacent;
	FWoTInstallerLocation Location;
	FWoTNativeFileIdentity Identity;
};

/** The production search: the given adjacent location, the adopted user data
 * root, and Downloads except on Mac. On Mac the finder skips Downloads, and a
 * remembered pick inside it, until the Files & Folders measurement shows the
 * check cannot raise a permission prompt. */
FWoTLocalInstallerSearch WoTProductionLocalInstallerSearch(
	TOptional<FWoTInstallerLocation> Adjacent);

/** Adjacent, Remembered, Received, then Downloads; the first Candidate. */
TOptional<FWoTLocalInstallerForSharing> WoTFindLocalInstallerForSharing(
	const FWoTLocalInstallerSearch& Search);

/** Opens a found installer for one transfer without hashing it. */
TWoTBootstrapResult<FWoTNativeFileHandle> WoTOpenLocalInstallerForSharing(
	const FWoTLocalInstallerForSharing& Found);

/** The received-installer folder's leaf under the user data root. */
inline constexpr TCHAR WoTReceivedInstallerDirectoryLeaf[] = TEXT("OriginalGame");

/** <UserStateRoot>/OriginalGame/<pinned installer name>, or empty. */
FString WoTReceivedInstallerPathUnder(const FString& UserStateRoot);

/** The received-installer path under the adopted user data root, or empty
 * before the root is adopted. */
FString WoTReceivedInstallerPath();
