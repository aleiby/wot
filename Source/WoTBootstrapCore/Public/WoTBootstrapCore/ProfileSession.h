// One process-owned shared/exclusive lock for every mutable profile entry.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/NativeHandles.h"

/** Exact root-level lock leaf below Data. */
WOTBOOTSTRAPCORE_API const TCHAR* WoTProfileSessionLockLeaf();

/** Clear player-facing reason when another process owns Data exclusively. */
WOTBOOTSTRAPCORE_API const TCHAR* WoTProfileSessionContentionMessage();

/**
 * A live hold on Data/session.lock.
 *
 * Games hold SHARED for their process lifetime. Cache mutation, Mac uninstall,
 * and the Mac quit swap temporarily take EXCLUSIVE without opening another
 * lock. The object is move-only so ownership cannot be duplicated accidentally.
 */
class WOTBOOTSTRAPCORE_API FWoTProfileSession final
{
public:
	FWoTProfileSession();
	~FWoTProfileSession();
	FWoTProfileSession(FWoTProfileSession&& Other);
	FWoTProfileSession& operator=(FWoTProfileSession&& Other);

	FWoTProfileSession(const FWoTProfileSession&) = delete;
	FWoTProfileSession& operator=(const FWoTProfileSession&) = delete;

	bool IsShared() const;
	bool IsExclusive() const;
	const FString& DataRoot() const { return RootPath; }
	const FWoTNativeFileIdentity& DataRootIdentity() const
	{
		return RootIdentity;
	}

	/** Drop SHARED, try EXCLUSIVE without waiting, and restore SHARED on refusal. */
	TWoTBootstrapResult<bool> TryAcquireExclusive();

	/** Release EXCLUSIVE and reacquire the ordinary process-lifetime SHARED hold. */
	FWoTBootstrapStatus RestoreShared();

	/** Delete every Data entry except the held root-level session.lock. */
	FWoTBootstrapStatus DeleteProfileContentsExceptSessionLock();

	/** Unlink session.lock last, remove the empty Data root, then become invalid. */
	FWoTBootstrapStatus DeleteSessionLockAndEmptyProfileRoot();

private:
	friend WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTProfileSession>
		AcquireWoTProfileSessionShared(FStringView);

	enum class EMode : uint8
	{
		None,
		Shared,
		Exclusive,
	};

	FWoTBootstrapStatus RevalidatePath() const;
	FWoTBootstrapStatus AcquireLock(
		EWoTNativeFileLockMode NativeMode, EMode NewMode);

	FString RootPath;
	FWoTNativeFileIdentity RootIdentity;
	FWoTNativeFileIdentity FileIdentity;
	FWoTNativeDirectoryHandle RootDirectory;
	FWoTNativeFileHandle File;
	FWoTNativeFileLock Lock;
	EMode Mode = EMode::None;
};

/** Open/create Data/session.lock and take a nonblocking SHARED hold. */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTProfileSession>
	AcquireWoTProfileSessionShared(FStringView DataRoot);

/** Install the one process-lifetime owner. Repeated acquisition of the same
 * root is idempotent; a different root is refused. */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus
	WoTAcquireProcessProfileSessionShared(FStringView DataRoot);

/** Temporarily remove the process owner for an exclusive mutation transaction. */
WOTBOOTSTRAPCORE_API TUniquePtr<FWoTProfileSession>
	WoTTakeProcessProfileSession();

/** Restore a SHARED transaction owner to the process-lifetime slot. */
WOTBOOTSTRAPCORE_API bool WoTRestoreProcessProfileSession(
	TUniquePtr<FWoTProfileSession> Session);

/** Read-only view of the current process owner; never retain this pointer. */
WOTBOOTSTRAPCORE_API const FWoTProfileSession*
	WoTGetProcessProfileSession();

/** Select the startup message without showing a platform dialog. */
WOTBOOTSTRAPCORE_API FString WoTProfileSessionFailureMessage(
	FStringView DataRoot, const FWoTBootstrapError& Error);

#if WITH_AUTOMATION_TESTS
WOTBOOTSTRAPCORE_API void WoTResetProcessProfileSessionForTesting();
#endif
