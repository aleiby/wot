#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotFactionProfile.h"
#include "AI/WoTNPCCharacter.h"
#include "WoTZoneVolume.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "EngineUtils.h"
namespace WoTBotTest
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNavIsolationSpec, "WoT.Bots.Navigation.OnlyBotsSelectPlayerAgent", Flags)
bool FBotNavIsolationSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UNavigationSystemV1* Nav = BuildNav(W);
	if (!TestNotNull(TEXT("navigation built"), Nav)) return false;
	TestTrue(TEXT("plugin installs before nav initialization"), Nav->IsA<UWoTBotNavigationSystem>());
	const ANavigationData* Default = Nav->GetDefaultNavDataInstance();
	if (!TestNotNull(TEXT("original main mesh"), Default)) return false;
	TestTrue(TEXT("main retains original class"), Default->GetClass() == ARecastNavMesh::StaticClass());
	TestEqual(TEXT("original radius"), Default->GetConfig().AgentRadius, 35.0f);
	TestEqual(TEXT("original height"), Default->GetConfig().AgentHeight, 144.0f);
	TestEqual(TEXT("original step"), CastChecked<ARecastNavMesh>(Default)->GetAgentMaxStepHeight(ENavigationDataResolution::Default), 35.0f);
	AWoTBotController* B = Bot(W, FVector(-800, 0, 50));
	const ANavigationData* BotData = Nav->GetNavDataForProps(B->GetPawn()->GetNavAgentPropertiesRef());
	if (!TestNotNull(TEXT("bot data"), BotData)) return false;
	TestTrue(TEXT("bot explicitly selects own data"), BotData->IsA<AWoTBotNavMesh>() && BotData != Default);
	TestEqual(TEXT("bot radius"), BotData->GetConfig().AgentRadius, 17.0f);
	TestEqual(TEXT("bot height"), BotData->GetConfig().AgentHeight, 92.0f);
	TestEqual(TEXT("bot step"), CastChecked<ARecastNavMesh>(BotData)->GetAgentMaxStepHeight(ENavigationDataResolution::Default), 25.0f);
	AWoTNPCCharacter* NPC = W->SpawnActor<AWoTNPCCharacter>(FVector(800, 0, 50), FRotator::ZeroRotator);
	for (float Radius : { 10.0f, 17.0f, 35.0f, 60.0f, 100.0f })
	{
		FNavAgentProperties Props = NPC->GetNavAgentPropertiesRef();
		Props.AgentRadius = Radius; Props.AgentHeight = Radius * 2;
		TestTrue(TEXT("all NPC sizes keep original data with no preference"), Nav->GetNavDataForProps(Props) == Default);
		TestTrue(TEXT("location-aware NPC query also keeps original data"), Nav->GetNavDataForProps(Props, NPC->GetActorLocation(), FVector(50)) == Default);
	}
	UNavigationPath* Path = Nav->FindPathToLocationSynchronously(W, FVector(-500, 500, 50), FVector(500, 500, 50));
	TestTrue(TEXT("seeker's context-free fallback retains main data"), Path && Path->GetPath().IsValid()
		&& Path->GetPath()->GetNavigationDataUsed() == Default);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPassageSpec, "WoT.Bots.Navigation.PlayerWidthPassage", Flags)
