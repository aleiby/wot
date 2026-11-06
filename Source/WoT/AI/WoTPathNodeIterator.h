#pragma once

#include "CoreMinimal.h"

class AActor;
class AWoTPathNode;
class UWorld;
struct FCollisionShape;
struct FHitResult;

// Plain-C++ port of Legend.PathNodeIteratorII. The shipped contract is
// exports/script/Legend/PathNodeIteratorII.uc:12-26; the native algorithm is
// reimplemented from the original game's native code. Paths contain at most 512
// nodes and retain start-first order. Per-search Dijkstra state is local rather
// than written onto shared actors, so concurrent iterators cannot interfere.
//
// UE1 used UModel::PointRegion(...).ZoneNumber==0 for outside-world endpoint
// handling. The port uses the converted BSP region payload; a missing/malformed
// light-table payload is deliberately treated as inside the world.
class FWoTPathNodeIterator
{
public:
	static constexpr int32 MaxPathNodes = 512;

	bool BuildPath(UWorld* World, FVector Start, FVector End, bool bDesperate);
	AWoTPathNode* GetFirst();
	AWoTPathNode* GetLast();
	AWoTPathNode* GetNext();
	AWoTPathNode* GetPrevious();
	AWoTPathNode* GetCurrent() const;
	void Reset();

	int32 NodeCount = 0;
	float NodeCost = 0.0f;

private:
	TArray<AWoTPathNode*> NodePath;
	int32 NodeIndex = 0;
};

// Live backend seam shared by every seeking/reachability consumer. The cvar is
// wot.SeekNavBackend: "paths" (default) or "navmesh"; unknown values preserve
// the parity-first paths behavior.
bool WoTSeekNavigationUsesPathNodes();

// Shared outside-world query used by endpoint selection and retail seeker
// steering. Missing converted BSP data returns false (inside adaptation).
bool WoTIsPointOutsideWorld(const UWorld* World, const FVector& Point);

// Retail SingleLineCheck(TRACE_VisBlocking), which is also what
// Trace(..., bTraceActors=False) uses in UE1: level geometry
// and movers only. Pawns, projectiles and decorations never
// block it. Path visibility (the native PathNodeIteratorII) and the seeker's
// direct-reach trace (SeekingProjectile.uc:644-657) both depend on that: a
// channel trace stops on the destination pawn's own capsule, so a seeker could
// never "see" its target and fell back to its through-the-walls bee-line.
WOT_API bool WoTVisBlockingTrace(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor, FHitResult& OutHit);
WOT_API bool WoTVisBlockingTraceHits(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor = nullptr);

// Extent form of the same UE1 TRACE_VisBlocking query. MovementManager's
// Trace(..., false, Extent) needs the shape preserved while still filtering
// actor capsules that happen to use a world object channel.
WOT_API bool WoTVisBlockingSweep(UWorld* World, const FVector& Start,
	const FVector& End, const FQuat& Rotation, const FCollisionShape& Shape,
	const AActor* SourceActor, FHitResult& OutHit);
WOT_API bool WoTVisBlockingTraceRecursive(UWorld* World,
	const FVector& Start, const FVector& Direction, float TraceLimit,
	const AActor* SourceActor, FHitResult& OutHit);

// Retail Trace(..., bTraceActors=true): level/movers, pawns, projectile
// targets, and actors that block actors and players (ULevel::SingleLineCheck).
WOT_API bool WoTRetailActorTrace(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor, FHitResult& OutHit);
WOT_API bool WoTRetailActorTraceRecursive(UWorld* World, const FVector& Start,
	const FVector& Direction, float TraceLimit, const AActor* SourceActor,
	FHitResult& OutHit);

#if WITH_AUTOMATION_TESTS
WOT_API void WoTResetRetailTraceQueryCountForTesting();
WOT_API int32 WoTGetRetailTraceQueryCountForTesting();
WOT_API void WoTSetRetailTraceObserverForTesting(
	TFunction<void(const FVector&, const FVector&)> Observer);
#endif
