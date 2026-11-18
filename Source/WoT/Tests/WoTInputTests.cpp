// Hardware-input dispatch spec: Fire edges delivered through the REAL action
// bindings and UPlayerInput::ProcessInputStack. The direct Fire()/
// CeaseUsingAngreal() specs bypass this layer, which is where the stuck-fire
// regression lived: a controller-side press-only binding consumes its key on
// BOTH edges for lower components, starving a pawn-owned release binding.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"

namespace
{
	bool InputSpecHasActionMapping(FName Action, const FKey& Key)
	{
		TArray<FInputActionKeyMapping> Mappings;
		GetDefault<UInputSettings>()->GetActionMappingByName(Action, Mappings);
		return Mappings.ContainsByPredicate([&Key](const FInputActionKeyMapping& Mapping)
		{
			return Mapping.Key == Key;
		});
	}

	bool InputSpecHasAxisMapping(FName Axis, const FKey& Key, float Scale)
	{
		TArray<FInputAxisKeyMapping> Mappings;
		GetDefault<UInputSettings>()->GetAxisMappingByName(Axis, Mappings);
		return Mappings.ContainsByPredicate([&Key, Scale](const FInputAxisKeyMapping& Mapping)
		{
			return Mapping.Key == Key && FMath::IsNearlyEqual(Mapping.Scale, Scale);
		});
	}

	bool InputSpecHasBinding(const UInputComponent* Input, FName Action)
	{
		if (!Input) return false;
		for (int32 Index = 0; Index < Input->GetNumActionBindings(); ++Index)
		{
			if (Input->GetActionBinding(Index).GetActionName() == Action)
			{
				return true;
			}
		}
		return false;
	}

