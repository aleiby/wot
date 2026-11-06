#include "WoTFrontEndGroundTrace.h"

#include "../Payload/WoTRuntimeWorldResource.h"

namespace
{
constexpr int32 MaxEntryGroundNodes = 2000000;
constexpr uint8 EntryNF_NotCsg = 0x01;
constexpr uint8 EntryNF_IsNew = 0x20;
constexpr double EntryPlaneTolerance = 0.001;

struct FEntryGroundTraceState
{
	int32 NodeIndex = INDEX_NONE;
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	bool bOutside = true;
};

bool EntryVectorIsFinite(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z);
}

bool EntryChildIsValid(int32 Child, int32 NodeCount)
{
	return Child == INDEX_NONE || (Child >= 0 && Child < NodeCount);
}

EWoTEntryTraceResult EntryGroundInvalid(FString& Error, const TCHAR* Message)
{
	Error = Message;
	return EWoTEntryTraceResult::Invalid;
}
}

EWoTEntryTraceResult WoTTraceEntryGround(
	TConstArrayView<FWoTRuntimeWorldNode> Nodes, bool bRootOutside,
	FVector Start, FVector End, FVector& Hit, FString& Error)
{
	Hit = FVector::ZeroVector;
	Error.Reset();
	if (!EntryVectorIsFinite(Start) || !EntryVectorIsFinite(End))
		return EntryGroundInvalid(Error,
			TEXT("entry ground trace segment is nonfinite"));
	if (Nodes.Num() > MaxEntryGroundNodes)
		return EntryGroundInvalid(Error,
			TEXT("entry ground trace node count exceeds its work bound"));
	if (Nodes.IsEmpty())
	{
		if (bRootOutside) return EWoTEntryTraceResult::Miss;
		return EntryGroundInvalid(Error,
			TEXT("entry ground trace has solid root state without a BSP node"));
	}

	const int64 MaxStates = 4ll * static_cast<int64>(Nodes.Num()) + 1ll;
	int64 VisitedStates = 0;
	TBitArray<> VisitedNodes(false, Nodes.Num());
	TArray<FEntryGroundTraceState> Pending;
	Pending.Reserve(FMath::Min(Nodes.Num(), 1024));
	Pending.Add({0, Start, End, bRootOutside});
	while (!Pending.IsEmpty())
	{
		if (++VisitedStates > MaxStates)
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace exceeded its traversal-state bound"));
		const FEntryGroundTraceState State = Pending.Pop(EAllowShrinking::No);
		if (!EntryVectorIsFinite(State.Start) || !EntryVectorIsFinite(State.End))
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace produced a nonfinite segment state"));
		if (State.NodeIndex == INDEX_NONE)
		{
			if (State.bOutside) continue;
			Hit = State.Start;
			return EWoTEntryTraceResult::Hit;
		}
		if (!Nodes.IsValidIndex(State.NodeIndex))
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace reached an invalid node index"));
		if (VisitedNodes[State.NodeIndex])
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace BSP contains a child cycle"));
		VisitedNodes[State.NodeIndex] = true;

		const FWoTRuntimeWorldNode& Node = Nodes[State.NodeIndex];
		const double NX = static_cast<double>(Node.Plane[0]);
		const double NY = static_cast<double>(Node.Plane[1]);
		const double NZ = static_cast<double>(Node.Plane[2]);
		const double W = static_cast<double>(Node.Plane[3]);
		const double NormalSquared = NX * NX + NY * NY + NZ * NZ;
		if (!FMath::IsFinite(NX) || !FMath::IsFinite(NY)
			|| !FMath::IsFinite(NZ) || !FMath::IsFinite(W)
			|| !FMath::IsFinite(NormalSquared)
			|| NormalSquared <= static_cast<double>(UE_SMALL_NUMBER))
		{
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace node plane is invalid"));
		}
		if (!EntryChildIsValid(Node.Back, Nodes.Num())
			|| !EntryChildIsValid(Node.Front, Nodes.Num()))
		{
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace node child is outside the node table"));
		}

		auto PlaneDistance = [NX, NY, NZ, W](const FVector& Point)
		{
			double Distance = NX * Point.X;
			Distance += NY * Point.Y;
			Distance += NZ * Point.Z;
			return Distance - W;
		};
		const double StartDistance = PlaneDistance(State.Start);
		const double EndDistance = PlaneDistance(State.End);
		if (!FMath::IsFinite(StartDistance) || !FMath::IsFinite(EndDistance))
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace plane distance is nonfinite"));
		const bool bCsg = Node.VertexCount > 0
			&& (Node.Flags & (EntryNF_NotCsg | EntryNF_IsNew)) == 0;
		auto ChildOutside = [bCsg](bool bFront, bool bOutside)
		{
			return bFront ? (bOutside || bCsg) : (bOutside && !bCsg);
		};
		auto ChildIndex = [&Node](bool bFront)
		{
			return bFront ? Node.Front : Node.Back;
		};

		if (StartDistance > -EntryPlaneTolerance
			&& EndDistance > -EntryPlaneTolerance)
		{
			Pending.Add({ChildIndex(true), State.Start, State.End,
				ChildOutside(true, State.bOutside)});
			continue;
		}
		if (StartDistance < EntryPlaneTolerance
			&& EndDistance < EntryPlaneTolerance)
		{
			Pending.Add({ChildIndex(false), State.Start, State.End,
				ChildOutside(false, State.bOutside)});
			continue;
		}

		const double Denominator = EndDistance - StartDistance;
		if (!FMath::IsFinite(Denominator) || Denominator == 0.0)
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace split denominator is invalid"));
		const double Alpha = StartDistance / Denominator;
		const FVector Middle = State.Start
			+ (State.Start - State.End) * Alpha;
		if (!EntryVectorIsFinite(Middle))
			return EntryGroundInvalid(Error,
				TEXT("entry ground trace split point is nonfinite"));
		const bool bNearFront = StartDistance > 0.0;
		// LIFO: enqueue the far portion first so the segment is visited from
		// its start to end and the first solid leaf is the earliest boundary.
		Pending.Add({ChildIndex(!bNearFront), Middle, State.End,
			ChildOutside(!bNearFront, State.bOutside)});
		Pending.Add({ChildIndex(bNearFront), State.Start, Middle,
			ChildOutside(bNearFront, State.bOutside)});
	}
	return EWoTEntryTraceResult::Miss;
}
