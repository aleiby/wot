#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTPathNode.generated.h"

class AWoTPathNode;

USTRUCT()
struct FWoTPathEdge
{
	GENERATED_BODY()

	// The other stamped NavigationPoint, resolved from the corresponding retail
	// Paths()/upstreamPaths() ReachSpec endpoint in the exported T3D.
	UPROPERTY(EditAnywhere, Category = "WoT")
	TObjectPtr<AWoTPathNode> Other = nullptr;

	// ReachSpec.Distance from the retail level package.
	UPROPERTY(EditAnywhere, Category = "WoT")
	float Distance = 0.0f;
};

// Patcher-stamped, collision-free data actor carrying the FULL retail
// NavigationPointList for MP maps. G34's Seal.GetTouchingPathNode walks every
// NavigationPoint base (Seal.uc:184-201); bDark distinguishes the subset legal
// for the Myrddraal's shadow teleport (Myrddraal.uc:229-306). The ordered edge
// graph carries the retail ReachSpec facts needed by the G40/G41 iterators.
// Pawn movement remains on UE5 NavMesh. Tools/import/import_path_nodes.py stamps
// the node facts from the retail T3D and package-derived ReachSpec JSON.
UCLASS()
class AWoTPathNode : public AActor
{
	GENERATED_BODY()

public:
	AWoTPathNode();

	// WotPathNode.bDark: only dark (shadow) nodes are legal teleport destinations
	// (RejectTeleportPosition rejects non-dark, Myrddraal.uc:301-304).
	UPROPERTY(EditAnywhere, Category = "WoT")
	bool bDark = false;
	UPROPERTY(EditAnywhere, Category = "WoT")
	bool bSnipe = false;

	// NavigationPoint cylinder base = Location.Z - CollisionHeight. Instance-editable
	// because the T3D authors a few per-node overrides (Seal.uc:145-148,184-201).
	UPROPERTY(EditAnywhere, Category = "WoT")
	float NodeCollisionHeight = 50.0f;

	// NavigationPoint.Paths() order, with each endpoint resolved through its
	// package ReachSpec to the stamped node named by the T3D export.
	UPROPERTY(EditAnywhere, Category = "WoT")
	TArray<FWoTPathEdge> OutEdges;

	// NavigationPoint.upstreamPaths() order. Other is each ReachSpec's Start.
	UPROPERTY(EditAnywhere, Category = "WoT")
	TArray<FWoTPathEdge> InEdges;

	// NavigationPoint.ExtraCost from the shipped T3D; zero when unauthored
	// (the engine NavigationPoint default).
	UPROPERTY(EditAnywhere, Category = "WoT")
	int32 ExtraCost = 0;

	// The retail NavigationPoint-family class that produced this nav-list twin
	// (PathNode, WotPathNode, PlayerStart, Teleporter, ...).
	UPROPERTY(EditAnywhere, Category = "WoT")
	FName SourceClass;

	bool GetDarkFlag() const { return bDark; }
	bool IsTeleporter() const
	{
		return SourceClass.ToString().Contains(TEXT("Teleporter"), ESearchCase::IgnoreCase);
	}
};
