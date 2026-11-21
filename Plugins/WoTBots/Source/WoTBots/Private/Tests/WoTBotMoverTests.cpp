#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTBotNavProbe.h"
#include "WoTBotRoster.h"
#include "WoTMover.h"
#include "WoTEvents.h"
#include "AI/WoTPathNode.h"
#include "Testing/WoTSoakEvents.h"
#include "WoTBotMoverLink.h"
#include "AI/WoTAIUtil.h"
#include "AI/WoTPathNodeIterator.h"
#include "AI/WoTSight.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "HAL/PlatformProcess.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"

namespace WoTBotTest
{
struct FWoTBotMoverAccess
{
	// Fix the initial intention, not the movement. Controller Tick, body movement,
	// stuck recovery, climb and all normal mover/trigger collisions remain live.
	static void Prepare(AWoTBotController* B)
	{
		B->GetWorldTimerManager().ClearTimer(B->ThinkTimer);
		B->State = EWoTBotState::Roam;
		B->LastProgressTime = B->LastRecoveryTime = B->GetWorld()->GetTimeSeconds();
		B->ProgressPosition = B->LastBodyLocation = B->GetPawn()->GetActorLocation();
	}
	static bool Seek(AWoTBotController* B, AWoTAngrealSpawnPool* Pool)
	{
		Prepare(B);
		B->State = EWoTBotState::SeekItem;
		B->SeekingPool = Pool;
		return B->NavigateTo(Pool->GetActorLocation());
	}
	static bool Walk(AWoTBotController* B, const FVector& Goal)
	{
		Prepare(B);
		return B->NavigateTo(Goal);
	}
	// The controller's own think timer, as StartThinking schedules it in a match.
	static void Think(AWoTBotController* B)
	{
		if (!B->GetWorldTimerManager().IsTimerActive(B->ThinkTimer)) B->StartThinking();
	}
	static AWoTBotMoverLink* Active(const AWoTBotController* B) { return B->ActiveMoverLink.Get(); }
	// Read-only observation of the handler's stage for this bot; -1 when none.
	static int32 Stage(AWoTBotController* B)
	{
		const AWoTBotMoverLink* Link = Active(B);
		const AWoTBotMoverLink::FTraversal* T = Link ? Link->Traversals.Find(B) : nullptr;
		return T ? int32(T->Stage) : -1;
	}
	static constexpr int32 Wait = int32(AWoTBotMoverLink::EStage::Wait);
	static constexpr int32 Ride = int32(AWoTBotMoverLink::EStage::Ride);
	static constexpr int32 Exit = int32(AWoTBotMoverLink::EStage::Exit);
	static constexpr int32 Contended = int32(AWoTBotMoverLink::EStage::Contended);
};
}

namespace WoTBotMoverTest
{
using namespace WoTBotTest;
constexpr float Step = 1.0f / 60.0f;

struct FFixture
{
	UWorld* W = nullptr;
	UNavigationSystemV1* Nav = nullptr;
	const ANavigationData* Data = nullptr;
	~FFixture() { WoTTest::DestroySpecWorld(W); }
	void Tick(float Seconds = Step) { WoTTest::TickWorld(W, Seconds, Step); }
	bool Flush()
	{
		// Tick publishes the callbacks that EnsureBuildCompletion bypasses.
		Nav->Tick(Step);
		const double Deadline = FPlatformTime::Seconds() + 10;
		while ((Nav->GetNumRemainingBuildTasks() || Nav->HasDirtyAreasQueued()) && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(.001f);
			++GFrameCounter;
			Nav->Tick(Step);
		}
		return !Nav->GetNumRemainingBuildTasks() && !Nav->HasDirtyAreasQueued();
	}
	bool Load(FAutomationTestBase& Test, const TCHAR* Map)
	{
		SeedDecisions(9417);
		FMapSet Maps; FString Error;
		if (!Maps.Load(Error)) { Test.AddError(Error); return false; }
		const auto* Descriptor = Maps.Descriptors.FindByPredicate([Map](const auto& D) { return D.MapIdentity == Map; });
		if (!Test.TestNotNull(TEXT("real map descriptor"), Descriptor)) return false;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false; Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		W = WoTTest::CreateSpecWorld(Options);
		if (!Materialize(W, Maps, *Descriptor, Error)) { Test.AddError(Error); return false; }
		UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
		Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
		W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
		FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
		Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
		if (!Test.TestNotNull(TEXT("real map navigation"), Nav)) return false;
		Nav->Build();
		for (ANavigationData* Mesh : Nav->NavDataSet) if (Mesh) Mesh->EnsureBuildCompletion();
		Data = Nav->GetNavDataForAgentName(TEXT("ArenaBot"));
		if (!Test.TestNotNull(TEXT("ArenaBot mesh"), Data)) return false;
		W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?BotFill=0?FragLimit=0?TimeLimit=0"), TRAVEL_Absolute);
		if (!Test.TestTrue(TEXT("real Arena rules"), W->SetGameMode(URL))) return false;
		W->InitializeActorsForPlay(URL); W->BeginPlay();
		Tick(12);
		const int32 Links = W->GetSubsystem<UWoTBotRoster>()->BuildLinks();
		Test.AddInfo(FString::Printf(TEXT("MOVER_FIXTURE map=%s links=%d"), Map, Links));
		return Test.TestTrue(TEXT("fixture navigation settles"), Flush());
	}
	bool Project(const FVector& P, FVector& Feet) const
	{
		FNavLocation Result;
		if (!Nav->ProjectPointToNavigation(P, Result, FVector(60, 60, 150), Data)) return false;
		Feet = Result.Location; return true;
	}
	bool Complete(const FVector& From, const FVector& To) const
	{
		FVector A, B;
		if (!Project(From, A) || !Project(To, B)) return false;
		FPathFindingQuery Query(nullptr, *Data, A, B);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Result = Nav->FindPathSync(Data->GetConfig(), Query);
		return Result.IsSuccessful() && Result.Path.IsValid() && !Result.Path->IsPartial();
	}
};

struct FObservedBot
{
	AWoTBotController* Controller;
	TWeakObjectPtr<AWoTCharacter> Pawn;
	FVector Start;
	int32 Respawns = 0, Recoveries = 0, Collections = 0;
	bool bIdentityChanged = false, bBased = false, bRisen = false, bExited = false;
	FDelegateHandle Handle;
	FObservedBot(AWoTBotController* B, AWoTAngrealSpawnPool* Goal) : Controller(B),
		Pawn(CastChecked<AWoTCharacter>(B->GetPawn())), Start(B->GetPawn()->GetActorLocation())
	{
		Handle = WoTSoakEvents::Events().AddLambda([this, Goal](WoTSoakEvents::EKind Kind, AActor* Subject, AActor* Participant)
		{
			if (Subject == Controller && Kind == WoTSoakEvents::EKind::SilentRespawn) ++Respawns;
			if (Subject == Controller && Kind == WoTSoakEvents::EKind::Recovery) ++Recoveries;
			if (Subject == Goal && Participant == Pawn.Get() && Kind == WoTSoakEvents::EKind::PoolCollect) ++Collections;
		});
	}
	~FObservedBot() { WoTSoakEvents::Events().Remove(Handle); }
	void Sample(AWoTMover* Lift = nullptr, double Top = 0)
	{
		bIdentityChanged |= !Pawn.IsValid() || Pawn.Get() != Controller->GetPawn();
		if (bIdentityChanged || !Lift) return;
		const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Pawn->GetMovementBaseObject());
		const bool bOnLift = Base && Base->GetOwner() == Lift;
		const double FeetZ = Pawn->GetActorLocation().Z - Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		bBased |= bOnLift;
		bRisen |= bOnLift && FeetZ >= Top - 35;
		bExited |= bRisen && !bOnLift && Pawn->GetCharacterMovement()->IsMovingOnGround() && FeetZ >= Top - 35;
	}
	void AssertIdentity(FAutomationTestBase& Test, const TCHAR* Label)
	{
		Sample();
		Test.TestFalse(*FString::Printf(TEXT("%s keeps the original pawn throughout"), Label), bIdentityChanged);
		Test.TestEqual(*FString::Printf(TEXT("%s has no silent respawn"), Label), Respawns, 0);
		Test.TestTrue(*FString::Printf(TEXT("%s survives the traversal"), Label), Pawn.IsValid() && Pawn->IsAlive());
		Test.AddInfo(FString::Printf(TEXT("MOVER_BOT label=%s pawn=%s position=%s destination=%s based=%d risen=%d exited=%d collections=%d recoveries=%d respawns=%d repaths=%llu"),
			Label, *GetNameSafe(Pawn.Get()), Pawn.IsValid() ? *Pawn->GetActorLocation().ToString() : TEXT("missing"),
			*Controller->GetDestination().ToString(), bBased, bRisen, bExited, Collections, Recoveries, Respawns, Controller->GetRepathCount()));
	}
};

