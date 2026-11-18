// Diagnostic probe that disproved a suspected engine issue: the claim that
// post-registration render-state changes
// (per-tick SetActorTransform, one-shot moves, SetMaterial, SetCastShadow)
// leave primitives permanently undrawn in this project's -game was FALSE.
// Every such path renders correctly — verified here for runtime spawns and
// level-saved actors, tiny and 100000uu moves, off-frustum births, per-tick
// identical re-sets, post-registration rescales, huge-bounds sky meshes, and
// a level-saved AWoTMover teleported into view. The one invisible actor, the
// placed AWoTSkyBox, was invisible because setup_childsky.py's hide loop
// overrode the box's OWN slots with M_WoT_SkyHidden (found via this probe's
// propdiff mode; script fixed, maps re-saved, box verified rendering through
// its own tick on Battle4_01/Battle2_02). No engine bug; a vanilla-5.8 repro
// is also clean under stock AND our renderer config.
//
// Activate with -eofrepro="<outdir>". Default mode spawns four distinct
// shapes (plus a floor slab) 50000uu above the host map's origin, lights them
// with its own movable sun, and screenshots at t=4s and t=6s:
//   Y=-375  CONTROL  cube      spawn-time state only
//   Y=-125  ONCE     sphere    single SetActorLocation at t=1s
//   Y=+125  TICK     cylinder  per-tick SetActorTransform (osc. Z)
//   Y=+375  MAT      cone      SetMaterial(0, WorldGrid) at t=1s
//   (plus JUMP: off-frustum birth teleported into frame, and SKYCLONE: the
//   map's sky mesh moved onto the camera post-registration)
// -eofmode=none     baseline, no mutations (all shapes must render)
// -eofmode=sky      camera only: the frame is whatever the map's ticking
//                   AWoTSkyBox paints (+2000uu camera step between shots)
// -eofmode=clone_*  sky-mesh clone lifecycle variants (plain/scale/origin/
//                   lod/tick/wotbox)
// -eofmode=boxfix   MarkRenderStateDirty on the placed box at t=1s
// -eofmode=reregister  full component reregister on the placed box at t=1s
// -eofmode=propdiff reflected property diff: placed box vs runtime twin
//                   (this is the mode that found OverrideMaterials)
// -eofmode=mover    teleport a level-saved AWoTMover into the rig's view
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTEOFProbe.generated.h"

UCLASS()
class AWoTEOFProbeShape : public AActor
{
	GENERATED_BODY()

public:
	AWoTEOFProbeShape();
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Mesh;

	// When set, Tick re-issues SetActorTransform every frame (oscillating a
	// few units in Z so the transform genuinely changes and the update can't
	// be early-outed) — the same call pattern as AWoTSkyBox::Tick.
	bool bTickTransform = false;
	FVector Home = FVector::ZeroVector;
	float Age = 0.0f;
};

UCLASS()
class UWoTEOFProbeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	void Mutate();
	void MutateClone();
	void FixBox();
	void MoveMover();
	void TeleportCamera();
	void LogSkyBox(const TCHAR* Tag);
	void Shot1();
	void Shot2();
	void Quit();

	FString OutDir;
	bool bSkyOnly = false;
	bool bReregister = false;
	FString CloneMode;
	double CloneScale = 1.0;
	FTimerHandle MutateTimer, Shot1Timer, Shot2Timer, QuitTimer;

	UPROPERTY()
	TObjectPtr<AWoTEOFProbeShape> ControlShape;

	UPROPERTY()
	TObjectPtr<AWoTEOFProbeShape> OnceShape;

	UPROPERTY()
	TObjectPtr<AWoTEOFProbeShape> TickShape;

	UPROPERTY()
	TObjectPtr<AWoTEOFProbeShape> MatShape;

	// Spawns 100000uu off-frame; Mutate() teleports it INTO frame — does a
	// single LARGE post-registration move survive where the skybox's does not?
	UPROPERTY()
	TObjectPtr<AWoTEOFProbeShape> JumpShape;

	// The map's own sky mesh at the map's SkyScale on a plain StaticMeshActor
	// (the PlaceSkyClone recipe, proven to render when spawned in place);
	// spawned 20000uu away, then teleported onto the camera by Mutate() — does
	// a huge-bounds primitive survive a post-registration move?
	UPROPERTY()
	TObjectPtr<AActor> SkyCloneActor;

	UPROPERTY()
	TObjectPtr<AActor> Camera;

	UPROPERTY()
	TObjectPtr<AActor> MoverActor;
};
