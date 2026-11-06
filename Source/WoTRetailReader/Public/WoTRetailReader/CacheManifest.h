// Canonical cache-v3 object inventory shared by every typed retail importer.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTRetailReader/Contract.h"

enum class EWoTBSPPartRole : uint8
{
	Main,
	Portal,
};

/** One ordered output of the canonical main/portal/procedural BSP split. */
struct FWoTBSPPartDescriptor
{
	EWoTBSPPartRole Role = EWoTBSPPartRole::Main;
	/** split_world_glb asset identity (for example Arena_01_Fractal_000). */
	FString PartIdentity;
	/** Empty for the base part; otherwise the one record-bearing material slot. */
	FString ProceduralMaterialSlot;
	FString LogicalId;
	FString Schema;
	FString FactId;
	/** Hash of the exact canonical WOTPBSP payload, before WOTOBJ3 framing. */
	FWoTSha256 PayloadSha256;
	FWoTSha256 Sha256;
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	uint32 SectionCount = 0;
};

/** Complete authenticated description consumed unchanged by the world materializer. */
struct FWoTWorldDescriptor
{
	FString MapIdentity;
	uint32 NeutralMapSlot = 0;
	/** Canonical order: main base/parts, then portal base/parts. */
	TArray<FWoTBSPPartDescriptor> BSPParts;
	FString BSPFactId;
	FString BSPImporter;
	FString WorldLogicalId;
	FString WorldSchema;
	FString WorldFactId;
	FString WorldImporter;
	FWoTSha256 PackageSha256;
	FWoTSha256 WorldSha256;
	/** Package-byte provenance only; it does not commit to emitted objects. */
	FWoTSha256 SourceRoot;
	/** Canonical descriptor root committing to every emitted object and fact. */
	FWoTSha256 MapRoot;
	uint32 ActorCount = 0;
	uint32 AttachmentCount = 0;
	uint32 BSPNodeCount = 0;
	uint32 BSPSurfaceCount = 0;
	uint32 ZoneCount = 0;
	uint32 PortalSurfaceCount = 0;
	uint32 MoverCount = 0;
	/** Static-mover baked-lightmap diagnostics; excluded from descriptor roots. */
	uint32 StaticLightmappedMoverCount = 0;
	uint32 MoverLitPolyCount = 0;
	uint32 MoverRenderTriangleCount = 0;
	uint32 MoverCollisionTriangleCount = 0;
	uint32 MoverUV1OutsidePaddedRectCount = 0;
	uint32 MoverPolyOwnershipViolationCount = 0;
	uint32 MoverAnimatedLightPolyCount = 0;
	uint32 MoverAtlasWidth = 0;
	uint32 MoverAtlasHeight = 0;
	/** Battle2_02 Mover6 poly 2/3 RGB7 oracle witnesses; diagnostics only. */
	TArray<uint8> MoverOracleRGB7;
	uint32 LightCount = 0;
	/** Native lightmap-composer diagnostics; excluded from the sealed descriptor. */
	TMap<FString, uint32> LightmapSkippedLights;
	uint32 LightmapPerTexelZoneSurfaceCount = 0;
	uint32 SoundCount = 0;
	uint32 NavigationSeedCount = 0;
	uint32 SemanticObjectCount = 0;
	uint32 ReachSpecCount = 0;
};

struct FWoTCacheBlob
{
	FString LogicalId;
	FString Schema;
	FWoTSha256 Sha256;
	uint64 Size = 0;
	TArray<FWoTRequiredFactId> Satisfies;
};

struct FWoTCacheManifestV3
{
	FString Format = TEXT("wot-payload-cache");
	uint32 CacheVersion = 3;
	FString HashAlgorithm = TEXT("sha256");
	TArray<FWoTCacheBlob> Blobs;

	/** Present together on a completed cache build; absent on importer fixtures. */
	bool bHasBuildIdentity = false;
	FWoTSha256 InstallerSha256;
	FWoTSha256 ContractSha256;
	FWoTSha256 ProductionDescriptorSha256;
	/** Publication metadata belongs to the same completed manifest as its blobs. */
	TArray<FWoTWorldDescriptor> WorldDescriptors;
	FWoTSha256 FrontEndDescriptorRoot;
};

/** Canonical JSON, or a specific error when the in-memory value is not representable. */
WOTRETAILREADER_API TWoTBootstrapResult<TArray<uint8>> WriteCanonicalCacheManifest(
	const FWoTCacheManifestV3& Manifest);
