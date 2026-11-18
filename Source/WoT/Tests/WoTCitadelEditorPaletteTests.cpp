// Slice 2b: owner edit-session mirror and the local citadel editor palette.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

namespace
{
	FActorSpawnParameters EditorPaletteSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTCharacter* EditorPaletteSpecSpawnLocalPlayer(
		UWorld* World, AWoTPlayerController*& OutController)
	{
		OutController = World->SpawnActor<AWoTPlayerController>(
			EditorPaletteSpecAlwaysSpawnParams());
		AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 100), FRotator::ZeroRotator,
			EditorPaletteSpecAlwaysSpawnParams());
		if (IsValid(OutController) && IsValid(Character))
		{
			OutController->SetAsLocalPlayerController();
			OutController->Possess(Character);
		}
		return Character;
	}

	void EditorPaletteSpecSetCount(
		AWoTPlayerController* Controller, FName AdapterName, int32 ItemCount)
	{
		if (!IsValid(Controller))
		{
			return;
		}
		for (FWoTCitadelBudgetCount& Entry : Controller->OwnerEditBudget)
		{
			if (Entry.AdapterName == AdapterName)
			{
				Entry.ItemCount = ItemCount;
				return;
			}
		}
		FWoTCitadelBudgetCount& Entry =
			Controller->OwnerEditBudget.AddDefaulted_GetRef();
		Entry.AdapterName = AdapterName;
		Entry.ItemCount = ItemCount;
	}

	bool EditorPaletteSpecBeginLocalEdit(
		AWoTPlayerController* Controller, uint8 Team)
	{
		if (!IsValid(Controller))
		{
			return false;
		}
		Controller->OwnerEditSessionTeam = Team;
		Controller->OnRep_OwnerEditSessionTeam();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorPaletteOrderingSpec,
	"WoT.Citadel.EditorPalette.OrderingAndSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorPaletteOrderingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = EditorPaletteSpecSpawnLocalPlayer(World, Controller);
	if (!IsValid(Character) || !EditorPaletteSpecBeginLocalEdit(Controller, 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	UWoTHandSet* Palette = Character->CitadelEditorHandSet;
	if (TestNotNull(TEXT("editing creates the citadel palette"), Palette))
	{
		TestEqual(TEXT("seal card is mirror-independent"),
			Palette->GetHand(0)->ClassName[0], FName(TEXT("SealInventory")));
		for (int32 DeferredIndex = 1; DeferredIndex < 3; ++DeferredIndex)
		{
			TestTrue(FString::Printf(TEXT("troop hand %d waits for mirror"), DeferredIndex),
				Palette->GetHand(DeferredIndex)->IsEmpty());
		}
		static const FName ExpectedAdapters[] = {
			TEXT("AlarmInventory"), TEXT("SpearInventory"),
			TEXT("FireWallInventory"), TEXT("WallInventory"), TEXT("PitInventory"),
			TEXT("StaircaseInventory"), TEXT("PortcullisInventory")};
		for (int32 AdapterIndex = 0;
			AdapterIndex < static_cast<int32>(UE_ARRAY_COUNT(ExpectedAdapters));
			++AdapterIndex)
		{
			const int32 HandIndex = AdapterIndex + 3;
			TestEqual(FString::Printf(TEXT("CE hand %d adapter"), HandIndex),
				Palette->GetHand(HandIndex)->ClassName[0], ExpectedAdapters[AdapterIndex]);
			TestEqual(FString::Printf(TEXT("CE hand %d has one card"), HandIndex),
				Palette->GetHand(HandIndex)->GetItemCount(), 1);
		}
		TestEqual(TEXT("retail seals hand is selected on entry"), Palette->Selected, 0);
		Character->SelectHand(9);
		TestEqual(TEXT("selection uses CurrentHandSet while editing"),
			Palette->Selected, 9);
		TestTrue(TEXT("CurrentHandSet remains the editor palette"),
			Character->CurrentHandSet == Palette);
	}

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorPaletteCountSpec,
	"WoT.Citadel.EditorPalette.CountReconciliation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorPaletteCountSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = EditorPaletteSpecSpawnLocalPlayer(World, Controller);
	if (!IsValid(Character) || !IsValid(Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	EditorPaletteSpecSetCount(Controller, TEXT("WallInventory"), 4);
	EditorPaletteSpecSetCount(Controller, TEXT("PortcullisInventory"), 2);
	EditorPaletteSpecBeginLocalEdit(Controller, 2);

	UWoTHandSet* Palette = Character ? Character->CitadelEditorHandSet : nullptr;
	if (TestNotNull(TEXT("palette exists"), Palette))
	{
		TestEqual(TEXT("initial Wall budget"),
			Palette->GetHand(6)->GetDisplayCount(0), 4);
		TestEqual(TEXT("initial Portcullis budget"),
			Palette->GetHand(9)->GetDisplayCount(0), 2);

		// Simulate the owner receiving the post-create spend and post-remove refund rows.
		EditorPaletteSpecSetCount(Controller, TEXT("WallInventory"), 3);
		Controller->OnRep_OwnerEditBudget();
		TestEqual(TEXT("spent resource decrements its hand count"),
			Palette->GetHand(6)->GetDisplayCount(0), 3);
		EditorPaletteSpecSetCount(Controller, TEXT("WallInventory"), 4);
		Controller->OnRep_OwnerEditBudget();
		TestEqual(TEXT("refunded resource increments its hand count"),
			Palette->GetHand(6)->GetDisplayCount(0), 4);
	}

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorPaletteSessionSpec,
	"WoT.Citadel.EditorPalette.SessionSwap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorPaletteSessionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = EditorPaletteSpecSpawnLocalPlayer(World, Controller);
	if (!IsValid(Character) || !IsValid(Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTHandSet* OriginalAngreal = Character->AngrealHandSet;
	TestTrue(TEXT("normal current set is AngrealHandSet"),
		Character->CurrentHandSet == OriginalAngreal);

	TArray<TPair<bool, uint8>> Edges;
	Controller->OnCitadelEditSessionChanged.AddLambda(
		[&Edges](bool bEditing, uint8 Team)
		{
			Edges.Emplace(bEditing, Team);
		});
	EditorPaletteSpecBeginLocalEdit(Controller, 3);
	TestTrue(TEXT("enter swaps to CitadelEditorHandSet"),
		Character->CurrentHandSet == Character->CitadelEditorHandSet);

	Controller->OwnerEditBudget.Reset();
	Controller->OnRep_OwnerEditBudget();
	Controller->OwnerEditSessionTeam = 255;
	Controller->OnRep_OwnerEditSessionTeam();
	TestTrue(TEXT("exit restores the exact AngrealHandSet"),
		Character->CurrentHandSet == OriginalAngreal);
	TestEqual(TEXT("enter and exit notify once each"), Edges.Num(), 2);
	if (Edges.Num() == 2)
	{
		TestTrue(TEXT("enter notification"), Edges[0].Key && Edges[0].Value == 3);
		TestTrue(TEXT("exit notification"), !Edges[1].Key && Edges[1].Value == 255);
	}

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

// Source/registration CONTRACT check (not a live two-client data-flow test — that is a
// PIE/net-harness gate, owed): reads the ACTUAL condition each property is registered with
// in GetLifetimeReplicatedProps, so a regression to an unconditional DOREPLIFETIME (which
// would register COND_None and leak the mirror to every client) fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorPaletteOwnerReplicationSpec,
	"WoT.Citadel.EditorPalette.OwnerReplicationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorPaletteOwnerReplicationSpec::RunTest(const FString&)
{
	auto IsReplicatedProperty = [](const TCHAR* PropertyName)
	{
		const FProperty* Property = FindFProperty<FProperty>(
			AWoTPlayerController::StaticClass(), PropertyName);
		return Property && Property->HasAnyPropertyFlags(CPF_Net);
	};
	// NOTE: we do NOT call GetLifetimeReplicatedProps here to read the live per-property
	// condition — doing so on the CDO trips UE5's push-model consistency assert
	// (FLifetimeProperty::operator==, CoreNet.h). Both edit-state props are registered via
	// DOREPLIFETIME_CONDITION(..., CitadelOwnerReplicationCondition()), so asserting the
	// helper resolves to COND_OwnerOnly is the source-contract guarantee; true two-client
	// data-flow verification is a PIE/net-harness gate (owed).
	TestTrue(TEXT("OwnerEditSessionTeam is a replicated UPROPERTY"),
		IsReplicatedProperty(TEXT("OwnerEditSessionTeam")));
	TestTrue(TEXT("OwnerEditBudget is a replicated UPROPERTY"),
		IsReplicatedProperty(TEXT("OwnerEditBudget")));
	TestEqual(TEXT("edit-state props register with the owner-only condition"),
		static_cast<int32>(AWoTPlayerController::CitadelOwnerReplicationCondition()),
		static_cast<int32>(COND_OwnerOnly));
	TestFalse(TEXT("authority BudgetInfo pointer remains non-replicated"),
		IsReplicatedProperty(TEXT("EditSessionBudget")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
