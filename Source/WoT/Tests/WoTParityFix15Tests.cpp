#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#if WITH_EDITOR

#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "WoTTestBattleClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Slate/SceneViewport.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr EAutomationTestFlags Fix15_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent Fix15_KeyEvent(const FKey& Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
}

FPointerEvent Fix15_MousePointerEvent(const FVector2D& Position,
	const FVector2D& LastPosition, const FKey& EffectingButton,
	TSet<FKey> PressedButtons = {}, const float WheelDelta = 0.0f)
{
	return FPointerEvent(0, FSlateApplicationBase::CursorPointerIndex,
		Position, LastPosition, MoveTemp(PressedButtons), EffectingButton,
		WheelDelta, FModifierKeysState());
}

FPointerEvent Fix15_TouchPointerEvent(const FVector2D& Position,
	const FVector2D& LastPosition, const bool bTouchDown)
{
	return FPointerEvent(/*UserIndex*/ 0, /*PointerIndex*/ 1,
		Position, LastPosition, /*Force*/ 1.0f, bTouchDown);
}

struct Fix15_ChatFixture
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
	FVector2D Inside = FVector2D::ZeroVector;

	~Fix15_ChatFixture()
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
		FWidgetPath ViewportPath;
		if (!Test.TestTrue(TEXT("real scene viewport owns keyboard focus"),
				Slate.GetKeyboardFocusedWidget() == ViewportWidget)
			|| !Test.TestTrue(TEXT("Slate builds a path to the scene viewport"),
				Slate.GeneratePathToWidgetUnchecked(
					ViewportWidget.ToSharedRef(), ViewportPath)))
		{
			return false;
		}
		const FGeometry& ViewportGeometry = ViewportPath.Widgets.Last().Geometry;
		Inside = ViewportGeometry.LocalToAbsolute(
			ViewportGeometry.GetLocalSize() * 0.5f);

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
		CastProbe->CurCharges = 10;
		Pawn->InventoryComponent->SelectItem(CastProbe);
		WoTTest::TickWorld(World, 0.1f);
		return true;
	}

	bool OpenChat(FAutomationTestBase& Test) const
	{
		FSlateApplication::Get().ProcessKeyDownEvent(Fix15_KeyEvent(EKeys::T));
		WoTTest::TickWorld(World, 1.0f / 60.0f);
		return Test.TestTrue(TEXT("Talk opens the actual chat widget"),
			Controller->IsChatTyping());
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

	bool PressGameplayMouseButton(const FKey& Key,
		TSet<FKey> PressedButtons) const
	{
		FWidgetPath ViewportPath;
		if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(
			ViewportWidget.ToSharedRef(), ViewportPath))
		{
			return false;
		}
		TArray<FWidgetAndPointer> PointerWidgets;
		PointerWidgets.Reserve(ViewportPath.Widgets.Num());
		for (int32 Index = 0; Index < ViewportPath.Widgets.Num(); ++Index)
		{
			PointerWidgets.Emplace(ViewportPath.Widgets[Index],
				FVirtualPointerPosition(Inside, Inside));
		}
		return FSlateApplication::Get().RoutePointerDownEvent(
			FWidgetPath(MakeArrayView(PointerWidgets)),
			Fix15_MousePointerEvent(Inside, Inside, Key,
				MoveTemp(PressedButtons))).IsEventHandled();
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix15ChatDoubleClickRoutingSpec,
	"WoT.Parity.Fix15.ChatDoubleClickRouting", Fix15_Flags)

bool FFix15ChatDoubleClickRoutingSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	Fix15_ChatFixture Fixture;
	if (!Fixture.Initialize(*this) || !Fixture.OpenChat(*this))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	for (const TCHAR Character : FString(TEXT("alpha beta")))
	{
		Slate.ProcessKeyCharEvent(FCharacterEvent(
			Character, FModifierKeysState(), 0, false));
	}
	const FString TextBefore = Fixture.Controller->GetChatTextForTesting();
	TestEqual(TEXT("keyboard text reaches the actual chat field"),
		TextBefore, FString(TEXT("Say alpha beta")));

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
	const FGeometry& TextGeometry = TextPath.Widgets.Last().Geometry;
	const FVector2D WordPoint = TextGeometry.LocalToAbsolute(
		FVector2D(8.0f, TextGeometry.GetLocalSize().Y * 0.5f));
	TestTrue(TEXT("double-click point is over the editable chat text"),
		TextGeometry.IsUnderLocation(WordPoint));

	TestTrue(TEXT("chat consumes the double-click press"),
		Slate.ProcessMouseButtonDoubleClickEvent(nullptr,
			Fix15_MousePointerEvent(WordPoint, WordPoint,
				EKeys::LeftMouseButton, {EKeys::LeftMouseButton})));
	TestFalse(TEXT("double-click does not select a chat word"),
		Editable->AnyTextSelected());
	TestFalse(TEXT("double-click does not capture the editable text"),
		Editable->HasMouseCapture());
	TestFalse(TEXT("double-click leaves no widget holding capture"),
		Slate.HasAnyMouseCaptor());
	TestEqual(TEXT("double-click leaves chat text unchanged"),
		Fixture.Controller->GetChatTextForTesting(), TextBefore);
	TestEqual(TEXT("double-click press never fires the selected artifact"),
		Fixture.CastProbe->CastCount, 0);
	TestFalse(TEXT("double-click press never reaches gameplay held state"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));

	Slate.ProcessKeyCharEvent(FCharacterEvent(
		TEXT('z'), FModifierKeysState(), 0, false));
	TestEqual(TEXT("typing after double-click appends instead of replacing a word"),
		Fixture.Controller->GetChatTextForTesting(),
		FString(TEXT("Say alpha betaz")));
	Slate.ProcessMouseButtonUpEvent(Fix15_MousePointerEvent(
		WordPoint, WordPoint, EKeys::LeftMouseButton));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix15ChatMouseWheelRoutingSpec,
	"WoT.Parity.Fix15.ChatMouseWheelRouting", Fix15_Flags)

bool FFix15ChatMouseWheelRoutingSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	Fix15_ChatFixture Fixture;
	if (!Fixture.Initialize(*this) || !Fixture.OpenChat(*this))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const FVector2D Outside(5000.0f, 5000.0f);
	const FPointerEvent EmptyWheel = Fix15_MousePointerEvent(
		Outside, Outside, EKeys::Invalid);
	const FPointerEvent MagnifyGesture(Outside, Outside, TSet<FKey>(),
		FModifierKeysState(), EGestureEvent::Magnify, EGesturePhase::Update,
		FVector2D(1.0f, 0.0f), false);
	TestFalse(TEXT("non-wheel gestures remain available to Slate"),
		Slate.ProcessMouseWheelOrGestureEvent(EmptyWheel, &MagnifyGesture));

	TestTrue(TEXT("chat handles a real wheel notch"),
		Slate.ProcessMouseWheelOrGestureEvent(
			Fix15_MousePointerEvent(Fixture.Inside, Fixture.Inside,
				EKeys::Invalid, {}, 1.0f), nullptr));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("wheel press remains eaten while typing"),
		Fixture.Controller->PlayerInput->WasJustPressed(EKeys::MouseScrollUp));
	TestTrue(TEXT("wheel release reaches PlayerInput while typing"),
		Fixture.Controller->PlayerInput->WasJustReleased(EKeys::MouseScrollUp));
	TestEqual(TEXT("wheel axis value reaches PlayerInput while typing"),
		Fixture.Controller->PlayerInput->GetKeyValue(EKeys::MouseWheelAxis),
		1.0f, UE_KINDA_SMALL_NUMBER);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix15ChatTouchIsolationSpec,
	"WoT.Parity.Fix15.ChatTouchIsolation", Fix15_Flags)

bool FFix15ChatTouchIsolationSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	Fix15_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	TestTrue(TEXT("real mouse path starts gameplay Fire"),
		Fixture.PressGameplayMouseButton(
			EKeys::LeftMouseButton, {EKeys::LeftMouseButton}));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	if (!TestTrue(TEXT("mouse Fire is held before chat"),
			Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton))
		|| !TestEqual(TEXT("mouse press starts the selected artifact"),
			Fixture.CastProbe->CastCount, 1)
		|| !Fixture.OpenChat(*this))
	{
		return false;
	}
	TestTrue(TEXT("chat retains capture for held mouse Fire"),
		Fixture.ViewportWidget->HasMouseCapture());
	const int32 UnCastBeforeTouch = Fixture.CastProbe->UnCastCount;
	const FRotator RotationBeforeTouch = Fixture.Controller->GetControlRotation();
	const FVector2D TouchEnd = Fixture.Inside + FVector2D(80.0f, 40.0f);

	Slate.ProcessTouchStartedEvent(nullptr,
		Fix15_TouchPointerEvent(Fixture.Inside, Fixture.Inside, true));
	Slate.ProcessTouchMovedEvent(
		Fix15_TouchPointerEvent(TouchEnd, Fixture.Inside, true));
	Slate.ProcessTouchEndedEvent(
		Fix15_TouchPointerEvent(TouchEnd, TouchEnd, false));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);

	TestTrue(TEXT("touch tap does not release held mouse Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestEqual(TEXT("touch tap does not end the held cast"),
		Fixture.CastProbe->UnCastCount, UnCastBeforeTouch);
	TestTrue(TEXT("touch drag does not turn the view"),
		Fixture.Controller->GetControlRotation().Equals(
			RotationBeforeTouch, UE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("touch tap preserves retained mouse capture"),
		Fixture.ViewportWidget->HasMouseCapture());

	Slate.ProcessMouseButtonUpEvent(Fix15_MousePointerEvent(
		TouchEnd, TouchEnd, EKeys::LeftMouseButton));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("the later physical mouse release ends held Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	return !HasAnyErrors();
}


// Motion input is axis input,
// which retail Console Typing passes through to the player while typing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix16ChatMotionRoutingSpec,
	"WoT.Parity.Fix16.ChatMotionRouting", Fix15_Flags)

bool FFix16ChatMotionRoutingSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	Fix15_ChatFixture Fixture;
	if (!Fixture.Initialize(*this) || !Fixture.OpenChat(*this))
	{
		return false;
	}
	const FVector Tilt(0.25, -0.5, 0.75);
	FSlateApplication::Get().ProcessMotionDetectedEvent(FMotionEvent(
		/*UserIndex*/ 0, Tilt, FVector(1.0, 0.0, 0.0), FVector(0.0, 0.0, -1.0),
		FVector(0.0, 2.0, 0.0)));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("motion tilt reaches PlayerInput while typing"),
		Fixture.Controller->PlayerInput->GetRawVectorKeyValue(EKeys::Tilt)
			.Equals(Tilt, UE_KINDA_SMALL_NUMBER));
	return !HasAnyErrors();
}

#endif // WITH_EDITOR

#endif // WITH_AUTOMATION_TESTS
