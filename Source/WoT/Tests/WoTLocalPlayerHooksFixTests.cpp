#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTHUDStreamFixture.h"
#include "WoTLocalPlayerHooksTestFixture.h"
#include "WoTUIScreensTestFixture.h"
#include "../WoTCharacter.h"
#include "../WoTGameViewportClient.h"
#include "../WoTHUD.h"
#include "../WoTInGameSlate.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTMenuStyle.h"
#include "../WoTPlayerController.h"
#include "../UI/WoTControllerUIInput.h"
#include "../UI/WoTTextEntry.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/LocalPlayer.h"
#include "Engine/UserInterfaceSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Rendering/DrawElements.h"
#include "Styling/WidgetStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Input/HittestGrid.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/SWindow.h"

#include <type_traits>

// The four host-side points of the H5/H6 review, and the host's screen size:
// a detaching host leaves nothing held; a host that detaches inside its own
// Show strands no screen; the snapshot's status timer is the flat HUD's
// 19-step frame; a draw command's run-time face is a weak reference; and a
// host can give the screens that open its own surface's size.
namespace WoTHookFixTest
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName HostOwner(TEXT("FixHost"));

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
	TFunction<void(const TSharedRef<SWidget>&)> DuringShow;
	FIntPoint Size = FIntPoint::ZeroValue;
	int32 Shown = 0;
	int32 Hidden = 0;

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
		Host.Show = [this](const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent)
		{
			Surface->AddSlot(ZOrder)[Screen];
			++Shown;
			if (DuringShow)
			{
				DuringShow(Screen);
			}
			return true;
		};
		Host.Hide = [this](const TSharedRef<SWidget>& Screen)
		{
			Surface->RemoveSlot(Screen);
			++Hidden;
		};
		Host.Scope = [this]() -> TSharedPtr<SWidget> { return Window; };
		if (Size.X > 0)
		{
			Host.ScreenSize = [this]() { return Size; };
		}
		FWoTLocalPlayerConsumer Consumer;
		Consumer.UIHost = Host;
		return Consumer;
	}

	/** Lays out and paints the window, which fills its hit test. */
	void Paint()
	{
		Window->SlatePrepass(1.0f);
		FSlateWindowElementList Elements(Window);
		Window->PaintWindow(FSlateApplication::Get().GetCurrentTime(), 0.0f, Elements,
			FWidgetStyle(), true);
	}

	bool Hosts(const TSharedRef<SWidget>& Widget) const
	{
		return FSlateApplication::Get().FindWidgetWindow(Widget) == Window;
	}
};

