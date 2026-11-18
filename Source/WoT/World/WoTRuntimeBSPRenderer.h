#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../WoTBSPPayload.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "WoTLightmapShimmer.h"
#include "RHI.h"
#include <atomic>
#include "WoTRuntimeBSPRenderer.generated.h"

class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;
struct FWoTRuntimeProceduralMip;
struct FWoTTexturePayloadData;

struct WOT_API FWoTBSPTJunctionStats
{
	uint64 InputTriangles = 0;
	uint64 OutputTriangles = 0;
	uint64 SplitTriangles = 0;
	uint64 DroppedDegenerateTriangles = 0;
	uint64 DroppedDegenerateLoops = 0;
	uint64 LocallyOrientedLoops = 0;
	uint64 FallbackTriangles = 0;
	uint64 RepairedEdges = 0;
	uint64 InsertedEdgeVertices = 0;
	uint64 RepairedNearCoincidentEdges = 0;
	uint64 SkippedNearCoincidentTriangles = 0;
	uint64 RepairedLightmapRibbonSurfaces = 0;
	uint64 RepairedLightmapRibbonTriangles = 0;
	uint64 RepairedLightmapRibbonVertices = 0;
	uint64 RetessellatedFanSliverPairs = 0;
	uint64 LightmapRibbonSourceTriangleVisits = 0;
	uint64 LightmapRibbonChecks = 0;
	uint64 NearCollapsedChildEdges = 0;
	uint64 CellVisits = 0;
	uint64 CandidateChecks = 0;
	double RepairMilliseconds = 0.0;
	double MaxRepairedLightmapRibbonWidth = 0.0;
	double MaxRepairedNearCoincidentGap = 0.0;
	bool bFellBackToUnrepaired = false;
	bool bWholePayloadFallback = false;
	FString FallbackReason;
	// Exact triples used only to correlate final-buffer audit witnesses with
	// local source fallback or near-collapsed output ambiguity.
	TArray<FVector3f> FallbackTriangleCorners;
	TArray<FVector3f> NearCollapsedEdgeWitnesses;
};

/** Pure, joint repair of render-only payload copies. Never pass collision data.
 * On failure RenderParts is unchanged; OutError describes invalid data or a
 * work/output budget exceeded, so callers can safely render the original copy.
 * Missing TriangleSurfaces remains missing. Exact undirected edge incidence is
 * computed across parts, ignoring zero-area triangles; only incidence-one edges
 * are repair candidates.
 * Donor float positions within 0.01 units of an edge are shared exactly;
 * projected segment parameters interpolate host attributes and UV seams.
 * Same-facing incidence-one edges that share an exact anchor may also close
 * a <=0.5-unit near-collinear fork with opposite triangle interiors and one
 * unambiguous free endpoint shorter by more than 0.01 units than the host.
 * An isolated dark lightmap ribbon narrower than 0.5 units may inherit the
 * exact coplanar neighbour edge's lightmap UVs; positions, base UVs, sections,
 * surface provenance, authenticated input and collision remain unchanged.
 * A locally thinnest, poor-quality fan triangle may exchange its diagonal with
 * one exact coplanar same-surface neighbour when the quad is convex and all
 * vertex attributes are one affine mapping. This retessellates render indices
 * only; authenticated positions, attributes and collision remain unchanged. */
WOT_API bool WoTRuntimeBSPRepairRenderTJunctions(
	TArray<FWoTBSPPayloadData>& RenderParts,
	FWoTBSPTJunctionStats& OutStats, FString& OutError);

inline constexpr TCHAR WoTRuntimeSkyWindowMasterPath[] =
	TEXT("/Game/WoT/Materials/M_WoT_SkyWindow.M_WoT_SkyWindow");
inline constexpr TCHAR WoTRuntimeSkyWindowCubeParameter[] = TEXT("SkyCube");
// The two fixed-function passes of a stacked translucent sky layer
// (Tools/import/sky_layer_materials.py): Dst*M*(1-Tex), then + Tex*M.
inline constexpr TCHAR WoTRuntimeSkyModulateMasterPath[] =
	TEXT("/Game/WoT/Materials/M_WoT_EntrySkyModulate.M_WoT_EntrySkyModulate");
inline constexpr TCHAR WoTRuntimeSkyAddMasterPath[] =
	TEXT("/Game/WoT/Materials/M_WoT_EntrySkyAdd.M_WoT_EntrySkyAdd");

