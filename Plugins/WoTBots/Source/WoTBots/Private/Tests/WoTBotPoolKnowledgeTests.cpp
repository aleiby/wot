#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotRoster.h"
#include "Engine/DamageEvents.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "WoTBotDecision.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "WoTSoundTable.h"
#include "EngineUtils.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTMapLightTable.h"
#endif
#include "TimerManager.h"

namespace WoTBotTest
{
struct FWoTBotPoolAccess
{
	static void Observe(AWoTBotController* B) { B->RememberNearbyPools(); }
	static AWoTAngrealSpawnPool* Best(AWoTBotController* B, float* Out = nullptr)
	{ float Score; auto* P = B->BestPool(Score); if (Out) *Out = Score; return P; }
	static void Goal(AWoTBotController* B, AWoTAngrealSpawnPool* P)
	{ B->SeekingPool = P; B->State = EWoTBotState::SeekItem; B->bWantsMovement = true; }
	static void Perceive(AWoTBotController* B) { B->Perceive(); }
	static void Engage(AWoTBotController* B, AWoTCharacter* E)
	{ B->SetTarget(E, true); B->State = EWoTBotState::Engage; }
	static float Fight(AWoTBotController* B) { return B->FightScore(); }
	static void StaleProgress(AWoTBotController* B) { B->LastCombatProgress -= 10; }
	static float Progress(AWoTBotController* B) { return B->LastCombatProgress; }
	static float SightSince(AWoTBotController* B) { return B->VisibleSince; }
	static void Cast(AWoTBotController* B) { B->CastArtifacts(); }
	static void Aim(AWoTBotController* B) { B->UpdateAim(); }
	static void Recover(AWoTBotController* B) { B->ChooseDestination(true); }
	static const FWoTBotPoolBelief& Belief(AWoTBotController* B, AWoTAngrealSpawnPool* P)
	{ return B->PoolBeliefs.FindChecked(P); }
	static float Delay(AWoTBotController* B, AWoTAngrealSpawnPool* P) { return B->PoolRespawnDelay(P); }
};

static AWoTBotController* QuietBot(UWorld* W, FVector At)
{
	auto* B = Bot(W, At);
	W->GetTimerManager().ClearAllTimersForObject(B);
	B->SetActorTickEnabled(false);
	CastChecked<AWoTCharacter>(B->GetPawn())->GetCharacterMovement()->DisableMovement();
	return B;
}
static void EmptyPool(AWoTAngrealSpawnPool* P)
{ if (P->PickupItem) P->PickupItem->Destroy(); P->PickupItem = nullptr; }
static AWoTAngrealSpawnPool* KnowledgePool(UWorld* W)
{
	auto* P = Pool(W, FVector(700,0,30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	P->RespawnTime = 30;
	return P;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPendingReactionTargetReviewSpec, "WoT.Bots.ReviewFix2.PendingDamageReactionRefreshesCurrentTarget", Flags)
bool FPendingReactionTargetReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .6f; Settings.Value->TargetMemorySeconds = .2f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* A = Enemy(W,FVector(900,200,50));
	FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("A is acquired through sight"),B->GetTarget(),A);
	const float Since = FWoTBotPoolAccess::SightSince(B);
	WoTTest::TickWorld(W,.7f);
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(-10000,0,0));
	auto* Candidate = Enemy(W,FVector(500,-200,50));
	FWoTBotPoolAccess::Perceive(B);
	WoTTest::TickWorld(W,.05f);
	A->SetActorLocation(FVector(900,250,50)); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("pending candidate does not replace A"),B->GetTarget(),A);
	TestTrue(TEXT("A really remains visible during the initial wait"),B->IsTargetVisible());
	TestEqual(TEXT("continuous sight is not restarted"),FWoTBotPoolAccess::SightSince(B),Since);
	TestTrue(TEXT("visible A refreshes its remembered position during the wait"),B->GetLastKnownPosition().Equals(A->GetActorLocation()));
	const FVector Known = B->GetLastKnownPosition();
	Box(W,FVector(700,300,100),FVector(30,100,150),false);
	A->SetActorLocation(FVector(900,400,50)); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("hidden A stays remembered before expiry"),B->GetTarget(),A);
	TestFalse(TEXT("hidden A cannot remain marked visible during the wait"),B->IsTargetVisible());
	TestTrue(TEXT("hidden movement does not update last known position"),B->GetLastKnownPosition().Equals(Known));
	auto* Item = Kit(B); const int32 Shots = Item->CastUseCount;
	FWoTBotPoolAccess::Cast(B);
	TestEqual(TEXT("no attack starts against hidden A during the wait"),Item->CastUseCount,Shots);
	WoTTest::TickWorld(W,.25f); FWoTBotPoolAccess::Perceive(B);
	TestNull(TEXT("A's memory expires even while candidate reaction is pending"),B->GetTarget());
	WoTTest::TickWorld(W,.35f); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("candidate is acquired only after its reaction completes"),B->GetTarget(),Candidate);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollectionRotationReviewSpec, "WoT.Bots.ReviewFix2.RefillPreservesCollectionRotation", Flags)
