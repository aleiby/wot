#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTParticleSprayer.h"

namespace WoTRuntimeParticleProjectionLimits
{
// The shipped .uc maximum is Volume=5000 (the BloodBurst/BloodSpray classes).
// 8192 is the next power-of-two ceiling and limits a real 30 Hz prime tick to
// 274 emissions (and a 120 Hz runtime substep to 69), while keeping the float
// timer decrement well above the ULP at those timer magnitudes.
inline constexpr float MaximumVolume = 8192.0f;
}

struct WOT_API FWoTRuntimeParticleSprayerConfiguration
{
	FTransform Transform = FTransform::Identity;
	TArray<FWoTParticleTemplate> Templates;
	TArray<FString> TextureIdentities;
	float Spread = 45.0f;
	float Volume = 10.0f;
	float MinVolume = 0.0f;
	FVector Gravity = FVector::ZeroVector;
	float PrimeCount = 0.0f;
	float VolumeScalePct = 0.5f;
	float VisibilityRadius = 1600.0f;
	float VisibilityHeight = 1600.0f;
	uint8 Style = 1; // Engine.Actor ERenderStyle: STY_Normal.
	FName InitialState;
	float TimerDuration = 0.15f;
	bool bInitiallyOn = true;
	bool bOn = true;
	bool bLOSClip = false;
	bool bLinearDistribution = false;
};

/** Canonical decoder for the retained 50-byte UE1 Particle struct. */
WOT_API bool WoTRuntimeReadParticleTemplate(
	const FWoTRuntimeWorldProperty& Property, FWoTParticleTemplate& Out);

/** Shared entry/gameplay record-to-sprayer configuration path. */
WOT_API bool WoTRuntimeReadParticleSprayerConfiguration(
	TFunctionRef<const FWoTRuntimeWorldProperty*(FStringView, uint32)> Find,
	FWoTRuntimeParticleSprayerConfiguration& Out);

WOT_API void WoTRuntimeApplyParticleSprayerConfiguration(
	const FWoTRuntimeParticleSprayerConfiguration& Source,
	AWoTParticleSprayer& Target);
