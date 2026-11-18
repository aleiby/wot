#pragma once

#include "CoreMinimal.h"
#include "../Payload/WoTRuntimeWorldResource.h"

class AActor;
class AWoTMover;
class UWorld;
class UTexture2D;
class UWoTMapLightTable;
class FWoTRuntimeClassDefaults;
struct FWoTRetailTextureFacts;

struct WOT_API FWoTWorldProjectionSource
{
	FString MapIdentity;
	TConstArrayView<FWoTRuntimeWorldActor> Records;
	TConstArrayView<FVector3f> Points;
	TConstArrayView<FWoTRuntimeWorldNode> Nodes;
	TConstArrayView<FWoTRuntimeWorldVertex> Vertices;
	TConstArrayView<FWoTRuntimeWorldZone> Zones;
	TConstArrayView<FWoTRuntimeWorldReachSpec> ReachSpecs;
	TConstArrayView<FWoTRuntimeWorldSurface> Surfaces;
	TConstArrayView<FWoTRuntimeWorldLeaf> Leaves;
	TConstArrayView<uint32> ModelLights;
	TConstArrayView<FWoTRuntimeMoverModel> MoverModels;
	TConstArrayView<FWoTRuntimeMoverAnimationGroup> MoverAnimationGroups;
	TConstArrayView<FWoTRuntimeMoverAnimationTerm> MoverAnimationTerms;
	TConstArrayView<UTexture2D*> MoverLightResponses;
	TConstArrayView<FWoTRuntimeWorldSemanticObject> SemanticObjects;
	UTexture2D* MoverLightmap = nullptr;
	UTexture2D* MoverBlackLightResponse = nullptr;
	bool bBSPRootOutside = false;
	bool bHasDedicatedMoverCollision = false;
	TFunction<bool(FStringView, FWoTRetailTextureFacts&)> FindTexture;
	/** Optional DT_Sprite-only facts seam; production falls back to FindTexture. */
	TFunction<bool(FStringView, FWoTRetailTextureFacts&)>
		FindDecorationTexture;
};

struct WOT_API FWoTWorldProjectionResult
{
	TArray<AActor*> DeferredActors;
	uint32 PlayerStartCount = 0;
	bool bUsedPlayerStartFallback = false;
	TMap<FName, uint32> RuleCounts;
	uint32 InertRecordCount = 0;
	uint32 UnsupportedRecordCount = 0;
	// Leaf-class disposition ledgers keep the retail review auditable. These
	// are deliberately counts, rather than the bounded diagnostic sample below.
	TMap<FName, uint32> InertClassCounts;
	TMap<FName, uint32> UnsupportedClassCounts;
	uint32 DroppedPoolEntries = 0;
	uint32 EmptyPoolCount = 0;
	uint32 SkippedSprayerCount = 0;
	TMap<FName, uint32> SkippedRuleCounts;
	TArray<FString> SkippedSamples;
	TArray<FString> UnsupportedClassNames;
	TObjectPtr<UWoTMapLightTable> MapLightTable = nullptr;
	bool bLightTablePartial = false;
	// Retail Level.Title (ArenaScoreBoard.DrawHeader uc:315-318): the Title of
	// the first Engine.LevelInfo record, i.e. Level->GetLevelInfo() ==
	// Actors(0). Empty when the world carries no LevelInfo or its Title is
	// empty/malformed (retail then draws no map line).
	FString LevelTitle;

	uint32 RuleCount(FName Rule) const
	{
		if (const uint32* Count = RuleCounts.Find(Rule)) return *Count;
		return 0;
	}
	uint32 SkippedRuleCount(FName Rule) const
	{
		if (const uint32* Count = SkippedRuleCounts.Find(Rule)) return *Count;
		return 0;
	}
	uint32 InertClassCount(FName ClassLeaf) const
	{
		if (const uint32* Count = InertClassCounts.Find(ClassLeaf)) return *Count;
		return 0;
	}
	uint32 UnsupportedClassCount(FName ClassLeaf) const
	{
		if (const uint32* Count = UnsupportedClassCounts.Find(ClassLeaf)) return *Count;
		return 0;
	}
};

/**
 * The brush-local half of UE1 ABrush::ToWorld for a mover:
 * world = Location + PostScale * Rotation * MainScale * (local - PrePivot)
 * (ABrush ToWorld/BuildCoords; UE1's dynamic BSP feeds FPoly::Transform the
 * PrePivot pre-subtract and Location post-add). The projected actor carries
 * Location and Rotation, so its mesh is the remaining
 * Rotation^-1 * PostScale * Rotation * MainScale * (local - PrePivot), with
 * InitialRotation conjugating the world-axis PostScale.
 */
struct WOT_API FWoTMoverBrushFrame
{
	FVector PrePivot = FVector::ZeroVector;
	FVector MainScale = FVector::OneVector;
	FVector PostScale = FVector::OneVector;
	FRotator InitialRotation = FRotator::ZeroRotator;
};

/**
 * Applies Frame to a mover model in place: positions and section plane
 * origins as points, normals and section plane normals by the inverse
 * transpose, and FPoly::Transform's vertex-order flip when the combined scale
 * orientation is negative. Texture UVs are frame-invariant (FPoly::Transform
 * moves TextureU/V by the same VectorXform) and are left unchanged. Fails on a
 * zero or non-finite scale.
 */
WOT_API bool WoTApplyMoverBrushFrame(const FWoTMoverBrushFrame& Frame,
	FWoTRuntimeMoverModel& InOutModel, FString& OutError);

#if WITH_AUTOMATION_TESTS
/** Drives the production dynamic-light mover material path for focused specs. */
WOT_API bool WoTBuildMoverMeshForTesting(AWoTMover& Mover,
	const FWoTRuntimeMoverModel& Model, FString& OutError);
#endif

/** Universal case-insensitive retail-class projection table. */
class WOT_API FWoTWorldProjector final
{
public:
	/**
	 * Spawns projections deferred into the persistent level. The materializer
	 * finishes the returned actors at its existing single exposure point.
	 */
	static bool ProjectDeferred(UWorld& World,
		TConstArrayView<FWoTRuntimeWorldActor> Records,
		FWoTWorldProjectionResult& Out, FString& OutError);
	static bool ProjectDeferred(UWorld& World,
		const FWoTWorldProjectionSource& Source,
		const FWoTRuntimeClassDefaults& Defaults,
		FWoTWorldProjectionResult& Out, FString& OutError);
};