enum class EWoTRuntimeBSPBlend : uint8
{
	Opaque,
	Masked,
	Translucent,
	Modulated,
};

struct WOT_API FWoTRuntimeBSPSectionStyle
{
	FString ConverterSlotBase;
	EWoTRuntimeBSPBlend Blend = EWoTRuntimeBSPBlend::Opaque;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bSky = false;
	bool bSpecialLit = false;
	bool bNoSmooth = false;
};

struct FWoTRuntimeBSPConverterCandidate
{
	FString LogicalBase;
	FString SourceExport;
};

WOT_API UTexture2D* WoTRuntimeBSPMaterializeTexture(
	const FWoTTexturePayloadData& Payload, bool bRawSurfaceTexture,
	bool bNoSmooth = false);
/** Guarded pre-activation texture for one procedural record: the
    authenticated mip-0 seed through the record's own palette, named
    "WoTProc_<Pkg>__<Tex>" so UWoTFractalSubsystem can discover and replace
    it with the live simulation (the exact retail-authored still frame,
    never grey indices). Raw display-space bytes like every runtime surface. */
WOT_API UTexture2D* WoTRuntimeProceduralPlaceholderTexture(
	const struct FWoTFractalTextureRecord& Record);

WOT_API UTexture2D* WoTRuntimeBSPMaterializeProceduralIndices(
	const FWoTRuntimeProceduralMip& Mip);
WOT_API UTexture2D* WoTRuntimeBSPMaterializeLightmap(
	const FWoTBSPPayloadData& Payload);
WOT_API UTexture2D* WoTRuntimeBSPMaterializeLightmap(
	uint32 Width, uint32 Height, TConstArrayView<uint8> BGRA);
/** Native lightmap composer fullbright texel: BGRA 128,128,128,255. */
WOT_API UTexture2D* WoTRuntimeBSPMaterializeNeutralLightmap();
WOT_API UTexture2D* WoTRuntimeBSPMaterializeNeutralGreyTexture();
/** 1x1 opaque black: the sky-rest blocker texel (setup_childsky.black_material). */
WOT_API UTexture2D* WoTRuntimeBSPMaterializeSolidBlackTexture();

WOT_API FString WoTRuntimeBSPConverterSlotBase(FStringView SourceExport);
WOT_API bool WoTRuntimeBSPConverterSlotMatches(
	FStringView MaterialSlot, FStringView Base);
/** Runtime-world variant: converter base identity is case-insensitive. */
WOT_API bool WoTRuntimeBSPConverterSlotMatchesFolded(
	FStringView MaterialSlot, FStringView Base);
/** Unique candidate whose logical base agrees with source.export. */
WOT_API int32 WoTRuntimeBSPResolveAuthenticatedConverterSlot(
	FStringView MaterialSlot,
	TConstArrayView<FWoTRuntimeBSPConverterCandidate> Candidates);
WOT_API int32 WoTRuntimeBSPResolveConverterSlot(
	FStringView MaterialSlot,
	TConstArrayView<FString> AuthenticatedSourceExports);
WOT_API bool WoTRuntimeBSPSectionStyle(
	FStringView MaterialSlot, FWoTRuntimeBSPSectionStyle& OutStyle);
WOT_API bool WoTRuntimeBSPSectionStyleForBase(
	FStringView MaterialSlot, FStringView ConverterSlotBase,
	FWoTRuntimeBSPSectionStyle& OutStyle);
/** BSP-v4 path: style comes from the authenticated last-writer flags, not the
 * lossy converter-slot suffix grammar. */
WOT_API bool WoTRuntimeBSPSectionStyleForIdentity(
	FStringView CanonicalPackage, FStringView CanonicalName,
	uint32 SurfaceFlags, FWoTRuntimeBSPSectionStyle& OutStyle);
WOT_API void WoTRuntimeBSPApplyTextureFlags(
	FWoTRuntimeBSPSectionStyle& Style, bool bTextureMasked);

/** UE1 Texture.DetailTexture of a canonical base texture (the generated world
 * texture registry) and the UV ratio the opaque/masked world masters bind.
 * Glide tiles the detail at its own texel rate on both axes (the driver's
 * SetTexture/CopyVerts, Pan 0) while UV0 divides U by W*DrawScale and V by
 * H*DrawScale, so the ratio is per axis: (W*DrawScale)/(dW*dDrawScale) on U
 * (DetailRatio) and (H*DrawScale)/(dH*dDrawScale) on V (DetailRatioV). */
