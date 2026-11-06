#include "WoTBrainComponent.h"
#include "../WoTRandom.h"
#include "WoTPathNodeIterator.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Engine/World.h"
#include "WoTSight.h"
#include "WoTNPCCharacter.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTCharacter.h"
#include "../WoTSealAltar.h"

namespace
{
	// The 15uu engagement unit and its square.
	constexpr float EngineThreshold = 15.0f;
	constexpr float EngineThresholdSquared = 225.0f;
	// Fallbacks for location/nav-point targets resolved as cylinders.
	constexpr float BogusGoalHalfHeight = 8.0f;
	constexpr float BogusGoalRadius = 15.0f;
}

// ---------------------------------------------------------------- cache

void FWoTGoalCache::Stamp(EWoTGoalCacheItem Item, const FVector& GoalLoc, const FVector& ObserverLoc)
{
	FWoTGoalCacheItem& I = Get(Item);
	I.bValid = true;
	I.GoalLocation = GoalLoc;
	I.ObserverLocation = ObserverLoc;

	// Pair layout is OK/Fail adjacent: flip the low bit for the sibling.
	const int32 Sibling = static_cast<int32>(Item) ^ 1;
	Items[Sibling].bValid = false;
}

bool FWoTGoalCache::IsCurrent(EWoTGoalCacheItem Item, const FVector& GoalLoc,
                              const FVector& ObserverLoc, bool bCheckObserver) const
{
	const FWoTGoalCacheItem& I = Get(Item);
	if (!I.bValid)
	{
		return false;
	}
	if (FVector::Dist(I.GoalLocation, GoalLoc) > CurrentThreshold)
	{
		return false;
	}
	if (bCheckObserver && FVector::Dist(I.ObserverLocation, ObserverLoc) > CurrentThreshold)
	{
		return false;
	}
	return true;
}

int32 FWoTGoalCache::CachedAnswer(EWoTGoalCacheItem OKItem, const FVector& GoalLoc,
                                  const FVector& ObserverLoc, bool bCheckObserver) const
{
	if (IsCurrent(OKItem, GoalLoc, ObserverLoc, bCheckObserver))
	{
		return 1;
	}
	const EWoTGoalCacheItem FailItem = static_cast<EWoTGoalCacheItem>(static_cast<int32>(OKItem) ^ 1);
	if (IsCurrent(FailItem, GoalLoc, ObserverLoc, bCheckObserver))
	{
		return 0;
	}
	return -1;
}

void FWoTGoalCache::Invalidate()
{
	for (FWoTGoalCacheItem& I : Items)
	{
		I.bValid = false;
	}
}

// ---------------------------------------------------------------- notifier

bool FWoTDurationNotifier::Tick(float DeltaSeconds)
{
	// Elapsed accrues regardless; enable reseeds the phase. Period 0 fires
	// every tick, -1 never.
	Elapsed += DeltaSeconds;
	if (bEnabled && Period != -1.0f && Elapsed >= Period)
	{
		Elapsed = 0.0f;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- component

UWoTBrainComponent::UWoTBrainComponent()
{
	// The controller drives ReflectTick explicitly, first in its own tick.
	PrimaryComponentTick.bCanEverTick = false;

	// The base goal-slot table (LegendPawn.uc defaultproperties). Usage codes: ZeroIfGreater = "only within
	// distance", ZeroIfLess = "only beyond". Waypoint/Focus carry no priority —
	// they are mechanical movement inputs, never priority-selected.
	auto Slot = [this](EWoTGoalIndex Idx, bool bPawn, float Pri, float Dist,
	                   EWoTGoalDistanceUsage Usage, float Speed)
	{
		FWoTGoal& G = GetGoal(Idx);
		G.bSlotUsed = true;
		G.bPawnsMustBeAlive = bPawn;
		G.Priority = Pri;
		G.PriorityDistance = Dist;
		G.DistanceUsage = Usage;
		G.SuggestedSpeed = Speed;
	};
	Slot(EWoTGoalIndex::Threat,       true,  80.0f, 8000.0f, EWoTGoalDistanceUsage::ZeroIfGreater, 320.0f);
	// ExternalDirective: descriptor None — slot unused (SP scripted sequences).
	Slot(EWoTGoalIndex::Refuge,       false, 100.0f, 8000.0f, EWoTGoalDistanceUsage::ZeroIfGreater, 320.0f);
	Slot(EWoTGoalIndex::Guarding,     false, 90.0f,  480.0f,  EWoTGoalDistanceUsage::ZeroIfLess,    320.0f);
	Slot(EWoTGoalIndex::Colleague,    true,  40.0f,  8000.0f, EWoTGoalDistanceUsage::ZeroIfGreater, 320.0f);
	Slot(EWoTGoalIndex::Intermediate, false, 85.0f,  8000.0f, EWoTGoalDistanceUsage::ZeroIfGreater, 320.0f);
	Slot(EWoTGoalIndex::Waiting,      false, 50.0f,  8000.0f, EWoTGoalDistanceUsage::ZeroIfGreater, 160.0f);
	Slot(EWoTGoalIndex::CurrentWaypoint, false, -1.0f, -1.0f, EWoTGoalDistanceUsage::Unused, -1.0f);
	Slot(EWoTGoalIndex::CurrentFocus,    false, -1.0f, -1.0f, EWoTGoalDistanceUsage::Unused, -1.0f);

	// Notifier cadences (LegendPawn.uc:2634-2637; Rotation/Movement are the
	// per-tick re-aim pulses, period 0).
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Visibility)].Period = 2.0f;
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Rotation)].Period   = 0.0f;
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Movement)].Period   = 0.0f;
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Defensive)].Period  = 0.05f;
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)].Period  = 0.5f;
	Notifiers[static_cast<int32>(EWoTNotifierSlot::Waiting)].Period    = 0.25f;
}

