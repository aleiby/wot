#pragma once

#include "CoreTypes.h"
#include "Math/IntPoint.h"

enum class EWoTMetalLayerRefreshReason : uint8
{
	None,
	ResizeSettled,
	FrameStall
};

enum class EWoTMetalLayerRefreshResult : uint8
{
	Refreshed,
	Deferred,
	Unavailable
};

struct FWoTMetalLayerRefreshTuning
{
	double SettleSeconds = 0.25;
	double RetrySeconds = 0.25;
	double StallFrameSeconds = 0.25;
	int32 StallFrameCount = 4;
	double StallCooldownSeconds = 5.0;
	double StallCooldownMaxSeconds = 60.0;
	double HealthyFrameSeconds = 0.1;
	double HealthySeconds = 2.0;
};

// Time and viewport observations are supplied by the caller, with no platform dependencies.
class FWoTMetalLayerRefreshPolicy
{
public:
	explicit FWoTMetalLayerRefreshPolicy(const FWoTMetalLayerRefreshTuning& InTuning = {});

	// Once per frame. A returned reason stays in flight until OnResult is called.
	EWoTMetalLayerRefreshReason Tick(double NowSeconds, double FrameSeconds,
		FIntPoint ViewportSize, bool bStallWatchAllowed);
	void OnResult(double NowSeconds, EWoTMetalLayerRefreshResult Result);
	bool IsRequestPending() const;

private:
	FWoTMetalLayerRefreshTuning Tuning;
	FIntPoint LastViewportSize = FIntPoint::ZeroValue;
	double LastResizeSeconds = 0.0;
	double RetryAfterSeconds = 0.0;
	double NextStallSeconds = 0.0;
	double CurrentCooldown;
	double HealthyElapsedSeconds = 0.0;
	int32 SlowFrameCount = 0;
	bool bResizePending = false;
	bool bStallRetryPending = false;
	EWoTMetalLayerRefreshReason PendingReason = EWoTMetalLayerRefreshReason::None;
};

#if PLATFORM_MAC && !WITH_MULTI_VIEW_SLATE_WINDOW_SUPPORT
void WoTInstallMetalLayerRefresh();
void WoTShutdownMetalLayerRefresh();
int32 WoTGetMetalLayerRefreshCount();
#else
inline void WoTInstallMetalLayerRefresh() {}
inline void WoTShutdownMetalLayerRefresh() {}
inline int32 WoTGetMetalLayerRefreshCount() { return 0; }
#endif
