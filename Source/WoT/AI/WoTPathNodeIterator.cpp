#include "WoTPathNodeIterator.h"

#include "WoTPathNode.h"
#include "../WoTRetailProjectileTarget.h"
#include "../WoTMeshLight.h"
#include "../WoTRainCyd.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<FString> CVarWoTSeekNavBackend(
	TEXT("wot.SeekNavBackend"), TEXT("paths"),
	TEXT("Seeking navigation backend: paths (retail/default) or navmesh (A/B)."));

bool WoTSeekNavigationUsesPathNodes()
{
	return !CVarWoTSeekNavBackend.GetValueOnGameThread().Equals(
		TEXT("navmesh"), ESearchCase::IgnoreCase);
}

bool WoTIsPointOutsideWorld(const UWorld* World, const FVector& Point)
{
	const UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(World);
	int32 ZoneNumber = 0;
	int32 LeafIndex = INDEX_NONE;
	return Table
		&& UWoTMeshLight::ClassifyRegion(Table, Point, ZoneNumber, LeafIndex)
		&& ZoneNumber == 0;
}

namespace
{
#if WITH_AUTOMATION_TESTS
	int32 GRetailTraceQueryCount = 0;
	TFunction<void(const FVector&, const FVector&)> GRetailTraceObserver;
#endif
	void AddRetailSourceIgnores(FCollisionQueryParams& Params,
		const AActor* SourceActor)
	{
		// ULevel::SingleLineCheck skips CheckActor when SourceActor->IsOwnedBy it
		// in UE1. This includes a carried artifact's carrier.
		for (const AActor* It = SourceActor; IsValid(It); It = It->GetOwner())
		{
			Params.AddIgnoredActor(It);
		}
	}

	bool IsRetailActorTraceHit(const FHitResult& Hit)
	{
		AActor* Actor = Hit.GetActor();
		if (!IsValid(Actor))
		{
			return false;
		}
		if (WoTRainCydIsVisBlockingActor(Actor) || Actor->IsA<APawn>())
		{
			return true;
		}
		const UPrimitiveComponent* Component = Hit.GetComponent();
		if (!Component)
		{
			return false;
		}
		// UE1's bProjTarget is explicit because overlap responses cannot encode it;
		// blocking Pawn and WoTProjectile represents bBlockPlayers&&bBlockActors.
		// Ordinary pickups/triggers/teleporters and overlap-only blockers fail both.
		return WoTActorIsRetailProjectileTarget(Actor)
			|| (Component->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block
				&& Component->GetCollisionResponseToChannel(ECC_GameTraceChannel1)
					== ECR_Block);
	}

	bool IsRetailVisBlockingHit(const FHitResult& Hit)
	{
		return WoTRainCydIsVisBlockingActor(Hit.GetActor());
	}

	template <typename TraceFn, typename AcceptFn>
	bool TraceAccepting(UWorld* World, const AActor* SourceActor,
		FHitResult& OutHit, TraceFn&& Trace, AcceptFn&& Accept)
	{
		OutHit = FHitResult();
		if (!IsValid(World))
		{
			return false;
		}
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTRetailTrace), false);
		AddRetailSourceIgnores(Params, SourceActor);
		TArray<FHitResult> Hits;
#if WITH_AUTOMATION_TESTS
		++GRetailTraceQueryCount;
#endif
		Trace(Hits, Params);
		Hits.Sort([](const FHitResult& A, const FHitResult& B)
		{
			return A.Distance < B.Distance;
		});
		for (const FHitResult& Hit : Hits)
		{
			if (Accept(Hit))
			{
				OutHit = Hit;
				return true;
			}
		}
		return false;
	}

	float RetailRecursiveDistanceToWorldEdge(const FVector& Start,
		const FVector& Direction)
	{
		// UE1's zero TraceLimit means infinite recursion. Its coordinates still
		// live inside the legacy world cube, so the ray can end only after it
		// leaves that representable space; this is a domain boundary, not a
		// gameplay distance cap.
		float ExitDistance = TNumericLimits<float>::Max();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double StartAxis = Start[Axis];
			const double DirectionAxis = Direction[Axis];
			if (FMath::Abs(DirectionAxis) <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}
			constexpr double RetailHalfWorld = 32768.0;
			const double Edge = DirectionAxis > 0.0
				? RetailHalfWorld : -RetailHalfWorld;
			const double AxisDistance = (Edge - StartAxis) / DirectionAxis;
			if (AxisDistance >= 0.0)
			{
				ExitDistance = FMath::Min(
					ExitDistance, static_cast<float>(AxisDistance));
			}
		}
		return ExitDistance == TNumericLimits<float>::Max()
			? 0.0f : ExitDistance;
	}

}