/** The real game viewport in a real window. */
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
		return Test.TestNotNull(TEXT("the game's own viewport client"),
			Cast<UWoTGameViewportClient>(F.Viewport));
	}

	~FScreens()
	{
		if (F.Player)
		{
			FWoTLocalPlayerHooks::DetachAll(*F.Player);
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

	bool OnViewport(const TSharedRef<SWidget>& Screen) const
	{
		return Viewport().GetOverlayContent().ContainsByPredicate(
			[&Screen](const TPair<TSharedRef<SWidget>, int32>& Entry)
			{
				return Entry.Key == Screen;
			});
	}

	int32 User() const
	{
		return FWoTLocalPlayerHooks::UISlateUser(*F.Player);
	}
};

FVector2D Center(const TSharedRef<SWidget>& Widget)
{
	return FVector2D(Widget->GetCachedGeometry().GetAbsolutePositionAtCoordinates(
		FVector2f(0.5f, 0.5f)));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixReleaseOnDetachSpec,
	"WoT.VR.Hooks.Fixes.ReleaseOnDetach", WoTHookFixTest::Flags)

bool FWoTHookFixReleaseOnDetachSpec::RunTest(const FString&)
{
	using namespace WoTHookFixTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	// The game instance's own registration of the game window, as a running
	// game holds it: the controller UI input outlives the host's.
	const TSharedPtr<FWoTControllerUIInputRegistration> Game = FWoTControllerUIInput::Register(
		[&S]() -> TSharedPtr<SWidget> { return S.Window; });
	if (!TestTrue(TEXT("the game's scope registers"), Game.IsValid())) return false;
	const TSharedRef<SWidget> Menu = WoTUIScreensTest::MakeInGameMenu(*S.F.PC);
	TSharedPtr<SButton> Held;
	int32 Clicks = 0;
	H.Surface->AddSlot(5000)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	[
		SAssignNew(Held, SButton)
		.OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })
		[
			SNew(SBox).WidthOverride(120.0f).HeightOverride(60.0f)
		]
	];
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	S.Viewport().AddViewportWidgetContent(Menu, 2000);
	H.Paint();
	Slate.SetUserFocus(S.User(), WoTUIScreensTest::InGameMenuFocus(Menu), EFocusCause::SetDirectly);
	const TSharedPtr<FWoTControllerUIInput> Router = FWoTControllerUIInput::GetRegistered();
	if (!TestTrue(TEXT("the controller UI input runs"), Router.IsValid())) return false;

	// Held through the host's routes: the D-pad (its navigation repeat armed),
	// the stick past the threshold, and the pointer pressed on a button that
	// captures it.
	TestTrue(TEXT("the D-pad press is routed"), FWoTLocalPlayerHooks::RouteKey(
		*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, true));
	TestTrue(TEXT("the held D-pad arms the navigation repeat"), Router->IsRepeatArmed(S.User()));
	TestTrue(TEXT("the stick is routed"), FWoTLocalPlayerHooks::RouteAnalog(
		*S.F.Player, HostOwner, EKeys::Gamepad_LeftY, -0.9f));
	H.Paint();
	TestTrue(TEXT("the pointer presses the button"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Held.ToSharedRef()), EWoTUIPointerAction::Down));
	TestTrue(TEXT("the pressed button captures the pointer"), Held->HasMouseCapture());
	TestTrue(TEXT("the button is pressed"), Held->IsPressed());

	// Detaching lets go of all of it before the screens leave.
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	TestTrue(TEXT("the menu returns to the viewport"), S.OnViewport(Menu));
	TestFalse(TEXT("no navigation repeat stays armed in flat"), Router->IsRepeatArmed(S.User()));
	TestFalse(TEXT("no pointer capture stays"), Held->HasMouseCapture()
		|| Slate.HasUserMouseCapture(S.User()));
	TestFalse(TEXT("the button is no longer pressed"), Held->IsPressed());
	const EUINavigation Down = Router->GetNavigationConfig()->DirectionForAnalog(EKeys::Gamepad_LeftY, -0.9f);
	TestTrue(TEXT("the stick came back to rest: pushing it again is a fresh crossing"),
		Down != EUINavigation::Invalid
		&& Router->GetNavigationConfig()->ObserveAnalog(S.User(), EKeys::Gamepad_LeftY, -0.9f) == Down);
	Router->GetNavigationConfig()->ObserveAnalog(S.User(), EKeys::Gamepad_LeftY, 0.0f);
	TestEqual(TEXT("letting go of the pointer is not a click"), Clicks, 0);

	// A host that let everything go itself leaves nothing for the detach.
	if (TestTrue(TEXT("the host attaches again"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer())))
	{
		H.Paint();
		Slate.SetUserFocus(S.User(), WoTUIScreensTest::InGameMenuFocus(Menu), EFocusCause::SetDirectly);
		FWoTLocalPlayerHooks::RouteKey(*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, true);
		FWoTLocalPlayerHooks::RouteKey(*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, false);
		TestFalse(TEXT("a released key disarms the repeat"), Router->IsRepeatArmed(S.User()));
		FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
		TestFalse(TEXT("still nothing armed after the detach"), Router->IsRepeatArmed(S.User()));
	}

	// Detached from inside Slate's input handling, as a shortcut key's input
	// processor switches modes: the release follows once that input is done,
	// never nested in it.
	struct FDetachOnKey final : public IInputProcessor
	{
		ULocalPlayer* Player = nullptr;
		int32 Detaches = 0;
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
		{
			if (Event.GetKey() != EKeys::F9 || !Player)
			{
				return false;
			}
			++Detaches;
			FWoTLocalPlayerHooks::Detach(*Player, HostOwner);
			return true;
		}
	};
	const TSharedRef<FDetachOnKey> Shortcut = MakeShared<FDetachOnKey>();
	Shortcut->Player = S.F.Player;
	if (TestTrue(TEXT("the host attaches a third time"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer())))
	{
		Slate.RegisterInputPreProcessor(Shortcut);
		H.Paint();
		Slate.SetUserFocus(S.User(), WoTUIScreensTest::InGameMenuFocus(Menu), EFocusCause::SetDirectly);
		FWoTLocalPlayerHooks::RouteKey(*S.F.Player, HostOwner, EKeys::Gamepad_DPad_Down, true);
		TestTrue(TEXT("held: the repeat is armed"), Router->IsRepeatArmed(S.User()));
		TestTrue(TEXT("the shortcut takes its key"), Slate.ProcessKeyDownEvent(
			FKeyEvent(EKeys::F9, FModifierKeysState(), S.User(), false, 0, 0)));
		TestEqual(TEXT("the shortcut detached the host"), Shortcut->Detaches, 1);
		TestTrue(TEXT("the menu is back on the viewport"), S.OnViewport(Menu));
		TestTrue(TEXT("not nested inside the shortcut's input: still held"), Router->IsRepeatArmed(S.User()));
		Slate.Tick();
		TestFalse(TEXT("once the input is done the held key is released"), Router->IsRepeatArmed(S.User()));
		Slate.UnregisterInputPreProcessor(Shortcut);
	}
	S.Viewport().RemoveViewportWidgetContent(Menu);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixShowRechecksSpec,
	"WoT.VR.Hooks.Fixes.ShowRechecksHost", WoTHookFixTest::Flags)

