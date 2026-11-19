#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotDecision.h"
#include "WoTBotLinks.h"
#include "WoTBotRoster.h"
#include "AI/WoTPathNode.h"
#include "WoTTeleporter.h"
#include "Components/CapsuleComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"

namespace WoTBotTest
{
struct FWoTBotTraversalAccess
{
	static AWoTAngrealSpawnPool* Best(AWoTBotController* B) { float Score; return B->BestPool(Score); }
	// An enemy last sensed at Seen and no longer visible: the chase repaths now.
	static void Chase(AWoTBotController* B, AWoTCharacter* E, const FVector& Seen)
	{
		B->SetTarget(E, false);
		B->State = EWoTBotState::Engage;
		B->LastKnownPosition = Seen;
		B->NextRepathTime = 0.0f;
		B->ChooseDestination();
	}
	static void Think(AWoTBotController* B) { B->Think(); }
	// One jump made for the pool, then a fight took the goal without releasing it.
	static void LeaveAfterHop(AWoTBotController* B, const AWoTAngrealSpawnPool* P)
	{
		B->HopPool = P;
		B->HopCount = 1;
		B->SeekingPool.Reset();
		B->State = EWoTBotState::Engage;
	}
	static int32 HopsFor(const AWoTBotController* B, const AWoTAngrealSpawnPool* P) { return B->HopPool.Get() == P ? B->HopCount : 0; }
	static bool Uncollectable(const AWoTBotController* B, AWoTAngrealSpawnPool* P) { return B->UncollectablePools.Contains(P); }
};

namespace
{
const ANavigationData* BotData(UWorld* W, const AWoTBotController* B)
{
	const UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	return Nav ? Nav->GetNavDataForProps(B->GetPawn()->GetNavAgentPropertiesRef()) : nullptr;
}
bool CompleteRoute(UWorld* W, const AWoTBotController* B, const FVector& From, const FVector& To)
{
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	const ANavigationData* Data = BotData(W, B);
	FNavLocation Start, End;
	if (!Data || !Nav->ProjectPointToNavigation(From, Start, FVector(50, 50, 150), Data)
		|| !Nav->ProjectPointToNavigation(To, End, FVector(50, 50, 150), Data)) return false;
	FPathFindingQuery Query(B, *Data, Start.Location, End.Location);
	Query.SetAllowPartialPaths(false);
	const FPathFindingResult Path = Nav->FindPathSync(Data->GetConfig(), Query);
	return Path.IsSuccessful() && Path.Path.IsValid() && !Path.Path->IsPartial();
}
AWoTPathNode* Node(UWorld* W, const FVector& Location, const TCHAR* SourceClass = TEXT("Engine.PathNode"))
{
	AWoTPathNode* N = W->SpawnActor<AWoTPathNode>(Location, FRotator::ZeroRotator);
	N->SourceClass = FName(SourceClass);
	return N;
}
void Reach(AWoTPathNode* From, AWoTPathNode* To)
{
	FWoTPathEdge& Edge = From->OutEdges.AddDefaulted_GetRef();
	Edge.Other = To;
	Edge.Distance = FVector::Dist(From->GetActorLocation(), To->GetActorLocation());
}
// Ticks until the pool's item is taken (a single-shot pool then destroys itself).
bool Collected(UWorld* W, AWoTAngrealSpawnPool* P, float Seconds)
{
	const TWeakObjectPtr<AWoTAngrealSpawnPool> Weak = P;
	for (int32 I = 0; I < FMath::CeilToInt(Seconds * 30.0f); ++I)
	{
		if (!Weak.IsValid() || !Weak->PickupItem) return true;
		WoTTest::TickWorld(W, 1.0f / 30.0f);
	}
	return !Weak.IsValid() || !Weak->PickupItem;
}
AWoTTeleporter* Teleporter(UWorld* W, const FVector& Location, const TCHAR* URL, const TCHAR* Tag)
{
	const FTransform Transform(Location);
	AWoTTeleporter* T = W->SpawnActorDeferred<AWoTTeleporter>(AWoTTeleporter::StaticClass(), Transform);
	T->URL = URL;
	T->TeleTag = FName(Tag);
	T->FinishSpawning(Transform);
	return T;
}
// Links dirty the mesh around them; process those tiles before measuring routes.
void SettleNavigation(UWorld* W)
{
	WoTTest::TickWorld(W, .1f);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	for (ANavigationData* Data : Nav->NavDataSet) if (Data) Data->EnsureBuildCompletion();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPickupReachSpec, "WoT.Bots.Decisions.PickupReachStandHopNone", Flags)
bool FBotPickupReachSpec::RunTest(const FString&)
{
	using namespace WoTBotDecision;
	const FVector Feet = FVector::ZeroVector;
	TestTrue(TEXT("a pickup on the floor beside the feet is touched standing"), PickupReach(Feet, FVector(30, 0, 30), 30) == EPickupReach::Stand);
	TestTrue(TEXT("a pickup on a pedestal one stride away needs a jump"), PickupReach(Feet, FVector(50, 0, 60), 30) == EPickupReach::Hop);
	TestTrue(TEXT("a pickup floating overhead is reached at the top of a jump"), PickupReach(Feet, FVector(0, 0, 150), 30) == EPickupReach::Hop);
	TestTrue(TEXT("a pickup below the floor is never touched"), PickupReach(Feet, FVector(0, 0, -60), 30) == EPickupReach::None);
	TestTrue(TEXT("a pickup above the jump's reach is never touched"), PickupReach(Feet, FVector(0, 0, 200), 30) == EPickupReach::None);
	TestTrue(TEXT("a pickup beyond a jump's stride is never touched"), PickupReach(Feet, FVector(150, 0, 60), 30) == EPickupReach::None);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRaisedPickupSpec, "WoT.Bots.Movement.RaisedPickupCollectedByJumping", Flags)
bool FBotRaisedPickupSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	// Above a standing body's reach (92 tall) but within a jump's.
	AWoTAngrealSpawnPool* High = Pool(W, FVector(500, 0, 150), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("the raised pool is chosen"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	TestTrue(TEXT("jumping at the route's end collects it"), Collected(W, High, 6.0f));
	TestTrue(TEXT("collected by a jump"), B->GetHopCount() > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPedestalSpec, "WoT.Bots.Movement.PedestalPickupCollectedByClimbing", Flags)
bool FBotPedestalSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A 30-unit pedestal: one voxel-rounded step for the mesh, above the body's
	// own step (25), so the walk stops at its side.
	Box(W, FVector(500, 0, 15), FVector(32, 32, 15));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTAngrealSpawnPool* Top = Pool(W, FVector(500, 0, 60), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestTrue(TEXT("the pedestal's pickup is collected"), Collected(W, Top, 8.0f));
	TestTrue(TEXT("by jumping onto the pedestal"), B->GetClimbJumpCount() + B->GetHopCount() > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSunkenFloorSpec, "WoT.Bots.Movement.SunkenFloorLeftByJumping", Flags)
bool FBotSunkenFloorSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A raised floor 29 high everywhere but a 400-square pit around the body:
	// the mesh steps out of the pit, the walk cannot.
	Box(W, FVector(-1300, 0, 14.5f), FVector(1100, 1900, 14.5f));
	Box(W, FVector(1300, 0, 14.5f), FVector(1100, 1900, 14.5f));
	Box(W, FVector(0, -1100, 14.5f), FVector(200, 900, 14.5f));
	Box(W, FVector(0, 1100, 14.5f), FVector(200, 900, 14.5f));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTAngrealSpawnPool* Out = Pool(W, FVector(700, 0, 60), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	if (!TestTrue(TEXT("the mesh leads out of the pit"), CompleteRoute(W, B, FVector(0, 0, 50), FVector(700, 0, 80)))) return false;
	TestTrue(TEXT("the pickup beyond the pit's edge is collected"), Collected(W, Out, 8.0f));
	TestTrue(TEXT("by jumping the edge"), B->GetClimbJumpCount() > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotBuriedPoolSpec, "WoT.Bots.Decisions.BuriedPoolNeverChosen", Flags)
bool FBotBuriedPoolSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	// Inside the floor: it projects onto the floor above it, where no body reaches it.
	Pool(W, FVector(400, 0, -60), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-900, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestEqual(TEXT("the farther reachable pool is chosen over the nearer buried one"), FWoTBotTraversalAccess::Best(B), Far);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotUnreachableChaseSpec, "WoT.Bots.Movement.UnreachableChaseKeepsMoving", Flags)
bool FBotUnreachableChaseSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A block 300 high: its top has mesh but no route from the floor.
	Box(W, FVector(900, 0, 150), FVector(200, 200, 150));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(900, 0, 350));
	if (!TestFalse(TEXT("the top of the block is out of reach"), CompleteRoute(W, B, FVector(0, 0, 50), FVector(900, 0, 350)))) return false;
	FWoTBotTraversalAccess::Chase(B, E, E->GetActorLocation());
	TestTrue(TEXT("the chase follows a route rather than idling"),
		B->GetPathFollowingComponent()->GetStatus() == EPathFollowingStatus::Moving);
	TestTrue(TEXT("its destination is reachable"), CompleteRoute(W, B, B->GetPawn()->GetActorLocation(), B->GetDestination()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotOffMeshSpec, "WoT.Bots.Movement.OffMeshBodyReturnsToMesh", Flags)
bool FBotOffMeshSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A post too narrow to carry mesh and too tall for the mesh queries to see
	// the floor from its top: a body left there has no route anywhere.
	Box(W, FVector(300, 0, 200), FVector(20, 20, 200));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(300, 0, 450));
	const TWeakObjectPtr<APawn> Body = B->GetPawn();
	WoTTest::TickWorld(W, 4.0f);
	TestTrue(TEXT("the same body is still playing"), Body.IsValid() && Body == B->GetPawn());
	TestTrue(TEXT("it left the post for the floor"), Body.IsValid() && Body->GetActorLocation().Z < 60);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRetailLinkSpec, "WoT.Bots.Navigation.RetailReachLinksJoinRaisedFloor", Flags)
bool FBotRetailLinkSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A platform 44 high: beyond the mesh's step, within a jump.
	Box(W, FVector(1200, 0, 22), FVector(400, 400, 22));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-200, 0, 50));
	AWoTPathNode* Floor = Node(W, FVector(600, 0, 50));
	AWoTPathNode* Platform = Node(W, FVector(1000, 0, 94));
	Reach(Floor, Platform);
	Reach(Platform, Floor);
	if (!TestFalse(TEXT("the platform is an island of its own"), CompleteRoute(W, B, FVector(600, 0, 50), FVector(1000, 0, 94)))) return false;
	TestEqual(TEXT("one link each way along the map's reaches"), WoTBotLinks::Build(*W), 2);
	SettleNavigation(W);
	TestTrue(TEXT("the floor now routes onto the platform"), CompleteRoute(W, B, FVector(0, 0, 50), FVector(1200, 0, 94)));
	TestTrue(TEXT("and back down"), CompleteRoute(W, B, FVector(1200, 0, 94), FVector(0, 0, 50)));
	AWoTAngrealSpawnPool* Up = Pool(W, FVector(1300, 0, 74), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestTrue(TEXT("the platform's pickup is collected"), Collected(W, Up, 10.0f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotGapLinkSpec, "WoT.Bots.Navigation.GapReachCrossedByJumping", Flags)
bool FBotGapLinkSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// Two floors 200 high split by a gap 160 wide; the map's reach joins their
	// edges at one height. Walking that reach drops the body into the gap.
	Box(W, FVector(-1080, 0, 100), FVector(1000, 600, 100));
	Box(W, FVector(1080, 0, 100), FVector(1000, 600, 100));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-800, 0, 250));
	AWoTPathNode* Near = Node(W, FVector(-140, 0, 250));
	AWoTPathNode* Far = Node(W, FVector(140, 0, 250));
	Reach(Near, Far);
	Reach(Far, Near);
	if (!TestFalse(TEXT("the far floor is an island of its own"), CompleteRoute(W, B, FVector(-800, 0, 250), FVector(600, 0, 250)))) return false;
	TestEqual(TEXT("one link each way across the gap"), WoTBotLinks::Build(*W), 2);
	SettleNavigation(W);
	AWoTAngrealSpawnPool* Across = Pool(W, FVector(600, 0, 230), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestTrue(TEXT("the pickup across the gap is collected"), Collected(W, Across, 10.0f));
	TestTrue(TEXT("by jumping at the gap's edge"), B->GetClimbJumpCount() > 0);
	TestTrue(TEXT("without falling into the gap"), B->GetPawn() && B->GetPawn()->GetActorLocation().Z > 150.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotTeleporterLinkSpec, "WoT.Bots.Navigation.TeleporterLinkCrossesWall", Flags)
bool FBotTeleporterLinkSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A wall splits the floor; only the teleporter joins the halves.
	Box(W, FVector(0, 0, 300), FVector(20, 2000, 300));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-1200, 0, 50));
	Teleporter(W, FVector(-600, 0, 50), TEXT("Far"), TEXT("Near"));
	Teleporter(W, FVector(600, 0, 50), TEXT(""), TEXT("Far"));
	AWoTPathNode* Entry = Node(W, FVector(-600, 0, 50), TEXT("Engine.Teleporter"));
	AWoTPathNode* Exit = Node(W, FVector(600, 0, 50), TEXT("Engine.Teleporter"));
	Reach(Entry, Exit);
	if (!TestFalse(TEXT("the far half has no walking route"), CompleteRoute(W, B, FVector(-1200, 0, 50), FVector(1000, 0, 50)))) return false;
	TestEqual(TEXT("the teleporter's reach becomes a link"), WoTBotLinks::Build(*W), 1);
	SettleNavigation(W);
	AWoTAngrealSpawnPool* Beyond = Pool(W, FVector(1000, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestTrue(TEXT("the pickup past the wall is collected through the teleporter"), Collected(W, Beyond, 10.0f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDistantTeleporterSpec, "WoT.Bots.Navigation.DistantTeleporterRouted", Flags)
bool FBotDistantTeleporterSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A wall splits the floor, and a third floor lies beyond a gap. Teleporters
	// 3,300 apart (Arena_06's longest pair is 3,258) join the halves, and another
	// pair reaches 7,400 to the far floor: both span several mesh tiles, too far
	// for a mesh link, so the bot walks into the right one itself.
	Box(W, FVector(0, 0, 300), FVector(20, 2000, 300));
	Box(W, FVector(4800, 0, -50), FVector(800, 2000, 50));
	BuildNav(W, FVector(5700, 1900, 400));
	AWoTBotController* B = Bot(W, FVector(-1200, 0, 50));
	Teleporter(W, FVector(-1650, 0, 50), TEXT("Far"), TEXT("Near"));
	Teleporter(W, FVector(1650, 0, 50), TEXT(""), TEXT("Far"));
	Teleporter(W, FVector(-2600, 0, 50), TEXT("Beyond"), TEXT("Start"));
	Teleporter(W, FVector(4800, 0, 50), TEXT(""), TEXT("Beyond"));
	Reach(Node(W, FVector(-1650, 0, 50), TEXT("Engine.Teleporter")), Node(W, FVector(1650, 0, 50), TEXT("Engine.Teleporter")));
	Reach(Node(W, FVector(-2600, 0, 50), TEXT("Engine.Teleporter")), Node(W, FVector(4800, 0, 50), TEXT("Engine.Teleporter")));
	if (!TestFalse(TEXT("the far half has no walking route"), CompleteRoute(W, B, FVector(-1200, 0, 50), FVector(2400, 0, 50)))) return false;
	UWoTBotRoster* Roster = W->GetSubsystem<UWoTBotRoster>();
	TestEqual(TEXT("no mesh link spans more than the neighbouring tiles"), Roster->BuildLinks(), 0);
	TestEqual(TEXT("both teleporter reaches are kept for the bots to route through"), Roster->GetDistantTeleports().Num(), 2);
	SettleNavigation(W);
	AWoTAngrealSpawnPool* Beyond = Pool(W, FVector(2400, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestTrue(TEXT("the pickup past the wall is collected through the distant teleporter"), Collected(W, Beyond, 12.0f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotArrivalPadSpec, "WoT.Bots.Navigation.ArrivalPadLeftBeforeReuse", Flags)
bool FBotArrivalPadSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A wall splits the floor; two pads 3,300 apart, each with an 80-unit reach
	// (Arena_06's), join the halves both ways. A body the east pad sets down
	// stands within that pad's reach, which sends it back west only after the
	// body has stepped out of the reach.
	Box(W, FVector(0, 0, 300), FVector(20, 2000, 300));
	BuildNav(W);
	for (AWoTTeleporter* Pad : { Teleporter(W, FVector(-1650, 0, 50), TEXT("East"), TEXT("West")),
		Teleporter(W, FVector(1650, 0, 50), TEXT("West"), TEXT("East")) })
	{
		Pad->UE1CollisionRadius = Pad->UE1CollisionHalfHeight = 80.0f;
		Pad->Collision->SetCapsuleSize(80.0f, WoTUE1Collision::ContainingCapsuleHalfHeight(80.0f, 80.0f));
	}
	AWoTPathNode* WestNode = Node(W, FVector(-1650, 0, 50), TEXT("Engine.Teleporter"));
	AWoTPathNode* EastNode = Node(W, FVector(1650, 0, 50), TEXT("Engine.Teleporter"));
	Reach(WestNode, EastNode);
	Reach(EastNode, WestNode);
	UWoTBotRoster* Roster = W->GetSubsystem<UWoTBotRoster>();
	TestEqual(TEXT("no mesh link spans the pads"), Roster->BuildLinks(), 0);
	TestEqual(TEXT("both reaches are kept for the bots to route through"), Roster->GetDistantTeleports().Num(), 2);
	AWoTBotController* B = Bot(W, FVector(-1200, 0, 50));
	// Collected on arrival: the body is set down beside it, within the pad's reach.
	AWoTAngrealSpawnPool* ByPad = Pool(W, FVector(1690, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	if (!TestTrue(TEXT("the pickup by the east pad is collected through the west pad"), Collected(W, ByPad, 12.0f))) return false;
	WoTTest::TickWorld(W, .1f);
	const FVector EastPad(1650, 0, 50);
	if (!TestTrue(TEXT("the body stands within the east pad's reach"), FVector::Dist2D(B->GetPawn()->GetActorLocation(), EastPad) < 97.0f)) return false;
	AWoTAngrealSpawnPool* Back = Pool(W, FVector(-1000, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FWoTBotTraversalAccess::Think(B);
	TestEqual(TEXT("the pickup back west is sought through the pad"), B->GetSeekingPool(), Back);
	TestTrue(TEXT("by first stepping out of the pad's reach"), FVector::Dist2D(B->GetDestination(), EastPad) >= 97.0f);
	TestTrue(TEXT("the pickup back west is collected through the pad it arrived on"), Collected(W, Back, 12.0f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotAboveMeshSpec, "WoT.Bots.Movement.StrandedAboveMeshReplaced", Flags)
bool FBotAboveMeshSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->StuckRespawnSeconds = 4;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	// A floating plate too narrow to carry mesh and too high for the mesh
	// queries to see the floor from its top, with the floor's mesh straight
	// below it: no walk leads off, so the stuck rule must replace the body.
	Box(W, FVector(300, 0, 400), FVector(12, 12, 6));
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(300, 0, 456));
	const TWeakObjectPtr<APawn> Body = B->GetPawn();
	WoTTest::TickWorld(W, 7.0f);
	TestTrue(TEXT("the stranded body was replaced"), !Body.IsValid() || Body != B->GetPawn() || Body->GetActorLocation().Z < 200.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotHopResetSpec, "WoT.Bots.Decisions.HopStateClearedOnGoalSwitch", Flags)
bool FBotHopResetSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-900, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	WoTTest::TickWorld(W, .5f);
	FWoTBotTraversalAccess::LeaveAfterHop(B, Far);
	FWoTBotTraversalAccess::Think(B);
	TestEqual(TEXT("the pool is chosen again"), B->GetSeekingPool(), Far);
	TestEqual(TEXT("with no jumps counted for it"), FWoTBotTraversalAccess::HopsFor(B, Far), 0);
	FWoTBotTraversalAccess::Think(B);
	TestTrue(TEXT("and still sought for collection"), B->GetBehaviourState() == EWoTBotState::SeekItem && B->GetSeekingPool() == Far);
	TestFalse(TEXT("not written off as uncollectable"), FWoTBotTraversalAccess::Uncollectable(B, Far));
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
