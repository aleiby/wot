// The controller layer end to end: a real local player, Enhanced Input, the
// production contexts and handlers. Gamepad keys enter through the
// controller's InputKey exactly as the platform delivers them.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "WoTTestWorld.h"
#include "../Gamepad/WoTGamepadLayer.h"
#include "../Gamepad/WoTGamepadLayout.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "WoTTestBattleClasses.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputKeyEventArgs.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace
{
constexpr EAutomationTestFlags Gamepad_WorldFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FGamepad_Fixture
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;
	UGameInstance* GameInstance = nullptr;
	UGameViewportClient* ViewportClient = nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<FSceneViewport> SceneViewport;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;

	~FGamepad_Fixture()
	{
		if (ViewportClient && SceneViewport) { ViewportClient->RemoveAssociation(*SceneViewport); }
		SceneViewport.Reset();
		ViewportWidget.Reset();
		if (Context) { Context->GameViewport = nullptr; }
		WoTTest::DestroySpecWorld(World);
	}

	bool Initialize(FAutomationTestBase& Test,
		TSubclassOf<AGameModeBase> GameModeClass = AWoTGameMode::StaticClass())
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = GameModeClass;
		World = WoTTest::CreateSpecWorld(Options);
		Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr;
		GameInstance = World ? World->GetGameInstance() : nullptr;
		ViewportClient = Context && GameInstance
			? NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass) : nullptr;
		if (!Test.TestNotNull(TEXT("world"), World) || !Test.TestNotNull(TEXT("viewport client"), ViewportClient))
		{
			return false;
		}
		ViewportClient->Init(*Context, GameInstance);
		Context->GameViewport = ViewportClient;
		ViewportWidget = SNew(SViewport);
		SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
		SceneViewport->SetInitialSize(FIntPoint(640, 480));
		ViewportClient->AddAssociation(*SceneViewport);
		FString PlayerError;
		ULocalPlayer* LocalPlayer = GameInstance->CreateLocalPlayer(0, PlayerError, false);
		Controller = World->SpawnActor<AWoTPlayerController>();
		Pawn = World->SpawnActor<AWoTCharacter>(FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
		if (!Test.TestNotNull(TEXT("local player"), LocalPlayer)
			|| !Test.TestNotNull(TEXT("controller"), Controller) || !Test.TestNotNull(TEXT("pawn"), Pawn))
		{
			return false;
		}
		Controller->SetPlayer(LocalPlayer);
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Frames(2);   // the layer installs its context, the subsystem applies it
		return Test.TestNotNull(TEXT("gamepad layer"), Controller->GetGamepadLayerForTest());
	}

	void Send(const FKey& Key, EInputEvent Event, float Value)
	{
		Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, Event, Value));
	}
	void Frames(int32 Count, float Step = 1.0f / 60.0f) { WoTTest::TickWorld(World, Count, Step); }
	/** A deflected stick posts its axis every frame, as the platform does, and
	 * posts zero when it recenters (otherwise the last value stays held). */
	void HoldAxis(const FKey& Key, float Value, int32 Count, float Step = 1.0f / 60.0f)
	{
		for (int32 Index = 0; Index < Count; ++Index) { Send(Key, IE_Axis, Value); Frames(1, Step); }
		Send(Key, IE_Axis, 0.0f);
	}
	void ResetPawn()
	{
		Pawn->SetActorLocation(FVector(0.0f, 0.0f, 500.0f));
		Pawn->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Controller->SetControlRotation(FRotator::ZeroRotator);
		Frames(1);
	}
	float Speed() const { return Pawn->GetVelocity().Size(); }
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadModeContextsSpec,
	"WoT.Input.Gamepad.ModeContexts", Gamepad_WorldFlags)

bool FWoTGamepadModeContextsSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	UWoTGamepadLayer* Layer = Fx.Controller->GetGamepadLayerForTest();
	TestEqual(TEXT("a live player gets the gameplay context"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::Gameplay));
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		Fx.Controller->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	TestTrue(TEXT("the subsystem holds the gameplay context"),
		Subsystem->HasMappingContext(Layer->GetContext(EWoTGamepadContext::Gameplay)));
	for (EWoTGamepadAction Paused : {EWoTGamepadAction::Menu, EWoTGamepadAction::Jump, EWoTGamepadAction::Fire})
	{
		TestTrue(FString::Printf(TEXT("%s triggers while paused"), WoTGamepadActionName(Paused)),
			Layer->GetAction(Paused)->bTriggerWhenPaused);
	}
	TestFalse(TEXT("Look does not trigger while paused"),
		Layer->GetAction(EWoTGamepadAction::Look)->bTriggerWhenPaused);
	Fx.Controller->EnableChatWidgetForTesting();
	Fx.Controller->Talk();   // opens the chat line
	Fx.Frames(2);
	TestTrue(TEXT("chat typing installs no controller context"),
		Layer->GetMode() == EWoTGamepadMode::None
		&& !Subsystem->HasMappingContext(Layer->GetContext(EWoTGamepadContext::Gameplay)));
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 20);
	TestEqual(TEXT("the stick does not walk the player while chatting"), Fx.Speed(), 0.0f, 1.0f);
	Fx.Controller->CancelChatInput();
	Fx.Frames(2);
	TestEqual(TEXT("closing chat restores gameplay"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::Gameplay));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadMoveMatchesKeyboardSpec,
	"WoT.Input.Gamepad.MoveMatchesKeyboard", Gamepad_WorldFlags)

bool FWoTGamepadMoveMatchesKeyboardSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	Fx.ResetPawn();
	Fx.Send(EKeys::W, IE_Pressed, 1.0f);
	Fx.Frames(30);
	const float KeyboardSpeed = Fx.Speed();
	Fx.Send(EKeys::W, IE_Released, 0.0f);
	TestTrue(TEXT("W moves the pawn"), KeyboardSpeed > 10.0f);

	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 30);
	TestEqual(TEXT("a full stick matches W"), Fx.Speed(), KeyboardSpeed, 1.0f);

	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 0.2f + 0.75f * 0.22f, 30);
	TestEqual(TEXT("22% past the dead zone already matches W"), Fx.Speed(), KeyboardSpeed, 1.0f);

	// Partial tilt (below the gain's saturation point) moves less strongly:
	// compare at 0.1 s, before either caps.
	Fx.ResetPawn();
	Fx.Send(EKeys::W, IE_Pressed, 1.0f);
	Fx.Frames(6);
	const float KeyboardEarly = Fx.Speed();
	Fx.Send(EKeys::W, IE_Released, 0.0f);
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 0.2f + 0.75f * 0.1f, 6);
	TestTrue(TEXT("a partial stick accelerates more slowly"),
		Fx.Speed() > 1.0f && Fx.Speed() < KeyboardEarly - 1.0f);

	Fx.ResetPawn();
	Fx.Send(EKeys::W, IE_Pressed, 1.0f);
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 30);
	Fx.Send(EKeys::W, IE_Released, 0.0f);
	TestEqual(TEXT("W plus the stick is not faster than W"), Fx.Speed(), KeyboardSpeed, 1.0f);

	// Release: the platform stops posting the axis; movement input stops at once.
	// GetLastMovementInputVector is the input the movement component consumed
	// on the last tick.
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 10);
	TestFalse(TEXT("a held stick adds input"), Fx.Pawn->GetLastMovementInputVector().IsNearlyZero());
	Fx.Send(EKeys::Gamepad_LeftY, IE_Axis, 0.0f);
	Fx.Frames(2);
	TestTrue(TEXT("a released stick adds none"), Fx.Pawn->GetLastMovementInputVector().IsNearlyZero());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadNoDoubleSwimUpSpec,
	"WoT.Input.Gamepad.NoDoubleSwimUp", Gamepad_WorldFlags)

