#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTBotController.h"
#include "WoTBotRosterTestTypes.h"
#include "WoTBotRoster.h"
#include "WoTBotSettings.h"
#include "WoTBotFactionProfile.h"
#include "Tests/WoTTestWorld.h"
#include "WoTCharacter.h"
#include "WoTCollisionBlocker.h"
#include "TimerManager.h"
#include "WoTGameMode.h"
#include "WoTBattleGameMode.h"
#include "WoTFrontEndGameMode.h"
#include "WoTGameInstance.h"
#include "WoTPlayerState.h"
#include "WoTSessionSubsystem.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Engine/Player.h"
#include "Engine/Engine.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/ScopeExit.h"
#include "OnlineSessionSettings.h"

namespace
{
	constexpr EAutomationTestFlags LifecycleTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
	UWorld* BotWorld(int32 Fill, UClass* Mode = AWoTGameMode::StaticClass())
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = Mode;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.URL = FString::Printf(TEXT("/Engine/Maps/Entry?BotFill=%d"), Fill);
		Options.BeforeInitializeActorsForPlay = [](UWorld* World)
		{
			AActor* Floor = World->SpawnActor<AActor>();
			UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
			Floor->SetRootComponent(Box);
			Box->SetBoxExtent(FVector(12000, 12000, 50));
			Box->SetCollisionProfileName(TEXT("BlockAll"));
			Box->RegisterComponent();
			Floor->SetActorLocation(FVector(0, 0, -50));
			for (int32 Index = 0; Index < 20; ++Index)
			{
				World->SpawnActor<APlayerStart>(FVector((Index % 5) * 600, (Index / 5) * 600, 100), FRotator::ZeroRotator);
			}
			// Separate the lifecycle fixture's starts with real sight blockers so
			// combat cannot change the scores/deaths under its roster assertions.
			for (int32 Axis = 0; Axis < 2; ++Axis)
				for (int32 Index = 0; Index < 5; ++Index)
				{
					AActor* Wall = World->SpawnActor<AActor>();
					UBoxComponent* Shape = NewObject<UBoxComponent>(Wall);
					Wall->SetRootComponent(Shape);
					Shape->SetBoxExtent(Axis == 0 ? FVector(10, 4000, 500) : FVector(4000, 10, 500));
					Shape->SetCollisionProfileName(TEXT("BlockAll"));
					Shape->RegisterComponent();
					Wall->SetActorLocation(Axis == 0 ? FVector(Index * 600 + 300, 1200, 400)
						: FVector(1200, Index * 600 + 300, 400));
				}
		};
		return WoTTest::CreateSpecWorld(Options);
	}
	TArray<AWoTBotController*> BotList(UWorld* World)
	{
		TArray<AWoTBotController*> Result;
		for (TActorIterator<AWoTBotController> It(World); It; ++It) Result.Add(*It);
		return Result;
	}
	APlayerController* Human(UWorld* World, bool bNotify = true)
	{
		AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
		APlayerController* Player = World->SpawnActor<APlayerController>(Game->PlayerControllerClass);
		Player->Player = NewObject<UPlayer>(World->GetGameInstance());
		Player->PlayerState->SetPlayerName(TEXT("Human"));
		if (bNotify) Game->PostLogin(Player);
		return Player;
	}
	void Spectator(UWorld* World)
	{
		APlayerController* Player = Human(World, false);
		Player->PlayerState->SetIsSpectator(true);
		Player->PlayerState->SetIsOnlyASpectator(true);
		World->GetAuthGameMode()->PostLogin(Player);
	}
	int32 Advertised(UWorld* World)
	{
		int32 Count = -1;
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
			->BuildSessionSettingsForTesting(FWoTHostRequest()).Get(FName(TEXT("WOTBOTS")), Count);
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotFillPacingSpec, "WoT.Bots.Roster.FillCeilingAndPacing", LifecycleTestFlags)
bool FWoTBotFillPacingSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(100);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UWoTBotRoster* Roster = World->GetSubsystem<UWoTBotRoster>();
	TestTrue(TEXT("Arena roster active"), Roster->IsActive());
	TestEqual(TEXT("dedicated-sized empty world ceiling"), Roster->GetDesiredBotCount(), 16);
	TestEqual(TEXT("first add is paced"), Roster->GetBotCount(), 0);
	WoTTest::TickWorld(World, .4f);
	TestEqual(TEXT("no early add"), Roster->GetBotCount(), 0);
	WoTTest::TickWorld(World, .2f);
	TestEqual(TEXT("one add per interval"), Roster->GetBotCount(), 1);
	WoTTest::TickWorld(World, 8.0f);
	TestEqual(TEXT("sixteen bots maximum"), Roster->GetBotCount(), 16);
	APlayerController* Player = Human(World);
	TestEqual(TEXT("human immediately frees a scoreboard row"), Roster->GetBotCount(), 15);
	TestEqual(TEXT("humans counted"), Roster->GetDesiredBotCount(), 15);
	Player->PlayerState->SetIsInactive(true);
	Roster->Evaluate();
	TestEqual(TEXT("inactive states excluded"), Roster->GetDesiredBotCount(), 16);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotGracefulSpec, "WoT.Bots.Roster.GracefulAndCancelledDepartures", LifecycleTestFlags)
