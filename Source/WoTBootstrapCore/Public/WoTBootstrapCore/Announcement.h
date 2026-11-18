// The frozen announcement-v1 envelope.
//
// This is the first thing a client reads from the network and the last format that
// may ever change: a v1 build must be able to parse every future v1 announcement,
// or an obsolete build could never learn that it is obsolete. So the field set is
// closed and permanent, and everything that varies between releases lives behind
// the two references it carries.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

inline constexpr TCHAR WoTAnnouncementV1Format[] = TEXT("wot-announcement-v1");

/**
 * Authenticate and parse an announcement.
 *
 * Verification order matters: the signature is checked over the exact bytes
 * BEFORE any field is believed, then platform and domain are compared against what
 * this binary was compiled with. A signed announcement for another platform or
 * another domain is a valid signature over something that is not ours, so it is
 * refused rather than adapted.
 *
 * `Platform` and `CompiledDomain` are passed in rather than read from a global so
 * that the check is visible at the call site and testable without a build variant.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTAnnouncementV1> VerifyAnnouncementV1(
	TConstArrayView<uint8> Json, const FWoTEd25519Signature& Signature,
	FStringView Platform, FStringView CompiledDomain,
	const FWoTEd25519PublicKey& AnnouncementKey);
