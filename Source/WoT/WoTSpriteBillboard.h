#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTSpriteBillboard.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPrimitiveComponent;
class UTexture;
class UTexture2D;
struct FWoTRuntimeTextureOptions;

// UE5 records the world-time at which a primitive was actually submitted on
// screen. Consumers treat an ADVANCE of that observed timestamp as a draw;
// comparing it with a game-frame clock freezes under normal render-thread lag.
// WasRecentlyRendered is deliberately not used because its grace window
// advances hidden retail textures.
WOT_API bool WoTObservePrimitiveDraw(const UPrimitiveComponent* Primitive,
	float& LastObservedRenderTimeSeconds, float& OutDrawTimeSeconds);

#if WITH_AUTOMATION_TESTS
struct FWoTTextureAnimationResourceCounts
{
	int32 TextureResolutions = 0;
	int32 DynamicMaterialCreations = 0;
	int32 ParticleSubsteps = 0;
	int32 AnimationUpdates = 0;
};

WOT_API void WoTResetTextureAnimationResourceCountsForTesting();
WOT_API FWoTTextureAnimationResourceCounts
	WoTGetTextureAnimationResourceCountsForTesting();
WOT_API void WoTCountTextureAnimationResolutionForTesting();
WOT_API void WoTCountTextureAnimationMIDForTesting();
WOT_API void WoTCountParticleSprayerSubstepForTesting();
WOT_API void WoTCountParticleSprayerAnimationUpdateForTesting();
#endif

UCLASS()
class UWoTTextureAnimationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	static bool SupportsPresentationForNetMode(ENetMode NetMode)
		{ return NetMode != NM_DedicatedServer; }
	int32 RequestFrame(const FString& TextureIdentity, int32 FrameCount,
		float MinFrameRate, float MaxFrameRate, double DrawTimeSeconds,
		uint64 RenderFrameNumber);
	int32 GetStateCountForTesting() const { return States.Num(); }
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Packaged-Test play-through evidence: exact shared frame counters keyed by
	// retail texture identity. Shipping does not expose or compile this seam.
	TMap<FString, int32> SnapshotFramesForHarness() const;
#endif
#if WITH_AUTOMATION_TESTS
	int32 GetFrameForTesting(const FString& TextureIdentity) const
	{
		const FState* State = States.Find(TextureIdentity);
		return State ? State->Frame : INDEX_NONE;
	}
#endif

private:
	struct FState
	{
		float Accumulator = 0.0f;
		double LastUpdateDrawTime = -1.0;
		uint64 LastRenderFrame = MAX_uint64;
		int32 Frame = 0;
	};
	TMap<FString, FState> States;
};

// UE1 DT_Sprite (the original DrawActorSprite, re-derived): one world-space
// camera-facing quad, world size = DrawScale x (USize, VSize), STY_Translucent as
// the exact two-pass split (modulate under additive — the corona/sprayer scheme).
// Rendered as a single-instance ISM pair off the M_WoT_ParticleAdd/Mod masters.
// STY_Modulated instead uses M_WoT_MeshModulate for FB=2*Tex*Dst and ignores
// ScaleGlow, as UE1's PF_Modulated sprite draw did. The translucent pair reads
// PerInstanceCustomData 0 as ScaleGlow, so a plain static mesh would
// draw black — the ISM path is load-bearing, not an optimization.
// Frame cycling reproduces the Texture.AnimNext ring (chain end wraps to the
// drawn identity) via a per-pass MID texture swap. One per-world state is keyed
// by exact retail texture identity and advances at most once per rendered frame,
// only when a visible consumer requests it. Dedicated servers create no state.
UCLASS(ClassGroup = (WoT), meta = (BlueprintSpawnableComponent))
class UWoTSpriteBillboardComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UWoTSpriteBillboardComponent();

	// Actor DrawScale (world width = DrawScale x TexWidth uu).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float DrawScale = 1.0f;

	// Actor ScaleGlow (UE1 clamps the color multiply at 1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float ScaleGlow = 1.0f;

	// STY_Modulated sprites draw only the retail 2x-modulate pass.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bModulateOnly = false;

	// The AnimNext ring cadence (frames/sec; UE1 = one per rendered frame).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float FrameRate = 30.0f;

	// Play-once mode (Explosion.uc Tick: frame = NumFrames x elapsed/LifeSpan,
	// held at the last frame): spread the series over this many seconds instead
	// of looping at FrameRate. 0 = loop mode.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float PlayOnceSeconds = 0.0f;

	// Cycle an imported frame series: soft paths under /Game/WoT/Textures/<Dir>/,
	// sized TexW x TexH (the UE1 USize/VSize). Builds MIDs off the matching master.
	void SetSpriteSeries(const TCHAR* Dir, const TArray<FString>& FrameNames,
	                     float TexW, float TexH);
	void SetAnimatedTextureSeries(const TArray<FString>& TextureIdentities,
		float TexW, float TexH, float MinRate, float MaxRate);
	static FWoTRuntimeTextureOptions RuntimeTextureOptions();

	// Bind fixed materials (a time-scrubbed flipbook MI pair, or a static frame):
	// no per-frame cycling. Mod may be null when bModulateOnly is false and the
	// add MI carries the look alone (and vice versa). A procedural identity binds
	// both passes directly to their live fractal texture after quad creation.
	void SetSpriteMaterials(UMaterialInterface* Add, UMaterialInterface* Mod,
		float TexW, float TexH, FName RuntimeProceduralIdentity = NAME_None);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	// Did the series resolve any textures (spec probe: a broken import leaves
	// Frames all-null and the quad silently blank)?
	bool HasResolvedFrames() const { return Frames.Num() > 0 && Frames[0] != nullptr; }
	bool HasConfiguredFrames() const { return !Frames.IsEmpty(); }
	int32 GetCurrentFrameForTesting() const { return CurrentFrame; }
#if WITH_AUTOMATION_TESTS
	void MarkDrawnForTesting();
	void MarkDrawnForTesting(float DrawTimeSeconds);
	UTexture* GetBoundTextureForTesting() const;
	UMaterialInterface* GetModulateMaterialForTesting() const
	{
		return ModMid;
	}
	float GetTextureWidthForTesting() const { return TexWidth; }
	float GetTextureHeightForTesting() const { return TexHeight; }
	FString GetCurrentFrameIdentityForTesting() const
	{
		return FrameIdentities.IsValidIndex(CurrentFrame)
			? FrameIdentities[CurrentFrame] : FString();
	}
#endif

	// Owner-only visibility forwards to the inner ISM primitives (the component
	// itself is a plain scene node; retail bOnlyOwnerSee — TrapDetect icon).
	void SetOnlyOwnerSee(bool bValue);
	bool IsOnlyOwnerSee() const { return bOnlyOwnerSeeQuads; }

private:
	void EnsureQuads();
	void ApplyFrame(int32 Frame);

	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> AddQuad;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> ModQuad;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> AddMid;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ModMid;
	UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> Frames;
	TArray<FString> FrameIdentities;

	bool bOnlyOwnerSeeQuads = false;
	float TexWidth = 64.0f;
	float TexHeight = 64.0f;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	float Elapsed = 0.0f;
	float LastObservedRenderTimeSeconds = -1.0f;
	int32 CurrentFrame = INDEX_NONE;
};
