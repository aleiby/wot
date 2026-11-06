// Socket-free production-adapter specs. Every helper name is spec-prefixed
// because WoT test sources share unity translation units.
#if WITH_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "../Updater/WoTGameUpdateTransports.h"

namespace
{
constexpr EAutomationTestFlags WoTGameUpdateTransportsSpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

constexpr double WoTGameUpdateTransportsSpecDeadline = 100.0;
constexpr TCHAR WoTGameUpdateTransportsSpecAnnouncementUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/v2/announcement.json");
constexpr TCHAR WoTGameUpdateTransportsSpecKeyRingUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/v2/keyring.json");
constexpr TCHAR WoTGameUpdateTransportsSpecManifestUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/v2/manifest.json");
constexpr TCHAR WoTGameUpdateTransportsSpecArchiveUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/v2/release.zip");

struct FWoTGameUpdateTransportsSpecScriptStep
{
	FWoTHttpHopResponse Response;
	TArray<TArray<uint8>> StreamChunks;
	bool bTransportFailure = false;
};

TArray<uint8> WoTGameUpdateTransportsSpecBytes(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	while (*Text != '\0')
	{
		Bytes.Add(static_cast<uint8>(*Text));
		++Text;
	}
	return Bytes;
}

FWoTGameUpdateTransportsSpecScriptStep
WoTGameUpdateTransportsSpecStep(int32 Status,
	const ANSICHAR* Body = "",
	const TCHAR* ContentRange = TEXT(""),
	const TCHAR* ETag = TEXT(""),
	const TCHAR* LastModified = TEXT(""),
	// DeclaredBodyLength models the Content-Length header. -1 means "the
	// well-formed case": the response declares exactly what it delivers. A
	// non-negative value models a declared/delivered MISMATCH (e.g. an
	// interrupted transfer that declared more than it sent).
	int64 DeclaredBodyLength = -1)
{
	FWoTGameUpdateTransportsSpecScriptStep Step;
	Step.Response.Status = Status;


	Step.Response.Body = WoTGameUpdateTransportsSpecBytes(Body);
	Step.Response.BodyLength = DeclaredBodyLength < 0
		? static_cast<uint64>(Step.Response.Body.Num())
		: static_cast<uint64>(DeclaredBodyLength);
	return Step;
}

FWoTGameUpdateTransportsSpecScriptStep
WoTGameUpdateTransportsSpecTransportFailure()
{
	FWoTGameUpdateTransportsSpecScriptStep Step;
	Step.bTransportFailure = true;
	return Step;
}

class FWoTGameUpdateTransportsSpecExecutor final
	: public IWoTHttpHopExecutor
{
public:
	explicit FWoTGameUpdateTransportsSpecExecutor(
		TArray<FWoTGameUpdateTransportsSpecScriptStep> InScript)
		: Script(MoveTemp(InScript))
	{
	}

	FWoTBootstrapStatus ExecuteHop(const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) override
	{
		Requests.Add(Request);
		if (!Script.IsValidIndex(Cursor))
		{
			OutResponse = FWoTHttpHopResponse();
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("the game-update-transports spec script was exhausted")));
		}

		const FWoTGameUpdateTransportsSpecScriptStep& Step =
			Script[Cursor++];
		OutResponse = Step.Response;
		if (Step.bTransportFailure)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("scripted transport failure")));
		}
		if (Request.BodySink != nullptr)
		{
			FWoTHttpHopResponse Headers = Step.Response;
			Headers.Body.Reset();
			if (FWoTBootstrapStatus Started =
					Request.BodySink->OnResponseStart(Headers);
				Started.IsError())
			{
				OutResponse = FWoTHttpHopResponse();
				return Started;
			}
			const TArray<TArray<uint8>> Chunks = Step.StreamChunks.IsEmpty()
				? TArray<TArray<uint8>>{Step.Response.Body}
				: Step.StreamChunks;
			uint64 Streamed = 0;
			for (const TArray<uint8>& Chunk : Chunks)
			{
				if (FWoTBootstrapStatus Written = Request.BodySink->Write(Chunk);
					Written.IsError())
				{
					OutResponse = FWoTHttpHopResponse();
					return Written;
				}
				Streamed += static_cast<uint64>(Chunk.Num());
			}
			Headers.StreamedBodyLength = Streamed;
			OutResponse = MoveTemp(Headers);
		}
		return FWoTBootstrapStatus::Success();
	}

	TArray<FWoTHttpHopRequest> Requests;

