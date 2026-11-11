#include "WoTUE1Collision.h"

#include "AI/WoTPathNodeIterator.h"
#include "WoTLeafMap.h"
#include "WoTMover.h"
#include "WoTRainCyd.h"

#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "EngineUtils.h"

namespace WoTUE1Collision
{
	bool LevelPointCheckFits(UWorld& World, const FVector& Location,
		const FVector& Extent)
	{
		// UModel::PointCheck tests the box against the level's BSP: it fails a
		// box that crosses a surface or sits in solid space. The collision
		// triangles have no interior, so solid containment comes from the BSP
		// leaf classification of the centre (a box wholly in solid has its centre
		// there; one partly in solid crosses a surface).
		if (TActorIterator<AWoTLeafMap> It(&World); It && It->IsPointInSolid(Location))
		{
			return false;
		}
		// The level is the runtime BSP world (or the editor maps'
		// StaticMeshActors), the same actors TRACE_Level accepts; movers are
		// actors, absent with bActors=0.
		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTUE1LevelPointCheck), false);
		World.OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity, Objects,
			FCollisionShape::MakeBox(Extent), Params);
		for (const FOverlapResult& Overlap : Overlaps)
		{
			const AActor* Actor = Overlap.GetActor();
			if (WoTRainCydIsVisBlockingActor(Actor) && !Actor->IsA<AWoTMover>())
			{
				return false;
			}
		}
		return true;
	}

	bool FindSpotLevelOnly(UWorld& World, const FVector& Extent,
		FVector& InOutLocation, bool bAssumeFit)
	{
		if (Extent.IsZero())
		{
			return LevelPointCheckFits(World, InOutLocation, Extent);
		}
		if (bAssumeFit && LevelPointCheckFits(World, InOutLocation, Extent))
		{
			return true;
		}
		// ULevel::AdjustSpot: SingleLineCheck(TRACE_VisBlocking) from the current
		// spot to TraceDest; a hit pushes the spot along the hit normal by the
		// unused part of the probe plus 5%.
		auto AdjustSpot = [&World](FVector& Adjusted, const FVector& TraceDest,
			double TraceLength)
		{
			FHitResult Hit;
			if (WoTVisBlockingTrace(&World, Adjusted, TraceDest, nullptr, Hit))
			{
				Adjusted += Hit.ImpactNormal * (1.05 - Hit.Time) * TraceLength;
			}
		};

		FVector Adjusted = InOutLocation;
		for (int32 Sign = -1; Sign < 2; Sign += 2)
		{
			AdjustSpot(Adjusted, Adjusted + FVector(Sign * Extent.X, 0, 0), Extent.X);
			AdjustSpot(Adjusted, Adjusted + FVector(0, Sign * Extent.Y, 0), Extent.Y);
			AdjustSpot(Adjusted, Adjusted + FVector(0, 0, Sign * Extent.Z), Extent.Z);
		}
		if (LevelPointCheckFits(World, Adjusted, Extent))
		{
			InOutLocation = Adjusted;
			return true;
		}

		const double TraceLength = Extent.Size() + 2.0;
		for (int32 X = -1; X < 2; X += 2)
		{
			for (int32 Y = -1; Y < 2; Y += 2)
			{
				for (int32 Z = -1; Z < 2; Z += 2)
				{
					AdjustSpot(Adjusted, Adjusted
						+ FVector(X * Extent.X, Y * Extent.Y, Z * Extent.Z), TraceLength);
				}
			}
		}
		if ((Adjusted - InOutLocation).SizeSquared() > 1.5 * Extent.SizeSquared())
		{
			return false;
		}
		if (LevelPointCheckFits(World, Adjusted, Extent))
		{
			InOutLocation = Adjusted;
			return true;
		}
		return false;
	}
}