#if WITH_AUTOMATION_TESTS
void WoTResetRetailTraceQueryCountForTesting() { GRetailTraceQueryCount = 0; }
int32 WoTGetRetailTraceQueryCountForTesting() { return GRetailTraceQueryCount; }
void WoTSetRetailTraceObserverForTesting(
	TFunction<void(const FVector&, const FVector&)> Observer)
{
	GRetailTraceObserver = MoveTemp(Observer);
}
#endif

bool WoTVisBlockingTrace(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor, FHitResult& OutHit)
{
	// Same walk as the Balefire decal and RainCyd ground traces: query the
	// world object types, accept only level geometry or a mover
	// (WoTRainCydIsVisBlockingActor), and re-trace past anything else. Pawns
	// (ECC_Pawn) and projectiles (the WoTProjectile channel) are not queried.
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	return TraceAccepting(World, SourceActor, OutHit,
		[&](TArray<FHitResult>& Hits, const FCollisionQueryParams& Params)
		{
#if WITH_AUTOMATION_TESTS
			if (GRetailTraceObserver) GRetailTraceObserver(Start, End);
#endif
			return World->LineTraceMultiByObjectType(
				Hits, Start, End, Objects, Params);
		}, IsRetailVisBlockingHit);
}

bool WoTVisBlockingTraceHits(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor)
{
	FHitResult Hit;
	return WoTVisBlockingTrace(World, Start, End, SourceActor, Hit);
}

bool WoTVisBlockingSweep(UWorld* World, const FVector& Start,
	const FVector& End, const FQuat& Rotation, const FCollisionShape& Shape,
	const AActor* SourceActor, FHitResult& OutHit)
{
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	return TraceAccepting(World, SourceActor, OutHit,
		[&](TArray<FHitResult>& Hits, const FCollisionQueryParams& Params)
		{
#if WITH_AUTOMATION_TESTS
			if (GRetailTraceObserver) GRetailTraceObserver(Start, End);
#endif
			return World->SweepMultiByObjectType(
				Hits, Start, End, Rotation, Objects, Shape, Params);
		}, IsRetailVisBlockingHit);
}

bool WoTVisBlockingTraceRecursive(UWorld* World, const FVector& Start,
	const FVector& Direction, float TraceLimit, const AActor* SourceActor,
	FHitResult& OutHit)
{
	const FVector UnitDirection = Direction.IsNearlyZero()
		? FVector(0, 0, -1) : Direction.GetSafeNormal();
	const float Distance = TraceLimit > 0.0f ? TraceLimit
		: RetailRecursiveDistanceToWorldEdge(Start, UnitDirection);
	return Distance > 0.0f && WoTVisBlockingTrace(
		World, Start, Start + UnitDirection * Distance, SourceActor, OutHit);
}

bool WoTRetailActorTrace(UWorld* World, const FVector& Start,
	const FVector& End, const AActor* SourceActor, FHitResult& OutHit)
{
	// UE1's AActor::execTrace selects TRACE_AllColliding|TRACE_ProjTargets;
	// ULevel::SingleLineCheck defines the actor acceptance rules.
	return TraceAccepting(World, SourceActor, OutHit,
		[&](TArray<FHitResult>& Hits, const FCollisionQueryParams& Params)
		{
#if WITH_AUTOMATION_TESTS
			if (GRetailTraceObserver) GRetailTraceObserver(Start, End);
#endif
			return World->LineTraceMultiByObjectType(Hits, Start, End,
				FCollisionObjectQueryParams::AllObjects, Params);
		}, IsRetailActorTraceHit);
}