private:
	TArray<FWoTGameUpdateTransportsSpecScriptStep> Script;
	int32 Cursor = 0;
};

class FWoTGameUpdateTransportsSpecClock
{
public:
	explicit FWoTGameUpdateTransportsSpecClock(TArray<double> InTimes)
		: Times(MoveTemp(InTimes))
	{
	}

	double Next()
	{
		if (Times.IsEmpty())
		{
			return 0.0;
		}
		const int32 Index = FMath::Min(Cursor, Times.Num() - 1);
		++Cursor;
		return Times[Index];
	}

private:
	TArray<double> Times;
	int32 Cursor = 0;
};

TFunction<double()> WoTGameUpdateTransportsSpecNow(
	FWoTGameUpdateTransportsSpecClock& Clock)
{
	return [&Clock]()
	{
		return Clock.Next();
	};
}

FWoTAnnouncementV1 WoTGameUpdateTransportsSpecAnnouncement()
{
	FWoTAnnouncementV1 Announcement;
	Announcement.KeyRing.Url = WoTGameUpdateTransportsSpecKeyRingUrl;
	Announcement.Manifest.Url = WoTGameUpdateTransportsSpecManifestUrl;
	return Announcement;
}

bool WoTGameUpdateTransportsSpecMetadataIsEmpty(
	const FWoTReleaseStagingMetadata& Metadata)
{
	return Metadata.AnnouncementBytes.IsEmpty()
		&& Metadata.AnnouncementSignature.IsEmpty()
		&& Metadata.KeyRingBytes.IsEmpty()
		&& Metadata.KeyRingSignature.IsEmpty()
		&& Metadata.ReleaseManifestBytes.IsEmpty()
		&& Metadata.ReleaseManifestSignature.IsEmpty();
}

