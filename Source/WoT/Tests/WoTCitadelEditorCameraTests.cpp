// Slice 2h: deterministic orbit/roam state, camera math, and lifecycle closure.
// Rendered camera feel and hardware input remain PIE gates.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTHandSet.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTTrap.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "WoTTestBattleClasses.h"

#include "Camera/CameraTypes.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"

namespace
{
	FActorSpawnParameters EditorCameraSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* EditorCameraSpecCreateWorld()
	{
		return WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	}

	void EditorCameraSpecAddBudgetRow(
		AWoTBudgetInfo* Budget, FName AdapterName, int32 Count)
	{
		FWoTBudgetResource Row;
		Row.ResourcePackageName = TEXT("WoTTraps");
		Row.ResourceAdapterName = AdapterName;
		Row.ItemCount = Count;
		Budget->Budget.Add(Row);
	}

	struct FEditorCameraSpecFixture
	{
		UWorld* World = nullptr;
		AWoTBattleGameMode* BattleMode = nullptr;
		AWoTBattleInfo* Battle = nullptr;
		AWoTBudgetInfo* Budget = nullptr;
		AWoTZoneVolume* Zone = nullptr;
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Character = nullptr;
		AWoTHUD* HUD = nullptr;
		AWoTTrap* Root = nullptr;
		AWoTSpear* RevealedSpear = nullptr;
		AWoTPortcullis* RevealedPortcullis = nullptr;
	};

