#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTRainCyd.generated.h"

class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;
class UMaterialInstanceDynamic;

/** UE1 GUnlitColor for a bUnlit mesh: the rain skin's
 *  source multiplier, clamp(ScaleGlow*0.5 + AmbientGlow/256). */
WOT_API float WoTRainCydUnlitColor(float ScaleGlow, uint8 AmbientGlow);
/** Writes WoTRainCydUnlitColor into the mesh-translucent master's VECTOR
 *  MLAmbient (a scalar write of that name is a silent no-op). */
WOT_API void WoTRainCydApplyUnlitColor(UMaterialInstanceDynamic* Material,
	float ScaleGlow, uint8 AmbientGlow);
/** RainCyd.uc:90 Trace(bTraceActors=False) = TRACE_VisBlocking: only level
 *  geometry and movers may anchor a column. */
WOT_API bool WoTRainCydIsVisBlockingActor(const AActor* Actor);
/** The original renderer's RainCyd visibility gate: independent horizontal-radius and
 * vertical-height tests, with zero disabling that axis. */
WOT_API bool WoTRainCydVisibleFrom(const FVector& RainLocation,
	const FVector& ViewLocation, float VisibilityRadius, float VisibilityHeight);

#if WITH_AUTOMATION_TESTS
WOT_API void WoTSetRainCydStagedMotionObserverForTesting(
	TFunction<void(float, float)> Observer);
#endif

// RainCyd (Environment/RainCyd.uc): a stack of translucent rain-streak
// cylinders that physically FALLS at MinFallRate..MaxFallRate uu/s — the
// falling mesh IS the rain animation. The retail master traces to the ground
// (uc:90), anchors one segment height above the hit (uc:92), and spawns
// StackNum-1 clones stacked above itself (uc:94-113); each segment wraps to
// the top after passing BuryDistance below the hit (uc:130-133). Authenticated
// front-end segments preserve retail's labeled one-jump-per-Tick motion; the
// existing gameplay fallback retains its shared-offset behavior.
// The decoded segment geometry spans local z [-CylinderHeight, 0] (hangs
// below the anchor; bounds-probed 2026-07-24).
// Skin = Environment.RainDropsA (STY_Translucent, bUnlit) via the
// mesh-translucent master; importer sets the material, scale, and the
// authored per-actor overrides (StackNum/fall rates/TraceDepth/BuryDistance).
// MinRotRate/MaxRotRate spin is unported: no shipped map authors it (census
// 2026-07-24; defaults are 0 = no spin).
UCLASS()
class AWoTRainCyd : public AActor
{
	GENERATED_BODY()

public:
	AWoTRainCyd();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float CylinderHeight = 48.0f;      // unscaled segment height (RainCyd.uc)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	int32 StackNum = 5;                // segments in the column (RainCyd.uc)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float MinFallRate = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float MaxFallRate = 700.0f;

	// Ground-anchor trace reach below the authored location (RainCyd.uc:25).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float TraceDepth = 1000.0f;

	// Distance below the ground hit a segment travels before wrapping
	// (RainCyd.uc:56; negative values stop the column above the hit).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float BuryDistance = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float VisibilityRadius = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float VisibilityHeight = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	// Runtime clones of Mesh stacked above it (segments 1..StackNum-1).
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> ExtraSegments;

	/** Runtime-only authenticated topology path used when no cooked retail
	 * carrier exists. The caller retains the transient mesh/material. */
	void SetRuntimeRenderMesh(UStaticMesh* InMesh,
		TConstArrayView<UMaterialInterface*> Materials);
	/** Configure the authenticated retail mesh identity. The mesh is decoded
	 * into the runtime rain carrier on rendering roles at BeginPlay. */
	void ConfigureRuntimePresentation(FName InMeshName, FName InMeshPackage,
		const FSoftObjectPath& InSkin,
		const TMap<int32, FSoftObjectPath>& InSkinOverrides,
		uint8 InAmbientGlow, float InScaleGlow);
	/** Finish a deferred runtime bind and explicitly publish its render state. */
	void EnsureRuntimeRenderState();
	/** Front-end materialization opts into values precomputed during Stage so
	 * hidden BeginPlay is deterministic and never consumes the global RNG. */
	void SetFrontEndStagedMotion(float InYawDegrees, float InFallRate);
	/** Supplies the authenticated front-end trace point verbatim. The caller
	 * owns any legacy trace-wrapper bias before staging this value. */
	bool SetFrontEndGroundHit(FVector InHit);
	bool HasRuntimeRenderMesh() const { return RuntimeMesh != nullptr; }
	int32 RuntimeRenderMaterialCount() const;
#if WITH_AUTOMATION_TESTS
	float InitialYawForTesting() const { return InitialYawDegrees; }
	float FallRateForTesting() const { return FallRate; }
#endif

	virtual void Tick(float DeltaSeconds) override;
	/** Hide each stack segment the viewer is outside the retail limits of. */
	void CullSegmentsFrom(const FVector& ViewLocation);

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> RuntimeMesh;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> RuntimeSegments;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> RuntimeGeneratedMesh;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInterface>> RuntimeMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> RuntimeTextures;
	FName RuntimeMeshName;
	FName RuntimeMeshPackage;
	FSoftObjectPath RuntimeActorSkin;
	TMap<int32, FSoftObjectPath> RuntimeSkinOverrides;
	uint8 RuntimeAmbientGlow = 0;
	float RuntimeScaleGlow = 1.0f;
	FVector BasePosition = FVector::ZeroVector;
	FVector Down = -FVector::UpVector;
	float SegmentHeight = 48.0f;
	float FallRate = 600.0f;
	float InitialYawDegrees = 0.0f;
	float StagedFallRate = 600.0f;
	bool bUseFrontEndStagedMotion = false;
	FVector FrontEndGroundHit = FVector::ZeroVector;
	TArray<FVector> FrontEndSegmentPositions;
	bool bUseFrontEndGroundHit = false;
	float Offset = 0.0f;
};
