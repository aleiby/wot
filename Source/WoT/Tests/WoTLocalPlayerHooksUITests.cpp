#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTLocalPlayerHooksTestFixture.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTCharacter.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameViewportClient.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTPlayerController.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Online/SWoTSharingNotice.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../UI/SWoTOnScreenKeyboard.h"
#include "../UI/WoTControllerUIInput.h"
#include "../UI/WoTTextEntry.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Misc/ScopeExit.h"
#include "Rendering/DrawElements.h"
#include "Input/HittestGrid.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/SWindow.h"

// H6: a test UI host (an offscreen virtual window) takes the game's real
// screens off the real game viewport, in a real local player's world.
namespace WoTUIHostTest
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName HostOwner(TEXT("UIHostSpec"));

/** Leaves Pointer mode, no text entry and the native environment behind. */
struct FGuard
{
	TSharedRef<FWoTRecordingCursorPolicy> Policy = MakeShared<FWoTRecordingCursorPolicy>();
	FGuard()
	{
		FWoTControllerUIInput::ResetForTesting();
		FWoTControllerUIInput::SetCursorPolicyForTesting(Policy);
		FWoTTextEntry::ResetForTesting();
		WoTSetRunningUnderWineForTesting(false);
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
	}
	~FGuard()
	{
		FWoTTextEntry::ResetForTesting();
		WoTSetRunningUnderWineForTesting(TOptional<bool>());
		FWoTControllerUIInput::ResetForTesting();
	}
};

/** An offscreen surface: a virtual window holding the screens it takes. */
struct FHost
{
	TSharedRef<SVirtualWindow> Window = SNew(SVirtualWindow).Size(FVector2D(1024.0f, 768.0f));
	TSharedRef<SOverlay> Surface = SNew(SOverlay);
	TArray<TPair<TWeakPtr<SWidget>, EWoTUIContent>> Shown;
	int32 Hidden = 0;
	bool bDecline = false;

	FHost()
	{
		Window->SetContent(Surface);
		FSlateApplication::Get().RegisterVirtualWindow(Window);
	}
	~FHost()
	{
		FSlateApplication::Get().UnregisterVirtualWindow(Window);
	}

	FWoTLocalPlayerConsumer Consumer()
	{
		FWoTUIHost Host;
		Host.Show = [this](const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent Kind)
		{
			if (bDecline)
			{
				return false;
			}
			Surface->AddSlot(ZOrder)[Screen];
			Shown.Emplace(Screen, Kind);
			return true;
		};
		Host.Hide = [this](const TSharedRef<SWidget>& Screen)
		{
			Surface->RemoveSlot(Screen);
			++Hidden;
		};
		Host.Scope = [this]() -> TSharedPtr<SWidget> { return Window; };
		FWoTLocalPlayerConsumer Consumer;
		Consumer.UIHost = Host;
		return Consumer;
	}

	void Settle()
	{
		Window->SlatePrepass(1.0f);
	}

	/** The host draws its window: layout, then a paint, which fills the
	 * window's hit test as an offscreen renderer's draw would. */
	void Paint()
	{
		Window->SlatePrepass(1.0f);
		FSlateWindowElementList Elements(Window);
		Window->PaintWindow(FSlateApplication::Get().GetCurrentTime(), 0.0f, Elements,
			FWidgetStyle(), true);
	}

