#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "WoTVRSpecHMD.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "Tests/WoTUIScreensTestFixture.h"
#include "UI/SWoTOnScreenKeyboard.h"
#include "UI/WoTControllerUIInput.h"
#include "UI/WoTTextEntry.h"
#include "WoTGameViewportClient.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRPresentation.h"
#include "WoTVRSyntheticSession.h"

#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "IStereoLayers.h"
#include "Input/Events.h"
#include "Layout/WidgetPath.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SViewport.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/SWindow.h"

// The game's screens in the headset (design section 10, H6): while VR runs the
// plugin hosts every screen on an offscreen surface shown world-locked in front
// of the view (passive cards beside the HUD panel), points at it with the
// head's gaze, confirms with A, Enter or the left mouse button, and navigates
// and backs out with the gamepad through the hooks' routes as the player's one
// Slate user. Switching to flat, travel and session loss give every screen back
// to the desktop window with its focus.

namespace WoTVRUITest
{
/** Pointer mode, no text entry, no Steam keyboard, a recording cursor. */
struct FGuard
{
	TSharedRef<FWoTRecordingCursorPolicy> Policy = MakeShared<FWoTRecordingCursorPolicy>();
	FGuard()
	{
		FWoTControllerUIInput::ResetForTesting();
		FWoTControllerUIInput::SetCursorPolicyForTesting(Policy);
		FWoTTextEntry::ResetForTesting();
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
	}
	~FGuard()
	{
		FWoTTextEntry::ResetForTesting();
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(TOptional<bool>());
		FWoTControllerUIInput::ResetForTesting();
	}
};

/** The real game viewport in a real window, as GEngine->GameViewport. */
struct FScreens
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	TSharedPtr<SWindow> Window;
	UGameViewportClient* PreviousEngineViewport = nullptr;

	bool Init(FAutomationTestBase& Test)
	{
		if (!F.Init(Test))
		{
			return false;
		}
		Window = SNew(SWindow)
			.ClientSize(FVector2D(640.0f, 480.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[F.Widget.ToSharedRef()];
		FSlateApplication::Get().AddWindow(Window.ToSharedRef(), false);
		PreviousEngineViewport = GEngine->GameViewport;
		GEngine->GameViewport = F.Viewport;
		FSlateApplication::Get().Tick();
		return Test.TestNotNull(TEXT("the game's own viewport client"), Cast<UWoTGameViewportClient>(F.Viewport));
	}

	~FScreens()
	{
		if (F.Player)
		{
			if (UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player))
			{
				Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec teardown"));
			}
		}
		if (F.PC)
		{
			WoTUIScreensTest::CloseChat(*F.PC);
		}
		GEngine->GameViewport = PreviousEngineViewport;
		if (Window.IsValid())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			FSlateApplication::Get().Tick();
		}
	}

	UWoTGameViewportClient& Viewport() const
	{
		return *CastChecked<UWoTGameViewportClient>(F.Viewport);
	}

	/** On the viewport's own overlay, at this z-order. */
	bool OnViewport(const TSharedRef<SWidget>& Screen, TOptional<int32> ZOrder = TOptional<int32>()) const
	{
		return Viewport().GetOverlayContent().ContainsByPredicate(
			[&Screen, ZOrder](const TPair<TSharedRef<SWidget>, int32>& Entry)
			{
				return Entry.Key == Screen && (!ZOrder.IsSet() || Entry.Value == *ZOrder);
			});
	}

	int32 User() const
	{
		return FWoTLocalPlayerHooks::UISlateUser(*F.Player);
	}
};