void UWoTBrainComponent::InitGoalWithActor(EWoTGoalIndex Index, AActor* Target)
{
	FWoTGoal& G = GetGoal(Index);
	if (!G.bSlotUsed)
	{
		return;
	}
	// Re-targeting invalidates the cache only when the actor actually changes.
	if (G.GoalActor.Get() != Target || G.bUseGoalVector)
	{
		G.Cache.Invalidate();
	}
	G.GoalActor = Target;
	G.bUseGoalVector = false;
	G.bUseAssociatedRotation = false;
	if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetOwner()))
	{
		NPC->GoalInitialized(Index);
	}
}

void UWoTBrainComponent::InitGoalWithVector(EWoTGoalIndex Index, const FVector& Target)
{
	FWoTGoal& G = GetGoal(Index);
	if (!G.bSlotUsed)
	{
		return;
	}
	G.GoalActor = nullptr;
	G.GoalVector = Target;
	G.bUseGoalVector = true;
	G.bUseAssociatedRotation = false;
	G.Cache.Invalidate();
	if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetOwner()))
	{
		NPC->GoalInitialized(Index);
	}
}

void UWoTBrainComponent::InvalidateGoal(EWoTGoalIndex Index)
{
	FWoTGoal& G = GetGoal(Index);
	G.GoalActor = nullptr;
	G.bUseGoalVector = false;
	G.bUseAssociatedRotation = false;
	G.Cache.Invalidate();
}

void UWoTBrainComponent::AssignGoalFrom(EWoTGoalIndex Index, const UWoTBrainComponent* Other,
                                        EWoTGoalIndex OtherIndex)
{
	if (!Other)
	{
		return;
	}
	FWoTGoal& G = GetGoal(Index);
	const FWoTGoal& Src = Other->GetGoal(OtherIndex);
	if (!G.bSlotUsed || !Src.bSlotUsed)
	{
		return;
	}
	G.GoalActor = Src.GoalActor;
	G.GoalVector = Src.GoalVector;
	G.bUseGoalVector = Src.bUseGoalVector;
	G.AssociatedRotation = Src.AssociatedRotation;
	G.bUseAssociatedRotation = Src.bUseAssociatedRotation;
	G.Cache = Src.Cache;
	if (Index == EWoTGoalIndex::Threat)
	{
		if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetOwner()))
		{
			NPC->GoalInitialized(Index);
		}
	}
}

bool UWoTBrainComponent::TransferThreatGoalsFrom(const UWoTBrainComponent* Other)
{
	// LegendPawn.uc:656: guard on the SOURCE's Threat validity (Other.GetGoal(
	// GI_Threat).IsValid(Other)) before adopting it.
	if (!Other || !Other->IsGoalValid(EWoTGoalIndex::Threat))
	{
		return false;
	}
	AActor* ThreatActor = Other->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get();
	if (!ThreatActor)
	{
		// A valid Threat is always an actor (a pawn); a vector threat never occurs
		// but guard anyway rather than seed an actorless Threat slot.
		return false;
	}
	InitGoalWithActor(EWoTGoalIndex::Threat, ThreatActor);
	return true;
}

