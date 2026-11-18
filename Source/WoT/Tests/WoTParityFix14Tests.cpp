#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "WoTTestBattleClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Slate/SceneViewport.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr EAutomationTestFlags Fix14_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent Fix14_KeyEvent(const FKey& Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
}

FPointerEvent Fix14_PointerEvent(const FVector2D& Position,
	const FVector2D& LastPosition, const FKey& EffectingButton,
	TSet<FKey> PressedButtons = {})
{
	return FPointerEvent(0, FSlateApplicationBase::CursorPointerIndex,
		Position, LastPosition, MoveTemp(PressedButtons), EffectingButton,
		0.0f, FModifierKeysState());
}

bool Fix14_ReleaseExecutesWhenPaused(
	const UInputComponent& Input, const FName ActionName)
{
	for (int32 Index = 0; Index < Input.GetNumActionBindings(); ++Index)
	{
		const FInputActionBinding& Binding = Input.GetActionBinding(Index);
		if (Binding.GetActionName() == ActionName
			&& Binding.KeyEvent == IE_Released)
		{
			return Binding.bExecuteWhenPaused;
		}
	}
	return false;
}

#if WITH_EDITOR
struct Fix14_ChatFixture
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;
	UGameInstance* GameInstance = nullptr;
	UGameViewportClient* ViewportClient = nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<FSceneViewport> SceneViewport;
	TSharedPtr<SWindow> Window;
	UGameViewportClient* PreviousEngineViewport = nullptr;
	AWoTGameMode* Game = nullptr;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;
	AWoTTestChatAngreal* CastProbe = nullptr;

	~Fix14_ChatFixture()
	{
		if (Game)
		{
			Game->ClearPause();
		}
		if (Controller && Controller->IsChatTyping())
		{
			Controller->CancelChatInput();
		}
		if (GEngine)
		{
			GEngine->GameViewport = PreviousEngineViewport;
		}
		if (Window.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			FSlateApplication::Get().Tick();
		}
		if (ViewportClient && SceneViewport)
		{
			ViewportClient->RemoveAssociation(*SceneViewport);
		}
		SceneViewport.Reset();
		ViewportWidget.Reset();
		if (Context)
		{
			Context->GameViewport = nullptr;
		}
		WoTTest::DestroySpecWorld(World);
	}

	bool Initialize(FAutomationTestBase& Test)
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = AWoTGameMode::StaticClass();
		Options.BeforeInitializeActorsForPlay = [](UWorld* InWorld)
		{
			InWorld->WorldType = EWorldType::PIE;
			InWorld->SetPlayInEditorInitialNetMode(NM_ListenServer);
		};
		World = WoTTest::CreateSpecWorld(Options);
		Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr;
		GameInstance = World ? World->GetGameInstance() : nullptr;
		ViewportClient = Context && GameInstance
			? NewObject<UGameViewportClient>(
				GEngine, GEngine->GameViewportClientClass)
			: nullptr;
		PreviousEngineViewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
		if (!Test.TestNotNull(TEXT("listen world"), World)
			|| !Test.TestNotNull(TEXT("world context"), Context)
			|| !Test.TestNotNull(TEXT("game instance"), GameInstance)
			|| !Test.TestNotNull(TEXT("game viewport client"), ViewportClient)
			|| !Test.TestTrue(TEXT("Slate application is initialized"),
				FSlateApplication::IsInitialized()))
		{
			return false;
		}

		ViewportClient->Init(*Context, GameInstance);
		Context->GameViewport = ViewportClient;
		GEngine->GameViewport = ViewportClient;
		ViewportWidget = SNew(SViewport);
		SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
		SceneViewport->SetInitialSize(FIntPoint(640, 480));
		ViewportClient->AddAssociation(*SceneViewport);
		ViewportClient->SetMouseCaptureMode(
			EMouseCaptureMode::CaptureDuringMouseDown);
		Window = SNew(SWindow)
			.ScreenPosition(FVector2D(100.0f, 100.0f))
			.ClientSize(FVector2D(640.0f, 480.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[ViewportWidget.ToSharedRef()];
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.AddWindow(Window.ToSharedRef(), false);
		Slate.Tick();
		Window->SlatePrepass(1.0f);
		Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
		if (!Test.TestTrue(TEXT("real scene viewport owns keyboard focus"),
			Slate.GetKeyboardFocusedWidget() == ViewportWidget))
		{
			return false;
		}

		FString PlayerError;
		ULocalPlayer* LocalPlayer = GameInstance->CreateLocalPlayer(
			0, PlayerError, /*bSpawnPlayerController*/ false);
		Game = World->GetAuthGameMode<AWoTGameMode>();
		Controller = World->SpawnActor<AWoTPlayerController>();
		Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
		if (!Test.TestNotNull(TEXT("local player"), LocalPlayer)
			|| !Test.TestNotNull(TEXT("listen rules"), Game)
			|| !Test.TestNotNull(TEXT("local controller"), Controller)
			|| !Test.TestNotNull(TEXT("player pawn"), Pawn))
		{
			return false;
		}
		Controller->SetPlayer(LocalPlayer);
		Controller->Possess(Pawn);
		Controller->EnableChatWidgetForTesting();
		Controller->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Pawn->JumpMaxHoldTime = 10.0f;

		auto EnsureActionMapping = [this](const FName ActionName,
			const FKey& Key)
		{
			const bool bExists = Controller->PlayerInput->ActionMappings
				.ContainsByPredicate([&ActionName, &Key](
					const FInputActionKeyMapping& Mapping)
				{
					return Mapping.ActionName == ActionName && Mapping.Key == Key;
				});
			if (!bExists)
			{
				Controller->PlayerInput->AddActionMapping(
					FInputActionKeyMapping(ActionName, Key));
			}
		};
		EnsureActionMapping(TEXT("Fire"), EKeys::LeftMouseButton);
		EnsureActionMapping(TEXT("Walking"), EKeys::LeftShift);
		EnsureActionMapping(TEXT("Strafe"), EKeys::Apostrophe);
		EnsureActionMapping(TEXT("Jump"), EKeys::SpaceBar);
		EnsureActionMapping(TEXT("Talk"), EKeys::T);
		const bool bHasMouseX = Controller->PlayerInput->AxisMappings
			.ContainsByPredicate([](const FInputAxisKeyMapping& Mapping)
			{
				return Mapping.AxisName == TEXT("MouseX")
					&& Mapping.Key == EKeys::MouseX;
			});
		if (!bHasMouseX)
		{
			Controller->PlayerInput->AddAxisMapping(
				FInputAxisKeyMapping(TEXT("MouseX"), EKeys::MouseX, 1.0f));
		}

		CastProbe = World->SpawnActor<AWoTTestChatAngreal>();
		if (!Test.TestNotNull(TEXT("cast probe"), CastProbe))
		{
			return false;
		}
		CastProbe->GiveTo(Pawn);
		Pawn->InventoryComponent->SelectItem(CastProbe);
		WoTTest::TickWorld(World, 0.1f);
		return true;
	}

	TSharedPtr<SEditableText> FocusedEditableText(
		FAutomationTestBase& Test) const
	{
		TSharedPtr<SWidget> Focused =
			FSlateApplication::Get().GetKeyboardFocusedWidget();
		if (!Test.TestTrue(TEXT("chat owns keyboard focus"), Focused.IsValid()))
		{
			return nullptr;
		}
		return StaticCastSharedPtr<SEditableText>(Focused);
	}
};
#endif
} // namespace

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix14ChatMousePressRoutingSpec,
	"WoT.Parity.Fix14.ChatMousePressRouting", Fix14_Flags)

