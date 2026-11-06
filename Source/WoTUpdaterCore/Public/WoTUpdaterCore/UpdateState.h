// Best-effort high-water state for update metadata.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

struct FWoTUpdateStateRecord
{
	/** Highest root-verified release-key-ring epoch accepted by this process. */
	uint64 KeyRevocationFloor = 0;

	/** Highest authenticated announcement sequence accepted by this process. */
	uint64 NewestAnnouncementSequence = 0;
};

struct FWoTUpdateStateRecordReadResult
{
	FWoTUpdateStateRecord State;

	/** Empty for a valid record; otherwise explains why defaults won. */
	FString Diagnostic;
};

/** Missing state is normal. Unreadable, oversized, or malformed state defaults to zero. */
WOTUPDATERCORE_API FWoTUpdateStateRecordReadResult ReadWoTUpdateState(
	const FWoTUpdaterStateRoot& Root);

/**
 * Reread, merge fieldwise maxima into InOutState, and best-effort replace the
 * record through a unique sibling temporary. No lock or strict cross-process
 * monotonicity is implied. InOutState keeps its maxima when persistence fails.
 */
WOTUPDATERCORE_API FWoTBootstrapStatus MergeAndWriteWoTUpdateState(
	const FWoTUpdaterStateRoot& Root, FWoTUpdateStateRecord& InOutState,
	FString* OutDiagnostic = nullptr);
