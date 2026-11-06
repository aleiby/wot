#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapViewModel.h"

/** Immutable diagnostic filename for this process run. */
FString WoTBootstrapHeartbeatFileName();

/** Stable machine token for a bootstrap phase. Shared by the boot-failure
 * receipt and the heartbeat log; never localized. */
const TCHAR* WoTBootstrapPhaseName(EWoTBootstrapPhase Phase);

/**
 * Append one timestamped diagnostic line to this process run's per-user log.
 * Returns false when the record's bytes could not be written. In non-guarded
 * builds this is a no-op unless a testing root is installed, so editor
 * automation never touches the real per-user state root.
 */
bool WoTRecordBootstrapHeartbeat(
	FStringView Event, EWoTBootstrapPhase Phase, uint64 OperationEpoch,
	TOptional<uint64> Completed = {}, TOptional<uint64> Total = {},
	FStringView Detail = {});

#if WITH_AUTOMATION_TESTS
/** Route heartbeat lines below an explicit install root. Every reset starts a
 * new simulated process run with a new immutable filename. */
void SetWoTBootstrapHeartbeatRootForTesting(FString InstallRoot);
/** Make the next eligible open attempt fail before touching the filesystem. */
void FailNextWoTBootstrapHeartbeatOpenForTesting();
/** The exact line (including trailing newline) a record produces. */
FString WoTBootstrapHeartbeatLineForTesting(
	FStringView Event, EWoTBootstrapPhase Phase, uint64 OperationEpoch,
	TOptional<uint64> Completed, TOptional<uint64> Total,
	FStringView Detail, FStringView TimestampUtc, uint32 ProcessId);
#endif