bool FWoTBotGracefulSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(3);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	WoTTest::TickWorld(World, 1.7f);
	TArray<AWoTBotController*> Bots = BotList(World);
	if (!TestEqual(TEXT("three bots"), Bots.Num(), 3)) return false;
	for (int32 I = 0; I < 3; ++I) Bots[I]->GetPlayerState<AWoTPlayerState>()->Kills = I * 10;
	APlayerController* First = Human(World);
	TestTrue(TEXT("lowest score marked"), Bots[0]->IsDeparturePending());
	TestTrue(TEXT("live bot initially marked without immediate retirement"), IsValid(Bots[0]));
	APlayerController* Second = Human(World);
	TestTrue(TEXT("next lowest marked"), Bots[1]->IsDeparturePending());
	Second->Destroy();
	TestTrue(TEXT("oldest mark retained"), Bots[0]->IsDeparturePending());
	TestFalse(TEXT("newest mark cancelled on logout"), Bots[1]->IsDeparturePending());
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bots[0]->GetPawn());
	if (!TestNotNull(TEXT("live pawn"), Pawn)) return false;
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	TestFalse(TEXT("marked participant leaves at death"), IsValid(Bots[0]));
	TestEqual(TEXT("desired two remain"), World->GetSubsystem<UWoTBotRoster>()->GetBotCount(), 2);
	First->Destroy();
	WoTTest::TickWorld(World, .6f);
	TestEqual(TEXT("cancelled excess is refilled"), World->GetSubsystem<UWoTBotRoster>()->GetBotCount(), 3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotTimeoutSpec, "WoT.Bots.Roster.DeadAndTimeoutRetirement", LifecycleTestFlags)
bool FWoTBotTimeoutSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(2);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	WoTTest::TickWorld(World, 1.2f);
	TArray<AWoTBotController*> Bots = BotList(World);
	if (!TestEqual(TEXT("two bots"), Bots.Num(), 2)) return false;
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bots[1]->GetPawn());
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	const TWeakObjectPtr<AWoTCharacter> RetiredCorpse = Pawn;
	Human(World);
	TestFalse(TEXT("dead bot retires immediately"), IsValid(Bots[1]));
	TestFalse(TEXT("retired bot's dead body is removed"), RetiredCorpse.IsValid());
	AWoTBotRosterTestViewer* Watcher = World->SpawnActor<AWoTBotRosterTestViewer>();
	Watcher->Player = NewObject<UPlayer>(World->GetGameInstance());
	Watcher->ViewLocation = Bots[0]->GetPawn()->GetActorLocation() + FVector(-100, 0, 20);
	World->GetAuthGameMode()->PostLogin(Watcher);
	TestTrue(TEXT("remaining live bot marked"), Bots[0]->IsDeparturePending());
	WoTTest::TickWorld(World, 59.7f);
	TestTrue(TEXT("not before timeout"), IsValid(Bots[0]));
	WoTTest::TickWorld(World, .5f);
	TestFalse(TEXT("timeout retires"), IsValid(Bots[0]));
	WoTTest::TickWorld(World, 2.1f);
	TestEqual(TEXT("count ends on zero"), Advertised(World), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotIdentitySpec, "WoT.Bots.Roster.IdentityRoundRobinAndRespawnKit", LifecycleTestFlags)
