#include "WoTPathNode.h"

AWoTPathNode::AWoTPathNode()
{
	// A pure data/point actor: a bare scene root, no collision, invisible in
	// game — it exists only so retail node-list consumers can find its location,
	// cylinder-base height, darkness, cost, and ordered graph edges (the
	// AWoTTaggedMarker precedent).
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetActorEnableCollision(false);
	SetHidden(true);
	PrimaryActorTick.bCanEverTick = false;
}