void UWoTBrainComponent::SetWaitingGoal(const FVector& Location, const FRotator& Facing)
{
	FWoTGoal& G = GetGoal(EWoTGoalIndex::Waiting);
	if (!G.bSlotUsed)
	{
		return;
	}
	G.GoalActor = nullptr;
	G.GoalVector = Location;
	G.bUseGoalVector = true;
	G.AssociatedRotation = Facing;
	G.bUseAssociatedRotation = true;
	G.Cache.Invalidate();
}

bool UWoTBrainComponent::IsGoalValid(EWoTGoalIndex Index) const
{
	const FWoTGoal& G = GetGoal(Index);
	if (!G.bSlotUsed)
	{
		return false;
	}
	if (G.HasVector())
	{
		return true;
	}
	if (!G.HasActor())
	{
		return false;
	}
	if (G.bPawnsMustBeAlive)
	{
		// A dead pawn target invalidates the goal.
		if (const AWoTCharacter* Char = Cast<AWoTCharacter>(G.GoalActor.Get()))
		{
			return Char->Health > 0;
		}
	}
	return true;
}

float UWoTBrainComponent::GetGoalPriority(EWoTGoalIndex Index) const
{
	// The distance-gated step function.
	const FWoTGoal& G = GetGoal(Index);
	if (!IsGoalValid(Index))
	{
		return 0.0f;
	}
	if (G.DistanceUsage == EWoTGoalDistanceUsage::Unused)
	{
		return G.Priority;
	}
	float Dist = 0.0f;
	if (!GetGoalDistance(Index, Dist))
	{
		return 0.0f;
	}
	if (Dist < G.PriorityDistance)
	{
		return G.DistanceUsage == EWoTGoalDistanceUsage::ZeroIfGreater ? G.Priority : 0.0f;
	}
	if (Dist > G.PriorityDistance)
	{
		return G.DistanceUsage == EWoTGoalDistanceUsage::ZeroIfGreater ? 0.0f : G.Priority;
	}
	return G.Priority;   // exactly at the gate distance
}

void UWoTBrainComponent::GetSortedGoalIndices(TArray<EWoTGoalIndex>& OutIndices,
                                              TArray<float>& OutPriorities) const
{
	struct FEntry
	{
		float Priority;
		EWoTGoalIndex Index;
	};
	TArray<FEntry, TInlineAllocator<WoTGoalSlotCount>> Entries;
	for (int32 i = 0; i < WoTGoalSlotCount; ++i)
	{
		const EWoTGoalIndex Idx = static_cast<EWoTGoalIndex>(i);
		// An empty slot scores the -1 sentinel.
		const float Pri = GetGoal(Idx).bSlotUsed ? GetGoalPriority(Idx) : -1.0f;
		Entries.Add({Pri, Idx});
	}
	// Stable descending sort seeded in slot order: equal priorities keep slot
	// order, so the lowest EWoTGoalIndex wins ties — the locked tiebreak the
	// original's strict-greater sort produced.
	Entries.StableSort([](const FEntry& A, const FEntry& B) { return A.Priority > B.Priority; });

	OutIndices.Reset(WoTGoalSlotCount);
	OutPriorities.Reset(WoTGoalSlotCount);
	for (const FEntry& E : Entries)
	{
		OutIndices.Add(E.Index);
		OutPriorities.Add(E.Priority);
	}
}

EWoTGoalIndex UWoTBrainComponent::GetPriorityGoalIndex() const
{
	TArray<EWoTGoalIndex> Indices;
	TArray<float> Priorities;
	GetSortedGoalIndices(Indices, Priorities);
	return Indices[0];
}

bool UWoTBrainComponent::GetOwnerCylinder(FVector& OutLocation, float& OutRadius, float& OutHalfHeight) const
{
	const AActor* Owner = GetOwner();
	if (!IsValid(Owner))
	{
		return false;
	}
	OutLocation = Owner->GetActorLocation();
	Owner->GetSimpleCollisionCylinder(OutRadius, OutHalfHeight);
	return true;
}

