#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTLightWave.generated.h"

class UMaterialParameterCollection;

USTRUCT(BlueprintType)
struct FWoTLightWaveGroup
{
	GENERATED_BODY()

	// UE1 ELightType byte. Existing serialized groups predate this field and
	// default to Pulse; Pulse/SubtlePulse share the normalized sine scalar.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	uint8 Type = 2;

	// UE1 LightPeriod. Retail divides by Max(Period, 1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	int32 Period = 64;

	// UE1 LightPhase (0-255), converted to rotator units by Phase * 256.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	int32 Phase = 0;
};

// Exact values transported by MPC_WoTLightWave. The defaults are deliberately
// the frozen pose: all centered response coefficients are zero.
struct FWoTLightWaveParameterValues
{
	float Waves[4] = {0.5f, 0.5f, 0.5f, 0.5f};
	float NoiseFrame = 0.0f;
	float Freeze = 1.0f;
};

// One map-level driver for all centered lightmap responses. Means/amplitudes
// are baked into each response atlas. The shared runtime scalar is normalized
// to [0,1], and the material recovers z=2s-1. Blink therefore transports the
// exact integer parity sample while Pulse/SubtlePulse transport their shared
// normalized retail sine.
UCLASS()
class AWoTLightWave : public AActor
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxWaveformGroups = 4;
	static constexpr uint32 NoiseFrameMask = 0x00ffffffu;

	AWoTLightWave();

	// Manifest order is material Wave0..Wave3 order. Empty is valid for a
	// shimmer-only map; unused scalar slots remain exactly 0.5.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FWoTLightWaveGroup> Groups;

	// Wave-1 compatibility fields. Shipped pre-Wave-6 maps serialize these
	// exact property names and leave Groups empty; remove only when the new MPC,
	// masters, responses, and maps land atomically.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT",
		meta = (DeprecatedProperty,
			DeprecationMessage = "Use the ordered Groups manifest instead."))
	float Period = 64.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT",
		meta = (DeprecatedProperty,
			DeprecationMessage = "Use the ordered Groups manifest instead."))
	float Phase = 0.0f;

	static float EvaluateWaveScalar(
		float TimeSeconds, float Period, float Phase);
	static FWoTLightWaveParameterValues EvaluateParameters(
		float TimeSeconds, bool bFrozen,
		const TArray<FWoTLightWaveGroup>& WaveGroups,
		uint32& InOutNoiseFrame);
	static bool IsFreezeEnabled();

	virtual void Tick(float DeltaSeconds) override;

private:
	uint32 NoiseFrame = 0;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> Collection;
};
