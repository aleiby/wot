// The sole game-module owner of authenticated pre-play world materialization.
#pragma once

#include "CoreMinimal.h"
#include "WoTLaunchEngineHooks.h"
#include "WoTRetailReader/BSPWorld.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTBSPPayload.h"

class FWoTCacheGenerationLease;
class FWoTRuntimeWorldResource;
class FWoTGameModule;
class AWoTRuntimeBSPWorld;
class UWorld;
class UMaterialInterface;
struct FWoTRuntimeBSPSectionStyle;
struct FWoTBSPTJunctionStats;
struct FWoTRetailSkyWorldView;
struct FWoTRuntimeMirrorOptions;
struct FWoTRuntimeMirrorStageStats;

/** Automation-only fault injection for the late sky-box staging step (always
 * None in production): the map must then materialize without a sky. */
enum class EWoTRuntimeSkyStageFault : uint8
{
	None,
	SkyBoxAllocation,
	MaterialBinding,
};

/** Synthetic authenticated registry record used only by runtime-BSP specs. */
struct FWoTRuntimeBSPTestConverterResource
{
	bool bProcedural = false;
	FString LogicalBase;
	FString SourceExport;
	// Test-only stand-in for an authenticated payload object. Production calls
	// never provide synthetic resources.
	bool bMaterializable = false;
};

/** Raw authenticated level geometry used to recover retail-solid surfaces
 * that are intentionally absent from the render BSP payload. */
struct FWoTRuntimeBSPCollisionGeometryView
{
	TConstArrayView<FVector3f> Vectors;
	TConstArrayView<FVector3f> Points;
	TConstArrayView<FWoTRuntimeWorldNode> Nodes;
	TConstArrayView<FWoTRuntimeWorldSurface> Surfaces;
	TConstArrayView<FWoTRuntimeWorldVertex> Vertices;
};

/** Collision-only projection for PF_Invisible && !PF_NotSolid && !PF_Portal.
 * SurfaceIndices is also the materialized world's single collision-only
 * surface authority; per-triangle arrays preserve hit provenance for gameplay and
 * proof diagnostics even though this payload has no rendered counterpart. */
struct FWoTRuntimeInvisibleBSPCollisionData
{
	FWoTBSPPayloadData Payload;
	TArray<uint32> SurfaceIndices;
	TArray<FString> TriangleTextureIdentities;
	uint32 PortalWithoutNotSolidSurfaceCount = 0;
};

WOT_API bool WoTWorldBuildInvisibleSolidBSPCollision(
	const FWoTRuntimeBSPCollisionGeometryView& Geometry,
	FWoTRuntimeInvisibleBSPCollisionData& Out, FString& Error);

/** Evidence that the complete two-pass candidate was validated before exposure. */
struct FWoTMaterializedWorldReceipt
{
	FString MapIdentity;
	FString WorldLogicalId;
	FString MapRootHex;
	uint32 NeutralMapSlot = 0;
	uint32 ActorCount = 0;
	uint32 AttachmentCount = 0;
	uint32 BSPNodeCount = 0;
	uint32 BSPSurfaceCount = 0;
	uint32 NavigationSeedCount = 0;
	uint32 SemanticObjectCount = 0;
	uint32 ReachSpecCount = 0;
	uint32 ZoneCount = 0;
	uint32 PortalSurfaceCount = 0;
	uint32 MoverCount = 0;
	uint32 LightCount = 0;
	uint32 SoundCount = 0;
	uint64 RepairedLightmapRibbonSurfaces = 0;
	uint64 RepairedLightmapRibbonTriangles = 0;
	bool bIdentityAndAttachmentPassComplete = false;
	bool bPropertyGeometryAndNavigationPassComplete = false;
	bool bFullCensusValidated = false;
	bool bReadyForAtomicExposure = false;
};

class WOT_API FWoTWorldMaterializer final
{
public:
	TWoTBootstrapResult<FWoTMaterializedWorldReceipt> MaterializeBeforePlay(
		UWorld& World, const FWoTWorldDescriptor& Descriptor,
		const FWoTCacheGenerationLease& Lease);

#if WITH_AUTOMATION_TESTS
	/** Reject one otherwise-complete hidden candidate before engine exposure. */
	static void RejectNextCompletedCandidateForAutomationTests();
	/** Publish only the adopted-authority predicate for ordinary spec worlds. */
	static bool PublishNormalAuthorityAdoptedForAutomationTests();
	/** Remove only the ordinary-spec publication, preserving real authority. */
	static void ResetNormalAuthorityAdoptedForAutomationTests();
	/** Revoke the exact adopted world lease without clearing route state. */
	static bool RevokeNormalLeaseForAutomationTests();
	static void ResetAuthorityForAutomationTests();
#endif

};