	bool Hosts(const TSharedPtr<SWidget>& Widget) const
	{
		return Widget.IsValid()
			&& FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) == Window;
	}

	/** Whether the host was shown this screen as this kind. */
	bool Named(const TSharedRef<SWidget>& Screen, EWoTUIContent Kind) const
	{
		for (const TPair<TWeakPtr<SWidget>, EWoTUIContent>& Entry : Shown)
		{
			if (Entry.Key.HasSameObject(&Screen.Get()))
			{
				return Entry.Value == Kind;
			}
		}
		return false;
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
			.ClientSize(FVector2D(1024.0f, 768.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[F.Widget.ToSharedRef()];
		FSlateApplication::Get().AddWindow(Window.ToSharedRef(), false);
		PreviousEngineViewport = GEngine->GameViewport;
		GEngine->GameViewport = F.Viewport;
		Settle();
		return Test.TestNotNull(TEXT("the game's own viewport client"),
			Cast<UWoTGameViewportClient>(F.Viewport));
	}

	~FScreens()
	{
		if (F.Player)
		{
			FWoTLocalPlayerHooks::DetachAll(*F.Player);
		}
		if (F.PC && F.PC->IsChatTyping())
		{
			F.PC->CancelChatInput();
		}
		GEngine->GameViewport = PreviousEngineViewport;
		if (Window.IsValid())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			FSlateApplication::Get().Tick();
		}
	}

	void Settle()
	{
		FSlateApplication::Get().Tick();
		Window->SlatePrepass(1.0f);
	}

	UWoTGameViewportClient& Viewport() const
	{
		return *CastChecked<UWoTGameViewportClient>(F.Viewport);
	}

	/** On the viewport's own overlay, in the game window. */
	bool OnViewport(const TSharedPtr<SWidget>& Screen) const
	{
		if (!Screen.IsValid())
		{
			return false;
		}
		const bool bListed = Viewport().GetOverlayContent().ContainsByPredicate(
			[&Screen](const TPair<TSharedRef<SWidget>, int32>& Entry)
			{
				return Entry.Key == Screen;
			});
		return bListed && FSlateApplication::Get().FindWidgetWindow(Screen.ToSharedRef()) == Window;
	}

	int32 User() const
	{
		return FWoTLocalPlayerHooks::UISlateUser(*F.Player);
	}
};

class FFeedback final : public IWoTFrontEndFeedbackSink
{
public:
	virtual bool IsReady() const override { return true; }
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

/** The front end's menus build their entries from these labels (the UI
 * specs' own set, placeholder text). */
TSharedRef<FWoTFrontEndPresentationAuthority> Authority()
{
	TMap<FName, FString> Labels;
	const TPair<const TCHAR*, int32> Entries[] = {
		{TEXT("menuMain.MenuList[2]"), 11}, {TEXT("menuMain.MenuList[3]"), 8},
		{TEXT("menuMain.MenuList[4]"), 8}, {TEXT("menuMain.MenuList[7]"), 4},
		{TEXT("menuMultiPlayer.HelpMessage[1]"), 20}, {TEXT("menuMultiPlayer.MenuList[2]"), 13},
		{TEXT("menuStartArenaServer.MenuTitle"), 12}, {TEXT("menuStartBattleServer.MenuTitle"), 14},
		{TEXT("menuStartArenaServer.MenuList[1]"), 10}, {TEXT("menuStartArenaServer.MenuList[2]"), 12},
		{TEXT("menuStartArenaServer.MenuList[3]"), 10}, {TEXT("menuStartArenaServer.MenuList[5]"), 11},
		{TEXT("menuStartArenaServer.MenuList[7]"), 11}, {TEXT("menuPlayer.MenuTitle"), 13},
		{TEXT("menuPlayer.MenuList[1]"), 5}, {TEXT("menuPlayer.MenuList[3]"), 9},
		{TEXT("menuOptions.MenuTitle"), 8}, {TEXT("menuOptions.MenuList[1]"), 18},
		{TEXT("menuOptions.MenuList[2]"), 12}, {TEXT("menuOptions.MenuList[3]"), 17},
		{TEXT("menuConfiguration.MenuTitle"), 8}};
	for (const TPair<const TCHAR*, int32>& Entry : Entries)
	{
		Labels.Add(FName(Entry.Key), FString::ChrN(Entry.Value, TEXT('M')));
	}
	return FWoTFrontEndPresentationAuthority::CreateForTesting(TEXT("ui-host"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
		MoveTemp(Labels), MakeShared<FFeedback>());
}

TSharedRef<FWoTRequiredMenuFonts> Fonts()
{
	static const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Required = MakeShared<FWoTRequiredMenuFonts>();
	Required->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Required->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Required->Generation = Generation;
	return Required;
}

FPointerEvent Press(int32 User, const FVector2D& Where, bool bDown)
{
	TSet<FKey> Pressed;
	if (bDown)
	{
		Pressed.Add(EKeys::LeftMouseButton);
	}
	return FPointerEvent(User, FSlateApplication::CursorPointerIndex, Where, Where,
		Pressed, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
}

/** A widget's centre in its window's space, as last painted. */
FVector2D Center(const TSharedPtr<SWidget>& Widget)
{
	return FVector2D(Widget->GetCachedGeometry().GetAbsolutePositionAtCoordinates(
		FVector2f(0.5f, 0.5f)));
}

/** The host's pointer clicks a widget it shows, where the host drew it. */
bool Click(FScreens& S, FHost& H, const TSharedPtr<SWidget>& Widget)
{
	H.Paint();
	if (!Widget.IsValid())
	{
		return false;
	}
	const FVector2D Where = Center(Widget);
	const bool bDown = FWoTLocalPlayerHooks::RoutePointer(*S.F.Player, HostOwner, Where,
		EWoTUIPointerAction::Down);
	const bool bUp = FWoTLocalPlayerHooks::RoutePointer(*S.F.Player, HostOwner, Where,
		EWoTUIPointerAction::Up);
	return bDown && bUp;
}

/** The front end builds its screens as it ticks (as the UI specs drive it). */
void SettleFrontEnd(const TSharedRef<SWoTFrontEndRoot>& Root, FHost& H)
{
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		Root->Tick(Pass == 0 ? FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform())
			: Root->GetCachedGeometry(), 0.0, 0.0f);
		H.Paint();
	}
}

bool FocusInside(int32 User, const TSharedPtr<SWidget>& Field)
{
	for (TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(User);
		Focused.IsValid(); Focused = Focused->GetParentWidget())
	{
		if (Focused == Field)
		{
			return true;
		}
	}
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookUIHostScreensSpec,
	"WoT.VR.Hooks.H6.HostsEveryScreen", WoTUIHostTest::Flags)

bool FWoTHookUIHostScreensSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	using namespace WoTUIHostTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	// Hosting needs neither a pawn nor a HUD: this player has neither.
	S.F.PC->UnPossess();
	UWoTGameViewportClient& Viewport = S.Viewport();