bool FCollectionRotationReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* P = KnowledgePool(W);
	P->bRespawn = true;
	auto* Alternative = Pool(W,FVector(1600,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FWoTBotPoolAccess::Goal(B,P);
	B->GetPawn()->SetActorLocation(P->GetActorLocation()+FVector(0,0,40));
	TestNull(TEXT("ordinary collection really takes the pickup"),P->PickupItem.Get());
	FWoTBotPoolAccess::Observe(B);
	B->GetPawn()->SetActorLocation(FVector(0,0,50)); B->SetControlRotation(FRotator(0,180,0));
	WoTTest::TickWorld(W,10.0f);
	P->SpawnFromPool(true); FWoTBotPoolAccess::Observe(B);
	TestTrue(TEXT("audible refill updates stock belief"),FWoTBotPoolAccess::Belief(B,P).Rarity != 0);
	TestFalse(TEXT("refill is believed stocked"),FWoTBotPoolAccess::Belief(B,P).bEmpty);
	TestEqual(TEXT("visited refill still yields to an unvisited alternative"),FWoTBotPoolAccess::Best(B),Alternative);
	WoTTest::TickWorld(W,36.0f); FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("collection rotation eventually expires and permits nearer refill"),FWoTBotPoolAccess::Best(B),P);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageTurnReviewSpec, "WoT.Bots.ReviewFix.DamageTurnsFromOwnImpulse", Flags)
bool FDamageTurnReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .3f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(-800,0,50));
	auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	// The credited attacker is deliberately elsewhere: the body feels +X momentum.
	auto* Credited = Enemy(W,FVector(0,800,50));
	Body->TakeRawDamage(1,TEXT("hurt"),Credited->GetController(),Body->GetActorLocation(),FVector(10000,0,0));
	FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("turns toward the physical hit, not credited attacker"), B->GetControlRotation().Vector().X < -.9f);
	TestNull(TEXT("damage cue supplies no identity"), B->GetTarget());
	FWoTBotPoolAccess::Perceive(B);
	TestNull(TEXT("new sight after hit waits for reaction"), B->GetTarget());
	WoTTest::TickWorld(W,.35f); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("normal sight acquires the actually visible enemy after reaction"), B->GetTarget(), E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageSearchReviewSpec, "WoT.Bots.ReviewFix.DamageSearchPersistsAndExpires", Flags)
bool FDamageSearchReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	BuildNav(W); auto* P = KnowledgePool(W); FWoTBotPoolAccess::Goal(B,P);
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(10000,0,0));
	WoTTest::TickWorld(W,.5f); FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("short search holds the hit direction without a target"),B->GetControlRotation().Vector().X < -.9f);
	TestNull(TEXT("search invents no enemy"),B->GetTarget());
	WoTTest::TickWorld(W,3.0f);
	B->SetControlRotation(FRotator::ZeroRotator); FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("search expires"),B->GetControlRotation().Vector().X > .9f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageCreditReviewSpec, "WoT.Bots.ReviewFix.DamageDirectionIndependentOfVisibleCredit", Flags)