bool FWoTGamepadNoDoubleSwimUpSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	// Swim-up held, as Jump (Space or A) sets it.
	Fx.Pawn->SetSwimUpHeld(true);
	Fx.Pawn->SwimVerticalInputCountForTest = 0;
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		// The movement component leaves swimming outside a water zone each tick;
		// put it back before every input pass.
		Fx.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		Fx.Send(EKeys::Gamepad_LeftY, IE_Axis, 1.0f);
		Fx.Frames(1);
	}
	Fx.Pawn->SetSwimUpHeld(false);
	TestEqual(TEXT("swim-up is added once per frame, by the keyboard axis pass only"),
		Fx.Pawn->SwimVerticalInputCountForTest, 10);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadLookRateSpec,
	"WoT.Input.Gamepad.LookRate", Gamepad_WorldFlags)

bool FWoTGamepadLookRateSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	const FWoTGamepadTuning& Tuning = WoTGamepadTuning();
	auto YawAfter = [&Fx](float Step, int32 Count)
	{
		Fx.ResetPawn();
		const float Start = Fx.Controller->GetControlRotation().Yaw;
		Fx.HoldAxis(EKeys::Gamepad_RightX, 1.0f, Count, Step);
		return FMath::FindDeltaAngleDegrees(Start, Fx.Controller->GetControlRotation().Yaw);
	};
	// The default camera FOV is 90, so the retail FOV factor is 0.99999.
	const float Expected = Tuning.YawRate * 0.5f;
	const float At60 = YawAfter(1.0f / 60.0f, 30);
	TestEqual(TEXT("full right for 0.5 s at 60 Hz turns YawRate/2"), At60, Expected,
		Tuning.YawRate / 60.0f + 0.01f * Expected);
	const float At30 = YawAfter(1.0f / 30.0f, 15);
	TestEqual(TEXT("the same half second at 30 Hz turns the same"), At30, At60,
		Tuning.YawRate / 30.0f + 0.01f * Expected);

	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_RightY, 1.0f, 6);
	TestTrue(TEXT("stick up looks up"), Fx.Controller->GetControlRotation().GetNormalized().Pitch > 1.0f);
	UWoTGamepadLayer* Layer = Fx.Controller->GetGamepadLayerForTest();
	Layer->SetStickSettingsForTest(0.20f, 1.0f, /*bInvertY*/ true);
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_RightY, 1.0f, 6);
	TestTrue(TEXT("invert Y makes stick up look down"),
		Fx.Controller->GetControlRotation().GetNormalized().Pitch < -1.0f);
	Layer->SetStickSettingsForTest(0.20f, 2.0f, false);
	const float Doubled = YawAfter(1.0f / 60.0f, 15);
	Layer->ClearStickSettingsForTest();
	const float Single = YawAfter(1.0f / 60.0f, 15);
	TestEqual(TEXT("sensitivity 2 turns twice as far"), Doubled, 2.0f * Single,
		Tuning.YawRate / 30.0f + 0.02f * Doubled);

	const float StickOnly = YawAfter(1.0f / 60.0f, 6);
	Fx.ResetPawn();
	const float Start = Fx.Controller->GetControlRotation().Yaw;
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		Fx.Send(EKeys::MouseX, IE_Axis, 10.0f);
		Fx.Frames(1);
	}
	const float MouseOnly = FMath::FindDeltaAngleDegrees(Start, Fx.Controller->GetControlRotation().Yaw);
	Fx.ResetPawn();
	const float BothStart = Fx.Controller->GetControlRotation().Yaw;
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		Fx.Send(EKeys::MouseX, IE_Axis, 10.0f);
		Fx.Send(EKeys::Gamepad_RightX, IE_Axis, 1.0f);
		Fx.Frames(1);
	}
	const float Both = FMath::FindDeltaAngleDegrees(BothStart, Fx.Controller->GetControlRotation().Yaw);
	TestTrue(TEXT("the mouse turns on its own"), FMath::Abs(MouseOnly) > 0.1f);
	TestEqual(TEXT("mouse and stick add"), Both, StickOnly + MouseOnly,
		0.05f * (StickOnly + MouseOnly) + 0.1f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadJoystickOffSwallowsSpec,
	"WoT.Input.Gamepad.JoystickOffSwallows", Gamepad_WorldFlags)