	// The real screens, built as the game builds them.
	const TSharedRef<SWoTFrontEndRoot> FrontEnd = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority());
	const TSharedRef<SWoTInGameMenuOverlay> Menu = SNew(SWoTInGameMenuOverlay)
		.PlayerController(S.F.PC)
		.RequiredFontsForTesting(Fonts());
	AWoTAngrealInvFireball* Fireball = S.F.World->SpawnActor<AWoTAngrealInvFireball>();
	if (!TestNotNull(TEXT("an artifact to describe"), Fireball)) return false;
	const TSharedRef<SWoTItemInfoOverlay> ItemInfo = SNew(SWoTItemInfoOverlay)
		.PlayerController(S.F.PC)
		.Item(Fireball)
		.RequiredFontsForTesting(Fonts());
	const TSharedRef<SWoTSharingNotice> Notice = SNew(SWoTSharingNotice);

	// No host: every screen goes onto the viewport as it always has.
	Viewport.AddViewportWidgetContent(FrontEnd, 1000);
	Viewport.AddViewportWidgetContent(Menu, 2000);
	S.Settle();
	TestTrue(TEXT("without a host the front end is on the viewport"), S.OnViewport(FrontEnd));
	TestTrue(TEXT("without a host the menu is on the viewport"), S.OnViewport(Menu));

	// Attached in a menu: the open screens move to the host, bottom first.
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	H.Settle();
	TestTrue(TEXT("the front end moves to the host"), H.Hosts(FrontEnd) && !S.OnViewport(FrontEnd));
	TestTrue(TEXT("the in-game menu moves to the host"), H.Hosts(Menu) && !S.OnViewport(Menu));
	TestTrue(TEXT("the front end is named"), H.Named(FrontEnd, EWoTUIContent::FrontEnd));
	TestTrue(TEXT("the menu is named"), H.Named(Menu, EWoTUIContent::InGameMenu));
	TestTrue(TEXT("bottom first"), H.Shown.Num() == 2 && H.Shown[0].Key.HasSameObject(&FrontEnd.Get()));
	AddExpectedError(TEXT("exclusive local-player slot"), EAutomationExpectedErrorFlags::Contains, 1);
	FHost Second;
	TestFalse(TEXT("a second host is rejected"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("SecondHost"), Second.Consumer()));

	// The front end's own screens live inside it: the server browser and the
	// class and team choice are hosted with it.
	SettleFrontEnd(FrontEnd, H);
	FrontEnd->ActivateMainMenuEntryForTesting(0);
	SettleFrontEnd(FrontEnd, H);
	TestTrue(TEXT("the browser's address field is hosted"), H.Hosts(FrontEnd->DirectAddressTextBoxForTesting()));
	FrontEnd->OpenPlayerConfigForTesting();
	SettleFrontEnd(FrontEnd, H);
	TestTrue(TEXT("the class and team choice is hosted"), H.Hosts(FrontEnd->PlayerNameTextBoxForTesting()));

	// Screens that open while hosted go straight to the host.
	Viewport.AddViewportWidgetContent(ItemInfo, 1900);
	Viewport.AddViewportWidgetContent(Notice, 2300);
	TestTrue(TEXT("item information opens in the host"), H.Hosts(ItemInfo) && !S.OnViewport(ItemInfo));
	TestTrue(TEXT("item information is named"), H.Named(ItemInfo, EWoTUIContent::ItemInfo));
	TestTrue(TEXT("a notice is named"), H.Named(Notice, EWoTUIContent::Notice));

	// Chat: the controller's own chat line, through the same host.
	S.F.PC->EnableChatWidgetForTesting();
	S.F.PC->ChatByController(/*bTeam*/ false);
	S.Settle();
	H.Settle();
	const TSharedPtr<SWoTChatInputOverlay> Chat = S.F.PC->GetChatInputWidget();
	if (TestTrue(TEXT("controller chat opens"), Chat.IsValid()))
	{
		TestTrue(TEXT("the chat line opens in the host"), H.Hosts(Chat) && !S.OnViewport(Chat));
		TestTrue(TEXT("chat is named"), H.Named(Chat.ToSharedRef(), EWoTUIContent::Chat));
		// Its keyboard panel: no runtime entry here, so the game's own panel.
		const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
		TestTrue(TEXT("controller chat opens the keyboard panel"),
			Panel.IsValid() && FWoTTextEntry::GetActiveRoute() == EWoTTextEntryRoute::Panel);
		TestTrue(TEXT("the keyboard panel is hosted"), H.Hosts(Panel));
		const int32 HiddenBefore = H.Hidden;
		FWoTTextEntry::Cancel();
		S.F.PC->CancelChatInput();
		TestEqual(TEXT("closing chat lets the host let it go"), H.Hidden, HiddenBefore + 1);
		TestFalse(TEXT("closed chat is in neither place"), H.Hosts(Chat) || S.OnViewport(Chat));
	}

	// Closing a hosted screen lets it go.
	const int32 HiddenBefore = H.Hidden;
	Viewport.RemoveViewportWidgetContent(Menu);
	TestEqual(TEXT("a closed screen is let go"), H.Hidden, HiddenBefore + 1);
	TestFalse(TEXT("a closed screen is in neither place"), H.Hosts(Menu) || S.OnViewport(Menu));

	// Detaching returns every screen to the viewport at its z-order.
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	S.Settle();
	TestTrue(TEXT("the front end returns"), S.OnViewport(FrontEnd) && !H.Hosts(FrontEnd));
	TestTrue(TEXT("item information returns"), S.OnViewport(ItemInfo) && !H.Hosts(ItemInfo));
	TestTrue(TEXT("the notice returns"), S.OnViewport(Notice) && !H.Hosts(Notice));
	const TArray<TPair<TSharedRef<SWidget>, int32>> Back = Viewport.GetOverlayContent();
	TestTrue(TEXT("each at its own z-order"), Back.Num() == 3
		&& &Back[0].Key.Get() == &FrontEnd.Get() && Back[0].Value == 1000
		&& &Back[1].Key.Get() == &ItemInfo.Get() && Back[1].Value == 1900
		&& &Back[2].Key.Get() == &Notice.Get() && Back[2].Value == 2300);

	// A host may leave a screen on the viewport.
	H.bDecline = true;
	TestTrue(TEXT("a declining host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()));
	Viewport.AddViewportWidgetContent(Menu, 2000);
	TestTrue(TEXT("a declined screen stays on the viewport"), S.OnViewport(Menu) && S.OnViewport(FrontEnd));
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	Viewport.RemoveViewportWidgetContent(Menu);
	Viewport.RemoveViewportWidgetContent(ItemInfo);
	Viewport.RemoveViewportWidgetContent(Notice);
	Viewport.RemoveViewportWidgetContent(FrontEnd);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookUIHostFocusScopeSpec,
	"WoT.VR.Hooks.H6.FocusScope", WoTUIHostTest::Flags)

bool FWoTHookUIHostFocusScopeSpec::RunTest(const FString&)
{
	using namespace WoTUIHostTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<SWoTInGameMenuOverlay> Menu = SNew(SWoTInGameMenuOverlay)
		.PlayerController(S.F.PC)
		.RequiredFontsForTesting(Fonts());
	// A button outside every scope, in a window of its own.
	TSharedPtr<SButton> Elsewhere;
	const TSharedRef<SWindow> Other = SNew(SWindow).ClientSize(FVector2D(200.0f, 100.0f))
		.CreateTitleBar(false)[SAssignNew(Elsewhere, SButton)];
	Slate.AddWindow(Other, false);
	ON_SCOPE_EXIT
	{
		Slate.RequestDestroyWindow(Other);
		Slate.Tick();
	};
	// A button in the host's surface that no screen owns.
	TSharedPtr<SButton> Spare;
	H.Surface->AddSlot(0)[SAssignNew(Spare, SButton)];

	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	S.Viewport().AddViewportWidgetContent(Menu, 2000);
	H.Settle();
	auto GamepadKey = [&Slate, &S]()
	{
		const FKeyEvent Event(EKeys::Gamepad_DPad_Down, FModifierKeysState(), S.User(), false, 0, 0);
		Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
	};

	// The hosted surface is in the controller UI input scope.
	Slate.SetUserFocus(S.User(), Menu->InitialFocusWidget(), EFocusCause::SetDirectly);
	TestTrue(TEXT("focus is in the hosted menu"), FocusInside(S.User(), Menu));
	TestTrue(TEXT("the host's D-pad reaches the hosted menu"), FWoTLocalPlayerHooks::RouteKey(
		*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, true));
	FWoTLocalPlayerHooks::RouteKey(*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, false);
	TestTrue(TEXT("a controller in the hosted surface is controller input"),
		FWoTControllerUIInput::GetLastInput() == EWoTLastInput::Controller);

	// Focus anywhere else is still out of scope.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	Slate.SetUserFocus(S.User(), Elsewhere, EFocusCause::SetDirectly);
	TestFalse(TEXT("the host's keys never reach focus outside it"), FWoTLocalPlayerHooks::RouteKey(
		*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, true));
	GamepadKey();
	TestTrue(TEXT("out-of-scope focus stays out of scope"),
		FWoTControllerUIInput::GetLastInput() == EWoTLastInput::Pointer);
	TestFalse(TEXT("only the owner routes"), FWoTLocalPlayerHooks::RouteKey(
		*S.F.Player, TEXT("NotTheHost"), EKeys::Gamepad_DPad_Down, true));

	// Detached, the host's surface leaves the scope.
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	Slate.SetUserFocus(S.User(), Spare, EFocusCause::SetDirectly);
	GamepadKey();
	TestTrue(TEXT("a detached host's surface is out of scope"),
		FWoTControllerUIInput::GetLastInput() == EWoTLastInput::Pointer);
	S.Viewport().RemoveViewportWidgetContent(Menu);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookUIHostTextEntrySpec,
	"WoT.VR.Hooks.H6.PointerAndTextEntry", WoTUIHostTest::Flags)

bool FWoTHookUIHostTextEntrySpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	using namespace WoTUIHostTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<SWoTFrontEndRoot> FrontEnd = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority());
	if (!TestTrue(TEXT("the host attaches in the front end"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	S.Viewport().AddViewportWidgetContent(FrontEnd, 1000);
	SettleFrontEnd(FrontEnd, H);

	// A password stays masked (the host game's password on the Play screen),
	// entered by the host's confirm on the focused field, as the same user.
	FrontEnd->ActivateMainMenuEntryForTesting(0);
	SettleFrontEnd(FrontEnd, H);
	const TSharedPtr<SWidget> Password = FrontEnd->GamePasswordTextBoxForTesting();
	if (!TestTrue(TEXT("the password field is hosted"), H.Hosts(Password))) return false;
	Slate.SetUserFocus(S.User(), Password, EFocusCause::SetDirectly);
	TestTrue(TEXT("the host's confirm reaches the field"), FWoTLocalPlayerHooks::RouteKey(
		*S.F.Player, HostOwner, EKeys::Gamepad_FaceButton_Bottom, true));
	FWoTLocalPlayerHooks::RouteKey(*S.F.Player, HostOwner, EKeys::Gamepad_FaceButton_Bottom, false);
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (TestTrue(TEXT("the password opens the panel"), Panel.IsValid()))
	{
		TestTrue(TEXT("masked"), Panel->IsPassword());
		Panel->SetDraft(TEXT("Secret1"));
		TestFalse(TEXT("the preview hides it"), Panel->GetPreviewText().Contains(TEXT("Secret")));
		FWoTTextEntry::Cancel();
	}

	// The class and team choice: the player name.
	FrontEnd->OpenPlayerConfigForTesting();
	SettleFrontEnd(FrontEnd, H);
	const TSharedPtr<SWidget> Name = FrontEnd->PlayerNameTextBoxForTesting();
	if (!TestTrue(TEXT("the name field is hosted"), H.Hosts(Name))) return false;

	// An ordinary click (Slate's own route, the same hit) only places the
	// caret, as it always has.
	H.Paint();
	const FVector2D NameCenter = Center(Name);
	TArray<FWidgetAndPointer> Bubble =
		H.Window->GetHittestGrid().GetBubblePath(NameCenter, 0.0f, false, S.User());
	const FWidgetPath Path(Bubble);
	TestTrue(TEXT("the field is under its centre"), Path.ContainsWidget(Name.Get()));
	Slate.RoutePointerDownEvent(Path, Press(S.User(), NameCenter, true));
	Slate.RoutePointerUpEvent(Path, Press(S.User(), NameCenter, false));
	TestFalse(TEXT("a click does not start entry"), FWoTTextEntry::IsActive());

	// The host's pointer starts the controller entry on the game's panel.
	TestTrue(TEXT("the host's click is routed"), Click(S, H, Name));
	Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("the host's click starts entry on the panel"), FWoTTextEntry::IsActive()
		&& Panel.IsValid() && FWoTTextEntry::GetActiveRoute() == EWoTTextEntryRoute::Panel)) return false;
	TestTrue(TEXT("the panel is hosted"), H.Hosts(Panel));
	TestTrue(TEXT("the panel has the player's focus"), FocusInside(S.User(), Panel));
	// The field's limit and filter hold.
	TestEqual(TEXT("the name takes 20"), Panel->GetMaxLength(), 20);
	Panel->SetDraft(FString());
	for (const TCHAR Character : FString(TEXT("Sam?#=Brightwater of Lakeside")))
	{
		Panel->TypeCharacter(Character);
	}
	TestEqual(TEXT("the name filter and limit hold"), Panel->GetDraft(), FString(TEXT("SamBrightwater of La")));
	// Done commits and gives the field the player's focus back.
	TestTrue(TEXT("Done"), FWoTTextEntry::Done());
	TestFalse(TEXT("Done ends the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("Done commits the name"), FrontEnd->ViewModelForTesting().GetPendingPlayerName(),
		FString(TEXT("SamBrightwater of La")));
	TestTrue(TEXT("Done restores the field's focus"), FocusInside(S.User(), Name));
	// Cancel keeps the field and gives it focus back.
	TestTrue(TEXT("the host's click again"), Click(S, H, Name));
	Panel = FWoTTextEntry::GetPanel();
	if (TestTrue(TEXT("entry again"), Panel.IsValid()))
	{
		Panel->SetDraft(TEXT("Other"));
		FWoTTextEntry::Cancel();
		TestEqual(TEXT("Cancel keeps the name"), FrontEnd->ViewModelForTesting().GetPendingPlayerName(),
			FString(TEXT("SamBrightwater of La")));
		TestTrue(TEXT("Cancel restores the field's focus"), FocusInside(S.User(), Name));
	}

	// A host pointer has no authority outside the host: it hits only the
	// host's window, and only the host routes it.
	TestFalse(TEXT("only the host routes its pointer"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, TEXT("NotTheHost"), NameCenter, EWoTUIPointerAction::Down));
	TestFalse(TEXT("the host's pointer never focused the game viewport"),
		FocusInside(S.User(), S.F.Widget));
	TestFalse(TEXT("routing ended with the click"), FWoTLocalPlayerHooks::IsRoutingUIPointer(S.User()));
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	TestFalse(TEXT("without a host the panel is no longer forced"), FWoTLocalPlayerHooks::UsesPanelTextEntry());
	S.Viewport().RemoveViewportWidgetContent(FrontEnd);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookUIHostModalSpec,
	"WoT.VR.Hooks.H6.ModalReleasesOnce", WoTUIHostTest::Flags)

bool FWoTHookUIHostModalSpec::RunTest(const FString&)
{
	using namespace WoTUIHostTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	AWoTAngrealInvDart* Dart = S.F.World->SpawnActor<AWoTAngrealInvDart>();
	if (!TestNotNull(TEXT("a repeating artifact"), Dart)) return false;
	Dart->GiveTo(S.F.Pawn);
	Dart->CurCharges = 200;
	S.F.Pawn->InventoryComponent->SelectItem(Dart);
	auto Darts = [&S]()
	{
		int32 Count = 0;
		for (TActorIterator<AWoTDartProjectile> It(S.F.World); It; ++It)
		{
			++Count;
		}
		return Count;
	};

	int32 Releases = 0;
	FWoTLocalPlayerConsumer Commands;
	Commands.Lifecycle = [&Releases](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext&)
	{
		Releases += Event == EWoTLocalPlayerEvent::InputContextChanged ? 1 : 0;
	};
	if (!TestTrue(TEXT("the command consumer attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("Commands"), Commands))) return false;
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	FWoTLocalPlayerHooks::Command(*S.F.Player, TEXT("Commands"), EWoTLocalPlayerCommand::Fire);
	S.F.Frames(30);
	const int32 HeldDarts = Darts();
	TestTrue(TEXT("a held cast repeats"), HeldDarts > 1);

	// A passive screen in the host takes no input: the cast continues.
	const TSharedRef<SWoTSharingNotice> Notice = SNew(SWoTSharingNotice);
	S.Viewport().AddViewportWidgetContent(Notice, 2300);
	S.F.Frames(30);
	TestTrue(TEXT("the passive screen is hosted"), H.Hosts(Notice));
	TestFalse(TEXT("a passive screen leaves gameplay input alone"),
		FWoTLocalPlayerHooks::IsGameInputBlocked(S.F.PC));
	TestEqual(TEXT("a passive screen releases nothing"), Releases, 0);
	TestTrue(TEXT("the held cast continues behind it"), Darts() > HeldDarts);

	// A modal window (an optional module's, through the overlay seam) in the
	// host releases the held cast exactly once; play continues.
	const TSharedRef<SWoTInGameMenuOverlay> Window = SNew(SWoTInGameMenuOverlay)
		.PlayerController(S.F.PC)
		.RequiredFontsForTesting(Fonts());
	S.Viewport().AddViewportWidgetContent(Window, 1950);
	const float Before = S.F.World->GetTimeSeconds();
	if (!TestTrue(TEXT("the overlay seam is installed"), S.F.SetOverlayInput(true))) return false;
	TestEqual(TEXT("opening the modal releases once"), Releases, 1);
	TestTrue(TEXT("the modal holds UI input"), FWoTLocalPlayerHooks::IsGameInputBlocked(S.F.PC));
	const int32 Released = Darts();
	S.F.Frames(60);
	TestEqual(TEXT("the released cast stops"), Darts(), Released);
	TestEqual(TEXT("still released once"), Releases, 1);
	TestTrue(TEXT("network play continues"), S.F.World->GetTimeSeconds() > Before && !S.F.World->IsPaused());
	S.F.SetOverlayInput(false);
	TestEqual(TEXT("closing it releases nothing more"), Releases, 1);
	FWoTLocalPlayerHooks::Command(*S.F.Player, TEXT("Commands"), EWoTLocalPlayerCommand::CeaseFire);
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	S.Viewport().RemoveViewportWidgetContent(Window);
	S.Viewport().RemoveViewportWidgetContent(Notice);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
