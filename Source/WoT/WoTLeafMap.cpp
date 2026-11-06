#include "WoTLeafMap.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
bool IsOutsideAfterChild(const bool bOutside, const bool bCsg, const int32 ChildSide)
{
	return ChildSide != 0 ? (bOutside || bCsg) : (bOutside && !bCsg);
}
}

int32 AWoTLeafMap::PointLeaf(const FVector& P) const
{
	const int32 NumNodes = NodeChildren.Num() / 2;
	if (NumNodes == 0 || NodePlanes.Num() != NumNodes * 4
		|| NodeLeaves.Num() != NumNodes * 2)
	{
		return INDEX_NONE;
	}
	// UE1 PlaneDot runs in float32; match it for on-plane edge cases.
	const FVector3f Pf(P);
	int32 Node = 0, Parent = 0, Side = 0;
	while (Node != INDEX_NONE)
	{
		const float* Pl = &NodePlanes[Node * 4];
		Side = (Pl[0] * Pf.X + Pl[1] * Pf.Y + Pl[2] * Pf.Z - Pl[3]) >= 0.0f ? 1 : 0;
		Parent = Node;
		Node = NodeChildren[Node * 2 + Side];
	}
	const int32 Leaf = NodeLeaves[Parent * 2 + Side];
	return (Leaf >= 0 && Leaf < NumLeaves) ? Leaf : INDEX_NONE;
}

// This is the zero-extent BSP walk used by the corona visibility trace
// (Engine/Src/UnTrace.cpp:380-446). RootOutside flows down the baked tree;
// solid nodes make their front child outside and their back child inside.
// Reaching a leaf on the inside blocks the segment. The walk stays in float32,
// and NodeChildren stores each node's back child before its front child.
bool AWoTLeafMap::LineCheckNode(int32 Node, FVector3f End, FVector3f Start,
                                bool bOutside) const
{
	while (Node != INDEX_NONE)
	{
		const float* Pl = &NodePlanes[Node * 4];
		const float StartPlaneDistance =
			Pl[0] * Start.X + Pl[1] * Start.Y + Pl[2] * Start.Z - Pl[3];
		const float EndPlaneDistance =
			Pl[0] * End.X + Pl[1] * End.Y + Pl[2] * End.Z - Pl[3];
		const bool bCsg = NodeCsg[Node] != 0;
		if (StartPlaneDistance > -0.001f && EndPlaneDistance > -0.001f)
		{
			bOutside = IsOutsideAfterChild(bOutside, bCsg, 1);
			Node = NodeChildren[Node * 2 + 1];
		}
		else if (StartPlaneDistance < 0.001f && EndPlaneDistance < 0.001f)
		{
			bOutside = IsOutsideAfterChild(bOutside, bCsg, 0);
			Node = NodeChildren[Node * 2];
		}
		else
		{
			// Visit the portion containing Start before continuing through the far
			// side. The split keeps the backward-pointing form (toward Start, with the
			// plane distances in that order) so every float result, signed zero
			// included, matches the baked-tree traces recorded for parity.
			const FVector3f TowardStart = Start - End;
			const float TowardStartFraction = StartPlaneDistance
				/ (EndPlaneDistance - StartPlaneDistance);
			const FVector3f SplitPoint = Start + TowardStart * TowardStartFraction;
			const int32 NearSide = StartPlaneDistance > 0.0f ? 1 : 0;
			const bool bNearOutside = IsOutsideAfterChild(bOutside, bCsg, NearSide);
			if (!LineCheckNode(NodeChildren[Node * 2 + NearSide], SplitPoint, Start,
			                   bNearOutside))
			{
				return false;
			}
			const int32 FarSide = 1 - NearSide;
			bOutside = IsOutsideAfterChild(bOutside, bCsg, FarSide);
			Node = NodeChildren[Node * 2 + FarSide];
			Start = SplitPoint;
		}
	}
	return bOutside;
}

bool AWoTLeafMap::LineCheck(const FVector& Start, const FVector& End) const
{
	const int32 NumNodes = NodeChildren.Num() / 2;
	if (NumNodes == 0 || NodePlanes.Num() != NumNodes * 4
		|| NodeCsg.Num() != NumNodes)
	{
		return true;   // no csg bake — fail open, callers keep their gate
	}
	// Retail argument order is (End=light, Start=camera).
	return LineCheckNode(0, FVector3f(End), FVector3f(Start), bRootOutside);
}

bool AWoTLeafMap::IsPointInSolid(const FVector& P) const
{
	return !LineCheck(P, P);
}

int32 AWoTLeafMap::GetViewerLeaf()
{
	if (CacheFrame == GFrameCounter)
	{
		return CachedLeaf;
	}
	CacheFrame = GFrameCounter;
	CachedLeaf = INDEX_NONE;
	if (ViewerOverride.IsSet())
	{
		CachedLeaf = PointLeaf(ViewerOverride.GetValue());
		return CachedLeaf;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC || !PC->PlayerCameraManager)
	{
		return CachedLeaf;
	}
	// UE1 keys on the player pawn's Region (its Location); a non-pawn view
	// target (shot camera without an override, cinematics) falls back to the
	// camera position.
	AActor* ViewTarget = PC->PlayerCameraManager->GetViewTarget();
	const FVector P = (ViewTarget && ViewTarget->IsA<APawn>())
		? ViewTarget->GetActorLocation()
		: PC->PlayerCameraManager->GetCameraLocation();
	CachedLeaf = PointLeaf(P);
	return CachedLeaf;
}

void AWoTLeafMap::SetViewerOverride(const FVector& P)
{
	ViewerOverride = P;
	CacheFrame = MAX_uint64;   // invalidate the per-frame cache
}