bool DeckPoint(AWoTMover* Lift, FVector& Deck)
{
	// Trace the actual deck away from its central post, as the player-body fixture does.
	const FBox Bounds = Lift->GetComponentsBoundingBox(true);
	const FVector Center = Bounds.GetCenter();
	const double X = Center.X - Bounds.GetExtent().X * 2.0 / 3.0;
	FHitResult Hit;
	if (!Lift->Mesh->LineTraceComponent(Hit, FVector(X, Center.Y, Bounds.Max.Z + 10),
		FVector(X, Center.Y, Bounds.Min.Z - 10), FCollisionQueryParams(SCENE_QUERY_STAT(BotLiftDeck), true))) return false;
	Deck = Hit.ImpactPoint; return Hit.ImpactNormal.Z > .7;
}

struct FLiftLayout
{
	AWoTMover* Lift = nullptr;
	AWoTAngrealSpawnPool* Pool = nullptr;
	FVector Deck, Start;
	double Top = 0;
	bool Find(FFixture& F, FAutomationTestBase& Test, bool bNorth)
	{
		TArray<AWoTMover*> Lifts;
		for (TActorIterator<AWoTMover> It(F.W); It; ++It) Lifts.Add(*It);
		if (!Test.TestEqual(TEXT("both authored Arena_04 movers are present"), Lifts.Num(), 2)) return false;
		Lifts.Sort([](const AWoTMover& A, const AWoTMover& B) { return A.GetActorLocation().Y < B.GetActorLocation().Y; });
		Lift = Lifts[bNorth ? 1 : 0];
		if (!Test.TestTrue(TEXT("real returning bump lift"), Lift->InitialState == EWoTMoverState::BumpOpenTimed
			&& Lift->KeyPos.Num() == 2 && Lift->KeyPos[1].Z > 200 && Lift->KeyPos[1].Size2D() < 1)) return false;
		if (!Test.TestTrue(TEXT("actual deck collision away from the post"), DeckPoint(Lift, Deck))) return false;
		Top = Deck.Z + Lift->KeyPos[1].Z;
		double BestStart = DBL_MAX, BestPool = DBL_MAX;
		const FBox Box = Lift->GetComponentsBoundingBox(true).ExpandBy(45);
		for (TActorIterator<APlayerStart> It(F.W); It; ++It)
		{
			FVector Feet;
			if (!F.Project(It->GetActorLocation(), Feet) || FMath::Abs(Feet.Z - Deck.Z) > 40) continue;
			if (Feet.X >= Box.Min.X && Feet.X <= Box.Max.X && Feet.Y >= Box.Min.Y && Feet.Y <= Box.Max.Y) continue;
			const double Distance = FVector::DistSquared2D(Feet, Deck);
			if (Distance < BestStart) { BestStart = Distance; Start = Feet; }
		}
		for (TActorIterator<AWoTAngrealSpawnPool> It(F.W); It; ++It)
		{
			FVector Feet;
			if (!It->PickupItem || !F.Project(It->GetActorLocation(), Feet) || FMath::Abs(Feet.Z - Top) > 50) continue;
			const double Distance = FVector::DistSquared2D(Feet, Deck);
			if (Distance < BestPool) { BestPool = Distance; Pool = *It; }
		}
		Test.AddInfo(FString::Printf(TEXT("LIFT_LAYOUT side=%s actor=%s bounds=%s deck=%s start=%s top=%.1f pool=%s pool_position=%s"),
			bNorth ? TEXT("north") : TEXT("south"), *Lift->GetName(), *Lift->GetComponentsBoundingBox(true).ToString(),
			*Deck.ToString(), *Start.ToString(), Top, *GetNameSafe(Pool), Pool ? *Pool->GetActorLocation().ToString() : TEXT("missing")));
		return Test.TestTrue(TEXT("lower stationary authored spawn near this lift"), BestStart < FMath::Square(1000.0))
			&& Test.TestNotNull(TEXT("stocked middle-floor pool near this lift"), Pool);
	}
};