bool FocusInside(int32 User, const TSharedPtr<SWidget>& Widget)
{
	for (TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(User);
		Focused.IsValid(); Focused = Focused->GetParentWidget())
	{
		if (Focused == Widget)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<SWindow> WindowOf(const TSharedPtr<SWidget>& Widget)
{
	return Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
}

/** On the headset's menu surface (in its window). */
bool OnMenuSurface(const FWoTVRPresentation* Presentation, const TSharedPtr<SWidget>& Widget)
{
	const FWoTVRSlateSurface* Menu = Presentation ? Presentation->GetMenuSurface() : nullptr;
	return Menu && WindowOf(Widget) == StaticCastSharedRef<SWindow>(Menu->GetWindow());
}

bool OnCardSurface(const FWoTVRPresentation* Presentation, const TSharedPtr<SWidget>& Widget)
{
	const FWoTVRSlateSurface* Cards = Presentation ? Presentation->GetCardSurface() : nullptr;
	return Cards && WindowOf(Widget) == StaticCastSharedRef<SWindow>(Cards->GetWindow());
}

/** A widget's centre in its window's pixels, as last painted. */
FVector2D Center(const TSharedPtr<SWidget>& Widget)
{
	return FVector2D(Widget->GetCachedGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
}

/** Turns the synthetic head to look at a pixel of the menu panel. */
void LookAtPixel(FWoTVRSyntheticSession& Session, const FWoTVRPresentation& Presentation, const FVector2D& Pixel)
{
	const FWoTVRSlateSurface* Menu = Presentation.GetMenuSurface();
	const FVector Point = Presentation.GetMenuPanel().GetPose().PixelToTracking(Pixel, Menu->GetPixels());
	const FVector Head = Session.Pose.PositionMeters;
	const FRotator Look = (Point - Head).Rotation();
	Session.SetHead(Head, FRotator(Look.Pitch, Look.Yaw, 0.0));
}

/** Turns the synthetic head to look at a widget on the menu panel. */
void LookAt(FWoTVRSyntheticSession& Session, const FWoTVRPresentation& Presentation, const TSharedPtr<SWidget>& Widget)
{
	LookAtPixel(Session, Presentation, Center(Widget));
}

/** A physical key press and release, as the platform delivers them to Slate.
 * True when something before the widgets took both. */
bool Press(int32 User, const FKey& Key)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const FKeyEvent Event(Key, FModifierKeysState(), User, false, 0, 0);
	const bool bDown = Slate.ProcessKeyDownEvent(Event);
	const bool bUp = Slate.ProcessKeyUpEvent(Event);
	return bDown && bUp;
}

/** A physical left click somewhere on the desktop window. */
bool MouseClick(int32 User)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const FVector2D Where(5.0, 5.0);
	TSet<FKey> Pressed;
	Pressed.Add(EKeys::LeftMouseButton);
	const FPointerEvent Down(User, FSlateApplication::CursorPointerIndex, Where, Where, Pressed,
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
	const FPointerEvent Up(User, FSlateApplication::CursorPointerIndex, Where, Where, TSet<FKey>(),
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
	const bool bDown = Slate.ProcessMouseButtonDownEvent(nullptr, Down);
	const bool bUp = Slate.ProcessMouseButtonUpEvent(Up);
	return bDown && bUp;
}

/** Counts a gameplay action binding reaching the controller. */
void SpyOn(APlayerController& PC, FName Action, TSharedRef<int32> Count)
{
	if (PC.InputComponent)
	{
		FInputActionBinding Binding(Action, IE_Pressed);
		Binding.bConsumeInput = false;
		Binding.ActionDelegate.GetDelegateForManualSet().BindLambda([Count]() { ++*Count; });
		PC.InputComponent->AddActionBinding(Binding);
	}
}

/** A passive card like the onboarding module's: no focus, never hit. */
TSharedRef<SWidget> MakePassiveCard()
{
	TSharedRef<SWidget> Card = SNew(SBox).WidthOverride(300.0f).HeightOverride(100.0f);
	Card->SetVisibility(EVisibility::HitTestInvisible);
	return Card;
}

float SurfaceDpi(const FIntPoint& Pixels)
{
	return GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(Pixels);
}

/** The desktop mouse moving over the desktop window, as the platform sends it. */
void DesktopMove(int32 User, const FVector2D& Where)
{
	const FPointerEvent Move(User, FSlateApplication::CursorPointerIndex, Where, Where - FVector2D(2.0, 2.0),
		TSet<FKey>(), EKeys::Invalid, 0.0f, FModifierKeysState());
	FSlateApplication::Get().ProcessMouseMoveEvent(Move);
}

/** The first widget of this type in a widget's tree, or null. */
TSharedPtr<SWidget> FirstOfType(const TSharedRef<SWidget>& Root, FName Type)
{
	if (Root->GetType() == Type)
	{
		return Root;
	}
	FChildren* Children = Root->GetChildren();
	for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
	{
		if (TSharedPtr<SWidget> Found = FirstOfType(Children->GetChildAt(Index), Type))
		{
			return Found;
		}
	}
	return nullptr;
}

/** A window of two buttons, open over UI-only input as the game's windows are. */
struct FTwoButtons
{
	TSharedPtr<SButton> Top;
	TSharedPtr<SButton> Bottom;
	int32 TopClicks = 0;
	int32 BottomClicks = 0;

	TSharedRef<SWidget> Make()
	{
		return SNew(SBorder)
			.Padding(FMargin(200.0f, 150.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 80.0f)
				[
					SAssignNew(Top, SButton).OnClicked_Lambda([this]() { ++TopClicks; return FReply::Handled(); })
					[
						SNew(SBox).WidthOverride(500.0f).HeightOverride(140.0f)
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(Bottom, SButton).OnClicked_Lambda([this]() { ++BottomClicks; return FReply::Handled(); })
					[
						SNew(SBox).WidthOverride(500.0f).HeightOverride(140.0f)
					]
				]
			];
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRUIScreensSpec, "WoT.VR.UI.EveryScreenOnTheHeadset", WoTVRTest::Flags)
bool FWoTVRUIScreensSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	UWoTGameViewportClient& Viewport = S.Viewport();

	// Flat: the front end and the in-game menu on the viewport, the menu focused.
	const TSharedRef<SWidget> FrontEnd = WoTUIScreensTest::MakeFrontEnd();
	const TSharedRef<SWidget> Menu = WoTUIScreensTest::MakeInGameMenu(*S.F.PC);
	Viewport.AddViewportWidgetContent(FrontEnd, 1000);
	Viewport.AddViewportWidgetContent(Menu, 2000);
	if (!TestTrue(TEXT("the menu holds UI-only input"), S.F.SetOverlayInput(true))) return false;
	const TSharedPtr<SWidget> MenuFocus = WoTUIScreensTest::InGameMenuFocus(Menu);
	Slate.SetUserFocus(S.User(), MenuFocus, EFocusCause::SetDirectly);
	TestTrue(TEXT("flat: the menu is on the viewport and focused"), S.OnViewport(Menu, 2000)
		&& FocusInside(S.User(), MenuFocus));

	// VR: the open screens move to the headset's surface, focus and all.
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestTrue(TEXT("the headset menus attach"), Presentation && Presentation->IsAttached())) return false;
	TestTrue(TEXT("the front end is on the headset surface"), OnMenuSurface(Presentation, FrontEnd)
		&& !S.OnViewport(FrontEnd));
	TestTrue(TEXT("the in-game menu is on the headset surface"), OnMenuSurface(Presentation, Menu)
		&& !S.OnViewport(Menu));
	TestTrue(TEXT("its focus came with it"), FocusInside(S.User(), MenuFocus));
	S.F.Frames(1);
	TestTrue(TEXT("the menu panel shows"), Presentation->GetMenuPanel().IsShown());
	IStereoLayers::FLayerDesc Desc;
	if (TestTrue(TEXT("the menu panel is a stereo layer"), Stereo.HMD->GetStereoLayers()->GetLayerDesc(
		Presentation->GetMenuPanel().GetLayerId(), Desc)))
	{
		TestTrue(TEXT("the surface is its one texture"), Desc.TextureObj.Get() == Presentation->GetMenuSurface()->GetTarget());
		TestTrue(TEXT("menus draw over the HUD and the board"), Desc.Priority > FWoTVRPresentation::ReadingPriority);
		TestEqual(TEXT("world-locked about 1.5 m in front of the view"),
			FVector::Distance(Presentation->GetMenuPanel().GetPose().Center, Scope.Session->Pose.PositionMeters), 1.5, 1e-3);
	}
	// Hosted screens lay out at the surface's size, as on a viewport that big.
	const FIntPoint Pixels = Presentation->GetMenuSurface()->GetPixels();
	const FGeometry& MenuGeometry = Menu->GetCachedGeometry();
	TestTrue(TEXT("the menu lays out over the whole surface"), (MenuGeometry.GetLocalSize()
		* MenuGeometry.GetAccumulatedLayoutTransform().GetScale()).Equals(FVector2D(Pixels), 1.0));
	TestEqual(TEXT("at the viewport DPI rule for the surface's size"),
		MenuGeometry.GetAccumulatedLayoutTransform().GetScale(), SurfaceDpi(Pixels), 1e-4f);
	TestEqual(TEXT("screens that open size for the surface"), FWoTLocalPlayerHooks::UIScreenSize(Viewport),
		FVector2D(Pixels));

	// The front end's own screens: the server browser and the class and team
	// choice, on the same surface.
	const FVector2D Logical = FVector2D(Pixels) / SurfaceDpi(Pixels);
	WoTUIScreensTest::OpenServerBrowser(FrontEnd);
	WoTUIScreensTest::SettleFrontEnd(FrontEnd, Logical, [&S]() { S.F.Frames(1); });
	TestTrue(TEXT("the server browser's address field is on the headset surface"),
		OnMenuSurface(Presentation, WoTUIScreensTest::AddressField(FrontEnd)));
	WoTUIScreensTest::OpenPlayerSetup(FrontEnd);
	WoTUIScreensTest::SettleFrontEnd(FrontEnd, Logical, [&S]() { S.F.Frames(1); });
	TestTrue(TEXT("the class and team choice is on the headset surface"),
		OnMenuSurface(Presentation, WoTUIScreensTest::PlayerNameField(FrontEnd)));

	// Screens that open in VR open on the headset; passive ones beside the HUD.
	const TSharedRef<SWidget> ItemInfo = WoTUIScreensTest::MakeItemInfo(*S.F.PC);
	Viewport.AddViewportWidgetContent(ItemInfo, 1900);
	TestTrue(TEXT("item information opens on the headset surface"), OnMenuSurface(Presentation, ItemInfo));
	int32 HelpCloses = 0;
	const TSharedRef<SWidget> Help = WoTUIScreensTest::MakeHelp(FWoTLocalPlayerHooks::UIScreenSize(Viewport),
		[&HelpCloses]() { ++HelpCloses; });
	Viewport.AddViewportWidgetContent(Help, 2100);
	TestTrue(TEXT("help opens on the headset surface"), OnMenuSurface(Presentation, Help));
	TestEqual(TEXT("help is sized for the surface from its first frame (2x)"),
		WoTUIScreensTest::HelpPixelScale(Help), 2);
	TestEqual(TEXT("where the viewport would have made it 1x"),
		WoTUIScreensTest::HelpPixelScale(WoTUIScreensTest::MakeHelp(FVector2D(640.0, 480.0))), 1);
	const TSharedRef<SWidget> Notice = WoTUIScreensTest::MakeNotice();
	Viewport.AddViewportWidgetContent(Notice, 2300);
	const TSharedRef<SWidget> Card = MakePassiveCard();
	Viewport.AddViewportWidgetContent(Card, 1850);
	TestTrue(TEXT("the notice shows beside the HUD panel"), OnCardSurface(Presentation, Notice));
	TestTrue(TEXT("a passive card shows beside the HUD panel"), OnCardSurface(Presentation, Card));
	S.F.Frames(1);
	TestTrue(TEXT("the card panel shows"), Presentation->GetCardPanel().IsShown());
	TestTrue(TEXT("beside the HUD panel"), FVector::Distance(Presentation->GetCardPanel().GetPose().Center,
		Presentation->GetHUDPanel().GetPose().Center) < 1.0);

	// Chat and its keyboard panel, on the same surface.
	const TSharedPtr<SWidget> Chat = WoTUIScreensTest::OpenControllerChat(*S.F.PC);
	if (TestTrue(TEXT("controller chat opens"), Chat.IsValid()))
	{
		TestTrue(TEXT("the chat line opens on the headset surface"), OnMenuSurface(Presentation, Chat));
		TestEqual(TEXT("sized for the surface"), WoTUIScreensTest::ChatDpiScale(Chat.ToSharedRef()),
			SurfaceDpi(Pixels), 1e-4f);
		const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
		TestTrue(TEXT("the keyboard panel opens on the same surface"), Panel.IsValid()
			&& OnMenuSurface(Presentation, Panel) && FocusInside(S.User(), Panel));
		FWoTTextEntry::Cancel();
		WoTUIScreensTest::CloseChat(*S.F.PC);
		TestFalse(TEXT("closed chat is in neither place"), OnMenuSurface(Presentation, Chat) || S.OnViewport(Chat.ToSharedRef()));
	}

	// Flat: every screen back on the viewport at its z-order, focus intact.
	Slate.SetUserFocus(S.User(), MenuFocus, EFocusCause::SetDirectly);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	TestNull(TEXT("the headset menus are gone"), Rig->GetPresentation());
	const TPair<TSharedRef<SWidget>, int32> Expected[] = {{FrontEnd, 1000}, {Card, 1850}, {ItemInfo, 1900},
		{Menu, 2000}, {Help, 2100}, {Notice, 2300}};
	for (const TPair<TSharedRef<SWidget>, int32>& Screen : Expected)
	{
		TestTrue(FString::Printf(TEXT("%s is back on the viewport at %d"), *Screen.Key->GetTypeAsString(), Screen.Value),
			S.OnViewport(Screen.Key, Screen.Value) && WindowOf(Screen.Key) == S.Window);
	}
	TestEqual(TEXT("nothing else is on the viewport, nothing stranded"), Viewport.GetOverlayContent().Num(), 6);
	TestTrue(TEXT("the menu kept its focus"), FocusInside(S.User(), MenuFocus) && WindowOf(MenuFocus) == S.Window);
	TestEqual(TEXT("flat: screens size for the viewport again"), FWoTLocalPlayerHooks::UIScreenSize(Viewport),
		FVector2D(640.0, 480.0));
	TestFalse(TEXT("no registration left"), FWoTLocalPlayerHooks::HasConsumers(S.F.Player));
	TestEqual(TEXT("help was never closed by the moves"), HelpCloses, 0);
	for (const TPair<TSharedRef<SWidget>, int32>& Screen : Expected)
	{
		Viewport.RemoveViewportWidgetContent(Screen.Key);
	}
	S.F.SetOverlayInput(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRUIPointerSpec, "WoT.VR.UI.GazePointerConfirmAndNavigation", WoTVRTest::Flags)
bool FWoTVRUIPointerSpec::RunTest(const FString&)
{
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = S.User();
	TSharedRef<int32> Fires = MakeShared<int32>(0);
	TSharedRef<int32> Jumps = MakeShared<int32>(0);
	SpyOn(*S.F.PC, TEXT("Fire"), Fires);
	SpyOn(*S.F.PC, TEXT("Jump"), Jumps);
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;

	// A passive card alone takes no input: play carries on.
	const TSharedRef<SWidget> Card = MakePassiveCard();
	S.Viewport().AddViewportWidgetContent(Card, 1850);
	S.F.Frames(1);
	TestFalse(TEXT("a passive card does not hold the game's input"), FWoTLocalPlayerHooks::IsGameInputBlocked(S.F.PC));
	TestFalse(TEXT("a passive card takes no menu input"), Presentation->IsMenuInputActive());
	TestFalse(TEXT("A stays the game's"), Presentation->HandleKey(
		FKeyEvent(EKeys::Gamepad_FaceButton_Bottom, FModifierKeysState(), User, false, 0, 0), true));
	TestFalse(TEXT("the left mouse button stays the game's"), Presentation->HandleMouseButton(
		FPointerEvent(User, FSlateApplication::CursorPointerIndex, FVector2D::ZeroVector, FVector2D::ZeroVector,
			TSet<FKey>({EKeys::LeftMouseButton}), EKeys::LeftMouseButton, 0.0f, FModifierKeysState()), true, false));
	S.Viewport().RemoveViewportWidgetContent(Card);

	// A window of two buttons, open over UI-only input as the game's windows are.
	TSharedPtr<SButton> Top;
	TSharedPtr<SButton> Bottom;
	int32 TopClicks = 0;
	int32 BottomClicks = 0;
	const TSharedRef<SWidget> Window = SNew(SBorder)
		.Padding(FMargin(200.0f, 150.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 80.0f)
			[
				SAssignNew(Top, SButton).OnClicked_Lambda([&TopClicks]() { ++TopClicks; return FReply::Handled(); })
				[
					SNew(SBox).WidthOverride(500.0f).HeightOverride(140.0f)
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(Bottom, SButton).OnClicked_Lambda([&BottomClicks]() { ++BottomClicks; return FReply::Handled(); })
				[
					SNew(SBox).WidthOverride(500.0f).HeightOverride(140.0f)
				]
			]
		];
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	if (!TestTrue(TEXT("the window holds UI-only input"), S.F.SetOverlayInput(true))) return false;
	S.F.Frames(2);
	TestTrue(TEXT("the window is on the headset surface"), OnMenuSurface(Presentation, Window));
	TestTrue(TEXT("menu input is the window's"), Presentation->IsMenuInputActive());

	// The gaze finds the widget it looks at, and the reticle marks it.
	LookAt(*Scope.Session, *Presentation, Top);
	S.F.Frames(1);
	const TOptional<FVector2D> Pointer = Presentation->GetPointer();
	if (!TestTrue(TEXT("the gaze is on the surface"), Pointer.IsSet())) return false;
	TestTrue(TEXT("at the button it looks at"), Pointer->Equals(Center(Top), 1.0));
	const TOptional<FVector2D> Reticle = Presentation->GetMenuSurface()->GetReticle();
	TestTrue(TEXT("the reticle is drawn there"), Reticle.IsSet() && Reticle->Equals(*Pointer, 0.01));
	TestTrue(TEXT("the button under the gaze is hovered"), Top->IsHovered() && !Bottom->IsHovered());

	// Confirm: A, Enter or the left mouse button, where the gaze is.
	TestTrue(TEXT("A is taken"), Press(User, EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("A clicks what the gaze is on"), TopClicks, 1);
	TestTrue(TEXT("Enter is taken"), Press(User, EKeys::Enter));
	TestEqual(TEXT("Enter clicks it too"), TopClicks, 2);
	TestTrue(TEXT("the mouse button is taken"), MouseClick(User));
	TestEqual(TEXT("and the left mouse button"), TopClicks, 3);
	LookAt(*Scope.Session, *Presentation, Bottom);
	S.F.Frames(1);
	TestTrue(TEXT("A on the other button"), Press(User, EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("clicks the other button"), BottomClicks, 1);
	TestEqual(TEXT("and not the first"), TopClicks, 3);

	// Looking away, confirm presses the focused widget; the D-pad and the stick
	// move focus, all as the player's one Slate user.
	Scope.Session->SetHead(Scope.Session->Pose.PositionMeters, FRotator(0.0, 170.0, 0.0));
	S.F.Frames(1);
	TestFalse(TEXT("looking away, the gaze is off the surface"), Presentation->GetPointer().IsSet());
	Slate.SetUserFocus(User, Top, EFocusCause::SetDirectly);
	TestTrue(TEXT("D-pad down is routed"), Press(User, EKeys::Gamepad_DPad_Down));
	TestTrue(TEXT("and moves focus down"), FocusInside(User, Bottom));
	TestTrue(TEXT("A without the gaze confirms the focused button"), Press(User, EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("the focused button"), BottomClicks, 2);
	FAnalogInputEvent Up(EKeys::Gamepad_LeftY, FModifierKeysState(), User, false, 0, 0, 0.9f);
	TestTrue(TEXT("the stick is routed"), Slate.ProcessAnalogInputEvent(Up));
	FAnalogInputEvent Rest(EKeys::Gamepad_LeftY, FModifierKeysState(), User, false, 0, 0, 0.0f);
	Slate.ProcessAnalogInputEvent(Rest);
	TestTrue(TEXT("and moves focus up"), FocusInside(User, Top));

	// B backs out, through the same route: help closes on it.
	int32 HelpCloses = 0;
	const TSharedRef<SWidget> Help = WoTUIScreensTest::MakeHelp(FWoTLocalPlayerHooks::UIScreenSize(S.Viewport()),
		[&HelpCloses]() { ++HelpCloses; });
	S.Viewport().AddViewportWidgetContent(Help, 2600);
	S.F.Frames(1);
	Slate.SetUserFocus(User, Help, EFocusCause::SetDirectly);
	TestTrue(TEXT("B is routed"), Press(User, EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("B closes help"), HelpCloses, 1);
	S.Viewport().RemoveViewportWidgetContent(Help);

	// Nothing reached the game while the window held its input.
	TestEqual(TEXT("no cast"), *Fires, 0);
	TestEqual(TEXT("no jump"), *Jumps, 0);
	// Pointer events have no world authority: the gaze only reaches the surface.
	TestFalse(TEXT("the viewport never took the player's focus"), FocusInside(User, S.F.Widget));

	S.F.SetOverlayInput(false);
	S.F.Frames(1);
	TestFalse(TEXT("with the window's input released, A is the game's again"), Presentation->HandleKey(
		FKeyEvent(EKeys::Gamepad_FaceButton_Bottom, FModifierKeysState(), User, false, 0, 0), true));
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRUITextEntrySpec, "WoT.VR.UI.PasswordEntryFromTheGaze", WoTVRTest::Flags)
bool FWoTVRUITextEntrySpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	const int32 User = S.User();
	// The front end, pawnless, with its UI-only input.
	S.F.PC->UnPossess();
	const TSharedRef<SWidget> FrontEnd = WoTUIScreensTest::MakeFrontEnd();
	S.Viewport().AddViewportWidgetContent(FrontEnd, 1000);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	const FIntPoint Pixels = Presentation->GetMenuSurface()->GetPixels();
	const FVector2D Logical = FVector2D(Pixels) / SurfaceDpi(Pixels);
	WoTUIScreensTest::OpenServerBrowser(FrontEnd);
	WoTUIScreensTest::SettleFrontEnd(FrontEnd, Logical, [&S]() { S.F.Frames(1); });
	const TSharedPtr<SWidget> Password = WoTUIScreensTest::GamePasswordField(FrontEnd);
	if (!TestTrue(TEXT("the password field is on the headset surface"), OnMenuSurface(Presentation, Password))) return false;

	for (const bool bDone : {true, false})
	{
		const FString Label = bDone ? TEXT("Done") : TEXT("Cancel");
		// Look at the field and confirm: controller text entry on the panel.
		LookAt(*Scope.Session, *Presentation, Password);
		S.F.Frames(1);
		TestTrue(Label + TEXT(": the gaze is on the field"), Presentation->GetPointer().IsSet()
			&& Presentation->GetPointer()->Equals(Center(Password), 1.0));
		TestTrue(Label + TEXT(": A is taken"), Press(User, EKeys::Gamepad_FaceButton_Bottom));
		const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
		if (!TestTrue(Label + TEXT(": the gaze and confirm start entry on the panel"), FWoTTextEntry::IsActive()
			&& Panel.IsValid() && FWoTTextEntry::GetActiveRoute() == EWoTTextEntryRoute::Panel)) return false;
		TestTrue(Label + TEXT(": the panel is on the same surface"), OnMenuSurface(Presentation, Panel));
		TestTrue(Label + TEXT(": the panel has the player's focus"), FocusInside(User, Panel));
		TestTrue(Label + TEXT(": masked"), Panel->IsPassword());
		Panel->SetDraft(TEXT("Secret1"));
		TestFalse(Label + TEXT(": the preview hides it"), Panel->GetPreviewText().Contains(TEXT("Secret")));
		// The field's limit holds.
		const int32 Limit = Panel->GetMaxLength();
		if (TestTrue(Label + TEXT(": the field has a limit"), Limit > 0 && Limit < 1000))
		{
			Panel->SetDraft(FString());
			for (int32 Index = 0; Index < Limit + 6; ++Index)
			{
				Panel->TypeCharacter(TEXT('a') + (Index % 26));
			}
			TestEqual(Label + TEXT(": the limit holds"), Panel->GetDraft().Len(), Limit);
		}
		if (bDone)
		{
			TestTrue(Label, FWoTTextEntry::Done());
		}
		else
		{
			FWoTTextEntry::Cancel();
		}
		TestFalse(Label + TEXT(": the entry ends"), FWoTTextEntry::IsActive());
		TestTrue(Label + TEXT(": focus is back on the field"), FocusInside(User, Password));
		TestTrue(Label + TEXT(": still on the headset surface"), OnMenuSurface(Presentation, Password));
	}
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	TestTrue(TEXT("the front end is back on the viewport"), S.OnViewport(FrontEnd, 1000));
	TestFalse(TEXT("without the headset the panel is no longer forced"), FWoTLocalPlayerHooks::UsesPanelTextEntry());
	S.Viewport().RemoveViewportWidgetContent(FrontEnd);
	S.F.SetOverlayInput(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRUILifecycleSpec, "WoT.VR.UI.LifecycleSwitchTravelSessionLoss", WoTVRTest::Flags)
bool FWoTVRUILifecycleSpec::RunTest(const FString&)
{
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<SWidget> Menu = WoTUIScreensTest::MakeInGameMenu(*S.F.PC);
	S.Viewport().AddViewportWidgetContent(Menu, 2000);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	const TSharedPtr<SWidget> MenuFocus = WoTUIScreensTest::InGameMenuFocus(Menu);
	Slate.SetUserFocus(S.User(), MenuFocus, EFocusCause::SetDirectly);
	// The head looks up, over the menu: the gaze hovers no row (a hovered row
	// takes focus, as under a mouse), so only the switches act on focus.
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(60.0, 0.0, 0.0));
	auto Hosted = [&Rig, &Menu]() { return OnMenuSurface(Rig->GetPresentation(), Menu); };
	auto OnDesktop = [&S, &Menu]() { return S.OnViewport(Menu, 2000) && WindowOf(Menu) == S.Window; };
	auto Focused = [this, &S, &MenuFocus, &Menu]()
	{
		const bool bInside = FocusInside(S.User(), MenuFocus);
		if (!bInside)
		{
			const TSharedPtr<SWidget> Now = FSlateApplication::Get().GetUserFocusedWidget(S.User());
			bool bRowInMenu = false;
			for (TSharedPtr<SWidget> Parent = MenuFocus; Parent.IsValid(); Parent = Parent->GetParentWidget())
			{
				bRowInMenu |= Parent == Menu;
			}
			UE_LOG(LogTemp, Display, TEXT("WoT.VR.UI: focus is on %s (window %s); the menu's row is %sinside the menu"),
				Now.IsValid() ? *Now->GetTypeAsString() : TEXT("nothing"),
				WindowOf(Now).IsValid() ? *WindowOf(Now)->GetTypeAsString() : TEXT("none"),
				bRowInMenu ? TEXT("") : TEXT("not "));
		}
		return bInside;
	};

	// Repeated switches with the menu open: in and out, focus intact each time.
	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("cycle %d"), Cycle + 1);
		TestTrue(Label + TEXT(": switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		TestTrue(Label + TEXT(": the menu is in the headset"), Hosted() && !S.OnViewport(Menu));
		TestTrue(Label + TEXT(": with its focus"), Focused());
		S.F.Frames(2);
		TestTrue(Label + TEXT(": switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		TestTrue(Label + TEXT(": the menu is back on the viewport"), OnDesktop());
		TestTrue(Label + TEXT(": with its focus"), Focused());
		TestFalse(Label + TEXT(": nothing left registered"), FWoTLocalPlayerHooks::HasConsumers(S.F.Player));
	}

	// Session loss: the desktop window shows the menu, usable, until the
	// session recovers.
	TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
	TestTrue(TEXT("in the headset"), Hosted());
	Scope.Session->LoseSession();
	S.F.Frames(2);
	TestTrue(TEXT("session lost: holding still"), Rig->IsHoldingStill());
	TestNull(TEXT("session lost: the headset menus detach"), Rig->GetPresentation());
	TestTrue(TEXT("the desktop window shows the menu"), OnDesktop());
	TestTrue(TEXT("still focused, still usable"), Focused());
	TestFalse(TEXT("the flat HUD is back"), FWoTLocalPlayerHooks::OwnsHUDDestination(S.F.PC));
	Scope.Session->RecoverSession();
	S.F.Frames(2);
	TestTrue(TEXT("recovered: the menu is in the headset again"), Hosted());
	TestTrue(TEXT("with its focus"), Focused());

	// Travel: the screens go back to the desktop window for it, and stay there
	// until the next world.
	FWoTLocalPlayerHooks::Notify(*S.F.PC, EWoTLocalPlayerEvent::Travel);
	TestNull(TEXT("travel: the headset menus detach"), Rig->GetPresentation());
	TestTrue(TEXT("travel: the menu is on the desktop window"), OnDesktop());
	S.F.Frames(2);
	TestNull(TEXT("not in the same world again"), Rig->GetPresentation());
	TestTrue(TEXT("a new switch to VR presents again"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"))
		&& Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")) && Hosted());

	// A switch to flat with the menu open returns it with focus intact.
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	TestTrue(TEXT("the menu is back"), OnDesktop() && Focused());
	TestFalse(TEXT("no registration leaked"), FWoTLocalPlayerHooks::HasConsumers(S.F.Player));
	S.Viewport().RemoveViewportWidgetContent(Menu);
	S.F.SetOverlayInput(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixPausedMenuSpec, "WoT.VR.HeadsetFixes.MenuShownWhilePaused", WoTVRTest::Flags)
bool FWoTVRFixPausedMenuSpec::RunTest(const FString&)
{
	// A paused world (the Pause key, the map-load barrier) skips its post-actor
	// tick (UWorld::Tick). The panels are placed, pointed at and painted every
	// world tick all the same, so a menu opened while paused shows and works.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	S.F.Frames(1);
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;

	// Paused, as the game pauses: a pauser on the world settings.
	AWorldSettings* Settings = S.F.World->GetWorldSettings();
	APlayerState* Pauser = S.F.PC->PlayerState ? S.F.PC->PlayerState.Get() : S.F.World->SpawnActor<APlayerState>();
	if (!TestNotNull(TEXT("world settings"), Settings) || !TestNotNull(TEXT("a pauser"), Pauser)) return false;
	Settings->SetPauserPlayerState(Pauser);
	if (!TestTrue(TEXT("the world is paused"), S.F.World->IsPaused())) return false;
	const int32 Paints = Presentation->GetMenuSurface()->GetPaintCount();

	FTwoButtons Buttons;
	const TSharedRef<SWidget> Window = Buttons.Make();
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	S.F.SetOverlayInput(true);
	S.F.Frames(2);
	TestTrue(TEXT("paused: the window is on the headset surface"), OnMenuSurface(Presentation, Window));
	TestTrue(TEXT("paused: the menu panel shows"), Presentation->GetMenuPanel().IsShown());
	TestTrue(TEXT("paused: the surface paints"), Presentation->GetMenuSurface()->GetPaintCount() > Paints);
	TestEqual(TEXT("paused: placed about 1.5 m in front of the view"),
		FVector::Distance(Presentation->GetMenuPanel().GetPose().Center, Scope.Session->Pose.PositionMeters), 1.5, 1e-3);
	IStereoLayers::FLayerDesc Desc;
	TestTrue(TEXT("paused: the device shows its layer"), Stereo.HMD->GetStereoLayers()->GetLayerDesc(
		Presentation->GetMenuPanel().GetLayerId(), Desc) && (Desc.Flags & IStereoLayers::LAYER_FLAG_HIDDEN) == 0);

	// The gaze points and confirms while paused too.
	LookAt(*Scope.Session, *Presentation, Buttons.Top);
	S.F.Frames(1);
	TestTrue(TEXT("paused: the gaze is on the button"), Presentation->GetPointer().IsSet() && Buttons.Top->IsHovered());
	Press(S.User(), EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("paused: A clicks it"), Buttons.TopClicks, 1);

	Settings->SetPauserPlayerState(nullptr);
	S.F.SetOverlayInput(false);
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixKeptSurfacesSpec, "WoT.VR.HeadsetFixes.SurfacesKeptUntilFlat", WoTVRTest::Flags)
bool FWoTVRFixKeptSurfacesSpec::RunTest(const FString&)
{
	// Travel, tracking loss and the runtime's dashboard detach the headset HUD
	// and menus. Their render targets, offscreen windows, renderer and layers
	// stay (the layers hidden, nothing painted) and come back as they were; only
	// the switch to flat releases them.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	FWoTVRSpecLayers* Layers = Stereo.HMD->GetSpecLayers();
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("the spec display's layers"), Layers)) return false;
	const TSharedRef<SWidget> Menu = WoTUIScreensTest::MakeInGameMenu(*S.F.PC);
	S.Viewport().AddViewportWidgetContent(Menu, 2000);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	// The head looks over the menu: the gaze hovers nothing.
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(60.0, 0.0, 0.0));
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	S.F.Frames(2);
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	const uint32 MenuLayer = Presentation->GetMenuPanel().GetLayerId();
	if (!TestTrue(TEXT("the menu shows on its layer"), MenuLayer != 0 && Presentation->GetMenuPanel().IsShown())) return false;
	const TWeakPtr<SVirtualWindow> MenuWindow = Presentation->GetMenuSurface()->GetWindow();
	const TWeakPtr<SVirtualWindow> CardWindow = Presentation->GetCardSurface()->GetWindow();
	const TWeakObjectPtr<UTextureRenderTarget2D> MenuTarget = Presentation->GetMenuSurface()->GetTarget();
	const TWeakObjectPtr<UTextureRenderTarget2D> CardTarget = Presentation->GetCardSurface()->GetTarget();
	const TWeakObjectPtr<UTextureRenderTarget2D> HUDTarget = Presentation->GetHUDSurface()->GetTarget();
	const TWeakObjectPtr<UTextureRenderTarget2D> ReadingTarget = Presentation->GetReadingSurface()->GetTarget();
	const int32 Creates = Layers->Creates;
	const int32 Destroys = Layers->Destroys;
	// World-locked and still: the menu's layer is not rewritten frame by frame.
	const int32 MenuWrites = Layers->WritesTo(MenuLayer);
	S.F.Frames(4);
	TestEqual(TEXT("a world-locked menu is not rewritten each frame"), Layers->WritesTo(MenuLayer), MenuWrites);

	auto Kept = [&](const FString& Label)
	{
		TestNull(Label + TEXT(": the headset menus detach"), Rig->GetPresentation());
		TestTrue(Label + TEXT(": the menu is on the desktop window"), S.OnViewport(Menu, 2000));
		IStereoLayers::FLayerDesc Desc;
		TestTrue(Label + TEXT(": the menu's layer is hidden, not destroyed"), Layers->GetLayerDesc(MenuLayer, Desc)
			&& (Desc.Flags & IStereoLayers::LAYER_FLAG_HIDDEN) != 0);
		TestEqual(Label + TEXT(": no layer destroyed"), Layers->Destroys, Destroys);
		const TSharedPtr<SVirtualWindow> KeptWindow = MenuWindow.Pin();
		TestTrue(Label + TEXT(": the windows are kept"), KeptWindow.IsValid() && CardWindow.IsValid());
		// Slate's search of its registered windows (top-level and virtual).
		FWidgetPath Registered;
		TestTrue(Label + TEXT(": the menu window leaves Slate meanwhile"), KeptWindow.IsValid()
			&& !FSlateApplication::Get().FindWidgetWindow(KeptWindow->GetContent(), Registered).IsValid());
		const int32 Writes = Layers->TotalWrites();
		S.F.Frames(2);
		TestEqual(Label + TEXT(": nothing is written while detached"), Layers->TotalWrites(), Writes);
	};
	auto Back = [&](const FString& Label)
	{
		FWoTVRPresentation* Again = Rig->GetPresentation();
		if (!TestNotNull(Label + TEXT(": the headset menus attach again"), Again))
		{
			return;
		}
		TestTrue(Label + TEXT(": on the same menu window"), MenuWindow.Pin().Get() == &Again->GetMenuSurface()->GetWindow().Get());
		TestTrue(Label + TEXT(": and the same card window"), CardWindow.Pin().Get() == &Again->GetCardSurface()->GetWindow().Get());
		TestTrue(Label + TEXT(": with the same textures"), MenuTarget.Get() == Again->GetMenuSurface()->GetTarget()
			&& CardTarget.Get() == Again->GetCardSurface()->GetTarget()
			&& HUDTarget.Get() == Again->GetHUDSurface()->GetTarget()
			&& ReadingTarget.Get() == Again->GetReadingSurface()->GetTarget());
		TestEqual(Label + TEXT(": the same menu layer"), static_cast<int32>(Again->GetMenuPanel().GetLayerId()),
			static_cast<int32>(MenuLayer));
		TestEqual(Label + TEXT(": no layer made"), Layers->Creates, Creates);
		TestTrue(Label + TEXT(": the menu is in the headset"), OnMenuSurface(Again, Menu));
		TestTrue(Label + TEXT(": and shows"), Again->GetMenuPanel().IsShown());
		FWidgetPath Registered;
		TestTrue(Label + TEXT(": the menu window is Slate's again"), FSlateApplication::Get().FindWidgetWindow(
			Again->GetMenuSurface()->GetWindow()->GetContent(), Registered).IsValid());
	};

	// Tracking or session loss, and back.
	Scope.Session->LoseSession();
	S.F.Frames(2);
	Kept(TEXT("session lost"));
	Scope.Session->RecoverSession();
	S.F.Frames(2);
	Back(TEXT("recovered"));
	// The runtime's dashboard: the session runs without input focus.
	Scope.Session->RunningState = EWoTVRSessionState::Visible;
	S.F.Frames(2);
	Kept(TEXT("dashboard open"));
	Scope.Session->RunningState = EWoTVRSessionState::Running;
	S.F.Frames(2);
	Back(TEXT("dashboard closed"));
	// Travel: kept for the next world.
	FWoTLocalPlayerHooks::Notify(*S.F.PC, EWoTLocalPlayerEvent::Travel);
	Kept(TEXT("travel"));

	// The switch to flat releases all of it.
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	IStereoLayers::FLayerDesc Desc;
	TestFalse(TEXT("flat: the menu's layer is destroyed"), Layers->GetLayerDesc(MenuLayer, Desc));
	TestTrue(TEXT("flat: every layer made is destroyed"), Layers->Destroys - Destroys == Layers->Creates);
	TestFalse(TEXT("flat: the windows are released"), MenuWindow.IsValid() || CardWindow.IsValid());
	TestTrue(TEXT("flat: the menu is on the desktop window"), S.OnViewport(Menu, 2000));
	S.Viewport().RemoveViewportWidgetContent(Menu);
	S.F.SetOverlayInput(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixNavigatedFocusSpec, "WoT.VR.HeadsetFixes.NavigatedFocusTakesConfirm",
	WoTVRTest::Flags)
bool FWoTVRFixNavigatedFocusSpec::RunTest(const FString&)
{
	// After D-pad or stick navigation, A and Enter confirm the focused control
	// while the gaze stays where it was (reading the panel), until the gaze
	// moves onto a different control; then they press at the reticle again.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = S.User();
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	FTwoButtons Buttons;
	const TSharedRef<SWidget> Window = Buttons.Make();
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	S.F.Frames(2);

	// The gaze on the top button, focus there too; the D-pad moves focus down.
	LookAt(*Scope.Session, *Presentation, Buttons.Top);
	S.F.Frames(1);
	TestTrue(TEXT("the gaze is on the top button"), Buttons.Top->IsHovered());
	Slate.SetUserFocus(User, Buttons.Top, EFocusCause::SetDirectly);
	TestFalse(TEXT("before any navigation the reticle confirms"), Presentation->IsFocusPreferred());
	TestTrue(TEXT("D-pad down is routed"), Press(User, EKeys::Gamepad_DPad_Down));
	TestTrue(TEXT("focus moves down"), FocusInside(User, Buttons.Bottom));
	TestTrue(TEXT("navigation prefers the focused control"), Presentation->IsFocusPreferred());
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A confirms the focused button, not the gazed one"), Buttons.BottomClicks, 1);
	TestEqual(TEXT("the gazed button is not clicked"), Buttons.TopClicks, 0);
	Press(User, EKeys::Enter);
	TestEqual(TEXT("so does Enter"), Buttons.BottomClicks, 2);

	// Reading: the gaze drifts within the same button.
	LookAtPixel(*Scope.Session, *Presentation, Center(Buttons.Top) + FVector2D(60.0, 15.0));
	S.F.Frames(1);
	TestTrue(TEXT("the gaze is still on the top button"), Buttons.Top->IsHovered());
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A still confirms the focused button"), Buttons.BottomClicks, 3);
	TestEqual(TEXT("not the gazed one"), Buttons.TopClicks, 0);

	// The gaze moves onto the other button: the reticle again.
	LookAt(*Scope.Session, *Presentation, Buttons.Bottom);
	S.F.Frames(1);
	TestFalse(TEXT("a gaze on another control ends the preference"), Presentation->IsFocusPreferred());
	Slate.SetUserFocus(User, Buttons.Top, EFocusCause::SetDirectly);
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A presses where the gaze is"), Buttons.BottomClicks, 4);
	TestEqual(TEXT("not the focused button"), Buttons.TopClicks, 0);

	// The stick past the threshold navigates too.
	Slate.SetUserFocus(User, Buttons.Bottom, EFocusCause::SetDirectly);
	FAnalogInputEvent Up(EKeys::Gamepad_LeftY, FModifierKeysState(), User, false, 0, 0, 0.9f);
	TestTrue(TEXT("the stick is routed"), Slate.ProcessAnalogInputEvent(Up));
	FAnalogInputEvent Rest(EKeys::Gamepad_LeftY, FModifierKeysState(), User, false, 0, 0, 0.0f);
	Slate.ProcessAnalogInputEvent(Rest);
	TestTrue(TEXT("the stick moves focus up"), FocusInside(User, Buttons.Top));
	TestTrue(TEXT("stick navigation prefers the focused control"), Presentation->IsFocusPreferred());
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A confirms the focused top button with the gaze on the bottom one"), Buttons.TopClicks, 1);
	TestEqual(TEXT("the gazed bottom button is not clicked"), Buttons.BottomClicks, 4);

	S.F.SetOverlayInput(false);
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixEnterFinishesSpec, "WoT.VR.HeadsetFixes.EnterFinishesTextEntry",
	WoTVRTest::Flags)
bool FWoTVRFixEnterFinishesSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	// On the keyboard panel a physical Enter finishes the entry, as it does
	// flat, whatever key the gaze is on.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	const int32 User = S.User();
	S.F.PC->UnPossess();
	const TSharedRef<SWidget> FrontEnd = WoTUIScreensTest::MakeFrontEnd();
	S.Viewport().AddViewportWidgetContent(FrontEnd, 1000);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	const FIntPoint Pixels = Presentation->GetMenuSurface()->GetPixels();
	const FVector2D Logical = FVector2D(Pixels) / SurfaceDpi(Pixels);
	WoTUIScreensTest::OpenPlayerSetup(FrontEnd);
	WoTUIScreensTest::SettleFrontEnd(FrontEnd, Logical, [&S]() { S.F.Frames(1); });
	const TSharedPtr<SWidget> Name = WoTUIScreensTest::PlayerNameField(FrontEnd);
	if (!TestTrue(TEXT("the name field is on the headset surface"), OnMenuSurface(Presentation, Name))) return false;

	// The gaze and A start the entry on the panel.
	LookAt(*Scope.Session, *Presentation, Name);
	S.F.Frames(1);
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("entry runs on the panel"), FWoTTextEntry::IsActive() && Panel.IsValid()
		&& OnMenuSurface(Presentation, Panel))) return false;
	Panel->SetDraft(TEXT("Gazer"));
	S.F.Frames(2);

	// The gaze rests on one of the panel's keys.
	const TSharedPtr<SWidget> Key = FirstOfType(Panel.ToSharedRef(), FName(TEXT("SButton")));
	if (!TestTrue(TEXT("the panel has keys"), Key.IsValid())) return false;
	LookAt(*Scope.Session, *Presentation, Key);
	S.F.Frames(1);
	TestTrue(TEXT("the gaze is on a key"), Presentation->GetPointer().IsSet() && Key->IsHovered());
	Press(User, EKeys::Enter);
	TestFalse(TEXT("Enter finishes the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("Done commits the draft, with no key typed"), WoTUIScreensTest::PendingPlayerName(FrontEnd),
		FString(TEXT("Gazer")));
	TestTrue(TEXT("focus is back on the field"), FocusInside(User, Name));

	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	S.Viewport().RemoveViewportWidgetContent(FrontEnd);
	S.F.SetOverlayInput(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixDesktopMouseSpec, "WoT.VR.HeadsetFixes.DesktopMouseLeavesTheGazeAlone",
	WoTVRTest::Flags)
bool FWoTVRFixDesktopMouseSpec::RunTest(const FString&)
{
	// The gaze is the player's Slate user on a pointer of its own: the desktop
	// mouse moving over the desktop window neither unhovers what the gaze is
	// on nor reaches a press the gaze holds, and its buttons neither join nor
	// end that press.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = S.User();
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	FTwoButtons Buttons;
	const TSharedRef<SWidget> Window = Buttons.Make();
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	S.F.Frames(2);
	LookAt(*Scope.Session, *Presentation, Buttons.Top);
	S.F.Frames(1);
	TestTrue(TEXT("the gaze hovers the top button"), Buttons.Top->IsHovered());

	// The desktop mouse moves over the desktop window.
	DesktopMove(User, FVector2D(5.0, 5.0));
	DesktopMove(User, FVector2D(300.0, 200.0));
	TestTrue(TEXT("the gaze keeps its hover"), Buttons.Top->IsHovered());
	S.F.Frames(1);
	TestTrue(TEXT("still, a frame later"), Buttons.Top->IsHovered());

	// A gaze press held on A: its own pointer holds the capture.
	TestTrue(TEXT("A is taken"), Slate.ProcessKeyDownEvent(
		FKeyEvent(EKeys::Gamepad_FaceButton_Bottom, FModifierKeysState(), User, false, 0, 0)));
	TestTrue(TEXT("the button is pressed"), Buttons.Top->IsPressed());
	const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(User);
	TestTrue(TEXT("on the gaze's own pointer"), SlateUser.IsValid()
		&& SlateUser->DoesWidgetHaveCapture(Buttons.Top, FWoTLocalPlayerHooks::UIPointerIndex));
	TestFalse(TEXT("not the desktop mouse's"), SlateUser.IsValid()
		&& SlateUser->DoesWidgetHaveCapture(Buttons.Top, FSlateApplication::CursorPointerIndex));
	// The desktop mouse moves away and clicks meanwhile.
	DesktopMove(User, FVector2D(600.0, 450.0));
	TestTrue(TEXT("the desktop mouse does not reach the gaze's press"), Buttons.Top->IsPressed()
		&& Buttons.Top->IsHovered());
	MouseClick(User);
	TestEqual(TEXT("the desktop mouse neither joins nor ends the press"), Buttons.TopClicks, 0);
	TestTrue(TEXT("the press still holds"), Buttons.Top->IsPressed());
	Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::Gamepad_FaceButton_Bottom, FModifierKeysState(), User, false, 0, 0));
	TestEqual(TEXT("releasing A clicks once"), Buttons.TopClicks, 1);
	TestEqual(TEXT("and only the gazed button"), Buttons.BottomClicks, 0);

	S.F.SetOverlayInput(false);
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixGazeOffPanelSpec, "WoT.VR.HeadsetFixes.GazeOffPanelUnhovers",
	WoTVRTest::Flags)
bool FWoTVRFixGazeOffPanelSpec::RunTest(const FString&)
{
	// Hover is per pointer, and the gaze has a pointer of its own: when the
	// gaze leaves the panel (past its edge, or turned away from it) the control
	// it was on is no longer hovered, and it is again when the gaze comes back.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	FTwoButtons Buttons;
	const TSharedRef<SWidget> Window = Buttons.Make();
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	S.F.Frames(2);
	const FVector Head = Scope.Session->Pose.PositionMeters;
	const FVector2D OnTop = Center(Buttons.Top);
	const FIntPoint Pixels = Presentation->GetMenuSurface()->GetPixels();

	for (const bool bPastEdge : {true, false})
	{
		const FString Label = bPastEdge ? TEXT("past the panel's edge") : TEXT("turned away");
		LookAtPixel(*Scope.Session, *Presentation, OnTop);
		S.F.Frames(1);
		TestTrue(Label + TEXT(": the gaze hovers the top button"), Presentation->GetPointer().IsSet()
			&& Buttons.Top->IsHovered());
		if (bPastEdge)
		{
			// Beyond the panel's left edge, level with the button: the ray still
			// meets the panel's plane.
			LookAtPixel(*Scope.Session, *Presentation, FVector2D(-0.1 * Pixels.X, OnTop.Y));
		}
		else
		{
			Scope.Session->SetHead(Head, FRotator(0.0, 170.0, 0.0));
		}
		S.F.Frames(1);
		TestFalse(Label + TEXT(": the gaze is off the panel"), Presentation->GetPointer().IsSet());
		TestFalse(Label + TEXT(": the button is no longer hovered"), Buttons.Top->IsHovered()
			|| Buttons.Bottom->IsHovered());
		S.F.Frames(1);
		TestFalse(Label + TEXT(": still, a frame later"), Buttons.Top->IsHovered());
		// Back onto the same spot of the same button.
		LookAtPixel(*Scope.Session, *Presentation, OnTop);
		S.F.Frames(1);
		TestTrue(Label + TEXT(": back on the panel, the button is hovered again"),
			Presentation->GetPointer().IsSet() && Buttons.Top->IsHovered());
	}
	TestEqual(TEXT("looking around clicks nothing"), Buttons.TopClicks + Buttons.BottomClicks, 0);

	S.F.SetOverlayInput(false);
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixReleaseOffPanelSpec, "WoT.VR.HeadsetFixes.ReleaseOffPanelNeverClicks",
	WoTVRTest::Flags)
bool FWoTVRFixReleaseOffPanelSpec::RunTest(const FString&)
{
	// A press the gaze takes off the panel before its release ends there: the
	// release hits nothing (not the control at the panel's nearest edge), so it
	// is never a click; the capture goes, and the pressed control's hover with it.
	using namespace WoTVRUITest;
	WoTVRTest::FScope Scope;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = S.User();
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset menus"), Presentation)) return false;
	// A window with a button against the panel's left edge.
	TSharedPtr<SButton> Edge;
	int32 Clicks = 0;
	const TSharedRef<SWidget> Window = SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Center)
		[
			SAssignNew(Edge, SButton).OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })
			[
				SNew(SBox).WidthOverride(400.0f).HeightOverride(140.0f)
			]
		];
	S.Viewport().AddViewportWidgetContent(Window, 2500);
	if (!TestTrue(TEXT("UI-only input"), S.F.SetOverlayInput(true))) return false;
	S.F.Frames(2);
	if (!TestTrue(TEXT("the window is on the headset surface"), OnMenuSurface(Presentation, Window))) return false;
	const FVector2D OnEdge = Center(Edge);
	const FIntPoint Pixels = Presentation->GetMenuSurface()->GetPixels();
	TestTrue(TEXT("the button reaches the panel's left edge"),
		Edge->GetCachedGeometry().GetAbsolutePosition().X <= 0.5f);
	const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(User);
	const FKeyEvent A(EKeys::Gamepad_FaceButton_Bottom, FModifierKeysState(), User, false, 0, 0);

	// A on the button, the gaze off the panel's left edge level with it, then
	// the release.
	LookAtPixel(*Scope.Session, *Presentation, OnEdge);
	S.F.Frames(1);
	TestTrue(TEXT("the gaze hovers the button"), Presentation->GetPointer().IsSet() && Edge->IsHovered());
	TestTrue(TEXT("A is taken"), Slate.ProcessKeyDownEvent(A));
	TestTrue(TEXT("the button is pressed on the gaze's pointer"), Edge->IsPressed() && SlateUser.IsValid()
		&& SlateUser->DoesWidgetHaveCapture(Edge, FWoTLocalPlayerHooks::UIPointerIndex));
	LookAtPixel(*Scope.Session, *Presentation, FVector2D(-0.1 * Pixels.X, OnEdge.Y));
	S.F.Frames(1);
	TestFalse(TEXT("the gaze is off the panel"), Presentation->GetPointer().IsSet());
	TestTrue(TEXT("while held, the press keeps its capture"), Edge->IsPressed() && Edge->HasMouseCapture());
	Slate.ProcessKeyUpEvent(A);
	TestEqual(TEXT("released off the panel: no click"), Clicks, 0);
	TestFalse(TEXT("no capture left"), Edge->HasMouseCapture() || (SlateUser.IsValid()
		&& SlateUser->HasCapture(FWoTLocalPlayerHooks::UIPointerIndex)));
	TestFalse(TEXT("the button is no longer pressed"), Edge->IsPressed());
	TestFalse(TEXT("nor hovered"), Edge->IsHovered());
	S.F.Frames(1);
	TestFalse(TEXT("nor a frame later"), Edge->IsHovered());

	// The pressed button and the gaze come back: a fresh press there clicks.
	LookAtPixel(*Scope.Session, *Presentation, OnEdge);
	S.F.Frames(1);
	TestTrue(TEXT("back on the button: hovered"), Edge->IsHovered());
	Press(User, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("a press and release on it clicks"), Clicks, 1);

	S.F.SetOverlayInput(false);
	S.Viewport().RemoveViewportWidgetContent(Window);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
