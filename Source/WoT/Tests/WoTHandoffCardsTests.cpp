// Citadel hand cards, creature blocking columns, and TrollocStrong.
// Helper names are file-unique because the Tests unity build merges translation units.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTTroopInventories.h"
#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTTestAIClasses.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	FActorSpawnParameters HandoffCardsSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	void HandoffCardsSpecAddBudget(AWoTPlayerController& Controller,
		FName AdapterName, int32 ItemCount)
	{
		FWoTCitadelBudgetCount& Entry =
			Controller.OwnerEditBudget.AddDefaulted_GetRef();
		Entry.AdapterName = AdapterName;
		Entry.ItemCount = ItemCount;
	}

	AWoTCharacter* HandoffCardsSpecSpawnLocalPlayer(UWorld* World,
		const FVector& Location, AWoTPlayerController*& OutController)
	{
		OutController = World->SpawnActor<AWoTPlayerController>(
			HandoffCardsSpecAlwaysSpawnParams());
		AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, HandoffCardsSpecAlwaysSpawnParams());
		if (IsValid(OutController) && IsValid(Character))
		{
			OutController->SetAsLocalPlayerController();
			OutController->Possess(Character);
		}
		return Character;
	}

	const FWoTAnimTableEntry* HandoffCardsSpecFindAnimIndex(
		FName TableName, int32 Index)
	{
		const TArray<FWoTAnimTableEntry>* Table = WoTAnimTable::Find(TableName);
		if (!Table)
		{
			return nullptr;
		}
		for (const FWoTAnimTableEntry& Entry : *Table)
		{
			if (Entry.Index == Index)
			{
				return &Entry;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandoffCardsCitadelHandsSpec,
	"WoT.Handoff.CitadelHandCards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandoffCardsCitadelHandsSpec::RunTest(const FString&)
{
	// Mirror present at first reconcile: all four retail cards stamp and seals win
	// the initial-selection pass.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
		HandoffCardsSpecAlwaysSpawnParams());
	AWoTCharacter* Character = nullptr;
	if (IsValid(Controller))
	{
		HandoffCardsSpecAddBudget(*Controller, TEXT("TrollocInventory"), 5);
		HandoffCardsSpecAddBudget(*Controller, TEXT("MyrddraalInventory"), 3);
		HandoffCardsSpecAddBudget(*Controller, TEXT("BATrollocInventory"), 1);
		Controller->OwnerSealCardCount = 2;
		Character = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 100), FRotator::ZeroRotator,
			HandoffCardsSpecAlwaysSpawnParams());
		if (IsValid(Character))
		{
			Controller->SetAsLocalPlayerController();
			Controller->Possess(Character);
		}
	}
	if (!TestNotNull(TEXT("local controller"), Controller)
		|| !TestNotNull(TEXT("local character"), Character))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->ReconcileCitadelEditorHandSet();

	UWoTHandSet* Hands = Character->CitadelEditorHandSet;
	if (TestNotNull(TEXT("citadel hand set"), Hands))
	{
		TestEqual(TEXT("CE_SEALS class"), Hands->GetHand(0)->ClassName[0],
			FName(TEXT("SealInventory")));
		TestEqual(TEXT("CE_SEALS count"), Hands->GetHand(0)->GetDisplayCount(0), 2);
		TestEqual(TEXT("CE_GRUNT class"), Hands->GetHand(1)->ClassName[0],
			FName(TEXT("TrollocInventory")));
		TestEqual(TEXT("CE_GRUNT count"), Hands->GetHand(1)->GetDisplayCount(0), 5);
		TestEqual(TEXT("CE_CAPTAIN class"), Hands->GetHand(2)->ClassName[0],
			FName(TEXT("MyrddraalInventory")));
		TestEqual(TEXT("CE_CHAMPION class"), Hands->GetHand(2)->ClassName[1],
			FName(TEXT("BATrollocInventory")));
		TestEqual(TEXT("CE_CAPTAIN count"), Hands->GetHand(2)->GetDisplayCount(0), 3);
		TestEqual(TEXT("CE_CHAMPION count"), Hands->GetHand(2)->GetDisplayCount(1), 1);
		TestEqual(TEXT("retail initial hand is seals"), Hands->Selected, 0);

		// An idempotent reconcile must neither duplicate AddClassName nor pull a
		// user's current hand back to the initial card.
		Hands->Select(9);
		Character->ReconcileCitadelEditorHandSet();
		TestEqual(TEXT("repeat reconcile preserves user selection"), Hands->Selected, 9);
		TestEqual(TEXT("repeat reconcile leaves one grunt card"),
			Hands->GetHand(1)->GetItemCount(), 1);
		TestEqual(TEXT("repeat reconcile leaves two captain-tier cards"),
			Hands->GetHand(2)->GetItemCount(), 2);
	}
	WoTTest::DestroySpecWorld(World);

	// Mirror absent first, then late: None is never stamped and the OnRep-style
	// re-entry resolves the faction adapters when rows arrive.
	World = WoTTest::CreateSpecWorld();
	Controller = nullptr;
	Character = HandoffCardsSpecSpawnLocalPlayer(
		World, FVector(1000, 0, 100), Controller);
	if (!TestNotNull(TEXT("late-mirror controller"), Controller)
		|| !TestNotNull(TEXT("late-mirror character"), Character))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Character->ReconcileCitadelEditorHandSet();
	Hands = Character->CitadelEditorHandSet;
	if (TestNotNull(TEXT("late-mirror hand set"), Hands))
	{
		TestEqual(TEXT("seal card does not depend on mirror"),
			Hands->GetHand(0)->ClassName[0], FName(TEXT("SealInventory")));
		TestTrue(TEXT("grunt stays blank without mirror"),
			Hands->GetHand(1)->ClassName[0].IsNone());
		TestTrue(TEXT("captain stays blank without mirror"),
			Hands->GetHand(2)->ClassName[0].IsNone());
		TestTrue(TEXT("champion stays blank without mirror"),
			Hands->GetHand(2)->ClassName[1].IsNone());

		HandoffCardsSpecAddBudget(*Controller, TEXT("TrollocInventory"), 4);
		HandoffCardsSpecAddBudget(*Controller, TEXT("MyrddraalInventory"), 2);
		HandoffCardsSpecAddBudget(*Controller, TEXT("BATrollocInventory"), 1);
		Controller->OnRep_OwnerEditBudget();
		TestEqual(TEXT("late grunt stamps"), Hands->GetHand(1)->ClassName[0],
			FName(TEXT("TrollocInventory")));
		TestEqual(TEXT("late captain stamps"), Hands->GetHand(2)->ClassName[0],
			FName(TEXT("MyrddraalInventory")));
		TestEqual(TEXT("late champion stamps"), Hands->GetHand(2)->ClassName[1],
			FName(TEXT("BATrollocInventory")));
		TestEqual(TEXT("late counts reconcile"), Hands->GetHand(1)->GetDisplayCount(0), 4);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandoffCardsBlockingColumnsSpec,
	"WoT.Handoff.CreatureBlockingColumns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandoffCardsBlockingColumnsSpec::RunTest(const FString&)
{
	const FWoTCreatureRow* CombatRow = WoTCreatureData::Find(TEXT("Trolloc"));
	const FWoTCreatureRow* FocusRow =
		WoTCreatureData::Find(TEXT("MashadarFocusPawn"));
	if (TestNotNull(TEXT("combat blocking row"), CombatRow))
	{
		TestTrue(TEXT("combat bBlockActors defaults true"), CombatRow->bBlockActors);
		TestTrue(TEXT("combat bBlockPlayers defaults true"), CombatRow->bBlockPlayers);
		TestTrue(TEXT("combat bProjTarget defaults true"), CombatRow->bProjTarget);
	}
	if (TestNotNull(TEXT("focus blocking row"), FocusRow))
	{
		TestFalse(TEXT("focus bBlockActors false"), FocusRow->bBlockActors);
		TestFalse(TEXT("focus bBlockPlayers false"), FocusRow->bBlockPlayers);
		TestFalse(TEXT("focus bProjTarget false"), FocusRow->bProjTarget);
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	AWoTNPCCharacter* Focus = WoTCreatureData::SpawnCreature(
		*World, TEXT("MashadarFocusPawn"), FVector(1000, 0, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (TestNotNull(TEXT("combat creature"), Trolloc)
		&& TestNotNull(TEXT("focus marker"), Focus))
	{
		TestEqual(TEXT("combat row blocks pawn channel"),
			Trolloc->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Block);
		TestEqual(TEXT("focus row ignores pawn/projectile channel"),
			Focus->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Ignore);
		TestEqual(TEXT("focus retains world collision"),
			Focus->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_WorldStatic),
			ECR_Block);
		Focus->SetActorEnableCollision(true);
		TestEqual(TEXT("deploy collision enable preserves row response"),
			Focus->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Ignore);
	}

	// Remote-client row entry point: start from a generic blocking capsule and
	// invoke the replicated-row callback directly.
	AWoTNPCCharacter* ClientNPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator,
		HandoffCardsSpecAlwaysSpawnParams());
	if (TestNotNull(TEXT("fresh client NPC"), ClientNPC))
	{
		ClientNPC->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		ClientNPC->CreatureRowName = TEXT("MashadarFocusPawn");
		ClientNPC->OnRep_CreatureRow();
		TestEqual(TEXT("OnRep applies focus ignore"),
			ClientNPC->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Ignore);
		TestEqual(TEXT("OnRep leaves world response alone"),
			ClientNPC->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_WorldStatic),
			ECR_Block);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandoffCardsTrollocStrongSpec,
	"WoT.Handoff.TrollocStrong",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandoffCardsTrollocStrongSpec::RunTest(const FString&)
{
	const FWoTCreatureRow* Row = WoTCreatureData::Find(TEXT("TrollocStrong"));
	if (TestNotNull(TEXT("TrollocStrong registry row"), Row))
	{
		TestEqual(TEXT("GroundSpeed"), Row->GroundSpeed, 420.0f);
		TestEqual(TEXT("GroundSpeedMin"), Row->GroundSpeedMin, 420.0f);
		TestEqual(TEXT("GroundSpeedMax"), Row->GroundSpeedMax, 420.0f);
		TestEqual(TEXT("BaseWalkingSpeed"), Row->BaseWalkingSpeed, 105.0f);
		TestEqual(TEXT("chase suggested speed"), Row->ChaseSuggestedSpeed, 420.0f);
		TestEqual(TEXT("wait suggested speed"), Row->WaitSuggestedSpeed, 105.0f);
		TestEqual(TEXT("sidearm animation table"), Row->AnimTable,
			FName(TEXT("TrollocSideArm")));
		TestEqual(TEXT("DrawScale"), Row->DrawScale, 1.49f);
		TestEqual(TEXT("inherits Trolloc sound table"), Row->SoundTableClass,
			FName(TEXT("WoTPawns.SoundTableTrolloc")));
	}
	// The two retail tables DIVERGE on the Forsaken grunt: the summon table
	// (PlayerTroops(3), WOTPlayer.uc:7267) names base Trolloc, the citadel card
	// (TrollocInventory.uc ResourceClass) names TrollocStrong. Pin both.
	TestEqual(TEXT("Forsaken grunt SUMMON row stays base Trolloc"),
		WoTCreatureData::GetTroopRow(TEXT("Forsaken"), TEXT("Grunt")),
		FName(TEXT("Trolloc")));
	TestEqual(TEXT("Trolloc adapter row"),
		GetDefault<AWoTTrollocInventory>()->CreatureRowName,
		FName(TEXT("TrollocStrong")));

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Strong = WoTCreatureData::SpawnCreature(
		*World, TEXT("TrollocStrong"), FVector(0, 1000, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (TestNotNull(TEXT("spawned TrollocStrong"), Strong))
	{
		const UCharacterMovementComponent* Move = Strong->GetCharacterMovement();
		TestEqual(TEXT("spawned GroundSpeedMin"), Strong->GroundSpeedMin, 420.0f);
		TestEqual(TEXT("spawned GroundSpeedMax"), Strong->GroundSpeedMax, 420.0f);
		TestEqual(TEXT("movement max walk speed"), Move->MaxWalkSpeed, 420.0f);
		TestEqual(TEXT("movement max fly speed"), Move->MaxFlySpeed, 420.0f);
		TestEqual(TEXT("spawned chase hint"),
			Strong->Brain->GetGoal(EWoTGoalIndex::Threat).SuggestedSpeed, 420.0f);
		TestEqual(TEXT("spawned wait hint"),
			Strong->Brain->GetGoal(EWoTGoalIndex::Waiting).SuggestedSpeed, 105.0f);
		TestEqual(TEXT("spawned animation table"), Strong->AnimTableName,
			FName(TEXT("TrollocSideArm")));
		TestNotNull(TEXT("vertex animation body component"),
			Cast<UWoTVertexAnimComponent>(Strong->GetMesh()));
		TestEqual(TEXT("spawned mesh DrawScale"),
			static_cast<float>(Strong->GetMesh()->GetRelativeScale3D().X), 1.49f);
		TestNotNull(TEXT("inherited throwing axe"), Strong->GetRangedWeapon());
	}

	const FWoTAnimTableEntry* BaseThrow =
		HandoffCardsSpecFindAnimIndex(TEXT("Trolloc"), 30);
	const FWoTAnimTableEntry* StrongThrow =
		HandoffCardsSpecFindAnimIndex(TEXT("TrollocSideArm"), 30);
	if (TestNotNull(TEXT("base index 30"), BaseThrow)
		&& TestNotNull(TEXT("sidearm index 30"), StrongThrow))
	{
		TestEqual(TEXT("base projectile animation"), BaseThrow->AnimName,
			FName(TEXT("ATTACKRUNB")));
		TestEqual(TEXT("sidearm projectile animation"), StrongThrow->AnimName,
			FName(TEXT("ATTACKTHROW1")));
		TestEqual(TEXT("sidearm tween override"), StrongThrow->TweenTime, 0.2f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