struct FWoTRuntimeBSPDetailTexture
{
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	float RatioU = 0.0f;
	float RatioV = 0.0f;
};
WOT_API bool WoTRuntimeBSPFindDetailTexture(
	FStringView CanonicalPackage, FStringView CanonicalName,
	FWoTRuntimeBSPDetailTexture& OutDetail);

WOT_API bool WoTRuntimeBSPBuildRenderMesh(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError);
/**
 * Cook collision from Payload independently of Mesh's filtered render data.
 * Portal payloads intentionally return success with no body setup.
 */
WOT_API bool WoTRuntimeBSPBuildCollision(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError,
	TConstArrayView<uint32> SurfaceFlags = {});

/** Per-frame animated light-map coefficients: waveform z = 2*wave-1 per
    group and the retail shimmer random tables (WoTLightmapShimmer.h),
    indexed by the surface-local texel key. Without tables the shimmer
    terms compose at their mean (coefficient 0). */
struct WOT_API FWoTLightmapAnimationFrame
{
	float Wave[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	/** Live recompositions so far (one RandomBases regeneration each). */
	uint32 NoiseFrame = 0;
	bool bFrozen = true;
	const FWoTLightmapShimmerRandoms* Randoms = nullptr;
};

/** Composes one surface's animated interior texels: base + sum(coef*response)
    clamped to [0, 254] per channel, in place over Out (BGRA rows of W texels).
    Pure; the frame coefficients and the response rects are the only inputs. */
WOT_API void WoTLightmapComposeSurface(
	TConstArrayView<uint8> BaseBGRA, uint32 AtlasWidth,
	const FWoTBSPAnimationSurface& Surface,
	TConstArrayView<const FWoTBSPAnimationTerm*> Terms,
	TConstArrayView<const FWoTBSPAnimationRect*> Rects,
	const FWoTLightmapAnimationFrame& Frame, TArray<uint8>& Out);

/** GC owner for the locally projected, non-replicated world BSP. */
UCLASS(Transient, NotPlaceable)
class WOT_API AWoTRuntimeBSPWorld final : public AActor
{
	GENERATED_BODY()

public:
	AWoTRuntimeBSPWorld();
	virtual void Tick(float DeltaSeconds) override;

	/** Adopts the version-5 animated light-map responses of the atlas carrier:
	    the shared light-map texture is recomposed on the CPU every frame from
	    the literal base atlas and the centered responses, exactly as retail
	    re-merged its light maps per frame. */
	void SetLightmapAnimation(const FWoTBSPPayloadData& Payload,
		UTexture2D* SharedLightmap);
	bool HasLightmapAnimation() const { return !AnimationTerms.IsEmpty(); }
	int32 GetAnimatedSurfaceCount() const { return AffectedSurfaces.Num(); }
	const TArray<FWoTBSPAnimationGroup>& GetAnimationGroups() const { return AnimationGroups; }
	uint32 GetAnimationFrameForTesting() const { return LastFrame.NoiseFrame; }
	/** Recomposes and uploads one frame at the given time (Tick's body). */
	void RecomposeLightmap(float TimeSeconds, bool bFrozen);
	/** Upload instrumentation: the batches (one UpdateTextureRegions call
	    each) and regions the last recomposition staged, the running total of
	    calls actually enqueued (a texture without an RHI resource, as under
	    -nullrhi, stages but never enqueues), frames dropped because every
	    staging slot was still in flight, and the persistent staging footprint. */
	int32 GetLastUploadBatchCount() const { return LastUploadBatchCount; }
	int32 GetLastUploadRegionCount() const { return LastUploadRegionCount; }
	uint64 GetTotalUploadCallCount() const { return TotalUploadCallCount; }
	uint32 GetSkippedUploadFrameCount() const { return SkippedUploadFrames; }
	uint64 GetUploadStagingBytes() const;
	const FWoTLightmapShimmerRandoms& GetShimmerRandoms() const { return ShimmerRandoms; }

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> GeometryComponents;