bool FWoTGamepadJoystickOffSwallowsSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	Fx.Controller->SetJoystickEnabledOverrideForTest(false);
	Fx.ResetPawn();
	const double Yaw = Fx.Controller->GetControlRotation().Yaw;
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 20);
	Fx.HoldAxis(EKeys::Gamepad_RightX, 1.0f, 20);
	TestEqual(TEXT("no movement with the joystick off"), Fx.Speed(), 0.0f, 1.0f);
	TestEqual(TEXT("no look with the joystick off"), Fx.Controller->GetControlRotation().Yaw, Yaw, 0.01);
	Fx.Controller->SetJoystickEnabledOverrideForTest(TOptional<bool>());
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 1.0f, 20);
	TestTrue(TEXT("the same stick moves once the joystick is back on"), Fx.Speed() > 10.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadFireEdgesSpec,
	"WoT.Input.Gamepad.FireEdges", Gamepad_WorldFlags)

bool FWoTGamepadFireEdgesSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	// Balefire: hold to charge, release to fire. Only a delivered release lands it.
	AWoTAngrealInvBalefire* Balefire = Fx.World->SpawnActor<AWoTAngrealInvBalefire>(
		FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("balefire"), Balefire)) { return false; }
	Balefire->GiveTo(Fx.Pawn);
	Fx.Pawn->InventoryComponent->SelectItem(Balefire);
	AWoTCharacter* Victim = Fx.World->SpawnActor<AWoTCharacter>(
		FVector(300.0f, 0.0f, 542.0f), FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	Fx.Frames(42);   // pickup + the cast gate
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Pressed, 1.0f);
	Fx.Frames(150);  // past the 2 s prime, still held
	TestEqual(TEXT("a held weave has not fired"), Victim->Health, 1000);
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Released, 0.0f);
	Fx.Frames(2);
	TestEqual(TEXT("releasing RT lands the weave"), Victim->Health, 600);

	// Focus loss / overlay: the engine flushes held keys; the release still arrives.
	Victim->Health = 1000;
	Fx.Frames(60);
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Pressed, 1.0f);
	Fx.Frames(150);
	Fx.Controller->FlushPressedKeys();
	Fx.Frames(2);
	TestEqual(TEXT("a key flush delivers the release once"), Victim->Health, 600);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadDeadRespawnSpec,
	"WoT.Input.Gamepad.DeadRespawn", Gamepad_WorldFlags)

bool FWoTGamepadDeadRespawnSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	for (const FKey& Key : {EKeys::Gamepad_RightTrigger, EKeys::Gamepad_FaceButton_Bottom})
	{
		Fx.Controller->ArmRespawnFire(nullptr, 0.0f);
		Fx.Frames(1);
		TestEqual(TEXT("armed: the controller is Dying"),
			static_cast<int32>(Fx.Controller->GetPlayerStateMachineState()),
			static_cast<int32>(EWoTPlayerStateMachineState::Dying));
		Fx.Send(Key, IE_Pressed, 1.0f);
		Fx.Frames(2);
		Fx.Send(Key, IE_Released, 0.0f);
		Fx.Frames(1);
		TestNotEqual(FString::Printf(TEXT("%s requests a respawn"), *Key.ToString()),
			static_cast<int32>(Fx.Controller->GetPlayerStateMachineState()),
			static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadDropTapHoldSpec,
	"WoT.Input.Gamepad.DropTapHold", Gamepad_WorldFlags)

bool FWoTGamepadDropTapHoldSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	const int32 DropsBefore = Fx.Pawn->DropRequestCountForTest;
	const int32 TaintedBefore = Fx.Pawn->DropTaintedRequestCountForTest;
	Fx.Send(EKeys::Gamepad_FaceButton_Left, IE_Pressed, 1.0f);
	Fx.Frames(6);    // 0.1 s
	Fx.Send(EKeys::Gamepad_FaceButton_Left, IE_Released, 0.0f);
	Fx.Frames(2);
	TestEqual(TEXT("a tap drops once"), Fx.Pawn->DropRequestCountForTest, DropsBefore + 1);
	TestEqual(TEXT("a tap does not drop tainted"), Fx.Pawn->DropTaintedRequestCountForTest, TaintedBefore);
	Fx.Send(EKeys::Gamepad_FaceButton_Left, IE_Pressed, 1.0f);
	Fx.Frames(36);   // 0.6 s
	Fx.Send(EKeys::Gamepad_FaceButton_Left, IE_Released, 0.0f);
	Fx.Frames(2);
	TestEqual(TEXT("a hold drops tainted once"), Fx.Pawn->DropTaintedRequestCountForTest, TaintedBefore + 1);
	TestEqual(TEXT("a hold does not also drop"), Fx.Pawn->DropRequestCountForTest, DropsBefore + 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadScoreboardHoldSpec,
	"WoT.Input.Gamepad.ScoreboardHold", Gamepad_WorldFlags)

bool FWoTGamepadScoreboardHoldSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	Fx.Controller->bShowScores = false;
	Fx.Send(EKeys::Gamepad_Special_Left, IE_Pressed, 1.0f);
	Fx.Frames(2);
	TestTrue(TEXT("View held shows scores"), Fx.Controller->bShowScores);
	Fx.Send(EKeys::Gamepad_Special_Left, IE_Released, 0.0f);
	Fx.Frames(2);
	TestFalse(TEXT("releasing View hides them"), Fx.Controller->bShowScores);
	Fx.Controller->bShowScores = true;   // e.g. the death auto-show
	Fx.Send(EKeys::Gamepad_Special_Left, IE_Pressed, 1.0f);
	Fx.Frames(2);
	Fx.Send(EKeys::Gamepad_Special_Left, IE_Released, 0.0f);
	Fx.Frames(2);
	TestFalse(TEXT("a press dismisses an already-shown scoreboard, as the key toggle does"),
		Fx.Controller->bShowScores);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadWalkToggleSpec,
	"WoT.Input.Gamepad.WalkToggle", Gamepad_WorldFlags)

bool FWoTGamepadWalkToggleSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	auto Walking = [&Fx]() { return Fx.Pawn->IsWalkingRequestedForTest(); };
	auto Click = [&Fx]()
	{
		Fx.Send(EKeys::Gamepad_LeftThumbstick, IE_Pressed, 1.0f);
		Fx.Frames(1);
		Fx.Send(EKeys::Gamepad_LeftThumbstick, IE_Released, 0.0f);
		Fx.Frames(1);
	};
	Click();
	TestTrue(TEXT("L3 toggles walking on"), Walking());
	Click();
	TestFalse(TEXT("L3 again toggles it off"), Walking());
	Click();
	AWoTCharacter* OldPawn = Fx.Pawn;
	Fx.Controller->UnPossess();
	TestFalse(TEXT("losing the pawn ends the toggle"), OldPawn->IsWalkingRequestedForTest());
	Fx.Pawn = Fx.World->SpawnActor<AWoTCharacter>(FVector(200.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
	Fx.Controller->Possess(Fx.Pawn);
	Fx.Frames(1);
	TestFalse(TEXT("a new pawn does not inherit the toggle"), Walking());
	Click();
	TestTrue(TEXT("the toggle works on the new pawn"), Walking());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadEditorContextSpec,
	"WoT.Input.Gamepad.EditorContext", Gamepad_WorldFlags)

bool FWoTGamepadEditorContextSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	UWoTGamepadLayer* Layer = Fx.Controller->GetGamepadLayerForTest();
	Fx.Controller->SetOwnerEditSessionTeamForTest(0);   // the owner mirror IsOwnerEditingCitadel reads
	Fx.Frames(2);
	TestEqual(TEXT("editing installs the editor context"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::CitadelEditor));
	// RT reaches the controller's Fire, whose editing path (UseAngreal →
	// HandleCitadelEditorClick) the citadel specs already cover; here the editor
	// context must route the right keys to the right actions.
	auto Maps = [Layer](const FKey& Key, EWoTGamepadAction Action)
	{
		for (const FEnhancedActionKeyMapping& Mapping :
			Layer->GetContext(EWoTGamepadContext::CitadelEditor)->GetMappings())
		{
			if (Mapping.Key == Key && Mapping.Action == Layer->GetAction(Action)) { return true; }
		}
		return false;
	};
	TestTrue(TEXT("RT places (Fire)"), Maps(EKeys::Gamepad_RightTrigger, EWoTGamepadAction::Fire));
	TestTrue(TEXT("B leaves the editor"), Maps(EKeys::Gamepad_FaceButton_Right, EWoTGamepadAction::EditCitadel));
	TestTrue(TEXT("D-pad up cycles zones"), Maps(EKeys::Gamepad_DPad_Up, EWoTGamepadAction::ViewUp));
	TestFalse(TEXT("A does nothing in the editor"), Maps(EKeys::Gamepad_FaceButton_Bottom, EWoTGamepadAction::Jump));
	Fx.Controller->SetOwnerEditSessionTeamForTest(255);
	Fx.Frames(2);
	TestEqual(TEXT("leaving restores gameplay"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::Gameplay));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadPausedFirePressSpec,
	"WoT.Input.Gamepad.PausedFirePress", Gamepad_WorldFlags)

bool FWoTGamepadPausedFirePressSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	// Only Fire's release runs while paused (the legacy press binding does not
	// execute when paused); an RT press on a paused game must not respawn.
	Fx.Controller->ArmRespawnFire(nullptr, 0.0f);
	Fx.Frames(1);
	AWorldSettings* Settings = Fx.World->GetWorldSettings();
	Settings->SetPauserPlayerState(Fx.Controller->PlayerState);
	TestTrue(TEXT("the world is paused"), Fx.World->IsPaused());
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Pressed, 1.0f);
	Fx.Frames(2);
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Released, 0.0f);
	Fx.Frames(1);
	TestEqual(TEXT("a paused RT press does not respawn"),
		static_cast<int32>(Fx.Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	Settings->SetPauserPlayerState(nullptr);
	Fx.Frames(1);
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Pressed, 1.0f);
	Fx.Frames(2);
	Fx.Send(EKeys::Gamepad_RightTrigger, IE_Released, 0.0f);
	Fx.Frames(1);
	TestNotEqual(TEXT("unpaused, the same press respawns"),
		static_cast<int32>(Fx.Controller->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadTurnAroundSpec,
	"WoT.Input.Gamepad.TurnAround", Gamepad_WorldFlags)

bool FWoTGamepadTurnAroundSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this)) { return false; }
	Fx.ResetPawn();
	const double Start = Fx.Controller->GetControlRotation().Yaw;
	auto Turned = [&Fx, Start]()
	{
		return FMath::Abs(FMath::FindDeltaAngleDegrees(Start, Fx.Controller->GetControlRotation().Yaw));
	};
	Fx.Send(EKeys::Gamepad_LeftTrigger, IE_Pressed, 1.0f);
	Fx.Frames(3);
	TestTrue(FString::Printf(TEXT("LT turns gradually, not in one frame (%.1f deg after 0.05 s)"), Turned()),
		Turned() > 10.0 && Turned() < 170.0);
	Fx.Send(EKeys::Gamepad_LeftTrigger, IE_Released, 0.0f);
	Fx.Frames(15);
	TestEqual(TEXT("the turn ends facing the other way"), Turned(), 180.0, 0.5);
	Fx.Frames(10);
	TestEqual(TEXT("and stops there"), Turned(), 180.0, 0.5);
	// A second press turns back.
	Fx.Send(EKeys::Gamepad_LeftTrigger, IE_Pressed, 1.0f);
	Fx.Frames(1);
	Fx.Send(EKeys::Gamepad_LeftTrigger, IE_Released, 0.0f);
	Fx.Frames(20);
	TestEqual(TEXT("a second turn faces the start again"), Turned(), 0.0, 0.5);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGamepadTeamSelectSpec,
	"WoT.Input.Gamepad.TeamSelect", Gamepad_WorldFlags)

