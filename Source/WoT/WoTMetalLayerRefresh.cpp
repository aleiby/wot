#include "WoTMetalLayerRefresh.h"

#include "Math/UnrealMathUtility.h"

FWoTMetalLayerRefreshPolicy::FWoTMetalLayerRefreshPolicy(
	const FWoTMetalLayerRefreshTuning& InTuning)
	: Tuning(InTuning)
	, CurrentCooldown(InTuning.StallCooldownSeconds)
{
}

EWoTMetalLayerRefreshReason FWoTMetalLayerRefreshPolicy::Tick(
	double NowSeconds, double FrameSeconds, FIntPoint ViewportSize, bool bStallWatchAllowed)
{
	if (ViewportSize.X > 0 && ViewportSize.Y > 0)
	{
		if (LastViewportSize.X > 0 && ViewportSize != LastViewportSize)
		{
			bResizePending = true;
			LastResizeSeconds = NowSeconds;
		}
		LastViewportSize = ViewportSize;
	}

	const double SafeFrameSeconds = FMath::IsFinite(FrameSeconds) && FrameSeconds >= 0.0
		? FrameSeconds : 0.0;
	if (SafeFrameSeconds < Tuning.HealthyFrameSeconds)
	{
		HealthyElapsedSeconds += SafeFrameSeconds;
		if (HealthyElapsedSeconds >= Tuning.HealthySeconds)
		{
			CurrentCooldown = Tuning.StallCooldownSeconds;
			HealthyElapsedSeconds = Tuning.HealthySeconds;
		}
	}
	else
	{
		HealthyElapsedSeconds = 0.0;
	}

	if (bStallWatchAllowed && SafeFrameSeconds >= Tuning.StallFrameSeconds)
	{
		if (SlowFrameCount < Tuning.StallFrameCount)
		{
			++SlowFrameCount;
		}
	}
	else
	{
		SlowFrameCount = 0;
	}

	if (IsRequestPending() || NowSeconds < RetryAfterSeconds)
	{
		return EWoTMetalLayerRefreshReason::None;
	}

	if (bResizePending)
	{
		// A moving viewport postpones stall recovery too, so one settled refresh suffices.
		if (NowSeconds - LastResizeSeconds < Tuning.SettleSeconds)
		{
			return EWoTMetalLayerRefreshReason::None;
		}
		PendingReason = EWoTMetalLayerRefreshReason::ResizeSettled;
	}
	else if (bStallWatchAllowed && NowSeconds >= NextStallSeconds
		&& (bStallRetryPending || SlowFrameCount >= Tuning.StallFrameCount))
	{
		PendingReason = EWoTMetalLayerRefreshReason::FrameStall;
	}

	return PendingReason;
}

void FWoTMetalLayerRefreshPolicy::OnResult(
	double NowSeconds, EWoTMetalLayerRefreshResult Result)
{
	if (!IsRequestPending())
	{
		return;
	}

	const EWoTMetalLayerRefreshReason AnsweredReason = PendingReason;
	PendingReason = EWoTMetalLayerRefreshReason::None;
	if (Result == EWoTMetalLayerRefreshResult::Deferred)
	{
		RetryAfterSeconds = NowSeconds + Tuning.RetrySeconds;
		bStallRetryPending = AnsweredReason == EWoTMetalLayerRefreshReason::FrameStall;
		return;
	}

	bResizePending = false;
	bStallRetryPending = false;
	SlowFrameCount = 0;
	RetryAfterSeconds = 0.0;
	if (AnsweredReason == EWoTMetalLayerRefreshReason::FrameStall)
	{
		NextStallSeconds = NowSeconds + CurrentCooldown;
		CurrentCooldown = FMath::Min(CurrentCooldown * 2.0, Tuning.StallCooldownMaxSeconds);
	}
}

bool FWoTMetalLayerRefreshPolicy::IsRequestPending() const
{
	return PendingReason != EWoTMetalLayerRefreshReason::None;
}