bool FDamageCreditReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* Credited = Enemy(W,FVector(800,0,50));
	Body->TakeRawDamage(1,TEXT("hurt"),Credited->GetController(),Body->GetActorLocation(),FVector(10000,0,0));
	FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("physical impulse controls turn even if credited source is visible elsewhere"),B->GetControlRotation().Vector().X < -.9f);
	TestNull(TEXT("impulse does not identify the credited source"),B->GetTarget());
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(0,10000,0));
	FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("second same-frame impulse supplies its own direction"),B->GetControlRotation().Vector().Y < -.9f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageContinuousSightReviewSpec, "WoT.Bots.ReviewFix.DamagePreservesAlreadyVisibleTarget", Flags)
bool FDamageContinuousSightReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* E = Enemy(W,FVector(800,0,50));
	FWoTBotPoolAccess::Perceive(B); WoTTest::TickWorld(W,.5f);
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(-10000,0,0));
	TestEqual(TEXT("received impact does not erase an independently acquired identity"),B->GetTarget(),E);
	TestTrue(TEXT("target still in view stays continuously visible during damage reaction"),B->IsTargetVisible());
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(10000,0,0));
	TestFalse(TEXT("a turn away from that target does lose visibility"),B->IsTargetVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageNoCueReviewSpec, "WoT.Bots.ReviewFix.NoImpulseCannotIdentifyCreditedAttacker", Flags)
bool FDamageNoCueReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .3f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* E = Enemy(W,FVector(800,0,50));
	Body->TakeRawDamage(1,TEXT("hurt"),E->GetController(),Body->GetActorLocation());
	TestNull(TEXT("nondirectional damage supplies no credited identity even in view"),B->GetTarget());
	FWoTBotPoolAccess::Perceive(B);
	TestNull(TEXT("normal sight must finish reaction"),B->GetTarget());
	WoTTest::TickWorld(W,.35f); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("normal sight can acquire after reaction"),B->GetTarget(),E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageMovementReviewSpec, "WoT.Bots.ReviewFix.DamageCueAfterMovementUpdate", Flags)
bool FDamageMovementReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,200)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* Move = Body->GetCharacterMovement();
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(10000,0,0));
	Move->SetMovementMode(MOVE_Flying);
	Move->BrakingDecelerationFlying = 2000;
	Move->TickComponent(.1f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("real movement changed velocity between same-frame hits"),Move->Velocity.X < 50);
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(0,10000,0));
	FWoTBotPoolAccess::Aim(B);
	TestTrue(TEXT("second hit uses the post-movement baseline"),B->GetControlRotation().Vector().Y < -.99f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageLateSightReviewSpec, "WoT.Bots.ReviewFix.SearchExpiryPreservesPendingReaction", Flags)
bool FDamageLateSightReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .3f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(10000,0,0));
	WoTTest::TickWorld(W,1.4f); auto* E = Enemy(W,FVector(-800,0,50));
	FWoTBotPoolAccess::Perceive(B);
	WoTTest::TickWorld(W,.15f); FWoTBotPoolAccess::Perceive(B);
	TestNull(TEXT("search-facing expiry does not bypass unfinished sight reaction"),B->GetTarget());
	WoTTest::TickWorld(W,.2f); FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("late sight can complete its reaction"),B->GetTarget(),E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageRepeatedReviewSpec, "WoT.Bots.ReviewFix.RepeatedHitsDoNotRestartContinuousSight", Flags)
bool FDamageRepeatedReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .3f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* Body = CastChecked<AWoTCharacter>(B->GetPawn());
	auto* E = Enemy(W,FVector(-800,0,50));
	for (int32 I=0; I<5; ++I)
	{
		Body->TakeRawDamage(1,TEXT("hurt"),nullptr,Body->GetActorLocation(),FVector(10000,0,0));
		FWoTBotPoolAccess::Perceive(B); WoTTest::TickWorld(W,.1f);
	}
	FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("repeated pain does not restart reaction to continuously visible enemy"),B->GetTarget(),E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolFreshReviewSpec, "WoT.Bots.ReviewFix.ExactContentsStayFresh", Flags)