bool FWoTHookFixShowRechecksSpec::RunTest(const FString&)
{
	using namespace WoTHookFixTest;
	FGuard Guard;
	FHost H;
	FHost Second;
	FScreens S;
	if (!S.Init(*this)) return false;
	UWoTGameViewportClient& Viewport = S.Viewport();
	const TSharedRef<SWidget> Notice = WoTUIScreensTest::MakeNotice();
	const TSharedRef<SWidget> Menu = WoTUIScreensTest::MakeInGameMenu(*S.F.PC);
	const TSharedRef<SWidget> ItemInfo = WoTUIScreensTest::MakeItemInfo(*S.F.PC);

	// A host that detaches inside its own Show: the screen it was shown lands
	// on the viewport, and the host was told to let it go.
	H.DuringShow = [&S](const TSharedRef<SWidget>&)
	{
		FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	};
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	Viewport.AddViewportWidgetContent(Notice, 2300);
	TestTrue(TEXT("the host detached during its Show"), !FWoTLocalPlayerHooks::HasConsumers(S.F.Player));
	TestTrue(TEXT("the screen is not stranded in the detached host"), !H.Hosts(Notice));
	TestTrue(TEXT("the detached host let it go"), H.Hidden == 1);
	TestTrue(TEXT("the screen is on the viewport"), S.OnViewport(Notice));
	Viewport.RemoveViewportWidgetContent(Notice);
	TestFalse(TEXT("and closes from there"), S.OnViewport(Notice));

	// A host that hands over to another inside its Show: the new host is
	// offered the screen.
	H.Shown = 0;
	H.Hidden = 0;
	H.DuringShow = [&S, &Second](const TSharedRef<SWidget>&)
	{
		FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
		FWoTLocalPlayerHooks::Attach(*S.F.Player, TEXT("SecondHost"), Second.Consumer());
	};
	if (!TestTrue(TEXT("the host attaches again"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	Viewport.AddViewportWidgetContent(Menu, 2000);
	TestTrue(TEXT("the first host let it go"), !H.Hosts(Menu) && H.Hidden == 1);
	TestTrue(TEXT("the host that took over has it"), Second.Hosts(Menu) && !S.OnViewport(Menu));
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("SecondHost"));
	TestTrue(TEXT("its detach returns it to the viewport"), S.OnViewport(Menu) && !Second.Hosts(Menu));

	// Attached over open screens: a host that detaches while taking the first
	// leaves every screen on the viewport.
	Viewport.AddViewportWidgetContent(ItemInfo, 1900);
	H.Shown = 0;
	H.Hidden = 0;
	H.DuringShow = [&S](const TSharedRef<SWidget>&)
	{
		FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	};
	FWoTLocalPlayerHooks::Attach(*S.F.Player, HostOwner, H.Consumer());
	TestEqual(TEXT("it was shown one screen"), H.Shown, 1);
	TestTrue(TEXT("both screens stay on the viewport"), S.OnViewport(ItemInfo) && S.OnViewport(Menu));
	TestTrue(TEXT("neither is stranded in the host"), !H.Hosts(ItemInfo) && !H.Hosts(Menu));
	Viewport.RemoveViewportWidgetContent(Menu);
	Viewport.RemoveViewportWidgetContent(ItemInfo);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixStatusTimerSpec,
	"WoT.VR.Hooks.Fixes.StatusTimerStep", WoTHookFixTest::Flags)

bool FWoTHookFixStatusTimerSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	FScene S;
	if (!S.Init(*this)) return false;
	// Two effects with known timers: 15 of 20 s left (step 4) and 2.5 of 10 s
	// (step 13), then less each frame. The scene's effects have no source, so
	// a tick reads them as ended: each frame's timers are set after its tick,
	// and the HUD updates in a new local-player frame of its own.
	const float Remaining[2] = {15.0f, 2.5f};
	const float Initial[2] = {20.0f, 10.0f};
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		S.Wait(1);
		if (!TestEqual(TEXT("the scene's two effects"), S.Body->StatusIcons.Num(), 2)) return false;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			S.Body->StatusIcons[Index].InitialDuration = Initial[Index];
			S.Body->StatusIcons[Index].RemainingDuration = FMath::Max(0.0f, Remaining[Index] - 1.3f * Frame);
		}
		++GFrameCounter;
		WoTHUDHookTest::DrawHUDOnce(*S.HUD, 640, 480);
		const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
		const FWoTHUDSection* Icons = Shown.FindSection(EWoTHUDSection::StatusIcons);
		if (!TestNotNull(TEXT("the status icons show"), Icons)
			|| !TestEqual(TEXT("both effects"), Shown.Status.Num(), 2)) return false;
		// The flat HUD draws each effect's icon then its timer frame.
		TArray<FName> Frames;
		for (const FWoTHUDDrawCmd& Command : Icons->Commands)
		{
			if (Command.TexId.ToString().StartsWith(TEXT("T32_")))
			{
				Frames.Add(Command.TexId);
			}
		}
		if (!TestEqual(TEXT("one timer frame per effect"), Frames.Num(), 2)) return false;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const FWoTHUDStatus& Status = Shown.Status[Index];
			TestTrue(TEXT("a step of the 19"), Status.TimerStep >= 0
				&& Status.TimerStep < FWoTHUDStatus::TimerSteps);
			TestEqual(TEXT("the snapshot's step is the frame the flat HUD draws"),
				FName(*FString::Printf(TEXT("T32_%02d"), Status.TimerStep + 1)), Frames[Index]);
		}
		if (Frame == 0)
		{
			TestEqual(TEXT("15 of 20 s left: step 4"), Shown.Status[0].TimerStep, 4);
			TestEqual(TEXT("2.5 of 10 s left: step 13"), Shown.Status[1].TimerStep, 13);
		}
		else if (Frame == 2)
		{
			// 12.4 of 20 s: 18 x 0.38 = 6.84, step 6; none of 10 s: the last frame.
			TestEqual(TEXT("12.4 of 20 s left: step 6"), Shown.Status[0].TimerStep, 6);
			TestEqual(TEXT("ended: the last of the 19 frames"), Shown.Status[1].TimerStep, 18);
		}
	}
	static_assert(sizeof(FWoTHUDStatus) <= sizeof(FName) + 2 * sizeof(int32),
		"the snapshot's status carries the icon, its side and the step, not durations");
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixCommandFontSpec,
	"WoT.VR.Hooks.Fixes.CommandFontReference", WoTHookFixTest::Flags)

bool FWoTHookFixCommandFontSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	static_assert(std::is_same_v<decltype(FWoTHUDDrawCmd::Font), TWeakObjectPtr<UFont>>,
		"a draw command's run-time face is a weak reference, not a soft path");
	FScene S;
	if (!S.Init(*this)) return false;
	UFont* Face = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!TestNotNull(TEXT("a face of its own"), Face)) return false;
	S.HUD->AddGenericMessage(TEXT("Own face"), 40, 200, false, 255, Face, 5.0f);
	const TArray<FString> Flat = S.DrawFrame();
	const FWoTHUDSection* Messages = S.HUD->GetSnapshot().FindSection(EWoTHUDSection::Messages);
	if (!TestNotNull(TEXT("the message shows"), Messages)) return false;
	const FWoTHUDDrawCmd* Line = Messages->Commands.FindByPredicate([](const FWoTHUDDrawCmd& Command)
	{
		return Command.Text == TEXT("Own face");
	});
	if (!TestNotNull(TEXT("its command"), Line)) return false;
	TestTrue(TEXT("the command refers to the message's own face"), Line->Font.Get() == Face);
	TestTrue(TEXT("the face resolves through the reference"),
		S.HUD->ResolveCommandFont(*Line, 640) == Face);
	TestTrue(TEXT("the flat HUD draws the message in its own face"),
		Flat.ContainsByPredicate([Face](const FString& Submitted)
		{
			return Submitted.StartsWith(FString::Printf(TEXT("text %s \"Own face\""), *Face->GetName()));
		}));
	// A destination drawing the same command draws the same face.
	const TArray<FString> Destination = WoTHUDHookTest::DrawSectionsOnce(S.F.World,
		S.HUD->GetSnapshot().Sections, 640, 480);
	TestEqual(TEXT("a destination's canvas receives the flat HUD's stream"), Destination, Flat);
	// A face that is gone leaves the command's id to resolve.
	FWoTHUDDrawCmd Stale = *Line;
	Stale.Font.Reset();
	Stale.FontId = FName(TEXT("F_WOTReg14"));
	TestTrue(TEXT("without its face a command resolves its id"),
		S.HUD->ResolveCommandFont(Stale, 640) == S.HUD->ResolveCommandFont(
			[] { FWoTHUDDrawCmd Probe; Probe.FontId = FName(TEXT("F_WOTReg14")); return Probe; }(), 640));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixScreenSizeSpec,
	"WoT.VR.Hooks.Fixes.HostScreenSize", WoTHookFixTest::Flags)

bool FWoTHookFixScreenSizeSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	using namespace WoTHookFixTest;
	FGuard Guard;
	FScreens S;
	if (!S.Init(*this)) return false;
	FVector2D ViewportSize = FVector2D::ZeroVector;
	S.F.Viewport->GetViewportSize(ViewportSize);
	auto Dpi = [](const FVector2D& Size)
	{
		return GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(
			FIntPoint(FMath::TruncToInt32(Size.X), FMath::TruncToInt32(Size.Y)));
	};
	// Without a host: exactly the viewport's size, and a screen that opens is
	// sized for the viewport.
	TestEqual(TEXT("no host: the viewport's own size"),
		FWoTLocalPlayerHooks::UIScreenSize(*S.F.Viewport), ViewportSize);
	TSharedPtr<SWidget> Chat = WoTUIScreensTest::OpenControllerChat(*S.F.PC);
	if (TestTrue(TEXT("controller chat opens"), Chat.IsValid()))
	{
		TestEqual(TEXT("no host: chat lays out for the viewport"),
			WoTUIScreensTest::ChatDpiScale(Chat.ToSharedRef()), Dpi(ViewportSize), 1e-4f);
		FWoTTextEntry::Cancel();
		WoTUIScreensTest::CloseChat(*S.F.PC);
	}

	// A host without a size of its own changes nothing.
	FHost Plain;
	if (!TestTrue(TEXT("a host without a size attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, Plain.Consumer()))) return false;
	TestEqual(TEXT("a host without a size: the viewport's"),
		FWoTLocalPlayerHooks::UIScreenSize(*S.F.Viewport), ViewportSize);
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);

	// A host that gives its surface's size: screens that open are sized for it
	// from their first frame.
	FHost Sized;
	Sized.Size = FIntPoint(1600, 1200);
	if (!TestTrue(TEXT("a sized host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, Sized.Consumer()))) return false;
	TestEqual(TEXT("the host's surface"), FWoTLocalPlayerHooks::UIScreenSize(*S.F.Viewport),
		FVector2D(1600.0, 1200.0));
	Chat = WoTUIScreensTest::OpenControllerChat(*S.F.PC);
	if (TestTrue(TEXT("controller chat opens in the host"), Chat.IsValid()))
	{
		TestTrue(TEXT("hosted"), Sized.Hosts(Chat.ToSharedRef()));
		TestEqual(TEXT("chat lays out for the surface from its first frame"),
			WoTUIScreensTest::ChatDpiScale(Chat.ToSharedRef()), Dpi(FVector2D(1600.0, 1200.0)), 1e-4f);
		TestNotEqual(TEXT("which differs from the viewport's"), Dpi(FVector2D(1600.0, 1200.0)),
			Dpi(ViewportSize));
		FWoTTextEntry::Cancel();
		WoTUIScreensTest::CloseChat(*S.F.PC);
	}
	// Another viewport's screens keep their own size.
	UGameViewportClient* Other = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
	FVector2D OtherSize = FVector2D::ZeroVector;
	Other->GetViewportSize(OtherSize);
	TestEqual(TEXT("another viewport is not the host's"), FWoTLocalPlayerHooks::UIScreenSize(*Other), OtherSize);
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	TestEqual(TEXT("detached: the viewport's own size again"),
		FWoTLocalPlayerHooks::UIScreenSize(*S.F.Viewport), ViewportSize);
	return !HasAnyErrors();
}

namespace WoTHookFixTest
{
/** Records the pointer moves that reach it: their pointer index and whether
 * they carry the left button. */
class SPointerProbe final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPointerProbe) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&) {}

	TArray<TPair<uint32, bool>> Moves;

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(120.0, 60.0); }
	virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&,
		int32 LayerId, const FWidgetStyle&, bool) const override
	{
		return LayerId;
	}
	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override
	{
		Moves.Emplace(Event.GetPointerIndex(), Event.IsMouseButtonDown(EKeys::LeftMouseButton));
		return FReply::Unhandled();
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixPointerUpOverNothingSpec,
	"WoT.VR.Hooks.Fixes.PointerReleasedOverNothing", WoTHookFixTest::Flags)

bool FWoTHookFixPointerUpOverNothingSpec::RunTest(const FString&)
{
	// A press the host's pointer releases where its window's hit test finds
	// nothing still ends: the capture goes, nothing stays pressed, the next
	// move carries no held button, and letting go there is not a click. The
	// host's pointer is its own Slate pointer, never the desktop mouse's.
	using namespace WoTHookFixTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SButton> Button;
	TSharedPtr<SPointerProbe> Probe;
	int32 Clicks = 0;
	H.Surface->AddSlot(5000)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	[
		SAssignNew(Button, SButton)
		.OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })
		[
			SNew(SBox).WidthOverride(120.0f).HeightOverride(60.0f)
		]
	];
	H.Surface->AddSlot(5001)
	.HAlign(HAlign_Right)
	.VAlign(VAlign_Bottom)
	[
		SAssignNew(Probe, SPointerProbe)
	];
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	H.Paint();

	// Pressed on the button, released past the window's edge.
	TestTrue(TEXT("the pointer presses the button"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Button.ToSharedRef()), EWoTUIPointerAction::Down));
	TestTrue(TEXT("the pressed button captures the pointer"), Button->HasMouseCapture() && Button->IsPressed());
	const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(S.User());
	TestTrue(TEXT("on the host's own pointer index"), SlateUser.IsValid()
		&& SlateUser->DoesWidgetHaveCapture(Button, FWoTLocalPlayerHooks::UIPointerIndex));
	TestFalse(TEXT("not the desktop mouse's"), SlateUser.IsValid()
		&& SlateUser->DoesWidgetHaveCapture(Button, FSlateApplication::CursorPointerIndex));
	const FVector2D Nowhere(-200.0, -200.0);
	TestTrue(TEXT("nothing is under the release"), H.Window->GetHittestGrid().GetBubblePath(
		Nowhere, 0.0f, false, S.User()).IsEmpty());
	TestTrue(TEXT("the release is still routed"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Nowhere, EWoTUIPointerAction::Up));
	TestFalse(TEXT("nothing stays captured"), Button->HasMouseCapture() || Slate.HasUserMouseCapture(S.User()));
	TestFalse(TEXT("the button is no longer pressed"), Button->IsPressed());
	TestEqual(TEXT("released away from it: no click"), Clicks, 0);

	// The press is over: the next move carries no held button.
	TestTrue(TEXT("a move is routed"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Probe.ToSharedRef()), EWoTUIPointerAction::Move));
	if (TestTrue(TEXT("the move reaches the probe"), Probe->Moves.Num() > 0))
	{
		TestFalse(TEXT("with no button held"), Probe->Moves.Last().Value);
		TestEqual(TEXT("on the host's own pointer index"), static_cast<int32>(Probe->Moves.Last().Key),
			static_cast<int32>(FWoTLocalPlayerHooks::UIPointerIndex));
	}
	// And a fresh press and release on the button, the pointer moved onto it
	// first as a laser's or gaze's is every frame, is a click.
	H.Paint();
	TestTrue(TEXT("onto the button"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Button.ToSharedRef()), EWoTUIPointerAction::Move));
	TestTrue(TEXT("a fresh press"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Button.ToSharedRef()), EWoTUIPointerAction::Down));
	TestTrue(TEXT("and release"), FWoTLocalPlayerHooks::RoutePointer(
		*S.F.Player, HostOwner, Center(Button.ToSharedRef()), EWoTUIPointerAction::Up));
	TestEqual(TEXT("click"), Clicks, 1);
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFixPointerLeaveSpec,
	"WoT.VR.Hooks.Fixes.PointerLeave", WoTHookFixTest::Flags)

