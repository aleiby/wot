// Production updater-core seam adapters over the socket-free hop engine.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTUpdaterCore/AnnouncementPolicy.h"
#include "WoTUpdaterCore/ReleaseStaging.h"

#include "WoTUpdateTransports/WoTUpdateTransportPolicy.h"

class FWoTGameAnnouncementTransport final : public IWoTAnnouncementTransport
{
public:
	FWoTGameAnnouncementTransport(IWoTHttpHopExecutor& InHops,
		FString InAnnouncementJsonUrl, double InSharedDeadlineSeconds,
		TFunction<double()> InNowSeconds);
	virtual FWoTBootstrapStatus FetchPair(TArray<uint8>& OutAnnouncement,
		TArray<uint8>& OutSignature) override;

private:
	IWoTHttpHopExecutor& Hops;
	FString AnnouncementJsonUrl;
	double SharedDeadlineSeconds = 0.0;
	TFunction<double()> NowSeconds;
};

class FWoTGameReleaseDownload final : public IWoTReleaseDownload
{
public:
	/** AnnouncementBytes/Signature are the exact pair the assess step already
	 * fetched; FetchSignedMetadata returns them from this cache and never
	 * re-fetches the announcement. Progress receives the core-accepted archive
	 * prefix, whose base is selected by DecideHttpBody.
	 *
	 * InPerOperationDeadlineSeconds is a DURATION budget, not an absolute
	 * deadline: each logical operation (one FetchSignedMetadata, one Fetch)
	 * derives its own absolute deadline as NowSeconds() + this budget at the
	 * moment it runs. This object outlives a single operation — the worker
	 * prefetches metadata during CheckingGameUpdates and the archive Fetch
	 * runs a phase later in DownloadingUpdate — so a construction-time absolute
	 * deadline would have already elapsed by the time staging fetches ranges.
	 * (Contrast the announcement transport's single-use InSharedDeadlineSeconds,
	 * which is an absolute deadline shared across its two logical fetches.) */
	FWoTGameReleaseDownload(IWoTHttpHopExecutor& InHops,
		const FWoTAnnouncementV1& InAnnouncement,
		TArray<uint8> InAnnouncementBytes,
		TArray<uint8> InAnnouncementSignature,
		double InPerOperationDeadlineSeconds,
		TFunction<double()> InNowSeconds,
		TFunction<void(uint64)> InProgress);
	virtual FWoTBootstrapStatus Fetch(
		IWoTReleaseDownloadSink& Sink) override;
	virtual FWoTBootstrapStatus FetchSignedMetadata(
		FWoTReleaseStagingMetadata& OutMetadata) override;
	virtual void ReportProgress(uint64 CompletedBytes) override;

	/** Called only with a core-verified manifest's Archive ref. Until then
	 * Fetch refuses with EWoTBootstrapError::UnsafePath. */
	void SetVerifiedArchiveSource(FString ArchiveUrl, uint64 ArchiveSizeBytes);

private:
	IWoTHttpHopExecutor& Hops;
	FWoTAnnouncementV1 Announcement;
	FWoTReleaseStagingMetadata CachedMetadata;
	double PerOperationDeadlineSeconds = 0.0;
	TFunction<double()> NowSeconds;
	TFunction<void(uint64)> Progress;

	bool bMetadataCached = false;
	bool bHasVerifiedArchiveSource = false;
	FString VerifiedArchiveUrl;
	uint64 VerifiedArchiveSizeBytes = 0;

};