bool FFix14ChatMousePressRoutingSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	Fix14_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	if (!TestTrue(TEXT("Talk opens the actual chat widget"),
		Fixture.Controller->IsChatTyping()))
	{
		return false;
	}
	Slate.ProcessKeyCharEvent(FCharacterEvent(
		TEXT('x'), FModifierKeysState(), 0, false));
	TestEqual(TEXT("keyboard typing still edits chat"),
		Fixture.Controller->GetChatTextForTesting(), FString(TEXT("Say x")));

	Fixture.Window->SlatePrepass(1.0f);
	FSlateWindowElementList DrawElements(Fixture.Window);
	Fixture.Window->PaintWindow(FPlatformTime::Seconds(), 0.0f,
		DrawElements, FWidgetStyle(), true);
	TSharedPtr<SEditableText> Editable = Fixture.FocusedEditableText(*this);
	FWidgetPath TextPath;
	if (!Editable.IsValid()
		|| !TestTrue(TEXT("Slate builds a path to the editable chat text"),
			Slate.GeneratePathToWidgetUnchecked(
				Editable.ToSharedRef(), TextPath)))
	{
		return false;
	}
	const FGeometry TextGeometry = TextPath.Widgets.Last().Geometry;
	const FVector2D TextSize = TextGeometry.GetLocalSize();
	const FVector2D Start = TextGeometry.LocalToAbsolute(
		FVector2D(4.0f, TextSize.Y * 0.5f));
	const FVector2D End = TextGeometry.LocalToAbsolute(
		FVector2D(FMath::Max(8.0f, TextSize.X - 4.0f), TextSize.Y * 0.5f));
	TestTrue(TEXT("mouse-down point is over the editable chat text"),
		TextGeometry.IsUnderLocation(Start));

	const float YawBefore = Fixture.Controller->GetControlRotation().Yaw;
	TestTrue(TEXT("chat consumes a mouse-button press"),
		Slate.ProcessMouseButtonDownEvent(nullptr,
			Fix14_PointerEvent(Start, Start, EKeys::LeftMouseButton,
				{EKeys::LeftMouseButton})));
	TestFalse(TEXT("chat mouse-down does not start text capture"),
		Editable->HasMouseCapture());
	TestFalse(TEXT("chat mouse-down does not start a selection"),
		Editable->AnyTextSelected());
	Slate.ProcessMouseMoveEvent(Fix14_PointerEvent(
		End, Start, EKeys::Invalid, {EKeys::LeftMouseButton}));
	Slate.ProcessMouseButtonUpEvent(Fix14_PointerEvent(
		End, Start, EKeys::LeftMouseButton));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);

	TestFalse(TEXT("chat mouse gesture never selects text"),
		Editable->AnyTextSelected());
	TestFalse(TEXT("mouse-up leaves no widget holding capture"),
		Slate.HasAnyMouseCaptor());
	TestEqual(TEXT("chat mouse press never fires the selected artifact"),
		Fixture.CastProbe->CastCount, 0);
	TestFalse(TEXT("chat mouse press never enters gameplay held state"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestTrue(TEXT("mouse look still turns the player while chat is focused"),
		!FMath::IsNearlyEqual(
			Fixture.Controller->GetControlRotation().Yaw, YawBefore));
	TestEqual(TEXT("mouse routing does not alter typed chat text"),
		Fixture.Controller->GetChatTextForTesting(), FString(TEXT("Say x")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix14PausedHeldStateReleasesSpec,
	"WoT.Parity.Fix14.PausedHeldStateReleases", Fix14_Flags)

bool FFix14PausedHeldStateReleasesSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	Fix14_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	UWoTCharacterMovement* Movement = Cast<UWoTCharacterMovement>(
		Fixture.Pawn->GetCharacterMovement());
	if (!TestNotNull(TEXT("retail character movement"), Movement))
	{
		return false;
	}

	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::LeftShift));
	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::Apostrophe));
	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::SpaceBar));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Shift starts walking through the real viewport"),
		Movement->IsWalkingRequested());
	TestTrue(TEXT("Jump starts through the real viewport"),
		Fixture.Pawn->bPressedJump);
	TestTrue(TEXT("real listen pause succeeds"),
		Fixture.Game->TrySetPlayerPause(Fixture.Controller, NM_ListenServer));
	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the actual chat widget while paused"),
		Fixture.Controller->IsChatTyping());

	Slate.ProcessKeyUpEvent(Fix14_KeyEvent(EKeys::LeftShift));
	Slate.ProcessKeyUpEvent(Fix14_KeyEvent(EKeys::Apostrophe));
	Slate.ProcessKeyUpEvent(Fix14_KeyEvent(EKeys::SpaceBar));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("paused chat Shift release ends walking immediately"),
		Movement->IsWalkingRequested());
	TestFalse(TEXT("paused chat Jump release calls StopJumping immediately"),
		Fixture.Pawn->bPressedJump);

	TestTrue(TEXT("Walking release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Pawn->InputComponent, TEXT("Walking")));
	TestTrue(TEXT("Look release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Pawn->InputComponent, TEXT("Look")));
	TestTrue(TEXT("FreeLook release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Pawn->InputComponent, TEXT("FreeLook")));
	TestTrue(TEXT("CenterView release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Pawn->InputComponent, TEXT("CenterView")));
	TestTrue(TEXT("Strafe release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Pawn->InputComponent, TEXT("Strafe")));
	TestTrue(TEXT("StopJumping release is pause-enabled"),
		Fix14_ReleaseExecutesWhenPaused(
			*Fixture.Controller->InputComponent, TEXT("Jump")));

	Fixture.Game->ClearPause();
	const float YawBefore = Fixture.Controller->GetControlRotation().Yaw;
	Slate.ProcessMouseMoveEvent(Fix14_PointerEvent(
		FVector2D(360.0f, 240.0f), FVector2D(320.0f, 240.0f),
		EKeys::Invalid));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("paused Strafe release restores mouse look after unpause"),
		!FMath::IsNearlyEqual(
			Fixture.Controller->GetControlRotation().Yaw, YawBefore));

	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::Escape));
	TestFalse(TEXT("real Escape path closes chat"),
		Fixture.Controller->IsChatTyping());
	Slate.ProcessKeyDownEvent(Fix14_KeyEvent(EKeys::LeftShift));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("a later Shift press starts a fresh walking cycle"),
		Movement->IsWalkingRequested());
	Slate.ProcessKeyUpEvent(Fix14_KeyEvent(EKeys::LeftShift));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("the later Shift release completes the fresh cycle"),
		Movement->IsWalkingRequested());
	return !HasAnyErrors();
}
#endif

#endif
