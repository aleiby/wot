#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestBotParticipant.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTPlayerController.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameState.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTCollisionBlocker.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr EAutomationTestFlags BotRulesFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	AWoTCharacter* BotRulesPawn(UWorld* World, AController* Controller, float X = 0)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(X, 0, 100), FRotator::ZeroRotator, Params);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Controller->Possess(Pawn);
		return Pawn;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotDeathDisposalSpec,
	"WoT.Rules.BotDeathDisposal", BotRulesFlags)
bool FWoTBotDeathDisposalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	AWoTPlayerState* State = Bot->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("bot has the game player-state class"), State)) return false;
	TestTrue(TEXT("test participant is bot flagged"), State->IsABot());
	AWoTCharacter* Pawn = BotRulesPawn(World, Bot);
	Game->RespawnDelay = 2.25f;
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	TestNull(TEXT("bot released corpse"), Bot->GetPawn());
	TestTrue(TEXT("unpossession precedes callback"), Bot->bUnpossessedAtCallback);
	TestEqual(TEXT("one death notification"), Bot->PawnDiedCalls, 1);
	TestEqual(TEXT("configured delay forwarded"), Bot->LastRespawnDelay, 2.25f);
	TestTrue(TEXT("callback identifies corpse"), Bot->LastDeadPawn == Pawn);
	TestTrue(TEXT("persistent controller and state survive death"), IsValid(Bot) && IsValid(State));
	Pawn->Destroy();
	TestTrue(TEXT("corpse destruction retains controller"), IsValid(Bot));
	TestTrue(TEXT("corpse destruction retains original state"), IsValid(State) && Bot->PlayerState == State);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotDeathNonParticipantParitySpec,
	"WoT.Rules.BotDeathNonParticipantParity", BotRulesFlags)
bool FWoTBotDeathNonParticipantParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTNPCCharacter* NPCPawn = World->SpawnActor<AWoTNPCCharacter>();
	AWoTNPCController* NPC = Cast<AWoTNPCController>(NPCPawn->GetController());
	if (!TestNotNull(TEXT("NPC owns its normal AI controller"), NPC)) return false;
	Game->PlayerKilled(nullptr, NPCPawn, NAME_None);
	TestTrue(TEXT("NPC keeps corpse for its ordinary lifecycle"), NPC->GetPawn() == NPCPawn);
	TestNull(TEXT("NPC has no player state"), NPC->PlayerState.Get());
	AWoTPlayerController* Human = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* HumanPawn = BotRulesPawn(World, Human, 500);
	Game->PlayerKilled(nullptr, HumanPawn, NAME_None);
	TestNull(TEXT("human uses existing unpossession path"), Human->GetPawn());
	AWoTTestBotParticipant* Unflagged = World->SpawnActor<AWoTTestBotParticipant>();
	Unflagged->PlayerState->SetIsABot(false);
	AWoTCharacter* UnflaggedPawn = BotRulesPawn(World, Unflagged, 1000);
	Game->PlayerKilled(nullptr, UnflaggedPawn, NAME_None);
	TestTrue(TEXT("unflagged AI keeps existing path"), Unflagged->GetPawn() == UnflaggedPawn);
	TestEqual(TEXT("unflagged AI gets no callback"), Unflagged->PawnDiedCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotTeamsSpec,
	"WoT.Rules.BotTeams", BotRulesFlags)
bool FWoTBotTeamsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TArray<AController*> Players = { World->SpawnActor<AWoTPlayerController>(),
		World->SpawnActor<AWoTTestBotParticipant>(), World->SpawnActor<AWoTPlayerController>(),
		World->SpawnActor<AWoTTestBotParticipant>() };
	for (int32 Index = 0; Index < Players.Num(); ++Index)
	{
		Game->ChangeTeam(Players[Index], 99);
		TestEqual(TEXT("smallest free participant team"),
			int32(Players[Index]->GetPlayerState<AWoTPlayerState>()->Team), Index);
	}
	Game->ChangeTeam(Players[1], 99);
	TestEqual(TEXT("own state excluded when reassigning"),
		int32(Players[1]->GetPlayerState<AWoTPlayerState>()->Team), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotMatchEndedSpec,
	"WoT.Rules.BotMatchEnded", BotRulesFlags)