bool UWoTBrainComponent::GetGoalParams(EWoTGoalIndex Index, FVector& OutLocation,
                                       float& OutRadius, float& OutHalfHeight) const
{
	const FWoTGoal& G = GetGoal(Index);
	if (G.HasActor())
	{
		const AActor* Target = G.GoalActor.Get();
		OutLocation = Target->GetActorLocation();
		Target->GetSimpleCollisionCylinder(OutRadius, OutHalfHeight);
		// WotGoalAbstracter.DetermineGoalActorParams specializes the actor
		// cylinder used by every native goal query. Order is significant: the
		// non-pawn altar/alarm branches precede the friendly Grunt family.
		if (Target->IsA<AWoTSealAltar>())
		{
			OutRadius = 32.0f;
			OutHalfHeight = 32.0f;
		}
		else if (Target->IsA<AWoTAlarm>())
		{
			OutRadius = 160.0f;
		}
		else if (const AWoTNPCCharacter* TargetNPC = Cast<AWoTNPCCharacter>(Target);
			TargetNPC && !TargetNPC->IsPlayerControlled() && TargetNPC->IsFriendly(GetOwner()))
		{
			// The shipped predicate is target-side Grunt.IsFriendly(invoker),
			// which is observably asymmetric for the special team bytes.
			if (TargetNPC->bIsCaptain)
			{
				OutRadius = TargetNPC->FindHelpRadius;
			}
			else
			{
				OutRadius *= 4.0f;
			}
		}
		return true;
	}
	if (G.HasVector())
	{
		// Location goals resolve against the invoker's radius: 225/ownerRadius,
		// half-height 8.
		OutLocation = G.GoalVector;
		FVector OwnerLoc;
		float OwnerRadius = 0.0f, OwnerHalf = 0.0f;
		if (GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf) && OwnerRadius > 0.0f)
		{
			OutRadius = EngineThresholdSquared / OwnerRadius;
		}
		else
		{
			OutRadius = BogusGoalRadius;
		}
		OutHalfHeight = BogusGoalHalfHeight;
		return true;
	}
	return false;
}

bool UWoTBrainComponent::GetGoalDistance(EWoTGoalIndex Index, float& OutDistance) const
{
	// Cylinder gap, 2D: center distance minus both radii, floored at 0.
	// (Clarify-on-demand: exact original cylinder metric — revisit if gate
	// behavior mismatches in PIE.)
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	OutDistance = FMath::Max(0.0f, FVector::Dist2D(GoalLoc, OwnerLoc) - GoalRadius - OwnerRadius);
	return true;
}

float UWoTBrainComponent::DistanceBetweenCylinders(const FVector& A, float RadiusA, float HalfA,
	const FVector& B, float RadiusB, float HalfB)
{
	// Util.GetDistanceBetweenCylinders: preserve signed overlap, the diagonal
	// separated-Z case, and the coincident-centres zero-normal quirk.
	FVector Delta = B - A; Delta.Z = 0;
	const FVector Direction = Delta.GetSafeNormal();
	FVector SurfaceA = A + Direction * RadiusA, SurfaceB = B - Direction * RadiusB;
	if (B.Z - HalfB > A.Z + HalfA)
	{ SurfaceB.Z -= HalfB; SurfaceA.Z += HalfA; }
	else if (A.Z - HalfA > B.Z + HalfB)
	{ SurfaceA.Z -= HalfA; SurfaceB.Z += HalfB; }
	else
	{
		SurfaceA.Z = SurfaceB.Z = 0;
		return FVector::Dist(SurfaceA, SurfaceB) * (Delta.Size() < RadiusA + RadiusB ? -1 : 1);
	}
	return FVector::Dist(SurfaceA, SurfaceB);
}

bool UWoTBrainComponent::GetGoalDistanceToCylinder(EWoTGoalIndex Index, const FVector& Origin,
	float Radius, float HalfHeight, float& OutDistance) const
{
	FVector GoalOrigin; float GoalRadius, GoalHeight;
	if (!GetGoalParams(Index, GoalOrigin, GoalRadius, GoalHeight)) return false;
	OutDistance = DistanceBetweenCylinders(GoalOrigin, GoalRadius, GoalHeight, Origin, Radius, HalfHeight);
	return true;
}

