// Headless specs for the shipped HandInfo/HandSet angreal grouping and drive path.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvDecay.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTSound.h"

#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"

namespace
{
	FActorSpawnParameters WoTHandsTests_AlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsHandInfoSpec,
	"WoT.Hands.HandInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsHandInfoSpec::RunTest(const FString&)
{
	UWoTHandInfo* HandInfo = NewObject<UWoTHandInfo>();
	HandInfo->AddClassName(FName(TEXT("A")), 0);
	HandInfo->AddClassName(FName(TEXT("B")), 2);
	TestTrue(TEXT("empty"), HandInfo->IsEmpty());
	HandInfo->AddItem(FName(TEXT("B")), false, nullptr);
	TestFalse(TEXT("not empty"), HandInfo->IsEmpty());
	TestEqual(TEXT("selected B"), HandInfo->GetSelectedClassName(), FName(TEXT("B")));
	TestEqual(TEXT("count 1"), HandInfo->GetItemCount(), 1);
	HandInfo->AddItem(FName(TEXT("A")), false, nullptr);
	TestEqual(TEXT("count 2"), HandInfo->GetItemCount(), 2);
	HandInfo->Select(0);
	TestEqual(TEXT("sel A"), HandInfo->GetSelectedClassName(), FName(TEXT("A")));
	HandInfo->SelectNext();
	TestEqual(TEXT("next wraps to B"), HandInfo->GetSelectedClassName(), FName(TEXT("B")));
	HandInfo->SelectNext();
	TestEqual(TEXT("next wraps back to A"), HandInfo->GetSelectedClassName(), FName(TEXT("A")));
	HandInfo->SelectPrevious();
	TestEqual(TEXT("previous wraps to B"), HandInfo->GetSelectedClassName(), FName(TEXT("B")));
	HandInfo->SelectFirst();
	TestEqual(TEXT("first is A"), HandInfo->GetSelectedClassName(), FName(TEXT("A")));
	HandInfo->RemoveItem(FName(TEXT("A")));
	TestFalse(TEXT("A gone"), HandInfo->IsIn(FName(TEXT("A"))) &&
		HandInfo->GetClassName(0) == FName(TEXT("A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsHandSetSpec,
	"WoT.Hands.HandSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsHandSetSpec::RunTest(const FString&)
{
	UWoTHandSet* HandSet = NewObject<UWoTHandSet>();
	HandSet->Init(HandSet);
	HandSet->GetHand(0)->AddClassName(FName(TEXT("A")), 0);
	HandSet->GetHand(2)->AddClassName(FName(TEXT("B")), 0);
	HandSet->AddItem(FName(TEXT("A")), nullptr);
	HandSet->AddItem(FName(TEXT("B")), nullptr);
	HandSet->Select(0);
	TestEqual(TEXT("hand0"), HandSet->Selected, 0);
	TestEqual(TEXT("next non-empty hand is 2"), HandSet->GetNext(), 2);
	TestEqual(TEXT("prev non-empty hand is 2"), HandSet->GetPrevious(), 2);
	HandSet->Select(0);
	TestEqual(TEXT("still hand0"), HandSet->Selected, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsConfigSpec,
	"WoT.Hands.Config",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsConfigSpec::RunTest(const FString&)
{
	const UDataTable* Table = LoadObject<UDataTable>(
		nullptr, TEXT("/Game/WoT/Data/DT_WoTAngrealHands.DT_WoTAngrealHands"));
	TestNotNull(TEXT("DT_WoTAngrealHands loads"), Table);
	if (!Table)
	{
		return false;
	}
	TestEqual(TEXT("all shipped populated cells imported"), Table->GetRowNames().Num(), 42);

	UWoTHandSet* HandSet = NewObject<UWoTHandSet>();
	HandSet->Init(HandSet);
	HandSet->PopulateFromTable(Table);
	TestEqual(TEXT("fire hand starts with Balefire"),
		HandSet->GetHand(0)->ClassName[0], FName(TEXT("AngrealInvBalefire")));
	static const FName ShieldClasses[] = {
		FName(TEXT("AngrealInvShield")), FName(TEXT("AngrealInvFireShield")),
		FName(TEXT("AngrealInvAirShield")), FName(TEXT("AngrealInvEarthShield")),
		FName(TEXT("AngrealInvWaterShield")), FName(TEXT("AngrealInvSpiritShield"))};
	for (int32 SlotIndex = 0; SlotIndex < 6; ++SlotIndex)
	{
		TestEqual(FString::Printf(TEXT("shield slot %d"), SlotIndex),
			HandSet->GetHand(2)->ClassName[SlotIndex], ShieldClasses[SlotIndex]);
	}
	TestEqual(TEXT("seal class is unprefixed"),
		HandSet->GetHand(9)->ClassName[0], FName(TEXT("Seal")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsDriveSpec,
	"WoT.Hands.Drive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsDriveSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHandsTests_AlwaysSpawnParams());
	if (!PlayerController || !Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);
	PlayerController->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(PlayerController->GetHUD());
	if (!TestNotNull(TEXT("local controller owns the WoT HUD"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		WoTHandsTests_AlwaysSpawnParams());
	AWoTAngrealInvSeeker* Seeker = World->SpawnActor<AWoTAngrealInvSeeker>(
		WoTHandsTests_AlwaysSpawnParams());
	if (!Balefire || !Seeker)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTInventoryComponent* Inventory = Character->InventoryComponent;
	Inventory->AddInventory(Balefire);
	Inventory->AddInventory(Seeker);

	Character->SelectHand(1);
	TestTrue(TEXT("SelectHand drives Seeker on the server"),
		Inventory->SelectedItem.Get() == Seeker);
	// WOTPlayer.PerformSelection -> ClientSelectItemMessage (WOTPlayer.uc:1013-1023,
	// :1176-1210): every selection change lands "Selected the <Title> ter'angreal "
	// on the hand-message channel, composed from the WoT.int:45-46 fragments.
	TestEqual(TEXT("WoT.int:45 SelectedStr"), Character->SelectedStr,
		FString(TEXT("Selected the ")));
	TestEqual(TEXT("WoT.int:46 TerAngrealStr"), Character->TerAngrealStr,
		FString(TEXT(" ter'angreal ")));
	TestEqual(TEXT("selection posts the retail hand message"),
		HUD->HandMessage.Message, Character->SelectedStr +
			GetDefault<AWoTAngrealInvSeeker>()->Title + Character->TerAngrealStr);
	Character->SelectHand(0);
	TestTrue(TEXT("SelectHand drives Balefire on the server"),
		Inventory->SelectedItem.Get() == Balefire);
	TestEqual(TEXT("re-selection posts the new selection's hand message"),
		HUD->HandMessage.Message, Character->SelectedStr +
			GetDefault<AWoTAngrealInvBalefire>()->Title + Character->TerAngrealStr);
	Character->NextItem();
	TestTrue(TEXT("NextItem wraps to the next populated hand"),
		Inventory->SelectedItem.Get() == Seeker);

	// WOTPlayer.PerformSelection's feedback sounds (WOT.UI.SelectHand on a hand
	// change / WOT.UI.SelectArtifact on an item change) must be imported — playback
	// itself is unobservable headless.
	TestNotNull(TEXT("WOT.UI.SelectHand imported"),
		WoTSound::LoadBySubPath(FName(TEXT("WOT/SelectHand"))));
	TestNotNull(TEXT("WOT.UI.SelectArtifact imported"),
		WoTSound::LoadBySubPath(FName(TEXT("WOT/SelectArtifact"))));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsReconcileSpec,
	"WoT.Hands.Reconcile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsReconcileSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHandsTests_AlwaysSpawnParams());
	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		WoTHandsTests_AlwaysSpawnParams());
	AWoTAngrealInvSeeker* Seeker = World->SpawnActor<AWoTAngrealInvSeeker>(
		WoTHandsTests_AlwaysSpawnParams());
	if (!PlayerController || !Character || !Balefire || !Seeker)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);

	// Simulate owner-only replication: bypass AddInventory and place the replicated
	// array and selection directly on the receiving component.
	UWoTInventoryComponent* Inventory = Character->InventoryComponent;
	Inventory->Items.Add(Balefire);
	Inventory->Items.Add(Seeker);
	Inventory->SelectedItem = Seeker;
	Character->ReconcileHandSet();

	UWoTHandSet* HandSet = Character->AngrealHandSet;
	if (TestNotNull(TEXT("locally controlled pawn has a hand set"), HandSet))
	{
		TestTrue(TEXT("Balefire's hand reports it held"),
			HandSet->GetHand(0)->IsIn(FName(TEXT("AngrealInvBalefire"))));
		TestTrue(TEXT("Seeker's hand reports it held"),
			HandSet->GetHand(1)->IsIn(FName(TEXT("AngrealInvSeeker"))));
		TestEqual(TEXT("selected hand follows replicated SelectedItem"),
			HandSet->Selected, 1);
		TestEqual(TEXT("selected hand item follows replicated SelectedItem"),
			HandSet->GetSelectedHand()->GetSelectedClassName(),
			UWoTInventoryComponent::GetHandSetItemName(Seeker));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsContinuousScrollSpec,
	"WoT.Hands.ContinuousScroll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsContinuousScrollSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHandsTests_AlwaysSpawnParams());
	if (!PlayerController || !Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);
	UWoTInventoryComponent* Inventory = Character->InventoryComponent;
	// Hand 0: Balefire (slot 0), Fireball (1), Dart (4). Hand 1: Seeker (0), Decay (3).
	AWoTInventory* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Dart = World->SpawnActor<AWoTAngrealInvDart>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Seeker = World->SpawnActor<AWoTAngrealInvSeeker>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Decay = World->SpawnActor<AWoTAngrealInvDecay>(WoTHandsTests_AlwaysSpawnParams());
	if (!Balefire || !Fireball || !Dart || !Seeker || !Decay)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (AWoTInventory* Item : {Balefire, Fireball, Dart, Seeker, Decay})
	{
		Inventory->AddInventory(Item);
	}
	WoTTest::TickWorld(World, 0.1f);   // the hands register new inventory on a tick
	auto Sel = [Inventory]() -> FString
	{
		const AWoTInventory* Item = Inventory->SelectedItem.Get();
		return Item ? Item->GetClass()->GetName() : FString(TEXT("none"));
	};
	// Each hand remembers a selection that is not the edge a scroll enters at.
	// The hands follow the inventory's selection, so set them the way play does.
	Inventory->SelectItem(Decay);
	WoTTest::TickWorld(World, 0.05f);
	Inventory->SelectItem(Balefire);
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("hand 0 starts on Balefire"), *Sel()),
		Inventory->SelectedItem.Get() == Balefire);
	Character->NextItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("next within the hand reaches Fireball"), *Sel()),
		Inventory->SelectedItem.Get() == Fireball);
	Character->NextItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("next within the hand reaches Dart"), *Sel()), Inventory->SelectedItem.Get() == Dart);
	Character->NextItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("scrolling past the hand's last angreal enters the next hand at its first"), *Sel()),
		Inventory->SelectedItem.Get() == Seeker);
	Character->PrevItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("scrolling back past the hand's first angreal enters the previous hand at its last"), *Sel()),
		Inventory->SelectedItem.Get() == Dart);
	Character->PrevItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("previous within the hand reaches Fireball"), *Sel()), Inventory->SelectedItem.Get() == Fireball);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHandsSingleHandScrollWrapsSpec,
	"WoT.Hands.SingleHandScrollWraps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHandsSingleHandScrollWrapsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PlayerController = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTHandsTests_AlwaysSpawnParams());
	if (!PlayerController || !Character)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerController->SetAsLocalPlayerController();
	PlayerController->Possess(Character);
	UWoTInventoryComponent* Inventory = Character->InventoryComponent;
	// Every angreal is in hand 0: Balefire (slot 0), Fireball (1), Dart (4).
	AWoTInventory* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(WoTHandsTests_AlwaysSpawnParams());
	AWoTInventory* Dart = World->SpawnActor<AWoTAngrealInvDart>(WoTHandsTests_AlwaysSpawnParams());
	if (!Balefire || !Fireball || !Dart)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (AWoTInventory* Item : {Balefire, Fireball, Dart})
	{
		Inventory->AddInventory(Item);
	}
	WoTTest::TickWorld(World, 0.1f);   // the hands register new inventory on a tick
	auto Sel = [Inventory]() -> FString
	{
		const AWoTInventory* Item = Inventory->SelectedItem.Get();
		return Item ? Item->GetClass()->GetName() : FString(TEXT("none"));
	};
	Inventory->SelectItem(Dart);
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("the hand starts on Dart"), *Sel()),
		Inventory->SelectedItem.Get() == Dart);
	// The only hand is its own neighbour: scrolling wraps within it.
	Character->NextItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("next past the last wraps to the first"), *Sel()),
		Inventory->SelectedItem.Get() == Balefire);
	Character->PrevItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("previous past the first wraps to the last"), *Sel()),
		Inventory->SelectedItem.Get() == Dart);
	Character->PrevItem();
	TestTrue(FString::Printf(TEXT("%s (selected %s)"), TEXT("previous within the hand reaches Fireball"), *Sel()),
		Inventory->SelectedItem.Get() == Fireball);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
