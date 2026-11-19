#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotDecision.h"
#include "WoTBotRoster.h"
#include "AI/WoTCasterComponent.h"
#include "Engine/DamageEvents.h"
#include "TimerManager.h"

namespace WoTBotTest
{
struct FWoTBotDecisionAccess
{
	static void Think(AWoTBotController* B) { B->Think(); }
	static AWoTAngrealSpawnPool* Best(AWoTBotController* B, bool Roam = false)
	{ float Score; return B->BestPool(Score, Roam); }
	static void Visit(AWoTBotController* B, AWoTAngrealSpawnPool* P, float Age)
	{ B->PoolVisits.Add(P, B->GetWorld()->GetTimeSeconds() - Age); }
	static void Goal(AWoTBotController* B, AWoTAngrealSpawnPool* P, EWoTBotState State = EWoTBotState::SeekItem)
	{ B->State = State; B->SeekingPool = P; B->Roster->ClaimPool(B, P); }
	static void Engage(AWoTBotController* B, AWoTCharacter* E)
	{ B->SetTarget(E, true); B->State = EWoTBotState::Engage; }
	static void Remember(AWoTBotController* B) { B->RememberNearbyPools(); }
	static float Progress(AWoTBotController* B) { return B->LastCombatProgress; }
	static float FightScore(AWoTBotController* B) { return B->FightScore(); }
	static void Stale(AWoTBotController* B)
	{ B->LastCombatProgress = B->GetWorld()->GetTimeSeconds() - 3.1f; }
	static void CastOnRoute(AWoTBotController* B, AWoTCharacter* E, EWoTBotState State)
	{
		B->SetTarget(E, true); B->State = State; B->VisibleSince -= 1;
		B->Destination = FVector(-500, 0, 50); B->CastArtifacts();
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionScoresSpec, "WoT.Bots.Decisions.ScoreOrderingAndHysteresis", Flags)
bool FBotDecisionScoresSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	const float Stocked = WoTBotDecision::Collect(5, false, .8f, 3, true, 800, 1, false);
	const float Needed = WoTBotDecision::Collect(5, true, 0, 1, true, 800, 1, false);
	const float Distant = WoTBotDecision::Fight(2500, true, false, 1, 20, 1, 0, false);
	const float Threat = WoTBotDecision::Fight(300, true, true, 1, 20, .3f, 0, true);
	TestTrue(TEXT("stocked collecting retains value"), Stocked > 0);
	TestTrue(TEXT("missing classes and scarce offence increase collection"), Needed > Stocked);
	TestTrue(TEXT("low charges increase replenishment"), WoTBotDecision::Collect(5,false,.1f,3,true,800,1,false) > Stocked);
	TestTrue(TEXT("path length reduces collection"), WoTBotDecision::Collect(5,false,.8f,3,true,2400,1,false) < Stocked);
	TestTrue(TEXT("profile priority increases collection"), WoTBotDecision::Collect(9,false,.8f,3,true,800,1,false) > Stocked);
	TestTrue(TEXT("needed collection beats distant fight"), Needed > Distant);
	TestTrue(TEXT("close attacker beats collecting"), Threat > Needed);
	TestFalse(TEXT("distant opportunity cannot divert collection"), WoTBotDecision::ChooseFight(Stocked, 10, false, true, false));
	TestTrue(TEXT("close attacker may divert"), WoTBotDecision::ChooseFight(Needed, Threat, false, true, true));
	TestFalse(TEXT("small increase does not start fighting"), WoTBotDecision::ChooseFight(1, 1.3f, false, false, true));
	TestTrue(TEXT("small increase does not end fighting"), WoTBotDecision::ChooseFight(1.3f, 1, true, false, true));
	TestTrue(TEXT("clear advantage starts fighting"), WoTBotDecision::ChooseFight(1, 1.5f, false, false, true));
	TestFalse(TEXT("clear collection advantage ends fighting"), WoTBotDecision::ChooseFight(1.5f, 1, true, false, false));
	TestFalse(TEXT("no attack cannot fight"), WoTBotDecision::ChooseFight(0, 0, true, false, true));
	const float Running = WoTBotDecision::RoamWeight * WoTBotDecision::Roam(3000, 1, false);
	const float UpperHand = WoTBotDecision::Fight(1000, true, false, 1, 20, .3f, 0, false);
	TestTrue(TEXT("stocked running of the map keeps a value"), Running > 0);
	TestTrue(TEXT("a running bot commits to a clearly better fight"), WoTBotDecision::ChooseFight(Running, UpperHand, false, false, false));
	TestFalse(TEXT("that fight cannot divert a collection route"), WoTBotDecision::ChooseFight(Stocked, UpperHand, false, true, false));
	TestFalse(TEXT("an even fight at range does not pull a running bot"),
		WoTBotDecision::ChooseFight(Running, WoTBotDecision::Fight(1000, true, false, 1, 20, 1, 0, false), false, false, false));
	TestTrue(TEXT("roaming prefers far pools"), WoTBotDecision::Roam(3000, 1, false) > WoTBotDecision::Roam(800, 1, false));
	TestTrue(TEXT("roaming prefers unclaimed pools"), WoTBotDecision::Roam(800, 1, false) > WoTBotDecision::Roam(3000, 1, true));
	TestTrue(TEXT("health and ammo improve confidence"), Distant > WoTBotDecision::Fight(2500,true,false,.2f,1,1,0,false));
	TestTrue(TEXT("visible upper hand improves confidence"), WoTBotDecision::Fight(2500,true,false,1,20,.2f,0,false) > Distant);
	TestTrue(TEXT("unseen memory reduces confidence"), Distant > WoTBotDecision::Fight(2500,false,false,1,20,1,0,false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionClaimsSpec, "WoT.Bots.Decisions.PoolClaimsAndLifecycle", Flags)
bool FBotDecisionClaimsSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* A = Bot(W, FVector(0, 0, 50));
	AWoTBotController* B = Bot(W, FVector(0, 100, 50));
	AWoTAngrealSpawnPool* Near = Pool(W, FVector(500,0,30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-900,0,30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	UWoTBotRoster* R = W->GetSubsystem<UWoTBotRoster>();
	FWoTBotDecisionAccess::Think(A);
	TestTrue(TEXT("selected route claims pool"), R->IsPoolClaimed(Near, B));
	TestFalse(TEXT("own claim is excluded"), R->IsPoolClaimed(Near, A));
	TestEqual(TEXT("second bot prefers unclaimed route"), FWoTBotDecisionAccess::Best(B), Far);
	FWoTBotDecisionAccess::Goal(A, Far);
	TestFalse(TEXT("changing goal clears former claim"), R->IsPoolClaimed(Near, B));
	TestTrue(TEXT("new goal claimed"), R->IsPoolClaimed(Far, B));
	Far->PickupItem->Destroy(); Far->PickupItem = nullptr;
	FWoTBotDecisionAccess::Remember(A);
	TestFalse(TEXT("empty goal releases claim"), R->IsPoolClaimed(Far, B));
	FWoTBotDecisionAccess::Goal(A, Near);
	A->SetDeparturePending(true);
	TestFalse(TEXT("pending retirement releases claim"), R->IsPoolClaimed(Near, B));
	A->SetDeparturePending(false);
	FWoTBotDecisionAccess::Goal(A, Near);
	FDamageEvent Damage;
	CastChecked<AWoTCharacter>(A->GetPawn())->TakeDamage(1000, Damage, nullptr, nullptr);
	TestFalse(TEXT("death releases claim"), R->IsPoolClaimed(Near, B));
	FWoTBotDecisionAccess::Goal(B, Near);
	B->Retire();
	TestFalse(TEXT("retirement releases claim"), R->IsPoolClaimed(Near, nullptr));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionRouteSpec, "WoT.Bots.Decisions.CollectionRouteAndThreatDiversion", Flags)
bool FBotDecisionRouteSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W,FVector(0,0,50));
	AWoTAngrealSpawnPool* P = Pool(W,FVector(-900,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvLightning"));
	Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvSeeker"));
	AWoTCharacter* E = Enemy(W,FVector(2000,0,50));
	FWoTBotDecisionAccess::Think(B);
	TestTrue(TEXT("stocked bot still chooses supplies over distant combat"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	const FVector Destination = B->GetDestination();
	E->Health = 10;
	FWoTBotDecisionAccess::Think(B);
	TestTrue(TEXT("distant wounded enemy does not interrupt route"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	TestTrue(TEXT("route destination retained"), B->GetDestination().Equals(Destination));
	E->SetActorLocation(FVector(300,0,50));
	FDamageEvent Damage; CastChecked<AWoTCharacter>(B->GetPawn())->TakeDamage(1,Damage,E->GetController(),E);
	FWoTBotDecisionAccess::Think(B);
	TestTrue(TEXT("close attacker with clear advantage causes engagement"), B->GetBehaviourState() == EWoTBotState::Engage);
	TestFalse(TEXT("diversion releases collection claim"), W->GetSubsystem<UWoTBotRoster>()->IsPoolClaimed(P,nullptr));
	E->Destroy();
	FWoTBotDecisionAccess::Think(B);
	TestTrue(TEXT("after combat the bot resumes collection"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionCrowdingSpec, "WoT.Bots.Decisions.TargetCrowdingAndAttackerException", Flags)
bool FBotDecisionCrowdingSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* A = Bot(W, FVector(0,-100,50));
	AWoTBotController* B = Bot(W, FVector(0,100,50));
	AWoTBotController* C = Bot(W, FVector(0,0,50));
	AWoTCharacter* E = Enemy(W, FVector(800,0,50));
	AWoTCharacter* Other = Enemy(W, FVector(1200,200,50),2);
	FWoTBotDecisionAccess::Engage(A,E); FWoTBotDecisionAccess::Engage(B,E);
	TestEqual(TEXT("roster counts two other fighters"), W->GetSubsystem<UWoTBotRoster>()->FightersFor(E,C), 2);
	FWoTBotDecisionAccess::Think(C);
	TestEqual(TEXT("perception prefers uncrowded visible enemy"), C->GetTarget(), Other);
	FDamageEvent Damage; CastChecked<AWoTCharacter>(C->GetPawn())->TakeDamage(1,Damage,E->GetController(),E);
	FWoTBotDecisionAccess::Think(C);
	TestEqual(TEXT("attacker bypasses crowd penalty"), C->GetTarget(), E);
	TestTrue(TEXT("crowding reduces fight value"), WoTBotDecision::Fight(800,true,false,1,20,1,2,false)
		< WoTBotDecision::Fight(800,true,false,1,20,1,1,false));
	TestEqual(TEXT("attacker score ignores crowding"), WoTBotDecision::Fight(800,true,true,1,20,1,2,false),
		WoTBotDecision::Fight(800,true,true,1,20,1,0,false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionDisengageSpec, "WoT.Bots.Decisions.DisengageWithoutSightOrDamage", Flags)
bool FBotDecisionDisengageSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0,0,50));
	AWoTCharacter* E = Enemy(W, FVector(800,0,50));
	FWoTBotDecisionAccess::Engage(B,E);
	Box(W,FVector(400,0,100),FVector(30,300,150));
	FWoTBotDecisionAccess::Stale(B);
	FDamageEvent Damage; E->TakeDamage(1,Damage,B,E);
	TestEqual(TEXT("own dealt damage refreshes progress"), FWoTBotDecisionAccess::Progress(B), float(W->GetTimeSeconds()));
	FWoTBotDecisionAccess::Stale(B);
	CastChecked<AWoTCharacter>(B->GetPawn())->TakeDamage(1,Damage,E->GetController(),E);
	TestEqual(TEXT("taken damage refreshes progress"), FWoTBotDecisionAccess::Progress(B), float(W->GetTimeSeconds()));
	FWoTBotDecisionAccess::Think(B);
	TestTrue(TEXT("recent damage keeps remembered fight"), B->GetBehaviourState() == EWoTBotState::Engage);
	const float RememberedScore = FWoTBotDecisionAccess::FightScore(B);
	E->Health = 5;
	TestEqual(TEXT("hidden health changes cannot improve fight score"), FWoTBotDecisionAccess::FightScore(B), RememberedScore);
	FWoTBotDecisionAccess::Stale(B);
	FWoTBotDecisionAccess::Think(B);
	TestNull(TEXT("stalled fight releases target before four-second memory expiry"), B->GetTarget());
	TestTrue(TEXT("returns to map running"), B->GetBehaviourState() == EWoTBotState::Roam);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionRotationSpec, "WoT.Bots.Decisions.RotationAndFarRoaming", Flags)
bool FBotDecisionRotationSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W,FVector(0,0,50));
	AWoTAngrealSpawnPool* Near = Pool(W,FVector(500,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Far = Pool(W,FVector(-1800,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	TestEqual(TEXT("initial collection chooses near"), FWoTBotDecisionAccess::Best(B), Near);
	FWoTBotDecisionAccess::Visit(B,Near,5);
	TestEqual(TEXT("recent visit makes unvisited pool preferable"), FWoTBotDecisionAccess::Best(B), Far);
	FWoTBotDecisionAccess::Visit(B,Near,45);
	TestEqual(TEXT("visit penalty expires"), FWoTBotDecisionAccess::Best(B), Near);
	Near->PickupItem->Destroy(); Near->PickupItem = nullptr;
	Far->PickupItem->Destroy(); Far->PickupItem = nullptr;
	TestEqual(TEXT("roam heads toward far pool even when empty"), FWoTBotDecisionAccess::Best(B,true), Far);
	FWoTBotDecisionAccess::Visit(B,Far,5);
	TestEqual(TEXT("roam rotates away from recently empty pool"), FWoTBotDecisionAccess::Best(B,true), Near);
	FWoTBotDecisionAccess::Goal(B,Near);
	B->GetPawn()->SetActorLocation(Near->GetActorLocation() + FVector(0,0,20));
	FWoTBotDecisionAccess::Remember(B);
	TestFalse(TEXT("arrival releases claim"), W->GetSubsystem<UWoTBotRoster>()->IsPoolClaimed(Near,nullptr));
	TestNull(TEXT("recently empty pools are not collection goals"), FWoTBotDecisionAccess::Best(B));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionRoamCommitSpec, "WoT.Bots.Decisions.RoamingKeepsItsPoolUntilArrival", Flags)
bool FBotDecisionRoamCommitSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTAngrealSpawnPool* Ahead = Pool(W, FVector(2400, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Behind = Pool(W, FVector(-1600, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	for (AWoTAngrealSpawnPool* P : { Ahead, Behind }) { P->PickupItem->Destroy(); P->PickupItem = nullptr; }
	TestEqual(TEXT("running the map picks the farther pool"), FWoTBotDecisionAccess::Best(B, true), Ahead);
	FWoTBotDecisionAccess::Goal(B, Ahead, EWoTBotState::Roam);
	// Nearly there: the pool left behind is now much farther than the goal.
	B->GetPawn()->SetActorLocation(FVector(2000, 0, 50));
	TestEqual(TEXT("the goal is kept until arrival"), FWoTBotDecisionAccess::Best(B, true), Ahead);
	AWoTBotController* Other = Bot(W, FVector(0, 600, 50));
	W->GetSubsystem<UWoTBotRoster>()->ClaimPool(Other, Ahead);
	TestEqual(TEXT("another bot's claim reopens the choice"), FWoTBotDecisionAccess::Best(B, true), Behind);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDecisionRouteEndSpec, "WoT.Bots.Decisions.UntouchablePoolDoesNotStall", Flags)
bool FBotDecisionRouteEndSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	// Within a jump's reach above the floor, but behind a plate the mesh does
	// not see: the route ends below it and neither standing nor jumping touches it.
	Box(W, FVector(500, 0, 105), FVector(60, 60, 2), false);
	AWoTAngrealSpawnPool* High = Pool(W, FVector(500, 0, 150), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-900, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("nearer raised pool chosen"), B->GetBehaviourState() == EWoTBotState::SeekItem && B->GetDestination().X > 0);
	WoTTest::TickWorld(W, 5.0f);
	TestNotNull(TEXT("raised item really out of reach"), High->PickupItem.Get());
	TestFalse(TEXT("route end releases the claim"), W->GetSubsystem<UWoTBotRoster>()->IsPoolClaimed(High, nullptr));
	TestTrue(TEXT("bot moves on to the reachable pool"), B->GetDestination().X < 0 || !Far->PickupItem);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotDecisionRouteCastSpec, "WoT.Bots.Decisions.OpportunisticRouteCasting", Flags)
void FBotDecisionRouteCastSpec::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{ for (const TCHAR* Name : {TEXT("SeekItem"),TEXT("Roam")}) { Names.Add(Name); Commands.Add(Name); } }
bool FBotDecisionRouteCastSpec::RunTest(const FString& Name)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W,FVector(0,0,50));
	AWoTCharacter* E = Enemy(W,FVector(1000,0,50));
	const EWoTBotState State = Name == TEXT("SeekItem") ? EWoTBotState::SeekItem : EWoTBotState::Roam;
	FWoTBotDecisionAccess::CastOnRoute(B,E,State);
	TestTrue(TEXT("route allows real starting artifact shot"), Kit(B)->CastUseCount > 0);
	TestTrue(TEXT("casting preserves goal state"), B->GetBehaviourState() == State);
	TestTrue(TEXT("casting preserves route destination"), B->GetDestination().Equals(FVector(-500,0,50)));
	return !HasAnyErrors();
}
}
#endif