	// One hardware edge of the Fire key (LeftMouseButton), through the same
	// component stack APlayerController::BuildInputStack assembles while
	// possessing: the pawn's component is pushed first ("last dibs"), the
	// controller's last. UPlayerInput walks the stack top-down and applies key
	// consumption after each component, so the controller sees — and may
	// consume — the edge before the pawn does.
	void InputSpecSendFireEdge(AWoTPlayerController* PC,
		UInputComponent* PawnInput, EInputEvent Edge)
	{
		PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
			EKeys::LeftMouseButton, Edge, Edge == IE_Pressed ? 1.0f : 0.0f));
		TArray<UInputComponent*> Stack;
		Stack.Push(PawnInput);
		Stack.Push(PC->InputComponent);
		PC->PlayerInput->ProcessInputStack(Stack, 1.0f / 60.0f, false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDefaultBindingsSpec,
	"WoT.Input.RetailDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailDefaultBindingsSpec::RunTest(const FString&)
{
	const TPair<FName, FKey> Actions[] = {
		{TEXT("PrevHand"), EKeys::LeftBracket},
		{TEXT("NextHand"), EKeys::RightBracket},
		{TEXT("SShot"), EKeys::F9},
		{TEXT("ServerEditCitadel"), EKeys::F10},
		{TEXT("ToggleFullscreen"), EKeys::F12},
		{TEXT("ShowHelp"), EKeys::F1},
		{TEXT("SelectHand6"), EKeys::Q},
		{TEXT("SelectHand7"), EKeys::E},
		{TEXT("SelectHand8"), EKeys::Z},
		{TEXT("SelectHand9"), EKeys::X},
	};
	for (const TPair<FName, FKey>& Expected : Actions)
	{
		TestTrue(*FString::Printf(TEXT("%s maps to %s"),
			*Expected.Key.ToString(), *Expected.Value.ToString()),
			InputSpecHasActionMapping(Expected.Key, Expected.Value));
	}
	struct FAxisDefault { FName Axis; FKey Key; float Scale; };
	const FAxisDefault Axes[] = {
		{TEXT("MoveForward"), EKeys::Up, 1.0f},
		{TEXT("MoveForward"), EKeys::Down, -1.0f},
		{TEXT("MoveRight"), EKeys::Right, 1.0f},
		{TEXT("MoveRight"), EKeys::Left, -1.0f},
		{TEXT("Turn"), EKeys::Comma, -1.0f},
		{TEXT("Turn"), EKeys::Period, 1.0f},
		{TEXT("LookUp"), EKeys::Delete, 1.0f},
		{TEXT("LookUp"), EKeys::PageDown, -1.0f},
	};
	for (const FAxisDefault& Expected : Axes)
	{
		TestTrue(*FString::Printf(TEXT("%s axis resolves through %s"),
			*Expected.Key.ToString(), *Expected.Axis.ToString()),
			InputSpecHasAxisMapping(Expected.Axis, Expected.Key, Expected.Scale));
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>();
	PC->Possess(Pawn);
	PC->PlayerInput = NewObject<UPlayerInput>(PC);
	PC->SetupInputComponent();
	UInputComponent* PawnInput = NewObject<UInputComponent>(Pawn);
	Pawn->SetupPlayerInputComponent(PawnInput);
	for (const FName Action : {FName(TEXT("SShot")), FName(TEXT("ServerEditCitadel")),
		FName(TEXT("ToggleFullscreen")), FName(TEXT("ShowHelp"))})
	{
		TestTrue(*FString::Printf(TEXT("controller binds %s"), *Action.ToString()),
			InputSpecHasBinding(PC->InputComponent, Action));
	}
	for (const FName Action : {FName(TEXT("PrevHand")), FName(TEXT("NextHand"))})
	{
		TestTrue(*FString::Printf(TEXT("character binds %s"), *Action.ToString()),
			InputSpecHasBinding(PawnInput, Action));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHelpToggleInputSpec,
	"WoT.Input.HelpToggle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHelpToggleInputSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->ShowHelp();
	TestTrue(TEXT("F1 opens the retail help model"), PC->IsHelpOpen());
	PC->ShowHelp();
	TestFalse(TEXT("F1 toggles the retail help model closed"), PC->IsHelpOpen());
	PC->ShowHelp();
	PC->ToggleInGameMenu();
	TestFalse(TEXT("Escape closes help before opening the game menu"), PC->IsHelpOpen());
	TestFalse(TEXT("the same Escape does not also open the game menu"),
		PC->IsInGameMenuOpen());
	PC->ToggleInGameMenu();
	PC->ShowHelp();
	TestTrue(TEXT("ShowHelp replaces an open pause window"), PC->IsHelpOpen());
	TestFalse(TEXT("the replaced pause window is closed"), PC->IsInGameMenuOpen());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// Normal gameplay (Playing state, no citadel edit, not dead): a Fire press
// through the input stack starts angreal use, and the matching release ceases
// it. The release half is the regression trap: it only reaches the pawn if it
// is bound on the same input component as the controller's press.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInputFirePressReleaseStackSpec,
	"WoT.Input.FirePressReleaseThroughStack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInputFirePressReleaseStackSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A possessed player wired the way a local player would be: real controller
	// bindings (SetupInputComponent) + real pawn bindings (SetupPlayerInputComponent).
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);   // no floor
	PC->Possess(Pawn);
	PC->PlayerInput = NewObject<UPlayerInput>(PC);
	PC->SetupInputComponent();
	UInputComponent* PawnInput = NewObject<UInputComponent>(Pawn);
	Pawn->SetupPlayerInputComponent(PawnInput);
	if (!TestNotNull(TEXT("controller input component created"),
		PC->InputComponent.Get()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Precondition: the project maps Fire to LeftMouseButton (DefaultInput.ini),
	// so the simulated edges below exercise the shipped binding chain.
	TArray<FInputActionKeyMapping> FireMappings;
	GetDefault<UInputSettings>()->GetActionMappingByName(
		TEXT("Fire"), FireMappings);
	TestTrue(TEXT("Fire is mapped to LeftMouseButton"),
		FireMappings.ContainsByPredicate([](const FInputActionKeyMapping& M)
		{
			return M.Key == EKeys::LeftMouseButton;
		}));

	// Fireball in hand: the press edge must start angreal use (a projectile flies).
	World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;
	TestNotNull(TEXT("fireball picked up + selected"),
		Cast<AWoTAngrealInvFireball>(Inv->SelectedItem.Get()));

	InputSpecSendFireEdge(PC, PawnInput, IE_Pressed);
	AWoTFireballProjectile* Proj = nullptr;
	for (TActorIterator<AWoTFireballProjectile> It(World); It; ++It)
	{
		Proj = *It;
	}
	TestNotNull(TEXT("press through the input stack starts angreal use"), Proj);
	InputSpecSendFireEdge(PC, PawnInput, IE_Released);   // pair the press
	if (Proj)
	{
		Proj->Destroy();   // keep the phase-2 beam corridor deterministic
	}

	// Balefire in hand: hold-to-charge, release-to-fire. Once fully primed there
	// is no auto-fire — ONLY a delivered CeaseUsingAngreal makes the beam land,
	// so the victim's health drop is proof the release edge reached the pawn.
	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.7f);   // pickup + the 60/RoundsPerMinute cast gate
	Inv->SelectItem(Balefire);
	TestTrue(TEXT("balefire selected"), Inv->SelectedItem.Get() == Balefire);

	// One victim squarely on the +X beam axis at the caster's eye height.
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 142), FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;

	InputSpecSendFireEdge(PC, PawnInput, IE_Pressed);
	WoTTest::TickWorld(World, 2.5f);   // > PrimeTime (2.0): full charge, still held
	TestEqual(TEXT("held weave has not fired without a release"),
		Victim->Health, 1000);

	// THE REGRESSION: a pawn-owned release below the controller's press binding
	// was consumed before the pawn saw it, so the weave never ceased. The
	// controller-owned release delivers CeaseUsingAngreal and the beam fires.
	InputSpecSendFireEdge(PC, PawnInput, IE_Released);
	TestEqual(TEXT("release through the input stack ceases angreal use"),
		Victim->Health, 600);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif   // WITH_AUTOMATION_TESTS