bool WoTRetailActorTraceRecursive(UWorld* World, const FVector& Start,
	const FVector& Direction, float TraceLimit, const AActor* SourceActor,
	FHitResult& OutHit)
{
	const FVector UnitDirection = Direction.IsNearlyZero()
		? FVector(0, 0, -1) : Direction.GetSafeNormal();
	const float Distance = TraceLimit > 0.0f ? TraceLimit
		: RetailRecursiveDistanceToWorldEdge(Start, UnitDirection);
	return Distance > 0.0f && WoTRetailActorTrace(
		World, Start, Start + UnitDirection * Distance, SourceActor, OutHit);
}

namespace
{
	constexpr int32 MaxCandidates = 7;

	bool IsVisible(UWorld* World, const FVector& Start, const FVector& End)
	{
		// The original native iterator: do not let a precision-scale overlap
		// manufacture a geometry hit before the full world-only trace.
		const FVector Delta = Start - End;
		if (FMath::Abs(Delta.X) < 3.0 && FMath::Abs(Delta.Y) < 3.0
			&& FMath::Abs(Delta.Z) < 3.0)
		{
			return true;
		}
		// The original native iterator: SingleLineCheck(..., TRACE_VisBlocking).
		return !WoTVisBlockingTraceHits(World, Start, End);
	}

	float EndpointCost(const FVector& Point, const AWoTPathNode* Node,
		bool bStartEndpoint)
	{
		const float Distance = FVector::Dist(Point, Node->GetActorLocation());
		// The original native iterator: entering the network has no node
		// surcharge; leaving it pays the selected end node's ExtraCost.
		return bStartEndpoint ? Distance : Distance + Node->ExtraCost;
	}

	AWoTPathNode* SelectEndpoint(UWorld* World,
		const TArray<AWoTPathNode*>& Nodes, const FVector& Point,
		bool bStartEndpoint, bool bDesperate, float& OutCost)
	{
		OutCost = 0.0f;
		AWoTPathNode* Selected = nullptr;
		if (WoTIsPointOutsideWorld(World, Point))
		{
			for (AWoTPathNode* Node : Nodes)
			{
				const float Cost = EndpointCost(Point, Node, bStartEndpoint);
				if (!Selected || Cost < OutCost)
				{
					Selected = Node;
					OutCost = Cost;
				}
			}
			return Selected;
		}

		struct FCandidate
		{
			AWoTPathNode* Node = nullptr;
			float Cost = 0.0f;
		};
		FCandidate Candidates[MaxCandidates];
		for (AWoTPathNode* Node : Nodes)
		{
			const float Cost = EndpointCost(Point, Node, bStartEndpoint);
			for (int32 CandidateIndex = 0; CandidateIndex < MaxCandidates;
				++CandidateIndex)
			{
				if (!Candidates[CandidateIndex].Node
					|| Cost < Candidates[CandidateIndex].Cost)
				{
					for (int32 ShiftIndex = MaxCandidates - 1;
						ShiftIndex > CandidateIndex; --ShiftIndex)
					{
						Candidates[ShiftIndex] = Candidates[ShiftIndex - 1];
					}
					Candidates[CandidateIndex] = {Node, Cost};
					break;
				}
			}
		}
		for (const FCandidate& Candidate : Candidates)
		{
			if (!Candidate.Node)
			{
				break;
			}
			const FVector NodeLocation = Candidate.Node->GetActorLocation();
			const bool bCandidateVisible = bStartEndpoint
				? IsVisible(World, Point, NodeLocation)
				: IsVisible(World, NodeLocation, Point);
			if (bCandidateVisible)
			{
				OutCost = Candidate.Cost;
				return Candidate.Node;
			}
		}

		if (bDesperate)
		{
			for (AWoTPathNode* Node : Nodes)
			{
				const float Cost = EndpointCost(Point, Node, bStartEndpoint);
				if (!Selected || Cost < OutCost)
				{
					Selected = Node;
					OutCost = Cost;
				}
			}
		}
		return Selected;
	}

