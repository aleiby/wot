#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestBotParticipant.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTBagHolding.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAbsorbReflector.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerState.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags BotInventoryFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

AWoTCharacter* SpawnBotInventoryPawn(UWorld* World, AController* Controller,
	const FVector& Location)
{
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Controller->Possess(Pawn);
	return Pawn;
}

AWoTAngrealInvFireball* GiveBotInventoryItem(UWorld* World, AWoTCharacter* Pawn)
{
	AWoTAngrealInvFireball* Item = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(5000, 0, 200), FRotator::ZeroRotator);
	Item->GiveTo(Pawn);
	Item->Priority = 13.25f;
	return Item;
}

#if WITH_EDITOR
UE::Net::FTestWorldInstance CreateBotInventoryListenServer()
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_ListenServer;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	++PIEInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"),
			TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*GameInstance->GetWorldContext(), URL, Error) == EBrowseReturnVal::Success;
	}
	return Instance;
}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotBagClearsVictimSpec,
	"WoT.Inventory.Bot.BagClearsVictim", BotInventoryFlags)
bool FWoTInventoryBotBagClearsVictimSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	APlayerController* Human = World->SpawnActor<APlayerController>();
	Human->InitPlayerState();
	AWoTCharacter* Looter = SpawnBotInventoryPawn(World, Human, FVector(900, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Looter);
	Pawn->InventoryComponent->SelectItem(Item);
	Pawn->Drop();
	AWoTBagHolding* Bag = nullptr;
	for (TActorIterator<AWoTBagHolding> It(World); It; ++It)
	{
		if (IsValid(*It)) Bag = *It;
	}
	if (TestNotNull(TEXT("bot drop creates a bag"), Bag))
	{
		TestNull(TEXT("victim is cleared before a looter receives the item"), Item->GetVictim());
		Bag->GiveTo(Looter);
		WoTTest::TickWorld(World, 0.05f);
		TestTrue(TEXT("human receives the bot's item"), Looter->InventoryComponent->Items.Contains(Item));
		TestNull(TEXT("human inherits no AI victim"), Item->GetVictim());
		TestEqual(TEXT("no priority override preserves the instance priority"), Item->Priority, 13.25f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotDirectDropClearsVictimSpec,
	"WoT.Inventory.Bot.DirectDropClearsVictim", BotInventoryFlags)
bool FWoTInventoryBotDirectDropClearsVictimSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Pawn);
	Item->DropFrom(FVector(500, 0, 200));
	TestFalse(TEXT("drop removes the item"), Pawn->InventoryComponent->Items.Contains(Item));
	TestNull(TEXT("direct drop clears the bot victim"), Item->GetVictim());
	TestEqual(TEXT("direct drop preserves unmodified instance priority"), Item->Priority, 13.25f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotRemovalPreservesOtherHoldersSpec,
	"WoT.Inventory.Bot.RemovalPreservesOtherHolders", BotInventoryFlags)
bool FWoTInventoryBotRemovalPreservesOtherHoldersSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	APlayerController* Human = World->SpawnActor<APlayerController>();
	Human->InitPlayerState();
	AWoTCharacter* HumanPawn = SpawnBotInventoryPawn(World, Human, FVector(0, 0, 200));
	AWoTAngrealInvFireball* HumanItem = GiveBotInventoryItem(World, HumanPawn);
	HumanItem->SetVictim(HumanPawn);
	HumanPawn->InventoryComponent->DeleteInventory(HumanItem);
	TestTrue(TEXT("a human-held victim is untouched"), HumanItem->GetVictim() == HumanPawn);
	TestEqual(TEXT("a human-held priority is untouched"), HumanItem->Priority, 13.25f);

	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(900, 0, 200), FRotator::ZeroRotator);
	AWoTAngrealInvFireball* NPCItem = GiveBotInventoryItem(World, NPC);
	NPCItem->SetVictim(HumanPawn);
	NPC->InventoryComponent->DeleteInventory(NPCItem);
	TestTrue(TEXT("an NPC-held victim is untouched"), NPCItem->GetVictim() == HumanPawn);
	TestEqual(TEXT("an NPC-held priority is untouched"), NPCItem->Priority, 13.25f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotPriorityOverrideSpec,
	"WoT.Inventory.Bot.PriorityOverrideRestoresSavedValue", BotInventoryFlags)
bool FWoTInventoryBotPriorityOverrideSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Pawn);
	Item->SetBotPriorityOverride(81.0f);
	TestEqual(TEXT("first override affects AI selection priority"), Item->Priority, 81.0f);
	Item->SetBotPriorityOverride(92.0f);
	TestEqual(TEXT("repeated override updates selection priority"), Item->Priority, 92.0f);
	Pawn->Drop();
	TestNull(TEXT("bagging clears target independently of priority"), Item->GetVictim());
	TestEqual(TEXT("bagging restores the value from before the first override"), Item->Priority, 13.25f);
	Item->Priority = 26.5f;
	Item->ClearBotAIState();
	TestEqual(TEXT("a repeated clear does not restore an obsolete baseline"), Item->Priority, 26.5f);
	Item->SetBotPriorityOverride(7.0f);
	Item->ClearBotAIState();
	TestEqual(TEXT("a new override cycle captures the new instance priority"), Item->Priority, 26.5f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotBagRestoresPriorityForHumanSpec,
	"WoT.Inventory.Bot.BagRestoresPriorityForHuman", BotInventoryFlags)
