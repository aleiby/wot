// Test-only update overrides shared by the game, helper, and native transports.
#pragma once

#include "CoreMinimal.h"

/** Accept only an explicit IPv4-loopback HTTP URL with a port and non-empty path. */
WOTBOOTSTRAPCORE_API bool WoTParseStrictTestLoopbackUrl(
	FStringView Candidate, FString* OutUrl = nullptr);

/** Read and strictly validate one loopback URL override from the environment. */
WOTBOOTSTRAPCORE_API bool WoTTryGetStrictTestLoopbackOverride(
	const TCHAR* EnvironmentName, FString& OutUrl);

/** The single trust gate used by both game-side and helper-side test identities. */
WOTBOOTSTRAPCORE_API bool WoTIsTestLoopbackOverrideActive();

#ifndef WOT_BOOTSTRAP_TEST_IDENTITIES
#define WOT_BOOTSTRAP_TEST_IDENTITIES 0
#endif

#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
/** Test trust is unconditional for automation, but runtime Test builds require the gate. */
WOTBOOTSTRAPCORE_API bool WoTShouldAcceptTestUpdateIdentities();
WOTBOOTSTRAPCORE_API const TCHAR* WoTTestAnnouncementPublicKeyHex();
WOTBOOTSTRAPCORE_API const TCHAR* WoTTestReleaseKeyRingRootHex();
#endif