	class FNodeMinQueue
	{
	public:
		explicit FNodeMinQueue(const TMap<AWoTPathNode*, float>& InWeights)
			: Weights(InWeights)
		{
		}

		bool IsEmpty() const { return Heap.IsEmpty(); }

		bool Contains(AWoTPathNode* Node) const
		{
			return Heap.ContainsByPredicate(
				[Node](const FEntry& Entry) { return Entry.Node == Node; });
		}

		void Push(AWoTPathNode* Node)
		{
			// Retail Push (the linked-list scan) inserts an equal-weight node
			// BEFORE existing equals, so ties pop newest-first; the sequence
			// tie-break reproduces that (reinsertion gets a fresh sequence).
			const int32 Index = Heap.Add({Node, ++PushSequence});
			SiftUp(Index);
		}

		AWoTPathNode* Pop()
		{
			if (Heap.IsEmpty())
			{
				return nullptr;
			}
			AWoTPathNode* Result = Heap[0].Node;
			Heap[0] = Heap.Last();
			Heap.Pop(EAllowShrinking::No);
			if (!Heap.IsEmpty())
			{
				SiftDown(0);
			}
			return Result;
		}

		void Remove(AWoTPathNode* Node)
		{
			const int32 Index = Heap.IndexOfByPredicate(
				[Node](const FEntry& Entry) { return Entry.Node == Node; });
			if (Index == INDEX_NONE)
			{
				return;
			}
			Heap[Index] = Heap.Last();
			Heap.Pop(EAllowShrinking::No);
			if (Heap.IsValidIndex(Index))
			{
				if (Index > 0 && IsLower(Heap[Index], Heap[(Index - 1) / 2]))
				{
					SiftUp(Index);
				}
				else
				{
					SiftDown(Index);
				}
			}
		}

	private:
		struct FEntry
		{
			AWoTPathNode* Node = nullptr;
			uint64 Sequence = 0;
		};

		bool IsLower(const FEntry& A, const FEntry& B) const
		{
			const float WeightA = Weights.FindChecked(A.Node);
			const float WeightB = Weights.FindChecked(B.Node);
			if (WeightA != WeightB)
			{
				return WeightA < WeightB;
			}
			return A.Sequence > B.Sequence;
		}

		void SiftUp(int32 Index)
		{
			while (Index > 0)
			{
				const int32 Parent = (Index - 1) / 2;
				if (!IsLower(Heap[Index], Heap[Parent]))
				{
					break;
				}
				Heap.Swap(Index, Parent);
				Index = Parent;
			}
		}

		void SiftDown(int32 Index)
		{
			for (;;)
			{
				const int32 Left = Index * 2 + 1;
				const int32 Right = Left + 1;
				int32 Smallest = Index;
				if (Heap.IsValidIndex(Left) && IsLower(Heap[Left], Heap[Smallest]))
				{
					Smallest = Left;
				}
				if (Heap.IsValidIndex(Right) && IsLower(Heap[Right], Heap[Smallest]))
				{
					Smallest = Right;
				}
				if (Smallest == Index)
				{
					break;
				}
				Heap.Swap(Index, Smallest);
				Index = Smallest;
			}
		}

		const TMap<AWoTPathNode*, float>& Weights;
		TArray<FEntry> Heap;
		uint64 PushSequence = 0;
	};
}

