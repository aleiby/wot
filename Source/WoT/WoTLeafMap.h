#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "WoTLeafMap.generated.h"

// The shipped map's BSP point->leaf lookup, placed once per map by the
// importer from exports/leaves/<Map>.leaves.json (leafbake.py). UE1 resolves
// an actor's Region.iLeaf by descending the node tree from the root, taking
// the front child while PlaneDot >= 0; the leaf is the last node's
// iLeaf[side] (UModel::PointRegion). Retail keys corona visibility on the
// PLAYER PAWN's leaf — Viewport->Actor->Region.iLeaf, i.e. the pawn location,
// not the eye — so the shot harness feeds the manifest pawn point through
// SetViewerOverride; live play uses the view-target pawn, then the camera.
UCLASS()
class AWoTLeafMap : public AInfo
{
	GENERATED_BODY()

public:
	// Flat node arrays (4/2/2 entries per node), importer-set.
	// Planes: X,Y,Z,W with PlaneDot = (X,Y,Z)|P - W. Children/Leaves:
	// [back, front] per node, INDEX_NONE = none.
	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	TArray<float> NodePlanes;

	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	TArray<int32> NodeChildren;

	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	TArray<int32> NodeLeaves;

	// Per-node FBspNode::IsCsg(0) = NumVertices>0 && !(NodeFlags &
	// (NF_NotCsg|NF_IsNew)). Only csg nodes carry solidity in the line
	// check: non-solid/translucent/masked sheet and portal nodes never flip
	// the Outside state, so they never block.
	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	TArray<uint8> NodeCsg;

	// UModel::RootOutside — the Outside state entering the root node
	// (0 for the shipped subtractive worlds).
	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	bool bRootOutside = false;

	UPROPERTY(EditAnywhere, Category = "WoT|Leaf")
	int32 NumLeaves = 0;

	// UE1 walk; INDEX_NONE when outside every leaf (retail then draws no
	// coronas at all — the DrawFrame optics block is gated on iLeaf!=NONE).
	int32 PointLeaf(const FVector& P) const;

	// The zero-extent UModel::LineCheck over the baked tree: true =
	// unblocked. Blocks only where the segment enters CSG-solid volume —
	// the corona TRACE_VisBlocking gate (which also checks no actors:
	// movers, meshes, and pawns never hide a corona in retail). Fails open
	// when the map carries no csg bake.
	bool LineCheck(const FVector& Start, const FVector& End) const;

	// The zero-extent core of UModel::PointCheck: P lies in CSG-solid space
	// (the zero-length LineCheck). False without a csg bake.
	bool IsPointInSolid(const FVector& P) const;

	// The viewer's leaf, cached per frame.
	int32 GetViewerLeaf();

	// Pin the viewer point (the golden-capture pawn position); the shot
	// harness sets this per pose since its camera flies at pawn+eye_height.
	void SetViewerOverride(const FVector& P);

private:
	bool LineCheckNode(int32 Node, FVector3f End, FVector3f Start,
	                   bool bOutside) const;

	TOptional<FVector> ViewerOverride;
	uint64 CacheFrame = MAX_uint64;
	int32 CachedLeaf = INDEX_NONE;
};
