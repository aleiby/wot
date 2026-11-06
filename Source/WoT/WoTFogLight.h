#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTFogLight.generated.h"

// One UE1 volumetric fog light (a light with VolumeRadius/VolumeBrightness in
// a bFogZone zone). The retail renderer fogs each frame by walking the BSP
// leaves' baked volumetric light lists and drawing per-surface fog maps with
// the parabolic chord integral, screen-blended over the frame per channel
// (fog + dst*(1-fog)). The port draws one inverted sphere pair per fog light:
// a Modulate pass (dst *= 1-C, sort 100) under an Additive pass (dst += C,
// sort 101) — the same two-pass per-channel screen split the coronas use —
// with C computed per-pixel in the material (M_WoT_VolumeFog_* masters, exact
// chord integral, scene-depth clamped). Spheres render their inner faces
// (reversed culling) with depth test off so the integral survives the camera
// entering the volume.
//
// Retail gates the WHOLE volumetric pass on the VIEWER standing in a bFogZone
// zone; UWoTFogGateSubsystem mirrors that by toggling these actors' hidden
// flag per frame from the camera position vs the placed fog-zone volumes.
UCLASS()
class AWoTFogLight : public AActor
{
	GENERATED_BODY()

public:
	AWoTFogLight();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Extraction parameters (exports/fog/<Map>.fog.json), applied by the
	// importer onto the sphere materials; kept here for inspection.
	UPROPERTY(EditAnywhere, Category = "WoT|Fog") float Radius = 25.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Fog") FLinearColor FogColor = FLinearColor::White;
	UPROPERTY(EditAnywhere, Category = "WoT|Fog") float VolBrightness = 1.0f;

	// Cache-projected (guarded) instance: no importer-authored MI_Fog_* exists,
	// so activation binds a M_WoT_VolumeFog dynamic instance with the same
	// FogColor/VolB/R parameters and the importer's sphere scale (-R/50, R/50,
	// R/50; negative X flips winding so the inside faces draw).
	UPROPERTY(VisibleAnywhere, Category = "WoT|Fog") bool bRuntimeProjected = false;

	UPROPERTY(VisibleAnywhere, Category = "WoT|Fog")
	TObjectPtr<class UStaticMeshComponent> SphereMod;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Fog")
	TObjectPtr<class UStaticMeshComponent> SphereAdd;

	// The cache-projected binding: the engine sphere scaled to Radius with a
	// M_WoT_VolumeFog dynamic instance on SphereAdd. False when the sphere
	// mesh or the master is absent (the light then stays hidden).
	bool BindRuntimeSphere();

private:
	void HandleWorldReadinessChanged(bool bReady);
	void ActivateForAuthorizedWorld();
	void DeactivateForUnauthorizedWorld();
	FDelegateHandle WorldReadinessHandle;
	bool bAuthorizedWorldActive = false;
};

// Per-frame camera gate: retail renders volumetrics only while the viewer's
// zone is a fog zone. Fog-zone extents are the placed AWoTZoneVolume actors
// with bFogZone (zones.py emits them from the compiled BSP).
UCLASS()
class UWoTFogGateSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTFogGateSubsystem, STATGROUP_Tickables);
	}
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override
	{
		return Type == EWorldType::Game || Type == EWorldType::PIE;
	}

private:
	bool bLastInFog = true;   // force the first-tick apply
	bool bScanned = false;
	UPROPERTY() TArray<TObjectPtr<AWoTFogLight>> FogLights;
	UPROPERTY() TArray<TObjectPtr<class AWoTZoneVolume>> FogZones;
};
