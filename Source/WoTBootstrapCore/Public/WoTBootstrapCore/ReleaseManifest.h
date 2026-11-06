// The inner, release-key-signed manifest.
//
// The announcement says a release exists; this says what it IS. Only a valid key
// ring plus a valid manifest signature authorize installing code, which is why the
// announcement key deliberately cannot sign one.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/KeyRing.h"

inline constexpr TCHAR WoTReleaseManifestV2Format[] = TEXT("wot-release-manifest-v2");

/**
 * Authenticate and parse an inner release manifest.
 *
 * The manifest must agree with the announcement that already authenticated: same
 * domain, same platform, same release sequence, same signing key id. A manifest
 * that disagrees is not a newer release, it is a substituted one — so a mismatch
 * is refused rather than preferred in either direction.
 *
 * The signing key is selected from `Ring` by the announcement's key id and the
 * release sequence, so a retired key cannot sign a later release even if its bytes
 * are still present. Only ManifestVersion 2 is accepted; release-manifest-v1
 * survives solely as a frozen developer oracle and is never accepted here.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTReleaseManifestV2> VerifyReleaseManifestV2(
	TConstArrayView<uint8> Json, const FWoTEd25519Signature& Signature,
	const FWoTAnnouncementV1& Announcement, const FWoTKeyRing& Ring);

/**
 * True when this build implements every protocol the manifest demands.
 *
 * Split out from verification because "correctly signed but too new for me" is a
 * different outcome than "not correctly signed": the first sends the user to a
 * manual full-package install, the second is a metadata failure.
 */
WOTBOOTSTRAPCORE_API bool SupportsManifestProtocols(const FWoTReleaseManifestV2& Manifest);
