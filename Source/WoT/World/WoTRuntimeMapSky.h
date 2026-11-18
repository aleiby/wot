// Retail sky-zone child scene and AutoU/VPan texture panning for authenticated
// map worlds. Pure functions over the decoded world-v3 record
// and BSP-v4 parts: no UObjects, no registry access, testable on real data.
#pragma once

#include "CoreMinimal.h"
#include "../WoTBSPPayload.h"
#include "../Payload/WoTRuntimeWorldResource.h"

/** UE1 PolyFlags consumed by the sky and pan rules (bsp2obj.py:36). */
constexpr uint32 WoTRuntimeMapPFInvisible = 0x00000001u;
constexpr uint32 WoTRuntimeMapPFMasked = 0x00000002u;
constexpr uint32 WoTRuntimeMapPFTranslucent = 0x00000004u;
constexpr uint32 WoTRuntimeMapPFNotSolid = 0x00000008u;
constexpr uint32 WoTRuntimeMapPFModulated = 0x00000040u;
constexpr uint32 WoTRuntimeMapPFFakeBackdrop = 0x00000080u;
constexpr uint32 WoTRuntimeMapPFTwoSided = 0x00000100u;
constexpr uint32 WoTRuntimeMapPFAutoUPan = 0x00000200u;
constexpr uint32 WoTRuntimeMapPFAutoVPan = 0x00000400u;
constexpr uint32 WoTRuntimeMapPFSpecialLit = 0x00100000u;
constexpr uint32 WoTRuntimeMapPFUnlit = 0x00400000u;
constexpr uint32 WoTRuntimeMapPFPortal = 0x04000000u;
/** The bsp2obj.flag_suffix family: exactly the bits encoded in a slot name. */
constexpr uint32 WoTRuntimeMapSlotFamilyMask = WoTRuntimeMapPFMasked
	| WoTRuntimeMapPFTranslucent | WoTRuntimeMapPFModulated
	| WoTRuntimeMapPFTwoSided | WoTRuntimeMapPFUnlit
	| WoTRuntimeMapPFFakeBackdrop | WoTRuntimeMapPFSpecialLit;
/** import_map.py make_world_instances: a dangling/NULL texture pans over the
 * substituted 32x32 Engine.DefaultTexture (tex_w, tex_h = 32, 32). */
constexpr uint32 WoTRuntimeMapDanglingPanTextureSize = 32u;

/** Read-only world-v3 views consumed by the retail sky rules. */
struct FWoTRetailSkyWorldView
{
	TConstArrayView<FWoTRuntimeWorldActor> Actors;
	TConstArrayView<FWoTRuntimeWorldZone> Zones;
	TConstArrayView<FWoTRuntimeWorldNode> Nodes;
	TConstArrayView<FWoTRuntimeWorldSurface> Surfaces;
};

/**
 * Tools/convert/skybake.py zone_sky_links + pick_winner, then skygeo.py
 * sky_zone_of + sky_surface_ids. Shared by the entry SkyPlan (which applies
 * its stricter single-scene policy on top) and the map-world projection.
 */
struct FWoTRetailSkySelection
{
	/** SkyZoneInfo records in actor order (indices into the view's actors). */
	TArray<int32> SkyActors;
	/** FakeBackdrop node-side tallies, parallel to SkyActors. */
	TArray<int32> Tallies;
	/** Retail LinkToSkybox result for every BSP zone. Values index SkyActors. */
	TArray<int32> ZoneSkyLinks;
	/** Index into SkyActors of the majority winner (ties: earliest actor). */
	int32 WinnerSky = INDEX_NONE;
	/** Index into the view's actors of the winner. */
	int32 WinnerActor = INDEX_NONE;
	/** First BSP zone whose zone actor is the winner (skygeo.sky_zone_of). */
	int32 Zone = INDEX_NONE;
	/** How many BSP zones name the winner (the entry rejects more than one). */
	int32 ZoneMatchCount = 0;
	/** Surfaces owning at least one >=3-vertex node that touches Zone. */
	TSet<int32> Surfaces;
	/** A PF_Portal sky-zone node borders a nonzero foreign zone: retail's child
	 * frame keeps traversing into the rest of the map (the sky-rest maps). */
	bool bTraversablePortal = false;

	/** A usable sky zone: nonzero and addressable by a node's uint8 zone
	 * (the entry path rejected anything else before this was shared). */
	bool HasSkyZone() const
	{
		return WinnerActor != INDEX_NONE && Zone > 0 && Zone <= MAX_uint8;
	}
};

WOT_API bool WoTSelectRetailSky(const FWoTRetailSkyWorldView& View,
	FWoTRetailSkySelection& Out, FString& Error);