bool FWoTGamepadTeamSelectSpec::RunTest(const FString&)
{
	FGamepad_Fixture Fx;
	if (!Fx.Initialize(*this, AWoTBattleGameMode::StaticClass())) { return false; }
	UWorld* World = Fx.World;
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* BattleInfo = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	FActorSpawnParameters Always;
	Always.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerStart* Team0Start = World->SpawnActor<AWoTPlayerStart>(
		FVector(-2000.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Always);
	AWoTPlayerStart* Team1Start = World->SpawnActor<AWoTPlayerStart>(
		FVector(2000.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Always);
	if (!TestTrue(TEXT("battle fixture"), BattleMode && BattleInfo && Team0Start && Team1Start))
	{
		return false;
	}
	BattleInfo->NumTeams = 2;
	BattleInfo->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BattleInfo->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BattleInfo->FinishSpawning(FTransform::Identity);
	BattleInfo->bTeleportersDisabled = false;
	BattleMode->Battle = BattleInfo;
	Team0Start->TeamNumber = 0;
	Team1Start->TeamNumber = 1;
	Fx.Controller->ClientSetHUD_Implementation(AWoTHUD::StaticClass());
	AWoTPlayerState* State = Fx.Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("player state"), State)
		|| !TestNotNull(TEXT("HUD"), Cast<AWoTHUD>(Fx.Controller->GetHUD())))
	{
		return false;
	}
	State->Team = 255;
	Fx.Frames(2);

	// Without a team, the D-pad chooses one on top of the gameplay context.
	UWoTGamepadLayer* Layer = Fx.Controller->GetGamepadLayerForTest();
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		Fx.Controller->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	TestEqual(TEXT("a player without a team is choosing one"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::TeamSelect));
	TestTrue(TEXT("both the gameplay and team-select contexts are installed"),
		Subsystem->HasMappingContext(Layer->GetContext(EWoTGamepadContext::Gameplay))
		&& Subsystem->HasMappingContext(Layer->GetContext(EWoTGamepadContext::TeamSelect)));
	// D-pad Down is team 4: no such team here, and the taunt it normally plays stays quiet.
	Fx.Send(EKeys::Gamepad_DPad_Down, IE_Pressed, 1.0f);
	Fx.Frames(1);
	UEnhancedPlayerInput* PlayerInput = Subsystem->GetPlayerInput();
	TestTrue(TEXT("D-pad Down drives team 4"),
		PlayerInput->GetActionValue(Layer->GetAction(EWoTGamepadAction::JoinTeam4)).Get<bool>());
	TestFalse(TEXT("team selection takes D-pad Down from the taunt"),
		PlayerInput->GetActionValue(Layer->GetAction(EWoTGamepadAction::RandomTaunt)).Get<bool>());
	Fx.Send(EKeys::Gamepad_DPad_Down, IE_Released, 0.0f);
	Fx.Frames(1);
	TestEqual(TEXT("an absent team is not joined"), static_cast<int32>(State->Team), 255);

	Fx.Send(EKeys::Gamepad_DPad_Right, IE_Pressed, 1.0f);
	Fx.Frames(1);
	Fx.Send(EKeys::Gamepad_DPad_Right, IE_Released, 0.0f);
	Fx.Frames(2);
	TestEqual(TEXT("D-pad Right joins team 2"), static_cast<int32>(State->Team), 1);
	APawn* Restarted = Fx.Controller->GetPawn();
	TestTrue(TEXT("the player restarts as team 2's pawn"),
		Restarted && Restarted->IsA(AWoTTestTeamBravo::StaticClass()));
	TestEqual(TEXT("with a team, the D-pad is back on hands and taunts"),
		static_cast<int32>(Layer->GetMode()), static_cast<int32>(EWoTGamepadMode::Gameplay));
	TestFalse(TEXT("the team-select context is gone"),
		Subsystem->HasMappingContext(Layer->GetContext(EWoTGamepadContext::TeamSelect)));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS && WITH_EDITOR
