// Retail path-node runtime specs. Behavioral assertions cite the
// shipped script contract (PathNodeIteratorII.uc:12-26,
// SeekingProjectile.uc:480-601,663-681; AngrealInvTracer.uc:30-127) and the
// native-only Streak iterator shape re-expressed by the production port.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTPathNode.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTAngrealInvTracer.h"
#include "../Inventory/WoTAngrealInvTarget.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTAutoTargetReflector.h"
#include "../Projectiles/WoTTracerProjectile.h"
#include "../WoTBeamEffect.h"
#include "../WoTCharacter.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/IConsoleManager.h"

namespace
{
	FActorSpawnParameters Wave2PathAlwaysSpawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTPathNode* Wave2PathSpawnNode(UWorld* World, const FVector& Location,
		FName SourceClass = TEXT("PathNode"))
	{
		AWoTPathNode* Node = World->SpawnActor<AWoTPathNode>(
			Location, FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
		if (Node)
		{
			Node->SourceClass = SourceClass;
		}
		return Node;
	}

	void Wave2PathWire(AWoTPathNode* From, AWoTPathNode* To, float Distance)
	{
		FWoTPathEdge Out;
		Out.Other = To;
		Out.Distance = Distance;
		From->OutEdges.Add(Out);
		FWoTPathEdge In;
		In.Other = From;
		In.Distance = Distance;
		To->InEdges.Add(In);
	}

	AActor* Wave2PathSpawnWall(UWorld* World, const FVector& Location,
		const FVector& Extent)
	{
		// Level geometry (AStaticMeshActor = imported BSP in editor maps), the
		// only world-static kind TRACE_VisBlocking stops on. Spawned in place:
		// its Static-mobility root refuses a post-registration SetActorLocation.
		AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(
			Location, FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
		UBoxComponent* Box = NewObject<UBoxComponent>(Wall, TEXT("Wave2PathWall"));
		Box->SetupAttachment(Wall->GetRootComponent());
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Wall;
	}

	AActor* Wave2PathSpawnPointActor(UWorld* World, const FVector& Location)
	{
		AActor* Actor = World->SpawnActor<AActor>(Wave2PathAlwaysSpawn());
		USceneComponent* Root = NewObject<USceneComponent>(
			Actor, TEXT("Wave2PathPointRoot"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		Actor->SetActorLocation(Location);
		return Actor;
	}

	IConsoleVariable* Wave2PathBackendCVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("wot.SeekNavBackend"));
	}

	void Wave2PathSetBackend(const TCHAR* Value)
	{
		if (IConsoleVariable* CVar = Wave2PathBackendCVar())
		{
			CVar->Set(Value, ECVF_SetByCode);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorDiamondSpec,
	"WoT.PathNodes.Iterator.DiamondAndCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorDiamondSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPathNode* Start = Wave2PathSpawnNode(World, FVector(0, 0, 100));
	AWoTPathNode* Expensive = Wave2PathSpawnNode(World, FVector(100, 100, 100));
	AWoTPathNode* Cheap = Wave2PathSpawnNode(World, FVector(100, -100, 100));
	AWoTPathNode* Goal = Wave2PathSpawnNode(World, FVector(200, 0, 100));
	Start->ExtraCost = 4;
	Cheap->ExtraCost = 1;
	Goal->ExtraCost = 7;
	Wave2PathWire(Start, Expensive, 10.0f);
	Wave2PathWire(Expensive, Goal, 10.0f);
	Wave2PathWire(Start, Cheap, 2.0f);
	Wave2PathWire(Cheap, Goal, 3.0f);

	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("diamond path builds (PathNodeIteratorII.uc:21)"),
		Iterator.BuildPath(World, Start->GetActorLocation(), Goal->GetActorLocation(), false));
	TestEqual(TEXT("short weighted route has three start-first nodes"),
		Iterator.NodeCount, 3);
	TestTrue(TEXT("path starts at Start"), Iterator.GetFirst() == Start);
	TestTrue(TEXT("short branch wins"), Iterator.GetNext() == Cheap);
	TestTrue(TEXT("path ends at Goal"), Iterator.GetNext() == Goal);
	TestEqual(TEXT("entry + exit + reachspec/current-node costs"),
		Iterator.NodeCost, 17.0f);
	TestNull(TEXT("GetNext at end returns null"), Iterator.GetNext());
	TestTrue(TEXT("GetNext does not move the valid cursor off Goal"),
		Iterator.GetCurrent() == Goal);
	TestTrue(TEXT("GetLast remains Goal"), Iterator.GetLast() == Goal);
	TestNull(TEXT("GetNext after GetLast is still null"), Iterator.GetNext());
	TestTrue(TEXT("cursor still points at Goal"), Iterator.GetCurrent() == Goal);
	TestTrue(TEXT("GetFirst resets to Start"), Iterator.GetFirst() == Start);
	TestNull(TEXT("GetPrevious at start returns null"), Iterator.GetPrevious());
	TestTrue(TEXT("GetPrevious does not move before Start"),
		Iterator.GetCurrent() == Start);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorTieOrderSpec,
	"WoT.PathNodes.Iterator.EqualCostTieOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorTieOrderSpec::RunTest(const FString&)
{
	// Retail's PriorityQueue::Push inserts an equal-weight node BEFORE
	// existing equals (the scan stops at the first weight >= new), so among
	// equal-cost branches the LATER-relaxed out-edge pops first and owns the
	// goal's prev pointer. With Start's OutEdges authored [First, Second] and
	// a fully symmetric diamond, the retail pick is Second.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPathNode* Start = Wave2PathSpawnNode(World, FVector(0, 0, 100));
	AWoTPathNode* First = Wave2PathSpawnNode(World, FVector(100, 100, 100));
	AWoTPathNode* Second = Wave2PathSpawnNode(World, FVector(100, -100, 100));
	AWoTPathNode* Goal = Wave2PathSpawnNode(World, FVector(200, 0, 100));
	Wave2PathWire(Start, First, 10.0f);
	Wave2PathWire(Start, Second, 10.0f);
	Wave2PathWire(First, Goal, 10.0f);
	Wave2PathWire(Second, Goal, 10.0f);

	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("symmetric diamond path builds"),
		Iterator.BuildPath(World, Start->GetActorLocation(),
			Goal->GetActorLocation(), false));
	TestEqual(TEXT("tie path has three nodes"), Iterator.NodeCount, 3);
	TestTrue(TEXT("path starts at Start"), Iterator.GetFirst() == Start);
	TestTrue(TEXT("equal-cost tie pops the newer push (retail LIFO equals)"),
		Iterator.GetNext() == Second);
	TestTrue(TEXT("path ends at Goal"), Iterator.GetNext() == Goal);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorUpstreamSpec,
	"WoT.PathNodes.Iterator.UpstreamTraversal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorUpstreamSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPathNode* Start = Wave2PathSpawnNode(World, FVector(0, 0, 100));
	AWoTPathNode* Goal = Wave2PathSpawnNode(World, FVector(200, 0, 100));
	// Only Start.upstreamPaths exists: the authored ReachSpec points Goal->Start.
	FWoTPathEdge Upstream;
	Upstream.Other = Goal;
	Upstream.Distance = 12.0f;
	Start->InEdges.Add(Upstream);
	Goal->ExtraCost = 3;

	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("upstream-only route builds (native :622-686)"),
		Iterator.BuildPath(World, Start->GetActorLocation(), Goal->GetActorLocation(), false));
	TestEqual(TEXT("upstream route has two nodes"), Iterator.NodeCount, 2);
	TestTrue(TEXT("upstream route begins at Start"), Iterator.GetFirst() == Start);
	TestTrue(TEXT("upstream route reaches Goal"), Iterator.GetNext() == Goal);
	// End ExtraCost is paid once leaving the network and once as the upstream
	// ReachSpec's Start, matching the shipped asymmetry.
	TestEqual(TEXT("upstream edge charges the neighbor/start cost"),
		Iterator.NodeCost, 18.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorCandidateSpec,
	"WoT.PathNodes.Iterator.CandidateVisibilityAndDesperate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorCandidateSpec::RunTest(const FString&)
{
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		const FVector QueryStart(0, 0, 100);
		Wave2PathSpawnWall(World, FVector(100, 0, 100), FVector(10, 50, 200));
		AWoTPathNode* Occluded = Wave2PathSpawnNode(World, FVector(190, 0, 100));
		AWoTPathNode* Visible = Wave2PathSpawnNode(World, FVector(0, 220, 100));
		FWoTPathNodeIterator Iterator;
		TestTrue(TEXT("first visible candidate can produce a path"),
			Iterator.BuildPath(World, QueryStart, Visible->GetActorLocation(), false));
		TestTrue(TEXT("nearest occluded node is skipped for the first visible one"),
			Iterator.GetFirst() == Visible);
		TestTrue(TEXT("occluded node was genuinely nearer"),
			FVector::Dist(QueryStart, Occluded->GetActorLocation())
				< FVector::Dist(QueryStart, Visible->GetActorLocation()));
		WoTTest::DestroySpecWorld(World);
	}

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		const FVector QueryStart(0, 0, 100);
		Wave2PathSpawnWall(World, FVector(100, 0, 100), FVector(10, 300, 200));
		AWoTPathNode* Nearest = Wave2PathSpawnNode(World, FVector(190, 0, 100));
		Wave2PathSpawnNode(World, FVector(210, 120, 100));
		FWoTPathNodeIterator Iterator;
		TestTrue(TEXT("desperate mode accepts an occluded endpoint"),
			Iterator.BuildPath(World, QueryStart, Nearest->GetActorLocation(), true));
		TestTrue(TEXT("desperate mode takes the nearest candidate without a trace"),
			Iterator.GetFirst() == Nearest);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorRerelaxSpec,
	"WoT.PathNodes.Iterator.OpenQueueRerelaxation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorRerelaxSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPathNode* Start = Wave2PathSpawnNode(World, FVector(0, 0, 100));
	AWoTPathNode* Detour = Wave2PathSpawnNode(World, FVector(100, 100, 100));
	AWoTPathNode* ReachedTwice = Wave2PathSpawnNode(World, FVector(200, 0, 100));
	AWoTPathNode* Goal = Wave2PathSpawnNode(World, FVector(300, 0, 100));
	Wave2PathWire(Start, ReachedTwice, 50.0f); // inserted first, expensive
	Wave2PathWire(Start, Detour, 10.0f);
	Wave2PathWire(Detour, ReachedTwice, 1.0f);
	Wave2PathWire(ReachedTwice, Goal, 1.0f);

	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("re-relaxation graph builds"), Iterator.BuildPath(
		World, Start->GetActorLocation(), Goal->GetActorLocation(), false));
	TestEqual(TEXT("cheap predecessor replacement produces four nodes"),
		Iterator.NodeCount, 4);
	TestTrue(TEXT("path start"), Iterator.GetFirst() == Start);
	TestTrue(TEXT("cheap detour replaces expensive direct predecessor"),
		Iterator.GetNext() == Detour);
	TestTrue(TEXT("re-relaxed node follows detour"),
		Iterator.GetNext() == ReachedTwice);
	TestTrue(TEXT("path reaches goal"), Iterator.GetNext() == Goal);
	TestEqual(TEXT("reinserted queue weight is the cheap 10+1+1"),
		Iterator.NodeCost, 12.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathIteratorCapSpec,
	"WoT.PathNodes.Iterator.CapAndSingleNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathIteratorCapSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<AWoTPathNode*> Nodes;
	Nodes.Reserve(FWoTPathNodeIterator::MaxPathNodes + 1);
	for (int32 Index = 0; Index <= FWoTPathNodeIterator::MaxPathNodes; ++Index)
	{
		Nodes.Add(Wave2PathSpawnNode(World, FVector(Index * 10.0, 0, 100)));
		if (Index > 0)
		{
			Wave2PathWire(Nodes[Index - 1], Nodes[Index], 1.0f);
		}
	}
	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("513-node route still resolves"), Iterator.BuildPath(
		World, Nodes[0]->GetActorLocation(), Nodes.Last()->GetActorLocation(), false));
	TestEqual(TEXT("PathNodeIteratorII.uc:12 caps stored nodes at 512"),
		Iterator.NodeCount, FWoTPathNodeIterator::MaxPathNodes);
	TestTrue(TEXT("the capped path retains its first 512 nodes in order"),
		Iterator.GetLast() == Nodes[FWoTPathNodeIterator::MaxPathNodes - 1]);

	AWoTPathNode* Alone = Wave2PathSpawnNode(World, FVector(10000, 10000, 100));
	Alone->ExtraCost = 9;
	TestTrue(TEXT("same selected start/end node succeeds"), Iterator.BuildPath(
		World, Alone->GetActorLocation(), Alone->GetActorLocation(), false));
	TestEqual(TEXT("single-node fast path stores one node"), Iterator.NodeCount, 1);
	TestTrue(TEXT("single-node path contains the selected node"),
		Iterator.GetFirst() == Alone);
	TestEqual(TEXT("single-node path retains endpoint cost asymmetry"),
		Iterator.NodeCost, 9.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathSeekerChoiceSpec,
	"WoT.PathNodes.Seeker.ShortcutAndExplicitChoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathSeekerChoiceSpec::RunTest(const FString&)
{
	Wave2PathSetBackend(TEXT("paths"));
	UWorld* World = WoTTest::CreateSpecWorld();
	Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
	AWoTPathNode* First = Wave2PathSpawnNode(World, FVector(0, 128, 100));
	AWoTPathNode* Middle = Wave2PathSpawnNode(World, FVector(128, 128, 100));
	AWoTPathNode* Last = Wave2PathSpawnNode(World, FVector(256, 128, 100));
	Wave2PathWire(First, Middle, 128.0f);
	Wave2PathWire(Middle, Last, 128.0f);
	AActor* Destination = Wave2PathSpawnPointActor(World, FVector(256, 0, 100));

	auto SpawnSeeker = [&](bool bShortcuts)
	{
		AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
		Seeker->Speed = 100.0f;
		Seeker->bTakeShortcuts = bShortcuts;
		Seeker->SetDestination(Destination);
		Seeker->SetActorTickEnabled(false);
		Seeker->CalcNextDirection(false);
		return Seeker;
	};

	AWoTTestSeeker* Explicit = SpawnSeeker(false);
	TestTrue(TEXT("non-shortcut seeker advances to visible GetNext (uc:548-560)"),
		Explicit->NavPointNode == Middle);
	AWoTTestSeeker* Shortcut = SpawnSeeker(true);
	TestTrue(TEXT("shortcut seeker walks backward from GetLast (uc:534-547)"),
		Shortcut->NavPointNode == Last);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathSeekerBounceSpec,
	"WoT.PathNodes.Seeker.BounceAndTracerLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathSeekerBounceSpec::RunTest(const FString&)
{
	Wave2PathSetBackend(TEXT("paths"));
	UWorld* World = WoTTest::CreateSpecWorld();
	Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
	AWoTPathNode* Only = Wave2PathSpawnNode(World, FVector(128, 128, 100));
	AActor* Destination = Wave2PathSpawnPointActor(World, FVector(256, 0, 100));

	AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Seeker->Speed = 100.0f;
	Seeker->SetDestination(Destination);
	Seeker->SetActorTickEnabled(false);
	Seeker->CalcNextDirection(false);
	TestTrue(TEXT("single-node route establishes LastPoint"), Seeker->LastPoint == Only);
	Seeker->CalcNextDirection(true);
	TestTrue(TEXT("desperate cycle restricts the repeated LastPoint (uc:575-579)"),
		Seeker->RestrictedPoint == Only);
	TestTrue(TEXT("desperate one-node cycle calls base bounce -> Destroy (uc:575-585)"),
		Seeker->IsActorBeingDestroyed());

	AWoTTracerProjectile* Tracer = World->SpawnActor<AWoTTracerProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Tracer->SetDestination(Destination);
	Tracer->SetActorTickEnabled(false);
	Tracer->CalcNextDirection(false);
	Tracer->CalcNextDirection(true);
	TestFalse(TEXT("Tracer bounce override does not destroy the tracer"),
		Tracer->IsActorBeingDestroyed());
	TestTrue(TEXT("Tracer bounce disables later CalcNextDirection (Tracer uc:119-123)"),
		Tracer->IsCalcNextDirectionDisabled());
	const FVector LatchedVelocity = Tracer->ProjectileMovement->Velocity;
	Tracer->CalcNextDirection(false);
	TestTrue(TEXT("disabled tracer direction remains latched"),
		Tracer->ProjectileMovement->Velocity.Equals(LatchedVelocity));
	TestTrue(TEXT("latched tracer bee-lines toward DestLoc"),
		LatchedVelocity.GetSafeNormal().Equals(FVector::ForwardVector, 0.01));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathBackendCVarSpec,
	"WoT.PathNodes.Backend.LiveCVar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathBackendCVarSpec::RunTest(const FString&)
{
	IConsoleVariable* Backend = Wave2PathBackendCVar();
	if (!TestNotNull(TEXT("wot.SeekNavBackend is registered"), Backend))
	{
		return false;
	}
	TestEqual(TEXT("backend process default is paths"),
		FString(Backend->GetString()), FString(TEXT("paths")));
	Wave2PathSetBackend(TEXT("paths"));

	UWorld* World = WoTTest::CreateSpecWorld();
	Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
	AWoTPathNode* Guide = Wave2PathSpawnNode(World, FVector(128, 128, 100));
	AActor* Destination = Wave2PathSpawnPointActor(World, FVector(256, 0, 100));
	AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Seeker->Speed = 100.0f;
	Seeker->SetDestination(Destination);
	Seeker->SetActorTickEnabled(false);

	Seeker->CalcNextDirection(false);
	TestTrue(TEXT("paths value steers at the stamped node"),
		Seeker->NavPointNode == Guide);
	TestTrue(TEXT("paths reachability oracle finds the single-node route"),
		Seeker->CanReachDestination(Destination->GetActorLocation()));

	Wave2PathSetBackend(TEXT("navmesh"));
	Seeker->CalcNextDirection(false);
	TestTrue(TEXT("navmesh arm without nav data uses its bee-line fallback"),
		Seeker->ProjectileMovement->Velocity.GetSafeNormal().Equals(
			FVector::ForwardVector, 0.01));
	TestFalse(TEXT("navmesh reachability arm reports no path without nav data"),
		Seeker->CanReachDestination(Destination->GetActorLocation()));

	Wave2PathSetBackend(TEXT("paths"));
	Seeker->CalcNextDirection(false);
	TestTrue(TEXT("switching back live restores node steering"),
		Seeker->ProjectileMovement->Velocity.GetSafeNormal().Y > 0.5f);
	Wave2PathSetBackend(TEXT("unknown-value"));
	TestTrue(TEXT("unknown backend values behave as paths"),
		WoTSeekNavigationUsesPathNodes());
	Wave2PathSetBackend(TEXT("paths"));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathTracerRankingSpec,
	"WoT.PathNodes.Consumer.TracerNodeCostRanking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathTracerRankingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Wave2PathSpawnWall(World, FVector(150, 0, 100), FVector(10, 200, 200));
	AWoTPathNode* Start = Wave2PathSpawnNode(World, Caster->GetActorLocation());
	AWoTPathNode* NearEnd = Wave2PathSpawnNode(World, FVector(300, 0, 100));
	AWoTPathNode* FarEnd = Wave2PathSpawnNode(World, FVector(500, 0, 100));
	Wave2PathWire(Start, NearEnd, 1000.0f);
	Wave2PathWire(Start, FarEnd, 10.0f);
	AWoTSeal* Near = World->SpawnActor<AWoTSeal>(
		NearEnd->GetActorLocation(), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTSeal* Far = World->SpawnActor<AWoTSeal>(
		FarEnd->GetActorLocation(), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTAngrealInvTracer* Tracer = World->SpawnActor<AWoTAngrealInvTracer>(
		FVector(0, -500, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Tracer->SetOwner(Caster);
	WoTTest::TickWorld(World, 0.05f);

	Wave2PathSetBackend(TEXT("paths"));
	TestTrue(TEXT("retail paths arm ranks blocked seals by iterator NodeCost"),
		Tracer->GetBestTarget() == Far);
	Wave2PathSetBackend(TEXT("navmesh"));
	TestTrue(TEXT("retained no-nav arm ranks unreachable fallback by straight distance"),
		Tracer->GetBestTarget() == Near);
	Wave2PathSetBackend(TEXT("paths"));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathAutoTargetReachSpec,
	"WoT.PathNodes.Consumer.AutoTargetReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathAutoTargetReachSpec::RunTest(const FString&)
{
	Wave2PathSetBackend(TEXT("paths"));
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTCharacter* UnreachableNear = World->SpawnActor<AWoTCharacter>(
		FVector(300, -150, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTCharacter* ReachableFar = World->SpawnActor<AWoTCharacter>(
		FVector(400, 200, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Wave2PathSpawnWall(World, FVector(200, 0, 100), FVector(10, 300, 200));
	// Seal the nearer pawn inside world-static geometry so none of the seven
	// end candidates can see it; the farther pawn has an authored graph route.
	Wave2PathSpawnWall(World, UnreachableNear->GetActorLocation(), FVector(40, 40, 200));
	AWoTPathNode* Start = Wave2PathSpawnNode(World, Caster->GetActorLocation());
	AWoTPathNode* FarEnd = Wave2PathSpawnNode(World, ReachableFar->GetActorLocation());
	Wave2PathWire(Start, FarEnd, 25.0f);

	UWoTAutoTargetReflector* Auto = NewObject<UWoTAutoTargetReflector>(Caster);
	Auto->Install(Caster);
	AWoTAngrealInvTarget* Artifact = World->SpawnActor<AWoTAngrealInvTarget>(
		FVector(0, -500, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	Artifact->SetOwner(Caster);
	AActor* Picked = Artifact->FindBestTarget(
		Caster->GetActorLocation(), FRotator::ZeroRotator, 22.5f);
	TestTrue(TEXT("AutoTarget skips the nearer graph-unreachable pawn (uc:110-130)"),
		Picked == ReachableFar);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathBeamBendingSpec,
	"WoT.PathNodes.Beam.ContinuationBending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathBeamBendingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
	AWoTPathNode* First = Wave2PathSpawnNode(World, FVector(0, 128, 100));
	AWoTPathNode* Last = Wave2PathSpawnNode(World, FVector(256, 128, 100));
	Wave2PathWire(First, Last, 256.0f);
	AWoTBeamEffect* Beam = World->SpawnActor<AWoTBeamEffect>(Wave2PathAlwaysSpawn());
	Beam->Config = FName(TEXT("LightningStreak02"));
	Beam->Start = FVector(0, 0, 100);
	Beam->End = FVector(256, 0, 100);
	Beam->Tick(0.0f);

	const TArray<FVector>& Origins = Beam->GetLastSegmentOrigins();
	TestEqual(TEXT("three continuation runs produce six strict->64 origins"),
		Origins.Num(), 6);
	if (Origins.Num() == 6)
	{
		TestTrue(TEXT("mid-path origin follows the reached-point continuation"),
			Origins[2].Equals(FVector(62.09, 79.52, 100), 1.0));
		TestTrue(TEXT("the planned origins bend above the blocking wall"),
			Origins[4].Y > 100.0);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathBeamTeleportFallbackSpec,
	"WoT.PathNodes.Beam.TeleporterSkipAndFailureFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathBeamTeleportFallbackSpec::RunTest(const FString&)
{
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
		AWoTPathNode* First = Wave2PathSpawnNode(
			World, FVector(0, 128, 100), TEXT("Teleporter"));
		AWoTPathNode* Last = Wave2PathSpawnNode(
			World, FVector(256, 128, 100), TEXT("TeleporterWithEffects"));
		Wave2PathWire(First, Last, 256.0f);
		AWoTBeamEffect* Beam = World->SpawnActor<AWoTBeamEffect>(Wave2PathAlwaysSpawn());
		Beam->Config = FName(TEXT("LightningStreak02"));
		Beam->Start = FVector(0, 0, 100);
		Beam->End = FVector(256, 0, 100);
		Beam->Tick(0.0f);
		const TArray<FVector>& Origins = Beam->GetLastSegmentOrigins();
		TestEqual(TEXT("teleporter-pair jump leaves only the two outer runs"),
			Origins.Num(), 2);
		if (Origins.Num() == 2)
		{
			TestTrue(TEXT("no segment is placed between teleporter twins"),
				Origins[0].Equals(FVector(0, 0, 100))
				&& Origins[1].Equals(FVector(256, 128, 100)));
		}
		WoTTest::DestroySpecWorld(World);
	}

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		Wave2PathSpawnWall(World, FVector(128, 0, 100), FVector(10, 40, 200));
		AWoTBeamEffect* Beam = World->SpawnActor<AWoTBeamEffect>(Wave2PathAlwaysSpawn());
		Beam->Config = FName(TEXT("LightningStreak02"));
		Beam->Start = FVector(0, 0, 100);
		Beam->End = FVector(256, 0, 100);
		Beam->Tick(0.0f);
		const TArray<FVector>& Origins = Beam->GetLastSegmentOrigins();
		TestEqual(TEXT("BuildPath failure falls back to one direct strict run"),
			Origins.Num(), 3);
		for (const FVector& Origin : Origins)
		{
			TestEqual(TEXT("failure fallback stays on the direct line"),
				Origin.Y, 0.0);
		}
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPathVisBlockingSpec,
	"WoT.PathNodes.Seeker.PawnsAndProjectilesDoNotBlockSight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPathVisBlockingSpec::RunTest(const FString&)
{
	// TRACE_VisBlocking = TRACE_Level|TRACE_Movers in UE1 is what both
	// the seeker's direct trace (SeekingProjectile.uc:644-657, bTraceActors=False)
	// and path visibility (the native PathNodeIteratorII) use. A channel trace
	// stopped on the target's own capsule and on the other seekers, so every
	// seeker bee-lined through walls.
	Wave2PathSetBackend(TEXT("paths"));
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPathNode* Entry = Wave2PathSpawnNode(World, FVector(0, 0, 100));
	AWoTPathNode* Exit = Wave2PathSpawnNode(World, FVector(400, 0, 100));
	Wave2PathWire(Entry, Exit, 400.0f);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTCharacter* Bystander = World->SpawnActor<AWoTCharacter>(
		FVector(-100, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
		FVector(-200, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	AWoTTestSeeker* Ahead = World->SpawnActor<AWoTTestSeeker>(
		FVector(250, 0, 100), FRotator::ZeroRotator, Wave2PathAlwaysSpawn());
	if (!TestNotNull(TEXT("target pawn"), Target) || !TestNotNull(TEXT("bystander"), Bystander)
		|| !TestNotNull(TEXT("seeker"), Seeker) || !TestNotNull(TEXT("seeker ahead"), Ahead))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Seeker->SetActorTickEnabled(false);
	Ahead->SetActorTickEnabled(false);
	const FVector Goal = Target->GetActorLocation();

	FCollisionQueryParams Channel(SCENE_QUERY_STAT(WoTVisBlockingSpec), false, Seeker);
	TestTrue(TEXT("precondition: a WorldStatic channel trace stops on pawn/projectile collision"),
		World->LineTraceTestByChannel(Seeker->GetActorLocation(), Goal, ECC_WorldStatic, Channel));
	TestFalse(TEXT("VisBlocking ignores the bystander, the seeker ahead and the target capsule"),
		WoTVisBlockingTraceHits(World, Seeker->GetActorLocation(), Goal, Seeker));
	TestTrue(TEXT("seeker sees its target past pawns and seekers (uc:505-509)"),
		Seeker->CanDirectlyReachDestination(Goal));

	FWoTPathNodeIterator Iterator;
	TestTrue(TEXT("a path ending at a pawn builds: the exit trace runs into its capsule"),
		Iterator.BuildPath(World, FVector(-50, 0, 100), Goal, false));
	TestTrue(TEXT("the path enters at Entry"), Iterator.GetFirst() == Entry);
	TestTrue(TEXT("the path exits at Exit"), Iterator.GetLast() == Exit);

	Wave2PathSpawnWall(World, FVector(-150, 0, 100), FVector(10, 200, 200));
	TestTrue(TEXT("level geometry still blocks the seeker's sight"),
		WoTVisBlockingTraceHits(World, Seeker->GetActorLocation(), Goal, Seeker));
	TestFalse(TEXT("and its direct reach"), Seeker->CanDirectlyReachDestination(Goal));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