bool FPoolFreshReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* P = KnowledgePool(W);
	P->Pool.Add(FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTAngrealInvLightning"))); P->Weight.Add(3);
	auto* Full = Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvFireball")); Full->CurCharges = Full->MaxCharges;
	FWoTBotPoolAccess::Observe(B); B->SetControlRotation(FRotator(0,180,0));
	for (int32 I=0; I<10; ++I)
	{
		WoTTest::TickWorld(W,.1f);
		TestNull(TEXT("recent unwanted exact contents do not regain prior utility between thinks"),FWoTBotPoolAccess::Best(B));
	}
	WoTTest::TickWorld(W,50.0f);
	TestEqual(TEXT("old exact contents eventually forget"),FWoTBotPoolAccess::Best(B),P);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolZeroGoalReviewSpec, "WoT.Bots.ReviewFix.ZeroGoalStillUsesSwitchMargin", Flags)
bool FPoolZeroGoalReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* P = KnowledgePool(W);
	auto* Full = Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvFireball")); Full->CurCharges = Full->MaxCharges;
	auto* Other = Pool(W,FVector(900,300,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Other->Pool.Add(FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTAngrealInvLightning"))); Other->Weight[0] = 10000; Other->Weight.Add(1);
	FWoTBotPoolAccess::Observe(B); FWoTBotPoolAccess::Goal(B,P);
	// Let only the challenger become uncertain, while the current exact value remains zero.
	B->SetControlRotation(FRotator(0,180,0)); WoTTest::TickWorld(W,50.0f);
	B->SetControlRotation(FRotator::ZeroRotator); Other->SetActorLocation(FVector(-900,300,30));
	Other->PickupItem->SetActorLocation(Other->GetActorLocation());
	FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("tiny positive prior cannot bypass current goal switch margin"),FWoTBotPoolAccess::Best(B),P);
	Other->Weight[0] = 0;
	TestEqual(TEXT("a clearly useful challenger can replace zero-value current goal"),FWoTBotPoolAccess::Best(B),Other);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVisibleDeathReviewSpec, "WoT.Bots.ReviewFix.WitnessedDeathClearsTarget", Flags)
bool FVisibleDeathReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(800,0,50));
	FWoTBotPoolAccess::Perceive(B); E->TakeRawDamage(100,TEXT("hurt"),B,E->GetActorLocation());
	FWoTBotPoolAccess::Perceive(B);
	TestNull(TEXT("witnessed ordinary kill clears corpse memory"),B->GetTarget());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoverySoundReviewSpec, "WoT.Bots.ReviewFix.RefillPreservesFailedRouteAvoidance", Flags)
bool FRecoverySoundReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* P = KnowledgePool(W);
	FWoTBotPoolAccess::Goal(B,P); FWoTBotPoolAccess::Recover(B);
	TestNull(TEXT("recovery avoids failed route"),FWoTBotPoolAccess::Best(B));
	B->SetControlRotation(FRotator(0,180,0)); EmptyPool(P); P->SpawnFromPool(true); FWoTBotPoolAccess::Observe(B);
	TestTrue(TEXT("refill sound was heard"),FWoTBotPoolAccess::Belief(B,P).Rarity != 0);
	TestNull(TEXT("audible refill cannot erase route recovery avoidance"),FWoTBotPoolAccess::Best(B));
	WoTTest::TickWorld(W,WoTBotDecision::RevisitBlockSeconds+.1f);
	TestEqual(TEXT("route can be reconsidered after recovery interval"),FWoTBotPoolAccess::Best(B),P);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoveryRotationReviewSpec, "WoT.Bots.ReviewFix.RefillPreservesRecoveryRotation", Flags)
bool FRecoveryRotationReviewSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* P = KnowledgePool(W);
	auto* Alternative = Pool(W,FVector(1600,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FWoTBotPoolAccess::Goal(B,P); FWoTBotPoolAccess::Recover(B);
	B->SetControlRotation(FRotator(0,180,0)); EmptyPool(P); P->SpawnFromPool(true); FWoTBotPoolAccess::Observe(B);
	WoTTest::TickWorld(W,WoTBotDecision::RevisitBlockSeconds+.1f); FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("after the hard block, failed route still yields to an untried alternative"),FWoTBotPoolAccess::Best(B),Alternative);
	WoTTest::TickWorld(W,WoTBotDecision::VisitMemory);
	TestEqual(TEXT("old route failure eventually loses its rotation penalty"),FWoTBotPoolAccess::Best(B),P);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectileMaskReviewSpec, "WoT.Bots.ReviewFix.HiddenProjectileCannotMaskVisibleThreat", Flags)