/** One BSP part section, addressed in the descriptor's canonical part order. */
struct FWoTRuntimeMapSectionRef
{
	int32 Part = INDEX_NONE;
	int32 Section = INDEX_NONE;

	bool operator==(const FWoTRuntimeMapSectionRef& Other) const
	{
		return Part == Other.Part && Section == Other.Section;
	}
	friend uint32 GetTypeHash(const FWoTRuntimeMapSectionRef& Ref)
	{
		return HashCombineFast(GetTypeHash(Ref.Part), GetTypeHash(Ref.Section));
	}
};

/** How a sky-mesh section is drawn (including the sky-rest kinds). */
enum class EWoTRuntimeMapSkySectionKind : uint8
{
	/** A sky-zone slot bound to its world-slot material. */
	Sky,
	/** skygeo "SkyRest__<slot>": rest-of-map geometry the sky child frame sees
	 * through the sky's portal sheets, bound to that world slot's material. */
	RestVisible,
	/** skygeo "SkyRestBlock": flipped-winding opaque black solid-space blocker. */
	RestBlocker,
};

/**
 * The runtime equivalent of skygeo.py + setup_childsky.py for one map: the
 * exact skygeo sky-slot set recomputed from BSP-v4 section identity and flags,
 * the re-centred sky mesh, and the AWoTSkyBox parameters.
 */
struct FWoTRuntimeMapSkyPlan
{
	FWoTRetailSkySelection Selection;
	/** A winning SkyZoneInfo with a nonzero BSP zone and at least one sky slot. */
	bool bHasSky = false;
	FString SkyActorIdentity;
	FVector Origin = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	/** Parallel to the parts and their sections: true = moved to the sky mesh. */
	TArray<TArray<bool>> SkySections;
	/** Sorted unique sky slot names (= <Map>_skygeo.json "materials" keys). */
	TArray<FString> SkySlots;
	/** One section per SkySlots entry, vertices relative to Origin. */
	FWoTBSPPayloadData SkyMesh;
	/** Parallel to SkyMesh.Sections: the part section whose material the sky
	 * section reuses (setup_childsky.world_slot_materials: main parts first). */
	TArray<FWoTRuntimeMapSectionRef> SkyMaterialSources;
	/** Every part section merged into each sky section (main before portal). */
	TArray<TArray<FWoTRuntimeMapSectionRef>> SkySectionSources;
	/** max over main-role parts of the UE static-mesh bounds sphere radius. */
	double WorldRadius = 0.0;
	/** Nearest sky-mesh triangle to Origin (skygeo.tri_dist). */
	double MinSurfaceDistance = 0.0;
	float Scale = 0.0f;
	uint32 SkyTriangleCount = 0;
	/** Sky slots carrying PF_FakeBackdrop (never drawn by the runtime path). */
	uint32 FakeBackdropSkySlots = 0;
	/** Parallel to SkyMesh.Sections. A RestBlocker section's material source
	 * is {INDEX_NONE, INDEX_NONE}: the materializer binds opaque black. */
	TArray<EWoTRuntimeMapSkySectionKind> SkySectionKinds;
	/** The main part carried the version-6 sky-rest census (skybake.py
	 * bake_rest); false for a legacy cache, which keeps the documented
	 * sky-rest residual on the portal-unsealed maps. */
	bool bSkyRestCensus = false;
	/** Surfaces the census found visible / blocking / as nested windows. */
	uint32 SkyRestSurfaceCount = 0;
	/** Rest triangles added to the sky mesh (visible + blocker groups). */
	uint32 SkyRestTriangleCount = 0;
	/** Rest groups whose world slot has no part section (skygeo keeps the
	 * GLB default material there; the runtime leaves them out). */
	uint32 SkyRestMissingSlots = 0;
	/** "SkyRest__<slot>" / "SkyRestBlock" section names, in mesh order. */
	TArray<FString> SkyRestSlots;
};

/**
 * Returns false only for malformed inputs (bad indices, v4 identity/flag
 * contradictions between world-v3 and BSP). A world without a SkyZoneInfo or
 * without a winning sky zone succeeds with bHasSky=false (Arena_03/09/12).
 */
WOT_API bool WoTBuildRuntimeMapSkyPlan(const FWoTRetailSkyWorldView& View,
	TConstArrayView<FWoTBSPPayloadData> Parts, FWoTRuntimeMapSkyPlan& Out,
	FString& Error, int32 ForcedSky = INDEX_NONE);

/** Build one child scene for every SkyZoneInfo actually linked by a BSP zone.
 * ZonePlanIndices is parallel to View.Zones and names the plan shown while the
 * camera occupies that zone; INDEX_NONE means that zone has no usable sky. */
