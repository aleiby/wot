#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMeshActor.h"
#include "WoTSkyBox.generated.h"

class UMaterialInterface;
class USceneCaptureComponentCube;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTargetCube;

/** Runtime sky-window cube face size after applying wot.SkyWindowCubeSize. */
WOT_API int32 WoTSkyWindowCaptureSize();

// The UE1 FakeBackdrop child scene, ported as a camera-locked mesh. Inherits
// AStaticMeshActor (its render path is what actually rasterizes an offscreen
// capture — a bare AActor+UStaticMeshComponent did not) and adds a per-frame
// camera lock: the sky-zone geometry (skygeo.py, vertices re-centered on the
// SkyZoneInfo origin) is pinned so the sky origin coincides with the view point
// and scaled up beyond the world. Developer/front-end instances render it
// directly; guarded runtime instances expose it only to a cube capture sampled
// by opaque FakeBackdrop windows. It moves by rotation only (infinite parallax,
// like the UE1 child frame re-rooted at the sky zone).
UCLASS()
class AWoTSkyBox : public AStaticMeshActor
{
	GENERATED_BODY()

public:
	AWoTSkyBox();

	virtual void PostInitializeComponents() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Switch this child scene from direct rendering to the runtime-map
	 * capture path. Front-end skies deliberately do not call this. */
	bool InitializeSkyWindowCapture();
	// Only the plan bound to the windows needs a live cube: inactive plans stop
	// capturing; activating one captures at once so the switch shows no stale cube.
	void SetSkyWindowCaptureActive(bool bActive);
	USceneCaptureComponentCube* GetSkyCapture() const { return SkyCapture; }
	UTextureRenderTargetCube* GetSkyCaptureTarget() const { return SkyCaptureTarget; }

	/** Adds a capture-only child of the sky mesh drawing Mesh with Material.
	 * Retail draws the child frame's non-opaque surfaces back to front, so the
	 * runtime map gives every translucent or modulated sky layer its own
	 * component, ordered by SortPriority (farthest lowest). Call before
	 * InitializeSkyWindowCapture so the capture's ShowOnly list includes it. */
	UStaticMeshComponent* AddSkyLayer(UStaticMesh* Mesh, UMaterialInterface* Material,
		int32 SortPriority);
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetSkyLayers() const { return SkyLayers; }

	// SkyZoneInfo rotation applied to the whole sky frame.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT|Sky")
	FRotator SkyRotation = FRotator::ZeroRotator;

	// Uniform scale pushing the mesh beyond world geometry so it always sorts
	// behind (direction is scale-invariant; this only sets how far it sits).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT|Sky")
	float SkyScale = 12.0f;

private:
	UPROPERTY(VisibleAnywhere, Transient, Category = "WoT|Sky")
	TObjectPtr<USceneCaptureComponentCube> SkyCapture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTargetCube> SkyCaptureTarget;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SkyLayers;
};