/** The full-census mover count: mover models whose actor record carries that
 * model as its Brush (Engine.Model) - the cache builder's exact rule
 * (BSPWorld.cpp MoverCount), not a class-name heuristic. */
WOT_API uint32 WoTWorldCensusMoverCount(const FWoTRuntimeWorldResource& World);

/** Decode one descriptor-authorized world object and bind it to its complete
 * descriptor census and importer version, as materialization does first. */
WOT_API FWoTBootstrapStatus WoTWorldLoadDescribedResource(
	const FWoTWorldDescriptor& Descriptor,
	FWoTRuntimeWorldResource& WorldResource);

/** Decode one BSP part against its exact descriptor (header, payload digest,
 * geometry and sections), as materialization does for every part. */
WOT_API FWoTBootstrapStatus WoTWorldDecodeBSPPart(
	const FWoTBSPPartDescriptor& Descriptor,
	FWoTBSPPayloadData& OutData);

/** The exact static-world material-family router, shared by BSP and baked movers. */
WOT_API UMaterialInterface* WoTWorldRuntimeBSPMaster(
	const FWoTRuntimeBSPSectionStyle& Style, bool bProcedural,
	bool* bOutSceneTime = nullptr);

/** UE1 calls SetupForSurf only for surfaces carrying a light map. */
WOT_API float WoTRuntimeBSPDynamicLightEnable(
	const FWoTRuntimeBSPSectionStyle& Style);

/** The callback registered once with WoTLaunch's permanent engine dispatcher. */
WOT_API bool WoTMaterializeAuthenticatedWorld(
	UWorld* World, EWoTWorldStartPath Path, FString* Error);

/** Called by the engine after publishing GWorld/the context, before observers. */
WOT_API bool WoTCommitAuthenticatedWorldExposure(
	UWorld* World, FString* Error);

/** Give every registered primitive component that has no physics body one,
 * once the world has a physics scene. Materialized actors register before
 * InitWorld creates the scene; the exposed world's post-initialization hook
 * calls this. Returns the number of bodies created. */
WOT_API int32 WoTWorldCompleteRegisteredPhysicsState(UWorld& World);

/** The render-state twin of WoTWorldCompleteRegisteredPhysicsState: give every
 * registered primitive component that wants a render state
 * (ShouldCreateRenderState) and has none one, once the world has a scene. A
 * component registered before InitWorld allocated the scene skipped it
 * (UActorComponent::ExecuteRegisterEvents), and nothing later creates it: every
 * projected mover (the Arena_07 Cross slabs, every Battle door/portcullis) and
 * each RainCyd's master segment stayed collidable but invisible. Returns the
 * number of render states created. */
WOT_API int32 WoTWorldCompleteRegisteredRenderState(UWorld& World);

/** Atomically adopt another view of the verified cache snapshot. */
WOT_API FWoTBootstrapStatus WoTAuthorizeNormalWorldMaterialization(
	const FWoTCacheGenerationLease& Snapshot,
	TArray<FWoTWorldDescriptor>&& AuthenticatedWorlds);

/** Published at successful normal-authority adoption, before any normal world
 * is materialized or exposed. This is distinct from exposure-time readiness. */
WOT_API bool IsWoTNormalWorldAuthorityAdopted();

/** True only for the exact adopted generation whose use-time integrity
 * failure was durably recorded and whose retained front-end closure is
 * independently startup-verified. */
WOT_API bool IsWoTFrontEndIntegrityRecoveryAuthorized();

/** Game-instance shutdown only. Releases the process authority after scene and
 * font consumers have stopped, before their underlying cache leases leave. */
WOT_API void WoTShutdownNormalWorldMaterializationAuthority();

/** The neutral-shell package that materializes the given short map identity
 * (e.g. "Arena_01" -> "/WoTNeutralShells/WoTMapShell01") under the adopted
 * normal authority. Unset when no authority is adopted or the identity is
 * outside the authenticated descriptor set. Guarded server travel MUST route
 * through this: the materializer's allow-list admits only the bootstrap
 * package and the neutral shells, so a direct /Game/WoT/Maps/<map> travel is
 * rejected fail-closed (task9f host-join, 2026-07-31). */