bool FProjectileMaskReviewSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(-800,0,50));
	auto* Shield = Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvShield"));
	auto* Source = Give(E,TEXT("/Script/WoT.WoTAngrealInvDart"));
	FActorSpawnParameters Params; Params.Owner = E; Params.Instigator = E;
	for (const float X : {-60.0f,120.0f})
	{
		auto* P = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTDartProjectile")),FVector(X,0,60),FRotator::ZeroRotator,Params);
		P->SourceAngreal = Source; P->ProjectileMovement->Velocity = FVector(X < 0 ? 100 : -100,0,0);
	}
	FWoTBotPoolAccess::Cast(B);
	TestEqual(TEXT("visible incoming projectile triggers defence despite closer hidden one"),Shield->CastUseCount,1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolUnseenSpec, "WoT.Bots.PoolKnowledge.UnseenContentsDoNotChangeChoice", Flags)
bool FPoolUnseenSpec::RunTest(const FString&)
{
	FSettings Settings;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W, FVector(0,0,50)); B->SetControlRotation(FRotator(0,180,0));
	auto* P = KnowledgePool(W);
	float Before, After;
	TestEqual(TEXT("map prior chooses the unseen pool"), FWoTBotPoolAccess::Best(B, &Before), P);
	EmptyPool(P);
	TestEqual(TEXT("only unseen stock changed; choice is identical"), FWoTBotPoolAccess::Best(B, &After), P);
	TestEqual(TEXT("unseen stock cannot affect score"), After, Before);
	B->SetControlRotation(FRotator::ZeroRotator);
	Settings.Value->SightRange = 100;
	FWoTBotPoolAccess::Observe(B);
	WoTTest::TickWorld(W, Settings.Value->ReactionSeconds+.1f);
	FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("out-of-range stock stays unknown"), FWoTBotPoolAccess::Best(B), P);
	Settings.Value->SightRange = Settings.Sight;
	auto* Wall = Box(W,FVector(350,0,100),FVector(30,300,150),false);
	FWoTBotPoolAccess::Observe(B);
	WoTTest::TickWorld(W, Settings.Value->ReactionSeconds+.1f);
	FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("occluded stock stays unknown"), FWoTBotPoolAccess::Best(B), P);
	Wall->Destroy();
	FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("first view waits for reaction"), FWoTBotPoolAccess::Best(B), P);
	WoTTest::TickWorld(W, GetDefault<UWoTBotSettings>()->ReactionSeconds+.1f);
	FWoTBotPoolAccess::Observe(B);
	TestNull(TEXT("reacted sight reveals empty"), FWoTBotPoolAccess::Best(B));
	auto* Mixed = Pool(W,FVector(1000,300,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Mixed->Pool.Add(FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTAngrealInvLightning")));
	Mixed->Weight.Add(3);
	auto* Full = Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Full->CurCharges = Full->MaxCharges;
	FWoTBotPoolAccess::Observe(B);
	WoTTest::TickWorld(W, Settings.Value->ReactionSeconds+.1f);
	FWoTBotPoolAccess::Observe(B);
	TestNull(TEXT("recent exact contents already full have no collection value"), FWoTBotPoolAccess::Best(B));
	B->SetControlRotation(FRotator(0,180,0));
	WoTTest::TickWorld(W,45.1f);
	TestEqual(TEXT("forgotten contents regain the weighted prior's missing-class value"),FWoTBotPoolAccess::Best(B),Mixed);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FPoolTimingSpec, "WoT.Bots.PoolKnowledge.NoisyReturn", Flags)
void FPoolTimingSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{ for (const TCHAR* S : { TEXT("FoundEmpty"), TEXT("SawTake") }) { Names.Add(S); Commands.Add(S); } }
bool FPoolTimingSpec::RunTest(const FString& Mode)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	SeedDecisions(101);
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* P = KnowledgePool(W);
	auto* A = QuietBot(W, FVector(-1800,-80,50)); auto* B = QuietBot(W, FVector(-1800,80,50));
	if (Mode == TEXT("SawTake"))
	{
		P->bRespawn = true;
		auto* Collector = Enemy(W,FVector(-2300,1500,50));
		FWoTBotPoolAccess::Observe(A); FWoTBotPoolAccess::Observe(B);
		Collector->SetActorLocation(P->GetActorLocation()+FVector(0,0,40));
		TestNull(TEXT("another body really took the pickup"), P->PickupItem.Get());
		Collector->SetActorLocation(FVector(-2300,1500,50));
	}
	else EmptyPool(P);
	FWoTBotPoolAccess::Observe(A); FWoTBotPoolAccess::Observe(B);
	A->SetControlRotation(FRotator(0,180,0)); B->SetControlRotation(FRotator(0,180,0));
	const float Delay = AWoTAngrealSpawnPool::ComputeRespawnDelay(P->RespawnTime, 0, W->GetAuthGameMode<AWoTGameMode>()->GetNumCombatants());
	// A silent early refill must not override an observer's empty memory.
	if (Mode == TEXT("FoundEmpty")) P->SpawnFromPool(false);
	float FirstA = -1, FirstB = -1;
	for (float Elapsed = 0; Elapsed < Delay * 1.3f; Elapsed += .1f)
	{
		if (FirstA < 0 && FWoTBotPoolAccess::Best(A)) FirstA = Elapsed;
		if (FirstB < 0 && FWoTBotPoolAccess::Best(B)) FirstB = Elapsed;
		WoTTest::TickWorld(W, .1f);
	}
	TestTrue(TEXT("first observer waits for an approximate refill"), FirstA >= Delay*.8f && FirstA <= Delay*1.2f+.2f);
	TestTrue(TEXT("second observer waits inside noise band"), FirstB >= Delay*.8f && FirstB <= Delay*1.2f+.2f);
	TestTrue(TEXT("observers have distinct timing estimates"), FMath::Abs(FirstA-FirstB) > .05f);
	TestTrue(TEXT("return is not pinned to the real timer"), FMath::Abs(FirstA-Delay) > .05f);
	if (Mode == TEXT("SawTake")) TestNotNull(TEXT("real pool timer refilled while observers were outside hearing range"), P->PickupItem.Get());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolHearingSpec, "WoT.Bots.PoolKnowledge.HearsOnlyInsideRealSoundRange", Flags)
bool FPoolHearingSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* P = KnowledgePool(W); EmptyPool(P);
	P->SpawnFromPool(true);
	float HearingRadiusSquared = 0;
	UClass* SoundClass = FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTSoundBurst"));
	for (TActorIterator<AActor> It(W); It; ++It)
		if (It->IsA(SoundClass) && static_cast<AWoTSoundBurst*>(*It)->Source == P)
			HearingRadiusSquared = It->GetNetCullDistanceSquared();
	if (!TestTrue(TEXT("real emitted sound supplies its hearing range"), HearingRadiusSquared > 0)) return false;
	EmptyPool(P);
	auto* A = QuietBot(W, FVector(0,-80,50)); auto* B = QuietBot(W, FVector(0,80,50));
	FWoTBotPoolAccess::Observe(A); FWoTBotPoolAccess::Observe(B);
	A->SetControlRotation(FRotator(0,180,0)); B->SetControlRotation(FRotator(0,180,0));
	const double Height = A->GetPawn()->GetActorLocation().Z - P->GetActorLocation().Z;
	const double HorizontalRange = FMath::Sqrt(HearingRadiusSquared - Height*Height);
	A->GetPawn()->SetActorLocation(FVector(P->GetActorLocation().X-HorizontalRange+1,0,50));
	B->GetPawn()->SetActorLocation(FVector(P->GetActorLocation().X-HorizontalRange-1,0,50));
	// Both know empty; only A is inside the sound event's actual hearing range.
	P->SpawnFromPool(true);
	WoTTest::TickWorld(W, .1f);
	FWoTBotPoolAccess::Observe(A); FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("near observer learns refill from sound"), FWoTBotPoolAccess::Best(A), P);
	TestNull(TEXT("distant observer retains empty belief"), FWoTBotPoolAccess::Best(B));
	TestFalse(TEXT("hearing never reveals an exact class"), FWoTBotPoolAccess::Belief(A,P).Contents.IsValid());
	TestEqual(TEXT("sound reveals only common tier"), FWoTBotPoolAccess::Belief(A,P).Rarity, uint8(1));
	EmptyPool(P); P->bRespawn = true;
	A->GetPawn()->SetActorLocation(P->GetActorLocation()+FVector(0,0,40));
	P->SpawnFromPool(true);
	TestNull(TEXT("standing collector takes real refill"), P->PickupItem.Get());
	FWoTBotPoolAccess::Observe(A);
	TestTrue(TEXT("newer own pickup wins over same-frame sound"), FWoTBotPoolAccess::Belief(A,P).bEmpty);
	TestTrue(TEXT("own pickup creates witnessed-take estimate"), FWoTBotPoolAccess::Belief(A,P).EstimatedRestock > W->GetTimeSeconds());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolGoalSpec, "WoT.Bots.PoolKnowledge.HiddenEmptyGoalIsPursued", Flags)