bool FWoTBotMatchEndedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	AWoTTestBotParticipant* Other = World->SpawnActor<AWoTTestBotParticipant>();
	Other->PlayerState->SetIsABot(false);
	AWoTPlayerController* Human = World->SpawnActor<AWoTPlayerController>();
	BotRulesPawn(World, Human);
	Game->EndGame(TEXT("test"));
	Game->EndGame(TEXT("test"));
	TestEqual(TEXT("bot match notification once"), Bot->MatchEndedCalls, 1);
	TestEqual(TEXT("interface notification is generic"), Other->MatchEndedCalls, 1);
	TestTrue(TEXT("human notification retained"), Human->IsMatchEnded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotUnattendedRestartSpec,
	"WoT.Rules.BotUnattendedRestart", BotRulesFlags)
bool FWoTBotUnattendedRestartSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTTestBotRulesGameMode* Game = World->GetAuthGameMode<AWoTTestBotRulesGameMode>();
	World->SpawnActor<AWoTTestBotParticipant>();
	Game->TimeLimit = 0;
	Game->EndGame(TEXT("test"));
	WoTTest::TickWorld(World, 14.8f);
	Game->Timer();
	TestEqual(TEXT("no early unattended restart"), Game->RestartCalls, 0);
	WoTTest::TickWorld(World, 0.3f);
	Game->Timer();
	Game->Timer();
	TestEqual(TEXT("one restart after fifteen seconds"), Game->RestartCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotHumanRestartParitySpec,
	"WoT.Rules.BotHumanRestartParity", BotRulesFlags)
bool FWoTBotHumanRestartParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTTestBotRulesGameMode* Game = World->GetAuthGameMode<AWoTTestBotRulesGameMode>();
	World->SpawnActor<AWoTTestBotParticipant>();
	AWoTPlayerController* Human = World->SpawnActor<AWoTPlayerController>();
	BotRulesPawn(World, Human);
	Game->TimeLimit = 0;
	Game->EndGame(TEXT("test"));
	WoTTest::TickWorld(World, 16.1f);
	Game->Timer();
	TestEqual(TEXT("human presence retains fire-driven restart"), Game->RestartCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotRestartKitSpec,
	"WoT.Rules.BotRestartKitAndFailedStart", BotRulesFlags)
bool FWoTBotRestartKitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->GetPlayerState<AWoTPlayerState>()->SetFactionClass(AWoTHound::StaticClass());
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(500, 0, 100), FRotator::ZeroRotator);
	const FTransform BlockTransform(Start->GetActorLocation());
	AWoTCollisionBlocker* Blocker = World->SpawnActorDeferred<AWoTCollisionBlocker>(
		AWoTCollisionBlocker::StaticClass(), BlockTransform);
	Blocker->Configure(EWoTCollisionBlockerMode::Actor, 40.0f, 60.0f);
	Blocker->FinishSpawning(BlockTransform);
	Game->RestartPlayer(Bot);
	TestNull(TEXT("rejected start is signalled by no possessed pawn"), Bot->GetPawn());
	Blocker->Destroy();
	Game->RestartPlayer(Bot);
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Bot->GetPawn());
	if (!TestNotNull(TEXT("retry possesses a pawn"), Pawn)) return false;
	TestTrue(TEXT("respawn uses participant class"), Pawn->IsA<AWoTHound>());
	TestTrue(TEXT("respawn placed at player start"), Pawn->GetActorLocation().Equals(Start->GetActorLocation(), 1));
	AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(
		Pawn->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass()));
	if (TestNotNull(TEXT("human starting item granted"), Item))
		TestEqual(TEXT("human starting charges"), Item->CurCharges, 25);
	TestNotNull(TEXT("class default item granted"),
		Pawn->InventoryComponent->FindInventoryType(Pawn->DefaultAngrealInventory));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotPoolPacingSpec,
	"WoT.Inventory.BotCombatantPoolPacing", BotRulesFlags)
