// Authentication of the one player-supplied adjacent retail installer.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/Package.h"

enum class EWoTInstallerSource : uint8
{
	Adjacent,
	Downloads,
	Picked,
	/** A Browse pick recorded after it built the cache; searched right after
	 * the installer beside the game and before Downloads. */
	Remembered,
};

/** The canonical spelling of a Browse pick: its parent directory resolved once
 * through any links above it, then the picked leaf unchanged. A remembered pick
 * passes the same rule before it is searched again. */
TWoTBootstrapResult<FString> WoTCanonicalizeInstallerPickPath(
	const FString& AbsolutePath);

/** One identity-bound parent and child name that may hold the installer. */
struct FWoTInstallerLocation
{
	EWoTInstallerSource Source = EWoTInstallerSource::Adjacent;
	FString CanonicalPath;
	TOptional<FWoTNativeFileIdentity> ParentIdentity;
	bool bRequirePinnedName = true;
};

/** One identity-pinned installer parent held open for several candidate leaves. */
struct FWoTHeldInstallerParent
{
	FString CanonicalPath;
	FWoTNativeDirectoryHandle Directory;
};

struct FWoTVerifiedInstaller
{
	EWoTInstallerSource Source = EWoTInstallerSource::Adjacent;
	FWoTHeldInstallerSnapshot Snapshot;
};

enum class EWoTPinnedAdjacentInstallerProbe : uint8
{
	Valid,
	Missing,
	NotTheInstaller,
	Candidate,
	Downloading,
	OnlineOnly,
	UnsupportedReparsePoint,
	/** The installer's size, but the file has more than one name. */
	HardLinked,
};

/** A hash-free observation used to prove that a complete-looking copy is
 * stable before the one-pass held verification reads it. */
struct FWoTAdjacentInstallerInspection
{
	EWoTPinnedAdjacentInstallerProbe State =
		EWoTPinnedAdjacentInstallerProbe::Missing;
	TOptional<FWoTNativeFileIdentity> Identity;
	/** Player-facing retry guidance for states whose remedy depends on this path. */
	FString Message;
};

/** One-pass adjacent result. Valid owns the exact held snapshot whose bytes
 * were authenticated; the other states never carry installer authority. */
struct FWoTPinnedAdjacentInstallerResult
{
	EWoTPinnedAdjacentInstallerProbe State =
		EWoTPinnedAdjacentInstallerProbe::Missing;
	FString Message;
	TUniquePtr<FWoTVerifiedInstaller> Verified;
};

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocation(FWoTInstallerLocation& Location);

/** Opens a candidate parent once, capturing or enforcing its shared identity. */
TWoTBootstrapResult<FWoTHeldInstallerParent> WoTOpenInstallerParent(
	const FString& CanonicalParentPath,
	TOptional<FWoTNativeFileIdentity>& ParentIdentity);

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationAtParent(FWoTInstallerLocation& Location,
	const FWoTHeldInstallerParent& Parent);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocation(FWoTInstallerLocation& Location,
	TFunction<bool()> IsCancelled = {},
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationAtParent(FWoTInstallerLocation& Location,
	const FWoTHeldInstallerParent& Parent,
	TFunction<bool()> IsCancelled = {},
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectPinnedAdjacentInstaller(const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity = nullptr);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquirePinnedAdjacentInstaller(const FString& CanonicalAdjacentPath,
	TFunction<bool()> IsCancelled = {},
	const FWoTNativeFileIdentity* ExpectedParentIdentity = nullptr,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

/** Creates and removes a unique file through the authenticated adjacent parent.
 * Unknown create/delete results are a successful `false` (fail closed). */
TWoTBootstrapResult<bool> WoTProbeAdjacentInstallerParentWritable(
	const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity = nullptr);

/** Best-effort retirement of the removed download-resume directory. */
void WoTRemoveLegacyInstallerStateBestEffort(const FString& StateDir);

bool WoTInstallerAcquisitionIsParentIdentityMismatch(
	const FWoTBootstrapError& Error);
bool WoTInstallerAcquisitionIsIdentityMutation(
	const FWoTBootstrapError& Error);

#if WITH_AUTOMATION_TESTS
struct FWoTInstallerAcquisitionPins
{
	FWoTInstallerAcquisitionPins();

	FString Name;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

struct FWoTInstallerAcquisitionTestHooks
{
	/** Observes each complete held-file SHA-256 pass and its byte count. */
	TFunction<void(uint64)> FullHash;
	/** Runs after inspection but before the authenticated leaf is reopened. */
	TFunction<void()> BeforeHeldOpen;
	/** Runs after hashing and before the held snapshot is returned. */
	TFunction<void()> BeforeSnapshot;
};

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationForTesting(
	FWoTInstallerLocation& Location,
	const FWoTInstallerAcquisitionPins& Pins);

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationAtParentForTesting(
	FWoTInstallerLocation& Location, const FWoTHeldInstallerParent& Parent,
	const FWoTInstallerAcquisitionPins& Pins);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationForTesting(
	FWoTInstallerLocation& Location,
	const FWoTInstallerAcquisitionPins& Pins,
	TFunction<bool()> IsCancelled = {},
	const FWoTInstallerAcquisitionTestHooks& TestHooks = {},
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationAtParentForTesting(
	FWoTInstallerLocation& Location, const FWoTHeldInstallerParent& Parent,
	const FWoTInstallerAcquisitionPins& Pins,
	TFunction<bool()> IsCancelled = {},
	const FWoTInstallerAcquisitionTestHooks& TestHooks = {},
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectPinnedAdjacentInstallerForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTInstallerAcquisitionPins& Pins,
	const FWoTNativeFileIdentity* ExpectedParentIdentity = nullptr);

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquirePinnedAdjacentInstallerForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTInstallerAcquisitionPins& Pins,
	TFunction<bool()> IsCancelled = {},
	const FWoTInstallerAcquisitionTestHooks& TestHooks = {},
	const FWoTNativeFileIdentity* ExpectedParentIdentity = nullptr,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr);

TWoTBootstrapResult<bool> WoTProbeAdjacentInstallerParentWritableForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity,
	TFunction<void(FStringView)> AfterCreate);
#endif
