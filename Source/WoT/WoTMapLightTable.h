#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTMapLightTable.generated.h"

USTRUCT(BlueprintType)
struct FWoTLightRec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector Pos = FVector::ZeroVector;

	// Reciprocal UE world-light radius (1/R in Unreal units).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	float InvRadius = 0.0f;

	// Exact WorldLightRadius used by the retail distance/priority test. Kept
	// separately because reconstructing it from the shader reciprocal changes
	// boundary rounding.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	float Radius = 0.0f;

	// Display-space FloatColor, including brightness, waveform mean, and level brightness.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FLinearColor Color = FLinearColor::Black;

	// Index into this table's ordered WaveMeans/WaveAmplitudes vectors and the
	// map driver's Wave0..Wave3 MPC scalars. INDEX_NONE is a steady light.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	int32 WaveGroup = INDEX_NONE;

	// Spotlight cone sine; negative values mark omni lights.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	float Sine = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector Dir = FVector::ZeroVector;

	// Raw UE1 LightBrightness byte. Retail selection priority uses this
	// property rather than the display-ready Color magnitude above.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	uint8 Brightness = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	bool bSpecialLit = false;

	// False for zero-brightness lights or lights whose resting LightType is
	// LT_None (including targets folded off by Lightning.uc). They still
	// participate in AddConsider's threshold and visible-count logic, but
	// AddLight emits no material row.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	bool bEmits = true;
};

// Runtime point-to-zone ambient record. Zone is the compiled BSP zone number.
// Bounds remain serialized for compatibility with old tables that predate the
// exact BSP tree and use smallest-containing-AABB lookup.
USTRUCT(BlueprintType)
struct FWoTZoneAmbient
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	int32 Zone = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector BoundsMax = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FLinearColor Color = FLinearColor::Black;

	// LevelInfo/zone-zero fallback when no bounded record contains the point.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	bool bDefault = false;
};

UCLASS()
class UWoTMapLightTable : public UDataAsset
{
	GENERATED_BODY()

public:
	// LevelInfo.Brightness, shared by static table colors and runtime actor
	// lights. UE1 defaults to 1.0.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	float LevelBrightness = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<FWoTLightRec> Lights;

	// Parallel to the ordered waveform groups used by AWoTLightWave. Unused
	// components are (mean=1, amplitude=0), making every invalid/steady lookup
	// an identity. The material evaluates 1 + amplitude*(2*Wave-1)/mean.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector4 WaveMeans = FVector4(1.0, 1.0, 1.0, 1.0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	FVector4 WaveAmplitudes = FVector4(0.0, 0.0, 0.0, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<FWoTZoneAmbient> ZoneAmbients;

	// Exact UE1 point-to-zone BSP. Node zero is the root. A side value >= 0
	// is another node index; a negative value is leaf zone -(value + 1).
	// Point-on-plane selects front, matching the retail classifier.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<FVector4> ZoneBspPlanes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> ZoneBspFront;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> ZoneBspBack;

	// Zone-number-indexed retail light-count limits. This stationary proxy uses
	// the actor-zone MaxLightCount (clamped to 16); retail's live viewer-zone LOD
	// interpolation has no static-bake viewer. Min remains as provenance.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> ZoneMinLightCounts;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> ZoneMaxLightCounts;

	// Per-node [back, front] compiled leaf indices, parallel to ZoneBspPlanes.
	// INDEX_NONE makes point lookup fake-lit (no permeating run). LineCheck does
	// not inspect leaf ids: node CSG state and the model's root-outside bit drive
	// its terminal outside/solid state.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> LeafBspLeaves;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<uint8> LeafBspCsg;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	bool bBspRootOutside = false;

	// LevelInfo.bCheckWalkSurfaces; UE1's LevelInfo defaults it true.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	bool bCheckWalkSurfaces = true;

	// LevelInfo.IdealPlayerCount is a localized UE1 string (for example "6-8");
	// zones.py applies UE1 Int()/C atoi coercion while exporting the map table.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	int32 IdealPlayerCount = 0;

	// Sparse Texture.Friction channel keyed by compiled world BSP node. The
	// importer writes only non-default values, in ascending node order; lookup
	// defaults to UE1 Texture's 1.0. World nodes only deliberately preserve
	// TraceTexture's LevelInfo gate, so mover faces can never become ladders.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> FrictionNodeIndices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<float> FrictionNodeValues;

	// CSR encoding of each compiled leaf's shipped NULL-terminated permeating
	// light run. Leaf N occupies [Offsets[N], Offsets[N+1]) in Indices.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> LeafLightOffsets;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT")
	TArray<int32> LeafLightIndices;
};
