#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTCorona.generated.h"

// UE1 light corona: a screen-constant-size glow
// sprite drawn over the world (no depth test) for bCorona lights with a Skin
// texture. Visibility = the viewer pawn's BSP leaf carrying the light in its
// baked iPermeating run (UnRender DrawFrame walks
// Leaves(Region.iLeaf).iPermeating into Model->Lights; shadow-aware and
// leaf-granular — PermeatingLeaves + AWoTLeafMap reproduce it exactly; the
// Reach distance gate remains as the un-baked/dynamic-light fallback)
// AND a camera->light trace: on baked maps the exact retail one
// (AWoTLeafMap::LineCheck — BSP CSG solidity only, no actors), else an
// engine visibility trace;
// brightness fades in at 6/s and out at 3/s (Delta=3*dt, +2*Delta/-Delta),
// then scales by min(1, ProjZ/viewZ) — UE1's pixel-space Proj.Z coupling,
// 800 at the golden capture resolution (1600w FOV90).
// UE1 blends FB = Src + Dst*(1-Src) with a PER-CHANNEL dst factor; a single
// translucent material can only scalarize that (max3 over-darkens the
// background behind colored halos — it swallowed the lantern meshes). Exact
// two-pass split instead: SpriteMod (BLEND_Modulate, (1-Src)^2.2) draws
// first via lower translucency sort priority, Sprite (additive, Src) on top.
// The importer assigns both MICs and DrawScale/Color; this actor billboards
// the quads, keeps screen size constant, and drives the "Bright" scalars.
UCLASS()
class AWoTCorona : public AActor
{
	GENERATED_BODY()

public:
	AWoTCorona();

	// UE1: Scale = 512*DrawScale*(ScreenW/640) px → fraction of screen width.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float DrawScale = 1.0f;

	// Hue-triangle color x saturation lerp to white (computed at import).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FLinearColor Color = FLinearColor::White;

	// UE1 Frame->Proj.Z at the reference resolution (1600w FOV90 = 800).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float BrightZ = 800.0f;

	// Camera-distance visibility gate (uu): 25*(LightRadius+1), maxed with
	// 25*(VolumeRadius+1) for volumetric lights; set at import. Default =
	// the UE1 Light-class LightRadius 64. Fallback only — used when the map
	// carries no leaf bake (bLeafGated false).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float Reach = 1625.0f;

	// Retail bake: the BSP leaves whose iPermeating run carries this light
	// (sorted; importer-set from exports/leaves/<Map>.leaves.json). May be
	// EMPTY with bLeafGated — a static light in no run never shows a corona
	// in retail (embedded/shadowed).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<int32> PermeatingLeaves;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bLeafGated = false;

	// Retail texture identity, resolved through the authenticated B1 runtime
	// asset path after the deferred actor enters the world.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName SkinSubPath;

	// Additive pass: FB += Src (drawn second).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<UStaticMeshComponent> Sprite;

	// Modulate pass: FB *= (1-Src) per channel (drawn first).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<UStaticMeshComponent> SpriteMod;

	virtual void Tick(float DeltaSeconds) override;

	// Deferred projection registers the components before the runtime texture
	// and material instances exist. Rebind and dirty them at the publication
	// boundary (the same late-binding contract as UWoTVertexAnimComponent).
	void EnsureRuntimeRenderState();

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MidMod;

	TWeakObjectPtr<class AWoTLeafMap> LeafMap;

	float Bright = 0.0f;
	float DebugLogAccum = 0.0f;
	bool bRuntimeMaterialsBound = false;
};
