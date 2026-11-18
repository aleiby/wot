#include "WoTFallingPhysics.h"

#include "WoTZoneVolume.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/DamageType.h"

bool WoTFallingPhysics::DestroyOutsideWorld(AActor& Actor)
{
	if (!UWoTZoneIndex::IsPointOutsideWorld(Actor.GetWorld(), Actor.GetActorLocation()))
	{
		return false;
	}
	// UE5's own handler stops the actor's physics and collision and destroys it
	// on the server, as the original's does; pawns override it to die instead.
	Actor.FellOutOfWorld(*GetDefault<UDamageType>());
	return true;
}

bool WoTFallingPhysics::LiftOutOfFloor(UPrimitiveComponent& Shape)
{
	UWorld* World = Shape.GetWorld();
	AActor* Owner = Shape.GetOwner();
	if (World == nullptr || Owner == nullptr)
	{
		return false;
	}
	// Start one full shape height (plus a margin) above: a prop sunk deeper than
	// its own height is not resting on that floor and keeps falling.
	const FVector Here = Shape.GetComponentLocation();
	const FVector Above = Here + FVector(0.0, 0.0, Shape.Bounds.BoxExtent.Z * 2.0 + 1.0);
	FComponentQueryParams Params(SCENE_QUERY_STAT(WoTFallingLiftOutOfFloor), Owner);
	Params.AddIgnoredActors(Shape.GetMoveIgnoreActors());
	TArray<FHitResult> Hits;
	World->ComponentSweepMulti(Hits, &Shape, Above, Here, Shape.GetComponentQuat(), Params);
	for (const FHitResult& Hit : Hits)
	{
		if (!Hit.bBlockingHit)
		{
			continue;
		}
		// The first blocking contact decides. Starting inside something (a low
		// ceiling) or meeting a wall or slope proves nothing about a floor.
		if (Hit.bStartPenetrating || Hit.ImpactNormal.Z <= 0.7)
		{
			return false;
		}
		const FVector Lift = Hit.Location - Here;
		if (Lift.Z <= UE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		Owner->AddActorWorldOffset(Lift, false);
		return true;
	}
	return false;
}