class FWoTGameUpdateTransportsSpecSink final
	: public IWoTReleaseDownloadSink
{
public:
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTReleaseDownloadResponse& InResponse) override
	{
		Response = InResponse;
		bStarted = true;
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> InBytes) override
	{
		ChunkSizes.Add(InBytes.Num());
		Bytes.Append(InBytes.GetData(), InBytes.Num());
		return FWoTBootstrapStatus::Success();
	}

	FWoTReleaseDownloadResponse Response;
	TArray<uint8> Bytes;
	TArray<int32> ChunkSizes;
	bool bStarted = false;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecAnnouncementSuccess,
	"WoT.Update.Adapters.FetchPair.Success",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecAnnouncementSuccess::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "announcement"),
		WoTGameUpdateTransportsSpecStep(200, "signature"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameAnnouncementTransport Transport(Hops,
		WoTGameUpdateTransportsSpecAnnouncementUrl,
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock));

	TArray<uint8> Announcement;
	TArray<uint8> Signature;
	FWoTBootstrapStatus Status =
		Transport.FetchPair(Announcement, Signature);

	TestFalse(TEXT("the pair fetch succeeds"), Status.IsError());
	TestTrue(TEXT("the announcement body is copied exactly"),
		Announcement == WoTGameUpdateTransportsSpecBytes("announcement"));
	TestTrue(TEXT("the signature body is copied exactly"),
		Signature == WoTGameUpdateTransportsSpecBytes("signature"));
	TestEqual(TEXT("exactly two logical-object hops are issued"),
		Hops.Requests.Num(), 2);
	TestEqual(TEXT("the announcement URL is exact"),
		Hops.Requests[0].Url,
		FString(WoTGameUpdateTransportsSpecAnnouncementUrl));
	TestEqual(TEXT("the signature URL is the literal suffix"),
		Hops.Requests[1].Url,
		FString(WoTGameUpdateTransportsSpecAnnouncementUrl) + TEXT(".sig"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecAnnouncementSharedDeadline,
	"WoT.Update.Adapters.FetchPair.SharedDeadline",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecAnnouncementSharedDeadline::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "announcement"),
		WoTGameUpdateTransportsSpecStep(200, "unreachable-signature"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0, 100.0});
	FWoTGameAnnouncementTransport Transport(Hops,
		WoTGameUpdateTransportsSpecAnnouncementUrl,
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock));

	TArray<uint8> Announcement;
	TArray<uint8> Signature;
	FWoTBootstrapStatus Status =
		Transport.FetchPair(Announcement, Signature);

	TestTrue(TEXT("the exhausted shared deadline refuses the pair"),
		Status.IsError());
	TestEqual(TEXT("only the first logical fetch reaches the executor"),
		Hops.Requests.Num(), 1);
	TestTrue(TEXT("the announcement output is empty"),
		Announcement.IsEmpty());
	TestTrue(TEXT("the signature output is empty"), Signature.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecAnnouncementSecondFailure,
	"WoT.Update.Adapters.FetchPair.SecondFailureIsAtomic",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecAnnouncementSecondFailure::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "announcement"),
		WoTGameUpdateTransportsSpecTransportFailure(),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameAnnouncementTransport Transport(Hops,
		WoTGameUpdateTransportsSpecAnnouncementUrl,
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock));

	TArray<uint8> Announcement =
		WoTGameUpdateTransportsSpecBytes("sentinel-announcement");
	TArray<uint8> Signature =
		WoTGameUpdateTransportsSpecBytes("sentinel-signature");
	FWoTBootstrapStatus Status =
		Transport.FetchPair(Announcement, Signature);

	TestTrue(TEXT("the second logical fetch failure refuses the pair"),
		Status.IsError());
	TestEqual(TEXT("both logical fetches were attempted"),
		Hops.Requests.Num(), 2);
	TestTrue(TEXT("the announcement output is cleared"),
		Announcement.IsEmpty());
	TestTrue(TEXT("the signature output is cleared"), Signature.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRangeRequiresVerifiedSource,
	"WoT.Update.Adapters.FetchRange.RequiresVerifiedArchiveSource",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRangeRequiresVerifiedSource::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());

	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Status = Download.Fetch(Sink);

	TestTrue(TEXT("an unverified archive source is refused"),
		Status.IsError());
	if (Status.IsError())
	{
		TestEqual(TEXT("the refusal is UnsafePath"),
			Status.GetError().Code, EWoTBootstrapError::UnsafePath);
	}
	TestFalse(TEXT("the sink is not started"), Sink.bStarted);
	TestEqual(TEXT("the refusal issues no hop"), Hops.Requests.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRangeZero,
	"WoT.Update.Adapters.FetchRange.ZeroHasNoRange",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRangeZero::RunTest(const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "whole-body"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 10);

	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Fetched = Download.Fetch(Sink);

	TestFalse(TEXT("the whole-object fetch succeeds"), Fetched.IsError());
	TestTrue(TEXT("the response starts the core sink"), Sink.bStarted);
	TestEqual(TEXT("one hop is issued"), Hops.Requests.Num(), 1);
	TestEqual(TEXT("the terminal 200 is recorded"), Sink.Response.Status, 200);
	TestEqual(TEXT("the body length is recorded"),
		Sink.Response.BodyLength, static_cast<uint64>(10));
	TestTrue(TEXT("the whole body is returned"),
		Sink.Bytes == WoTGameUpdateTransportsSpecBytes("whole-body"));
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRangePerOperationBudget,
	"WoT.Update.Adapters.FetchRange.PerOperationBudgetNotConstructionDeadline",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRangePerOperationBudget::RunTest(
	const FString&)
{
	// Two successive range fetches on ONE long-lived adapter, with wall-clock
	// time advancing far past the per-operation budget between them. A correct
	// duration budget re-derives the absolute deadline each call, so the later
	// fetch still succeeds; a construction-time ABSOLUTE deadline would have
	// already elapsed and refuse it (the object outlives a single operation:
	// metadata is prefetched a phase before staging fetches archive ranges).
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(206, "first", TEXT("bytes 0-4/10")),
		WoTGameUpdateTransportsSpecStep(206, "second", TEXT("bytes 5-9/10")),
	});
	double ClockValue = 0.0;
	TFunction<double()> Now = [&ClockValue]() { return ClockValue; };
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		Now,
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 10);

	FWoTGameUpdateTransportsSpecSink First;
	FWoTBootstrapStatus FetchedFirst = Download.Fetch(First);
	TestFalse(TEXT("the first range within budget succeeds"),
		FetchedFirst.IsError());

	// Advance real time far beyond the per-operation budget (100).
	ClockValue = WoTGameUpdateTransportsSpecDeadline * 5.0;

	FWoTGameUpdateTransportsSpecSink Second;
	FWoTBootstrapStatus FetchedSecond = Download.Fetch(Second);
	TestFalse(TEXT("a later range still succeeds under the per-operation "
		"budget, not a construction-time absolute deadline"),
		FetchedSecond.IsError());
	TestEqual(TEXT("both ranges reached the executor"),
		Hops.Requests.Num(), 2);
	TestTrue(TEXT("the later body is returned"),
		Second.Bytes == WoTGameUpdateTransportsSpecBytes("second"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRange416,
	"WoT.Update.Adapters.FetchRange.Status416IsSurfaced",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRange416::RunTest(const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(
			416, "", TEXT("bytes */100"), TEXT("\"current-tag\"")),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 100);

	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Fetched = Download.Fetch(Sink);

	TestFalse(TEXT("a terminal HTTP status is not swallowed as transport error"),
		Fetched.IsError());
	TestTrue(TEXT("the 416 response starts the core sink"), Sink.bStarted);
	TestEqual(TEXT("the terminal 416 is surfaced"), Sink.Response.Status, 416);
	TestEqual(TEXT("the empty 416 body length is recorded"),
		Sink.Response.BodyLength, static_cast<uint64>(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRangeDeclaredLength,
	"WoT.Update.Adapters.FetchRange.RecordsDeclaredLengthNotDelivered",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRangeDeclaredLength::RunTest(const FString&)
{
	// An interrupted transfer: the response DECLARES 8 bytes (Content-Length)
	// but only 4 arrive. DecideHttpBody consumes the DECLARED length, so
	// The response-start callback must surface 8 -- not the 4 bytes delivered.
	// the delivered count here would let a truncated body masquerade as a
	// length-matched one.
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "abcd", TEXT(""), TEXT(""),
			TEXT(""), /*DeclaredBodyLength=*/8),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 8);

	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Fetched = Download.Fetch(Sink);

	TestFalse(TEXT("the terminal 200 is not a transport error"),
		Fetched.IsError());
	TestTrue(TEXT("the interrupted response starts the core sink"), Sink.bStarted);
	TestEqual(TEXT("only the delivered prefix is returned"),
		Sink.Bytes.Num(), 4);
	TestEqual(TEXT("the DECLARED content length is recorded, not the "
		"delivered byte count"),
		Sink.Response.BodyLength, static_cast<uint64>(8));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecDescribeBeforeFetch,
	"WoT.Update.Adapters.Streaming.ProgressForwarding",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecDescribeBeforeFetch::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	uint64 Reported = 0;
	FWoTGameReleaseDownload Reporting(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		[&Reported](uint64 CompletedBytes) { Reported = CompletedBytes; });
	Reporting.ReportProgress(777);
	TestEqual(TEXT("core progress is forwarded unchanged"),
		Reported, static_cast<uint64>(777));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecMetadataUsesAnnouncementCache,
	"WoT.Update.Adapters.FetchSignedMetadata.UsesAnnouncementCache",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecMetadataUsesAnnouncementCache::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "keyring"),
		WoTGameUpdateTransportsSpecStep(200, "keyring-signature"),
		WoTGameUpdateTransportsSpecStep(200, "manifest"),
		WoTGameUpdateTransportsSpecStep(200, "manifest-signature"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("cached-announcement"),
		WoTGameUpdateTransportsSpecBytes("cached-announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());

	FWoTReleaseStagingMetadata Metadata;
	FWoTBootstrapStatus Status =
		Download.FetchSignedMetadata(Metadata);

	TestFalse(TEXT("the signed metadata fetch succeeds"), Status.IsError());
	TestTrue(TEXT("the constructor-cached announcement is returned"),
		Metadata.AnnouncementBytes
			== WoTGameUpdateTransportsSpecBytes("cached-announcement"));
	TestTrue(TEXT("the constructor-cached announcement signature is returned"),
		Metadata.AnnouncementSignature
			== WoTGameUpdateTransportsSpecBytes(
				"cached-announcement-signature"));
	TestTrue(TEXT("the key ring is fetched"),
		Metadata.KeyRingBytes
			== WoTGameUpdateTransportsSpecBytes("keyring"));
	TestTrue(TEXT("the key-ring signature is fetched"),
		Metadata.KeyRingSignature
			== WoTGameUpdateTransportsSpecBytes("keyring-signature"));
	TestTrue(TEXT("the release manifest is fetched"),
		Metadata.ReleaseManifestBytes
			== WoTGameUpdateTransportsSpecBytes("manifest"));
	TestTrue(TEXT("the release-manifest signature is fetched"),
		Metadata.ReleaseManifestSignature
			== WoTGameUpdateTransportsSpecBytes("manifest-signature"));

	TestEqual(TEXT("only key-ring and manifest pair hops are issued"),
		Hops.Requests.Num(), 4);
	const FString ExpectedUrls[] = {
		FString(WoTGameUpdateTransportsSpecKeyRingUrl),
		FString(WoTGameUpdateTransportsSpecKeyRingUrl) + TEXT(".sig"),
		FString(WoTGameUpdateTransportsSpecManifestUrl),
		FString(WoTGameUpdateTransportsSpecManifestUrl) + TEXT(".sig"),
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedUrls); ++Index)
	{
		TestEqual(
			*FString::Printf(TEXT("metadata hop %d is exact"), Index + 1),
			Hops.Requests[Index].Url, ExpectedUrls[Index]);
		TestNotEqual(
			*FString::Printf(TEXT("metadata hop %d is not an announcement hop"),
				Index + 1),
			Hops.Requests[Index].Url,
			FString(WoTGameUpdateTransportsSpecAnnouncementUrl));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecMetadataSecondCallCached,
	"WoT.Update.Adapters.FetchSignedMetadata.SecondCallCached",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecMetadataSecondCallCached::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "keyring"),
		WoTGameUpdateTransportsSpecStep(200, "keyring-signature"),
		WoTGameUpdateTransportsSpecStep(200, "manifest"),
		WoTGameUpdateTransportsSpecStep(200, "manifest-signature"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());

	FWoTReleaseStagingMetadata First;
	FWoTReleaseStagingMetadata Second;
	FWoTBootstrapStatus FirstStatus =
		Download.FetchSignedMetadata(First);
	const int32 RequestsAfterFirst = Hops.Requests.Num();
	FWoTBootstrapStatus SecondStatus =
		Download.FetchSignedMetadata(Second);

	TestFalse(TEXT("the first metadata call succeeds"),
		FirstStatus.IsError());
	TestFalse(TEXT("the second metadata call succeeds"),
		SecondStatus.IsError());
	TestEqual(TEXT("the first call issued four hops"),
		RequestsAfterFirst, 4);
	TestEqual(TEXT("the second call issued zero additional hops"),
		Hops.Requests.Num(), RequestsAfterFirst);
	TestTrue(TEXT("the cached key ring is returned"),
		Second.KeyRingBytes == First.KeyRingBytes);
	TestTrue(TEXT("the cached manifest is returned"),
		Second.ReleaseManifestBytes == First.ReleaseManifestBytes);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecMetadataRouteRefusalAtomic,
	"WoT.Update.Adapters.FetchSignedMetadata.RouteRefusalIsAtomic",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecMetadataRouteRefusalAtomic::RunTest(
	const FString&)
{
	{
		FWoTAnnouncementV1 Announcement =
			WoTGameUpdateTransportsSpecAnnouncement();
		Announcement.KeyRing.Url =
			TEXT("https://example.com/keyring.json");
		FWoTGameUpdateTransportsSpecExecutor Hops({});
		FWoTGameUpdateTransportsSpecClock Clock({0.0});
		FWoTGameReleaseDownload Download(Hops, Announcement,
			WoTGameUpdateTransportsSpecBytes("announcement"),
			WoTGameUpdateTransportsSpecBytes("announcement-signature"),
			WoTGameUpdateTransportsSpecDeadline,
			WoTGameUpdateTransportsSpecNow(Clock),
			TFunction<void(uint64)>());

		FWoTReleaseStagingMetadata Metadata;
		FWoTBootstrapStatus Status =
			Download.FetchSignedMetadata(Metadata);
		TestTrue(TEXT("a bad key-ring host is refused"), Status.IsError());
		TestTrue(TEXT("a key-ring refusal leaves all metadata untouched"),
			WoTGameUpdateTransportsSpecMetadataIsEmpty(Metadata));
		TestEqual(TEXT("a bad key-ring URL issues no hop"),
			Hops.Requests.Num(), 0);
	}

	{
		FWoTAnnouncementV1 Announcement =
			WoTGameUpdateTransportsSpecAnnouncement();
		Announcement.Manifest.Url =
			TEXT("https://example.com/manifest.json");
		FWoTGameUpdateTransportsSpecExecutor Hops({
			WoTGameUpdateTransportsSpecStep(200, "keyring"),
			WoTGameUpdateTransportsSpecStep(200, "keyring-signature"),
		});
		FWoTGameUpdateTransportsSpecClock Clock({0.0});
		FWoTGameReleaseDownload Download(Hops, Announcement,
			WoTGameUpdateTransportsSpecBytes("announcement"),
			WoTGameUpdateTransportsSpecBytes("announcement-signature"),
			WoTGameUpdateTransportsSpecDeadline,
			WoTGameUpdateTransportsSpecNow(Clock),
			TFunction<void(uint64)>());

		FWoTReleaseStagingMetadata Metadata;
		FWoTBootstrapStatus Status =
			Download.FetchSignedMetadata(Metadata);
		TestTrue(TEXT("a bad manifest host is refused"), Status.IsError());
		TestTrue(TEXT("a manifest refusal leaves all metadata untouched"),
			WoTGameUpdateTransportsSpecMetadataIsEmpty(Metadata));
		TestEqual(TEXT("only the valid key-ring pair was fetched"),
			Hops.Requests.Num(), 2);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecRangeIgnoredFullBody,
	"WoT.Update.Adapters.FetchRange.RangeIgnoredAcceptsFullSignedBody",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecRangeIgnoredFullBody::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(200, "0123456789"),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 10);

	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Fetched = Download.Fetch(Sink);
	TestFalse(TEXT("a full 200 that ignores Range is accepted"),
		Fetched.IsError());
	TestTrue(TEXT("the complete signed-size body reaches resume policy"),
		Sink.Bytes == WoTGameUpdateTransportsSpecBytes("0123456789"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecMultiChunkProgress,
	"WoT.Update.Adapters.FetchRange.MultiChunkProgress",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecMultiChunkProgress::RunTest(
	const FString&)
{
	FWoTGameUpdateTransportsSpecScriptStep Step =
		WoTGameUpdateTransportsSpecStep(206, "abcdef",
			TEXT("bytes 4-9/10"));
	Step.StreamChunks = {
		WoTGameUpdateTransportsSpecBytes("ab"),
		WoTGameUpdateTransportsSpecBytes("cde"),
		WoTGameUpdateTransportsSpecBytes("f")};
	FWoTGameUpdateTransportsSpecExecutor Hops({MoveTemp(Step)});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		TFunction<void(uint64)>());
	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 10);
	FWoTGameUpdateTransportsSpecSink Sink;
	FWoTBootstrapStatus Fetched = Download.Fetch(Sink);

	TestFalse(TEXT("the multi-chunk response succeeds"), Fetched.IsError());
	TestEqual(TEXT("each transport chunk reaches the core sink"),
		Sink.ChunkSizes.Num(), 3);
	const int32 Expected[] = {2, 3, 1};
	for (int32 Index = 0; Index < Sink.ChunkSizes.Num(); ++Index)
	{
		TestEqual(TEXT("transport chunk boundaries are preserved"),
			Sink.ChunkSizes[Index], Expected[Index]);
	}
	TestTrue(TEXT("all streamed bytes reach the core sink"),
		Sink.Bytes == WoTGameUpdateTransportsSpecBytes("abcdef"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameUpdateTransportsSpecProgress,
	"WoT.Update.Adapters.FetchRange.ProgressOnlyOnSuccess",
	WoTGameUpdateTransportsSpecFlags)

bool FWoTGameUpdateTransportsSpecProgress::RunTest(const FString&)
{
	FWoTGameUpdateTransportsSpecExecutor Hops({
		WoTGameUpdateTransportsSpecStep(206, "seven"),
		WoTGameUpdateTransportsSpecTransportFailure(),
	});
	FWoTGameUpdateTransportsSpecClock Clock({0.0});
	TArray<uint64> ProgressRecords;
	FWoTGameReleaseDownload Download(Hops,
		WoTGameUpdateTransportsSpecAnnouncement(),
		WoTGameUpdateTransportsSpecBytes("announcement"),
		WoTGameUpdateTransportsSpecBytes("announcement-signature"),
		WoTGameUpdateTransportsSpecDeadline,
		WoTGameUpdateTransportsSpecNow(Clock),
		[&ProgressRecords](uint64 CompletedBytes)
		{
			ProgressRecords.Add(CompletedBytes);
		});

	FWoTGameUpdateTransportsSpecSink BeforeSink;
	FWoTBootstrapStatus BeforeSource = Download.Fetch(BeforeSink);
	TestTrue(TEXT("the pre-source call is refused"),
		BeforeSource.IsError());
	TestEqual(TEXT("a pre-source refusal reports no progress"),
		ProgressRecords.Num(), 0);

	Download.SetVerifiedArchiveSource(
		WoTGameUpdateTransportsSpecArchiveUrl, 100);
	FWoTGameUpdateTransportsSpecSink SuccessSink;
	FWoTBootstrapStatus Successful = Download.Fetch(SuccessSink);
	TestFalse(TEXT("the scripted range succeeds"), Successful.IsError());
	Download.ReportProgress(12);
	TestEqual(TEXT("one successful call reports one progress record"),
		ProgressRecords.Num(), 1);
	if (ProgressRecords.Num() == 1)
	{
		TestEqual(TEXT("progress reports the core-accepted prefix"),
			ProgressRecords[0], static_cast<uint64>(12));
	}

	FWoTGameUpdateTransportsSpecSink FailureSink;
	FWoTBootstrapStatus Failed = Download.Fetch(FailureSink);
	TestTrue(TEXT("the scripted transport failure is refused"),
		Failed.IsError());
	TestEqual(TEXT("a transport refusal reports no additional progress"),
		ProgressRecords.Num(), 1);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