AWoTBotController* SpawnOnFloor(FFixture& F, const FVector& Feet)
{
	const float Half = GetDefault<AWoTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	AWoTBotController* B = Bot(F.W, Feet + FVector(0, 0, Half + 3));
	FWoTBotMoverAccess::Prepare(B);
	return B;
}

bool SendAway(FFixture& F, FAutomationTestBase& Test, const FLiftLayout& L)
{
	// Setup only: a real player's falling body operates the deck through collision.
	// Never call NotifyHit, trigger events, set mover phase or move the tested bot.
	// A headless player controller is not locally driven and can wait for
	// network movement forever. Use the player-body spec's ordinary AI driver.
	AWoTCharacter* Caller = Body(F.W, L.Deck + FVector(0, 0, GetDefault<AWoTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 24));
	AAIController* CallerController = F.W->SpawnActor<AAIController>();
	CallerController->InitPlayerState();
	CallerController->Possess(Caller);
	Caller->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	for (int32 I = 0; I < 180 && L.Lift->GetActorLocation().Z < L.Lift->GetBasePosition().Z + 120; ++I) F.Tick();
	const bool bStarted = L.Lift->GetActorLocation().Z > L.Lift->GetBasePosition().Z + 100;
	const UPrimitiveComponent* CallerBase = Cast<UPrimitiveComponent>(Caller->GetMovementBaseObject());
	Test.AddInfo(FString::Printf(TEXT("LIFT_AWAY_SETUP caller=%s position=%s movement=%s local=%d alive=%d base=%s phase=%d state=%d key=%d mover=%s rest=%s"),
		*Caller->GetName(), *Caller->GetActorLocation().ToString(), *Caller->GetCharacterMovement()->GetMovementName(), Caller->IsLocallyControlled(), Caller->IsAlive(),
		*GetNameSafe(CallerBase ? CallerBase->GetOwner() : nullptr), int32(L.Lift->GetMoverPhase()), int32(L.Lift->InitialState), L.Lift->GetCurrentKeyNum(),
		*L.Lift->GetActorLocation().ToString(), *L.Lift->GetBasePosition().ToString()));
	Caller->Destroy(); if (CallerController) CallerController->Destroy();
	return Test.TestTrue(TEXT("real body operated the lift, which is away when the bot starts"), bStarted);
}

bool LiftCase(FAutomationTestBase& Test, const FString& Parameters)
{
	FFixture F; if (!F.Load(Test, TEXT("Arena_04"))) return false;
	FLiftLayout L; if (!L.Find(F, Test, Parameters.StartsWith(TEXT("North")))) return false;
	const bool bAway = Parameters.Contains(TEXT("Away")), bConcurrent = Parameters.Contains(TEXT("Concurrent"));
	if (bAway && !SendAway(F, Test, L)) return false;
	AWoTBotController* A = SpawnOnFloor(F, L.Start);
	FObservedBot First(A, L.Pool);
	AWoTBotController* B = nullptr;
	TUniquePtr<FObservedBot> Second;
	if (bConcurrent)
	{
		// Another nearby projected point on the same stationary floor, separated by capsules.
		FVector SecondFeet;
		const FVector Side = FVector::CrossProduct((L.Deck - L.Start).GetSafeNormal2D(), FVector::UpVector);
		if (!Test.TestTrue(TEXT("second contender has a real lower-floor start"), F.Project(L.Start + Side * 90, SecondFeet)
			&& FMath::Abs(SecondFeet.Z - L.Start.Z) < 20 && F.Complete(L.Start, SecondFeet))) return false;
		B = SpawnOnFloor(F, SecondFeet); Second = MakeUnique<FObservedBot>(B, L.Pool);
	}
	const bool bAccepted = FWoTBotMoverAccess::Seek(A, L.Pool);
	const bool bSecondAccepted = B && FWoTBotMoverAccess::Seek(B, L.Pool);
	Test.AddInfo(FString::Printf(TEXT("LIFT_REQUEST first=%d second=%d away=%d"), bAccepted, bSecondAccepted, bAway));
	bool bUnsafeWait = false, bWaited = !bAway;
	FVector ReplanOrigin[2] = {FVector::ZeroVector, FVector::ZeroVector};
	bool bReplanned[2] = {false, false}, bReplanProgress[2] = {false, false};
	for (int32 I = 0; I < 60 * 35; ++I)
	{
		F.Tick(); First.Sample(L.Lift, L.Top); if (Second) Second->Sample(L.Lift, L.Top);
		if (First.bIdentityChanged || (Second && Second->bIdentityChanged)) break;
		if (bAway && !First.bBased && L.Lift->GetActorLocation().Z > L.Lift->GetBasePosition().Z + 60)
		{
			const FVector P = First.Pawn->GetActorLocation();
			const FBox DeckBounds = L.Lift->GetComponentsBoundingBox(true);
			bUnsafeWait |= P.X > DeckBounds.Min.X && P.X < DeckBounds.Max.X && P.Y > DeckBounds.Min.Y && P.Y < DeckBounds.Max.Y;
			bWaited |= FVector::Dist2D(P, L.Start) < 100;
		}
		if (Second)
		{
			FObservedBot* Contenders[] = {&First, Second.Get()};
			for (int32 Rider = 0; Rider < 2; ++Rider)
			{
				FObservedBot& O = *Contenders[Rider];
				if (!bReplanned[Rider] && O.Controller->GetSeekingPool() != L.Pool)
				{ bReplanned[Rider] = true; ReplanOrigin[Rider] = O.Pawn->GetActorLocation(); }
				if (bReplanned[Rider]) bReplanProgress[Rider] |= FVector::Dist2D(O.Pawn->GetActorLocation(), ReplanOrigin[Rider]) > 100;
			}
			if ((First.bExited && (Second->bExited || bReplanProgress[1])) || (Second->bExited && bReplanProgress[0])) break;
		}
		else if (First.bExited && First.Collections > 0) break;
	}
	First.AssertIdentity(Test, TEXT("first")); if (Second) Second->AssertIdentity(Test, TEXT("second"));
	if (!Second)
	{
		Test.TestTrue(TEXT("bot acquires this lift as its actual movement base"), First.bBased);
		Test.TestTrue(TEXT("bot rides this lift to the upper stop and steps onto the middle landing"), First.bRisen && First.bExited);
		Test.TestTrue(TEXT("same bot collects the actual middle-floor pool"), First.Collections > 0);
	}
	else
	{
		Test.TestTrue(TEXT("at least one contender completes this lift ride"), First.bExited || Second->bExited);
		Test.TestTrue(TEXT("both get up or the other abandons the contested goal and makes progress"),
			(First.bExited && Second->bExited) || (First.bExited && bReplanProgress[1]) || (Second->bExited && bReplanProgress[0]));
	}
	if (bAway)
	{
		Test.TestTrue(TEXT("bot waits on the lower stationary floor while the lift is away"), bWaited);
		Test.TestFalse(TEXT("waiting bot never stands underneath the away deck"), bUnsafeWait);
	}
	return !Test.HasAnyErrors();
}