bool FWoTBotIdentitySpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(8);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Spectator(World);
	WoTTest::TickWorld(World, 4.2f);
	const TArray<AWoTBotController*> Bots = BotList(World);
	if (!TestEqual(TEXT("eight bots"), Bots.Num(), 8)) return false;
	TMap<UClass*, int32> Counts;
	TSet<FString> Names;
	for (AWoTBotController* Bot : Bots)
	{
		AWoTPlayerState* State = Bot->GetPlayerState<AWoTPlayerState>();
		TestTrue(TEXT("bot flag"), State->IsABot());
		++Counts.FindOrAdd(State->FactionClass.Get());
		Names.Add(State->GetPlayerName());
		TestTrue(TEXT("runtime class name prefix"), State->GetPlayerName().StartsWith(WoTBotProfiles::DisplayName(State->FactionClass.Get()) + TEXT(" ")));
	}
	TestEqual(TEXT("all four classes"), Counts.Num(), 4);
	for (const auto& Pair : Counts) TestEqual(TEXT("round robin balance"), Pair.Value, 2);
	TestEqual(TEXT("unique names"), Names.Num(), 8);
	AWoTBotController* Bot = Bots[0];
	AWoTPlayerState* State = Bot->GetPlayerState<AWoTPlayerState>();
	const FString Name = State->GetPlayerName();
	UClass* Class = State->FactionClass;
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bot->GetPawn());
	if (!TestNotNull(TEXT("spawned player body"), Pawn)) return false;
	TestEqual(TEXT("player health"), Pawn->Health, 100);
	const int32 KitSize = Pawn->InventoryComponent->Items.Num();
	TestEqual(TEXT("human kit, no caster loadout"), KitSize, 2);
	TestFalse(TEXT("no caster kit"), Pawn->FindComponentByClass<UWoTCasterComponent>()->bBuildLoadout);
	TestTrue(TEXT("acceleration path setting"), Pawn->GetCharacterMovement()->GetNavMovementProperties()->bUseAccelerationForPaths);
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	const TWeakObjectPtr<AWoTCharacter> Corpse = Pawn;
	WoTTest::TickWorld(World, .9f);
	TestNull(TEXT("delay respected"), Bot->GetPawn());
	TestTrue(TEXT("dead body kept until the respawn"), Corpse.IsValid());
	WoTTest::TickWorld(World, 1.2f);
	AWoTCharacter* Replacement = Cast<AWoTCharacter>(Bot->GetPawn());
	if (!TestNotNull(TEXT("respawned"), Replacement)) return false;
	TestTrue(TEXT("new body"), Replacement != Pawn);
	TestFalse(TEXT("dead body consumed at respawn"), Corpse.IsValid());
	TestEqual(TEXT("same kit size"), Replacement->InventoryComponent->Items.Num(), KitSize);
	TestEqual(TEXT("name retained"), State->GetPlayerName(), Name);
	TestTrue(TEXT("class retained"), Replacement->GetClass() == Class);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotSilentFrozenSpec, "WoT.Bots.Controller.SilentRespawnAndMatchFreeze", LifecycleTestFlags)
bool FWoTBotSilentFrozenSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(2);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Spectator(World);
	WoTTest::TickWorld(World, 1.2f);
	const auto Bots = BotList(World);
	if (!TestEqual(TEXT("two bots"), Bots.Num(), 2)) return false;
	AWoTBotController* Bot = Bots[0];
	AWoTPlayerState* State = Bot->GetPlayerState<AWoTPlayerState>();
	State->Kills = 4;
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bot->GetPawn());
	const auto Items = Pawn->InventoryComponent->Items;
	Bot->SilentRespawn();
	TestFalse(TEXT("old live body destroyed"), IsValid(Pawn));
	TestEqual(TEXT("no deaths"), State->Deaths, 0);
	TestEqual(TEXT("no suicides"), State->Suicides, 0);
	TestEqual(TEXT("score retained"), State->CalcScore(), 4);
	for (AWoTInventory* Item : Items) TestFalse(TEXT("held items destroyed, not dropped"), IsValid(Item));
	World->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
	UWoTBotRoster* Roster = World->GetSubsystem<UWoTBotRoster>();
	TestTrue(TEXT("roster frozen"), Roster->IsFrozen());
	for (AWoTBotController* Current : Bots) TestTrue(TEXT("controllers frozen"), Current->IsFrozen());
	Human(World, false);
	WoTTest::TickWorld(World, 4.0f);
	TestNull(TEXT("no pending respawn fires"), Bot->GetPawn());
	TestEqual(TEXT("no departure after end"), Roster->GetBotCount(), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotPrioritySummonsSpec, "WoT.Bots.Controller.PriorityAndOwnedSummons", LifecycleTestFlags)
bool FWoTBotPrioritySummonsSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(1);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Spectator(World);
	WoTTest::TickWorld(World, .7f);
	const auto Bots = BotList(World);
	if (!TestEqual(TEXT("one bot"), Bots.Num(), 1)) return false;
	AWoTBotController* Bot = Bots[0];
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bot->GetPawn());
	const auto* Profile = WoTBotProfiles::Get().Find(Pawn->GetClass());
	if (!TestNotNull(TEXT("profile"), Profile)) return false;
	AWoTAngrealInventory* Item = nullptr;
	for (AWoTInventory* Held : Pawn->InventoryComponent->Items)
	{
		if (Profile->Priorities.Contains(Held->GetClass())) { Item = Cast<AWoTAngrealInventory>(Held); break; }
	}
	if (!TestNotNull(TEXT("profile has kit item"), Item)) return false;
	TestEqual(TEXT("profile applied"), Item->Priority, Profile->Priorities.FindChecked(Item->GetClass()));
	Pawn->InventoryComponent->DeleteInventory(Item);
	TestEqual(TEXT("drop restores native priority"), Item->Priority, Item->GetClass()->GetDefaultObject<AWoTAngrealInventory>()->Priority);
	Pawn->InventoryComponent->AddInventory(Item);
	WoTTest::TickWorld(World, .2f);
	TestEqual(TEXT("pickup reapplies preference"), Item->Priority, Profile->Priorities.FindChecked(Item->GetClass()));
	AWoTNPCCharacter* Summon = World->SpawnActor<AWoTNPCCharacter>(FVector(6000, 6000, 100), FRotator::ZeroRotator);
	Summon->SummonOwner = Pawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(FVector(6000, 7000, 100), FRotator::ZeroRotator);
	Guide->SummonOwner = Pawn;
	AWoTNPCCharacter* Unowned = World->SpawnActor<AWoTNPCCharacter>(FVector(7000, 6000, 100), FRotator::ZeroRotator);
	AWoTCharacter* OtherBody = World->SpawnActor<AWoTCharacter>(FVector(8000, 6000, 100), FRotator::ZeroRotator);
	AWoTNPCCharacter* OtherSummon = World->SpawnActor<AWoTNPCCharacter>(FVector(9000, 6000, 100), FRotator::ZeroRotator);
	OtherSummon->SummonOwner = OtherBody;
	OtherBody->Destroy();
	Bot->SilentRespawn();
	Human(World);
	TestFalse(TEXT("departed bot"), IsValid(Bot));
	TestFalse(TEXT("prior body's live summon retired"), IsValid(Summon));
	TestFalse(TEXT("prior body's projectile helper retired"), IsValid(Guide));
	TestTrue(TEXT("unowned NPC survives bot retirement"), IsValid(Unowned));
	TestTrue(TEXT("another destroyed body's summon survives"), IsValid(OtherSummon));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotOptionsSpec, "WoT.Bots.Roster.OptionsAndInactiveWorlds", LifecycleTestFlags)