bool UWoTBrainComponent::IsCloseEnough(const FVector& A, float RadiusA, const FVector& B, float RadiusB)
{
	// Reached iff the 2D gap is within (RadiusA*RadiusB)/15 — the scaled
	// engagement predicate.
	const float ThresholdProduct = RadiusA * RadiusB;
	if (ThresholdProduct <= 0.0f)
	{
		return false;
	}
	const float DistSq = FVector::DistSquared2D(A, B);
	return DistSq * (EngineThresholdSquared / ThresholdProduct) <= ThresholdProduct;
}

bool UWoTBrainComponent::IsGoalReached(EWoTGoalIndex Index) const
{
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!IsGoalValid(Index) ||
	    !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	return IsCloseEnough(OwnerLoc, OwnerRadius, GoalLoc, GoalRadius);
}

bool UWoTBrainComponent::QueryVisible(const FWoTGoal& Goal) const
{
	// The goal layer's LOS test: actor targets
	// use native LineOfSightTo (range gate, near body/side rays, far center ray);
	// location targets are a plain visibility trace to the point.
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!IsValid(Owner) || !World)
	{
		return false;
	}
	const APawn* OwnerPawn = Cast<APawn>(Owner);
	if (const AActor* TargetActor = Goal.HasActor() ? Goal.GoalActor.Get() : nullptr;
	    TargetActor && OwnerPawn)
	{
		return WoTSight::OmniSight(OwnerPawn, TargetActor, WoTSight::VisibilityOf(TargetActor));
	}
	// The original GoalContextInterf traces vector goals from Invoker.Location,
	// not eye height, using the native VisBlocking line check.
	return !WoTVisBlockingTraceHits(
		const_cast<UWorld*>(World), Owner->GetActorLocation(), Goal.GoalVector, Owner);
}

bool UWoTBrainComponent::QueryReachable(EWoTGoalIndex Index) const
{
	// Straight-line walkability, no routing: a NavMesh raycast from the owner
	// to the goal that reaches without hitting a nav boundary. Both ends
	// project to the navmesh, reproducing the original's "test as if
	// standing" intent.
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!IsValid(Owner) || !World)
	{
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return false;
	}
	FVector GoalLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f;
	if (!GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf))
	{
		return false;
	}
	FVector HitLocation;
	AController* Querier = Cast<AController>(Owner->GetInstigatorController());
	if (!NavSys->NavigationRaycast(World, Owner->GetActorLocation(), GoalLoc,
	                               HitLocation, nullptr, Querier))
	{
		return true;
	}
	// An actor goal off the navmesh is reachable when its stand spot is.
	FVector StandSpot;
	return GetGoalStandSpot(Index, StandSpot)
		&& !NavSys->NavigationRaycast(World, Owner->GetActorLocation(), StandSpot,
		                              HitLocation, nullptr, Querier);
}

bool UWoTBrainComponent::QueryPathable(EWoTGoalIndex Index) const
{
	// A route exists. Partial paths count as pathable — the pawn approaches,
	// then the strike model drops the goal.
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!IsValid(Owner) || !World)
	{
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return false;
	}
	const ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	if (!NavData)
	{
		return false;
	}
	FVector GoalLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f;
	if (!GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf))
	{
		return false;
	}
	FPathFindingQuery Query(Owner, *NavData, Owner->GetActorLocation(), GoalLoc);
	Query.SetAllowPartialPaths(true);
	const FPathFindingResult Result = NavSys->FindPathSync(Query);
	if (Result.IsSuccessful() && Result.Path.IsValid())
	{
		return true;
	}
	// An actor goal that does not project onto the navmesh routes to its stand spot.
	FVector StandSpot;
	if (!GetGoalStandSpot(Index, StandSpot))
	{
		return false;
	}
	FPathFindingQuery SpotQuery(Owner, *NavData, Owner->GetActorLocation(), StandSpot);
	SpotQuery.SetAllowPartialPaths(true);
	const FPathFindingResult SpotResult = NavSys->FindPathSync(SpotQuery);
	return SpotResult.IsSuccessful() && SpotResult.Path.IsValid();
}

bool UWoTBrainComponent::IsGoalVisible(EWoTGoalIndex Index)
{
	FWoTGoal& G = GetGoal(Index);
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!IsGoalValid(Index) ||
	    !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	// Visible currency checks BOTH goal and observer drift.
	const int32 Cached = G.Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc, OwnerLoc, true);
	if (Cached != -1)
	{
		return Cached == 1;
	}
	const bool bVisible = QueryVisible(G);
	G.Cache.Stamp(bVisible ? EWoTGoalCacheItem::VisibleOK : EWoTGoalCacheItem::VisibleFail,
	              GoalLoc, OwnerLoc);
	return bVisible;
}

