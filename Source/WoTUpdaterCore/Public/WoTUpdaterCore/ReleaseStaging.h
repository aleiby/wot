// Authenticated release download, extraction, and exact-tree verification.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Announcement.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

/** Response facts checked before accepting any archive bytes. */
struct FWoTReleaseDownloadResponse
{
	int32 Status = 0;
	uint64 BodyLength = 0;
};

/** Core-owned sink for one release-object response. */
class IWoTReleaseDownloadSink
{
public:
	virtual ~IWoTReleaseDownloadSink() = default;
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTReleaseDownloadResponse& Response) = 0;
	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> Bytes) = 0;
};

/** Signed inputs used to authenticate the release manifest. */
struct FWoTReleaseStagingMetadata
{
	TArray<uint8> AnnouncementBytes;
	TArray<uint8> AnnouncementSignature;
	TArray<uint8> KeyRingBytes;
	TArray<uint8> KeyRingSignature;
	TArray<uint8> ReleaseManifestBytes;
	TArray<uint8> ReleaseManifestSignature;
};

struct FWoTStagedRelease
{
	uint64 ReleaseSequence = 0;
	FString StagedTreeDir;
	FWoTSha256 StagedTreeRoot;
};

/** Downloads the complete signed archive from byte zero on every attempt. */
class IWoTReleaseDownload
{
public:
	virtual ~IWoTReleaseDownload() = default;
	virtual FWoTBootstrapStatus Fetch(IWoTReleaseDownloadSink& Sink) = 0;
	virtual FWoTBootstrapStatus FetchSignedMetadata(
		FWoTReleaseStagingMetadata& OutMetadata) = 0;

	/** Reports the core sink's accepted prefix. Production forwards to the UI. */
	virtual void ReportProgress(uint64 CompletedBytes)
	{
		(void)CompletedBytes;
	}

	/** Deterministic checkpoint after the committed download closes, before readback. */
	virtual FWoTBootstrapStatus BeforeArchiveVerification(FStringView ArchivePath)
	{
		(void)ArchivePath;
		return FWoTBootstrapStatus::Success();
	}

	/** Test race checkpoint immediately after archive extraction completes. */
	virtual FWoTBootstrapStatus AfterArchiveExtracted(FStringView StagedTreeDir)
	{
		(void)StagedTreeDir;
		return FWoTBootstrapStatus::Success();
	}

	/** Deterministic race checkpoint; production transports leave it a no-op. */
	virtual FWoTBootstrapStatus BeforeInstalledTreeVerification(
		FStringView StagedTreeDir)
	{
		(void)StagedTreeDir;
		return FWoTBootstrapStatus::Success();
	}

	/** Test race checkpoint after enumeration/hash and before snapshot revalidation. */
	virtual FWoTBootstrapStatus BeforeInstalledTreeSnapshotValidation(
		FStringView StagedTreeDir)
	{
		(void)StagedTreeDir;
		return FWoTBootstrapStatus::Success();
	}
};

/**
 * Check live-tree mutation rights without changing it, then create and remove
 * `.wot-update/.probe-<guid>` through no-follow handles.
 */
WOTUPDATERCORE_API FWoTBootstrapStatus ProbeUpdaterInstallWritable(
	const FWoTUpdaterStateRoot& Root,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation =
		[]() { return FWoTBootstrapStatus::Success(); });

/**
 * Stage one accepted announcement, in the fixed fail-closed order documented in
 * the per-user updater plan. On every post-creation error the fresh staging
 * directory is recursively removed without following links.
 */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTStagedRelease> StageRelease(
	const FWoTAnnouncementV1& Announcement, const FWoTKeyRing& TrustedRing,
	IWoTReleaseDownload& Download, const FWoTUpdaterStateRoot& Root,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation = []()
	{
		return FWoTBootstrapStatus::Success();
	});

#if WITH_AUTOMATION_TESTS
using FWoTReleaseStagingCodeSealVerifierForTesting =
	TFunction<FWoTBootstrapStatus(const FString&)>;

class WOTUPDATERCORE_API
	FWoTReleaseStagingCodeSealVerifierOverrideForTesting final
{
public:
	explicit FWoTReleaseStagingCodeSealVerifierOverrideForTesting(
		FWoTReleaseStagingCodeSealVerifierForTesting InVerifier);
	~FWoTReleaseStagingCodeSealVerifierOverrideForTesting();

	FWoTReleaseStagingCodeSealVerifierOverrideForTesting(
		const FWoTReleaseStagingCodeSealVerifierOverrideForTesting&) = delete;
	FWoTReleaseStagingCodeSealVerifierOverrideForTesting& operator=(
		const FWoTReleaseStagingCodeSealVerifierOverrideForTesting&) = delete;

private:
	FWoTReleaseStagingCodeSealVerifierForTesting Previous;
};

/** Drives the exact production chunk loop without constructing an archive. */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTSha256>
	WoTStagingHashWithYieldForTesting(TConstArrayView<uint8> Bytes,
		TFunctionRef<FWoTBootstrapStatus()> CheckCancellation);

/** Forces the complete-archive streaming read to fail before its next chunk. */
WOTUPDATERCORE_API void WoTSetStagingArchiveReadFailureForTesting(bool bFail);
#endif