bool FWoTBotOptionsSpec::RunTest(const FString&)
{
	UWoTBotSettings* Settings = GetMutableDefault<UWoTBotSettings>();
	const int32 Previous = Settings->DefaultBotFill;
	Settings->DefaultBotFill = 7;
	ON_SCOPE_EXIT { Settings->DefaultBotFill = Previous; };
	{
		UWorld* World = BotWorld(0);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TestEqual(TEXT("URL zero wins over config"), World->GetSubsystem<UWoTBotRoster>()->GetDesiredBotCount(), 0);
	}
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = AWoTGameMode::StaticClass();
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TestEqual(TEXT("absent URL uses config"), World->GetSubsystem<UWoTBotRoster>()->GetDesiredBotCount(), 7);
	}
	for (UClass* Mode : { AWoTBattleGameMode::StaticClass(), AWoTFrontEndGameMode::StaticClass() })
	{
		UWorld* World = BotWorld(16, Mode);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TestFalse(TEXT("non-Arena inactive"), World->GetSubsystem<UWoTBotRoster>()->IsActive());
		WoTTest::TickWorld(World, 2.1f);
		TestEqual(TEXT("no bots"), BotList(World).Num(), 0);
		TestEqual(TEXT("inactive world clears advertised count"), Advertised(World), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotCountSpec, "WoT.Bots.Roster.SessionCountCoalesced", LifecycleTestFlags)
bool FWoTBotCountSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(4);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	TestEqual(TEXT("initial zero"), Advertised(World), 0);
	WoTTest::TickWorld(World, 1.8f);
	TestEqual(TEXT("initial publication held for two seconds"), Advertised(World), 0);
	WoTTest::TickWorld(World, .3f);
	const int32 Count = Advertised(World);
	TestTrue(TEXT("latest at first deadline"), Count >= 3 && Count <= 4);
	WoTTest::TickWorld(World, 2.0f);
	TestEqual(TEXT("last addition eventually advertised"), Advertised(World), 4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotBlockedStartSpec, "WoT.Bots.Controller.BlockedStartRetriesEverySecond", LifecycleTestFlags)
bool FWoTBotBlockedStartSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(1);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Spectator(World);
	WoTTest::TickWorld(World, .7f);
	const auto Bots = BotList(World);
	if (!TestEqual(TEXT("one bot"), Bots.Num(), 1)) return false;
	AWoTBotController* Bot = Bots[0];
	Bot->SilentRespawn();
	TArray<AWoTCollisionBlocker*> Blockers;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		const FTransform Transform(It->GetActorLocation());
		AWoTCollisionBlocker* Blocker = World->SpawnActorDeferred<AWoTCollisionBlocker>(AWoTCollisionBlocker::StaticClass(), Transform);
		Blocker->Configure(EWoTCollisionBlockerMode::Actor, 40.0f, 60.0f);
		Blocker->FinishSpawning(Transform);
		Blockers.Add(Blocker);
	}
	Bot->Restart();
	TestNull(TEXT("every start blocked"), Bot->GetPawn());
	WoTTest::TickWorld(World, 1.1f);
	TestNull(TEXT("first retry still blocked"), Bot->GetPawn());
	for (AWoTCollisionBlocker* Blocker : Blockers) Blocker->Destroy();
	WoTTest::TickWorld(World, .7f);
	TestNull(TEXT("not retried more frequently than one second"), Bot->GetPawn());
	WoTTest::TickWorld(World, .3f);
	TestTrue(TEXT("next one second retry succeeds"), IsValid(Bot->GetPawn()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotCapacityOrderSpec, "WoT.Bots.Roster.CapacityPrefersDeadThenLowestScore", LifecycleTestFlags)
bool FWoTBotCapacityOrderSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(16);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	WoTTest::TickWorld(World, 8.7f);
	const auto Bots = BotList(World);
	if (!TestEqual(TEXT("full roster"), Bots.Num(), 16)) return false;
	for (int32 Index = 0; Index < Bots.Num(); ++Index) Bots[Index]->GetPlayerState<AWoTPlayerState>()->Kills = Index;
	AWoTCharacter* Body = Cast<AWoTCharacter>(Bots.Last()->GetPawn());
	if (!TestNotNull(TEXT("body"), Body)) return false;
	Body->Died(nullptr, NAME_None, Body->GetActorLocation());
	Human(World);
	TestFalse(TEXT("dead high scorer leaves before live low scorer"), IsValid(Bots.Last()));
	TestTrue(TEXT("low scorer retained while dead candidate exists"), IsValid(Bots[0]));
	Human(World);
	TestFalse(TEXT("lowest live score retires immediately next"), IsValid(Bots[0]));
	TestEqual(TEXT("sixteen participants"), World->GetGameState()->PlayerArray.Num(), 16);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotTravelCountSpec, "WoT.Bots.Roster.TravelClearsCountAndRejectsOldWorld", LifecycleTestFlags)