	/** Retail-solid PF_Invisible level geometry. It has physics only: no render
	 * sections or materials, and it is deliberately excluded from the ordinary
	 * GeometryComponents part-index contract. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> InvisibleSolidCollisionComponent;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> InvisibleSolidCollisionMesh;

	TArray<uint32> InvisibleSolidCollisionTriangleSurfaces;
	TArray<FString> InvisibleSolidCollisionTriangleTextureIdentities;

	bool ResolveInvisibleSolidCollisionFace(const UPrimitiveComponent* Component,
		int32 FaceIndex, uint32& OutSurface, FString& OutTextureIdentity) const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> RuntimeMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> RuntimeTextures;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> RuntimeMaterials;

	/** Opaque FakeBackdrop window instances. Every instance samples the cube
	 * belonging to the sky plan selected for the current local BSP zone. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SkyWindowMaterials;

	// Entry-family (M_WoT_Entry*) section instances whose AutoU/VPan rate is
	// nonzero. Those masters pan on the WoTSceneTime scalar rather than the
	// engine Time node, so this actor drives it with the world clock.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SceneTimeMaterials;

	// The camera-locked sky-zone child scene (null on maps without a sky).
	UPROPERTY(Transient)
	TObjectPtr<class AWoTSkyBox> SkyBox;

	/** One locally rendered child scene per linked SkyZoneInfo. SkyBox remains
	 * the first/majority-compatible alias used by existing diagnostics. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class AWoTSkyBox>> SkyBoxes;
	/** BSP zone -> SkyBoxes index, and the authenticated nodes used to locate
	 * the local camera. These are client-only presentation data. */
	TArray<int32> SkyPlanByZone;
	TArray<FWoTRuntimeWorldNode> SkyZoneNodes;
	int32 ActiveSkyPlan = INDEX_NONE;
	void BindSkyWindowsToPlan(int32 PlanIndex);
	void ApplySkyForViewPoint(const FVector& ViewPoint);

	// Retail PF_Mirrored child frames (WoTRuntimeMirror.h); null when the map
	// has no mirror or the client skipped them.
	UPROPERTY(Transient)
	TObjectPtr<class UWoTRuntimeMirrorDriver> MirrorDriver;

	/** Set WoTSceneTime on every registered scene-time instance. */
	void ApplySceneTime(float Seconds);
	/** Animated light-map surfaces or scene-time instances, and not a
	    dedicated server: the single tick predicate. */
	bool ShouldTick() const;
	/** Apply ShouldTick() to the start flag and the live tick function;
	    called by every path that changes either duty. */
	void RefreshTickEnabled();

	// The unbound gamma boundary + flat tone look every dev map carries as
	// placed volumes; shell worlds get it here (see the materializer).
	UPROPERTY(Transient)
	TObjectPtr<class UPostProcessComponent> PeriodLook;

private:
	struct FAffectedSurface
	{
		uint32 Surface = 0;
		TArray<const FWoTBSPAnimationTerm*> Terms;
		TArray<const FWoTBSPAnimationRect*> Rects;
	};

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> AnimatedLightmap;
	TArray<uint8> BaseLightmapBGRA;
	uint32 AtlasWidth = 0;
	uint32 AtlasHeight = 0;
	TArray<FWoTBSPAnimationSurface> AnimationSurfaces;
	TArray<FWoTBSPAnimationGroup> AnimationGroups;
	TArray<FWoTBSPAnimationTerm> AnimationTerms;
	TArray<FAffectedSurface> AffectedSurfaces;
	FWoTLightmapAnimationFrame LastFrame;
	bool bHasComposedFrame = false;

	// Persistent upload staging: an atlas-sized page per pool slot holding
	// every affected surface's padded block at its own atlas position, so one
	// frame is ONE UpdateTextureRegions call (common pitch, one region per
	// surface) and allocates nothing. A slot stays in flight until the RHI
	// copy's cleanup fires on the render thread; the shared pointer keeps it
	// alive past this actor's death.
	struct FUploadSlot
	{
		TArray<uint8> Page;
		TArray<FUpdateTextureRegion2D> Regions;
		std::atomic<bool> bInFlight{false};
	};
	static constexpr int32 UploadSlotCount = 3;
	TSharedPtr<FUploadSlot, ESPMode::ThreadSafe> AcquireUploadSlot();
	TSharedPtr<FUploadSlot, ESPMode::ThreadSafe> UploadSlots[UploadSlotCount];
	TArray<uint8> ComposeScratch;
	FWoTLightmapShimmerRandoms ShimmerRandoms;
	int32 LastUploadBatchCount = 0;
	int32 LastUploadRegionCount = 0;
	uint64 TotalUploadCallCount = 0;
	uint32 SkippedUploadFrames = 0;
};