AWoTBotMoverLink* LinkFor(FFixture& F, const AWoTMover* Lift)
{
	for (TActorIterator<AWoTBotMoverLink> It(F.W); It; ++It) if (It->GetMover() == Lift) return *It;
	return nullptr;
}

// An idle person: an AI-driven player body on real floor, so its movement runs
// and its capsule blocks like anyone's. Team 0 is the tested bots' side.
AWoTCharacter* Stander(FFixture& F, const FVector& Feet, uint8 Team)
{
	AWoTCharacter* Pawn = Body(F.W, Feet + FVector(0, 0, GetDefault<AWoTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3));
	AAIController* Controller = F.W->SpawnActor<AAIController>();
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>()) State->Team = Team;
	Controller->Possess(Pawn);
	return Pawn;
}

void RemovePerson(AWoTCharacter* Pawn)
{
	if (!IsValid(Pawn)) return;
	AController* Controller = Pawn->GetController();
	Pawn->Destroy(); if (IsValid(Controller)) Controller->Destroy();
}

bool OnLift(const AWoTCharacter* Pawn, const AWoTMover* Lift)
{
	const UPrimitiveComponent* Base = Pawn ? Cast<UPrimitiveComponent>(Pawn->GetMovementBaseObject()) : nullptr;
	return Base && Base->GetOwner() == Lift;
}

// Whether the route this querier would be given runs through Link. The planner's
// own link filter sees the querier, exactly as for a controller's move request.
bool PlanUses(FFixture& F, const AWoTBotMoverLink* Link, const UObject* Querier, const FVector& From, const FVector& To)
{
	FVector A, B;
	if (!F.Project(From, A) || !F.Project(To, B)) return false;
	FPathFindingQuery Query(Querier, *F.Data, A, B);
	Query.SetAllowPartialPaths(false);
	const FPathFindingResult Result = F.Nav->FindPathSync(F.Data->GetConfig(), Query);
	if (!Result.IsSuccessful() || !Result.Path.IsValid() || Result.Path->IsPartial()) return false;
	for (const FNavPathPoint& Point : Result.Path->GetPathPoints())
		if (Point.CustomNavLinkId == Link->GetSmartLinkComp()->GetId()) return true;
	return false;
}