bool FPoolGoalSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W, FVector(0,0,50)); B->SetControlRotation(FRotator(0,180,0));
	auto* P = KnowledgePool(W); FWoTBotPoolAccess::Goal(B,P); EmptyPool(P);
	FWoTBotPoolAccess::Observe(B);
	TestEqual(TEXT("out-of-sight empty goal is retained"), B->GetSeekingPool(), P);
	B->GetPawn()->SetActorLocation(P->GetActorLocation()+FVector(0,0,40));
	FWoTBotPoolAccess::Observe(B);
	TestNull(TEXT("arrival reveals empty and releases goal"), B->GetSeekingPool());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolClaimsFairSpec, "WoT.Bots.FairPlay.PrivatePoolClaimsCannotAffectChoice", Flags)
bool FPoolClaimsFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W); auto* B = QuietBot(W,FVector(0,0,50)); auto* Other = QuietBot(W,FVector(-1800,500,50));
	auto* P = KnowledgePool(W); Pool(W,FVector(-1000,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
	auto* Before = FWoTBotPoolAccess::Best(B);
	W->GetSubsystem<UWoTBotRoster>()->ClaimPool(Other,P);
	TestEqual(TEXT("private intent cannot alter another player's choice"), FWoTBotPoolAccess::Best(B), Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFightIntentFairSpec, "WoT.Bots.FairPlay.PrivateFightIntentCannotAffectChoice", Flags)
bool FFightIntentFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* A = QuietBot(W,FVector(-1000,500,50)); auto* C = QuietBot(W,FVector(-1000,-500,50));
	auto* E = Enemy(W,FVector(800,0,50)); Enemy(W,FVector(1200,200,50),2);
	FWoTBotPoolAccess::Perceive(B); auto* Before = B->GetTarget();
	FWoTBotPoolAccess::Engage(A,E); FWoTBotPoolAccess::Engage(C,E);
	FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("hidden engagement states cannot change target"), B->GetTarget(), Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageSourceFairSpec, "WoT.Bots.FairPlay.HiddenDamageSourceDoesNotRevealPosition", Flags)
bool FDamageSourceFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(-800,0,50));
	const FVector Before = B->GetLastKnownPosition();
	FDamageEvent D; CastChecked<AWoTCharacter>(B->GetPawn())->TakeDamage(1,D,E->GetController(),E);
	TestNull(TEXT("damage metadata cannot identify hidden attacker"), B->GetTarget());
	TestTrue(TEXT("hidden source location is not revealed"), B->GetLastKnownPosition().Equals(Before));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiddenDeathFairSpec, "WoT.Bots.FairPlay.HiddenDeathDoesNotEraseMemory", Flags)