bool UWoTBrainComponent::IsGoalReachable(EWoTGoalIndex Index)
{
	FWoTGoal& G = GetGoal(Index);
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!IsGoalValid(Index) ||
	    !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	const int32 Cached = G.Cache.CachedAnswer(EWoTGoalCacheItem::ReachableOK, GoalLoc, OwnerLoc, true);
	if (Cached != -1)
	{
		if (Cached == 1)
		{
			// Reachable implies visible: refresh the visible stamp too.
			G.Cache.Stamp(EWoTGoalCacheItem::VisibleOK, GoalLoc, OwnerLoc);
		}
		return Cached == 1;
	}
	// Reachability is gated on visibility.
	if (!IsGoalVisible(Index))
	{
		G.Cache.Stamp(EWoTGoalCacheItem::ReachableFail, GoalLoc, OwnerLoc);
		return false;
	}
	const bool bReachable = QueryReachable(Index);
	G.Cache.Stamp(bReachable ? EWoTGoalCacheItem::ReachableOK : EWoTGoalCacheItem::ReachableFail,
	              GoalLoc, OwnerLoc);
	return bReachable;
}

bool UWoTBrainComponent::IsGoalPathable(EWoTGoalIndex Index)
{
	FWoTGoal& G = GetGoal(Index);
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!IsGoalValid(Index) ||
	    !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	// Pathable currency ignores observer motion — a fresh path stays current
	// while the pawn walks it.
	const int32 Cached = G.Cache.CachedAnswer(EWoTGoalCacheItem::PathableOK, GoalLoc, OwnerLoc, false);
	if (Cached != -1)
	{
		return Cached == 1;
	}
	const bool bPathable = QueryPathable(Index);
	G.Cache.Stamp(bPathable ? EWoTGoalCacheItem::PathableOK : EWoTGoalCacheItem::PathableFail,
	              GoalLoc, OwnerLoc);
	return bPathable;
}

bool UWoTBrainComponent::IsGoalNavigable(EWoTGoalIndex Index)
{
	// The original GoalContextInterf: reached || reachable || pathable.
	return IsGoalReached(Index) || IsGoalReachable(Index) || IsGoalPathable(Index);
}

bool UWoTBrainComponent::GetGoalStandSpot(EWoTGoalIndex Index, FVector& OutSpot) const
{
	// UE1 actorReachable test-moves the pawn onto a non-pawn goal; FarMoveActor's
	// FindSpot pushes it out of the geometry to where it fits, and the walk test
	// targets that spot. The navmesh stands in for it: the
	// nearest navigable point to where the pawn would touch the goal on its own
	// side, searched over the default query extent widened by the pawn's cylinder.
	// A wall-mounted goal (an alarm hangs 80 uu up a wall) lies off the navmesh,
	// which is eroded by its agent radius plus a cell.
	const FWoTGoal& G = GetGoal(Index);
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!G.HasActor() || !IsValid(Owner) || !World
		|| !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf)
		|| !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const ANavigationData* NavData = NavSys ? NavSys->GetDefaultNavDataInstance() : nullptr;
	if (!NavData)
	{
		return false;
	}
	const FVector Touch = GoalLoc
		+ (OwnerLoc - GoalLoc).GetSafeNormal2D() * (GoalRadius + OwnerRadius);
	const FVector Extent = NavData->GetConfig().DefaultQueryExtent
		+ FVector(OwnerRadius, OwnerRadius, OwnerHalf);
	FNavLocation Projected;
	if (!NavSys->ProjectPointToNavigation(Touch, Projected, Extent, NavData))
	{
		return false;
	}
	OutSpot = Projected.Location;
	return true;
}

bool UWoTBrainComponent::GetLastVisibleLocation(EWoTGoalIndex Index, FVector& OutLocation) const
{
	const FWoTGoalCacheItem& I = GetGoal(Index).Cache.Get(EWoTGoalCacheItem::VisibleOK);
	if (!I.bValid)
	{
		return false;
	}
	OutLocation = I.GoalLocation;
	return true;
}