bool FWoTBotTravelCountSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(3);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	WoTTest::TickWorld(World, 2.1f);
	if (!TestEqual(TEXT("old world advertises three"), Advertised(World), 3)) return false;
	UGameInstance* Instance = World->GetGameInstance();
	FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	UWorld* Replacement = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("replacement"), Replacement) || !TestNotNull(TEXT("context"), Context)) return false;
	Replacement->SetGameInstance(Instance);
	Context->SetCurrentWorld(Replacement);
	ON_SCOPE_EXIT
	{
		Replacement->BeginTearingDown();
		Replacement->EndPlay(EEndPlayReason::LevelTransition);
		Context->SetCurrentWorld(World);
		Replacement->SetGameInstance(nullptr);
		Replacement->DestroyWorld(false);
	};
	Replacement->GetWorldSettings()->DefaultGameMode = AWoTFrontEndGameMode::StaticClass();
	FURL URL;
	Replacement->SetGameMode(URL);
	Replacement->InitializeActorsForPlay(URL);
	Replacement->BeginPlay();
	TestEqual(TEXT("next inactive world publishes zero into same session"), Advertised(Replacement), 0);
	// Create a pending old-world publication, then let its timer run after travel.
	BotList(World)[0]->Retire();
	World->GetSubsystem<UWoTBotRoster>()->Freeze();
	++GFrameCounter;
	World->GetTimerManager().Tick(2.2f);
	TestEqual(TEXT("outgoing world cannot overwrite successor"), Advertised(Replacement), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotPendingFreezeSpec, "WoT.Bots.Roster.FreezeCancelsAddAndDepartureTimers", LifecycleTestFlags)
bool FWoTBotPendingFreezeSpec::RunTest(const FString&)
{
	{
		UWorld* World = BotWorld(4);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		World->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
		WoTTest::TickWorld(World, 2.2f);
		TestEqual(TEXT("empty roster adds nothing after match end"), BotList(World).Num(), 0);
		TestTrue(TEXT("empty roster freezes too"), World->GetSubsystem<UWoTBotRoster>()->IsFrozen());
	}
	{
		UWorld* World = BotWorld(2);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		WoTTest::TickWorld(World, 1.2f);
		Human(World);
		World->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
		WoTTest::TickWorld(World, 60.5f);
		TestEqual(TEXT("marked departure timeout cancelled"), BotList(World).Num(), 2);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotSilentSealSpec, "WoT.Bots.Controller.SilentRespawnDestroysEveryHeldItem", LifecycleTestFlags)
bool FWoTBotSilentSealSpec::RunTest(const FString&)
{
	UWorld* World = BotWorld(1);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Spectator(World);
	WoTTest::TickWorld(World, .7f);
	const auto Bots = BotList(World);
	if (!TestEqual(TEXT("one bot"), Bots.Num(), 1)) return false;
	AWoTCharacter* Body = Cast<AWoTCharacter>(Bots[0]->GetPawn());
	UClass* ItemClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTSeal"));
	if (!TestNotNull(TEXT("special held item class"), ItemClass)) return false;
	const FTransform Transform(FVector(9000, 9000, 200));
	AWoTInventory* Item = World->SpawnActorDeferred<AWoTInventory>(ItemClass, Transform, Body, Body,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Item->bHeldItem = true;
	Item->FinishSpawning(Transform);
	Item->GiveTo(Body);
	TestTrue(TEXT("special item held"), Body->InventoryComponent->Items.Contains(Item));
	Bots[0]->SilentRespawn();
	TestFalse(TEXT("special item cannot escape through pawn teardown"), IsValid(Item));
	WoTTest::TickWorld(World, 2.1f);
	TestTrue(TEXT("silent replacement arrives"), IsValid(Bots[0]->GetPawn()));
	TestEqual(TEXT("silent replacement has no death score"), Bots[0]->GetPlayerState<AWoTPlayerState>()->Deaths, 0);
	return !HasAnyErrors();
}

#endif