WOT_API bool WoTBuildRuntimeMapSkyPlans(const FWoTRetailSkyWorldView& View,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	TArray<FWoTRuntimeMapSkyPlan>& OutPlans, TArray<int32>& ZonePlanIndices,
	FString& Error);

/** UE1 BSP point-to-zone descent used to choose the camera's sky child scene. */
WOT_API int32 WoTRuntimeMapPointZone(
	TConstArrayView<FWoTRuntimeWorldNode> Nodes, const FVector& Point);

/**
 * Retail single-TMU Glide draws a lightmapped translucent surface in two passes
 * (the shipped GlideDrv.dll forces NumTmu=1 unless -HardwareMultiTexture):
 * the texture pass blends ONE/ONE_MINUS_SRC_COLOR, then the light-map pass
 * modulates the whole pixel DST_COLOR/SRC_COLOR, so FB = M*(Tex + Dst*(1-Tex))
 * PER CHANNEL. The guarded sky reproduces that with two fixed-function passes
 * that stack in draw order (Tools/import/sky_layer_materials.py): modulate by
 * M*(1-Tex), then add Tex*M. A single SceneColor-reading material could not
 * stack, because SceneColor is the pre-translucency scene (Arena_02's sepia
 * layer vanished behind its cloud sheet).
 *
 * Retail draws a frame's non-opaque surfaces back to front. The sky camera only
 * rotates about the sky origin, so one static far-to-near order per plan is
 * exact for layers that do not interpenetrate. Returns the sections flagged in
 * LayerSections (parallel to SkyMesh.Sections), ordered by decreasing minimum
 * face distance from the origin (sky vertices are origin-relative); equal
 * distances keep section order.
 */
WOT_API TArray<int32> WoTRuntimeMapSkyLayerOrder(const FWoTBSPPayloadData& SkyMesh,
	TConstArrayView<bool> LayerSections);

/** Out = Mesh holding only Sections, in that order, with their index ranges
 * (and per-triangle surfaces, when present) compacted; the vertex streams stay
 * whole. False when a section or its index range is out of bounds. */
WOT_API bool WoTRuntimeMapSkySubMesh(const FWoTBSPPayloadData& Mesh,
	TConstArrayView<int32> Sections, FWoTBSPPayloadData& Out);

/** skygeo.tri_dist (Ericson 5.1.5); a degenerate triangle yields
 * TNumericLimits<double>::Max() instead of a division by zero. */
WOT_API double WoTRuntimeMapPointTriangleDistance(const FVector3d& Point,
	const FVector3d& A, const FVector3d& B, const FVector3d& C);

/** setup_childsky.sky_scale_for: max(12, ceil(2*world_r/min_dist*1.25)). */
WOT_API double WoTRuntimeMapSkyScale(double WorldRadius, double MinSurfaceDistance);

/** UE StaticMeshBuilder ComputeBoundsFromVertexList sphere radius: the largest
 * distance of any vertex from its bounding-box centre. */
WOT_API double WoTRuntimeMapBoundsSphereRadius(TConstArrayView<FVector3f> Positions);

/** One slot's pan inputs after import_map.py main(): mats.update(pmats). */
struct FWoTRuntimeMapSlotPan
{
	uint32 Flags = 0;
	FString PanZoneActorIdentity;
};

/** Slot -> last-writer flags and pan zone, portal parts overriding main parts
 * exactly as import_map.py merges bsp.json "materials" then "portal_materials". */
WOT_API TMap<FString, FWoTRuntimeMapSlotPan> WoTRuntimeMapSlotPans(
	TConstArrayView<FWoTBSPPayloadData> Parts);

/** Zone actor identity (case-insensitive) -> (TexUPanSpeed, TexVPanSpeed). */
struct FWoTRuntimeMapPanSpeeds
{
	TMap<FString, FVector2f> ByZone;
	uint32 MalformedZones = 0;

	/** import_map._zone_pan: unknown or empty zone -> (1.0, 1.0). */
	FVector2f Find(FStringView ZoneActorIdentity) const;
};

/** UE1 ZoneInfo defaultproperties TexU/VPanSpeed=1.0; world-v3 retains only
 * authored deltas, so an absent property is the class default. */
WOT_API FWoTRuntimeMapPanSpeeds WoTRuntimeMapBuildPanSpeeds(
	TConstArrayView<FWoTRuntimeWorldActor> Actors);

/** import_map.py:1124-1131 in UV/sec: PanU = 35*TexUPanSpeed/tex_w when
 * PF_AutoUPan, PanV = 35*TexVPanSpeed/tex_h when PF_AutoVPan, else 0. */
WOT_API FVector2f WoTRuntimeMapPanRate(uint32 SlotFlags, const FVector2f& Speeds,
	uint32 TextureWidth, uint32 TextureHeight);