WOT_API TOptional<FString> WoTGuardedShellPackageForMapIdentity(
	const FString& MapIdentity);

/** Reverse the adopted shell route back to its authenticated retail identity.
 * Accepts either a long package path or the short runtime world name. */
WOT_API TOptional<FString> WoTMapIdentityForGuardedShellPackage(
	const FString& ShellPackageOrWorldName);

/** Short map identities (e.g. "Arena_01") of the adopted normal authority's
 * authenticated descriptor set, in descriptor order. Empty when no authority
 * is adopted. The front-end catalog lists these in guarded builds, where no
 * cooked /Game/WoT/Maps package exists and every hosted map is a neutral
 * shell materialized from its descriptor. */
WOT_API TArray<FString> WoTAdoptedWorldMapIdentities();

/** Guarantee the neutral-shell content root ("/WoTNeutralShells/") resolves as
 * a long package name. Cooked stages that omit the shell plugin from the
 * plugin manifest still carry the shell packages under
 * <Project>/Plugins/WoTNeutralShells/Content; without the mount point the
 * engine's CanServerTravel refuses every shell-routed host travel. Returns
 * true when the root resolves afterwards; idempotent. */
WOT_API bool WoTEnsureNeutralShellMountPoint();

/** Pure census arithmetic behind normal-world materialization: the active
 * generation's World/BSP resources must equal the authenticated descriptor
 * set exactly, except for the separately attested front-end entry world and
 * its BSP (reserved ids present in the registry), which the front-end
 * authority verifies on its own. */
WOT_API bool WoTWorldCensusReconciles(int32 RegistryWorlds, int32 RegistryBSP,
	int32 DescriptorWorlds, int32 DescriptorBSP,
	int32 ReservedWorldsPresent, int32 ReservedBSPPresent);

/** Recover and verify the descriptors bound to this exact active generation. */
WOT_API TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>
WoTDeriveWorldDescriptorsFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease);

#if WITH_AUTOMATION_TESTS
WOT_API void WoTSetWorldLeaseAdoptionObserverForTesting(
	TFunction<void(FString, const FWoTSha256&)> Observer);

/** Directly exercise atlas ownership, shared binding, and fallback accounting. */
WOT_API bool WoTWorldBuildRuntimeBSPForTesting(UWorld& World,
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	AWoTRuntimeBSPWorld*& OutWorld, uint32& OutFallbackLightmapPartCount,
	FString& Error,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources = {},
	uint32* OutUnresolvedSlotCount = nullptr,
	TArray<FString>* OutUnresolvedSlotNames = nullptr,
	const FWoTRetailSkyWorldView* WorldView = nullptr,
	FString* OutSkyError = nullptr,
	EWoTRuntimeSkyStageFault SkyFault = EWoTRuntimeSkyStageFault::None,
	uint32* OutDefaultTextureSlotCount = nullptr,
	TConstArrayView<uint32> SurfaceFlags = {},
	const FWoTRuntimeInvisibleBSPCollisionData* InvisibleCollision = nullptr,
	FWoTBSPTJunctionStats* OutTJunctionStats = nullptr,
	double* OutBSPMilliseconds = nullptr);

/** The runtime BSP build with the mirror stage's automation seams (stand-in
 * master, forced skip, assumed clip-plane support) and its stage statistics. */
WOT_API bool WoTWorldBuildRuntimeBSPWithMirrorsForTesting(UWorld& World,
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	AWoTRuntimeBSPWorld*& OutWorld, FString& Error,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources,
	const FWoTRuntimeMirrorOptions& MirrorOptions,
	FWoTRuntimeMirrorStageStats& OutMirrorStats);

/** The texture-resolution half of WoTWorldBuildRuntimeBSP without building any
 * mesh: every drawn section of every part resolved exactly as
 * MakeSectionMaterial does against the given authenticated stand-ins
 * (exact-first identity, FName-case source check, then the retail
 * DefaultTexture rule for NULL/dangling sections). Out-lists are material
 * slots, unique, in first-seen order; OutResolved maps slot -> logical id. */
WOT_API bool WoTWorldResolveBSPSectionTexturesForTesting(
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources,
	TArray<FString>& OutUnresolvedSlots, TArray<FString>& OutDefaultTextureSlots,
	TMap<FString, FString>& OutResolved, FString& Error);
#endif