	FEditorCameraSpecFixture EditorCameraSpecCreateFixture(int32 Mode)
	{
		FEditorCameraSpecFixture Fixture;
		Fixture.World = EditorCameraSpecCreateWorld();
		Fixture.BattleMode =
			Fixture.World->GetAuthGameMode<AWoTBattleGameMode>();

		Fixture.Battle = Fixture.World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		if (IsValid(Fixture.Battle))
		{
			Fixture.Battle->NumTeams = 2;
			Fixture.Battle->TeamClass[0] = AWoTCharacter::StaticClass();
			Fixture.Battle->TeamClass[1] = AWoTCharacter::StaticClass();
			Fixture.Battle->bTeleportersDisabled = true;
			Fixture.Battle->NumPlayers[1] = 1;
			Fixture.Battle->FinishSpawning(FTransform::Identity);
			Fixture.BattleMode->Battle = Fixture.Battle;
		}

		Fixture.Zone = Fixture.World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), FTransform::Identity);
		if (IsValid(Fixture.Zone))
		{
			Fixture.Zone->EditTeam = 1;
			Fixture.Zone->bAllowEditing = true;
			Fixture.Zone->Collision->SetBoxExtent(FVector(3000.0f));
			Fixture.Zone->FinishSpawning(FTransform::Identity);
		}

		Fixture.Budget = Fixture.World->SpawnActor<AWoTBudgetInfo>(
			EditorCameraSpecAlwaysSpawnParams());
		Fixture.Budget->Team = 1;
		EditorCameraSpecAddBudgetRow(
			Fixture.Budget, TEXT("SpearInventory"), 2);
		EditorCameraSpecAddBudgetRow(
			Fixture.Budget, TEXT("PortcullisInventory"), 1);

		AWoTSealAltar* Altar = Fixture.World->SpawnActor<AWoTSealAltar>(
			FVector::ZeroVector, FRotator::ZeroRotator,
			EditorCameraSpecAlwaysSpawnParams());
		if (IsValid(Altar))
		{
			Altar->Team = 1;
		}

		Fixture.Root = Fixture.World->SpawnActor<AWoTSpear>(
			FVector(100.0f, 100.0f, 100.0f), FRotator::ZeroRotator,
			EditorCameraSpecAlwaysSpawnParams());
		Fixture.RevealedSpear = Fixture.World->SpawnActor<AWoTSpear>(
			FVector(200.0f, 100.0f, 100.0f), FRotator::ZeroRotator,
			EditorCameraSpecAlwaysSpawnParams());
		Fixture.RevealedPortcullis = Fixture.World->SpawnActor<AWoTPortcullis>(
			FVector(300.0f, 100.0f, 100.0f), FRotator::ZeroRotator,
			EditorCameraSpecAlwaysSpawnParams());

		Fixture.Controller = Fixture.World->SpawnActor<AWoTTestSealNotifyController>(
			EditorCameraSpecAlwaysSpawnParams());
		Fixture.Controller->SetAsLocalPlayerController();
		Fixture.Controller->InitPlayerState();
		if (AWoTPlayerState* PlayerState =
			Fixture.Controller->GetPlayerState<AWoTPlayerState>())
		{
			PlayerState->Team = 1;
		}
		Fixture.Character = Fixture.World->SpawnActor<AWoTCharacter>(
			FVector(320.0f, -160.0f, 240.0f), FRotator(0.0f, 30.0f, 0.0f),
			EditorCameraSpecAlwaysSpawnParams());
		Fixture.Controller->Possess(Fixture.Character);
		Fixture.Controller->SetControlRotation(FRotator(-10.0f, 35.0f, 0.0f));
		Fixture.Controller->ClientSetHUD(AWoTHUD::StaticClass());
		Fixture.HUD = Cast<AWoTHUD>(Fixture.Controller->GetHUD());
		Fixture.Controller->CitadelBeginEdit(Mode);
		return Fixture;
	}

	enum class EEditorCameraSpecPresentation
	{
		Ghost,
		Reveal
	};

	void EditorCameraSpecPrimePresentation(FEditorCameraSpecFixture& Fixture,
		EEditorCameraSpecPresentation Presentation)
	{
		if (Presentation == EEditorCameraSpecPresentation::Ghost)
		{
			Fixture.Controller->SetCitadelPaletteSelection(TEXT("SpearInventory"));
			return;
		}
		Fixture.Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
		Fixture.Controller->SetHoveredCitadelResource(Fixture.RevealedPortcullis);
	}

	bool EditorCameraSpecAssertPresentationPrimed(FAutomationTestBase& Test,
		const TCHAR* Prefix, const FEditorCameraSpecFixture& Fixture,
		EEditorCameraSpecPresentation Presentation)
	{
		if (Presentation == EEditorCameraSpecPresentation::Ghost)
		{
			return Test.TestNotNull(
				FString::Printf(TEXT("%s precondition: ghost"), Prefix),
				Fixture.Controller->GetCitadelGhostPreview());
		}
		bool bPrimed = true;
		bPrimed &= Test.TestNull(
			FString::Printf(TEXT("%s precondition: no ghost in toggle state"), Prefix),
			Fixture.Controller->GetCitadelGhostPreview());
		bPrimed &= Test.TestTrue(
			FString::Printf(TEXT("%s precondition: reveal/highlight active"), Prefix),
			Fixture.RevealedPortcullis->GetCitadelEditorPresentation()
				!= EWoTCitadelResourcePresentation::Hidden);
		return bPrimed;
	}

	// The orbit body ghost (WOTPlayer.uc CitadelOrbitMode BeginState/EndState).
	bool EditorCameraSpecPawnGhosted(const AWoTCharacter* Character)
	{
		return IsValid(Character) && Character->IsCitadelOrbitGhosted()
			&& Character->IsHidden() && !Character->GetActorEnableCollision()
			&& Character->GetCharacterMovement()
			&& Character->GetCharacterMovement()->MovementMode == MOVE_None;
	}

	bool EditorCameraSpecPawnUnghosted(const AWoTCharacter* Character)
	{
		return !IsValid(Character)
			|| (!Character->IsCitadelOrbitGhosted() && !Character->IsHidden()
				&& Character->GetActorEnableCollision()
				&& Character->GetCharacterMovement()
				&& Character->GetCharacterMovement()->MovementMode != MOVE_None);
	}

	bool EditorCameraSpecAssertClean(FAutomationTestBase& Test,
		const TCHAR* Prefix, const FEditorCameraSpecFixture& Fixture)
	{
		if (!Test.TestTrue(FString::Printf(TEXT("%s: controller remains inspectable"), Prefix),
			IsValid(Fixture.Controller)))
		{
			return false;
		}
		bool bClean = true;
		bClean &= Test.TestFalse(FString::Printf(TEXT("%s: authority session ended"), Prefix),
			Fixture.Controller->IsEditingCitadel());
		bClean &= Test.TestFalse(FString::Printf(TEXT("%s: owner session ended"), Prefix),
			Fixture.Controller->IsOwnerEditingCitadel());
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: camera mode inactive"), Prefix),
			static_cast<int32>(Fixture.Controller->GetCitadelEditCameraMode()),
			static_cast<int32>(EWoTCitadelEditCameraMode::Inactive));
		bClean &= Test.TestTrue(FString::Printf(TEXT("%s: saved location cleared"), Prefix),
			Fixture.Controller->GetCitadelEditStartLocation().IsZero());
		bClean &= Test.TestNull(FString::Printf(TEXT("%s: edit zone cleared"), Prefix),
			Fixture.Controller->GetCitadelEditZone());
		bClean &= Test.TestNull(FString::Printf(TEXT("%s: ghost destroyed"), Prefix),
			Fixture.Controller->GetCitadelGhostPreview());
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: reveal cleared"), Prefix),
			static_cast<int32>(Fixture.RevealedPortcullis->GetCitadelEditorPresentation()),
			static_cast<int32>(EWoTCitadelResourcePresentation::Hidden));
		bClean &= Test.TestTrue(FString::Printf(TEXT("%s: no editor HUD mode"), Prefix),
			!IsValid(Fixture.HUD) || !Fixture.HUD->IsEditorMode());
		bClean &= Test.TestTrue(FString::Printf(TEXT("%s: editor input removed"), Prefix),
			!IsValid(Fixture.Character)
				|| !Fixture.Character->IsCitadelEditorInputActive());
		bClean &= Test.TestTrue(FString::Printf(TEXT("%s: orbit body ghost restored"), Prefix),
			EditorCameraSpecPawnUnghosted(Fixture.Character));
		bClean &= Test.TestTrue(FString::Printf(TEXT("%s: normal hands restored"), Prefix),
			!IsValid(Fixture.Character)
				|| Fixture.Character->CurrentHandSet
					== Fixture.Character->AngrealHandSet);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: budget EndEdit once"), Prefix),
			Fixture.Budget->PlayerEditCount, 0);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: root BeginEditing once"), Prefix),
			Fixture.Root->GetBeginEditingResourceCallCount(), 1);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: root EndEditing once"), Prefix),
			Fixture.Root->GetEndEditingResourceCallCount(), 1);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: spear BeginEditing once"), Prefix),
			Fixture.RevealedSpear->GetBeginEditingResourceCallCount(), 1);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: spear EndEditing once"), Prefix),
			Fixture.RevealedSpear->GetEndEditingResourceCallCount(), 1);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: portcullis BeginEditing once"), Prefix),
			Fixture.RevealedPortcullis->GetBeginEditingResourceCallCount(), 1);
		bClean &= Test.TestEqual(FString::Printf(TEXT("%s: portcullis EndEditing once"), Prefix),
			Fixture.RevealedPortcullis->GetEndEditingResourceCallCount(), 1);
		return bClean;
	}

	enum class EEditorCameraSpecTeardown
	{
		EnableTeleporters,
		Logout,
		Destroy
	};

	void EditorCameraSpecRunLifecycleTeardown(FAutomationTestBase& Test,
		EEditorCameraSpecTeardown Teardown,
		EEditorCameraSpecPresentation Presentation)
	{
		const TCHAR* TeardownName = Teardown == EEditorCameraSpecTeardown::EnableTeleporters
			? TEXT("EnableTeleporters")
			: Teardown == EEditorCameraSpecTeardown::Logout ? TEXT("Logout") : TEXT("Destroyed");
		const TCHAR* PresentationName = Presentation == EEditorCameraSpecPresentation::Ghost
			? TEXT("ghost") : TEXT("reveal");
		const FString Prefix = FString::Printf(
			TEXT("%s %s"), TeardownName, PresentationName);
		FEditorCameraSpecFixture Fixture = EditorCameraSpecCreateFixture(1);
		EditorCameraSpecPrimePresentation(Fixture, Presentation);
		EditorCameraSpecAssertPresentationPrimed(
			Test, *Prefix, Fixture, Presentation);

		if (Teardown != EEditorCameraSpecTeardown::Destroy)
		{
			if (Teardown == EEditorCameraSpecTeardown::EnableTeleporters)
			{
				Fixture.BattleMode->EnableTeleporters();
			}
			else
			{
				Fixture.BattleMode->Logout(Fixture.Controller);
			}
			EditorCameraSpecAssertClean(Test, *Prefix, Fixture);
			if (IsValid(Fixture.Controller))
			{
				Fixture.Controller->EndEditSession();
			}
			Test.TestEqual(FString::Printf(TEXT("%s cleanup remains once"), *Prefix),
				Fixture.Root->GetEndEditingResourceCallCount(), 1);
			WoTTest::TickWorld(Fixture.World, 1);
			WoTTest::DestroySpecWorld(Fixture.World);
			return;
		}

		TWeakObjectPtr<AWoTTrap> Ghost = Fixture.Controller->GetCitadelGhostPreview();
		bool bObservedExit = false;
		bool bObservedAuthorityEnded = false;
		bool bObservedCameraCleared = false;
		Fixture.Controller->OnCitadelEditSessionChanged.AddLambda(
			[&](bool bEditing, uint8)
			{
				if (!bEditing)
				{
					bObservedExit = true;
					bObservedAuthorityEnded =
						!Fixture.Controller->IsEditingCitadel();
					bObservedCameraCleared = Fixture.Controller
						->GetCitadelEditCameraMode()
						== EWoTCitadelEditCameraMode::Inactive
						&& Fixture.Controller->GetCitadelEditZone() == nullptr
						&& Fixture.Controller->GetCitadelEditStartLocation().IsZero();
				}
			});
		Fixture.Controller->Destroy();
		Test.TestTrue(FString::Printf(TEXT("%s broadcasts owner exit"), *Prefix),
			bObservedExit);
		Test.TestTrue(FString::Printf(
			TEXT("%s clears authority session before invalidation"), *Prefix),
			bObservedAuthorityEnded);
		Test.TestTrue(FString::Printf(
			TEXT("%s clears camera session before invalidation"), *Prefix),
			bObservedCameraCleared);
		Test.TestFalse(FString::Printf(TEXT("%s destroys ghost"), *Prefix),
			Ghost.IsValid());
		Test.TestEqual(FString::Printf(TEXT("%s clears reveal"), *Prefix),
			static_cast<int32>(Fixture.RevealedPortcullis->GetCitadelEditorPresentation()),
			static_cast<int32>(EWoTCitadelResourcePresentation::Hidden));
		Test.TestTrue(FString::Printf(TEXT("%s leaves no editor HUD mode"), *Prefix),
			!IsValid(Fixture.HUD) || !Fixture.HUD->IsEditorMode());
		Test.TestTrue(FString::Printf(
			TEXT("%s leaves no editor input routing"), *Prefix),
			!IsValid(Fixture.Character)
				|| !Fixture.Character->IsCitadelEditorInputActive());
		Test.TestTrue(FString::Printf(
			TEXT("%s restores the orbit body ghost"), *Prefix),
			EditorCameraSpecPawnUnghosted(Fixture.Character));
		Test.TestEqual(FString::Printf(TEXT("%s balances Budget EndEdit"), *Prefix),
			Fixture.Budget->PlayerEditCount, 0);
		Test.TestEqual(FString::Printf(TEXT("%s cleanup remains once"), *Prefix),
			Fixture.Root->GetEndEditingResourceCallCount(), 1);
		Test.TestEqual(FString::Printf(
			TEXT("%s ends each team-zone spear root once"), *Prefix),
			Fixture.RevealedSpear->GetEndEditingResourceCallCount(), 1);
		Test.TestEqual(FString::Printf(
			TEXT("%s ends team-zone portcullis once"), *Prefix),
			Fixture.RevealedPortcullis->GetEndEditingResourceCallCount(), 1);
		WoTTest::TickWorld(Fixture.World, 1);
		WoTTest::DestroySpecWorld(Fixture.World);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorCameraEntryRestoreSpec,
	"WoT.Citadel.EditorCamera.EntryAndRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorCameraEntryRestoreSpec::RunTest(const FString&)
{
	FEditorCameraSpecFixture Orbit = EditorCameraSpecCreateFixture(1);
	const FVector OrbitStart = FVector(320.0f, -160.0f, 240.0f);
	const FRotator OrbitStartRotation(-10.0f, 35.0f, 0.0f);
	TestTrue(TEXT("orbit entry accepted"), Orbit.Controller->IsEditingCitadel());
	TestEqual(TEXT("mode 1 selects orbit"),
		static_cast<int32>(Orbit.Controller->GetCitadelEditCameraMode()),
		static_cast<int32>(EWoTCitadelEditCameraMode::Orbit));
	TestEqual(TEXT("orbit saves entry location"),
		Orbit.Controller->GetCitadelEditStartLocation(), OrbitStart);
	TestTrue(TEXT("orbit saves entry view rotation"),
		Orbit.Controller->GetCitadelEditStartRotation().Equals(OrbitStartRotation));
	// CitadelOrbitMode.BeginState (WOTPlayer.uc:5262-5272): the body ghosts — hidden,
	// non-colliding, PHYS_None, drowning disarmed.
	TestTrue(TEXT("orbit entry ghosts the body"),
		EditorCameraSpecPawnGhosted(Orbit.Character));
	TestEqual(TEXT("orbit entry disarms drowning (UnderWaterTime -1)"),
		Orbit.Character->UnderWaterTime, -1.0f);
	Orbit.Character->SetActorLocation(FVector(900.0f, 800.0f, 700.0f));
	Orbit.Controller->SetControlRotation(FRotator(20.0f, 90.0f, 0.0f));
	Orbit.Controller->CitadelEndEdit();
	TestEqual(TEXT("orbit exit restores player location"),
		Orbit.Character->GetActorLocation(), OrbitStart);
	TestTrue(TEXT("orbit exit restores view rotation"),
		Orbit.Controller->GetControlRotation().Equals(OrbitStartRotation));
	// EndState (WOTPlayer.uc:5277-5289): collision/visibility back, PHYS_Falling,
	// default UnderWaterTime.
	TestTrue(TEXT("orbit exit restores the body"),
		EditorCameraSpecPawnUnghosted(Orbit.Character));
	TestEqual(TEXT("orbit exit resumes PHYS_Falling"),
		static_cast<int32>(
			Orbit.Character->GetCharacterMovement()->MovementMode.GetValue()),
		static_cast<int32>(MOVE_Falling));
	TestEqual(TEXT("orbit exit restores default UnderWaterTime"),
		Orbit.Character->UnderWaterTime,
		GetDefault<AWoTCharacter>()->UnderWaterTime);
	WoTTest::TickWorld(Orbit.World, 1);
	WoTTest::DestroySpecWorld(Orbit.World);

	FEditorCameraSpecFixture Roam = EditorCameraSpecCreateFixture(0);
	TestEqual(TEXT("mode 0 selects roam"),
		static_cast<int32>(Roam.Controller->GetCitadelEditCameraMode()),
		static_cast<int32>(EWoTCitadelEditCameraMode::Roam));
	TestTrue(TEXT("roam zeroes EditStart location"),
		Roam.Controller->GetCitadelEditStartLocation().IsZero());
	TestTrue(TEXT("roam zeroes EditStart rotation"),
		Roam.Controller->GetCitadelEditStartRotation().Equals(
			FRotator::ZeroRotator));
	// CitadelRoamMode keeps the walking body (retail roam MOVES the player).
	TestFalse(TEXT("roam never ghosts the body"),
		Roam.Character->IsCitadelOrbitGhosted());
	TestFalse(TEXT("roam body stays visible"), Roam.Character->IsHidden());
	const FVector RoamedLocation(710.0f, 620.0f, 530.0f);
	const FRotator RoamedRotation(15.0f, 80.0f, 0.0f);
	Roam.Character->SetActorLocation(RoamedLocation);
	Roam.Controller->SetControlRotation(RoamedRotation);
	Roam.Controller->CitadelEndEdit();
	TestEqual(TEXT("roam exit does not restore location"),
		Roam.Character->GetActorLocation(), RoamedLocation);
	TestTrue(TEXT("roam exit does not restore view rotation"),
		Roam.Controller->GetControlRotation().Equals(RoamedRotation));
	WoTTest::TickWorld(Roam.World, 1);
	WoTTest::DestroySpecWorld(Roam.World);
	return true;
}

// CitadelOrbitMode consumes movement input (WOTPlayer.uc:5262-5272): strafe and jump
// delivered through the REAL binding chain (UPlayerInput::ProcessInputStack over the
// controller + pawn input components, the WoT.Input harness pattern) must not drive
// the ghosted body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorCameraOrbitInputSpec,
	"WoT.Citadel.EditorCamera.OrbitConsumesMovementInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorCameraOrbitInputSpec::RunTest(const FString&)
{
	FEditorCameraSpecFixture Fixture = EditorCameraSpecCreateFixture(1);
	AWoTPlayerController* PC = Fixture.Controller;
	AWoTCharacter* Body = Fixture.Character;
	PC->PlayerInput = NewObject<UPlayerInput>(PC);
	PC->SetupInputComponent();
	UInputComponent* PawnInput = NewObject<UInputComponent>(Body);
	Body->SetupPlayerInputComponent(PawnInput);
	if (!TestNotNull(TEXT("controller input component created"),
		PC->InputComponent.Get()))
	{
		WoTTest::DestroySpecWorld(Fixture.World);
		return false;
	}
	auto ProcessHeldKeys = [&]()
	{
		TArray<UInputComponent*> Stack;
		Stack.Push(PawnInput);
		Stack.Push(PC->InputComponent);
		PC->PlayerInput->ProcessInputStack(Stack, 1.0f / 60.0f, false);
	};

	const FVector BodyLocation = Body->GetActorLocation();
	// Hold strafe (D -> MoveRight axis) and press jump (SpaceBar -> the controller-
	// owned Jump binding) simultaneously.
	PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		EKeys::D, IE_Pressed, 1.0f));
	PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		EKeys::SpaceBar, IE_Pressed, 1.0f));
	ProcessHeldKeys();
	TestTrue(TEXT("orbit consumes the strafe axis (no pending movement input)"),
		Body->GetPendingMovementInputVector().IsNearlyZero());
	TestFalse(TEXT("orbit consumes the jump press before the pawn"),
		Body->bPressedJump);
	TestEqual(TEXT("body stays PHYS_None under held input"),
		static_cast<int32>(Body->GetCharacterMovement()->MovementMode.GetValue()),
		static_cast<int32>(MOVE_None));
	WoTTest::TickWorld(Fixture.World, 1);
	TestEqual(TEXT("held input does not move the ghosted body"),
		Body->GetActorLocation(), BodyLocation);
	TestTrue(TEXT("body stays ghosted under held input"),
		EditorCameraSpecPawnGhosted(Body));

	PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		EKeys::D, IE_Released, 0.0f));
	PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
		EKeys::SpaceBar, IE_Released, 0.0f));
	ProcessHeldKeys();
	PC->CitadelEndEdit();
	TestTrue(TEXT("exit after held input restores the body"),
		EditorCameraSpecPawnUnghosted(Body));

	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorCameraMathSpec,
	"WoT.Citadel.EditorCamera.DistanceMathAndCurrentZone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorCameraMathSpec::RunTest(const FString&)
{
	TestEqual(TEXT("distance clamps to 5 feet"),
		AWoTPlayerController::ClampCitadelEditViewDistanceFeet(-1.0f), 5.0f);
	TestEqual(TEXT("distance clamps to 100 feet"),
		AWoTPlayerController::ClampCitadelEditViewDistanceFeet(101.0f), 100.0f);
	TestEqual(TEXT("feet convert to UE1 UU"),
		AWoTPlayerController::CitadelEditViewDistanceFeetToUU(32.0f), 512.0f);
	TestEqual(TEXT("forward update retains exported /16"),
		AWoTPlayerController::AdjustCitadelEditViewDistanceFeet(
			50.0f, 16.0f, 1.0f), 49.0f);
	TestEqual(TEXT("forward update clamps at lower bound"),
		AWoTPlayerController::AdjustCitadelEditViewDistanceFeet(
			5.0f, 16.0f, 1.0f), 5.0f);
	TestEqual(TEXT("backward update clamps at upper bound"),
		AWoTPlayerController::AdjustCitadelEditViewDistanceFeet(
			100.0f, -16.0f, 1.0f), 100.0f);

	FEditorCameraSpecFixture Fixture = EditorCameraSpecCreateFixture(1);
	Fixture.Zone->SetActorLocation(FVector(1000.0f, 2000.0f, 3000.0f));
	Fixture.Zone->EditViewAngle = FRotator(-30.0f, 90.0f, 0.0f);
	Fixture.Zone->EditViewDistance = 40.0f;
	TestTrue(TEXT("accepted entry retains its current zone"),
		Fixture.Controller->GetCitadelEditZone() == Fixture.Zone);
	FMinimalViewInfo View;
	Fixture.Controller->CalcCamera(1.0f / 60.0f, View);
	FVector ExpectedLocation;
	FRotator ExpectedRotation;
	AWoTPlayerController::CalculateCitadelOrbitView(
		Fixture.Zone->GetActorLocation(), Fixture.Zone->EditViewAngle,
		Fixture.Zone->EditViewDistance, ExpectedLocation, ExpectedRotation);
	TestTrue(TEXT("current zone angle drives orbit view rotation"),
		View.Rotation.Equals(ExpectedRotation));
	TestTrue(TEXT("current zone distance drives orbit view location"),
		View.Location.Equals(ExpectedLocation));
	Fixture.Controller->AdjustCitadelOrbitDistance(16.0f, 1.0f);
	TestEqual(TEXT("orbit adjustment mutates current zone feet"),
		Fixture.Zone->EditViewDistance, 39.0f);
	Fixture.Controller->CitadelEndEdit();
	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorCameraLifecycleSpec,
	"WoT.Citadel.EditorCamera.LifecycleCleanupAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorCameraLifecycleSpec::RunTest(const FString&)
{
	for (EEditorCameraSpecTeardown Teardown :
		{EEditorCameraSpecTeardown::EnableTeleporters,
			EEditorCameraSpecTeardown::Logout,
			EEditorCameraSpecTeardown::Destroy})
	{
		EditorCameraSpecRunLifecycleTeardown(
			*this, Teardown, EEditorCameraSpecPresentation::Ghost);
		EditorCameraSpecRunLifecycleTeardown(
			*this, Teardown, EEditorCameraSpecPresentation::Reveal);
	}
	{
		FEditorCameraSpecFixture Fixture = EditorCameraSpecCreateFixture(1);
		EditorCameraSpecPrimePresentation(
			Fixture, EEditorCameraSpecPresentation::Ghost);
		Fixture.Battle->bTeleportersDisabled = false;
		WoTTest::TickWorld(Fixture.World, 1);
		EditorCameraSpecAssertClean(*this, TEXT("forced phase transition"), Fixture);
		if (IsValid(Fixture.Controller))
		{
			Fixture.Controller->EndEditSession();
		}
		TestEqual(TEXT("forced transition cleanup remains once"),
			Fixture.Root->GetEndEditingResourceCallCount(), 1);
		WoTTest::DestroySpecWorld(Fixture.World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CCitadelOrbitViewCycleSpec,
	"WoT.Parity.P1C.CitadelOrbitViewCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTP1CCitadelOrbitViewCycleSpec::RunTest(const FString&)
{
	FEditorCameraSpecFixture Fixture = EditorCameraSpecCreateFixture(1);
	Fixture.Zone->ProjectedZoneIndex = 10;
	Fixture.Zone->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	auto SpawnZone = [&](int32 ZoneIndex, uint8 Team, const FVector& Location)
	{
		AWoTZoneVolume* Zone = Fixture.World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), FTransform(Location));
		Zone->ProjectedZoneIndex = ZoneIndex;
		Zone->EditTeam = Team;
		Zone->bAllowEditing = true;
		Zone->Collision->SetBoxExtent(FVector(100.0f));
		Zone->FinishSpawning(FTransform(Location));
		return Zone;
	};
	AWoTZoneVolume* EnemyZone = SpawnZone(20, 2, FVector(2000.0f, 0.0f, 0.0f));
	AWoTZoneVolume* NeutralZone = SpawnZone(30, 255, FVector(3000.0f, 0.0f, 0.0f));
	AWoTZoneVolume* TeamZone = SpawnZone(40, 1, FVector(4000.0f, 0.0f, 0.0f));
	AWoTTestSealNotifyController* Recording =
		CastChecked<AWoTTestSealNotifyController>(Fixture.Controller);
	const int32 SoundCount = Recording->UISounds.Num();
	FMinimalViewInfo Before;
	Fixture.Controller->CalcCamera(1.0f / 60.0f, Before);

	Fixture.Controller->ViewUp();
	TestTrue(TEXT("Equals command skips the opposing team and selects the next visible zone"),
		Fixture.Controller->GetCitadelEditZone() == NeutralZone);
	FMinimalViewInfo After;
	Fixture.Controller->CalcCamera(1.0f / 60.0f, After);
	TestFalse(TEXT("the orbit camera moves to the selected zone"),
		After.Location.Equals(Before.Location));
	TestEqual(TEXT("zone selection plays one edit-select cue"),
		Recording->UISounds.Num(), SoundCount + 1);
	if (Recording->UISounds.Num() > SoundCount)
	{
		TestEqual(TEXT("zone selection uses the edit-select cue"),
			Recording->UISounds.Last(), FName(TEXT("WOT/Editor/EditSelect")));
	}

	Fixture.Controller->ViewDown();
	TestTrue(TEXT("Minus command wraps to the previous visible zone"),
		Fixture.Controller->GetCitadelEditZone() == Fixture.Zone);
	TestTrue(TEXT("opposing-team zone remains excluded"),
		Fixture.Controller->GetCitadelEditZone() != EnemyZone);
	TestTrue(TEXT("team zone is available to the orbit cycle"), TeamZone != nullptr);
	Fixture.Controller->SwitchView(2);
	TestTrue(TEXT("SwitchView selects by visible-zone ordinal"),
		Fixture.Controller->GetCitadelEditZone() == TeamZone);
	Fixture.Controller->NextView();
	TestTrue(TEXT("NextView wraps from the last visible zone"),
		Fixture.Controller->GetCitadelEditZone() == Fixture.Zone);
	Fixture.Controller->PrevView();
	TestTrue(TEXT("PrevView wraps from the first visible zone"),
		Fixture.Controller->GetCitadelEditZone() == TeamZone);

	Fixture.Controller->CitadelEndEdit();
	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