bool FHiddenDeathFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(800,0,50));
	FWoTBotPoolAccess::Perceive(B); B->SetControlRotation(FRotator(0,180,0)); E->Health = 0;
	FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("unobserved death retains last sighting"), B->GetTarget(), E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExactHealthFairSpec, "WoT.Bots.FairPlay.OpponentHealthIsNotTelemetry", Flags)
bool FExactHealthFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(800,0,50));
	FWoTBotPoolAccess::Perceive(B); const float Before = FWoTBotPoolAccess::Fight(B);
	E->Health = 1; FWoTBotPoolAccess::Perceive(B);
	TestEqual(TEXT("exact unseen health value is not readable from appearance"), FWoTBotPoolAccess::Fight(B), Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiddenHitFairSpec, "WoT.Bots.FairPlay.HiddenHitDoesNotRefreshPursuit", Flags)
bool FHiddenHitFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(800,0,50));
	FWoTBotPoolAccess::Engage(B,E); B->SetControlRotation(FRotator(0,180,0));
	FWoTBotPoolAccess::StaleProgress(B); const float Before = FWoTBotPoolAccess::Progress(B);
	FDamageEvent D; E->TakeDamage(1,D,B,B->GetPawn());
	TestEqual(TEXT("hidden hit callback does not report combat progress"), FWoTBotPoolAccess::Progress(B), Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiddenProjectileFairSpec, "WoT.Bots.FairPlay.HiddenProjectileDoesNotTriggerDefence", Flags)