bool FWoTInventoryBotBagRestoresPriorityForHumanSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	APlayerController* Human = World->SpawnActor<APlayerController>();
	Human->InitPlayerState();
	AWoTCharacter* Looter = SpawnBotInventoryPawn(World, Human, FVector(900, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Looter);
	Item->SetBotPriorityOverride(89.0f);
	Pawn->Drop();
	AWoTBagHolding* Bag = nullptr;
	for (TActorIterator<AWoTBagHolding> It(World); It; ++It)
	{
		if (IsValid(*It)) Bag = *It;
	}
	if (TestNotNull(TEXT("bot bag exists"), Bag))
	{
		Bag->GiveTo(Looter);
		WoTTest::TickWorld(World, 0.05f);
		TestTrue(TEXT("human holds the original item"), Item->GetOwner() == Looter);
		TestNull(TEXT("human has no inherited bot target"), Item->GetVictim());
		TestEqual(TEXT("human gets the exact pre-override priority"), Item->Priority, 13.25f);
		Item->SetVictim(Looter);
		Looter->InventoryComponent->DeleteInventory(Item);
		TestTrue(TEXT("subsequent human removal preserves its own target"), Item->GetVictim() == Looter);
		TestEqual(TEXT("subsequent human removal preserves priority"), Item->Priority, 13.25f);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotEmptyPuzzleCleanupSpec,
	"WoT.Inventory.Bot.EmptyPuzzleCleanup", BotInventoryFlags)
bool FWoTInventoryBotEmptyPuzzleCleanupSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Pawn);
	Item->SetBotPriorityOverride(89.0f);
	Item->bPuzzleRespawn = true;
	Item->GoEmpty();
	TestFalse(TEXT("empty item leaves bot inventory"), Pawn->InventoryComponent->Items.Contains(Item));
	TestEqual(TEXT("puzzle item returns to pickup state"), Item->GetItemState(), EWoTInventoryState::Pickup);
	TestNull(TEXT("returned puzzle pickup has no bot victim"), Item->GetVictim());
	TestEqual(TEXT("returned puzzle pickup restores exact priority"), Item->Priority, 13.25f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotTheftCleanupSpec,
	"WoT.Inventory.Bot.TheftCleanup", BotInventoryFlags)
bool FWoTInventoryBotTheftCleanupSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	APlayerController* Human = World->SpawnActor<APlayerController>();
	Human->InitPlayerState();
	AWoTCharacter* Receiver = SpawnBotInventoryPawn(World, Human, FVector(900, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Receiver);
	Item->SetBotPriorityOverride(89.0f);
	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(Receiver);
	Absorb->Install(Receiver);
	FWoTDamageEffectSpec Effect;
	Effect.SourceAngreal = Item;
	Effect.bDeleterious = true;
	TestTrue(TEXT("the item effect is absorbed"), Absorb->ConsumesEffect(Effect));
	TestFalse(TEXT("theft removes the bot's item"), Pawn->InventoryComponent->Items.Contains(Item));
	TestTrue(TEXT("human receives the original stolen item"), Item->GetOwner() == Receiver);
	TestNull(TEXT("theft clears the bot victim before transfer"), Item->GetVictim());
	TestEqual(TEXT("theft restores exact priority"), Item->Priority, 13.25f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBotDeathBagLootSpec,
	"WoT.Inventory.Bot.DeathBagLoot", BotInventoryFlags)
bool FWoTInventoryBotDeathBagLootSpec::RunTest(const FString&)
{
	UE::Net::FTestWorldInstance Server = CreateBotInventoryListenServer();
	if (!TestTrue(TEXT("listen world loads for the real death-bag path"), Server.IsLoaded()))
	{
		return false;
	}
	UWorld* World = Server.GetWorld();
	TestTrue(TEXT("death-bag fixture uses multiplayer rules"), World->GetNetMode() != NM_Standalone);
	AWoTTestBotParticipant* Bot = World->SpawnActor<AWoTTestBotParticipant>();
	Bot->InitPlayerState();
	AWoTCharacter* Pawn = SpawnBotInventoryPawn(World, Bot, FVector(0, 0, 200));
	APlayerController* Human = World->SpawnActor<APlayerController>();
	Human->InitPlayerState();
	AWoTCharacter* Looter = SpawnBotInventoryPawn(World, Human, FVector(900, 0, 200));
	AWoTAngrealInvFireball* Item = GiveBotInventoryItem(World, Pawn);
	Item->SetVictim(Looter);
	Item->SetBotPriorityOverride(89.0f);
	Pawn->Health = 0;
	Pawn->Died(Human, NAME_None, Pawn->GetActorLocation());
	AWoTBagHolding* Bag = nullptr;
	for (TActorIterator<AWoTBagHolding> It(World); It; ++It)
	{
		if (IsValid(*It) && It->GetItems().Contains(Item)) Bag = *It;
	}
	if (TestNotNull(TEXT("death bags the bot's original item"), Bag))
	{
		TestNull(TEXT("death detaches and clears victim before loot"), Item->GetVictim());
		TestEqual(TEXT("death restores exact saved priority before loot"), Item->Priority, 13.25f);
		Bag->GiveTo(Looter);
		WoTTest::TickWorld(World, 0.05f);
		TestTrue(TEXT("human loots the original death-bag item"), Item->GetOwner() == Looter);
		TestNull(TEXT("human inherits no victim from the dead bot"), Item->GetVictim());
		TestEqual(TEXT("human inherits the pre-bot priority"), Item->Priority, 13.25f);
	}
	return !HasAnyErrors();
}
#endif

#endif
