// Release key rings: parsing, key selection, and epoch policy.
//
// A ring is data, not authority. Parsing one proves only that it is well formed;
// it becomes trusted when a caller verifies its signature under
// EWoTSignatureContext::ReleaseKeyRingV1 against an already trusted key, and
// checks its epoch against the epoch already installed. Both steps are the
// caller's, so no code path can accidentally trust a ring merely by reading it.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/CanonicalJson.h"

/** The release-signing ring: keys that sign release manifests. */
inline constexpr TCHAR WoTReleaseKeyRingFormat[] = TEXT("wot-release-key-ring-v1");

/**
 * The offline-root ring: keys that sign RELEASE RINGS, and nothing else.
 *
 * Structurally identical to a release ring and parsed by the same code, but a
 * distinct format string, so a release ring can never be presented where a root
 * ring is expected. The roles differ in blast radius: a release key signs one
 * release, a root key decides which release keys exist at all.
 */
inline constexpr TCHAR WoTOfflineRootRingFormat[] = TEXT("wot-offline-root-ring-v1");

/**
 * Parse a key ring from an already-parsed canonical document.
 *
 * The schema is closed: exactly `format`, `epoch`, `keys`, `revoked_key_ids`, and
 * each key exactly `key_id`, `public_key`, `first_sequence`, `last_sequence`. An
 * unexpected field is UnknownField rather than ignored — a field this build does
 * not understand may be the one carrying a restriction.
 *
 * `ExpectedFormat` is required at the call site rather than inferred from the
 * document: a caller that wanted a root ring must not silently accept a release
 * ring just because the bytes parse.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTKeyRing> ParseKeyRing(
	const FWoTCanonicalJsonValue& Document,
	FStringView ExpectedFormat = WoTReleaseKeyRingFormat);

/**
 * Choose the key that may sign a given release sequence.
 *
 * Refuses an unknown id, a revoked id, and a sequence outside the key's
 * [FirstSequence, LastSequence] window. There is no fallback to "any key in the
 * ring": a signature is trusted for the exact key the artifact names.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTReleaseKey> SelectReleaseKey(
	const FWoTKeyRing& Ring, FStringView KeyId, uint64 ReleaseSequence);

/**
 * Reject a ring whose epoch is older than the one already installed.
 *
 * Separate from parsing and from signature verification because the comparison
 * needs durable local state, which this module deliberately cannot read.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus RequireRingEpochAtLeast(
	const FWoTKeyRing& Ring, uint64 MinimumEpoch);
