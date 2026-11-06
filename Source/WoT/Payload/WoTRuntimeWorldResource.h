// Strict cache-v3 world-record object decoder for Task-28 materialization.
#pragma once

#include "CoreMinimal.h"
#include "WoTRuntimeRegistry.h"

enum class EWoTRuntimeWorldPropertyType : uint8
{
	Byte = 1,
	Integer = 2,
	Boolean = 3,
	Float = 4,
	Object = 5,
	Name = 6,
	Struct = 10,
	String = 13,
};

struct FWoTRuntimeWorldProperty
{
	FString Name;
	EWoTRuntimeWorldPropertyType Type = EWoTRuntimeWorldPropertyType::Byte;
	uint32 ArrayIndex = 0;
	FString StructName;
	FString NameValue;
	FString ObjectIdentity;
	FString ObjectClassIdentity;
	TArray<uint8> Value;
	TArray<FWoTRuntimeWorldProperty> StructMembers;
};

struct FWoTRuntimeWorldSemanticObject
{
	FString Identity;
	FString ClassIdentity;
	bool bActor = false;
};

struct FWoTRuntimeWorldActor
{
	FString Identity;
	FString ClassIdentity;
	FString ParentIdentity;
	TArray<FWoTRuntimeWorldProperty> Properties;
};

struct FWoTRuntimeWorldNode
{
	float Plane[4] = {};
	uint64 ZoneMask = 0;
	uint8 Flags = 0;
	int32 VertexPool = 0;
	int32 Surface = 0;
	int32 Back = INDEX_NONE;
	int32 Front = INDEX_NONE;
	int32 PlaneNode = INDEX_NONE;
	int32 CollisionBound = INDEX_NONE;
	int32 RenderBound = INDEX_NONE;
	uint8 Zones[2] = {};
	uint8 VertexCount = 0;
	int32 Leaves[2] = {};
};

struct FWoTRuntimeWorldSurface
{
	int32 Base = 0;
	int32 Normal = 0;
	int32 TextureU = 0;
	int32 TextureV = 0;
	int32 LightMap = INDEX_NONE;
	int32 BrushPoly = INDEX_NONE;
	uint32 Flags = 0;
	int16 PanU = 0;
	int16 PanV = 0;
	float Friction = 1.0f;
	FString TextureIdentity;
	FString ActorIdentity;
};

struct FWoTRuntimeWorldLeaf
{
	int32 Zone = 0;
	int32 Permeating = INDEX_NONE;
	int32 Volumetric = INDEX_NONE;
};

struct FWoTRuntimeMoverSection
{
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	uint32 MinVertex = 0;
	uint32 MaxVertex = 0;
	bool bCollisionEnabled = true;
	bool bAnimatedLightmap = false;
	uint32 SurfaceFlags = 0;
	FVector3f PlaneOrigin = FVector3f::ZeroVector;
	FVector3f PlaneNormal = FVector3f::ZAxisVector;
	FString MaterialSlot;
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	FString MacroCanonicalPackage;
	FString MacroCanonicalName;
	FString MacroSourceTextureIdentity;
	float MacroRatio = 0.0f;
};

struct FWoTRuntimeMoverAnimationGroup
{
	uint8 Type = 0;
	uint8 Period = 0;
	uint8 Phase = 0;
	uint32 LightCount = 0;
};

struct FWoTRuntimeMoverAnimationTerm
{
	uint8 Kind = 0;
	int32 Group = INDEX_NONE;
	uint8 Effect = 0;
	uint32 LightCount = 0;
	// Expanded once at decode. Runtime materials sample these immutable pages;
	// animation never rewrites the mover atlas on the CPU.
	TArray<uint8> ResponseBGRA;
};

struct FWoTRuntimeMoverModel
{
	FString ActorIdentity;
	FString ModelIdentity;
	bool bLightmapped = false;
	TArray<FWoTRuntimeMoverSection> Sections;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UV0;
	TArray<FVector2f> UV1;
	TArray<uint32> Indices;
	TArray<FVector3f> CollisionPositions;
	TArray<uint32> CollisionIndices;
};

struct FWoTRuntimeWorldVertex
{
	int32 Point = 0;
	int32 Side = 0;
};

struct FWoTRuntimeWorldZone
{
	FString ActorIdentity;
	uint64 Connectivity = 0;
	uint64 Visibility = 0;
};

struct FWoTRuntimeWorldReachSpec
{
	int32 Distance = 0;
	int32 CollisionRadius = 0;
	int32 CollisionHeight = 0;
	uint32 ReachFlags = 0;
	bool bPruned = false;
	FString StartIdentity;
	FString EndIdentity;
};

class WOT_API FWoTRuntimeWorldResource final
{
public:
	FWoTRuntimeWorldResource() = default;
	FWoTRuntimeWorldResource(FWoTRuntimeWorldResource&&) = default;
	FWoTRuntimeWorldResource& operator=(FWoTRuntimeWorldResource&&) = default;
	FWoTRuntimeWorldResource(const FWoTRuntimeWorldResource&) = delete;
	FWoTRuntimeWorldResource& operator=(const FWoTRuntimeWorldResource&) = delete;

