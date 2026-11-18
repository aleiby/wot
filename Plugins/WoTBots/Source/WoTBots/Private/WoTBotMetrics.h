#pragma once
#include "CoreMinimal.h"

namespace WoTBotMetrics
{
#if WITH_AUTOMATION_TESTS
inline bool bEnabled = false;
inline double Seconds = 0;
struct FScope
{
	const double Start = bEnabled ? FPlatformTime::Seconds() : 0;
	~FScope() { if (bEnabled) Seconds += FPlatformTime::Seconds() - Start; }
};
#else
struct FScope {};
#endif
}

// A timing scope that exists only in automation builds. Shipping has no
// local at all, so MSVC's unreferenced-local check (an error there) can't fire.
#if WITH_AUTOMATION_TESTS
#define WOT_BOT_METRICS_SCOPE() WoTBotMetrics::FScope UE_JOIN(WoTBotMetricsScope, __LINE__)
#else
#define WOT_BOT_METRICS_SCOPE()
#endif
