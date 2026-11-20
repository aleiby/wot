#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTBotRoster.h"
#include "AI/WoTPathNode.h"
#include "Testing/WoTSoakEvents.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "EngineUtils.h"
#include "TimerManager.h"

namespace BotArenaNavigation
{
using namespace WoTBotTest;
constexpr float Step = 1.0f / 30.0f;

struct FFixture
{
	UWorld* W = nullptr;
	UNavigationSystemV1* Nav = nullptr;
	const ANavigationData* Data = nullptr;
	~FFixture() { WoTTest::DestroySpecWorld(W); }
	bool Load(FAutomationTestBase& Test)
	{
		SeedDecisions(822098492);
		FMapSet Maps; FString Error;
		if (!Maps.Load(Error)) { Test.AddError(Error); return false; }
		const auto* Map = Maps.Descriptors.FindByPredicate([](const auto& D) { return D.MapIdentity == TEXT("Arena_08"); });
		if (!Test.TestNotNull(TEXT("real Arena_08 descriptor"), Map)) return false;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false; Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		W = WoTTest::CreateSpecWorld(Options);
		if (!Materialize(W, Maps, *Map, Error)) { Test.AddError(Error); return false; }
		UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
		Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
		W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
		FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
		Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
		if (!Test.TestNotNull(TEXT("runtime navigation"), Nav)) return false;
		Nav->Build();
		for (ANavigationData* Mesh : Nav->NavDataSet) if (Mesh) Mesh->EnsureBuildCompletion();
		Data = Nav->GetNavDataForAgentName(TEXT("ArenaBot"));
		if (!Test.TestNotNull(TEXT("runtime bot mesh"), Data)) return false;
		W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?BotFill=0?FragLimit=0?TimeLimit=0"), TRAVEL_Absolute);
		if (!Test.TestTrue(TEXT("real game mode"), W->SetGameMode(URL))) return false;
		W->InitializeActorsForPlay(URL); W->BeginPlay();
		Test.AddInfo(FString::Printf(TEXT("ARENA_LINKS count=%d"), W->GetSubsystem<UWoTBotRoster>()->BuildLinks()));
		WoTTest::TickWorld(W, .2f);
		for (ANavigationData* Mesh : Nav->NavDataSet) if (Mesh) Mesh->EnsureBuildCompletion();
		return true;
	}
	AWoTAngrealSpawnPool* FindPool(FVector Near)
	{
		for (TActorIterator<AWoTAngrealSpawnPool> It(W); It; ++It)
			if (FVector::Dist(It->GetActorLocation(), Near) < 5) return *It;
		return nullptr;
	}
	void OnlyPool(AWoTAngrealSpawnPool* Keep)
	{
		// Isolate the destination, retaining its runtime item and all map geometry.
		for (TActorIterator<AWoTAngrealSpawnPool> It(W); It; ++It) if (*It != Keep) It->Destroy();
	}
	bool Route(FVector From, FVector To)
	{
		FNavLocation Start, End;
		if (!Nav->ProjectPointToNavigation(From, Start, FVector(50, 50, 150), Data)
			|| !Nav->ProjectPointToNavigation(To, End, FVector(50, 50, 150), Data)) return false;
		FPathFindingQuery Query(nullptr, *Data, Start.Location, End.Location);
		Query.SetAllowPartialPaths(false);
		const auto Path = Nav->FindPathSync(Data->GetConfig(), Query);
		return Path.IsSuccessful() && Path.Path.IsValid() && !Path.Path->IsPartial();
	}
	TArray<AWoTPathNode*> PoolApproaches(FAutomationTestBase& Test, AWoTAngrealSpawnPool* Pool)
	{
		TArray<AWoTPathNode*> Starts;
		for (TActorIterator<AWoTPathNode> It(W); It; ++It)
			for (const FWoTPathEdge& Edge : It->OutEdges)
			{
				if (!Edge.Other || FVector::Dist(Edge.Other->GetActorLocation(), Pool->GetActorLocation()) > 100
					|| FVector::Dist2D(It->GetActorLocation(), Pool->GetActorLocation()) < 300) continue;
				FNavLocation A, B;
				if (!Nav->ProjectPointToNavigation(It->GetActorLocation(), A, FVector(50,50,150), Data)
					|| !Nav->ProjectPointToNavigation(Edge.Other->GetActorLocation(), B, FVector(50,50,150), Data)) continue;
				Test.AddInfo(FString::Printf(TEXT("POOL_REACH %s node=%s projected=%s -> %s node=%s projected=%s rise=%.2f projected_rise=%.2f distance=%.2f route=%d"),
					*It->GetName(), *It->GetActorLocation().ToString(), *A.Location.ToString(), *Edge.Other->GetName(),
					*Edge.Other->GetActorLocation().ToString(), *B.Location.ToString(), Edge.Other->GetActorLocation().Z - It->GetActorLocation().Z,
					B.Location.Z - A.Location.Z, Edge.Distance, Route(It->GetActorLocation(), Pool->GetActorLocation())));
				Starts.AddUnique(*It);
			}
		return Starts;
	}
	bool Collect(FAutomationTestBase& Test, AWoTBotController* B, AWoTAngrealSpawnPool* Pool, float Seconds)
	{
		const TWeakObjectPtr<APawn> Original = B->GetPawn();
		int32 Collections = 0, Respawns = 0;
		bool Swam = false, Fell = false;
		const auto Handle = WoTSoakEvents::Events().AddLambda([&](WoTSoakEvents::EKind Kind, AActor* Subject, AActor* Participant)
		{
			if (Kind == WoTSoakEvents::EKind::PoolCollect && Subject == Pool && Participant == Original.Get()) ++Collections;
			if (Kind == WoTSoakEvents::EKind::SilentRespawn && Subject == B) ++Respawns;
		});
		ON_SCOPE_EXIT { WoTSoakEvents::Events().Remove(Handle); };
		int32 I = 0;
		for (; I < FMath::CeilToInt(Seconds / Step) && Collections == 0; ++I)
		{
			WoTTest::TickWorld(W, Step, Step);
			if (const auto* Pawn = Cast<AWoTCharacter>(B->GetPawn()))
			{
				Swam |= Pawn->GetCharacterMovement()->IsSwimming();
				Fell |= Pawn->GetCharacterMovement()->IsFalling();
			}
			if (I % 30 == 0 && B->GetPawn())
				Test.AddInfo(FString::Printf(TEXT("ARENA_MOTION t=%.2f body=%s destination=%s mode=%d speed=%.1f follow=%d unavailable=%d"),
					(I+1)*Step, *B->GetPawn()->GetActorLocation().ToString(), *B->GetDestination().ToString(),
					int32(CastChecked<AWoTCharacter>(B->GetPawn())->GetCharacterMovement()->MovementMode), B->GetPawn()->GetVelocity().Size(),
					int32(B->GetPathFollowingComponent()->GetStatus()), B->IsNavigationUnavailable()));
		}
		Test.AddInfo(FString::Printf(TEXT("ARENA_RESULT seconds=%.2f collections=%d silent_respawns=%d climbs=%llu swam=%d fell=%d"),
			I*Step, Collections, Respawns, B->GetClimbJumpCount(), Swam, Fell));
		Test.TestEqual(TEXT("no silent respawn"), Respawns, 0);
		Test.TestTrue(TEXT("the original body reaches the destination"), Original.IsValid() && Original == B->GetPawn());
		return Collections > 0;
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterRingLinkSpec, "WoT.Bots.Navigation.Arena08.WaterRingLink", Flags)
bool FWaterRingLinkSpec::RunTest(const FString&)
{
	FFixture F; if (!F.Load(*this)) return false;
	AWoTAngrealSpawnPool* Pool = F.FindPool(FVector(-768,-1151,-482));
	if (!TestNotNull(TEXT("runtime water-ring pool"), Pool)) return false;
	const auto Starts = F.PoolApproaches(*this, Pool);
	TestTrue(TEXT("the platform has original incoming reaches"), Starts.Num() >= 4);
	for (const AWoTPathNode* Start : Starts)
		TestTrue(TEXT("the original reach has a complete mesh route onto the platform"), F.Route(Start->GetActorLocation(), Pool->GetActorLocation()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterRingCollectSpec, "WoT.Bots.Navigation.Arena08.WaterRingCollected", Flags)
bool FWaterRingCollectSpec::RunTest(const FString&)
{
	FFixture F; if (!F.Load(*this)) return false;
	AWoTAngrealSpawnPool* Pool = F.FindPool(FVector(-768,-1151,-482));
	if (!TestNotNull(TEXT("runtime water-ring pool"), Pool)) return false;
	const auto Starts = F.PoolApproaches(*this, Pool);
	if (!TestTrue(TEXT("an approach across the water exists"), !Starts.IsEmpty())) return false;
	F.OnlyPool(Pool);
	AWoTBotController* B = Bot(F.W, Starts[0]->GetActorLocation());
	TestTrue(TEXT("a bot across the water collects the platform pool within 12 seconds"), F.Collect(*this, B, Pool, 12));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgeEscapeSpec, "WoT.Bots.Navigation.Arena08.LedgeEscape", Flags)
bool FLedgeEscapeSpec::RunTest(const FString&)
{
	FFixture F; if (!F.Load(*this)) return false;
	// Observe the ordinary path follower's departure from the upper floor,
	// before allowing the controller's recovery to participate.
	AWoTBotController* Arriving = Bot(F.W, FVector(-591,128,50));
	F.W->GetTimerManager().ClearAllTimersForObject(Arriving);
	Arriving->SetActorTickEnabled(false);
	FNavLocation Below;
	if (F.Nav->ProjectPointToNavigation(FVector(-382,-132,-350), Below, FVector(50,50,150), F.Data))
	{
		const auto Request = Arriving->MoveToLocation(Below.Location, 20, false, true, false, false, nullptr, false);
		AddInfo(FString::Printf(TEXT("LEDGE_ARRIVAL request=%d target=%s"), int32(Request), *Below.Location.ToString()));
		if (const auto Path = Arriving->GetPathFollowingComponent()->GetPath(); Path.IsValid())
			for (const auto& Point : Path->GetPathPoints())
				AddInfo(FString::Printf(TEXT("LEDGE_PATH point=%s flags=%u"), *Point.Location.ToString(), Point.Flags));
		for (int32 I = 0; I < 90; ++I)
		{
			WoTTest::TickWorld(F.W, Step, Step);
			if (I == 12)
			{
				// A regular destination refresh while crossing the bevel loses the
				// old drop path: its new start is outside the mesh query extent.
				const auto Repath = Arriving->MoveToLocation(Below.Location, 20, false, true, false, false, nullptr, false);
				AddInfo(FString::Printf(TEXT("LEDGE_REPATH result=%d body=%s follow=%d"), int32(Repath),
					*Arriving->GetPawn()->GetActorLocation().ToString(), int32(Arriving->GetPathFollowingComponent()->GetStatus())));
			}
			if (I % 3 == 0) AddInfo(FString::Printf(TEXT("LEDGE_ARRIVAL t=%.2f body=%s mode=%d speed=%.1f"),
				(I+1)*Step, *Arriving->GetPawn()->GetActorLocation().ToString(),
				int32(CastChecked<AWoTCharacter>(Arriving->GetPawn())->GetCharacterMovement()->MovementMode), Arriving->GetPawn()->GetVelocity().Size()));
		}
	}
	Arriving->GetPawn()->Destroy(); Arriving->Destroy();
	AWoTAngrealSpawnPool* Pool = F.FindPool(FVector(-384,1024,-226));
	if (!TestNotNull(TEXT("runtime lower-floor destination"), Pool)) return false;
	F.OnlyPool(Pool);
	AWoTBotController* B = Bot(F.W, FVector(-499,76,10));
	for (TActorIterator<AWoTPathNode> It(F.W); It; ++It)
		if (FVector::Dist(It->GetActorLocation(), FVector(-523,125,50)) < 5)
			for (const auto& Edge : It->OutEdges) if (Edge.Other)
				AddInfo(FString::Printf(TEXT("LEDGE_REACH from=%s to=%s route=%d"), *It->GetActorLocation().ToString(),
					*Edge.Other->GetActorLocation().ToString(), F.Route(It->GetActorLocation(), Edge.Other->GetActorLocation())));
	FNavLocation Nearest;
	const bool Projected = F.Nav->ProjectPointToNavigation(B->GetPawn()->GetNavAgentLocation(), Nearest, FVector(400,400,800), F.Data);
	AddInfo(FString::Printf(TEXT("LEDGE_PROJECTION found=%d feet=%s nearest=%s horizontal=%.2f"), Projected,
		*B->GetPawn()->GetNavAgentLocation().ToString(), *Nearest.Location.ToString(), FVector::Dist2D(B->GetPawn()->GetActorLocation(), Nearest.Location)));
	TestTrue(TEXT("a bot on the ledge drops off and collects the lower destination within 15 seconds"), F.Collect(*this, B, Pool, 15));
	return !HasAnyErrors();
}
}
#endif