	FWoTBootstrapStatus LoadFirstUse(
		const FWoTNeutralResourceId& Resource);
#if WITH_AUTOMATION_TESTS
	FWoTBootstrapStatus LoadObjectForTesting(FWoTRuntimeVerifiedObject&& Object);
#endif
	const FString& MapIdentity() const { return ParsedMapIdentity; }
	const FString& FactId() const { return ParsedFactId; }
	const FString& Importer() const { return ParsedImporter; }
	uint32 ImporterVersion() const { return ParsedImporterVersion; }
	uint16 FormatVersion() const { return ParsedFormatVersion; }
	const FString& SourceExport() const { return ParsedSourceExport; }
	const FString& SourcePackage() const { return ParsedSourcePackage; }
	const FString& SourceSha256() const { return ParsedSourceSha256; }
	uint32 ActorCount() const { return ParsedActorCount; }
	uint32 AttachmentCount() const { return ParsedAttachmentCount; }
	uint32 BSPNodeCount() const { return ParsedBSPNodeCount; }
	uint32 BSPSurfaceCount() const { return ParsedBSPSurfaceCount; }
	uint32 NavigationSeedCount() const { return ParsedNavigationSeedCount; }
	uint32 SemanticObjectCount() const { return ParsedSemanticObjectCount; }
	uint32 ReachSpecCount() const { return ParsedReachSpecCount; }
	uint32 ZoneCount() const { return ParsedZoneCount; }
	uint32 PortalSurfaceCount() const { return ParsedPortalSurfaceCount; }
	uint32 MoverCount() const { return ParsedMoverCount; }
	uint32 LightCount() const { return ParsedLightCount; }
	uint32 SoundCount() const { return ParsedSoundCount; }
	const TArray<FVector3f>& Vectors() const { return ParsedVectors; }
	const TArray<FVector3f>& Points() const { return ParsedPoints; }
	const TArray<FWoTRuntimeWorldNode>& Nodes() const { return ParsedNodes; }
	const TArray<FWoTRuntimeWorldSurface>& Surfaces() const { return ParsedSurfaces; }
	const TArray<FWoTRuntimeWorldVertex>& Vertices() const { return ParsedVertices; }
	const TArray<FWoTRuntimeWorldZone>& Zones() const { return ParsedZones; }
	const TArray<FWoTRuntimeWorldLeaf>& Leaves() const { return ParsedLeaves; }
	const TArray<uint32>& ModelLights() const { return ParsedModelLights; }
	bool BSPRootOutside() const { return bParsedRootOutside; }
	const TArray<FWoTRuntimeMoverModel>& MoverModels() const
	{
		return ParsedMoverModels;
	}
	uint32 MoverAtlasWidth() const { return ParsedMoverAtlasWidth; }
	uint32 MoverAtlasHeight() const { return ParsedMoverAtlasHeight; }
	const TArray<uint8>& MoverAtlasBGRA() const
	{
		return ParsedMoverAtlasBGRA;
	}
	const TArray<FWoTRuntimeMoverAnimationGroup>& MoverAnimationGroups() const
	{
		return ParsedMoverAnimationGroups;
	}
	const TArray<FWoTRuntimeMoverAnimationTerm>& MoverAnimationTerms() const
	{
		return ParsedMoverAnimationTerms;
	}
	const TArray<FWoTRuntimeWorldSemanticObject>& SemanticObjects() const
	{
		return ParsedSemanticObjects;
	}
	const TArray<FWoTRuntimeWorldActor>& Actors() const { return ParsedActors; }
	const TArray<FWoTRuntimeWorldReachSpec>& ReachSpecs() const
	{
		return ParsedReachSpecs;
	}

private:
	FWoTBootstrapStatus LoadVerifiedObject(FWoTRuntimeVerifiedObject&& Verified);
	TOptional<FWoTRuntimeVerifiedObject> Object;
	FString ParsedMapIdentity;
	FString ParsedFactId;
	FString ParsedImporter;
	uint32 ParsedImporterVersion = 0;
	uint16 ParsedFormatVersion = 0;
	FString ParsedSourceExport;
	FString ParsedSourcePackage;
	FString ParsedSourceSha256;
	uint32 ParsedActorCount = 0;
	uint32 ParsedAttachmentCount = 0;
	uint32 ParsedBSPNodeCount = 0;
	uint32 ParsedBSPSurfaceCount = 0;
	uint32 ParsedNavigationSeedCount = 0;
	uint32 ParsedSemanticObjectCount = 0;
	uint32 ParsedReachSpecCount = 0;
	uint32 ParsedZoneCount = 0;
	uint32 ParsedPortalSurfaceCount = 0;
	uint32 ParsedMoverCount = 0;
	uint32 ParsedLightCount = 0;
	uint32 ParsedSoundCount = 0;
	TArray<FVector3f> ParsedVectors;
	TArray<FVector3f> ParsedPoints;
	TArray<FWoTRuntimeWorldNode> ParsedNodes;
	TArray<FWoTRuntimeWorldSurface> ParsedSurfaces;
	TArray<FWoTRuntimeWorldVertex> ParsedVertices;
	TArray<FWoTRuntimeWorldZone> ParsedZones;
	TArray<FWoTRuntimeWorldLeaf> ParsedLeaves;
	TArray<uint32> ParsedModelLights;
	bool bParsedRootOutside = false;
	TArray<FWoTRuntimeMoverModel> ParsedMoverModels;
	uint32 ParsedMoverAtlasWidth = 0;
	uint32 ParsedMoverAtlasHeight = 0;
	TArray<uint8> ParsedMoverAtlasBGRA;
	TArray<FWoTRuntimeMoverAnimationGroup> ParsedMoverAnimationGroups;
	TArray<FWoTRuntimeMoverAnimationTerm> ParsedMoverAnimationTerms;
	TArray<FWoTRuntimeWorldSemanticObject> ParsedSemanticObjects;
	TArray<FWoTRuntimeWorldActor> ParsedActors;
	TArray<FWoTRuntimeWorldReachSpec> ParsedReachSpecs;
};