bool FWoTHookFixPointerLeaveSpec::RunTest(const FString&)
{
	// Hover is per pointer: Slate unhovers a widget only when a move of the
	// same pointer no longer finds it. Leave is that move for the host's
	// pointer once it has left the window; it is refused while the pointer's
	// press is held, and a detaching host sends it.
	using namespace WoTHookFixTest;
	FGuard Guard;
	FHost H;
	FScreens S;
	if (!S.Init(*this)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SButton> Button;
	int32 Clicks = 0;
	H.Surface->AddSlot(5000)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	[
		SAssignNew(Button, SButton)
		.OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })
		[
			SNew(SBox).WidthOverride(120.0f).HeightOverride(60.0f)
		]
	];
	if (!TestTrue(TEXT("the host attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer()))) return false;
	H.Paint();
	const FVector2D On = Center(Button.ToSharedRef());
	auto Route = [&S](const FVector2D& Position, EWoTUIPointerAction Action)
	{
		return FWoTLocalPlayerHooks::RoutePointer(*S.F.Player, HostOwner, Position, Action);
	};
	auto UnderPointer = [&Slate, &S, &Button]()
	{
		const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(S.User());
		return SlateUser.IsValid()
			&& SlateUser->IsWidgetUnderPointer(Button, FWoTLocalPlayerHooks::UIPointerIndex);
	};

	// A move of the host's pointer hovers the button; Leave unhovers it.
	TestTrue(TEXT("a move is routed"), Route(On, EWoTUIPointerAction::Move));
	TestTrue(TEXT("the button under the pointer is hovered"), Button->IsHovered() && UnderPointer());
	TestTrue(TEXT("Leave is routed"), Route(On, EWoTUIPointerAction::Leave));
	TestFalse(TEXT("Leave unhovers it"), Button->IsHovered());
	TestFalse(TEXT("the pointer is over nothing"), UnderPointer());
	TestEqual(TEXT("leaving is not a click"), Clicks, 0);
	TestTrue(TEXT("back onto the button"), Route(On, EWoTUIPointerAction::Move));
	TestTrue(TEXT("hovered again"), Button->IsHovered());

	// Pressed: Leave is refused and changes nothing.
	TestTrue(TEXT("a press"), Route(On, EWoTUIPointerAction::Down));
	TestTrue(TEXT("the pressed button captures the pointer"), Button->HasMouseCapture() && Button->IsPressed());
	TestFalse(TEXT("Leave is refused while the press is held"), Route(On, EWoTUIPointerAction::Leave));
	TestTrue(TEXT("the pressed button keeps its hover"), Button->IsHovered() && UnderPointer());
	TestTrue(TEXT("and its press and capture"), Button->IsPressed() && Button->HasMouseCapture());
	TestTrue(TEXT("the release"), Route(On, EWoTUIPointerAction::Up));
	TestEqual(TEXT("the press was untouched: released on the button, it clicks"), Clicks, 1);
	TestTrue(TEXT("released: Leave is routed"), Route(On, EWoTUIPointerAction::Leave));
	TestFalse(TEXT("and unhovers it"), Button->IsHovered());

	// Detaching unhovers what the host's pointer hovered.
	Route(On, EWoTUIPointerAction::Move);
	TestTrue(TEXT("hovered before the detach"), Button->IsHovered());
	FWoTLocalPlayerHooks::Detach(*S.F.Player, HostOwner);
	TestFalse(TEXT("detached: the button is no longer hovered"), Button->IsHovered());
	TestFalse(TEXT("detached: the pointer is over nothing"), UnderPointer());

	// Detached from inside Slate's input handling, with no key to release: the
	// leave follows once that input is done, never nested in it.
	struct FDetachOnKey final : public IInputProcessor
	{
		ULocalPlayer* Player = nullptr;
		int32 Detaches = 0;
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
		{
			if (Event.GetKey() != EKeys::F9 || !Player)
			{
				return false;
			}
			++Detaches;
			FWoTLocalPlayerHooks::Detach(*Player, HostOwner);
			return true;
		}
	};
	const TSharedRef<FDetachOnKey> Shortcut = MakeShared<FDetachOnKey>();
	Shortcut->Player = S.F.Player;
	if (TestTrue(TEXT("the host attaches again"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, HostOwner, H.Consumer())))
	{
		H.Paint();
		Route(On, EWoTUIPointerAction::Move);
		TestTrue(TEXT("hovered again"), Button->IsHovered());
		Slate.RegisterInputPreProcessor(Shortcut);
		TestTrue(TEXT("the shortcut takes its key"), Slate.ProcessKeyDownEvent(
			FKeyEvent(EKeys::F9, FModifierKeysState(), S.User(), false, 0, 0)));
		TestEqual(TEXT("the shortcut detached the host"), Shortcut->Detaches, 1);
		TestTrue(TEXT("not nested inside the shortcut's input: still hovered"), Button->IsHovered());
		Slate.Tick();
		TestFalse(TEXT("once the input is done the leave follows"), Button->IsHovered());
		Slate.UnregisterInputPreProcessor(Shortcut);
	}
	TestEqual(TEXT("no click from any of it"), Clicks, 1);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