bool FBotPassageSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	Box(W, FVector(0, -1032, 150), FVector(100, 1000, 150));
	Box(W, FVector(0, 1032, 150), FVector(100, 1000, 150));
	UNavigationSystemV1* Nav = BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-800, 0, 50));
	AWoTAngrealSpawnPool* Goal = Pool(W, FVector(800, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	const ANavigationData* Default = Nav ? Nav->GetDefaultNavDataInstance() : nullptr;
	if (!TestNotNull(TEXT("default mesh built"), Default)) return false;
	FPathFindingQuery Query(B, *Default, FVector(-800, 0, 0), FVector(800, 0, 0));
	Query.SetAllowPartialPaths(false);
	TestFalse(TEXT("default agent cannot traverse passage"), Nav->FindPathSync(Default->GetConfig(), Query).IsSuccessful());
	const ARecastNavMesh* Small = Cast<ARecastNavMesh>(Nav->GetNavDataForProps(B->GetPawn()->GetNavAgentPropertiesRef()));
	if (!TestNotNull(TEXT("small mesh exists"), Small)) return false;
	FPathFindingQuery SmallQuery(B, *Small, FVector(-800, 0, 0), FVector(800, 0, 0));
	SmallQuery.SetAllowPartialPaths(false);
	TestTrue(TEXT("bot agent has complete route"), Nav->FindPathSync(Small->GetConfig(), SmallQuery).IsSuccessful());
	bool Crossed = false;
	for (int32 I = 0; I < 600; ++I)
	{
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		Crossed |= B->GetPawn() && B->GetPawn()->GetActorLocation().X > 200;
	}
	TestTrue(TEXT("bot physically traverses narrow passage"), Crossed);
	TestNull(TEXT("bot picks up on other side"), Goal->PickupItem.Get());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotWaterSpec, "WoT.Bots.Navigation.WaterEntrySwimAndExit", Flags)
bool FBotWaterSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTZoneVolume* Water = W->SpawnActor<AWoTZoneVolume>(FVector(0, 0, 0), FRotator::ZeroRotator);
	Water->Collision->SetBoxExtent(FVector(250, 1900, 150)); Water->bWaterZone = true;
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-800, 0, 50));
	Pool(W, FVector(800, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	bool Entered = false, Swam = false, Exited = false;
	float EntryX = 0;
	for (int32 I = 0; I < 900; ++I)
	{
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		AWoTCharacter* Pawn = Cast<AWoTCharacter>(B->GetPawn());
		if (!Pawn) break;
		if (Pawn->GetCharacterMovement()->IsSwimming())
		{
			if (!Entered) EntryX = Pawn->GetActorLocation().X;
			Entered = true;
			Swam |= Pawn->GetActorLocation().X > EntryX + 200;
		}
		else if (Entered && Pawn->GetActorLocation().X > 300) { Exited = true; break; }
	}
	TestTrue(TEXT("enters real water zone"), Entered);
	TestTrue(TEXT("path acceleration moves across water"), Swam);
	TestTrue(TEXT("exits water on far side"), Exited);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSeekSpec, "WoT.Bots.Movement.SeekValuePerPathDistanceAndPickup", Flags)
bool FBotSeekSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-1500, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	AWoTAngrealSpawnPool* Near = Pool(W, FVector(500, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	const auto* Profile = WoTBotProfiles::Get().Find(B->GetPawn()->GetClass());
	TestTrue(TEXT("fixture exercises native item value fallback"), Profile && !Profile->Priorities.Contains(Far->PickupItem->GetClass()));
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("stocked pool selected"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	TestTrue(TEXT("equal profile value chooses shorter path"), B->GetDestination().X > 0);
	WoTTest::TickWorld(W, 1.8f);
	TestNull(TEXT("real overlap collects selected pool"), Near->PickupItem.Get());
	TestNotNull(TEXT("farther pool still stocked"), Far->PickupItem.Get());
	bool Holds = false;
	for (AWoTInventory* Item : CastChecked<AWoTCharacter>(B->GetPawn())->InventoryComponent->Items)
		Holds |= Item->GetClass()->GetName() == TEXT("WoTAngrealInvFireball");
	TestTrue(TEXT("collected artifact really held"), Holds);
	Pool(W, FVector(1200, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	CastChecked<AWoTAngrealInventory>(Far->PickupItem)->Priority = 10000;
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("higher native value can justify a longer path"), B->GetDestination().X < 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPoolProfileSpec, "WoT.Bots.Movement.ProfileOverridesNativePoolValue", Flags)
bool FBotPoolProfileSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector::ZeroVector + FVector(0, 0, 50));
	const auto* Profile = WoTBotProfiles::Get().Find(B->GetPawn()->GetClass());
	if (!TestNotNull(TEXT("runtime class profile"), Profile)) return false;
	TArray<UClass*> Classes;
	for (const auto& Entry : Profile->Priorities)
		if (Entry.Value > 0 && Entry.Key != CastChecked<AWoTCharacter>(B->GetPawn())->DefaultAngrealInventory)
			Classes.Add(Entry.Key);
	Classes.Sort([](const UClass& A, const UClass& Other) { return A.GetPathName() < Other.GetPathName(); });
	if (!TestTrue(TEXT("profile has a valued artifact"), !Classes.IsEmpty())) return false;
	const FString Path = Classes[0]->GetPathName();
	AWoTAngrealSpawnPool* Far = Pool(W, FVector(-1500, 0, 30), *Path);
	Pool(W, FVector(500, 0, 30), *Path);
	CastChecked<AWoTAngrealInventory>(Far->PickupItem)->Priority = 10000;
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("equal class preference overrides native value and chooses shorter path"), B->GetDestination().X > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotFrozenSwimmingSpec, "WoT.Bots.Navigation.MatchEndFreezesSwimming", Flags)
bool FBotFrozenSwimmingSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTZoneVolume* Water = W->SpawnActor<AWoTZoneVolume>(FVector::ZeroVector, FRotator::ZeroRotator);
	Water->Collision->SetBoxExtent(FVector(250, 1900, 150)); Water->bWaterZone = true;
	Water->ZoneVelocity = FVector(0, 30, 0);
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(-800, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Pool(W, FVector(800, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	for (int32 I = 0; I < 300 && !Pawn->GetCharacterMovement()->IsSwimming(); ++I)
		WoTTest::TickWorld(W, 1.0f / 60.0f);
	TestTrue(TEXT("swimming before match ends"), Pawn->GetCharacterMovement()->IsSwimming() && Pawn->GetVelocity().Size() > 20);
	W->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
	const FVector Position = Pawn->GetActorLocation(); const uint64 Thinks = B->GetThinkCount();
	WoTTest::TickWorld(W, 2.0f);
	TestTrue(TEXT("match end stops actual path movement in water"), Pawn->GetActorLocation().Equals(Position, .1f));
	TestEqual(TEXT("no think after match end"), B->GetThinkCount(), Thinks);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotRoamStuckSpec, "WoT.Bots.Movement.RoamRecoveryAndSilentRespawn", Flags)
bool FBotRoamStuckSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	// The assertions require a roam outside the arrival radius and a different
	// recovery destination. Pin those draws after world/nav initialization.
	SeedDecisions(0x7051);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	const FVector Start = Pawn->GetActorLocation();
	WoTTest::TickWorld(W, 1.0f);
	TestTrue(TEXT("roam physically moves"), FVector::Dist2D(Start, Pawn->GetActorLocation()) > 50);
	const FVector Cage = Pawn->GetActorLocation();
	Box(W, Cage + FVector(45, 0, 50), FVector(10, 70, 150), false);
	Box(W, Cage - FVector(45, 0, -50), FVector(10, 70, 150), false);
	Box(W, Cage + FVector(0, 45, 50), FVector(70, 10, 150), false);
	Box(W, Cage - FVector(0, 45, -50), FVector(70, 10, 150), false);
	const FVector Destination = B->GetDestination();
	const uint64 Repaths = B->GetRepathCount();
	WoTTest::TickWorld(W, 3.8f);
	TestTrue(TEXT("blocked path chooses another destination"), B->GetRepathCount() > Repaths && !B->GetDestination().Equals(Destination));
	const TWeakObjectPtr<AWoTCharacter> Old = Pawn;
	TArray<TWeakObjectPtr<AWoTInventory>> Items;
	for (AWoTInventory* Item : Pawn->InventoryComponent->Items) Items.Add(Item);
	WoTTest::TickWorld(W, 22.0f);
	TestFalse(TEXT("twenty seconds stuck silently retires body"), Old.IsValid());
	TestEqual(TEXT("stuck has no death score"), B->GetPlayerState<AWoTPlayerState>()->Deaths, 0);
	for (const auto& Item : Items) TestFalse(TEXT("no death drop"), Item.IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotEndlessFallSpec, "WoT.Bots.Movement.EndlessFallIsNotProgress", Flags)
bool FBotEndlessFallSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->StuckRespawnSeconds = 3;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	// Beyond the floor, as through a gap in a floor's collision: the body would
	// otherwise fall for minutes before reaching KillZ, still counted as moving.
	AWoTBotController* B = Bot(W, FVector(5000, 0, 50));
	const TWeakObjectPtr<AWoTCharacter> Falling = CastChecked<AWoTCharacter>(B->GetPawn());
	WoTTest::TickWorld(W, .5f);
	TestTrue(TEXT("body falls"), Falling.IsValid() && Falling->GetCharacterMovement()->IsFalling());
	WoTTest::TickWorld(W, 3.0f);
	TestFalse(TEXT("an endless fall ends in a silent respawn"), Falling.IsValid());
	TestEqual(TEXT("no death scored"), B->GetPlayerState<AWoTPlayerState>()->Deaths, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotArenaScenarioSpec, "WoT.Bots.Scenario.TwoBotsTwoPoolsAndKillWithinSixtySeconds", Flags)
bool FBotArenaScenarioSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	Box(W, FVector(-1800, 0, 150), FVector(50, 700, 150));
	Box(W, FVector(1800, 0, 150), FVector(50, 700, 150));
	Box(W, FVector(0, -700, 150), FVector(1800, 50, 150));
	Box(W, FVector(0, 700, 150), FVector(1800, 50, 150));
	const double BuildStarted = FPlatformTime::Seconds();
	UNavigationSystemV1* Nav = BuildNav(W, FVector(1720, 620, 400));
	if (!TestNotNull(TEXT("scenario navigation built"), Nav)) return false;
	if (!TestEqual(TEXT("scenario navigation initially complete"), Nav->GetNumRemainingBuildTasks(), 0)) return false;
	AddInfo(FString::Printf(TEXT("SCENARIO nav_build_ms=%.3f"), (FPlatformTime::Seconds() - BuildStarted) * 1000));
	// The arena geometry is fixed. Combat actors dirty dynamic navigation after
	// the initial build, so asynchronous tile completion can otherwise invalidate
	// paths on different simulated frames depending on worker scheduling. Keep
	// this world's completed mesh fixed for the scenario; the lock dies with W.
	Nav->AddNavigationBuildLock(ENavigationBuildLock::Custom);
	// This timed integration scenario controls its strafe, pool/charge and nav
	// draws explicitly; hit probability is covered by the separate aim spec.
	SeedDecisions(0x7051);
	AWoTBotController* A = Bot(W, FVector(-1600, 0, 50), 0);
	AWoTBotController* B = Bot(W, FVector(1600, 0, 50), 1);
	B->SetControlRotation(FRotator(0, 180, 0));
	Pool(W, FVector(-1300, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Pool(W, FVector(1300, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	bool APicked = false, BPicked = false;
	int32 Frames = 0;
	int32 MaxPendingNavTasks = 0;
	for (int32 I = 0; I < 3600; ++I)
	{
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		++Frames;
		MaxPendingNavTasks = FMath::Max(MaxPendingNavTasks, Nav->GetNumRemainingBuildTasks());
		if (const AWoTCharacter* Pawn = Cast<AWoTCharacter>(A->GetPawn())) APicked |= Pawn->InventoryComponent->Items.Num() > 2;
		if (const AWoTCharacter* Pawn = Cast<AWoTCharacter>(B->GetPawn())) BPicked |= Pawn->InventoryComponent->Items.Num() > 2;
		if (APicked && BPicked && A->GetPlayerState<AWoTPlayerState>()->Kills + B->GetPlayerState<AWoTPlayerState>()->Kills > 0) break;
	}
	TestEqual(TEXT("static scenario navigation stays complete throughout combat"), MaxPendingNavTasks, 0);
	TestTrue(TEXT("first bot collects artifact"), APicked);
	TestTrue(TEXT("second bot collects artifact"), BPicked);
	AddInfo(FString::Printf(TEXT("SCENARIO simulated_seconds=%.3f max_pending_nav_tasks=%d"), Frames / 60.0f, MaxPendingNavTasks));
	for (AWoTBotController* C : { A, B })
		if (const AWoTCharacter* Pawn = Cast<AWoTCharacter>(C->GetPawn()))
		{
			AddInfo(FString::Printf(TEXT("SCENARIO health=%d position=%s visible=%d state=%d"), Pawn->Health,
				*Pawn->GetActorLocation().ToString(), C->IsTargetVisible(), int32(C->GetBehaviourState())));
			for (AWoTInventory* Held : Pawn->InventoryComponent->Items)
				if (const AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held))
					AddInfo(FString::Printf(TEXT("SCENARIO artifact charges=%d uses=%d"), Item->CurCharges, Item->CastUseCount));
		}
	TestTrue(TEXT("real artifact combat scores kill"), A->GetPlayerState<AWoTPlayerState>()->Kills + B->GetPlayerState<AWoTPlayerState>()->Kills > 0);
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
