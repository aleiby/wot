// Native, one-hop HTTP execution. Redirect and allow-list policy lives above.
#pragma once

#ifndef WOT_BOOTSTRAP_TEST_ENDPOINTS
	#define WOT_BOOTSTRAP_TEST_ENDPOINTS 0
#endif

#include "WoTUpdateTransportPolicy.h"

/** The platform executor: WinHttp on Win64, NSURLSession on Mac. Synchronous
 * on the calling (worker) thread; safe off the game thread. */
WOTUPDATETRANSPORTS_API TUniquePtr<IWoTHttpHopExecutor> WoTCreatePlatformHttpHopExecutor();

#if PLATFORM_MAC && WITH_AUTOMATION_TESTS
/** Mac executor seam (B10): suppress the cancelled task's completion signal and
 * set the post-cancel grace. Restore with (false, 10.0). */
WOTUPDATETRANSPORTS_API void WoTPlatformHttpHopMacSetCancelStallForTesting(
	bool bSuppressCompletion, double GraceSeconds);
#endif

#if PLATFORM_WINDOWS && WITH_AUTOMATION_TESTS
/** Windows executor seam: make WINHTTP_OPTION_CONNECT_RETRIES fail as Wine's
 * WinHttp does (ERROR_WINHTTP_INVALID_OPTION). Restore with false. */
WOTUPDATETRANSPORTS_API void WoTPlatformHttpHopWindowsFailConnectRetriesForTesting(bool bFail);
#endif

#if WOT_BOOTSTRAP_TEST_ENDPOINTS
/** Test-build-only inspection of the native release-base rewrite allowlist. */
WOTUPDATETRANSPORTS_API bool WoTPlatformHttpHopRewriteReleaseUrlForTesting(
	const FString& OriginalUrl, FString& OutUrl);
#endif
