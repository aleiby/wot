// Per-user once-per-release rollback notice marker.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

/** Shared producer/UI directory policy; Mac uses the preferences location. */
WOTUPDATERCORE_API bool WoTResolveUpdateFailureNoticeDirectory(
	FStringView UpdaterUserStateRoot, FString& OutDirectory);

/** Preserve a seen marker for the same N; a different N becomes pending. */
WOTUPDATERCORE_API FWoTBootstrapStatus RecordUpdateFailedNotice(
	const FWoTUpdaterStateRoot& Root, uint64 FailedReleaseSequence,
	uint64 RestoredReleaseSequence);

/** Reopen and verify the exact pending/seen marker for this failed release. */
WOTUPDATERCORE_API FWoTBootstrapStatus VerifyUpdateFailedNotice(
	const FWoTUpdaterStateRoot& Root, uint64 FailedReleaseSequence,
	uint64 RestoredReleaseSequence);

#if WITH_AUTOMATION_TESTS
WOTUPDATERCORE_API void WoTSetUpdateFailureNoticeDirectoryForTesting(
	FString Directory);
/** Let a policy spec resolve the real canonical directory (never write to it). */
WOTUPDATERCORE_API void WoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting(
	bool bAllow);
/** Exercise the platform choice without consulting process-global overrides. */
WOTUPDATERCORE_API bool WoTResolveUpdateFailureNoticeDirectoryForTesting(
	FStringView UpdaterUserStateRoot, FStringView CanonicalPreferencesDirectory,
	bool bMacPolicy, FString& OutDirectory);
#endif

/** Reads failed=N from pending AND acknowledged notices. Zero means absent. */
WOTUPDATERCORE_API TWoTBootstrapResult<uint64> ReadUpdateFailedSequence(const FWoTUpdaterStateRoot& Root);
/** Only an authenticated strictly newer announcement clears the failed sequence. */
WOTUPDATERCORE_API FWoTBootstrapStatus ClearUpdateFailedSequenceForNewerRelease(
	const FWoTUpdaterStateRoot& Root, uint64 AuthenticatedSequence);
