// Pure, socket-free HTTPS route policy and logical-fetch engine.
#pragma once

#ifndef WOT_BOOTSTRAP_TEST_ENDPOINTS
	#define WOT_BOOTSTRAP_TEST_ENDPOINTS 0
#endif

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"

struct FWoTHttpHopResponse;

class IWoTHttpBodySink
{
public:
	virtual ~IWoTHttpBodySink() = default;
	// Override only for bounded consumers that must stream non-success bodies.
	virtual bool ShouldStreamResponseBody(int32 Status) const
	{
		return Status == 200;
	}
	// Called exactly once for an opted-in streamed response, before the first Write.
	// HeadersOnly.Body is empty and StreamedBodyLength is zero. An error aborts
	// the hop without delivering body bytes.
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse& HeadersOnly) = 0;
	// Ordered, on the transport thread. An error aborts the hop.
	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> Bytes) = 0;
	// True only for cooperative updater shutdown. Native executors preserve
	// already-observed terminal headers for this error so core can durably resume
	// the accepted prefix. Ordinary sink failures remain fail-closed.
	virtual bool ShouldPreserveResponseOnError() const { return false; }
};

struct FWoTHttpHopRequest
{
	FString Url;                          // absolute https:// URL for this hop
	double AbsoluteDeadlineSeconds = 0.0; // FPlatformTime::Seconds() domain
	IWoTHttpBodySink* BodySink = nullptr; // sink selects which statuses it streams
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
	bool bAllowPlainHttpForLoopbackTesting = false; // test builds only
#endif
};

struct FWoTHttpHopResponse
{
	int32 Status = 0;      // 0 = transport failure (offline/DNS/TLS/timeout)
	FString Location;      // verbatim Location header if present, else empty
	// Declared Content-Length of THIS response; 0 when absent or unknown. This
	// is what core's DecideHttpBody consumes as "what the response says it will
	// deliver" and is deliberately distinct from delivered-byte counts.
	uint64 BodyLength = 0;
	// Bytes successfully delivered to BodySink for a streamed 200/206. Zero for
	// buffered responses and before OnResponseStart returns successfully.
	uint64 StreamedBodyLength = 0;
	TArray<uint8> Body;
};

/** Execute exactly one HTTP request. Implementations NEVER follow redirects,
 * NEVER send credentials, cookies, or Authorization, and honor the absolute
 * deadline. The request struct is the entire request surface. */
class IWoTHttpHopExecutor
{
public:
	virtual ~IWoTHttpHopExecutor() = default;
	virtual FWoTBootstrapStatus ExecuteHop(const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) = 0;
	// Thread-safe, non-blocking cancellation of the currently executing native
	// request. Scripted executors may keep the default no-op.
	virtual void CancelCurrentRequest() {}
};

/** True only for a well-formed https URL whose host is exactly github.com
 * (no userinfo, no port, no IP form, no trailing-dot, ASCII only). */
WOTUPDATETRANSPORTS_API bool WoTIsAllowedInitialReleaseUrl(FStringView Url);

/** Judge one Location header against the CDN allow-list. HopIndex is 1-based;
 * a fourth hop is refused before the host is even parsed. */
enum class EWoTRedirectVerdict : uint8 { Follow, Refuse };
WOTUPDATETRANSPORTS_API EWoTRedirectVerdict WoTJudgeRedirectHop(FStringView Location, int32 HopIndex);
/** As above, plus the one github.com hop GitHub's "latest" route needs: hop 1
 * from https://github.com/<owner>/<repo>/releases/latest/download/<asset> to
 * https://github.com/<owner>/<repo>/releases/download/<tag>/<asset> with the
 * same owner, repo and asset, a plain tag, and no query.
 * PreviousUrl is the URL whose response carried Location. */
WOTUPDATETRANSPORTS_API EWoTRedirectVerdict WoTJudgeRedirectHop(
	FStringView Location, int32 HopIndex, FStringView PreviousUrl);

/** Fetch one logical object: initial URL through at most three judged redirect
 * hops to a terminal response. Terminal statuses propagate; the caller decides
 * what is acceptable. Buffered failures leave OutBody empty. When BodySink is
 * supplied, successfully delivered prefix bytes remain owned by that sink even
 * if the hop is interrupted. NowSeconds is injectable for deadline specs. */
WOTUPDATETRANSPORTS_API FWoTBootstrapStatus WoTFetchReleaseObject(IWoTHttpHopExecutor& Hops,
	FStringView Url, double AbsoluteDeadlineSeconds,
	TFunction<double()> NowSeconds, TArray<uint8>& OutBody,
	FWoTHttpHopResponse& OutTerminal, IWoTHttpBodySink* BodySink = nullptr);

/** The two-logical-fetch signed-pair operation: <JsonUrl> then literally
 * <JsonUrl> + ".sig", one shared absolute deadline, no retry. Failure of either
 * fetch (or a non-200 terminal) leaves BOTH outputs empty. */
WOTUPDATETRANSPORTS_API FWoTBootstrapStatus WoTFetchSignedPair(IWoTHttpHopExecutor& Hops,
	FStringView JsonUrl, double AbsoluteDeadlineSeconds,
	TFunction<double()> NowSeconds, TArray<uint8>& OutJson,
	TArray<uint8>& OutSignature);