// Review fixes for the lift handler. The approach to the link stays pinned (as in
// LiftCase); from the moment the handler owns a bot, that bot's normal Think()
// timer runs with its Tick, as in a match. Everyone else is a real body.
bool LiftHandlerCase(FAutomationTestBase& Test, const FString& Parameters)
{
	FString Side, Mode;
	Parameters.Split(TEXT("."), &Side, &Mode);
	FFixture F; if (!F.Load(Test, TEXT("Arena_04"))) return false;
	FLiftLayout L; if (!L.Find(F, Test, Side == TEXT("North"))) return false;
	AWoTBotMoverLink* Link = LinkFor(F, L.Lift);
	if (!Test.TestNotNull(TEXT("this lift has its bot mover link"), Link)) return false;
	const FVector Entry = Link->GetEntryLocation(), Exit = Link->GetExitLocation();
	const FVector Base = L.Lift->GetBasePosition();
	const double Half = GetDefault<AWoTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const double Radius = GetDefault<AWoTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FBox Shaft = L.Lift->GetComponentsBoundingBox(true);
	const auto InFootprint = [&](const FVector& P)
	{
		return P.X > Shaft.Min.X - Radius && P.X < Shaft.Max.X + Radius && P.Y > Shaft.Min.Y - Radius && P.Y < Shaft.Max.Y + Radius;
	};
	const bool bFight = Mode == TEXT("CastsAtVisibleEnemy"), bDisplaced = Mode == TEXT("AbandonsWhenDisplaced"),
		bLanding = Mode == TEXT("LandingExitCountsWhenDeckLeaves"), bContend = Mode == TEXT("ContenderWaitsBesideEntry"),
		bCooldown = Mode == TEXT("FailedLinkCoolsDown"), bOccupied = Mode == TEXT("RetreatEndsBesideOccupiedEntry"),
		bMissed = bOccupied || bCooldown || Mode == TEXT("StepsOffAfterMissedExit");
	if (!Test.TestTrue(TEXT("known handler case"), bFight || bDisplaced || bLanding || bContend || bMissed)) return false;
	if (bDisplaced && !Test.TestTrue(TEXT("displacement target is well away from the entry"), FVector::Dist2D(L.Start, Entry) > 150)) return false;
	if ((bFight || bDisplaced) && !SendAway(F, Test, L)) return false;

	AWoTBotController* A = SpawnOnFloor(F, L.Start);
	if (bFight) Give(CastChecked<AWoTCharacter>(A->GetPawn()), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FObservedBot First(A, L.Pool);
	AWoTBotController* B = nullptr;
	TUniquePtr<FObservedBot> Second;
	if (bContend)
	{
		FVector SecondFeet;
		const FVector Across = FVector::CrossProduct((L.Deck - L.Start).GetSafeNormal2D(), FVector::UpVector);
		if (!Test.TestTrue(TEXT("second contender has a real lower-floor start"), F.Project(L.Start + Across * 90, SecondFeet)
			&& FMath::Abs(SecondFeet.Z - L.Start.Z) < 20 && F.Complete(L.Start, SecondFeet))) return false;
		B = SpawnOnFloor(F, SecondFeet); Second = MakeUnique<FObservedBot>(B, L.Pool);
	}
	int32 Casts = 0;
	const FDelegateHandle CastHandle = WoTSoakEvents::Events().AddLambda([&](WoTSoakEvents::EKind Kind, AActor*, AActor* Participant)
	{
		if (Kind == WoTSoakEvents::EKind::Cast && Participant && Participant == First.Pawn.Get() && FWoTBotMoverAccess::Active(A) == Link) ++Casts;
	});
	ON_SCOPE_EXIT { WoTSoakEvents::Events().Remove(CastHandle); };
	Test.TestTrue(TEXT("bot accepts a route through this lift"), FWoTBotMoverAccess::Seek(A, L.Pool));
	if (B) Test.TestTrue(TEXT("second bot accepts a route through this lift"), FWoTBotMoverAccess::Seek(B, L.Pool));

	TArray<AWoTCharacter*> People;
	AWoTCharacter* Foe = nullptr;
	FVector LastLift = L.Lift->GetActorLocation();
	double LastLiftMotion = 0, BeganAt = -1, EndedAt = -1, DisplacedAt = -1, BackAt = -1, PeopleAt = -1;
	int32 RidesAtEnd = -1, ContenderSamples = 0;
	double ContenderClosest = DBL_MAX;
	bool bContenderUnderDeck = false, bEndOnLift = false, bEndGrounded = false, bEndInFootprint = false;
	bool bRetriedEarly = false, bCooldownBlocks = false, bOthersPlan = false, bCooldownEnds = false, bCooldownChecked = false;
	double EndFeetZ = 0;
	uint64 ThinksAtBegin = 0, ThinksDuring = 0;
	int32 SeenFoe = 0;
	bool bFoeInSight = false;
	TArray<FVector> Trail;
	FVector ReplanOrigin[2] = {FVector::ZeroVector, FVector::ZeroVector};
	bool bReplanned[2] = {false, false}, bReplanProgress[2] = {false, false};
	for (int32 I = 0; I < 60 * 40; ++I)
	{
		F.Tick();
		const double Now = F.W->GetTimeSeconds();
		const FVector Lift = L.Lift->GetActorLocation();
		if (!Lift.Equals(LastLift, .02)) LastLiftMotion = Now;
		LastLift = Lift;
		const bool bStoppedAtBottom = Now - LastLiftMotion > .1 && FMath::Abs(Lift.Z - Base.Z) < 1;
		First.Sample(L.Lift, L.Top); if (Second) Second->Sample(L.Lift, L.Top);
		if (First.bIdentityChanged || (Second && Second->bIdentityChanged)) break;
		for (AWoTBotController* Each : {A, B}) if (Each && FWoTBotMoverAccess::Active(Each) == Link) FWoTBotMoverAccess::Think(Each);
		const int32 Stage = FWoTBotMoverAccess::Stage(A);
		if (BeganAt < 0 && FWoTBotMoverAccess::Active(A) == Link) BeganAt = Now;
		if (BeganAt >= 0 && EndedAt < 0 && !FWoTBotMoverAccess::Active(A))
		{
			EndedAt = Now; RidesAtEnd = Link->GetLiftRides();
			bEndOnLift = OnLift(First.Pawn.Get(), L.Lift);
			bEndGrounded = First.Pawn.IsValid() && First.Pawn->GetCharacterMovement()->IsMovingOnGround();
			bEndInFootprint = First.Pawn.IsValid() && InFootprint(First.Pawn->GetActorLocation());
			EndFeetZ = First.Pawn.IsValid() ? First.Pawn->GetActorLocation().Z - Half : 0;
		}
		if (bFight && BeganAt < 0 && First.Pawn.IsValid()) Trail.Add(First.Pawn->GetActorLocation() - FVector(0, 0, Half));
		if (bFight && BeganAt >= 0 && !Foe)
		{
			// An enemy steps onto this floor where the waiting bot can actually see it:
			// the first real navigable spot ahead of its view, else on its own approach.
			const FVector Here = First.Pawn->GetActorLocation();
			TArray<FVector> Spots;
			for (const float Turn : {0.f, 25.f, -25.f, 50.f, -50.f})
				for (const float Reach : {300.f, 450.f, 600.f, 200.f})
				{
					FVector Spot;
					if (F.Project(Here + FRotator(0, A->GetControlRotation().Yaw + Turn, 0).Vector() * Reach, Spot)
						&& FMath::Abs(Spot.Z - (Here.Z - Half)) < 40 && FVector::Dist2D(Spot, Here) > 150 && !InFootprint(Spot)) Spots.Add(Spot);
				}
			for (int32 Back = Trail.Num() - 1; Back >= 0; --Back)
				if (FVector::Dist2D(Trail[Back], Here) >= 150 && !InFootprint(Trail[Back])) Spots.Add(Trail[Back]);
			for (const FVector& Spot : Spots)
			{
				if (!Foe) Foe = Stander(F, Spot, 1);
				else Foe->TeleportTo(Spot + FVector(0, 0, Half + 3), Foe->GetActorRotation());
				if (A->CanSee(Foe)) break;
			}
			if (Foe)
			{
				const APawn* Eye = A->GetPawn();
				const float Cone = FMath::Cos(FMath::DegreesToRadians(GetDefault<UWoTBotSettings>()->SightConeDegrees * .5f));
				Test.AddInfo(FString::Printf(TEXT("LIFT_FIGHT_PLACE bot=%s foe=%s view=%s to_foe=%s cone=%d vis_blocked=%d hostile_team=%d can_see=%d"),
					*Eye->GetActorLocation().ToString(), *Foe->GetActorLocation().ToString(), *Eye->GetViewRotation().ToString(),
					*(Foe->GetActorLocation() - Eye->GetActorLocation()).GetSafeNormal().ToString(),
					WoTSight::PawnCanSeeActor(Eye, Foe, Cone), WoTVisBlockingTraceHits(F.W, Eye->GetPawnViewLocation(), Foe->GetActorLocation() + FVector(0, 0, Half * .8), Eye),
					int32(WoTAIUtil::ApparentTeamOf(Foe)), A->CanSee(Foe)));
			}
			ThinksAtBegin = A->GetThinkCount();
		}
		if (bFight && Foe && FWoTBotMoverAccess::Active(A) == Link)
		{
			SeenFoe += A->GetTarget() == Foe && A->IsTargetVisible();
			ThinksDuring = A->GetThinkCount() - ThinksAtBegin;
			bFoeInSight |= A->CanSee(Foe);
		}
		if (bDisplaced && DisplacedAt < 0 && Stage == FWoTBotMoverAccess::Wait && Lift.Z > Base.Z + 60)
		{
			// A swap or a blast sets the waiting body down elsewhere on the floor.
			First.Pawn->TeleportTo(L.Start + FVector(0, 0, Half + 3), First.Pawn->GetActorRotation());
			DisplacedAt = Now;
		}
		if ((bMissed || bLanding) && PeopleAt < 0 && Stage == (bLanding ? FWoTBotMoverAccess::Exit : FWoTBotMoverAccess::Ride))
		{
			// Someone steps onto the landing exit (before the stop, or as the bot steps
			// off); for the occupied case someone also waits at the lower entry.
			People.Add(Stander(F, Exit, 0));
			if (bOccupied) People.Add(Stander(F, Entry, 0));
			PeopleAt = Now;
		}
		if (bMissed && PeopleAt >= 0 && BackAt < 0 && First.bRisen && bStoppedAtBottom && OnLift(First.Pawn.Get(), L.Lift)) BackAt = Now;
		if (bContend)
		{
			AWoTBotController* Bots[] = {A, B};
			for (int32 R = 0; R < 2; ++R)
			{
				AWoTBotController* Other = Bots[1 - R];
				if (FWoTBotMoverAccess::Stage(Bots[R]) == FWoTBotMoverAccess::Ride && Lift.Z > Base.Z + 60
					&& FWoTBotMoverAccess::Stage(Other) == FWoTBotMoverAccess::Contended && Other->GetPawn())
				{
					const FVector P = Other->GetPawn()->GetActorLocation();
					ContenderClosest = FMath::Min(ContenderClosest, FVector::Dist2D(P, Entry));
					bContenderUnderDeck |= InFootprint(P);
					++ContenderSamples;
				}
			}
			FObservedBot* Contenders[] = {&First, Second.Get()};
			for (int32 R = 0; R < 2; ++R)
			{
				FObservedBot& O = *Contenders[R];
				if (!bReplanned[R] && O.Controller->GetSeekingPool() != L.Pool)
				{ bReplanned[R] = true; ReplanOrigin[R] = O.Pawn->GetActorLocation(); }
				if (bReplanned[R]) bReplanProgress[R] |= FVector::Dist2D(O.Pawn->GetActorLocation(), ReplanOrigin[R]) > 100;
			}
		}
		if (bCooldown && EndedAt >= 0)
		{
			if (!bCooldownChecked)
			{
				bCooldownChecked = true;
				for (AWoTCharacter* Person : People) RemovePerson(Person);
				People.Reset();
				bCooldownBlocks = !PlanUses(F, Link, A, L.Start, L.Pool->GetActorLocation());
				bOthersPlan = PlanUses(F, Link, nullptr, L.Start, L.Pool->GetActorLocation());
			}
			bRetriedEarly |= Now < EndedAt + 14.5 && FWoTBotMoverAccess::Active(A) == Link;
			if (Now > EndedAt + 15.5) { bCooldownEnds = PlanUses(F, Link, A, L.Start, L.Pool->GetActorLocation()); break; }
		}
		else if (bContend)
		{
			if ((First.bExited && (Second->bExited || bReplanProgress[1])) || (Second->bExited && bReplanProgress[0])) break;
		}
		else if (bFight && First.bExited) break;
		else if (EndedAt >= 0 && Now > EndedAt + 1) break;
	}
	First.AssertIdentity(Test, TEXT("first")); if (Second) Second->AssertIdentity(Test, TEXT("second"));
	Test.AddInfo(FString::Printf(TEXT("LIFT_HANDLER case=%s began=%.2f ended=%.2f displaced=%.2f people=%.2f back=%.2f rides=%d casts=%d end_on_lift=%d end_grounded=%d end_footprint=%d end_feet_z=%.1f exit_z=%.1f contender_samples=%d contender_closest=%.1f contender_under_deck=%d cooldown_blocks=%d others_plan=%d retried_early=%d cooldown_ends=%d"),
		*Parameters, BeganAt, EndedAt, DisplacedAt, PeopleAt, BackAt, RidesAtEnd, Casts, bEndOnLift, bEndGrounded, bEndInFootprint, EndFeetZ, Exit.Z,
		ContenderSamples, ContenderSamples ? ContenderClosest : -1.0, bContenderUnderDeck, bCooldownBlocks, bOthersPlan, bRetriedEarly, bCooldownEnds));
	Test.TestTrue(TEXT("the lift handler took this bot"), BeganAt >= 0);
	if (bFight)
	{
		Test.AddInfo(FString::Printf(TEXT("LIFT_FIGHT foe=%s in_sight=%d seen_ticks=%d thinks=%llu"),
			*GetNameSafe(Foe), bFoeInSight, SeenFoe, ThinksDuring));
		Test.TestTrue(TEXT("a visible enemy appeared while the handler owned the bot"), Foe != nullptr && bFoeInSight);
		Test.TestTrue(TEXT("bot casts at the visible enemy during its lift traversal"), Casts > 0);
		Test.TestTrue(TEXT("bot still rides to the landing, or abandons cleanly"),
			First.bExited || (EndedAt >= 0 && !bEndOnLift && First.Respawns == 0));
	}
	if (bDisplaced)
	{
		Test.TestTrue(TEXT("waiting body was displaced while the lift was away"), DisplacedAt >= 0);
		Test.TestTrue(TEXT("displaced bot abandons the traversal promptly"), EndedAt >= DisplacedAt && EndedAt >= 0 && EndedAt - DisplacedAt < .5);
		Test.TestEqual(TEXT("an abandoned wait is not a ride"), RidesAtEnd, 0);
	}
	if (bMissed)
	{
		Test.TestTrue(TEXT("bot rode up to the occupied exit"), PeopleAt >= 0 && First.bRisen);
		Test.TestTrue(TEXT("bot came back down on the deck"), BackAt >= 0);
		Test.TestTrue(TEXT("bot leaves the stopped deck within seconds, not at the stage timeout"), BackAt >= 0 && EndedAt >= BackAt && EndedAt - BackAt < 3);
		Test.TestTrue(TEXT("bot ends on stationary floor outside the deck footprint"), EndedAt >= 0 && !bEndOnLift && bEndGrounded && !bEndInFootprint);
		Test.TestEqual(TEXT("a missed exit is not a ride"), RidesAtEnd, 0);
	}
	if (bLanding)
	{
		Test.TestTrue(TEXT("someone stood at the exit as the bot stepped off"), PeopleAt >= 0);
		Test.TestTrue(TEXT("traversal ended"), EndedAt >= 0);
		Test.TestEqual(TEXT("bot on the landing when the deck leaves completed the ride"), RidesAtEnd, 1);
		Test.TestTrue(TEXT("bot ended grounded at the landing height, off the deck"),
			EndedAt >= 0 && !bEndOnLift && bEndGrounded && FMath::Abs(EndFeetZ - Exit.Z) < 25);
	}
	if (bContend)
	{
		Test.TestTrue(TEXT("one bot waited while the other rode"), ContenderSamples > 0);
		Test.TestTrue(TEXT("waiting contender leaves the entry clear"), ContenderSamples > 0 && ContenderClosest >= 45);
		Test.TestFalse(TEXT("waiting contender never stands in the deck footprint"), bContenderUnderDeck);
		Test.TestTrue(TEXT("at least one contender completes this lift ride"), First.bExited || Second->bExited);
		Test.TestTrue(TEXT("both get up or the other abandons the contested goal and makes progress"),
			(First.bExited && Second->bExited) || (First.bExited && bReplanProgress[1]) || (Second->bExited && bReplanProgress[0]));
	}
	if (bCooldown)
	{
		Test.TestTrue(TEXT("other queriers still plan through this lift"), bOthersPlan);
		Test.TestTrue(TEXT("bot that just failed this lift does not plan through it"), bCooldownBlocks);
		Test.TestFalse(TEXT("bot does not start this lift again during its cooldown"), bRetriedEarly);
		Test.TestTrue(TEXT("bot plans through this lift again once the cooldown ends"), bCooldownEnds);
	}
	for (AWoTCharacter* Person : People) RemovePerson(Person);
	RemovePerson(Foe);
	return !Test.HasAnyErrors();
}

struct FDoorLayout
{
	AWoTMover* Door = nullptr;
	AWoTTrigger* Trigger = nullptr;
	AWoTAngrealSpawnPool* ChamberPool = nullptr;
	FVector Inside, Outside, Normal, Center;
	bool Find(FFixture& F, FAutomationTestBase& Test)
	{
		int32 Movers = 0;
		for (TActorIterator<AWoTMover> It(F.W); It; ++It) { Door = *It; ++Movers; }
		if (!Test.TestEqual(TEXT("Arena_02 has its one authored door"), Movers, 1)) return false;
		for (TActorIterator<AWoTTrigger> It(F.W); It; ++It)
			if (!Door->TagName.IsNone() && It->EventName == Door->TagName) Trigger = *It;
		if (!Test.TestNotNull(TEXT("actual wired chamber trigger"), Trigger)) return false;
		const FBox Box = Door->GetComponentsBoundingBox(true);
		Center = Box.GetCenter();
		Normal = Box.GetExtent().X < Box.GetExtent().Y ? FVector::ForwardVector : FVector::RightVector;
		if (FVector::DotProduct(Trigger->GetActorLocation() - Center, Normal) > 0) Normal *= -1;
		double Closest = DBL_MAX;
		for (TActorIterator<AWoTAngrealSpawnPool> It(F.W); It; ++It)
		{
			FVector Feet;
			if (FVector::DotProduct(It->GetActorLocation() - Center, Normal) >= -40 || !F.Project(It->GetActorLocation(), Feet)) continue;
			const double Distance = FVector::DistSquared(Feet, Trigger->GetActorLocation());
			if (Distance < Closest) { Closest = Distance; ChamberPool = *It; Inside = Feet; }
		}
		if (!Test.TestNotNull(TEXT("real chamber pool supplies the inside start"), ChamberPool)) return false;
		Closest = DBL_MAX;
		for (TActorIterator<AWoTPathNode> It(F.W); It; ++It)
		{
			FVector Feet;
			if (!F.Project(It->GetActorLocation(), Feet) || FMath::Abs(Feet.Z - Inside.Z) > 60
				|| FVector::DotProduct(Feet - Center, Normal) < 150) continue;
			const double Distance = FVector::DistSquared(Feet, Center);
			if (Distance < Closest) { Closest = Distance; Outside = Feet; }
		}
		Test.AddInfo(FString::Printf(TEXT("DOOR_LAYOUT actor=%s trigger=%s bounds=%s inside=%s outside=%s normal=%s pool=%s"),
			*Door->GetName(), *Trigger->GetName(), *Box.ToString(), *Inside.ToString(), *Outside.ToString(), *Normal.ToString(), *ChamberPool->GetName()));
		return Test.TestTrue(TEXT("outside goal comes from a nearby real path node"), Closest < FMath::Square(1200.0))
			&& Test.TestTrue(TEXT("door begins closed with its authored trigger-control behavior"),
				Door->InitialState == EWoTMoverState::TriggerControl && Door->GetMoverPhase() == EWoTMoverPhase::Closed);
	}
};

bool DoorCase(FAutomationTestBase& Test, bool bExit)
{
	FFixture F; if (!F.Load(Test, TEXT("Arena_02"))) return false;
	FDoorLayout D; if (!D.Find(F, Test)) return false;
	AWoTBotController* B = SpawnOnFloor(F, bExit ? D.Inside : D.Outside);
	FObservedBot Observed(B, nullptr);
	const FVector Goal = bExit ? D.Outside : D.Inside;
	const bool bRoute = F.Complete(B->GetPawn()->GetActorLocation(), Goal);
	const bool bAccepted = FWoTBotMoverAccess::Walk(B, Goal);
	bool bOpened = false, bReached = false, bCrossed = false;
	for (int32 I = 0; I < 60 * (bExit ? 25 : 5); ++I)
	{
		F.Tick(); Observed.Sample(); if (Observed.bIdentityChanged) break;
		bOpened |= D.Door->GetMoverPhase() != EWoTMoverPhase::Closed;
		const FVector P = Observed.Pawn->GetActorLocation();
		const double Side = FVector::DotProduct(P - D.Center, D.Normal);
		bCrossed |= bExit ? Side > 50 : Side < -50;
		bReached |= FVector::Dist2D(P, Goal) < 65 && FMath::Abs(P.Z - Goal.Z - Observed.Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) < 35;
		if (bExit && bReached) break;
	}
	Observed.AssertIdentity(Test, bExit ? TEXT("chamber exit") : TEXT("exterior"));
	Test.AddInfo(FString::Printf(TEXT("DOOR_RESULT exit=%d complete_route=%d accepted=%d opened=%d crossed=%d reached=%d"), bExit, bRoute, bAccepted, bOpened, bCrossed, bReached));
	if (bExit)
	{
		Test.TestTrue(TEXT("chamber exit has a complete planned route before the door opens"), bRoute);
		Test.TestTrue(TEXT("chamber bot accepts its initial outside-goal request"), bAccepted);
		Test.TestEqual(TEXT("planned chamber exit does not rely on generic stuck recovery"), Observed.Recoveries, 0);
		Test.TestTrue(TEXT("chamber bot opens the real door by walking into its trigger"), bOpened);
		Test.TestTrue(TEXT("chamber bot passes through the door to the outside goal"), bCrossed && bReached);
	}
	else
	{
		Test.TestFalse(TEXT("outside has no complete route through the closed one-sided door"), bRoute);
		Test.TestFalse(TEXT("outside bot does not accept an impossible inward route"), bAccepted);
		Test.TestFalse(TEXT("outside body cannot operate the unreachable chamber trigger"), bOpened);
		Test.TestFalse(TEXT("outside bot never crosses the closed door"), bCrossed);
	}
	return !Test.HasAnyErrors();
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotRealLiftSpec, "WoT.Bots.Movers.RealMap.Lift", WoTBotTest::Flags)
void FBotRealLiftSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Side : {TEXT("North"), TEXT("South")})
		for (const TCHAR* Mode : {TEXT("Collect"), TEXT("Away"), TEXT("Concurrent")})
		{ const FString Name = FString(Side) + TEXT(".") + Mode; Names.Add(Name); Commands.Add(Name); }
}
bool FBotRealLiftSpec::RunTest(const FString& Parameters) { return WoTBotMoverTest::LiftCase(*this, Parameters); }

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotRealLiftHandlerSpec, "WoT.Bots.Movers.RealMap.LiftHandler", WoTBotTest::Flags)
void FBotRealLiftHandlerSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Side : {TEXT("North"), TEXT("South")})
		for (const TCHAR* Mode : {TEXT("CastsAtVisibleEnemy"), TEXT("AbandonsWhenDisplaced"), TEXT("StepsOffAfterMissedExit"),
			TEXT("RetreatEndsBesideOccupiedEntry"), TEXT("LandingExitCountsWhenDeckLeaves"), TEXT("ContenderWaitsBesideEntry"),
			TEXT("FailedLinkCoolsDown")})
		{ const FString Name = FString(Side) + TEXT(".") + Mode; Names.Add(Name); Commands.Add(Name); }
}
bool FBotRealLiftHandlerSpec::RunTest(const FString& Parameters) { return WoTBotMoverTest::LiftHandlerCase(*this, Parameters); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRealChamberExitSpec, "WoT.Bots.Movers.RealMap.Door.ChamberExit", WoTBotTest::Flags)
bool FBotRealChamberExitSpec::RunTest(const FString&) { return WoTBotMoverTest::DoorCase(*this, true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRealChamberExteriorSpec, "WoT.Bots.Movers.RealMap.Door.ExteriorRejectsClosedDoor", WoTBotTest::Flags)
bool FBotRealChamberExteriorSpec::RunTest(const FString&) { return WoTBotMoverTest::DoorCase(*this, false); }

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotRealMoverSteadyNavSpec, "WoT.Bots.Movers.RealMap.SteadyNavigation", WoTBotTest::Flags)
void FBotRealMoverSteadyNavSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Map : {TEXT("Arena_02"), TEXT("Arena_04")}) { Names.Add(Map); Commands.Add(Map); }
}
bool FBotRealMoverSteadyNavSpec::RunTest(const FString& Parameters)
{
	using namespace WoTBotMoverTest;
	FFixture F; if (!F.Load(*this, *Parameters)) return false;
	TMap<AWoTMover*, FTransform> Poses;
	for (TActorIterator<AWoTMover> It(F.W); It; ++It) Poses.Add(*It, It->GetActorTransform());
	WoTBotSoak::FNavProbe Probe(F.W);
	bool bStationary = true;
	for (int32 I = 0; I < 10 * 60; ++I)
	{
		F.Tick();
		for (const auto& Pair : Poses) bStationary &= Pair.Key->GetActorTransform().Equals(Pair.Value);
	}
	TestTrue(TEXT("steady navigation work drains"), F.Flush());
	TestTrue(TEXT("movers stayed stationary throughout the idle sample"), bStationary);
	AddInfo(FString::Printf(TEXT("MOVER_STEADY map=%s movers=%d seconds=10 areas=%d tiles=%d"), *Parameters, Poses.Num(), Probe.Areas, Probe.Tiles));
	TestEqual(TEXT("zero ArenaBot tile rebuilds while no mover moves"), Probe.Tiles, 0);
	return !HasAnyErrors();
}
#endif
