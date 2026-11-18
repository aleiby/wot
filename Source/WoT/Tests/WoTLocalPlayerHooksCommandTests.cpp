#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "WoTLocalPlayerHooksTestFixture.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvDecay.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace
{
constexpr EAutomationTestFlags CommandHookFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName CommandHookOwner(TEXT("CommandHookSpec"));

FActorSpawnParameters CommandHookSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

struct FCommandHookWorld
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;
	UGameViewportClient* Viewport = nullptr;
	TSharedPtr<SViewport> Widget;
	TSharedPtr<FSceneViewport> Scene;
	ULocalPlayer* Player = nullptr;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;

	~FCommandHookWorld()
	{
		if (Player) { FWoTLocalPlayerHooks::DetachAll(*Player); }
		if (Viewport && Scene) { Viewport->RemoveAssociation(*Scene); }
		Scene.Reset();
		Widget.Reset();
		if (Context) { Context->GameViewport = nullptr; }
		WoTTest::DestroySpecWorld(World);
	}

	bool Init(FAutomationTestBase& Test)
	{
		World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
		if (!Test.TestNotNull(TEXT("command world"), World)) { return false; }
		Context = GEngine->GetWorldContextFromWorld(World);
		UGameInstance* GameInstance = World->GetGameInstance();
		if (!Test.TestNotNull(TEXT("world context"), Context)
			|| !Test.TestNotNull(TEXT("game instance"), GameInstance)) { return false; }
		Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
		Viewport->Init(*Context, GameInstance);
		Context->GameViewport = Viewport;
		Widget = SNew(SViewport);
		Scene = Viewport->CreateViewport(Widget);
		Scene->SetInitialSize(FIntPoint(640, 480));
		Viewport->AddAssociation(*Scene);
		FString Error;
		Player = GameInstance->CreateLocalPlayer(0, Error, false);
		Controller = World->SpawnActor<AWoTPlayerController>();
		Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 500), FRotator::ZeroRotator, CommandHookSpawnParams());
		if (!Test.TestNotNull(TEXT("local player"), Player)
			|| !Test.TestNotNull(TEXT("controller"), Controller)
			|| !Test.TestNotNull(TEXT("pawn"), Pawn)) { return false; }
		Controller->SetPlayer(Player);
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Frames(2);
		Pawn->EnsureAngrealHands();
		return Test.TestNotNull(TEXT("controller bindings"), Controller->InputComponent.Get())
			&& Test.TestNotNull(TEXT("pawn bindings"), Pawn->InputComponent.Get())
			&& Test.TestNotNull(TEXT("real hand model"), Pawn->CurrentHandSet.Get());
	}

	void Frames(int32 Count) { WoTTest::TickWorld(World, Count, 1.0f / 60.0f); }
	bool Attach()
	{
		return FWoTLocalPlayerHooks::Attach(*Player, CommandHookOwner, FWoTLocalPlayerConsumer());
	}
	bool Command(EWoTLocalPlayerCommand Action, int32 Index = 0)
	{
		return FWoTLocalPlayerHooks::Command(*Player, CommandHookOwner, Action, Index);
	}
	bool Binding(FName Name, EInputEvent Edge = IE_Pressed)
	{
		UInputComponent* Inputs[] = {
			Controller ? Controller->InputComponent.Get() : nullptr,
			Pawn ? Pawn->InputComponent.Get() : nullptr
		};
		for (UInputComponent* Input : Inputs)
		{
			if (!Input) { continue; }
			for (int32 Index = 0; Index < Input->GetNumActionBindings(); ++Index)
			{
				FInputActionBinding& Action = Input->GetActionBinding(Index);
				if (Action.GetActionName() == Name && Action.KeyEvent == Edge)
				{
					Action.ActionDelegate.Execute(FKey());
					return true;
				}
			}
		}
		return false;
	}
	template <typename T>
	T* Grant(int32 Charges)
	{
		T* Item = World->SpawnActor<T>(CommandHookSpawnParams());
		if (Item)
		{
			Item->GiveTo(Pawn);
			Item->CurCharges = Charges;
		}
		return Item;
	}
	void Select(AWoTInventory* Item)
	{
		Pawn->InventoryComponent->SelectItem(Item);
		Pawn->EnsureAngrealHands();
	}
};