bool FWoTBotPoolPacingSpec::RunTest(const FString&)
{
	for (int32 BotCount : { 0, 3 })
	{
		UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTPlayerController* Human = World->SpawnActor<AWoTPlayerController>();
		AWoTCharacter* Pawn = BotRulesPawn(World, Human);
		for (int32 Index = 0; Index < BotCount; ++Index)
			World->SpawnActor<AWoTTestBotParticipant>();
		TestEqual(TEXT("connection capacity still counts only the human"),
			World->GetAuthGameMode()->GetNumPlayers(), 1);
		TestEqual(TEXT("combatant count includes bot player states"),
			World->GetAuthGameMode<AWoTGameMode>()->GetNumCombatants(), 1 + BotCount);
		UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
		Table->IdealPlayerCount = 2;
		UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table);
		const FTransform Transform(FVector(500, 0, 100));
		AWoTAngrealSpawnPool* Pool = World->SpawnActorDeferred<AWoTAngrealSpawnPool>(
			AWoTAngrealSpawnPool::StaticClass(), Transform);
		Pool->Pool = { AWoTAngrealInvDart::StaticClass() };
		Pool->Weight = { 1 };
		Pool->RespawnTime = 1.6f;
		Pool->FinishSpawning(Transform);
		WoTTest::PlaceWithoutOverlapEvents(Pawn, Transform.GetLocation());
		Pool->OnPickupOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
		TestNull(TEXT("pool handed its item to the human"), Pool->PickupItem.Get());
		WoTTest::PlaceWithoutOverlapEvents(Pawn, FVector(0, 0, 100));
		WoTTest::TickWorld(World, 1.2f);
		if (BotCount == 3)
			TestNotNull(TEXT("one human plus three bots replenishes after 1.1 seconds"), Pool->PickupItem.Get());
		else
			TestNull(TEXT("one human alone still waits 1.9 seconds"), Pool->PickupItem.Get());
		WoTTest::TickWorld(World, 0.8f);
		TestNotNull(TEXT("human-only original delay still replenishes"), Pool->PickupItem.Get());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotAbsentRestartParitySpec,
	"WoT.Rules.BotAbsentRestartParity", BotRulesFlags)
bool FWoTBotAbsentRestartParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTTestBotRulesGameMode* Game = World->GetAuthGameMode<AWoTTestBotRulesGameMode>();
	World->SpawnActor<AWoTNPCCharacter>();
	Game->EndGame(TEXT("test"));
	WoTTest::TickWorld(World, 16.1f);
	Game->Timer();
	TestEqual(TEXT("NPC-only game has no new unattended restart"), Game->RestartCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotFragLimitDeathSpec,
	"WoT.Rules.BotFragLimitDeathKeepsEndView", BotRulesFlags)
bool FWoTBotFragLimitDeathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTTestBotRulesGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTTestBotParticipant* Killer = World->SpawnActor<AWoTTestBotParticipant>();
	AWoTTestBotParticipant* Victim = World->SpawnActor<AWoTTestBotParticipant>();
	Game->ChangeTeam(Killer, 255);
	Game->ChangeTeam(Victim, 255);
	BotRulesPawn(World, Killer);
	AWoTCharacter* Pawn = BotRulesPawn(World, Victim, 500);
	Game->FragLimit = 1;
	Game->PlayerKilled(Killer, Pawn, NAME_None);
	TestTrue(TEXT("bot frag decides the match"), Game->bGameEnded);
	TestEqual(TEXT("frag-limit death does not arm a respawn"), Victim->PawnDiedCalls, 0);
	TestTrue(TEXT("frag-limit victim retains end-view body"), Victim->GetPawn() == Pawn);
	TestEqual(TEXT("bot killer received ordinary score"), Killer->GetPlayerState<AWoTPlayerState>()->Kills, 1);
	TestEqual(TEXT("both participants notified"), Killer->MatchEndedCalls + Victim->MatchEndedCalls, 2);
	return true;
}

#endif
