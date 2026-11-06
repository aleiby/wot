// The sole authority for per-user knowledge and per-copy activation paths.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTBootstrapCore/NativeHandles.h"

struct FWoTUpdaterStateRoot
{
	FString UserStateRoot;
	FString InstallRoot;
	/** Actual safe running-tree leaf; the staged archive leaf remains canonical. */
	FString LiveLeaf;
	FString UpdateDir;
	FString StateDir;
	FString StagingDir;
	FString WorkDir;
	FString DownloadPath;
	FString StagedTreeDir;
	FString PreviousTreeDir;
	FString ReadyPath;
	FString TrialPath;
	FString RolledBackPath;
};

/** Project only per-user state. Install/update fields remain empty. */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTUpdaterStateRoot>
ResolveUpdaterUserStateRoot(FStringView UserStateRoot);

/** Validate and project updater paths. This function never touches the filesystem. */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTUpdaterStateRoot>
ResolveUpdaterStateRoot(FStringView UserStateRoot, FStringView InstallRoot);

/** Resolve roots for a copy whose running tree has the supplied actual leaf. */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTUpdaterStateRoot>
ResolveUpdaterStateRoot(FStringView UserStateRoot, FStringView InstallRoot,
	FStringView LiveLeaf);

enum class EWoTUpdateTrialState : uint8 { None, Pending, Started };
struct FWoTUpdateMarker
{
	uint64 Sequence = 0;
	EWoTUpdateTrialState State = EWoTUpdateTrialState::None;
};

/** Bounded ASCII, positive int64 decimal followed by LF; trials add a state. */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTUpdateMarker> ParseWoTUpdateMarker(
	TConstArrayView<uint8> Bytes, bool bTrial);
WOTUPDATERCORE_API TWoTBootstrapResult<TOptional<FWoTUpdateMarker>> ReadWoTUpdateMarker(
	const FWoTUpdaterStateRoot& Root, FStringView Leaf, bool bTrial = false);
WOTUPDATERCORE_API FWoTBootstrapStatus WriteWoTUpdateMarker(
	const FWoTUpdaterStateRoot& Root, FStringView Leaf, const FWoTUpdateMarker& Marker);
WOTUPDATERCORE_API FWoTBootstrapStatus RemoveWoTUpdateEntry(
	const FWoTNativeDirectoryHandle& Parent, FStringView Leaf);
/** Called by the game only after its lifetime shared profile acquire. */
WOTUPDATERCORE_API FWoTBootstrapStatus WoTMarkUpdateTrialStarted(
	const FWoTUpdaterStateRoot& Root);
WOTUPDATERCORE_API FWoTBootstrapStatus WoTConsumeUpdateRollback(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence);
/** Only the frontend-handoff milestone may retire trial and prev. */
WOTUPDATERCORE_API FWoTBootstrapStatus WoTCompleteUpdateTrial(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence);