const FWoTSelectionSlot* CommandHookFindSlot(
	const FWoTSelectionSnapshot& Snapshot, int32 Group, int32 Slot)
{
	return Snapshot.Slots.FindByPredicate([Group, Slot](const FWoTSelectionSlot& Entry)
	{
		return Entry.Group == Group && Entry.Slot == Slot;
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFireBindingParitySpec,
	"WoT.VR.Hooks.H8.FireBindingParity", CommandHookFlags)

bool FWoTHookFireBindingParitySpec::RunTest(const FString&)
{
	FCommandHookWorld F;
	if (!F.Init(*this)) { return false; }
	AWoTAngrealInvBalefire* Item = F.Grant<AWoTAngrealInvBalefire>(10);
	AWoTCharacter* Victim = F.World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 542), FRotator::ZeroRotator, CommandHookSpawnParams());
	if (!TestNotNull(TEXT("granted artifact"), Item)
		|| !TestNotNull(TEXT("damage target"), Victim)) { return false; }
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	F.Select(Item);
	TestFalse(TEXT("unregistered Fire is rejected"), F.Command(EWoTLocalPlayerCommand::Fire));
	TestFalse(TEXT("rejected Fire does not begin charging"), Item->bCasting);
	TestFalse(TEXT("rejected command creates no consumer"), FWoTLocalPlayerHooks::HasConsumers(F.Player));

	for (const bool bAdapter : {false, true})
	{
		const FString Label = bAdapter ? TEXT("adapter") : TEXT("binding without consumer");
		if (bAdapter && !TestTrue(TEXT("attach in an active match"), F.Attach())) { return false; }
		F.Pawn->SetActorLocation(FVector(0, 0, 500));
		F.Pawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		F.Controller->SetControlRotation(FRotator::ZeroRotator);
		Victim->SetActorLocation(FVector(300, 0, 542));
		Victim->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Victim->Health = 1000;
		F.Frames(60);
		const int32 Charges = Item->CurCharges;
		TestTrue(Label + TEXT(": Fire dispatches"), bAdapter
			? F.Command(EWoTLocalPlayerCommand::Fire) : F.Binding(TEXT("Fire")));
		TestTrue(Label + TEXT(": the real artifact starts charging"), Item->bCasting);
		F.Frames(150);
		TestEqual(Label + TEXT(": holding does not release the cast"), Victim->Health, 1000);
		TestTrue(Label + TEXT(": release dispatches"), bAdapter
			? F.Command(EWoTLocalPlayerCommand::CeaseFire) : F.Binding(TEXT("Fire"), IE_Released));
		F.Frames(2);
		TestFalse(Label + TEXT(": release ends charging"), Item->bCasting);
		TestEqual(Label + TEXT(": release applies ordinary full-charge damage"), Victim->Health, 600);
		TestEqual(Label + TEXT(": release spends one ordinary charge"), Item->CurCharges, Charges - 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookJumpBindingParitySpec,
	"WoT.VR.Hooks.H8.JumpBindingAndHeldSwim", CommandHookFlags)

bool FWoTHookJumpBindingParitySpec::RunTest(const FString&)
{
	FCommandHookWorld F;
	if (!F.Init(*this)) { return false; }
	TestFalse(TEXT("unregistered Jump is rejected"), F.Command(EWoTLocalPlayerCommand::Jump));
	TestFalse(TEXT("rejected Jump does not press the pawn's jump"), F.Pawn->bPressedJump);
	for (const bool bAdapter : {false, true})
	{
		const FString Label = bAdapter ? TEXT("adapter") : TEXT("binding without consumer");
		if (bAdapter && !TestTrue(TEXT("attach command consumer"), F.Attach())) { return false; }
		TestTrue(Label + TEXT(": Jump dispatches"), bAdapter
			? F.Command(EWoTLocalPlayerCommand::Jump) : F.Binding(TEXT("Jump")));
		TestTrue(Label + TEXT(": ordinary jump is pressed"), F.Pawn->bPressedJump);
		F.Pawn->SwimVerticalInputCountForTest = 0;
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			// A headless world has no water volume; restore swimming before each input pass.
			F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
			F.Frames(1);
		}
		TestEqual(Label + TEXT(": held swim-up enters ordinary input once per frame"),
			F.Pawn->SwimVerticalInputCountForTest, 10);
		TestTrue(Label + TEXT(": StopJumping dispatches"), bAdapter
			? F.Command(EWoTLocalPlayerCommand::StopJumping) : F.Binding(TEXT("Jump"), IE_Released));
		TestFalse(Label + TEXT(": jump is released"), F.Pawn->bPressedJump);
		F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		F.Frames(1);
		TestEqual(Label + TEXT(": release stops swim-up too"), F.Pawn->SwimVerticalInputCountForTest, 10);
	}
	FWoTLocalPlayerHooks::Detach(*F.Player, CommandHookOwner);
	TestTrue(TEXT("flat Jump binding survives detach"), F.Binding(TEXT("Jump")));
	TestTrue(TEXT("flat Jump works after detach"), F.Pawn->bPressedJump);
	F.Binding(TEXT("Jump"), IE_Released);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookSelectionSnapshotSpec,
	"WoT.VR.Hooks.H8.SelectionSnapshotAndBindingOrder", CommandHookFlags)

bool FWoTHookSelectionSnapshotSpec::RunTest(const FString&)
{
	FCommandHookWorld F;
	if (!F.Init(*this)) { return false; }
	// Acquisition order deliberately differs from the authored group/slot order.
	AWoTAngrealInvDart* Dart = F.Grant<AWoTAngrealInvDart>(23);
	AWoTAngrealInvDecay* Decay = F.Grant<AWoTAngrealInvDecay>(29);
	AWoTAngrealInvBalefire* Balefire = F.Grant<AWoTAngrealInvBalefire>(7);
	AWoTAngrealInvSeeker* Seeker = F.Grant<AWoTAngrealInvSeeker>(19);
	AWoTAngrealInvFireball* Fireball = F.Grant<AWoTAngrealInvFireball>(11);
	if (!TestNotNull(TEXT("Dart"), Dart) || !TestNotNull(TEXT("Decay"), Decay)
		|| !TestNotNull(TEXT("charged artifact"), Balefire) || !TestNotNull(TEXT("Seeker"), Seeker)
		|| !TestNotNull(TEXT("Fireball"), Fireball)) { return false; }
	F.Select(Balefire);
	const FWoTSelectionSnapshot Initial = FWoTLocalPlayerHooks::Selection(*F.Player);
	TestFalse(TEXT("reading selection creates no consumer"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	TestEqual(TEXT("snapshot includes all 42 authored slots, including unowned entries"), Initial.Slots.Num(), 42);
	TestEqual(TEXT("initial selected group"), Initial.SelectedGroup, 0);
	TestEqual(TEXT("initial selected slot"), Initial.SelectedSlot, 0);
	int32 AuthoredIndex = 0;
	for (int32 Group = 0; Group < UWoTHandSet::NumHands; ++Group)
	{
		const UWoTHandInfo* Hand = F.Pawn->CurrentHandSet->GetHand(Group);
		for (int32 Slot = 0; Slot < UWoTHandInfo::NumSlots; ++Slot)
		{
			if (Hand->ClassName[Slot].IsNone()) { continue; }
			if (!TestTrue(TEXT("snapshot contains each authored hand entry"), Initial.Slots.IsValidIndex(AuthoredIndex)))
			{
				return false;
			}
			const FWoTSelectionSlot& Entry = Initial.Slots[AuthoredIndex++];
			TestEqual(TEXT("snapshot group order follows the real hand model"), Entry.Group, Group);
			TestEqual(TEXT("snapshot slot order follows the real hand model"), Entry.Slot, Slot);
			TestEqual(TEXT("snapshot class comes from the authored slot"), Entry.ClassName, Hand->ClassName[Slot]);
			TestEqual(TEXT("snapshot ownership follows actual inventory"), Entry.bOwned, Hand->bHaveThis[Slot]);
		}
	}
	TestEqual(TEXT("no invented slots"), Initial.Slots.Num(), AuthoredIndex);
	struct FOwnedSlot { AWoTInventory* Item; int32 Group; int32 Slot; int32 Charges; };
	const FOwnedSlot Owned[] = {
		{Balefire, 0, 0, 7}, {Fireball, 0, 1, 11}, {Dart, 0, 4, 23},
		{Seeker, 1, 0, 19}, {Decay, 1, 3, 29}
	};
	for (const FOwnedSlot& Expected : Owned)
	{
		const FWoTSelectionSlot* Entry = CommandHookFindSlot(Initial, Expected.Group, Expected.Slot);
		if (!TestNotNull(TEXT("owned authored slot is present"), Entry)) { return false; }
		TestTrue(TEXT("granted actor is owned"), Entry->bOwned);
		TestEqual(TEXT("charges come from the real inventory actor"), Entry->Charges, Expected.Charges);
		TestEqual(TEXT("only the selected owned entry is marked selected"), Entry->bSelected, Expected.Item == Balefire);
	}
	const FWoTSelectionSlot* Unowned = CommandHookFindSlot(Initial, 2, 0);
	if (!TestNotNull(TEXT("unowned authored shield slot is present"), Unowned)) { return false; }
	TestFalse(TEXT("unowned slot remains unowned"), Unowned->bOwned);
	TestFalse(TEXT("unowned slot is not selected"), Unowned->bSelected);
	TestEqual(TEXT("unowned slot has no charges"), Unowned->Charges, 0);
	if (!TestTrue(TEXT("attach for selection commands"), F.Attach())) { return false; }

	for (const bool bForward : {true, false})
	{
		for (const bool bAdapter : {false, true})
		{
			F.Select(Balefire);
			for (int32 Step = 1; Step <= UE_ARRAY_COUNT(Owned); ++Step)
			{
				const int32 ExpectedIndex = bForward ? Step % UE_ARRAY_COUNT(Owned)
					: (UE_ARRAY_COUNT(Owned) - Step) % UE_ARRAY_COUNT(Owned);
				const FOwnedSlot& Expected = Owned[ExpectedIndex];
				const FString Label = FString::Printf(TEXT("%s %s step %d"),
					bAdapter ? TEXT("adapter") : TEXT("binding"), bForward ? TEXT("next") : TEXT("previous"), Step);
				TestTrue(Label + TEXT(": dispatch"), bAdapter
					? F.Command(bForward ? EWoTLocalPlayerCommand::NextItem : EWoTLocalPlayerCommand::PreviousItem)
					: F.Binding(bForward ? TEXT("NextItem") : TEXT("PrevItem")));
				TestTrue(Label + TEXT(": selects the same owned actor"), F.Pawn->InventoryComponent->SelectedItem.Get() == Expected.Item);
				const FWoTSelectionSnapshot Snapshot = FWoTLocalPlayerHooks::Selection(*F.Player);
				TestEqual(Label + TEXT(": selected group"), Snapshot.SelectedGroup, Expected.Group);
				TestEqual(Label + TEXT(": selected slot"), Snapshot.SelectedSlot, Expected.Slot);
			}
		}
	}
	struct FGroupCommand { EWoTLocalPlayerCommand Command; const TCHAR* Binding; int32 Index; };
	const FGroupCommand Groups[] = {
		{EWoTLocalPlayerCommand::NextGroup, TEXT("NextHand"), 0},
		{EWoTLocalPlayerCommand::PreviousGroup, TEXT("PrevHand"), 0},
		{EWoTLocalPlayerCommand::SelectGroup, TEXT("SelectHand2"), 1}
	};
	for (const FGroupCommand& Group : Groups)
	{
		for (const bool bAdapter : {false, true})
		{
			// Each group remembers its selected slot; prepare it through real selection.
			F.Select(Seeker);
			F.Select(Balefire);
			TestTrue(TEXT("group dispatch"), bAdapter ? F.Command(Group.Command, Group.Index) : F.Binding(Group.Binding));
			TestTrue(TEXT("group command matches binding and selects the remembered owned slot"),
				F.Pawn->InventoryComponent->SelectedItem.Get() == Seeker);
		}
	}
	F.Controller->ClientSetHUD(AWoTHUD::StaticClass());
	AWoTHUD* HUD = Cast<AWoTHUD>(F.Controller->GetHUD());
	if (!TestNotNull(TEXT("selection feedback HUD"), HUD)) { return false; }
	F.Select(Seeker);
	TestTrue(TEXT("slot dispatch"), F.Command(EWoTLocalPlayerCommand::SelectSlot, 3));
	TestTrue(TEXT("slot is resolved in the current group"), F.Pawn->InventoryComponent->SelectedItem.Get() == Decay);
	TestEqual(TEXT("slot selection retains ordinary hand feedback"), HUD->HandMessage.Message,
		F.Pawn->SelectedStr + Decay->Title + F.Pawn->TerAngrealStr);
	for (const int32 Invalid : {-1, 10})
	{
		F.Command(EWoTLocalPlayerCommand::SelectSlot, Invalid);
		F.Command(EWoTLocalPlayerCommand::SelectGroup, Invalid);
		TestTrue(TEXT("invalid group or slot leaves selection unchanged"), F.Pawn->InventoryComponent->SelectedItem.Get() == Decay);
	}
	Fireball->CurCharges = 3;
	F.Pawn->InventoryComponent->DeleteInventory(Dart);
	const FWoTSelectionSnapshot Updated = FWoTLocalPlayerHooks::Selection(*F.Player);
	const FWoTSelectionSlot* UpdatedFireball = CommandHookFindSlot(Updated, 0, 1);
	const FWoTSelectionSlot* RemovedDart = CommandHookFindSlot(Updated, 0, 4);
	if (!TestNotNull(TEXT("updated Fireball slot"), UpdatedFireball)
		|| !TestNotNull(TEXT("removed Dart retains authored slot"), RemovedDart)) { return false; }
	TestEqual(TEXT("fresh snapshot reads current charges"), UpdatedFireball->Charges, 3);
	TestFalse(TEXT("fresh snapshot reconciles removed inventory"), RemovedDart->bOwned);
	TestEqual(TEXT("previous snapshot remains a value copy"), CommandHookFindSlot(Initial, 0, 1)->Charges, 11);
	TestTrue(TEXT("previous snapshot keeps its original ownership"), CommandHookFindSlot(Initial, 0, 4)->bOwned);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookScoresAndMenuSpec,
	"WoT.VR.Hooks.H8.ScoreboardAndMenu", CommandHookFlags)

bool FWoTHookScoresAndMenuSpec::RunTest(const FString&)
{
	FCommandHookWorld F;
	if (!F.Init(*this) || !TestTrue(TEXT("attach command consumer"), F.Attach())) { return false; }
	TestTrue(TEXT("existing scoreboard binding dispatches"), F.Binding(TEXT("ShowScores")));
	TestTrue(TEXT("existing binding shows scores"), F.Controller->bShowScores);
	F.Binding(TEXT("ShowScores"));
	TestFalse(TEXT("second binding press hides scores"), F.Controller->bShowScores);
	TestTrue(TEXT("ShowScores command dispatches"), F.Command(EWoTLocalPlayerCommand::ShowScores));
	TestTrue(TEXT("ShowScores shows the same scoreboard"), F.Controller->bShowScores);
	F.Command(EWoTLocalPlayerCommand::ShowScores);
	TestTrue(TEXT("duplicate hold does not toggle scores off"), F.Controller->bShowScores);
	TestTrue(TEXT("HideScores command dispatches"), F.Command(EWoTLocalPlayerCommand::HideScores));
	TestFalse(TEXT("releasing a hidden-to-shown hold restores hidden"), F.Controller->bShowScores);
	F.Command(EWoTLocalPlayerCommand::HideScores);
	TestFalse(TEXT("duplicate release does not toggle scores on"), F.Controller->bShowScores);
	F.Binding(TEXT("ShowScores"));
	F.Command(EWoTLocalPlayerCommand::ShowScores);
	F.Command(EWoTLocalPlayerCommand::HideScores);
	TestTrue(TEXT("a hold preserves scores that were already visible"), F.Controller->bShowScores);
	F.Binding(TEXT("ShowScores"));
	for (const bool bAdapter : {false, true})
	{
		TestTrue(TEXT("menu dispatch"), bAdapter ? F.Command(EWoTLocalPlayerCommand::Menu) : F.Binding(TEXT("InGameMenu")));
		TestTrue(TEXT("menu command opens the existing menu"), F.Controller->IsInGameMenuOpen());
		TestTrue(TEXT("menu close dispatch"), bAdapter ? F.Command(EWoTLocalPlayerCommand::Menu) : F.Binding(TEXT("InGameMenu")));
		TestFalse(TEXT("second menu command closes the existing menu"), F.Controller->IsInGameMenuOpen());
	}
	F.Controller->ArmRespawnFire(nullptr, 10.0f);
	F.Command(EWoTLocalPlayerCommand::ShowScores);
	F.Frames(121);
	TestTrue(TEXT("death timer owns automatic score visibility"), F.Controller->IsDeathScoreboardAutoShowActive());
	F.Command(EWoTLocalPlayerCommand::HideScores);
	TestTrue(TEXT("releasing an earlier hold preserves death auto-show"), F.Controller->bShowScores);
	F.Controller->ShowScores();
	F.Command(EWoTLocalPlayerCommand::ShowScores);
	F.Controller->NotifyMatchEnded();
	F.Command(EWoTLocalPlayerCommand::HideScores);
	TestTrue(TEXT("releasing an earlier hold preserves match-end scores"), F.Controller->bShowScores);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookCommandReleaseSpec,
	"WoT.VR.Hooks.H8.ContextReleaseAndDetach", CommandHookFlags)

bool FWoTHookCommandReleaseSpec::RunTest(const FString&)
{
	// A release must reach the real cast, jump and scoreboard, and must not be replayed.
	for (int32 ReleaseMode = 0; ReleaseMode < 4; ++ReleaseMode)
	{
		FCommandHookWorld F;
		if (!F.Init(*this) || !TestTrue(TEXT("attach command consumer"), F.Attach())) { return false; }
		AWoTAngrealInvBalefire* Item = F.Grant<AWoTAngrealInvBalefire>(10);
		if (!TestNotNull(TEXT("held artifact"), Item)) { return false; }
		F.Select(Item);
		F.Frames(60);
		F.Command(EWoTLocalPlayerCommand::Fire);
		F.Command(EWoTLocalPlayerCommand::Jump);
		F.Command(EWoTLocalPlayerCommand::ShowScores);
		F.Frames(150);
		TestTrue(TEXT("cast is held before context change"), Item->bCasting);
		F.Pawn->TeleportTo(FVector(1500, 0, 520), FRotator::ZeroRotator, false, true);
		F.Frames(2);
		TestTrue(TEXT("a teleport does not interrupt the held cast"), Item->bCasting);
		const int32 Charges = Item->CurCharges;
		const FName Observer(TEXT("CommandObserver"));
		TestTrue(TEXT("read-only observer can coexist"),
			FWoTLocalPlayerHooks::Attach(*F.Player, Observer, FWoTLocalPlayerConsumer()));
		FWoTLocalPlayerHooks::ReleaseCommands(*F.Player, Observer);
		FWoTLocalPlayerHooks::Detach(*F.Player, Observer);
		TestTrue(TEXT("another owner's release cannot release this owner's cast"), Item->bCasting);
		switch (ReleaseMode)
		{
		case 0: FWoTLocalPlayerHooks::ReleaseCommands(*F.Player, CommandHookOwner); break;
		case 1: FWoTLocalPlayerHooks::Detach(*F.Player, CommandHookOwner); break;
		case 2:
			F.Command(EWoTLocalPlayerCommand::Menu);
			F.Frames(1); // The ordinary input layer observes the new context on its tick.
			break;
		case 3: F.Controller->UnPossess(); break;
		}
		TestFalse(TEXT("context change releases the charging artifact"), Item->bCasting);
		TestFalse(TEXT("context change releases pawn jump"), F.Pawn->bPressedJump);
		TestFalse(TEXT("context change restores scores"), F.Controller->bShowScores);
		TestEqual(TEXT("context change releases the cast exactly once"), Item->CurCharges, Charges - 1);
		FWoTLocalPlayerHooks::ReleaseCommands(*F.Player, CommandHookOwner);
		FWoTLocalPlayerHooks::Detach(*F.Player, CommandHookOwner);
		TestEqual(TEXT("repeated cleanup never spends another charge"), Item->CurCharges, Charges - 1);
		TestFalse(TEXT("commands cannot run after detach"), F.Command(EWoTLocalPlayerCommand::Jump));
		TestFalse(TEXT("last detach removes the consumer"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
		if (F.Controller->IsInGameMenuOpen()) { F.Controller->ResumeFromInGameMenu(); }
		if (!F.Controller->GetPawn()) { F.Controller->Possess(F.Pawn); }
		F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		F.Pawn->SwimVerticalInputCountForTest = 0;
		F.Frames(1);
		TestEqual(TEXT("cleanup releases held swim-up too"), F.Pawn->SwimVerticalInputCountForTest, 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookRespawnCommandSpec,
	"WoT.VR.Hooks.H8.FireAndJumpRespawn", CommandHookFlags)

bool FWoTHookRespawnCommandSpec::RunTest(const FString&)
{
	for (const bool bJump : {false, true})
	{
		for (const bool bAdapter : {false, true})
		{
			FCommandHookWorld F;
			if (!F.Init(*this)) { return false; }
			if (bAdapter && !TestTrue(TEXT("attach before pawn loss"), F.Attach())) { return false; }
			AWoTPlayerStart* Start = F.World->SpawnActor<AWoTPlayerStart>(
				FVector(1000, 0, 500), FRotator::ZeroRotator, CommandHookSpawnParams());
			if (!TestNotNull(TEXT("real respawn start"), Start)) { return false; }
			F.Controller->UnPossess();
			F.Pawn->Destroy();
			F.Pawn = nullptr;
			F.Controller->ArmRespawnFire(nullptr, 0.1f);
			const FWoTSelectionSnapshot Pawnless = FWoTLocalPlayerHooks::Selection(*F.Player);
			TestEqual(TEXT("pawnless selection has no group"), Pawnless.SelectedGroup, INDEX_NONE);
			TestEqual(TEXT("pawnless selection has no slot"), Pawnless.SelectedSlot, INDEX_NONE);
			TestTrue(TEXT("pawnless selection has no stale inventory"), Pawnless.Slots.IsEmpty());
			auto Press = [&]()
			{
				return bAdapter ? F.Command(bJump ? EWoTLocalPlayerCommand::Jump : EWoTLocalPlayerCommand::Fire)
					: F.Binding(bJump ? TEXT("Jump") : TEXT("Fire"));
			};
			TestTrue(TEXT("pawnless press dispatches"), Press());
			TestNull(TEXT("ordinary respawn delay rejects an early press"), F.Controller->GetPawn());
			TestEqual(TEXT("early press remains in Dying"), static_cast<int32>(F.Controller->GetPlayerStateMachineState()),
				static_cast<int32>(EWoTPlayerStateMachineState::Dying));
			if (bAdapter)
			{
				F.Command(bJump ? EWoTLocalPlayerCommand::StopJumping : EWoTLocalPlayerCommand::CeaseFire);
			}
			else { F.Binding(bJump ? TEXT("Jump") : TEXT("Fire"), IE_Released); }
			F.Frames(8);
			TestTrue(TEXT("fresh press after the gate dispatches"), Press());
			F.Pawn = Cast<AWoTCharacter>(F.Controller->GetPawn());
			if (!TestNotNull(TEXT("command requests an ordinary replacement pawn"), F.Pawn)) { return false; }
			TestEqual(TEXT("respawn returns to Playing"), static_cast<int32>(F.Controller->GetPlayerStateMachineState()),
				static_cast<int32>(EWoTPlayerStateMachineState::Playing));
			if (bAdapter)
			{
				F.Command(bJump ? EWoTLocalPlayerCommand::StopJumping : EWoTLocalPlayerCommand::CeaseFire);
				TestTrue(TEXT("consumer survives respawn"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
			}
			else { F.Binding(bJump ? TEXT("Jump") : TEXT("Fire"), IE_Released); }
			TestFalse(TEXT("respawn press does not leave the replacement jumping"), F.Pawn->bPressedJump);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookRestartCommandSpec,
	"WoT.VR.Hooks.H8.FireMatchRestart", CommandHookFlags)

bool FWoTHookRestartCommandSpec::RunTest(const FString&)
{
	for (const bool bAdapter : {false, true})
	{
		FCommandHookWorld F;
		if (!F.Init(*this)) { return false; }
		if (bAdapter && !TestTrue(TEXT("attach before match end"), F.Attach())) { return false; }
		AWoTGameMode* Game = F.World->GetAuthGameMode<AWoTGameMode>();
		if (!TestNotNull(TEXT("ordinary game mode"), Game)) { return false; }
		Game->bChangeLevels = true;
		Game->SetInstalledMapsForTesting({TEXT("Arena_02")});
		F.Controller->PlayerRestartGameDelay = 0.1f;
		Game->EndGame(TEXT("fraglimit"));
		TestTrue(TEXT("match ended"), F.Controller->IsMatchEnded());
		TestTrue(TEXT("frozen Fire dispatches"), bAdapter ? F.Command(EWoTLocalPlayerCommand::Fire) : F.Binding(TEXT("Fire")));
		TestTrue(TEXT("Fire preserves the ordinary restart freeze"), Game->PendingMapURL.IsEmpty());
		if (bAdapter) { F.Command(EWoTLocalPlayerCommand::CeaseFire); }
		else { F.Binding(TEXT("Fire"), IE_Released); }
		F.Frames(12);
		TestTrue(TEXT("ordinary restart gate elapsed"), F.Controller->IsRestartAllowed());
		TestTrue(TEXT("post-freeze Fire dispatches"), bAdapter ? F.Command(EWoTLocalPlayerCommand::Fire) : F.Binding(TEXT("Fire")));
		TestEqual(TEXT("Fire follows the binding's match restart path"), Game->Map, FString(TEXT("Arena_02")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookSelectGroupTeamSpec,
	"WoT.VR.Hooks.H8.SelectGroupTeamPrompt", CommandHookFlags)

bool FWoTHookSelectGroupTeamSpec::RunTest(const FString&)
{
	// While the team prompt is up, a neutral player's hand key chooses a team
	// (the binding's own routing); the command presses the same key.
	for (const bool bAdapter : {false, true})
	{
		const FString Label = bAdapter ? TEXT("adapter") : TEXT("binding");
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this, FVector(0, 0, 500), MOVE_Flying, AWoTBattleGameMode::StaticClass())) { return false; }
		AWoTBattleGameMode* Battle = F.World->GetAuthGameMode<AWoTBattleGameMode>();
		AWoTBattleInfo* Info = F.World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		if (!TestNotNull(Label + TEXT(": battle rules"), Battle)
			|| !TestNotNull(Label + TEXT(": battle info"), Info)) { return false; }
		Info->NumTeams = 2;
		Info->TeamClass[0] = AWoTCharacter::StaticClass();
		Info->TeamClass[1] = AWoTCharacter::StaticClass();
		Info->FinishSpawning(FTransform::Identity);
		Info->bTeleportersDisabled = false;
		Battle->Battle = Info;
		for (int32 Team = 0; Team < 2; ++Team)
		{
			AWoTPlayerStart* Start = F.World->SpawnActor<AWoTPlayerStart>(
				FVector(Team == 0 ? -2000.0 : 2000.0, 0, 100), FRotator::ZeroRotator, CommandHookSpawnParams());
			if (!TestNotNull(Label + TEXT(": team start"), Start)) { return false; }
			Start->TeamNumber = Team;
		}
		F.PC->ClientSetHUD(AWoTHUD::StaticClass());
		AWoTHUD* HUD = Cast<AWoTHUD>(F.PC->GetHUD());
		AWoTPlayerState* State = F.PC->GetPlayerState<AWoTPlayerState>();
		if (!TestNotNull(Label + TEXT(": HUD"), HUD) || !TestNotNull(Label + TEXT(": player state"), State)) { return false; }
		State->Team = 255;
		TestTrue(Label + TEXT(": the team prompt is up"), HUD->IsChoosingTeam());
		if (bAdapter && !TestTrue(TEXT("attach"), FWoTLocalPlayerHooks::Attach(
			*F.Player, CommandHookOwner, FWoTLocalPlayerConsumer()))) { return false; }
		AWoTCharacter* Neutral = F.Pawn;
		TestTrue(Label + TEXT(": the second hand key dispatches"), bAdapter
			? FWoTLocalPlayerHooks::Command(*F.Player, CommandHookOwner, EWoTLocalPlayerCommand::SelectGroup, 1)
			: F.Binding(TEXT("SelectHand2")));
		TestEqual(Label + TEXT(": the neutral player's key chooses the second team"), static_cast<int32>(State->Team), 1);
		TestTrue(Label + TEXT(": the team choice restarts the player"),
			F.PC->GetPawn() != nullptr && F.PC->GetPawn() != Neutral);
		F.Pawn = Cast<AWoTCharacter>(F.PC->GetPawn());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookPausedCommandsSpec,
	"WoT.VR.Hooks.H8.PausedPressesFollowBindings", CommandHookFlags)

bool FWoTHookPausedCommandsSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this, FVector(0, 0, 500), MOVE_Flying, AWoTGameMode::StaticClass())) { return false; }
	F.Pawn->EnsureAngrealHands();
	AWoTAngrealInvDart* Dart = F.World->SpawnActor<AWoTAngrealInvDart>(CommandHookSpawnParams());
	AWoTAngrealInvSeeker* Seeker = F.World->SpawnActor<AWoTAngrealInvSeeker>(CommandHookSpawnParams());
	if (!TestNotNull(TEXT("Dart"), Dart) || !TestNotNull(TEXT("Seeker"), Seeker)) { return false; }
	Dart->GiveTo(F.Pawn);
	Dart->CurCharges = 50;
	Seeker->GiveTo(F.Pawn);
	Seeker->CurCharges = 10;
	F.Pawn->InventoryComponent->SelectItem(Dart);
	F.Pawn->EnsureAngrealHands();
	if (!TestTrue(TEXT("attach"), FWoTLocalPlayerHooks::Attach(
		*F.Player, CommandHookOwner, FWoTLocalPlayerConsumer()))) { return false; }
	auto Command = [&F](EWoTLocalPlayerCommand Action, int32 Index = 0)
	{
		return FWoTLocalPlayerHooks::Command(*F.Player, CommandHookOwner, Action, Index);
	};
	auto RunsWhenPaused = [&F](const TCHAR* Name, EInputEvent Edge)
	{
		const FInputActionBinding* Binding = F.FindBinding(Name, Edge);
		return Binding && Binding->bExecuteWhenPaused;
	};

	// Holds that began before the pause.
	Command(EWoTLocalPlayerCommand::Fire);
	Command(EWoTLocalPlayerCommand::ShowScores);
	TestTrue(TEXT("the held cast is casting"), Dart->bCasting);
	TestTrue(TEXT("the held scoreboard shows"), F.PC->bShowScores);
	F.PC->Pause();
	if (!TestTrue(TEXT("the Pause key pauses the game"), F.World->IsPaused())) { return false; }

	// Releases run while paused, as every release binding does.
	TestTrue(TEXT("the Fire release binding runs while paused"), RunsWhenPaused(TEXT("Fire"), IE_Released));
	Command(EWoTLocalPlayerCommand::CeaseFire);
	TestFalse(TEXT("a paused release ends the cast"), Dart->bCasting);
	Command(EWoTLocalPlayerCommand::HideScores);
	TestFalse(TEXT("a paused release restores the scoreboard"), F.PC->bShowScores);

	// A press whose binding does not run while paused does nothing.
	TestFalse(TEXT("the Fire press binding does not run while paused"), RunsWhenPaused(TEXT("Fire"), IE_Pressed));
	Command(EWoTLocalPlayerCommand::Fire);
	TestFalse(TEXT("a paused Fire press does not cast"), Dart->bCasting);
	Command(EWoTLocalPlayerCommand::CeaseFire);
	struct FSelectionPress { EWoTLocalPlayerCommand Action; const TCHAR* Binding; int32 Index; };
	const FSelectionPress Presses[] = {
		{EWoTLocalPlayerCommand::NextItem, TEXT("NextItem"), 0},
		{EWoTLocalPlayerCommand::PreviousItem, TEXT("PrevItem"), 0},
		{EWoTLocalPlayerCommand::NextGroup, TEXT("NextHand"), 0},
		{EWoTLocalPlayerCommand::PreviousGroup, TEXT("PrevHand"), 0},
		{EWoTLocalPlayerCommand::SelectGroup, TEXT("SelectHand2"), 1},
		{EWoTLocalPlayerCommand::SelectSlot, nullptr, 0}
	};
	for (const FSelectionPress& Press : Presses)
	{
		const FString Label = Press.Binding ? FString(Press.Binding) : FString(TEXT("SelectSlot"));
		if (Press.Binding)
		{
			TestFalse(Label + TEXT(": the binding does not run while paused"), RunsWhenPaused(Press.Binding, IE_Pressed));
		}
		TestTrue(Label + TEXT(": dispatches"), Command(Press.Action, Press.Index));
		TestTrue(Label + TEXT(": a paused press leaves the selection"),
			F.Pawn->InventoryComponent->SelectedItem.Get() == Dart);
	}
	TestFalse(TEXT("the scoreboard binding does not run while paused"), RunsWhenPaused(TEXT("ShowScores"), IE_Pressed));
	Command(EWoTLocalPlayerCommand::ShowScores);
	TestFalse(TEXT("a paused scoreboard press leaves the scores hidden"), F.PC->bShowScores);
	Command(EWoTLocalPlayerCommand::HideScores);

	// The menu and Jump bindings run while paused, and so do their commands.
	TestTrue(TEXT("the menu binding runs while paused"), RunsWhenPaused(TEXT("InGameMenu"), IE_Pressed));
	Command(EWoTLocalPlayerCommand::Menu);
	TestTrue(TEXT("a paused menu press opens the menu"), F.PC->IsInGameMenuOpen());
	Command(EWoTLocalPlayerCommand::Menu);
	TestFalse(TEXT("a second paused menu press closes it"), F.PC->IsInGameMenuOpen());
	TestTrue(TEXT("the Jump binding runs while paused"), RunsWhenPaused(TEXT("Jump"), IE_Pressed));
	Command(EWoTLocalPlayerCommand::Jump);
	TestFalse(TEXT("a paused Jump press asks to resume, as its key does"), F.World->IsPaused());
	Command(EWoTLocalPlayerCommand::StopJumping);

	// Unpaused, the same presses act again.
	Command(EWoTLocalPlayerCommand::NextItem);
	TestTrue(TEXT("unpaused, the selection press acts"),
		F.Pawn->InventoryComponent->SelectedItem.Get() == Seeker);
	Command(EWoTLocalPlayerCommand::ShowScores);
	TestTrue(TEXT("unpaused, the scoreboard press acts"), F.PC->bShowScores);
	Command(EWoTLocalPlayerCommand::HideScores);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookUIOnlyCommandsSpec,
	"WoT.VR.Hooks.H8.UIOnlyInputBlocksPresses", CommandHookFlags)

bool FWoTHookUIOnlyCommandsSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this, FVector(0, 0, 500), MOVE_Flying, AWoTGameMode::StaticClass())) { return false; }
	F.Pawn->EnsureAngrealHands();
	AWoTAngrealInvDart* Dart = F.World->SpawnActor<AWoTAngrealInvDart>(CommandHookSpawnParams());
	AWoTAngrealInvSeeker* Seeker = F.World->SpawnActor<AWoTAngrealInvSeeker>(CommandHookSpawnParams());
	if (!TestNotNull(TEXT("Dart"), Dart) || !TestNotNull(TEXT("Seeker"), Seeker)) { return false; }
	Dart->GiveTo(F.Pawn);
	Dart->CurCharges = 50;
	Seeker->GiveTo(F.Pawn);
	Seeker->CurCharges = 10;
	F.Pawn->InventoryComponent->SelectItem(Dart);
	F.Pawn->EnsureAngrealHands();
	if (!TestTrue(TEXT("attach"), FWoTLocalPlayerHooks::Attach(
		*F.Player, CommandHookOwner, FWoTLocalPlayerConsumer()))) { return false; }
	auto Command = [&F](EWoTLocalPlayerCommand Action, int32 Index = 0)
	{
		return FWoTLocalPlayerHooks::Command(*F.Player, CommandHookOwner, Action, Index);
	};

	// An onboarding window takes UI-only input through the controller's overlay
	// seam: its key flush releases the held Jump, and no key reaches a binding.
	Command(EWoTLocalPlayerCommand::Jump);
	TestTrue(TEXT("Jump is held"), F.Pawn->bPressedJump);
	if (!TestTrue(TEXT("the controller's overlay seam is installed"), F.SetOverlayInput(true))) { return false; }
	TestTrue(TEXT("the window holds UI-only input, so no key reaches a binding"), F.ViewportIgnoresInput());
	TestFalse(TEXT("the window's key flush releases the held Jump"), F.Pawn->bPressedJump);
	Command(EWoTLocalPlayerCommand::Fire);
	TestFalse(TEXT("Fire does not cast behind the window"), Dart->bCasting);
	Command(EWoTLocalPlayerCommand::Jump);
	TestFalse(TEXT("Jump does not jump behind the window"), F.Pawn->bPressedJump);
	Command(EWoTLocalPlayerCommand::NextItem);
	TestTrue(TEXT("a selection press does nothing behind the window"),
		F.Pawn->InventoryComponent->SelectedItem.Get() == Dart);
	Command(EWoTLocalPlayerCommand::ShowScores);
	TestFalse(TEXT("the scoreboard press does nothing behind the window"), F.PC->bShowScores);
	Command(EWoTLocalPlayerCommand::Menu);
	TestFalse(TEXT("the menu key is the foreign window's, not the game's"), F.PC->IsInGameMenuOpen());
	Command(EWoTLocalPlayerCommand::CeaseFire);
	Command(EWoTLocalPlayerCommand::StopJumping);
	F.SetOverlayInput(false);
	TestFalse(TEXT("closing the window restores game input"), F.ViewportIgnoresInput());
	Command(EWoTLocalPlayerCommand::Fire);
	TestTrue(TEXT("with game input back, Fire casts"), Dart->bCasting);
	Command(EWoTLocalPlayerCommand::CeaseFire);

	// The in-game menu holds UI-only input as it does in play: Fire and Jump
	// stay out, and the menu key still closes the menu.
	Command(EWoTLocalPlayerCommand::Menu);
	TestTrue(TEXT("the menu opens"), F.PC->IsInGameMenuOpen());
	FInputModeUIOnly MenuInput;
	F.PC->SetInputMode(MenuInput);
	TestTrue(TEXT("the menu holds UI-only input"), F.ViewportIgnoresInput());
	F.Frames(30);
	Command(EWoTLocalPlayerCommand::Fire);
	TestFalse(TEXT("Fire does not cast behind the in-game menu"), Dart->bCasting);
	Command(EWoTLocalPlayerCommand::Jump);
	TestFalse(TEXT("Jump does not jump behind the in-game menu"), F.Pawn->bPressedJump);
	Command(EWoTLocalPlayerCommand::Menu);
	TestFalse(TEXT("the menu key closes the menu that holds UI-only input"), F.PC->IsInGameMenuOpen());
	F.PC->SetInputMode(FInputModeGameOnly());
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
