#include "WoTGameUpdateTransports.h"

namespace
{
FWoTBootstrapStatus WoTGameUpdateTransportsError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(
		MakeWoTBootstrapError(Code, Message));
}

class FWoTGameReleaseStreamingSink final : public IWoTHttpBodySink
{
public:
	explicit FWoTGameReleaseStreamingSink(IWoTReleaseDownloadSink& InSink)
		: Sink(InSink)
	{
	}

	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse& Response) override
	{
		// Redirect bodies are never release bodies. Production executors invoke a
		// sink only for terminal 200/206 responses, but keep scripted executors
		// from turning a redirect into a core policy decision.
		if (Response.Status != 200)
		{
			return FWoTBootstrapStatus::Success();
		}
		bResponseStarted = true;
		return Sink.OnResponseStart(Describe(Response));
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> InBytes) override
	{
		return Sink.Write(InBytes);
	}

	bool HasResponseStarted() const { return bResponseStarted; }

	static FWoTReleaseDownloadResponse Describe(
		const FWoTHttpHopResponse& Response)
	{
		FWoTReleaseDownloadResponse Described;
		Described.Status = Response.Status;
		Described.BodyLength = Response.BodyLength;
		return Described;
	}

private:
	IWoTReleaseDownloadSink& Sink;
	bool bResponseStarted = false;
};
} // namespace

FWoTGameAnnouncementTransport::FWoTGameAnnouncementTransport(
	IWoTHttpHopExecutor& InHops, FString InAnnouncementJsonUrl,
	double InSharedDeadlineSeconds, TFunction<double()> InNowSeconds)
	: Hops(InHops)
	, AnnouncementJsonUrl(MoveTemp(InAnnouncementJsonUrl))
	, SharedDeadlineSeconds(InSharedDeadlineSeconds)
	, NowSeconds(MoveTemp(InNowSeconds))
{
}

FWoTBootstrapStatus FWoTGameAnnouncementTransport::FetchPair(
	TArray<uint8>& OutAnnouncement, TArray<uint8>& OutSignature)
{
	return WoTFetchSignedPair(Hops, AnnouncementJsonUrl,
		SharedDeadlineSeconds, NowSeconds,
		OutAnnouncement, OutSignature);
}

FWoTGameReleaseDownload::FWoTGameReleaseDownload(
	IWoTHttpHopExecutor& InHops,
	const FWoTAnnouncementV1& InAnnouncement,
	TArray<uint8> InAnnouncementBytes,
	TArray<uint8> InAnnouncementSignature,
	double InPerOperationDeadlineSeconds,
	TFunction<double()> InNowSeconds,
	TFunction<void(uint64)> InProgress)
	: Hops(InHops)
	, Announcement(InAnnouncement)
	, PerOperationDeadlineSeconds(InPerOperationDeadlineSeconds)
	, NowSeconds(MoveTemp(InNowSeconds))
	, Progress(MoveTemp(InProgress))
{
	CachedMetadata.AnnouncementBytes = MoveTemp(InAnnouncementBytes);
	CachedMetadata.AnnouncementSignature =
		MoveTemp(InAnnouncementSignature);
}

FWoTBootstrapStatus FWoTGameReleaseDownload::Fetch(
	IWoTReleaseDownloadSink& Sink)
{
	if (!bHasVerifiedArchiveSource)
	{
		return WoTGameUpdateTransportsError(EWoTBootstrapError::UnsafePath,
			TEXT("a core-verified archive source has not been installed"));
	}

	// Retained with the verified source for the worker/core handoff; transport
	// intentionally leaves all size interpretation to the updater core.
	(void)VerifiedArchiveSizeBytes;

	// Per-operation budget: this object outlives a single fetch (metadata is
	// prefetched a phase earlier), so derive the absolute deadline fresh now
	// rather than at construction time.
	const double AbsoluteDeadlineSeconds =
		NowSeconds() + PerOperationDeadlineSeconds;
	FWoTHttpHopResponse Terminal;
	TArray<uint8> BufferedBody;
	// Every attempt requests a full object and enforces its signed size.
	FWoTGameReleaseStreamingSink StreamingSink(Sink);
	FWoTBootstrapStatus Fetched = WoTFetchReleaseObject(Hops,
		VerifiedArchiveUrl, AbsoluteDeadlineSeconds,
		NowSeconds, BufferedBody, Terminal, &StreamingSink);

	if (Fetched.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(Fetched).GetError());
	}
	if (!StreamingSink.HasResponseStarted())
	{
		if (FWoTBootstrapStatus Started = Sink.OnResponseStart(
				FWoTGameReleaseStreamingSink::Describe(Terminal)); Started.IsError())
		{
			return Started;
		}
		if (!BufferedBody.IsEmpty())
		{
			return Sink.Write(BufferedBody);
		}
	}
	return FWoTBootstrapStatus::Success();
}

void FWoTGameReleaseDownload::ReportProgress(uint64 CompletedBytes)
{
	if (Progress)
	{
		Progress(CompletedBytes);
	}
}

FWoTBootstrapStatus FWoTGameReleaseDownload::FetchSignedMetadata(
	FWoTReleaseStagingMetadata& OutMetadata)
{
	if (bMetadataCached)
	{
		OutMetadata = CachedMetadata;
		return FWoTBootstrapStatus::Success();
	}

	FWoTReleaseStagingMetadata FetchedMetadata;
	FetchedMetadata.AnnouncementBytes =
		CachedMetadata.AnnouncementBytes;
	FetchedMetadata.AnnouncementSignature =
		CachedMetadata.AnnouncementSignature;

	// One fresh per-operation deadline covers both signed-pair sub-fetches of
	// this single metadata operation (see the header's duration note).
	const double AbsoluteDeadlineSeconds =
		NowSeconds() + PerOperationDeadlineSeconds;
	FWoTBootstrapStatus KeyRingStatus = WoTFetchSignedPair(Hops,
		Announcement.KeyRing.Url, AbsoluteDeadlineSeconds,
		NowSeconds, FetchedMetadata.KeyRingBytes,
		FetchedMetadata.KeyRingSignature);
	if (KeyRingStatus.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(KeyRingStatus).GetError());
	}

	FWoTBootstrapStatus ManifestStatus = WoTFetchSignedPair(Hops,
		Announcement.Manifest.Url, AbsoluteDeadlineSeconds,
		NowSeconds, FetchedMetadata.ReleaseManifestBytes,
		FetchedMetadata.ReleaseManifestSignature);
	if (ManifestStatus.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(ManifestStatus).GetError());
	}

	CachedMetadata = MoveTemp(FetchedMetadata);
	bMetadataCached = true;
	OutMetadata = CachedMetadata;
	return FWoTBootstrapStatus::Success();
}

void FWoTGameReleaseDownload::SetVerifiedArchiveSource(
	FString ArchiveUrl, uint64 ArchiveSizeBytes)
{
	VerifiedArchiveUrl = MoveTemp(ArchiveUrl);
	VerifiedArchiveSizeBytes = ArchiveSizeBytes;
	bHasVerifiedArchiveSource = true;
}
