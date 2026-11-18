// Authenticated launch-time announcement assessment.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/Announcement.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTUpdaterCore/UpdateState.h"

enum class EWoTUpdateDecision : uint8
{
	Current,
	MustStage,
	ContinueCurrentAndLogUnavailable,
	StayBlocked,
};

/**
 * The launch-time announcement transport seam.
 *
 * One call represents exactly two logical fetches: the platform's compiled
 * announcement JSON URL and that literal URL plus `.sig`, under one shared,
 * bounded deadline. A concrete implementation must start at the exact HTTPS
 * `github.com` URL, follow at most three HTTPS redirects, and accept redirect
 * hosts only when they exactly equal `release-assets.githubusercontent.com`,
 * `objects.githubusercontent.com`, or `github-releases.githubusercontent.com`.
 * The one exception is hop 1 of GitHub's "latest" route: from
 * `/<owner>/<repo>/releases/latest/download/<asset>` to
 * `/<owner>/<repo>/releases/download/<tag>/<asset>` on github.com, with the
 * same owner, repo and asset (`WoTJudgeRedirectHop` with PreviousUrl).
 * It must preserve the redirected path/query byte-for-byte, never forward
 * credentials, never downgrade, reset the deadline, retry, select a mirror, or
 * switch either logical object. Redirect hops remain internal to the two
 * logical fetches. Implementations live with the game/tools, not this module.
 *
 * Success returns the exact response bodies together. The signature body is
 * the raw 64-byte Ed25519 signature. Failure of either logical fetch fails the
 * pair and must leave both output arrays empty.
 */
class IWoTAnnouncementTransport
{
public:
	virtual ~IWoTAnnouncementTransport() = default;
	virtual FWoTBootstrapStatus FetchPair(
		TArray<uint8>& OutAnnouncement, TArray<uint8>& OutSignature) = 0;
};

struct FWoTUpdateAssessment
{
	EWoTUpdateDecision Decision = EWoTUpdateDecision::StayBlocked;
	TOptional<FWoTAnnouncementV1> Announcement;
	FString NotifyOnlyReason;
	/** Underlying error retained for diagnostic heartbeats; never drives policy. */
	FString NotifyOnlyDetail;
};

/**
 * Fetch, authenticate, classify, and best-effort record one announcement pair.
 *
 * `TrustedRing` is the injected announcement trust ring and must contain one
 * non-revoked dedicated announcement key. Its private half is never present.
 */
WOTUPDATERCORE_API TWoTBootstrapResult<FWoTUpdateAssessment> AssessAnnouncement(
	IWoTAnnouncementTransport& Transport, const FWoTKeyRing& TrustedRing,
	FWoTUpdateStateRecord& State, const FWoTUpdaterStateRoot& Root,
	uint64 RunningReleaseSequence, bool bRecordState = true);
