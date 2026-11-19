#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"
#include "WoTTestBotParticipant.h"
#include "WoTTestBotCasting.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTAIUtil.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	AWoTCharacter* SpawnBotCastingPawn(UWorld* World)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 200), FRotator::ZeroRotator, Params);
		AWoTTestBotParticipant* Controller = World->SpawnActor<AWoTTestBotParticipant>();
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Pawn;
	}

	AWoTAngrealInventory* GiveBotCastingItem(AWoTCharacter* Pawn)
	{
		const FTransform Transform(Pawn->GetActorTransform());
		AWoTAngrealInventory* Item = Pawn->GetWorld()->SpawnActorDeferred<AWoTAngrealInventory>(
			AWoTTestBotCastingItem::StaticClass(), Transform, Pawn, Pawn,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Item->bHeldItem = true;
		Item->CurCharges = 20;
		Item->bOffensive = true;
		Item->Priority = 1.0f;
		Item->MinChargesInGroup = Item->MaxChargesInGroup = 10;
		Item->MinChargeGroupInterval = 0.0f;
		Item->RoundsPerMinute = 600.0f;
		Item->FinishSpawning(Transform);
		Item->GiveTo(Pawn);
		Item->CurCharges = 20;
		return Item;
	}

	UWoTCasterComponent* AttachBotCaster(AWoTCharacter* Pawn)
	{
		UWoTCasterComponent* Caster = NewObject<UWoTCasterComponent>(Pawn);
		Pawn->AddInstanceComponent(Caster);
		Caster->RegisterComponent();
		return Caster;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotChargeUsageIntervalSpec,
	"WoT.AI.BotChargeUsageInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotChargeUsageIntervalSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnBotCastingPawn(World);
	UWoTCasterComponent* Caster = AttachBotCaster(Pawn);
	Caster->MinOffensiveUsageInterval = 5.0f;
	AWoTAngrealInventory* First = GiveBotCastingItem(Pawn);
	AWoTAngrealInventory* Alternative = GiveBotCastingItem(Pawn);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(FVector(1000, 0, 200), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	Caster->BeginCast(First);
	if (!TestTrue(TEXT("the real inventory activation is active"), Caster->IsCasting()))
	{
		return false;
	}
	First->UseCharge();
	TestEqual(TEXT("the real charge expenditure occurred"), First->CurCharges, 19);
	Caster->Release();
	TestNull(TEXT("charge notification blocks even a different item during the usage interval"),
		Caster->SelectOffensiveWeave(Victim));
	TestTrue(TEXT("the alternative item remains held"), Pawn->InventoryComponent->Items.Contains(Alternative));
	WoTTest::TickWorld(World, 5.1f);
	TestEqual(TEXT("the alternative becomes selectable after the usage interval"),
		Caster->SelectOffensiveWeave(Victim), Alternative);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotHardLandingCastSpec,
	"WoT.AI.BotHardLandingCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotHardLandingCastSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnBotCastingPawn(World);
	UWoTCasterComponent* Caster = AttachBotCaster(Pawn);
	AWoTAngrealInventory* Item = GiveBotCastingItem(Pawn);
	WoTTest::TickWorld(World, 0.2f);
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::LandingFreeze);
	Caster->BeginCast(Item);
	TestFalse(TEXT("hard landing rejects a bot caster activation"), Caster->IsCasting());
	TestEqual(TEXT("hard landing reaches no inventory activation"), CastChecked<AWoTTestBotCastingItem>(Item)->ActivationCount, 0);
	TestFalse(TEXT("hard landing does not set the player fire latch"), Pawn->IsPlayerAnimFireHeldForTesting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotMatchEndedCastSpec,
	"WoT.AI.BotMatchEndedCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotMatchEndedCastSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnBotCastingPawn(World);
	UWoTCasterComponent* Caster = AttachBotCaster(Pawn);
	AWoTAngrealInventory* Item = GiveBotCastingItem(Pawn);
	WoTTest::TickWorld(World, 0.2f);
	World->GetAuthGameMode<AWoTGameMode>()->EndGame(FName(TEXT("test")));
	Caster->BeginCast(Item);
	TestFalse(TEXT("a decided match rejects a bot caster activation"), Caster->IsCasting());
	TestEqual(TEXT("a decided match reaches no inventory activation"), CastChecked<AWoTTestBotCastingItem>(Item)->ActivationCount, 0);
	TestFalse(TEXT("a decided match does not set the player fire latch"), Pawn->IsPlayerAnimFireHeldForTesting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotOwnedInventoryCastingSpec,
	"WoT.AI.BotOwnedInventoryCasting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotOwnedInventoryCastingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnBotCastingPawn(World);
	AWoTAngrealInventory* Item = GiveBotCastingItem(Pawn);
	UWoTCasterComponent* Caster = NewObject<UWoTCasterComponent>(Pawn);
	Caster->bBuildLoadout = false;
	FWoTWeaveSlot Slot;
	Slot.WeaveClass = AWoTTestBotCastingItem::StaticClass();
	Caster->Weaves.Add(Slot);
	Pawn->AddInstanceComponent(Caster);
	Caster->RegisterComponent();
	TestEqual(TEXT("disabling loadout construction grants no extra inventory"), Pawn->InventoryComponent->Items.Num(), 1);
	int32 ItemActors = 0;
	for (TActorIterator<AWoTTestBotCastingItem> It(World); It; ++It) { ++ItemActors; }
	TestEqual(TEXT("disabling loadout construction spawns no kit actors"), ItemActors, 1);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(FVector(1000, 0, 200), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	Item->MaxChargeUsedInterval = 0.0f;
	TestEqual(TEXT("the caster selects its pawn's own item"), Caster->SelectOffensiveWeave(Victim), Item);
	const FRotator Aim(12.0f, 35.0f, 0.0f);
	Pawn->GetController()->SetControlRotation(Aim);
	Caster->BeginCast(Item);
	TestTrue(TEXT("activation sets the player fire latch"), Pawn->IsPlayerAnimFireHeldForTesting());
	TestEqual(TEXT("activation follows player inventory selection"), Pawn->InventoryComponent->SelectedItem.Get(), static_cast<AWoTInventory*>(Item));
	TestEqual(TEXT("activation reaches the owned item once"), CastChecked<AWoTTestBotCastingItem>(Item)->ActivationCount, 1);
	TestTrue(TEXT("activation preserves the controller's chosen aim"), Pawn->GetController()->GetControlRotation().Equals(Aim));
	TestTrue(TEXT("continuous casting preserves player animation ownership"), Pawn->RepAnim.Sequence != FName(TEXT("ATTACKRUN")));
	Item->UseCharge();
	TestTrue(TEXT("charge use preserves player animation ownership"), Pawn->RepAnim.Sequence != FName(TEXT("ATTACKRUN")));
	Caster->Release();
	TestFalse(TEXT("release clears the player fire latch"), Pawn->IsPlayerAnimFireHeldForTesting());
	WoTTest::TickWorld(World, 0.2f);
	TestNull(TEXT("release preserves the adapted-item exclusion"), Caster->SelectOffensiveWeave(Victim));
	Caster->ResetAdaptation();
	TestEqual(TEXT("explicit adaptation reset makes the held item selectable"), Caster->SelectOffensiveWeave(Victim), Item);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotNPCPathPreservedSpec,
	"WoT.AI.BotNPCPathPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotNPCPathPreservedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(FVector(0, 0, 200), FRotator::ZeroRotator);
	NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTCasterComponent* Caster = NewObject<UWoTCasterComponent>(NPC);
	FWoTWeaveSlot Slot;
	Slot.WeaveClass = AWoTTestBotCastingItem::StaticClass();
	Slot.MinInitialCharges = Slot.MaxInitialCharges = 20;
	Slot.MinChargesInGroup = Slot.MaxChargesInGroup = 10;
	Caster->Weaves.Add(Slot);
	NPC->AddInstanceComponent(Caster);
	Caster->RegisterComponent();
	TestTrue(TEXT("existing casters build their loadout by default"), Caster->bBuildLoadout);
	TestEqual(TEXT("the default loadout still grants its authored item"), NPC->InventoryComponent->Items.Num(), 1);
	AWoTTestBotCastingItem* Item = CastChecked<AWoTTestBotCastingItem>(NPC->InventoryComponent->Items[0]);
	Item->MaxChargeUsedInterval = 0.0f;
	WoTTest::TickWorld(World, 0.2f);
	World->GetAuthGameMode<AWoTGameMode>()->EndGame(FName(TEXT("test")));
	Caster->BeginCast(Item);
	TestTrue(TEXT("the existing NPC path still casts after match end"), Caster->IsCasting());
	TestEqual(TEXT("NPC activation still reaches its inventory item"), Item->ActivationCount, 1);
	TestFalse(TEXT("NPC activation never owns the player fire latch"), NPC->IsPlayerAnimFireHeldForTesting());
	TestEqual(TEXT("NPC continuous casts retain their attack animation"), NPC->RepAnim.Sequence, FName(TEXT("ATTACKRUN")));
	Caster->Release();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRulesBotHumanCastParitySpec,
	"WoT.Rules.BotHumanCastParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRulesBotHumanCastParitySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTPlayerController* Existing = World->SpawnActor<AWoTPlayerController>();
	Existing->InitPlayerState();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector(0, 0, 200), FRotator::ZeroRotator);
	Existing->Possess(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestBotCastingItem* Item = CastChecked<AWoTTestBotCastingItem>(GiveBotCastingItem(Pawn));
	WoTTest::TickWorld(World, 0.2f);
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::LandingFreeze);
	TestFalse(TEXT("the shared gate retains the human landing rejection"), Pawn->CanCastNow());
	Pawn->ServerUseAngreal_Implementation(FRotator::ZeroRotator);
	TestEqual(TEXT("the existing human RPC rejects hard landing"), Item->ActivationCount, 0);
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	const FRotator Aim(15.0f, 40.0f, 0.0f);
	Pawn->ServerUseAngreal_Implementation(Aim);
	TestEqual(TEXT("the existing human RPC still activates inventory"), Item->ActivationCount, 1);
	TestTrue(TEXT("the existing human RPC still stamps aim"), Existing->GetControlRotation().Equals(Aim));
	TestTrue(TEXT("the existing human RPC still sets fire animation"), Pawn->IsPlayerAnimFireHeldForTesting());
	Pawn->ServerCeaseUsingAngreal_Implementation();
	TestFalse(TEXT("the existing human release still clears fire animation"), Pawn->IsPlayerAnimFireHeldForTesting());
	World->GetAuthGameMode<AWoTGameMode>()->EndGame(FName(TEXT("test")));
	TestFalse(TEXT("a notified human reads its controller's ended flag"), Pawn->CanCastNow());
	Pawn->ServerUseAngreal_Implementation(Aim);
	TestEqual(TEXT("the existing human RPC rejects a notified match end"), Item->ActivationCount, 1);
	AWoTPlayerController* Late = World->SpawnActor<AWoTPlayerController>();
	Late->InitPlayerState();
	AWoTCharacter* LatePawn = World->SpawnActor<AWoTCharacter>(FVector(500, 0, 200), FRotator::ZeroRotator);
	Late->Possess(LatePawn);
	AWoTTestBotCastingItem* LateItem = CastChecked<AWoTTestBotCastingItem>(GiveBotCastingItem(LatePawn));
	TestFalse(TEXT("a late human retains its existing unnotified flag"), Late->IsMatchEnded());
	TestTrue(TEXT("a late human gate reads its controller instead of game mode"), LatePawn->CanCastNow());
	LatePawn->ServerUseAngreal_Implementation(Aim);
	TestEqual(TEXT("the existing late-human activation remains accepted"), LateItem->ActivationCount, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBotApparentTeamSharedSpec,
	"WoT.AI.BotApparentTeamShared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAIBotApparentTeamSharedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTCharacter* Pawn = SpawnBotCastingPawn(World);
	Pawn->GetPlayerState<AWoTPlayerState>()->Team = 2;
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(FVector(200, 0, 200), FRotator::ZeroRotator);
	NPC->AssignedTeam = 7;
	TestEqual(TEXT("a bot presents its real participant team"), WoTAIUtil::ApparentTeamOf(Pawn), static_cast<uint8>(2));
	TestEqual(TEXT("an NPC retains its assigned team"), WoTAIUtil::ApparentTeamOf(NPC), static_cast<uint8>(7));
	TestEqual(TEXT("null retains the teamless sentinel"), WoTAIUtil::ApparentTeamOf(nullptr), static_cast<uint8>(253));
	World->GetAuthGameMode<AWoTGameMode>()->GivePawnAngreal(Pawn, AWoTAngrealInvDisguise::StaticClass(), 20);
	AWoTAngrealInvDisguise* Item = CastChecked<AWoTAngrealInvDisguise>(
		Pawn->InventoryComponent->FindInventoryType(AWoTAngrealInvDisguise::StaticClass()));
	Item->CastArtifact();
	TestEqual(TEXT("the shared helper honours a bot's active apparent team"), WoTAIUtil::ApparentTeamOf(Pawn), static_cast<uint8>(7));
	TestEqual(TEXT("the NPC forwarding API preserves the apparent team"), AWoTNPCCharacter::TeamOf(Pawn), static_cast<uint8>(7));
	TestTrue(TEXT("the NPC friend check retains its apparent-team behavior"), NPC->IsFriendly(Pawn));
	Item->UnCastArtifact();
	TestEqual(TEXT("uncast restores the bot's real team"), WoTAIUtil::ApparentTeamOf(Pawn), static_cast<uint8>(2));
	TestFalse(TEXT("the NPC friend check restores its hostile result"), NPC->IsFriendly(Pawn));
	return !HasAnyErrors();
}

#endif