bool FWoTPathNodeIterator::BuildPath(UWorld* World, FVector Start,
	FVector End, bool bDesperate)
{
	Reset();
	if (!IsValid(World))
	{
		return false;
	}

	TArray<AWoTPathNode*> Nodes;
	for (AWoTPathNode* Node : TActorRange<AWoTPathNode>(World))
	{
		if (IsValid(Node))
		{
			Nodes.Add(Node);
		}
	}
	// No stamped graph (unstamped/test map): fail before touching the region
	// payload — the per-frame beam fallback must not re-attempt table loads.
	if (Nodes.IsEmpty())
	{
		return false;
	}

	float StartCost = 0.0f;
	float EndCost = 0.0f;
	AWoTPathNode* StartNode = SelectEndpoint(
		World, Nodes, Start, true, bDesperate, StartCost);
	AWoTPathNode* EndNode = SelectEndpoint(
		World, Nodes, End, false, bDesperate, EndCost);
	if (!StartNode || !EndNode)
	{
		Reset();
		return false;
	}

	NodeCost = StartCost + EndCost;
	if (StartNode == EndNode)
	{
		NodePath.Add(StartNode);
		NodeCount = 1;
		return true;
	}

	TMap<AWoTPathNode*, float> VisitedWeight;
	TMap<AWoTPathNode*, AWoTPathNode*> Previous;
	TSet<AWoTPathNode*> Closed;
	FNodeMinQueue Open(VisitedWeight);
	VisitedWeight.Add(StartNode, 0.0f);
	Open.Push(StartNode);

	auto Relax = [&](AWoTPathNode* Current, AWoTPathNode* Neighbor,
		float NewCost)
	{
		if (!IsValid(Neighbor) || Closed.Contains(Neighbor))
		{
			return;
		}
		if (Open.Contains(Neighbor))
		{
			if (NewCost < VisitedWeight.FindChecked(Neighbor))
			{
				Open.Remove(Neighbor);
				Previous.Add(Neighbor, Current);
				VisitedWeight.Add(Neighbor, NewCost);
				Open.Push(Neighbor);
			}
		}
		else
		{
			Previous.Add(Neighbor, Current);
			VisitedWeight.Add(Neighbor, NewCost);
			Open.Push(Neighbor);
		}
	};

	while (!Open.IsEmpty())
	{
		AWoTPathNode* Current = Open.Pop();
		if (!Current)
		{
			Reset();
			return false;
		}
		Closed.Add(Current);
		const float CurrentWeight = VisitedWeight.FindChecked(Current);
		if (Current == EndNode)
		{
			TArray<AWoTPathNode*> ReversePath;
			for (AWoTPathNode* Node = EndNode; Node; Node = Previous.FindRef(Node))
			{
				ReversePath.Add(Node);
			}
			for (int32 Index = ReversePath.Num() - 1;
				Index >= 0 && NodePath.Num() < MaxPathNodes; --Index)
			{
				NodePath.Add(ReversePath[Index]);
			}
			NodeCount = NodePath.Num();
			NodeCost += CurrentWeight;
			return true;
		}

		// As the original native iterator: preserve authored out-edge then
		// upstream-edge order, including the asymmetric ReachSpec start cost.
		for (const FWoTPathEdge& Edge : Current->OutEdges)
		{
			Relax(Current, Edge.Other,
				CurrentWeight + Edge.Distance + Current->ExtraCost);
		}
		for (const FWoTPathEdge& Edge : Current->InEdges)
		{
			if (IsValid(Edge.Other))
			{
				Relax(Current, Edge.Other,
					CurrentWeight + Edge.Distance + Edge.Other->ExtraCost);
			}
		}
	}

	Reset();
	return false;
}

AWoTPathNode* FWoTPathNodeIterator::GetCurrent() const
{
	return NodePath.IsValidIndex(NodeIndex) ? NodePath[NodeIndex] : nullptr;
}

AWoTPathNode* FWoTPathNodeIterator::GetFirst()
{
	NodeIndex = 0;
	return GetCurrent();
}

AWoTPathNode* FWoTPathNodeIterator::GetLast()
{
	NodeIndex = NodeCount - 1;
	return GetCurrent();
}

AWoTPathNode* FWoTPathNodeIterator::GetNext()
{
	if (NodeIndex < NodeCount - 1)
	{
		++NodeIndex;
		return GetCurrent();
	}
	return nullptr;
}

AWoTPathNode* FWoTPathNodeIterator::GetPrevious()
{
	if (NodeIndex > 0)
	{
		--NodeIndex;
		return GetCurrent();
	}
	return nullptr;
}

void FWoTPathNodeIterator::Reset()
{
	NodePath.Reset();
	NodeCount = 0;
	NodeIndex = 0;
	NodeCost = 0.0f;
}
