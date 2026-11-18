// Code-only owners for one authenticated world-v3 semantic generation.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "../WoTBSPPayload.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "WoTMaterializedWorldHost.generated.h"

class UWoTMapLightTable;

/** Neutral host for a non-actor semantic identity. */
UCLASS(Transient)
class WOT_API UWoTMaterializedObjectHost final : public UObject
{
	GENERATED_BODY()

public:
	FString SemanticIdentity;
	FString SemanticClassIdentity;
	TArray<FWoTRuntimeWorldProperty> AppliedProperties;
};

/** Neutral host for an actor identity. It never performs an asset load. */
UCLASS(Transient, NotPlaceable)
class WOT_API AWoTMaterializedActorHost final : public AActor
{
	GENERATED_BODY()

public:
	AWoTMaterializedActorHost();
	virtual void BeginPlay() override;

	FString SemanticIdentity;
	FString SemanticClassIdentity;
	TArray<FWoTRuntimeWorldProperty> AppliedProperties;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> ResolvedPropertyObjects;
	uint32 MaterializedBeginPlayCount = 0;
};

/** Complete BSP/collision/zone/navigation input retained by the candidate. */
UCLASS(Transient)
class WOT_API UWoTMaterializedGeometryHost final : public UObject
{
	GENERATED_BODY()

public:
	TArray<FVector3f> Vectors;
	TArray<FVector3f> Points;
	TArray<FWoTRuntimeWorldNode> Nodes;
	TArray<FWoTRuntimeWorldSurface> Surfaces;
	TArray<FWoTRuntimeWorldVertex> Vertices;
	TArray<FWoTRuntimeWorldZone> Zones;
	TArray<FWoTRuntimeWorldReachSpec> ReachSpecs;
	TArray<FWoTBSPPayloadData> BSPParts;
	TArray<FString> NavigationSeedIdentities;
	TArray<uint32> CollisionSurfaceIndices;
};

/** One extra, non-semantic actor keeps every materialized object GC-reachable. */
UCLASS(Transient, NotPlaceable)
class WOT_API AWoTMaterializedWorldRoot final : public AActor
{
	GENERATED_BODY()

public:
	AWoTMaterializedWorldRoot();

	UPROPERTY(Transient)
	TArray<TObjectPtr<AWoTMaterializedActorHost>> ActorHosts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UWoTMaterializedObjectHost>> ObjectHosts;

	UPROPERTY(Transient)
	TObjectPtr<UWoTMaterializedGeometryHost> Geometry;

	UPROPERTY(Transient)
	TObjectPtr<UWoTMapLightTable> MapLightTable;

	FString MapIdentity;
	FString MapRootHex;
	FString WorldLogicalId;
	/** Retail Level.Title from the world record's first LevelInfo (may be
	 * empty). Every UI surface reads it through WoTWorldDisplayTitle. */
	FString LevelTitle;
};
