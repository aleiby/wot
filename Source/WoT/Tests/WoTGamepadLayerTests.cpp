// The controller layer end to end: a real local player, Enhanced Input, the
// production contexts and handlers. Gamepad keys enter through the
// controller's InputKey exactly as the platform delivers them.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "WoTTestWorld.h"
#include "../Gamepad/WoTGamepadLayer.h"
#include "../Gamepad/WoTGamepadLayout.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "InputAction.h"
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

	bool Initialize(FAutomationTestBase& Test)
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = AWoTGameMode::StaticClass();
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

	// Partial tilt: slower acceleration (compare at 0.1 s, before either caps),
	// the same top speed.
	Fx.ResetPawn();
	Fx.Send(EKeys::W, IE_Pressed, 1.0f);
	Fx.Frames(6);
	const float KeyboardEarly = Fx.Speed();
	Fx.Send(EKeys::W, IE_Released, 0.0f);
	Fx.ResetPawn();
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 0.2f + 0.75f * 0.1f, 6);
	TestTrue(TEXT("a partial stick accelerates more slowly"),
		Fx.Speed() > 1.0f && Fx.Speed() < KeyboardEarly - 1.0f);
	Fx.HoldAxis(EKeys::Gamepad_LeftY, 0.2f + 0.75f * 0.1f, 114);
	TestEqual(TEXT("a partial stick reaches the same top speed"), Fx.Speed(), KeyboardSpeed, 1.0f);

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

#endif // WITH_AUTOMATION_TESTS && WITH_EDITOR