bool FHiddenProjectileFairSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* B = QuietBot(W,FVector(0,0,50)); auto* E = Enemy(W,FVector(-800,0,50));
	auto* Shield = Give(CastChecked<AWoTCharacter>(B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvShield"));
	auto* Source = Give(E,TEXT("/Script/WoT.WoTAngrealInvDart"));
	FActorSpawnParameters Params; Params.Owner = E; Params.Instigator = E;
	auto* Incoming = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,
		TEXT("/Script/WoT.WoTDartProjectile")),FVector(-100,0,60),FRotator::ZeroRotator,Params);
	Incoming->SourceAngreal = Source; Incoming->ProjectileMovement->Velocity = FVector(100,0,0);
	FWoTBotPoolAccess::Cast(B);
	TestEqual(TEXT("unseen projectile does not spend defence"), Shield->CastUseCount, 0);
	return !HasAnyErrors();
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoolMapDelaySpec, "WoT.Bots.PoolKnowledge.MaterializedMapRespawnDelay", Flags)
bool FPoolMapDelaySpec::RunTest(const FString&)
{
	FMapSet Maps; FString Error;
	if (!Maps.Load(Error)) { AddError(Error); return false; }
	const auto* Map = Maps.Descriptors.FindByPredicate([](const auto& D) { return D.MapIdentity == TEXT("Arena_01"); });
	if (!TestNotNull(TEXT("real Arena descriptor"),Map)) return false;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UWoTMapLightTable* Table = nullptr;
	if (!Materialize(W,Maps,*Map,Error,&Table)) { AddError(Error); return false; }
	if (!TestNotNull(TEXT("projector's actual registered map metadata"),Table)) return false;
	TestTrue(TEXT("map has nontrivial ideal population"),Table->IdealPlayerCount > 1);
	AWoTBotController* B = nullptr;
	for (int32 I=0; I<8; ++I)
	{
		B = W->SpawnActor<AWoTBotController>();
		B->InitializeIdentity(nullptr,W->GetAuthGameMode()->DefaultPawnClass,TEXT("Timing observer"));
	}
	AWoTAngrealSpawnPool* P = nullptr;
	for (TActorIterator<AWoTAngrealSpawnPool> It(W); It; ++It) { P = *It; break; }
	if (!TestNotNull(TEXT("real projected pool"),P)) return false;
	P->RespawnTime = 30;
	const float Expected = AWoTAngrealSpawnPool::ComputeRespawnDelay(P->RespawnTime,Table->IdealPlayerCount,
		W->GetAuthGameMode<AWoTGameMode>()->GetNumCombatants());
	TestEqual(TEXT("belief delay uses the same map population rule as the real pool"),FWoTBotPoolAccess::Delay(B,P),Expected);
	return !HasAnyErrors();
}
#endif
}
#endif