bool UWoTBrainComponent::GetNavigationGoalLocation(EWoTGoalIndex Index, FVector& OutLocation)
{
	// Reached → hold; reachable → contact point at touching distance;
	// pathable → the goal itself (the path-node hop collapses into NavMesh
	// path following).
	const FWoTGoal& G = GetGoal(Index);
	FVector GoalLoc, OwnerLoc;
	float GoalRadius = 0.0f, GoalHalf = 0.0f, OwnerRadius = 0.0f, OwnerHalf = 0.0f;
	if (!IsGoalValid(Index) ||
	    !GetGoalParams(Index, GoalLoc, GoalRadius, GoalHalf) ||
	    !GetOwnerCylinder(OwnerLoc, OwnerRadius, OwnerHalf))
	{
		return false;
	}
	if (IsGoalReached(Index))
	{
		OutLocation = OwnerLoc;
		return true;
	}
	if (IsGoalReachable(Index))
	{
		const AActor* Target = G.HasActor() ? G.GoalActor.Get() : nullptr;
		const UPrimitiveComponent* TargetRoot = Target
			? Cast<UPrimitiveComponent>(Target->GetRootComponent()) : nullptr;
		if (TargetRoot
			&& TargetRoot->GetCollisionEnabled() != ECollisionEnabled::NoCollision
			&& TargetRoot->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
		{
			// Stop at touching distance rather than inside a target that blocks us.
			const FVector Dir = (GoalLoc - OwnerLoc).GetSafeNormal2D();
			OutLocation = GoalLoc - Dir * (OwnerRadius + GoalRadius);
			return true;
		}
		// A reachable goal is itself the navigation goal (the original native
		// FindNavigationGoalFromActor AssignObject); a non-blocking one such as the
		// alarm's touch volume is walked into until the geometry behind it stops us.
		OutLocation = GoalLoc;
		return true;
	}
	if (IsGoalPathable(Index))
	{
		OutLocation = GoalLoc;
		// Path following needs a navigable end: an off-mesh actor goal routes to
		// its stand spot, and is walked into once it turns reachable.
		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		FNavLocation OnMesh;
		FVector StandSpot;
		if (G.HasActor() && NavSys
			&& !NavSys->ProjectPointToNavigation(GoalLoc, OnMesh)
			&& GetGoalStandSpot(Index, StandSpot))
		{
			OutLocation = StandSpot;
		}
		return true;
	}
	return false;
}

void UWoTBrainComponent::NotifyMoveResult(bool bReachedCleanly)
{
	// The 2-strike nugget model: one give-up
	// drops the waypoint, two consecutive drop the goal and reset.
	if (bReachedCleanly)
	{
		NavigationStrikes = 0;
		return;
	}
	++NavigationStrikes;
	if (NavigationStrikes >= 2)
	{
		InvalidateGoal(CurrentGoalIndex);
		NavigationStrikes = 0;
	}
	else
	{
		InvalidateGoal(EWoTGoalIndex::CurrentWaypoint);
	}
}

void UWoTBrainComponent::ReflectTick(float DeltaSeconds)
{
	for (int32 i = 0; i < WoTNotifierSlotCount; ++i)
	{
		if (Notifiers[i].Tick(DeltaSeconds))
		{
			OnNotifierPulse.Broadcast(static_cast<EWoTNotifierSlot>(i));
		}
	}
}

void UWoTBrainComponent::SetNotifierEnabled(EWoTNotifierSlot Slot, bool bEnabled)
{
	FWoTDurationNotifier& N = Notifiers[static_cast<int32>(Slot)];
	if (bEnabled && !N.bEnabled)
	{
		// Random phase on enable desyncs a crowd's pulses.
		N.Elapsed = (N.bRandomizePhase && N.Period > 0.0f) ? WoTRandom::FRand() * N.Period : 0.0f;
	}
	N.bEnabled = bEnabled;
}

bool UWoTBrainComponent::IsNotifierEnabled(EWoTNotifierSlot Slot) const
{
	return Notifiers[static_cast<int32>(Slot)].bEnabled;
}

void UWoTBrainComponent::SetActiveNotifiers(std::initializer_list<EWoTNotifierSlot> Slots)
{
	bool bWanted[WoTNotifierSlotCount] = {};
	for (EWoTNotifierSlot Slot : Slots)
	{
		bWanted[static_cast<int32>(Slot)] = true;
	}
	for (int32 i = 0; i < WoTNotifierSlotCount; ++i)
	{
		SetNotifierEnabled(static_cast<EWoTNotifierSlot>(i), bWanted[i]);
	}
}
