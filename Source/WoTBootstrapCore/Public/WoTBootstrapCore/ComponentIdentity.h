// Separately identified components inside a release.
//
// The candidate root executable is bound as its own component rather than as a
// tree entry, because it is promoted by a different transaction than the tree it
// ships beside: the tree is swapped as a directory, the root image is replaced in
// place by the service.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/CanonicalJson.h"

inline constexpr TCHAR WoTComponentIdentityV1Format[] = TEXT("wot-component-identity-v1");

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTComponentIdentity> ParseComponentIdentityV1(
	const FWoTCanonicalJsonValue& Document);

/**
 * Compare a component against what a held handle actually reports.
 *
 * Takes an identity from an OPEN handle, not a path: checking the path would
 * re-open a name that may no longer refer to the bytes just verified.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus MatchComponentIdentity(
	const FWoTComponentIdentity& Expected, const FWoTNativeFileIdentity& Actual,
	const FWoTSha256& ActualContentSha256);
