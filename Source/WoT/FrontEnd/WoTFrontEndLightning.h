#pragma once

#include "CoreMinimal.h"
#include "../WoTBSPPayload.h"

WOT_API uint8 WoTEntryMergeIllumination(
	uint8 BaseRGBA7, uint8 Illumination, uint32 FixedColor);

// Base is an authenticated decoded payload: provenance, disjoint padded
// allocations and decoder work caps remain prerequisites. Failure leaves OutBGRA
// unchanged; the immutable base is never modified.
WOT_API bool WoTComposeEntryLightning(
	const FWoTBSPPayloadData& Base,
	TConstArrayView<float> Samples,
	TArray<uint8>& OutBGRA,
	FString& Error);

struct FWoTEntryStrikeSettings
{
	float MinInterval = 10, MaxInterval = 20, MinThunderDelay = 0, MaxThunderDelay = 3;
	TArray<float> LightningLengths;
};

struct FWoTEntryStrikeFrame
{
	TArray<float> Samples;
	bool bStrike = false, bThunder = false;
	float ThunderDelay = 0;
};

class WOT_API FWoTEntryStrikeState
{
public:
	// Validate/reset without drawing; first Advance performs initial scheduling.
	// Intervals must be >= .05 seconds; lengths/delays are nonnegative; <=8 lights.
	// Failed calls leave state/output unchanged (the caller's stream may advance).
	bool Initialize(const FWoTEntryStrikeSettings& Settings, FString& Error);
	bool Advance(float DeltaSeconds, TFunctionRef<float()> Random01,
		FWoTEntryStrikeFrame& Out, FString& Error);
	void Reset();
	float SecondsUntilNextStrike() const
	{
		return bScheduled ? static_cast<float>(FMath::Max(0.0, NextStrike - Time)) : 0.f;
	}

private:
	FWoTEntryStrikeSettings Settings;
	bool bInitialized = false;
	bool bScheduled = false;
	bool bHasStrike = false;
	bool bThunderPending = false;
	// Local epoch resets at a strike, preserving subsecond precision after hitches.
	double Time = 0, NextStrike = 0, StrikeTime = 0;
	float CurrentThunderDelay = 0;
};
