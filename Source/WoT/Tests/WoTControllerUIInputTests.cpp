// Automation specs for controller sub-project 3, PR 1: the controller UI input
// foundation (last input, cursor lease, navigation repeat, back stack, focus
// graphs, initial focus). Spec r3 sections 1, 1a and Testing. Every spec runs
// headless through injected seams: a recording cursor policy and a fake clock.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../UI/SWoTBootstrapScreen.h"
#include "../UI/SWoTButtonPrompts.h"
#include "../UI/SWoTFocusOutline.h"
#include "../UI/SWoTListCombo.h"
#include "../UI/WoTBackStack.h"
#include "../UI/WoTControllerUIInput.h"
#include "../UI/WoTMenuNavigation.h"
#include "../Online/WoTLocalNetworkAccess.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Settings/WoTGameSettings.h"
#include "../Settings/WoTSettingsSlate.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "WoTTestWorld.h"

#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Types/NavigationMetaData.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Framework/Application/SlateUser.h"

namespace
{
constexpr EAutomationTestFlags WoTUITest_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent WoTUITest_Key(FKey Key, bool bRepeat = false)
{
	return FKeyEvent(Key, FModifierKeysState(), 0, bRepeat, 0, 0);
}

FAnalogInputEvent WoTUITest_Analog(FKey Key, float Value)
{
	return FAnalogInputEvent(Key, FModifierKeysState(), 0, false, 0, 0, Value);
}

FPointerEvent WoTUITest_MouseMove(const FVector2D& Position, const FVector2D& Last)
{
	return FPointerEvent(0, FSlateApplication::CursorPointerIndex, Position, Last,
		TSet<FKey>(), EKeys::Invalid, 0.0f, FModifierKeysState());
}

FPointerEvent WoTUITest_MouseDown(const FVector2D& Position)
{
	TSet<FKey> Down;
	Down.Add(EKeys::LeftMouseButton);
	return FPointerEvent(0, FSlateApplication::CursorPointerIndex, Position, Position,
		Down, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
}

FPointerEvent WoTUITest_Touch(const FVector2D& Position)
{
	return FPointerEvent(0u, 0u, Position, Position, 1.0f, true);
}

FVector2D WoTUITest_Center(const TSharedPtr<SWidget>& Widget)
{
	return FVector2D(Widget->GetCachedGeometry()
		.GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
}

/** Where the focus graph sends a navigation from this widget, or null. */
TSharedPtr<SWidget> WoTUITest_Navigate(const TSharedPtr<SWidget>& From,
	EUINavigation Direction)
{
	if (!From.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FNavigationMetaData> MetaData =
		From->GetMetaData<FNavigationMetaData>();
	if (!MetaData.IsValid()
		|| MetaData->GetBoundaryRule(Direction) != EUINavigationRule::Custom
		|| !MetaData->GetFocusDelegate(Direction).IsBound())
	{
		return nullptr;
	}
	return MetaData->GetFocusDelegate(Direction).Execute(Direction);
}

/** A real Slate window around the content, destroyed with the fixture. */
struct FWoTUITest_Window
{
	TSharedRef<SWindow> Window;

	explicit FWoTUITest_Window(const TSharedRef<SWidget>& Content,
		FVector2D Size = FVector2D(1024.0f, 768.0f))
		: Window(SNew(SWindow)
			.ClientSize(Size)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[Content])
	{
		FSlateApplication::Get().AddWindow(Window, true);
		Settle();
	}

	void Settle()
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.Tick();
		Window->SlatePrepass(1.0f);
		Paint();
		Slate.Tick();
		Window->SlatePrepass(1.0f);
		Paint();
	}

	/** Headless Slate never draws: an explicit paint arranges the widgets
	 * (their cached geometry) and fills the window's hit-test grid. */
	void Paint()
	{
		FSlateWindowElementList Elements(Window);
		Window->PaintWindow(FSlateApplication::Get().GetCurrentTime(), 0.0f,
			Elements, FWidgetStyle(), true);
	}

	/** A widget's centre in desktop space, for real mouse events. */
	FVector2D ScreenCenter(const TSharedPtr<SWidget>& Widget) const
	{
		return FVector2D(Window->GetPositionInScreen()) + WoTUITest_Center(Widget);
	}

	~FWoTUITest_Window()
	{
		Window->RequestDestroyWindow();
		FSlateApplication::Get().Tick();
	}
};

/**
 * Registers the controller UI preprocessor for one window with a fake clock
 * and a recording cursor policy; restores Pointer mode and production state.
 */
struct FWoTUITest_InputScope
{
	TSharedRef<double> Time = MakeShared<double>(100.0);
	TSharedRef<FWoTRecordingCursorPolicy> Policy =
		MakeShared<FWoTRecordingCursorPolicy>();
	TSharedPtr<FWoTControllerUIInputRegistration> Registration;
	TSharedPtr<FWoTControllerUIInput> Input;

	explicit FWoTUITest_InputScope(const TSharedRef<SWindow>& Window,
		bool bApplySlateSideEffects = false)
	{
		FWoTControllerUIInput::ResetForTesting();
		Policy->bApplySlateSideEffects = bApplySlateSideEffects;
		FWoTControllerUIInput::SetCursorPolicyForTesting(Policy);
		const TWeakPtr<SWindow> WeakWindow = Window;
		const TSharedRef<double> Clock = Time;
		Registration = FWoTControllerUIInput::Register(
			[WeakWindow]() { return WeakWindow.Pin(); },
			[Clock]() { return *Clock; });
		Input = FWoTControllerUIInput::GetRegistered();
	}

	~FWoTUITest_InputScope()
	{
		Input.Reset();
		Registration.Reset();
		FWoTControllerUIInput::ResetForTesting();
	}
};

/** Leaves Pointer mode and the production policy behind, whatever happens. */
struct FWoTUITest_ModeGuard
{
	FWoTUITest_ModeGuard() { FWoTControllerUIInput::ResetForTesting(); }
	~FWoTUITest_ModeGuard() { FWoTControllerUIInput::ResetForTesting(); }
};

class FWoTUITest_Feedback final : public IWoTFrontEndFeedbackSink
{
public:
	virtual bool IsReady() const override { return true; }
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

FString WoTUITest_Label(int32 Length, TCHAR Character)
{
	return FString::ChrN(Length, Character);
}

TSharedRef<FWoTFrontEndPresentationAuthority> WoTUITest_Authority()
{
	TMap<FName, FString> Labels;
	Labels.Add(FName(TEXT("menuMain.MenuList[2]")), WoTUITest_Label(11, TEXT('M')));
	Labels.Add(FName(TEXT("menuMain.MenuList[3]")), WoTUITest_Label(8, TEXT('C')));
	Labels.Add(FName(TEXT("menuMain.MenuList[4]")), WoTUITest_Label(8, TEXT('H')));
	Labels.Add(FName(TEXT("menuMain.MenuList[7]")), WoTUITest_Label(4, TEXT('Q')));
	Labels.Add(FName(TEXT("menuMultiPlayer.HelpMessage[1]")), WoTUITest_Label(20, TEXT('J')));
	Labels.Add(FName(TEXT("menuMultiPlayer.MenuList[2]")), WoTUITest_Label(13, TEXT('P')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuTitle")), WoTUITest_Label(12, TEXT('A')));
	Labels.Add(FName(TEXT("menuStartBattleServer.MenuTitle")), WoTUITest_Label(14, TEXT('B')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[1]")), WoTUITest_Label(10, TEXT('S')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[2]")), WoTUITest_Label(12, TEXT('K')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[3]")), WoTUITest_Label(10, TEXT('T')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[5]")), WoTUITest_Label(11, TEXT('N')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[7]")), WoTUITest_Label(11, TEXT('V')));
	Labels.Add(FName(TEXT("menuPlayer.MenuTitle")), WoTUITest_Label(13, TEXT('R')));
	Labels.Add(FName(TEXT("menuPlayer.MenuList[1]")), WoTUITest_Label(5, TEXT('N')));
	Labels.Add(FName(TEXT("menuPlayer.MenuList[3]")), WoTUITest_Label(9, TEXT('S')));
	Labels.Add(FName(TEXT("menuOptions.MenuTitle")), WoTUITest_Label(8, TEXT('O')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[1]")), WoTUITest_Label(18, TEXT('U')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[2]")), WoTUITest_Label(12, TEXT('I')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[3]")), WoTUITest_Label(17, TEXT('E')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuTitle")), WoTUITest_Label(8, TEXT('G')));
	return FWoTFrontEndPresentationAuthority::CreateForTesting(
		TEXT("controller-ui-input"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
		MoveTemp(Labels), MakeShared<FWoTUITest_Feedback>());
}

/** A front-end root shown in a real window at the given size. */
struct FWoTUITest_FrontEnd
{
	TSharedRef<FWoTFrontEndPresentationAuthority> Authority;
	TSharedRef<SWoTFrontEndRoot> Root;
	FWoTUITest_Window Shown;
	FVector2D Size;

	FWoTUITest_FrontEnd(TSharedRef<FWoTFrontEndPresentationAuthority> InAuthority,
		TSharedRef<SWoTFrontEndRoot> InRoot, FVector2D InSize)
		: Authority(InAuthority), Root(InRoot), Shown(InRoot, InSize), Size(InSize)
	{
		Settle();
		Root->FocusInitialWidget();
		Settle();
	}

	void Settle()
	{
		Root->Tick(FGeometry::MakeRoot(Size, FSlateLayoutTransform()), 0.0, 0.0f);
		Shown.Settle();
		Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
		Shown.Settle();
	}

	bool Press(FKey Key, bool bRepeat = false)
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		const bool bHandled = Slate.ProcessKeyDownEvent(WoTUITest_Key(Key, bRepeat));
		Slate.ProcessKeyUpEvent(WoTUITest_Key(Key));
		Settle();
		return bHandled;
	}
};

TSharedRef<SWoTFrontEndRoot> WoTUITest_MakeRoot(
	const TSharedRef<FWoTFrontEndPresentationAuthority>& Authority,
	AWoTFrontEndPlayerController* Controller = nullptr,
	bool bShowDisclosure = false, UWoTPreferencesSubsystem* Preferences = nullptr)
{
	return SNew(SWoTFrontEndRoot)
		.PlayerController(Controller)
		.PresentationAuthorityForTesting(Authority)
		.ShowOnlineDisclosureForTesting(bShowDisclosure)
		.PreferencesForTesting(Preferences);
}

UWorld* WoTUITest_CreateWorld()
{
	ResetWoTPayloadActivationForTesting();
	if (PublishWoTPayloadBootstrapActivationForTesting().IsError()
		|| PublishWoTBootstrapWorldReadyForTesting().IsError())
	{
		ResetWoTPayloadActivationForTesting();
		return nullptr;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (World == nullptr)
	{
		ResetWoTPayloadActivationForTesting();
	}
	return World;
}

void WoTUITest_DestroyWorld(UWorld*& World)
{
	if (World != nullptr)
	{
		WoTTest::DestroySpecWorld(World);
		World = nullptr;
	}
	ResetWoTPayloadActivationForTesting();
}

TArray<uint8> WoTUITest_Utf8(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

/** A real, activated preferences owner writing to a temporary file. */
struct FWoTUITest_Preferences
{
	FString PreviousCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	float PreviousScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale() : 1.0f;
	float PreviousVolume = FApp::GetVolumeMultiplier();
	FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("ControllerUI-")) + FGuid::NewGuid().ToString());
	FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	UWoTGameInstance* Instance = nullptr;
	UWoTPreferencesSubsystem* Owner = nullptr;
	bool bReady = false;

	FWoTUITest_Preferences()
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
		ResetWoTPayloadActivationForTesting();
		Instance = NewObject<UWoTGameInstance>();
		Instance->AddToRoot();
		Owner = NewObject<UWoTPreferencesSubsystem>(Instance);
		Owner->AddToRoot();
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
			WoTUITest_Utf8("Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"),
			FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
		if (Parsed.IsError()) return;
		if (Owner->AdoptAuthenticatedEarlyUI(MoveTemp(Parsed).GetValue()).IsError()) return;
		if (PublishWoTPayloadBootstrapActivationForTesting().IsError()) return;
		bReady = !Owner->ApplyFullPreferencesAfterCacheActivation().IsError();
	}

	~FWoTUITest_Preferences()
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		if (Owner) Owner->RemoveFromRoot();
		if (Instance) Instance->RemoveFromRoot();
		FApp::SetVolumeMultiplier(PreviousVolume);
		FInternationalization::Get().SetCurrentCulture(PreviousCulture);
		if (FSlateApplication::IsInitialized())
			FSlateApplication::Get().SetApplicationScale(PreviousScale);
	}
};

/** Empties the onboarding settings rows for a spec; restores them on exit. */
struct FWoTUITest_SettingsRowsGuard
{
	TArray<FWoTOnboardingRow> Saved;
	FWoTUITest_SettingsRowsGuard()
	{
		Saved = MoveTemp(FWoTOnboardingSeams::Get().SettingsRows);
		FWoTOnboardingSeams::Get().SettingsRows.Reset();
	}
	~FWoTUITest_SettingsRowsGuard()
	{
		FWoTOnboardingSeams::Get().SettingsRows = MoveTemp(Saved);
	}
};

TSharedRef<FWoTRequiredMenuFonts> WoTUITest_Fonts()
{
	// The fonts hold their generation weakly; keep one alive for the specs.
	static const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	return Fonts;
}

bool WoTUITest_RealSyntheticMovesEnabled()
{
	const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
		TEXT("Slate.EnableSyntheticCursorMoves"));
	return Variable == nullptr || Variable->GetBool();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputRegistrationSpec,
	"WoT.UI.ControllerInput.Registration", WoTUITest_Flags)

bool FWoTUIControllerInputRegistrationSpec::RunTest(const FString&)
{
	TestTrue(TEXT("a game instance registers"),
		FWoTControllerUIInput::ShouldRegisterForGameInstance(false, true, false));
	TestFalse(TEXT("a dedicated server never registers"),
		FWoTControllerUIInput::ShouldRegisterForGameInstance(true, true, false));
	TestFalse(TEXT("no Slate, no preprocessor"),
		FWoTControllerUIInput::ShouldRegisterForGameInstance(false, false, false));
	TestFalse(TEXT("automation runs keep the editor's input"),
		FWoTControllerUIInput::ShouldRegisterForGameInstance(false, true, true));
	if (!TestTrue(TEXT("Slate is initialized"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	TestFalse(TEXT("nothing is registered at rest"),
		FWoTControllerUIInput::GetRegistered().IsValid());
	const TSharedRef<FNavigationConfig> Before = Slate.GetNavigationConfig();
	{
		TSharedPtr<FWoTControllerUIInputRegistration> First =
			FWoTControllerUIInput::Register([]() { return TSharedPtr<SWindow>(); });
		TSharedPtr<FWoTControllerUIInputRegistration> Second =
			FWoTControllerUIInput::Register([]() { return TSharedPtr<SWindow>(); });
		const TSharedPtr<FWoTControllerUIInput> Input = FWoTControllerUIInput::GetRegistered();
		if (TestTrue(TEXT("one preprocessor serves every scope"), Input.IsValid()))
		{
			TestTrue(TEXT("the threshold-crossing navigation config is installed"),
				&Slate.GetNavigationConfig().Get()
					== static_cast<FNavigationConfig*>(&Input->GetNavigationConfig().Get()));
		}
		First.Reset();
		TestTrue(TEXT("it stays while any scope remains"),
			FWoTControllerUIInput::GetRegistered().IsValid());
		Second.Reset();
		TestFalse(TEXT("dropping the last scope unregisters it"),
			FWoTControllerUIInput::GetRegistered().IsValid());
		TestTrue(TEXT("the previous navigation config is restored"),
			&Slate.GetNavigationConfig().Get() == &Before.Get());
	}
	TestFalse(TEXT("a scope needs a resolver"),
		FWoTControllerUIInput::Register(nullptr).IsValid());

	// The config navigates once per threshold crossing; holding never repeats.
	FWoTNavigationConfig Config;
	TestEqual(TEXT("a crossing navigates"),
		Config.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.8f)),
		EUINavigation::Up);
	TestEqual(TEXT("several widgets asking about one event get one navigation"),
		Config.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.8f)),
		EUINavigation::Invalid);
	TestEqual(TEXT("a held stick never repeats"),
		Config.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.95f)),
		EUINavigation::Invalid);
	TestEqual(TEXT("below the threshold is no navigation"),
		Config.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.2f)),
		EUINavigation::Invalid);
	TestEqual(TEXT("the next crossing navigates again"),
		Config.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftY, -0.7f)),
		EUINavigation::Down);
	FWoTNavigationConfig Observed;
	Observed.ObserveAnalog(0, EKeys::Gamepad_LeftX, 0.9f);
	Observed.ObserveAnalog(0, EKeys::Gamepad_LeftX, 0.9f);
	TestEqual(TEXT("a crossing nobody navigated on is never replayed"),
		Observed.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftX, 0.9f)),
		EUINavigation::Invalid);
	TestEqual(TEXT("an observed crossing navigates once"),
		Observed.ObserveAnalog(0, EKeys::Gamepad_LeftX, -0.9f), EUINavigation::Left);
	TestEqual(TEXT("Slate reads the observed crossing"),
		Observed.GetNavigationDirectionFromAnalog(WoTUITest_Analog(EKeys::Gamepad_LeftX, -0.9f)),
		EUINavigation::Left);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputLastInputSpec,
	"WoT.UI.ControllerInput.LastInput", WoTUITest_Flags)

bool FWoTUIControllerInputLastInputSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SButton> Button;
	FWoTUITest_Window Shown(SAssignNew(Button, SButton)[SNew(STextBlock).Text(FText::FromString(TEXT("Button")))]);
	FWoTUITest_InputScope Scope(Shown.Window);
	if (!TestTrue(TEXT("the preprocessor is registered"), Scope.Input.IsValid()))
	{
		return false;
	}
	Slate.SetKeyboardFocus(Button, EFocusCause::SetDirectly);
	int32 Changes = 0;
	const FDelegateHandle Handle = FWoTControllerUIInput::OnLastInputChanged().AddLambda(
		[&Changes](EWoTLastInput) { ++Changes; });
	ON_SCOPE_EXIT { FWoTControllerUIInput::OnLastInputChanged().Remove(Handle); };
	FWoTControllerUIInput& Input = *Scope.Input;
	auto Mode = []() { return FWoTControllerUIInput::GetLastInput(); };
	Scope.Policy->CursorPosition = FVector2D(200.0f, 200.0f);

	TestEqual(TEXT("Pointer at rest"), Mode(), EWoTLastInput::Pointer);
	TestFalse(TEXT("a gamepad key is never consumed for the mode"),
		Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Top)));
	TestEqual(TEXT("a gamepad key is Controller"), Mode(), EWoTLastInput::Controller);
	TestFalse(TEXT("a keyboard key is never consumed"),
		Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::A)));
	TestEqual(TEXT("a keyboard key is Pointer"), Mode(), EWoTLastInput::Pointer);
	Input.HandleAnalogInputEvent(Slate, WoTUITest_Analog(EKeys::Gamepad_RightY, 0.3f));
	TestEqual(TEXT("a stick under 0.35 stays Pointer"), Mode(), EWoTLastInput::Pointer);
	Input.HandleAnalogInputEvent(Slate, WoTUITest_Analog(EKeys::Gamepad_RightY, 0.36f));
	TestEqual(TEXT("a stick past 0.35 is Controller"), Mode(), EWoTLastInput::Controller);
	Input.HandleAnalogInputEvent(Slate, WoTUITest_Analog(EKeys::Gamepad_RightY, 0.0f));

	// The 4 px rule, from where Controller mode began.
	Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
		FVector2D(203.0f, 202.0f), FVector2D(200.0f, 200.0f)));
	TestEqual(TEXT("a real move of 4 px or less stays Controller"),
		Mode(), EWoTLastInput::Controller);
	TestFalse(TEXT("with no lease a short move is not consumed"),
		Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
			FVector2D(200.0f, 203.0f), FVector2D(200.0f, 200.0f))));
	Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
		FVector2D(204.5f, 200.0f), FVector2D(200.0f, 200.0f)));
	TestEqual(TEXT("a real move past 4 px is Pointer"), Mode(), EWoTLastInput::Pointer);

	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_DPad_Down));
	Input.HandleKeyUpEvent(Slate, WoTUITest_Key(EKeys::Gamepad_DPad_Down));
	TestEqual(TEXT("the D-pad is Controller"), Mode(), EWoTLastInput::Controller);
	TestFalse(TEXT("a mouse button is never consumed"),
		Input.HandleMouseButtonDownEvent(Slate, WoTUITest_MouseDown(FVector2D(10.0f, 10.0f))));
	TestEqual(TEXT("a mouse button is Pointer"), Mode(), EWoTLastInput::Pointer);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	Input.HandleMouseButtonDownEvent(Slate, WoTUITest_Touch(FVector2D(10.0f, 10.0f)));
	TestEqual(TEXT("touch is Pointer"), Mode(), EWoTLastInput::Pointer);

	// The active-entry exemption: typing into a text entry keeps Controller.
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	FWoTControllerUIInput::SetTextEntryActive(true);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Q));
	TestEqual(TEXT("keys typed during an active entry stay Controller"),
		Mode(), EWoTLastInput::Controller);
	FWoTControllerUIInput::SetTextEntryActive(false);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Q));
	TestEqual(TEXT("after the entry a key is Pointer again"), Mode(), EWoTLastInput::Pointer);
	TestEqual(TEXT("each change fires the event once"), Changes, 10);

	// Scoped to its window family: another window's events are ignored.
	TSharedPtr<SButton> Elsewhere;
	FWoTUITest_Window Other(SAssignNew(Elsewhere, SButton));
	Slate.SetKeyboardFocus(Elsewhere, EFocusCause::SetDirectly);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("a window outside the scope never changes the mode"),
		Mode(), EWoTLastInput::Pointer);

	// A widget scope (the editor's game viewport during PIE) covers only the
	// focus path through that widget, not the rest of its window.
	TSharedPtr<SButton> InViewport;
	TSharedPtr<SButton> EditorPanel;
	TSharedPtr<SBox> ViewportWidget;
	FWoTUITest_Window Editor(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SAssignNew(ViewportWidget, SBox)[SAssignNew(InViewport, SButton)]
		]
		+ SHorizontalBox::Slot().AutoWidth()[SAssignNew(EditorPanel, SButton)]);
	const TWeakPtr<SWidget> WeakViewport = ViewportWidget;
	const TSharedPtr<FWoTControllerUIInputRegistration> ViewportScope =
		FWoTControllerUIInput::Register([WeakViewport]() { return WeakViewport.Pin(); });
	Slate.SetKeyboardFocus(EditorPanel, EFocusCause::SetDirectly);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("the editor's own panels are outside a viewport scope"),
		Mode(), EWoTLastInput::Pointer);
	Slate.SetKeyboardFocus(InViewport, EFocusCause::SetDirectly);
	Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	TestEqual(TEXT("focus inside the viewport is in scope"),
		Mode(), EWoTLastInput::Controller);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputCursorLeaseSpec,
	"WoT.UI.ControllerInput.CursorLease", WoTUITest_Flags)

bool FWoTUIControllerInputCursorLeaseSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SButton> Button;
	FWoTUITest_Window Shown(SAssignNew(Button, SButton));
	FWoTUITest_InputScope Scope(Shown.Window);
	if (!TestTrue(TEXT("the preprocessor is registered"), Scope.Input.IsValid()))
	{
		return false;
	}
	Slate.SetKeyboardFocus(Button, EFocusCause::SetDirectly);
	FWoTRecordingCursorPolicy& Policy = *Scope.Policy;
	FWoTControllerUIInput& Input = *Scope.Input;
	Policy.CursorPosition = FVector2D(50.0f, 50.0f);

	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestTrue(TEXT("with no lease the cursor is left as the screen sets it"),
		Policy.Calls.IsEmpty());
	TSharedPtr<FWoTCursorLease> Lease = FWoTControllerUIInput::AcquireCursorLease();
	TestEqual(TEXT("a lease in Controller mode hides, stops synthetic moves, clears hover"),
		Policy.Calls, TArray<FString>{TEXT("HideCursor"), TEXT("SyntheticMovesOff"),
			TEXT("ClearHover")});
	TestFalse(TEXT("the cursor user is told to hide the cursor"), Policy.bCursorVisible);
	TestFalse(TEXT("synthetic cursor moves are off while leased"), Policy.bSyntheticMoves);
	TSharedPtr<FWoTCursorLease> Second = FWoTControllerUIInput::AcquireCursorLease();
	TestEqual(TEXT("a second lease changes nothing"), Policy.Calls.Num(), 3);
	TestTrue(TEXT("real moves under 4 px are consumed while leased"),
		Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
			FVector2D(52.0f, 51.0f), FVector2D(50.0f, 50.0f))));
	TestFalse(TEXT("a real move past 4 px reaches the widgets"),
		Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
			FVector2D(60.0f, 50.0f), FVector2D(52.0f, 51.0f))));
	TestEqual(TEXT("and returns to Pointer"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Pointer);
	TestTrue(TEXT("Pointer mode shows the cursor again"), Policy.bCursorVisible);
	TestTrue(TEXT("Pointer mode restores synthetic moves"), Policy.bSyntheticMoves);
	TestFalse(TEXT("Pointer mode never consumes a move"),
		Input.HandleMouseMoveEvent(Slate, WoTUITest_MouseMove(
			FVector2D(61.0f, 50.0f), FVector2D(60.0f, 50.0f))));

	// Ending the lease restores what was there before, not a default.
	Policy.bSyntheticMoves = false;
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestTrue(TEXT("Controller mode under a lease suppresses again"),
		FWoTControllerUIInput::IsCursorSuppressed());
	Lease.Reset();
	TestTrue(TEXT("one remaining lease keeps it suppressed"),
		FWoTControllerUIInput::IsCursorSuppressed());
	Second.Reset();
	TestFalse(TEXT("the last lease ending restores the cursor"),
		FWoTControllerUIInput::IsCursorSuppressed());
	TestTrue(TEXT("the cursor shows when the lease ends"), Policy.bCursorVisible);
	TestFalse(TEXT("synthetic moves return to their previous setting"),
		Policy.bSyntheticMoves);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputHoverFramesSpec,
	"WoT.UI.ControllerInput.HoverFrames", WoTUITest_Flags)

bool FWoTUIControllerInputHoverFramesSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	int32 Selected = 0;
	TSharedPtr<SWoTMenuButton> Menu[2];
	TSharedPtr<SButton> Stock;
	TSharedPtr<SCheckBox> Check;
	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Content->AddSlot().AutoHeight()
		[
			SNew(SBox).HeightOverride(80.0f).WidthOverride(400.0f)
			[
				SAssignNew(Menu[Index], SWoTMenuButton)
				.IsSelected_Lambda([&Selected, Index]() { return Selected == Index; })
				.OnSelected_Lambda([&Selected, Index]() { Selected = Index; })
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Entry")))
				]
			]
		];
	}
	Content->AddSlot().AutoHeight()
	[
		SNew(SBox).HeightOverride(80.0f).WidthOverride(400.0f)[SAssignNew(Stock, SButton)]
	];
	Content->AddSlot().AutoHeight()
	[
		SNew(SBox).HeightOverride(80.0f).WidthOverride(400.0f)[SAssignNew(Check, SCheckBox)]
	];
	FWoTUITest_Window Shown(Content);
	FWoTUITest_InputScope Scope(Shown.Window, /*bApplySlateSideEffects*/ true);
	const TSharedRef<FWoTCursorLease> Lease = FWoTControllerUIInput::AcquireCursorLease();
	Shown.Settle();

	// Pointer: a real move hovers and selects, as today.
	const FVector2D Over = Shown.ScreenCenter(Menu[1]);
	Slate.ProcessMouseMoveEvent(WoTUITest_MouseMove(Over, Over), /*bIsSynthetic*/ false);
	if (!TestTrue(TEXT("Pointer mode: a real move hovers the entry"), Menu[1]->IsHovered()))
	{
		return false;
	}
	TestEqual(TEXT("Pointer mode: hover selects the entry"), Selected, 1);
	const TSharedPtr<SWidget> FocusBefore = Slate.GetKeyboardFocusedWidget();

	// A controller press: hover clears once and cannot come back.
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Top));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Top));
	TestEqual(TEXT("the press is Controller"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Controller);
	TestFalse(TEXT("the cursor is hidden through the policy"), Scope.Policy->bCursorVisible);
	TestFalse(TEXT("synthetic cursor moves are off while leased"),
		WoTUITest_RealSyntheticMovesEnabled());
	bool bAllConsumed = true;
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		// The OS keeps reporting the cursor where it rests over the entry.
		bAllConsumed &= Slate.ProcessMouseMoveEvent(WoTUITest_MouseMove(Over, Over), false);
		Slate.Tick();
		Shown.Paint();
	}
	TestTrue(TEXT("the resting cursor's real moves are consumed"), bAllConsumed);
	TestFalse(TEXT("no entry draws hover in Controller mode"),
		Menu[0]->IsHovered() || Menu[1]->IsHovered());
	TestFalse(TEXT("the stock button and check box are not hovered"),
		Stock->IsHovered() || Check->IsHovered());
	TestEqual(TEXT("the selection did not move"), Selected, 1);
	TestTrue(TEXT("focus did not move"), Slate.GetKeyboardFocusedWidget() == FocusBefore);

	// A real move past 4 px is the pointer again: hover resumes on it.
	const FVector2D Away = Shown.ScreenCenter(Menu[0]);
	Slate.ProcessMouseMoveEvent(WoTUITest_MouseMove(Away, Over), false);
	TestEqual(TEXT("the real move is Pointer"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Pointer);
	TestTrue(TEXT("hover resumes on the next real move"), Menu[0]->IsHovered());
	TestEqual(TEXT("and selects as today"), Selected, 0);
	TestTrue(TEXT("synthetic moves are back"), WoTUITest_RealSyntheticMovesEnabled());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputRepeatSpec,
	"WoT.UI.ControllerInput.Repeat", WoTUITest_Flags)

bool FWoTUIControllerInputRepeatSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	TArray<TSharedPtr<SWidget>> Items;
	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		TSharedPtr<SButton> Item;
		List->AddSlot().AutoHeight()
		[
			SNew(SBox).HeightOverride(60.0f)[SAssignNew(Item, SButton)]
		];
		Items.Add(Item);
	}
	TSharedPtr<SButton> Other;
	List->AddSlot().AutoHeight()[SAssignNew(Other, SButton)];
	WoTWrapVerticalNavigation(Items);
	const TSharedRef<SBox> Surface = SNew(SBox)[List];
	FWoTUITest_Window Shown(Surface);
	FWoTUITest_InputScope Scope(Shown.Window);
	if (!TestTrue(TEXT("the preprocessor is registered"), Scope.Input.IsValid()))
	{
		return false;
	}
	bool bSurfaceActive = true;
	const TSharedRef<FWoTBackStackEntryHandle> Entry = FWoTBackStack::Get().Register(
		Surface, [&bSurfaceActive]() { return bSurfaceActive; }, []() { return true; });
	FWoTControllerUIInput& Input = *Scope.Input;
	auto FocusedIndex = [&Slate, &Items]()
	{
		return Items.IndexOfByKey(Slate.GetKeyboardFocusedWidget());
	};
	auto Press = [&Slate, &Shown](FKey Key, bool bRepeat = false)
	{
		const bool bHandled = Slate.ProcessKeyDownEvent(WoTUITest_Key(Key, bRepeat));
		Shown.Settle();
		return bHandled;
	};
	auto Release = [&Slate](FKey Key) { Slate.ProcessKeyUpEvent(WoTUITest_Key(Key)); };
	auto At = [&Scope, &Input, &Slate, &Shown](double Time)
	{
		*Scope.Time = Time;
		Input.TickRepeat(Slate);
		Shown.Settle();
	};

	Slate.SetKeyboardFocus(Items[0], EFocusCause::SetDirectly);
	const double Start = *Scope.Time;
	Press(EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("the press itself navigates"), FocusedIndex(), 1);
	TestTrue(TEXT("a press inside a back-stack surface arms the repeat"),
		Input.IsRepeatArmed(0));
	At(Start + FWoTControllerUIInput::FirstRepeatDelay - 0.01);
	TestEqual(TEXT("no repeat before 0.35 s"), FocusedIndex(), 1);
	const double First = Start + FWoTControllerUIInput::FirstRepeatDelay;
	At(First);
	TestEqual(TEXT("first repeat at 0.35 s"), FocusedIndex(), 2);
	const double Second = First + FWoTControllerUIInput::RepeatInterval;
	At(Second);
	TestEqual(TEXT("then every 0.08 s"), FocusedIndex(), 3);
	At(Second + FWoTControllerUIInput::RepeatInterval);
	TestEqual(TEXT("repeats run each widget's OnNavigation: the wrap link"),
		FocusedIndex(), 0);
	TestTrue(TEXT("XInput's own repeat of a held D-pad is dropped"),
		Press(EKeys::Gamepad_DPad_Down, /*bRepeat*/ true));
	TestEqual(TEXT("so it never navigates"), FocusedIndex(), 0);
	Release(EKeys::Gamepad_DPad_Down);
	TestFalse(TEXT("release cancels the repeat"), Input.IsRepeatArmed(0));
	At(*Scope.Time + 1.0);
	TestEqual(TEXT("nothing repeats after release"), FocusedIndex(), 0);

	// The back-stack top changing resets the repeat.
	Press(EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("a new press navigates"), FocusedIndex(), 1);
	{
		const TSharedRef<FWoTBackStackEntryHandle> Covering =
			FWoTBackStack::Get().Register(Other.ToSharedRef(), nullptr, nullptr);
		At(*Scope.Time + 0.01);
		TestFalse(TEXT("a new top resets the repeat"), Input.IsRepeatArmed(0));
		At(*Scope.Time + 1.0);
		TestEqual(TEXT("and nothing repeats under it"), FocusedIndex(), 1);
	}
	Release(EKeys::Gamepad_DPad_Down);

	// A press that starts outside any back-stack surface (gameplay) never
	// repeats once a surface takes focus.
	bSurfaceActive = false;
	Press(EKeys::Gamepad_DPad_Down);
	TestFalse(TEXT("a press outside a surface does not arm"), Input.IsRepeatArmed(0));
	const int32 AfterGameplayPress = FocusedIndex();
	bSurfaceActive = true;
	At(*Scope.Time + 1.0);
	TestEqual(TEXT("it does not repeat when the surface appears"),
		FocusedIndex(), AfterGameplayPress);
	TestTrue(TEXT("its native repeats are dropped inside the surface"),
		Press(EKeys::Gamepad_DPad_Down, /*bRepeat*/ true));
	TestEqual(TEXT("so the held press never moves focus"), FocusedIndex(), AfterGameplayPress);
	Release(EKeys::Gamepad_DPad_Down);

	// The stick: Slate navigates on the crossing, the repeat clock does the rest.
	Slate.SetKeyboardFocus(Items[2], EFocusCause::SetDirectly);
	Shown.Settle();
	const double StickStart = *Scope.Time;
	Slate.ProcessAnalogInputEvent(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.8f));
	Shown.Settle();
	TestEqual(TEXT("a stick crossing navigates once"), FocusedIndex(), 1);
	TestTrue(TEXT("and arms the repeat"), Input.IsRepeatArmed(0));
	for (int32 Step = 1; Step <= 4; ++Step)
	{
		*Scope.Time = StickStart + 0.08 * Step;
		Slate.ProcessAnalogInputEvent(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.8f));
		Input.TickRepeat(Slate);
		Shown.Settle();
	}
	TestEqual(TEXT("a held stick never double-repeats before 0.35 s"), FocusedIndex(), 1);
	At(StickStart + FWoTControllerUIInput::FirstRepeatDelay);
	TestEqual(TEXT("the stick repeats on the same clock"), FocusedIndex(), 0);
	Slate.ProcessAnalogInputEvent(WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.0f));
	TestFalse(TEXT("releasing the stick cancels the repeat"), Input.IsRepeatArmed(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIControllerInputMenuRepeatSpec,
	"WoT.UI.ControllerInput.RepeatReachesInGameMenu", WoTUITest_Flags)

bool FWoTUIControllerInputMenuRepeatSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	UWorld* World = WoTUITest_CreateWorld();
	if (!TestNotNull(TEXT("world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTUITest_DestroyWorld(World); };
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("controller exists"), Controller))
	{
		return false;
	}
	Controller->ToggleInGameMenu();
	const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	FWoTUITest_Window Shown(Overlay);
	FWoTUITest_InputScope Scope(Shown.Window);
	FSlateApplication& Slate = FSlateApplication::Get();
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform()),
		0.0, 0.0f);
	Shown.Settle();
	Overlay->FocusResumeButton();
	Shown.Settle();
	auto Press = [&Slate, &Shown](FKey Key)
	{
		const bool bHandled = Slate.ProcessKeyDownEvent(WoTUITest_Key(Key));
		Shown.Settle();
		return bHandled;
	};
	Press(EKeys::Gamepad_DPad_Down);
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_DPad_Down));
	TestEqual(TEXT("D-pad down selects the next entry"),
		Overlay->SelectedMainMenuIndexForTesting(), 1);
	const double Start = *Scope.Time;
	Press(EKeys::Gamepad_DPad_Up);
	TestEqual(TEXT("D-pad up returns to Resume"), Overlay->SelectedMainMenuIndexForTesting(), 0);
	TestTrue(TEXT("the menu arms the repeat"), Scope.Input->IsRepeatArmed(0));
	*Scope.Time = Start + FWoTControllerUIInput::FirstRepeatDelay;
	Scope.Input->TickRepeat(Slate);
	Shown.Settle();
	const int32 Last = Overlay->MenuLabelsForTesting().Num() - 1;
	TestEqual(TEXT("the repeat reaches the menu's OnNavigation and wraps"),
		Overlay->SelectedMainMenuIndexForTesting(), Last);
	TestTrue(TEXT("focus follows the wrapped entry"),
		Slate.GetKeyboardFocusedWidget() == Overlay->MainMenuButtonForTesting(Last));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_DPad_Up));

	// The menu's pages back out one level on B.
	Overlay->OpenPlayerConfigForTesting();
	Shown.Settle();
	TestFalse(TEXT("Player Config is open"), Overlay->IsShowingMainMenuForTesting());
	TestTrue(TEXT("B is handled on Player Config"), Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("B returns from Player Config to the menu"),
		Overlay->IsShowingMainMenuForTesting());
	TestTrue(TEXT("the menu is still open"), Controller->IsInGameMenuOpen());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackCoreSpec,
	"WoT.UI.BackStack.Core", WoTUITest_Flags)

bool FWoTUIBackStackCoreSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTBackStack& Stack = FWoTBackStack::Get();
	TSharedPtr<SButton> Lower;
	TSharedPtr<SButton> Upper;
	FWoTUITest_Window Shown(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()[SAssignNew(Lower, SButton)]
		+ SHorizontalBox::Slot().AutoWidth()[SAssignNew(Upper, SButton)]);
	Slate.SetKeyboardFocus(Lower, EFocusCause::SetDirectly);
	const int32 Baseline = Stack.NumEntries();
	int32 LowerCount = 0;
	int32 UpperCount = 0;
	bool bUpperActive = true;
	bool bUpperHandles = true;
	TSharedPtr<FWoTBackStackEntryHandle> LowerEntry = Stack.Register(
		Lower.ToSharedRef(), nullptr, [&LowerCount]() { ++LowerCount; return true; });
	TSharedPtr<FWoTBackStackEntryHandle> UpperEntry = Stack.Register(
		Upper.ToSharedRef(), [&bUpperActive]() { return bUpperActive; },
		[&UpperCount, &bUpperHandles]() { ++UpperCount; return bUpperHandles; });
	auto Press = [&Slate](FKey Key, bool bRepeat = false)
	{
		return Slate.ProcessKeyDownEvent(WoTUITest_Key(Key, bRepeat));
	};

	TestTrue(TEXT("B is handled"), Press(EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("the top active entry handles it"), UpperCount, 1);
	TestEqual(TEXT("and nothing below also runs"), LowerCount, 0);
	TestTrue(TEXT("View is handled"), Press(EKeys::Gamepad_Special_Left));
	TestEqual(TEXT("View is a back press too"), UpperCount, 2);
	TestTrue(TEXT("a held B's repeats are consumed"),
		Press(EKeys::Gamepad_FaceButton_Right, /*bRepeat*/ true));
	TestEqual(TEXT("but back nothing"), UpperCount + LowerCount, 2);
	TestFalse(TEXT("Escape keeps its own path"),
		Stack.HandleKeyDown(Slate, WoTUITest_Key(EKeys::Escape)));
	bUpperActive = false;
	Press(EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("an inactive entry is skipped"), LowerCount, 1);
	bUpperActive = true;
	bUpperHandles = false;
	TestFalse(TEXT("the first active entry's answer is final"),
		Press(EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("no fallthrough to the entry below"), LowerCount, 1);
	bUpperHandles = true;

	// Pruning: an owner destroyed without unregistering is dropped at press time.
	TestEqual(TEXT("two entries registered"), Stack.NumEntries(), Baseline + 2);
	TSharedPtr<SButton> Orphan = SNew(SButton);
	const TSharedRef<FWoTBackStackEntryHandle> OrphanEntry =
		Stack.Register(Orphan.ToSharedRef(), nullptr, nullptr);
	TestEqual(TEXT("three entries registered"), Stack.NumEntries(), Baseline + 3);
	Orphan.Reset();
	Press(EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("a dead owner's entry is pruned"), Stack.NumEntries(), Baseline + 2);
	UpperEntry.Reset();
	TestEqual(TEXT("dropping a handle removes its entry"), Stack.NumEntries(), Baseline + 1);
	Press(EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("the entry below is now the top"), LowerCount, 2);

	// Only the focused window's family is in reach.
	TSharedPtr<SButton> Elsewhere;
	FWoTUITest_Window Other(SAssignNew(Elsewhere, SButton));
	Slate.SetKeyboardFocus(Elsewhere, EFocusCause::SetDirectly);
	TestFalse(TEXT("a press in another window is not handled"),
		Press(EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("so nothing runs"), LowerCount, 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackFrontEndSpec,
	"WoT.FrontEnd.ControllerBack.Screens", WoTUITest_Flags)

bool FWoTUIBackStackFrontEndSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTUITest_FrontEnd FrontEnd(WoTUITest_Authority(), WoTUITest_MakeRoot(WoTUITest_Authority()),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	auto Screen = [&Root]() { return Root->CurrentScreenForTesting(); };

	// Main: nothing to back out of; consumed so nothing else sees it.
	const uint64 MainRebuilds = Root->ScreenRebuildCountForTesting();
	TestTrue(TEXT("B on Main is consumed"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("B on Main stays on Main"), Screen() == EWoTFrontEndScreen::Main);
	TestEqual(TEXT("B on Main rebuilds nothing"), Root->ScreenRebuildCountForTesting(),
		MainRebuilds);
	Root->ShowUpdateFailedNoticeForFocusTesting();
	TestTrue(TEXT("B on a notice is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B dismisses Main's top notice"), Root->IsUpdateFailedNoticeVisibleForTesting());

	// Play backs to Main; Player backs to Play.
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	TestTrue(TEXT("Play is open"), Screen() == EWoTFrontEndScreen::Play);
	Root->OpenPlayerConfigForTesting();
	FrontEnd.Settle();
	TestTrue(TEXT("B on Player is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("B on Player cancels to Play"), Screen() == EWoTFrontEndScreen::Play);
	TestTrue(TEXT("B on Play is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("B on Play backs to Main"), Screen() == EWoTFrontEndScreen::Main);

	// Controls: binding capture first, then the page.
	Root->ActivateMainMenuEntryForTesting(1);
	FrontEnd.Settle();
	const TSharedPtr<SWoTSettingsScreen> Controls = Root->SettingsScreenForTesting();
	if (!TestTrue(TEXT("Controls is open"), Controls.IsValid()))
	{
		return false;
	}
	Controls->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
	TestTrue(TEXT("B during a capture is handled"),
		FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B cancels the capture"), Controls->IsCapturingBinding());
	TestTrue(TEXT("and stays on Controls"), Screen() == EWoTFrontEndScreen::Controls);
	Controls->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("a gamepad key stays refused during a capture"),
		Controls->IsCapturingBinding()
		&& Controls->ErrorTextForTesting().Contains(TEXT("keyboard or mouse")));
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("B still cancels it"), Controls->IsCapturingBinding());

	// Held B backs exactly one level.
	const uint64 BeforeHeld = Root->ScreenRebuildCountForTesting();
	FSlateApplication& Slate = FSlateApplication::Get();
	TestTrue(TEXT("B backs Controls"),
		Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Right)));
	FrontEnd.Settle();
	for (int32 Repeat = 0; Repeat < 3; ++Repeat)
	{
		TestTrue(TEXT("the held repeats are consumed"), Slate.ProcessKeyDownEvent(
			WoTUITest_Key(EKeys::Gamepad_FaceButton_Right, /*bRepeat*/ true)));
		FrontEnd.Settle();
	}
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("a held B lands on Main"), Screen() == EWoTFrontEndScreen::Main);
	TestEqual(TEXT("a held B backs exactly one level"),
		Root->ScreenRebuildCountForTesting(), BeforeHeld + 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackPlayOverlaySpec,
	"WoT.FrontEnd.ControllerBack.PlayOverlays", WoTUITest_Flags)

bool FWoTUIBackStackPlayOverlaySpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	{
		// The local-network heads-up, raised by entering Play where the OS asks.
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(true);
		const TSharedRef<FWoTFrontEndPresentationAuthority> LanAuthority =
			WoTUITest_Authority();
		FWoTUITest_FrontEnd LanFrontEnd(LanAuthority, WoTUITest_MakeRoot(LanAuthority),
			FVector2D(1280.0f, 720.0f));
		LanFrontEnd.Root->ActivateMainMenuEntryForTesting(0);
		LanFrontEnd.Settle();
		if (TestTrue(TEXT("the local-network heads-up shows"),
			LanFrontEnd.Root->IsLocalNetworkHeadsUpVisibleForTesting()))
		{
			LanFrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
			TestFalse(TEXT("B takes its Not Now"),
				LanFrontEnd.Root->IsLocalNetworkHeadsUpVisibleForTesting());
			TestTrue(TEXT("still on Play"),
				LanFrontEnd.Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
		}
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	}
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority,
		WoTUITest_MakeRoot(Authority, nullptr, /*bShowDisclosure*/ true),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	FSlateApplication& Slate = FSlateApplication::Get();
	if (!TestTrue(TEXT("the disclosure covers Play"), Root->IsOnlineDisclosureVisibleForTesting()
		&& Root->IsPlayOverlayVisibleForTesting()))
	{
		return false;
	}
	const TSharedPtr<SWidget> Accept = Root->OnlineDisclosurePrimaryWidgetForTesting();
	const TSharedPtr<SWidget> Cancel = Root->OnlineDisclosureCancelWidgetForTesting();
	TestTrue(TEXT("focus moves into the overlay"), Slate.GetKeyboardFocusedWidget() == Accept);
	TestFalse(TEXT("the host panel behind it is out of reach"),
		WoTIsNavigable(Root->MapComboBoxForTesting()));
	TestFalse(TEXT("the join panel behind it is out of reach"),
		WoTIsNavigable(Root->SessionListViewForTesting()));
	TestFalse(TEXT("the footer behind it is out of reach"),
		WoTIsNavigable(Root->PlayBackButtonForTesting()));
	TestTrue(TEXT("the overlay's buttons are reachable"),
		WoTIsNavigable(Accept) && WoTIsNavigable(Cancel));
	TestTrue(TEXT("Down cannot leave the overlay"),
		WoTUITest_Navigate(Accept, EUINavigation::Down) == nullptr);
	TestTrue(TEXT("Right moves to Cancel"), WoTUITest_Navigate(Accept, EUINavigation::Right) == Cancel);
	TestTrue(TEXT("Right from Cancel wraps to Accept"),
		WoTUITest_Navigate(Cancel, EUINavigation::Right) == Accept);
	FrontEnd.Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("the D-pad cannot reach the panels behind"),
		Slate.GetKeyboardFocusedWidget() == Accept);

	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestTrue(TEXT("B on the overlay is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B takes the overlay's cancel"), Root->IsOnlineDisclosureVisibleForTesting());
	TestTrue(TEXT("and never also backs the screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	TestTrue(TEXT("the panels are back in reach"), WoTIsNavigable(Root->MapComboBoxForTesting()));
	TestTrue(TEXT("a controller lands back on the map list"),
		Slate.GetKeyboardFocusedWidget() == Root->MapComboBoxForTesting());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);

	// The keychain heads-up is an overlay too.
	Root->SetKeychainAccessForTesting(EWoTKeychainAccess::WillPrompt);
	Root->SetInternetModeForTesting();
	Root->RefreshSessionsForTesting();
	FrontEnd.Settle();
	if (TestTrue(TEXT("the keychain heads-up shows"), Root->IsKeychainHeadsUpVisibleForTesting()))
	{
		FrontEnd.Press(EKeys::Gamepad_Special_Left);
		TestFalse(TEXT("View takes the heads-up's Not Now"),
			Root->IsKeychainHeadsUpVisibleForTesting());
		TestTrue(TEXT("still on Play"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	}
	Root->SetKeychainAccessForTesting({});
	TestTrue(TEXT("a second B backs Play"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("to Main"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);

	{
		// Keyboard and mouse: when an overlay closes, focus returns to where it
		// was before the overlay took it, never left on the collapsed overlay.
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
		const TSharedRef<FWoTFrontEndPresentationAuthority> PointerAuthority =
			WoTUITest_Authority();
		FWoTUITest_FrontEnd PointerFrontEnd(PointerAuthority,
			WoTUITest_MakeRoot(PointerAuthority), FVector2D(1280.0f, 720.0f));
		const TSharedRef<SWoTFrontEndRoot>& PointerRoot = PointerFrontEnd.Root;
		PointerRoot->ActivateMainMenuEntryForTesting(0);
		PointerFrontEnd.Settle();
		const TSharedPtr<SWidget> Back = PointerRoot->PlayBackButtonForTesting();
		if (TestTrue(TEXT("Play is open with a Back button"), Back.IsValid()
			&& PointerRoot->CurrentScreenForTesting() == EWoTFrontEndScreen::Play))
		{
			Slate.SetKeyboardFocus(Back, EFocusCause::SetDirectly);
			PointerRoot->SetKeychainAccessForTesting(EWoTKeychainAccess::WillPrompt);
			PointerRoot->SetInternetModeForTesting();
			PointerRoot->RefreshSessionsForTesting();
			PointerFrontEnd.Settle();
			if (TestTrue(TEXT("the keychain heads-up shows for a keyboard player"),
				PointerRoot->IsKeychainHeadsUpVisibleForTesting()))
			{
				TestTrue(TEXT("focus moved into the heads-up"),
					Slate.GetKeyboardFocusedWidget() != Back);
				PointerRoot->CancelKeychainHeadsUpForTesting();
				PointerFrontEnd.Settle();
				TestFalse(TEXT("the heads-up closed"),
					PointerRoot->IsKeychainHeadsUpVisibleForTesting());
				TestTrue(TEXT("a keyboard player's focus is back on Back"),
					Slate.GetKeyboardFocusedWidget() == Back);
			}
			PointerRoot->SetKeychainAccessForTesting({});
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackPasswordSpec,
	"WoT.FrontEnd.ControllerBack.PasswordChallenge", WoTUITest_Flags)

bool FWoTUIBackStackPasswordSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	UWorld* World = WoTUITest_CreateWorld();
	if (!TestNotNull(TEXT("world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTUITest_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions = World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	AWoTFrontEndPlayerController* Controller =
		World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("sessions"), Sessions) || !TestNotNull(TEXT("controller"), Controller))
	{
		return false;
	}
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetServerTravelHookForTesting([](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT
	{
		Sessions->SetClientTravelControllerForTesting(nullptr);
		Sessions->SetServerTravelHookForTesting({});
	};
	Sessions->SetPasswordChallengeForTesting(EWoTPasswordChallenge::NeedPassword,
		TEXT("127.0.0.1:7777"));
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority, Controller),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	TestTrue(TEXT("the challenge opens Play"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	TestTrue(TEXT("the prompt covers the panels"), Root->IsPlayOverlayVisibleForTesting());
	TestTrue(TEXT("focus starts in its box"), Root->PasswordTextBoxForTesting().IsValid()
		&& Root->PasswordTextBoxForTesting()->HasKeyboardFocus());
	TestTrue(TEXT("B in the box is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("B cancels the challenge"),
		static_cast<int32>(Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::None));
	TestTrue(TEXT("and stays on Play"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackSettingsStripSpec,
	"WoT.FrontEnd.ControllerBack.VideoStrip", WoTUITest_Flags)

bool FWoTUIBackStackSettingsStripSpec::RunTest(const FString&)
{
	TArray<FWoTGameSettings> Projected;
	FWoTGameSettingsAdapter::SetDisplayProjectionForTesting(
		[&Projected](const FWoTGameSettings& Settings, bool, bool) { Projected.Add(Settings); });
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetDisplayProjectionForTesting({}); };
	FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting([](float) {});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting({}); };
	int32 Backs = 0;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	Authority->PrepareBrowserResources();
	const TSharedRef<SWoTSettingsScreen> Screen = SNew(SWoTSettingsScreen)
		.Page(EWoTSettingsPage::Hardware)
		.PresentationAuthority(Authority)
		.HeadingFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30))
		.BodyFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 20))
		.OnBack(FSimpleDelegate::CreateLambda([&Backs]() { ++Backs; }));
	FWoTUITest_Window Shown(Screen, FVector2D(1280.0f, 720.0f));
	FWoTUITest_InputScope Scope(Shown.Window);
	FSlateApplication& Slate = FSlateApplication::Get();
	// The window's paints tick the screen at Slate's own clock, so the
	// 15-second confirmation deadline is measured from now.
	Slate.SetKeyboardFocus(Screen->SaveButtonForTesting(), EFocusCause::SetDirectly);
	Shown.Settle();

	// Controller mode: the strip takes focus on Keep; B reverts and returns
	// focus to Save without leaving the page.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	FWoTGameSettings Draft = Screen->DraftForTesting();
	Draft.Width = static_cast<uint16>(Draft.Width == 1920 ? 1280 : 1920);
	Draft.Height = static_cast<uint16>(Draft.Height == 1080 ? 720 : 1080);
	Screen->SetDraftForTesting(Draft);
	Screen->SaveForTesting();
	Shown.Settle();
	if (!TestTrue(TEXT("Save previews and waits for Keep"),
		Screen->IsAwaitingVideoConfirmationForTesting()))
	{
		return false;
	}
	TestTrue(TEXT("the strip appears with focus on Keep"),
		Slate.GetKeyboardFocusedWidget() == Screen->KeepButtonForTesting());
	TestTrue(TEXT("Up cannot leave the strip"),
		WoTUITest_Navigate(Screen->KeepButtonForTesting(), EUINavigation::Up) == nullptr);
	TestTrue(TEXT("Right moves to Revert"),
		WoTUITest_Navigate(Screen->KeepButtonForTesting(), EUINavigation::Right)
			== Screen->RevertButtonForTesting());
	TestTrue(TEXT("B is handled on the strip"),
		Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Right)));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Right));
	Shown.Settle();
	TestFalse(TEXT("B reverts the preview"), Screen->IsAwaitingVideoConfirmationForTesting());
	TestEqual(TEXT("the confirmed display is projected back"), Projected.Num(), 2);
	TestEqual(TEXT("B on the strip does not leave the page"), Backs, 0);
	TestTrue(TEXT("focus returns to Save"),
		Slate.GetKeyboardFocusedWidget() == Screen->SaveButtonForTesting());

	// Pointer mode keeps today's focus when the strip appears.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	Screen->SetDraftForTesting(Draft);
	Screen->SaveForTesting();
	Shown.Settle();
	TestTrue(TEXT("Pointer mode leaves focus where it was"),
		Slate.GetKeyboardFocusedWidget() == Screen->SaveButtonForTesting());
	Screen->RevertVideoForTesting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIBackStackHelpAndBootstrapSpec,
	"WoT.UI.BackStack.HelpAndBootstrap", WoTUITest_Flags)

bool FWoTUIBackStackHelpAndBootstrapSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	auto Press = [&Slate](FKey Key, bool bRepeat = false)
	{
		const bool bHandled = Slate.ProcessKeyDownEvent(WoTUITest_Key(Key, bRepeat));
		Slate.ProcessKeyUpEvent(WoTUITest_Key(Key));
		return bHandled;
	};
	{
		FWoTHelpContent Content;
		Content.Title = TEXT("Help");
		Content.SubTitle = TEXT("Press a key");
		Content.Content.Add(TEXT("Line"));
		int32 Closes = 0;
		const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
			.Content(Content)
			.OnClose(FSimpleDelegate::CreateLambda([&Closes]() { ++Closes; }))
			.RequiredFontsForTesting(WoTUITest_Fonts());
		FWoTUITest_Window Shown(Help);
		Slate.SetKeyboardFocus(Help, EFocusCause::SetDirectly);
		TestTrue(TEXT("B on Help is handled"), Press(EKeys::Gamepad_FaceButton_Right));
		TestEqual(TEXT("B closes Help"), Closes, 1);
		Press(EKeys::Gamepad_FaceButton_Right, /*bRepeat*/ true);
		TestEqual(TEXT("a held B closes it once"), Closes, 1);
		Press(EKeys::Gamepad_Special_Left);
		TestEqual(TEXT("View closes Help"), Closes, 2);
		Press(EKeys::Escape);
		TestEqual(TEXT("Escape still closes Help"), Closes, 3);
	}
	{
		int32 Exits = 0;
		int32 Continues = 0;
		const TSharedRef<SWoTBootstrapScreen> Bootstrap = SNew(SWoTBootstrapScreen)
			.Snapshot(FWoTBootstrapSnapshot())
			.OnExitRequested(FSimpleDelegate::CreateLambda([&Exits]() { ++Exits; }))
			.OnContinueRequested(FSimpleDelegate::CreateLambda([&Continues]() { ++Continues; }));
		FWoTUITest_Window Shown(Bootstrap);
		TestTrue(TEXT("Pointer mode keeps today's initial focus (Quit)"),
			Bootstrap->InitialFocusWidgetForTesting() == Bootstrap->ExitWidgetForTesting());
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		TestTrue(TEXT("Controller mode starts on the first visible button"),
			Bootstrap->InitialFocusWidgetForTesting() == Bootstrap->ContinueWidgetForTesting());
		Bootstrap->FocusInitialWidget();
		TestTrue(TEXT("B on the setup screen is consumed"), Press(EKeys::Gamepad_FaceButton_Right));
		TestTrue(TEXT("View on the setup screen is consumed"), Press(EKeys::Gamepad_Special_Left));
		TestEqual(TEXT("B never quits the game"), Exits, 0);
		TestTrue(TEXT("Right wraps through the visible buttons"),
			WoTUITest_Navigate(Bootstrap->ContinueWidgetForTesting(), EUINavigation::Right)
				== Bootstrap->ExitWidgetForTesting()
			&& WoTUITest_Navigate(Bootstrap->ExitWidgetForTesting(), EUINavigation::Right)
				== Bootstrap->ContinueWidgetForTesting());
		TestTrue(TEXT("Down cannot leave the button row"),
			WoTUITest_Navigate(Bootstrap->ContinueWidgetForTesting(), EUINavigation::Down)
				== nullptr);
		Slate.SetKeyboardFocus(Bootstrap->ExitWidgetForTesting(), EFocusCause::SetDirectly);
		Press(EKeys::Escape);
		TestEqual(TEXT("Escape keeps quitting from the setup screen"), Exits, 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIIsolationSpec,
	"WoT.UI.ControllerInput.Isolation", WoTUITest_Flags)

bool FWoTUIIsolationSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	{
		// Gameplay: no back-stack surface has focus, so nothing is consumed.
		TSharedPtr<SButton> Viewport;
		FWoTUITest_Window Shown(SAssignNew(Viewport, SButton));
		FWoTUITest_InputScope Scope(Shown.Window);
		Slate.SetKeyboardFocus(Viewport, EFocusCause::SetDirectly);
		FWoTControllerUIInput& Input = *Scope.Input;
		TestFalse(TEXT("gameplay B passes through the back stack"),
			FWoTBackStack::Get().HandleKeyDown(Slate, WoTUITest_Key(EKeys::Gamepad_FaceButton_Right)));
		TestFalse(TEXT("gameplay D-pad passes through"),
			Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_DPad_Up)));
		TestFalse(TEXT("and arms no repeat"), Input.IsRepeatArmed(0));
		TestFalse(TEXT("its native repeats pass through"),
			Input.HandleKeyDownEvent(Slate, WoTUITest_Key(EKeys::Gamepad_DPad_Up, true)));
		Input.HandleKeyUpEvent(Slate, WoTUITest_Key(EKeys::Gamepad_DPad_Up));
		TestFalse(TEXT("gameplay sticks pass through"),
			Input.HandleAnalogInputEvent(Slate, WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.9f)));
		Input.HandleAnalogInputEvent(Slate, WoTUITest_Analog(EKeys::Gamepad_LeftY, 0.0f));
		for (const FKey& Key : {EKeys::T, EKeys::Y, EKeys::Enter, EKeys::Escape})
		{
			TestFalse(FString::Printf(TEXT("the chat key %s passes through"),
				*Key.ToString()), Input.HandleKeyDownEvent(Slate, WoTUITest_Key(Key)));
		}
	}
	{
		// A menu: every key the UI uses is consumed by the UI, so nothing
		// reaches the game viewport (which ignores input in UI-only mode).
		FWoTUITest_ModeGuard ModeGuard;
		FWoTUITest_FrontEnd FrontEnd(WoTUITest_Authority(),
			WoTUITest_MakeRoot(WoTUITest_Authority()), FVector2D(1280.0f, 720.0f));
		FWoTUITest_InputScope Scope(FrontEnd.Shown.Window);
		for (const FKey& Key : {EKeys::Gamepad_DPad_Down, EKeys::Gamepad_DPad_Up,
			EKeys::Gamepad_FaceButton_Right, EKeys::Gamepad_Special_Left})
		{
			TestTrue(FString::Printf(TEXT("Main consumes %s"), *Key.ToString()),
				FrontEnd.Press(Key));
		}
		TestTrue(TEXT("still on Main"),
			FrontEnd.Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
		for (const FKey& Key : {EKeys::T, EKeys::Y, EKeys::Enter})
		{
			TestFalse(FString::Printf(TEXT("the controller layer never takes %s"),
				*Key.ToString()), Scope.Input->HandleKeyDownEvent(Slate, WoTUITest_Key(Key)));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUINavigationHelpersSpec,
	"WoT.UI.Navigation.Helpers", WoTUITest_Flags)

bool FWoTUINavigationHelpersSpec::RunTest(const FString&)
{
	auto Button = []()
	{
		TSharedPtr<SButton> Result;
		SAssignNew(Result, SButton);
		return Result;
	};
	TArray<TSharedPtr<SWidget>> Vertical = {Button(), Button(), Button()};
	Vertical[1]->SetVisibility(EVisibility::Collapsed);
	TArray<TSharedPtr<SWidget>> Strip = {Button(), Button()};
	TArray<TSharedPtr<SWidget>> Left = {Button(), Button(), Button()};
	TArray<TSharedPtr<SWidget>> Right = {Button(), Button()};
	TArray<TSharedPtr<SWidget>> Footer = {Button(), Button()};
	auto Column = [](const TArray<TSharedPtr<SWidget>>& Widgets)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		for (const TSharedPtr<SWidget>& Widget : Widgets)
		{
			Box->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(40.0f)[Widget.ToSharedRef()]];
		}
		return Box;
	};
	auto Row = [](const TArray<TSharedPtr<SWidget>>& Widgets)
	{
		TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
		for (const TSharedPtr<SWidget>& Widget : Widgets)
		{
			Box->AddSlot().FillWidth(1.0f)[SNew(SBox).HeightOverride(40.0f)[Widget.ToSharedRef()]];
		}
		return Box;
	};
	FWoTUITest_Window Shown(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Column(Vertical)]
		+ SVerticalBox::Slot().AutoHeight()[Row(Strip)]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Top)[Column(Left)]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Top)[Column(Right)]
		]
		+ SVerticalBox::Slot().AutoHeight()[Row(Footer)]);
	WoTWrapVerticalNavigation(Vertical);
	WoTWrapHorizontalNavigation(Strip, /*bHoldVertical*/ true);
	TArray<FWoTNavRow> LeftRows = {{Left[0]}, {Left[1]}, {Left[2]}};
	TArray<FWoTNavRow> RightRows = {{Right[0]}, {Right[1]}};
	WoTTwoColumnNavigation(LeftRows, RightRows, Footer);
	Shown.Settle();

	TestTrue(TEXT("vertical: a collapsed entry is skipped"),
		WoTUITest_Navigate(Vertical[0], EUINavigation::Down) == Vertical[2]);
	TestTrue(TEXT("vertical: Down wraps from last to first"),
		WoTUITest_Navigate(Vertical[2], EUINavigation::Down) == Vertical[0]);
	TestTrue(TEXT("vertical: Up wraps from first to last"),
		WoTUITest_Navigate(Vertical[0], EUINavigation::Up) == Vertical[2]);
	Vertical[1]->SetVisibility(EVisibility::Visible);
	Shown.Settle();
	TestTrue(TEXT("vertical: an entry that appears joins the walk"),
		WoTUITest_Navigate(Vertical[0], EUINavigation::Down) == Vertical[1]);
	TestTrue(TEXT("strip: Right moves on"),
		WoTUITest_Navigate(Strip[0], EUINavigation::Right) == Strip[1]);
	TestTrue(TEXT("strip: Right wraps"), WoTUITest_Navigate(Strip[1], EUINavigation::Right) == Strip[0]);
	TestTrue(TEXT("strip: Up holds"), WoTUITest_Navigate(Strip[0], EUINavigation::Up) == nullptr);
	TestTrue(TEXT("two columns: Right switches at the nearest row"),
		WoTUITest_Navigate(Left[1], EUINavigation::Right) == Right[1]);
	TestTrue(TEXT("two columns: Left from the right column switches back"),
		WoTUITest_Navigate(Right[0], EUINavigation::Left) == Left[0]);
	TestTrue(TEXT("two columns: Left from the left column wraps to the right"),
		WoTUITest_Navigate(Left[0], EUINavigation::Left) == Right[0]);
	TestTrue(TEXT("two columns: Down from a column's last row reaches the footer"),
		WoTUITest_Navigate(Left[2], EUINavigation::Down) == Footer[0]);
	TestTrue(TEXT("two columns: the footer is entered nearest horizontally"),
		WoTUITest_Navigate(Right[1], EUINavigation::Down) == Footer[1]);
	TestTrue(TEXT("two columns: Down from the footer wraps to the top"),
		WoTUITest_Navigate(Footer[0], EUINavigation::Down) == Left[0]);
	TestTrue(TEXT("two columns: Up from the footer returns to the left column"),
		WoTUITest_Navigate(Footer[0], EUINavigation::Up) == Left[2]);
	TestTrue(TEXT("two columns: the footer wraps on itself"),
		WoTUITest_Navigate(Footer[1], EUINavigation::Right) == Footer[0]);
	Left[1]->SetEnabled(false);
	Shown.Settle();
	TestTrue(TEXT("a disabled control is skipped"),
		WoTUITest_Navigate(Left[0], EUINavigation::Down) == Left[2]);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIFrontEndGraphsSpec,
	"WoT.FrontEnd.ControllerFocus.Graphs", WoTUITest_Flags)

bool FWoTUIFrontEndGraphsSpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_FrontEnd FrontEnd(WoTUITest_Authority(),
		WoTUITest_MakeRoot(WoTUITest_Authority()), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	const int32 Last = SWoTFrontEndRoot::RetailMainEntries().Num() - 1;

	// Main: the arrow keys wrap for everyone (owner ruling); the update
	// banner, not interactive here, is skipped.
	Slate.SetKeyboardFocus(Root->MainMenuButtonForTesting(0), EFocusCause::SetDirectly);
	FrontEnd.Press(EKeys::Up);
	TestTrue(TEXT("Up arrow on the first entry wraps to the last"),
		Slate.GetKeyboardFocusedWidget() == Root->MainMenuButtonForTesting(Last));
	FrontEnd.Press(EKeys::Down);
	TestTrue(TEXT("Down arrow on the last entry wraps to the first"),
		Slate.GetKeyboardFocusedWidget() == Root->MainMenuButtonForTesting(0));
	FrontEnd.Press(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("the D-pad wraps the same way"),
		Slate.GetKeyboardFocusedWidget() == Root->MainMenuButtonForTesting(Last));
	TestTrue(TEXT("Main's initial focus is the selected entry in both modes"),
		Root->InitialFocusWidget() == Root->MainMenuButtonForTesting(0));

	// Play: two columns over the footer; Controller mode starts on the map list.
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	TestTrue(TEXT("Pointer mode keeps today's Play focus (Back)"),
		Root->InitialFocusWidget() == Root->PlayBackButtonForTesting());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestTrue(TEXT("Controller mode starts on the map list"),
		Root->InitialFocusWidget() == Root->MapComboBoxForTesting());
	const TArray<FWoTNavRow>& HostRows = Root->HostNavigationRowsForTesting();
	const TArray<FWoTNavRow>& JoinRows = Root->JoinNavigationRowsForTesting();
	if (TestTrue(TEXT("both columns are built"), HostRows.Num() > 4 && JoinRows.Num() > 4))
	{
		TestTrue(TEXT("the host column runs server name, then the map list"),
			WoTUITest_Navigate(HostRows[0][0], EUINavigation::Down) == Root->MapComboBoxForTesting());
		const TSharedPtr<SWidget> Switched =
			WoTUITest_Navigate(Root->MapComboBoxForTesting(), EUINavigation::Right);
		TestTrue(TEXT("Right switches to the join column"),
			JoinRows.ContainsByPredicate([&Switched](const FWoTNavRow& Row)
			{
				return Row.Contains(Switched);
			}));
		const TSharedPtr<SWidget> BelowHost =
			WoTUITest_Navigate(Root->HostMatchButtonForTesting(), EUINavigation::Down);
		TestTrue(TEXT("Down from Host Match reaches the footer"),
			BelowHost == Root->PlayBackButtonForTesting()
			|| BelowHost == Root->PlayerConfigButtonForTesting());
		TestTrue(TEXT("Right from Back reaches Player Config"),
			WoTUITest_Navigate(Root->PlayBackButtonForTesting(), EUINavigation::Right)
				== Root->PlayerConfigButtonForTesting());
		TestTrue(TEXT("Down from the footer wraps to the server name"),
			WoTUITest_Navigate(Root->PlayBackButtonForTesting(), EUINavigation::Down)
				== HostRows[0][0]);
	}

	// Player: Controller mode starts on Save; the page wraps.
	Root->OpenPlayerConfigForTesting();
	FrontEnd.Settle();
	TestTrue(TEXT("Controller mode starts Player on Save"),
		Root->InitialFocusWidget() == Root->PlayerConfigSaveButtonForTesting());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	TestTrue(TEXT("Pointer mode keeps the name box"),
		Root->InitialFocusWidget() != Root->PlayerConfigSaveButtonForTesting()
		&& Root->InitialFocusWidget().IsValid());
	TestTrue(TEXT("Down from Save wraps to the name box"),
		WoTUITest_Navigate(Root->PlayerConfigSaveButtonForTesting(), EUINavigation::Down)
			== Root->InitialFocusWidget());

	// Controls: Controller mode starts on the first control.
	FrontEnd.Press(EKeys::Escape);
	FrontEnd.Press(EKeys::Escape);
	Root->ActivateMainMenuEntryForTesting(1);
	FrontEnd.Settle();
	const TSharedPtr<SWoTSettingsScreen> Controls = Root->SettingsScreenForTesting();
	if (TestTrue(TEXT("Controls is open"), Controls.IsValid()))
	{
		TestTrue(TEXT("Pointer mode keeps the page itself"),
			Root->InitialFocusWidget() == Controls);
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		TestTrue(TEXT("Controller mode starts on the first control"),
			Root->InitialFocusWidget() == Controls->InitialControllerFocusWidget());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUISettingsWalkSpec,
	"WoT.FrontEnd.ControllerFocus.SettingsWalk", WoTUITest_Flags)

bool FWoTUISettingsWalkSpec::RunTest(const FString&)
{
	FWoTUITest_Preferences Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady))
	{
		return false;
	}
	FWoTUITest_SettingsRowsGuard RowsGuard;
	FWoTOnboardingSeams::Get().SettingsRows.Add({
		[]() { return FString(TEXT("Walk Aid Alpha")); }, []() {}});
	FWoTOnboardingSeams::Get().SettingsRows.Add({
		[]() { return FString(TEXT("Walk Aid Beta")); }, []() {}});
	FWoTUITest_ModeGuard ModeGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority,
		WoTUITest_MakeRoot(Authority, nullptr, false, Fixture.Owner), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	Root->ActivateMainMenuEntryForTesting(1);
	FrontEnd.Settle();
	const TSharedPtr<SWoTSettingsScreen> Controls = Root->SettingsScreenForTesting();
	if (!TestTrue(TEXT("Controls is open"), Controls.IsValid()))
	{
		return false;
	}
	const TArray<FWoTNavRow>& Rows = Controls->NavigationRowsForTesting();
	const TSharedPtr<SScrollBox> Body =
		StaticCastSharedPtr<SScrollBox>(Controls->BodyScrollForTesting());
	if (!TestTrue(TEXT("the page has rows and a scrolling body"), Rows.Num() > 10 && Body.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the body is one scroll-box slot per row"),
		Body->NumSlots() >= Rows.Num() - 1);
	FSlateApplication& Slate = FSlateApplication::Get();
	auto RowOf = [&Rows](const TSharedPtr<SWidget>& Widget)
	{
		return Rows.IndexOfByPredicate([&Widget](const FWoTNavRow& Row)
		{
			return Row.Contains(Widget);
		});
	};
	auto LabelOf = [](const TSharedPtr<SWidget>& Widget)
	{
		FString Label;
		TArray<TSharedPtr<SWidget>> Pending = {Widget};
		while (!Pending.IsEmpty() && Label.IsEmpty())
		{
			const TSharedPtr<SWidget> Current = Pending.Pop();
			if (Current->GetType() == FName(TEXT("STextBlock")))
			{
				Label = StaticCastSharedPtr<STextBlock>(Current)->GetText().ToString();
			}
			if (FChildren* Children = Current->GetChildren())
			{
				for (int32 Index = 0; Index < Children->Num(); ++Index)
				{
					Pending.Add(Children->GetChildAt(Index));
				}
			}
		}
		return Label;
	};
	TestEqual(TEXT("the onboarding rows close the body"),
		LabelOf(Rows[Rows.Num() - 2][0]), FString(TEXT("Walk Aid Beta")));
	TestEqual(TEXT("the first onboarding row is part of the graph"),
		LabelOf(Rows[Rows.Num() - 3][0]), FString(TEXT("Walk Aid Alpha")));

	// First to last by the D-pad at 1280x720, each row scrolled into view.
	Slate.SetKeyboardFocus(Controls->InitialControllerFocusWidget(), EFocusCause::SetDirectly);
	FrontEnd.Settle();
	bool bInOrder = true;
	bool bInView = true;
	for (int32 Expected = 1; Expected < Rows.Num(); ++Expected)
	{
		FrontEnd.Press(EKeys::Gamepad_DPad_Down);
		const TSharedPtr<SWidget> Focused = Slate.GetKeyboardFocusedWidget();
		const int32 Row = RowOf(Focused);
		if (Row != Expected)
		{
			AddInfo(FString::Printf(TEXT("step %d landed on row %d"), Expected, Row));
			bInOrder = false;
			break;
		}
		if (Expected < Rows.Num() - 1)
		{
			const FSlateRect View = Body->GetCachedGeometry().GetLayoutBoundingRect();
			const FSlateRect Control = Focused->GetCachedGeometry().GetLayoutBoundingRect();
			if (Control.Top < View.Top - 1.0f || Control.Bottom > View.Bottom + 1.0f)
			{
				AddInfo(FString::Printf(TEXT("row %d at %g-%g outside the view %g-%g"),
					Expected, Control.Top, Control.Bottom, View.Top, View.Bottom));
				bInView = false;
			}
		}
	}
	TestTrue(TEXT("the D-pad walks every row in order, onboarding rows and the footer included"),
		bInOrder);
	TestTrue(TEXT("focus scrolls each row into view"), bInView);
	FrontEnd.Press(EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("Down from the footer wraps to the first row"),
		RowOf(Slate.GetKeyboardFocusedWidget()), 0);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Controller sub-project 3, PR 2: the slider, the touch-friendly dropdown,
// focus outlines and the button prompt strip (spec r3 sections 1a, 2 and 5).

namespace
{
/** A key press and release routed through Slate, then a settle. */
bool WoTWidgetTest_Press(FWoTUITest_Window& Shown, FKey Key, bool bRepeat = false)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const bool bHandled = Slate.ProcessKeyDownEvent(WoTUITest_Key(Key, bRepeat));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(Key));
	Shown.Settle();
	return bHandled;
}

/** Controller-genesis navigation through the reply path, as the held-D-pad
 * repeat sends it (spec r3 [I1]). */
void WoTWidgetTest_ControllerNavigate(FWoTUITest_Window& Shown, EUINavigation Direction)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	if (const TSharedPtr<FSlateUser> User = Slate.GetUser(0))
	{
		const FWidgetPath FocusPath = User->GetWeakFocusPath().ToWidgetPath();
		if (FocusPath.IsValid())
		{
			Slate.ProcessExternalReply(FocusPath,
				FReply::Handled().SetNavigation(Direction, ENavigationGenesis::Controller), 0);
		}
	}
	Shown.Settle();
}

FPointerEvent WoTWidgetTest_MouseUp(const FVector2D& Position)
{
	return FPointerEvent(0, FSlateApplication::CursorPointerIndex, Position, Position,
		TSet<FKey>(), EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
}

/** A mouse move with the left button held. */
FPointerEvent WoTWidgetTest_MouseDrag(const FVector2D& Position, const FVector2D& Last)
{
	TSet<FKey> Down;
	Down.Add(EKeys::LeftMouseButton);
	return FPointerEvent(0, FSlateApplication::CursorPointerIndex, Position, Last,
		Down, EKeys::Invalid, 0.0f, FModifierKeysState());
}

FPointerEvent WoTWidgetTest_Wheel(const FVector2D& Position, float Delta)
{
	return FPointerEvent(0, FSlateApplication::CursorPointerIndex, Position, Position,
		TSet<FKey>(), EKeys::MouseWheelAxis, Delta, FModifierKeysState());
}

FPointerEvent WoTWidgetTest_TouchEnd(const FVector2D& Position)
{
	return FPointerEvent(0u, 0u, Position, Position, 0.0f, false);
}

/** The caller side of a dropdown: its committed item and every pick. */
struct FWoTWidgetTest_ComboState
{
	int32 Selected = 0;
	TArray<int32> Picks;
	TArray<ESelectInfo::Type> PickInfos;
};

TSharedRef<SWoTListCombo> WoTWidgetTest_MakeCombo(
	const TSharedRef<FWoTWidgetTest_ComboState>& State, int32 Count)
{
	return SNew(SWoTListCombo)
		.NumItems(Count)
		.SelectedIndex_Lambda([State]() { return State->Selected; })
		.OnGenerateItem_Lambda([](int32 Index) -> TSharedRef<SWidget>
		{
			return SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Item %d"), Index)));
		})
		.OnPicked_Lambda([State](int32 Index, ESelectInfo::Type SelectInfo)
		{
			State->Picks.Add(Index);
			State->PickInfos.Add(SelectInfo);
			State->Selected = Index;
		})
		[
			SNew(STextBlock).Text_Lambda([State]()
			{
				return FText::FromString(FString::Printf(TEXT("Item %d"), State->Selected));
			})
		];
}

/** Several paints: the popup is placed, then its list generates rows. */
void WoTWidgetTest_SettlePopup(FWoTUITest_Window& Shown)
{
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		Shown.Settle();
	}
}

/** A row the open list has on screen, past the first (which may be clipped),
 * other than Exclude. */
int32 WoTWidgetTest_VisibleRow(const TSharedRef<SWoTListCombo>& Combo, int32 Count,
	int32 Exclude = INDEX_NONE)
{
	int32 Seen = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<SWidget> Row = Combo->GetRowWidget(Index);
		if (Row.IsValid() && Row->GetCachedGeometry().GetLocalSize().Y > 0.0f
			&& ++Seen >= 2 && Index != Exclude)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsSliderSpec,
	"WoT.UI.Widgets.Slider", WoTUITest_Flags)

bool FWoTUIWidgetsSliderSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	float Value = 0.5f;
	TSharedPtr<SWoTRetailSlider> Slider;
	TSharedPtr<SButton> Below;
	TSharedPtr<SButton> OtherColumn;
	FWoTUITest_Window Shown(SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).WidthOverride(320.0f).HeightOverride(40.0f)
				[
					// Eleven stops: each stop is a tenth.
					SAssignNew(Slider, SWoTRetailSlider)
					.Positions(11)
					.Value_Lambda([&Value]() { return Value; })
					.OnValueChanged_Lambda([&Value](float NewValue) { Value = NewValue; })
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).WidthOverride(200.0f).HeightOverride(40.0f)[SAssignNew(Below, SButton)]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(200.0f).HeightOverride(40.0f)[SAssignNew(OtherColumn, SButton)]
		]);
	// Play's layout: Left/Right on a control switch columns (PR 1).
	const TSharedPtr<SWidget> SliderWidget = Slider;
	const TSharedPtr<SWidget> BelowWidget = Below;
	const TSharedPtr<SWidget> OtherWidget = OtherColumn;
	TArray<FWoTNavRow> LeftRows;
	LeftRows.Add(FWoTNavRow{SliderWidget});
	LeftRows.Add(FWoTNavRow{BelowWidget});
	TArray<FWoTNavRow> RightRows;
	RightRows.Add(FWoTNavRow{OtherWidget});
	WoTTwoColumnNavigation(LeftRows, RightRows, FWoTNavRow());
	FWoTUITest_InputScope Scope(Shown.Window);
	Shown.Settle();
	TestTrue(TEXT("the graph would switch columns on Right"),
		WoTUITest_Navigate(SliderWidget, EUINavigation::Right) == OtherWidget);
	auto Stop = [&Value]() { return FMath::RoundToInt(Value * 10.0f); };
	auto Focused = [&Slate]() { return Slate.GetKeyboardFocusedWidget(); };
	Slate.SetKeyboardFocus(Slider, EFocusCause::SetDirectly);

	// The D-pad steps the slider; it no longer switches columns.
	TestTrue(TEXT("D-pad Right is handled"), WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Right));
	TestEqual(TEXT("D-pad Right steps one stop"), Stop(), 6);
	TestTrue(TEXT("and focus stays on the slider"), Focused() == SliderWidget);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Left);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Left);
	TestEqual(TEXT("D-pad Left steps back one stop each"), Stop(), 4);
	TestTrue(TEXT("D-pad Left never leaves the slider"), Focused() == SliderWidget);

	// Controller-genesis Left/Right (held-D-pad repeats, the stick) step too.
	WoTWidgetTest_ControllerNavigate(Shown, EUINavigation::Right);
	TestEqual(TEXT("a Controller-genesis Right steps"), Stop(), 5);
	WoTWidgetTest_ControllerNavigate(Shown, EUINavigation::Left);
	TestEqual(TEXT("a Controller-genesis Left steps"), Stop(), 4);
	TestTrue(TEXT("repeats keep focus on the slider"), Focused() == SliderWidget);
	Slate.ProcessAnalogInputEvent(WoTUITest_Analog(EKeys::Gamepad_LeftX, 0.9f));
	Shown.Settle();
	Slate.ProcessAnalogInputEvent(WoTUITest_Analog(EKeys::Gamepad_LeftX, 0.0f));
	TestEqual(TEXT("the stick steps it"), Stop(), 5);
	TestTrue(TEXT("and keeps focus on it"), Focused() == SliderWidget);

	// The keyboard is unchanged: all four arrows, Home and End.
	WoTWidgetTest_Press(Shown, EKeys::Right);
	TestEqual(TEXT("Right arrow steps up"), Stop(), 6);
	WoTWidgetTest_Press(Shown, EKeys::Up);
	TestEqual(TEXT("Up arrow steps up"), Stop(), 7);
	WoTWidgetTest_Press(Shown, EKeys::Down);
	TestEqual(TEXT("Down arrow steps down"), Stop(), 6);
	WoTWidgetTest_Press(Shown, EKeys::Left);
	TestEqual(TEXT("Left arrow steps down"), Stop(), 5);
	WoTWidgetTest_Press(Shown, EKeys::Home);
	TestEqual(TEXT("Home goes to the first stop"), Stop(), 0);
	WoTWidgetTest_Press(Shown, EKeys::End);
	TestEqual(TEXT("End goes to the last stop"), Stop(), 10);
	TestTrue(TEXT("the keys keep focus on the slider"), Focused() == SliderWidget);

	// D-pad Up and Down move focus.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("D-pad Down moves focus to the next row"), Focused() == BelowWidget);
	TestEqual(TEXT("without changing the value"), Stop(), 10);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("D-pad Up moves focus back"), Focused() == SliderWidget);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsPlaySliderSpec,
	"WoT.FrontEnd.ControllerFocus.PlaySlider", WoTUITest_Flags)

bool FWoTUIWidgetsPlaySliderSpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTUITest_FrontEnd FrontEnd(WoTUITest_Authority(),
		WoTUITest_MakeRoot(WoTUITest_Authority()), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	FSlateApplication& Slate = FSlateApplication::Get();
	TSharedPtr<SWidget> KillSlider;
	for (const FWoTNavRow& Row : Root->HostNavigationRowsForTesting())
	{
		if (!Row.IsEmpty() && Row[0].IsValid()
			&& Row[0]->GetType() == FName(TEXT("SWoTRetailSlider")))
		{
			KillSlider = Row[0];
			break;
		}
	}
	if (!TestTrue(TEXT("the host column has a slider (the kill limit)"), KillSlider.IsValid()))
	{
		return false;
	}
	auto Kills = [&Root]() { return Root->ViewModelForTesting().GetHostRequest().FragLimit; };
	Slate.SetKeyboardFocus(KillSlider, EFocusCause::SetDirectly);
	const int32 Before = Kills();
	FrontEnd.Press(EKeys::Gamepad_DPad_Right);
	TestEqual(TEXT("D-pad Right on Play's slider steps it"), Kills(), FMath::Min(Before + 1, 100));
	TestTrue(TEXT("instead of switching to the join column"),
		Slate.GetKeyboardFocusedWidget() == KillSlider);
	FrontEnd.Press(EKeys::Gamepad_DPad_Left);
	TestEqual(TEXT("D-pad Left steps it back"), Kills(), Before);
	TestTrue(TEXT("and stays on it"), Slate.GetKeyboardFocusedWidget() == KillSlider);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsListComboKeysSpec,
	"WoT.UI.Widgets.ListComboKeys", WoTUITest_Flags)

bool FWoTUIWidgetsListComboKeysSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<FWoTWidgetTest_ComboState> State = MakeShared<FWoTWidgetTest_ComboState>();
	State->Selected = 3;
	const TSharedRef<SWoTListCombo> Combo = WoTWidgetTest_MakeCombo(State, 12);
	TSharedPtr<SButton> Above;
	TSharedPtr<SButton> Below;
	FWoTUITest_Window Shown(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).WidthOverride(300.0f).HeightOverride(40.0f)[SAssignNew(Above, SButton)]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).WidthOverride(300.0f)[Combo]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).WidthOverride(300.0f).HeightOverride(40.0f)[SAssignNew(Below, SButton)]
		]);
	const TSharedPtr<SWidget> ComboWidget = Combo;
	const TArray<TSharedPtr<SWidget>> Column = {Above, ComboWidget, Below};
	WoTWrapVerticalNavigation(Column);
	FWoTUITest_InputScope Scope(Shown.Window);
	Shown.Settle();
	const int32 Baseline = FWoTBackStack::Get().NumEntries();
	auto Focused = [&Slate]() { return Slate.GetKeyboardFocusedWidget(); };
	auto Press = [&Shown](FKey Key, bool bRepeat = false)
	{
		const bool bHandled = WoTWidgetTest_Press(Shown, Key, bRepeat);
		WoTWidgetTest_SettlePopup(Shown);
		return bHandled;
	};
	Slate.SetKeyboardFocus(Combo, EFocusCause::SetDirectly);

	// Closed, the D-pad moves focus and never the value (PR 1's release gate).
	Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("closed: D-pad Down moves focus off the list"), Focused() == Below);
	TestEqual(TEXT("closed: D-pad Down never changes the value"), State->Selected, 3);
	Press(EKeys::Gamepad_DPad_Up);
	Press(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("closed: D-pad Up moves focus off it too"), Focused() == Above);
	Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("and back onto it"), Focused() == ComboWidget);
	TestTrue(TEXT("closed: nothing was picked"), State->Picks.IsEmpty() && !Combo->IsOpen());

	// A opens it with the committed row highlighted and a back-stack entry.
	TestTrue(TEXT("A is handled"), Press(EKeys::Gamepad_FaceButton_Bottom));
	if (!TestTrue(TEXT("A opens the list"), Combo->IsOpen()))
	{
		return false;
	}
	TestTrue(TEXT("the open list takes focus"), Focused() == Combo->GetPopupWidget());
	TestEqual(TEXT("the committed row is highlighted"), Combo->GetHighlightedIndex(), 3);
	TestEqual(TEXT("an open list registers a back-stack entry"),
		FWoTBackStack::Get().NumEntries(), Baseline + 1);

	// Navigation moves the highlight, wraps, and commits nothing.
	Press(EKeys::Gamepad_DPad_Down);
	Press(EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("D-pad Down moves the highlight"), Combo->GetHighlightedIndex(), 5);
	for (int32 Step = 0; Step < 6; ++Step)
	{
		Press(EKeys::Gamepad_DPad_Up);
	}
	TestEqual(TEXT("D-pad Up wraps from the first row to the last"),
		Combo->GetHighlightedIndex(), 11);
	Press(EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("D-pad Down wraps from the last row to the first"),
		Combo->GetHighlightedIndex(), 0);
	Press(EKeys::Down);
	TestEqual(TEXT("the Down arrow moves it too"), Combo->GetHighlightedIndex(), 1);
	Press(EKeys::Up);
	Press(EKeys::Up);
	TestEqual(TEXT("the Up arrow wraps too"), Combo->GetHighlightedIndex(), 11);
	WoTWidgetTest_ControllerNavigate(Shown, EUINavigation::Up);
	TestEqual(TEXT("a held D-pad's repeat moves it"), Combo->GetHighlightedIndex(), 10);
	Press(EKeys::Gamepad_DPad_Left);
	TestTrue(TEXT("Left keeps focus in the open list"),
		Combo->IsOpen() && Focused() == Combo->GetPopupWidget());
	TestTrue(TEXT("navigation commits nothing"), State->Picks.IsEmpty() && State->Selected == 3);

	// A picks the highlighted row and closes.
	TestTrue(TEXT("A in the list is handled"), Press(EKeys::Gamepad_FaceButton_Bottom));
	TestFalse(TEXT("A closes the list"), Combo->IsOpen());
	TestEqual(TEXT("A picks the highlighted row"), State->Picks, TArray<int32>{10});
	TestTrue(TEXT("as a key press"), State->PickInfos.Num() == 1
		&& State->PickInfos[0] == ESelectInfo::OnKeyPress);
	TestTrue(TEXT("focus returns to the control"), Focused() == ComboWidget);
	TestEqual(TEXT("its back-stack entry is gone"), FWoTBackStack::Get().NumEntries(), Baseline);

	// A held A never reopens it; Enter opens and picks like A.
	Press(EKeys::Gamepad_FaceButton_Bottom, /*bRepeat*/ true);
	TestFalse(TEXT("a held A's native repeat does not reopen the list"), Combo->IsOpen());
	Press(EKeys::Enter);
	TestTrue(TEXT("Enter opens the list"), Combo->IsOpen());
	TestEqual(TEXT("on the new committed row"), Combo->GetHighlightedIndex(), 10);
	Press(EKeys::Gamepad_DPad_Down);
	Press(EKeys::Enter);
	TestEqual(TEXT("Enter picks"), State->Picks, (TArray<int32>{10, 11}));

	// B and View close without a change; the press is consumed.
	Press(EKeys::Gamepad_FaceButton_Bottom);
	Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("B on the open list is handled"), Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B closes the list"), Combo->IsOpen());
	TestEqual(TEXT("B changes nothing"), State->Selected, 11);
	TestEqual(TEXT("B picks nothing"), State->Picks.Num(), 2);
	TestTrue(TEXT("B returns focus to the control"), Focused() == ComboWidget);
	Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("View on the open list is handled"), Press(EKeys::Gamepad_Special_Left));
	TestFalse(TEXT("View closes it too"), Combo->IsOpen());

	// Escape closes only the list.
	Press(EKeys::Gamepad_FaceButton_Bottom);
	Press(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("Escape on the open list is handled"), Press(EKeys::Escape));
	TestFalse(TEXT("Escape closes the list"), Combo->IsOpen());
	TestEqual(TEXT("Escape changes nothing"), State->Selected, 11);
	TestTrue(TEXT("Escape returns focus to the control"), Focused() == ComboWidget);
	TestEqual(TEXT("no entry is left behind"), FWoTBackStack::Get().NumEntries(), Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsListComboPointerSpec,
	"WoT.UI.Widgets.ListComboPointer", WoTUITest_Flags)

bool FWoTUIWidgetsListComboPointerSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	constexpr int32 Count = 12;
	const TSharedRef<FWoTWidgetTest_ComboState> State = MakeShared<FWoTWidgetTest_ComboState>();
	const TSharedRef<SWoTListCombo> Combo = WoTWidgetTest_MakeCombo(State, Count);
	FWoTUITest_Window Shown(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
		[
			SNew(SBox).WidthOverride(300.0f)[Combo]
		]);
	auto Open = [&Slate, &Shown, &Combo]()
	{
		const FVector2D At = Shown.ScreenCenter(Combo);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		WoTWidgetTest_SettlePopup(Shown);
	};
	Open();
	if (!TestTrue(TEXT("a click opens the list"), Combo->IsOpen()))
	{
		return false;
	}
	const TSharedPtr<SWidget> Popup = Combo->GetPopupWidget();
	const TSharedPtr<SWidget> Row = Combo->GetRowWidget(1);
	if (!TestTrue(TEXT("the open list has rows on screen"), Row.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("rows are at least 48 px tall"),
		Row->GetCachedGeometry().GetAbsoluteSize().Y >= 47.5f);
	AddInfo(FString::Printf(TEXT("row height %.1f px"),
		Row->GetCachedGeometry().GetAbsoluteSize().Y));

	// A press only arms the row; a release within 8 px picks it.
	const FVector2D RowAt = Shown.ScreenCenter(Row);
	Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(RowAt));
	TestTrue(TEXT("a press picks nothing"), State->Picks.IsEmpty());
	TestEqual(TEXT("a press moves no highlight"), Combo->GetHighlightedIndex(), 0);
	TestFalse(TEXT("a press captures nothing"),
		Row->HasMouseCapture() || Popup->HasMouseCapture());
	const FVector2D Near = RowAt + FVector2D(0.0, 3.0);
	Slate.ProcessMouseMoveEvent(WoTWidgetTest_MouseDrag(Near, RowAt));
	Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(Near));
	Shown.Settle();
	TestEqual(TEXT("a release within 8 px picks the pressed row"), State->Picks,
		TArray<int32>{1});
	TestTrue(TEXT("as a click"), State->PickInfos.Num() == 1
		&& State->PickInfos[0] == ESelectInfo::OnMouseClick);
	TestFalse(TEXT("and closes the list"), Combo->IsOpen());

	// Past 8 px the press drags the list instead and picks nothing.
	Open();
	if (!TestTrue(TEXT("the list reopens"), Combo->IsOpen()))
	{
		return false;
	}
	const int32 DragRow = WoTWidgetTest_VisibleRow(Combo, Count);
	if (!TestTrue(TEXT("a row to drag from"), DragRow != INDEX_NONE))
	{
		return false;
	}
	const float OffsetBefore = Combo->GetScrollOffset();
	const FVector2D DragFrom = Shown.ScreenCenter(Combo->GetRowWidget(DragRow));
	Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(DragFrom));
	const FVector2D Small = DragFrom - FVector2D(0.0, 5.0);
	Slate.ProcessMouseMoveEvent(WoTWidgetTest_MouseDrag(Small, DragFrom));
	TestFalse(TEXT("under 8 px nothing is captured yet"), Popup->HasMouseCapture());
	const FVector2D Far = DragFrom - FVector2D(0.0, 60.0);
	Slate.ProcessMouseMoveEvent(WoTWidgetTest_MouseDrag(Far, Small));
	TestTrue(TEXT("past 8 px the list takes the drag"), Popup->HasMouseCapture());
	TestTrue(TEXT("and scrolls with the pointer"), Combo->GetScrollOffset() > OffsetBefore + 0.5f);
	Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(Far));
	WoTWidgetTest_SettlePopup(Shown);
	TestEqual(TEXT("a drag picks nothing"), State->Picks.Num(), 1);
	TestTrue(TEXT("the list stays open after a drag"), Combo->IsOpen());
	TestFalse(TEXT("the drag's capture ends with it"), Popup->HasMouseCapture());

	// The wheel scrolls.
	const float BeforeWheel = Combo->GetScrollOffset();
	Combo->Close();
	Open();
	const float Reopened = Combo->GetScrollOffset();
	Slate.ProcessMouseWheelOrGestureEvent(
		WoTWidgetTest_Wheel(Shown.ScreenCenter(Popup), -1.0f), nullptr);
	WoTWidgetTest_SettlePopup(Shown);
	AddInfo(FString::Printf(TEXT("offsets: before wheel %.2f, reopened %.2f, after %.2f"),
		BeforeWheel, Reopened, Combo->GetScrollOffset()));
	TestTrue(TEXT("the wheel scrolls the list"), Combo->GetScrollOffset() > Reopened);
	TestTrue(TEXT("the wheel picks nothing and keeps it open"),
		State->Picks.Num() == 1 && Combo->IsOpen());

	// Touch: a tap picks the row.
	const int32 TapRow = WoTWidgetTest_VisibleRow(Combo, Count);
	if (TestTrue(TEXT("a row to tap"), TapRow != INDEX_NONE))
	{
		const FVector2D TapAt = Shown.ScreenCenter(Combo->GetRowWidget(TapRow));
		Slate.ProcessTouchStartedEvent(nullptr, WoTUITest_Touch(TapAt));
		TestEqual(TEXT("a touch press picks nothing"), State->Picks.Num(), 1);
		Slate.ProcessTouchEndedEvent(WoTWidgetTest_TouchEnd(TapAt));
		Shown.Settle();
		TestTrue(TEXT("a tap picks the tapped row"),
			State->Picks.Num() == 2 && State->Picks.Last() == TapRow);
		TestFalse(TEXT("and closes the list"), Combo->IsOpen());
	}

	// Hover: the pointer draws it, a controller never does.
	Open();
	const FTableRowStyle& RowStyle =
		FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
	const int32 HoverRow =
		WoTWidgetTest_VisibleRow(Combo, Count, Combo->GetHighlightedIndex());
	if (TestTrue(TEXT("a row to hover"), HoverRow != INDEX_NONE
		&& HoverRow != Combo->GetHighlightedIndex()))
	{
		const FVector2D Over = Shown.ScreenCenter(Combo->GetRowWidget(HoverRow));
		// Checked before Slate ticks: a tick's synthetic move would hover
		// whatever lies under the real OS cursor instead.
		Slate.ProcessMouseMoveEvent(WoTUITest_MouseMove(Over, Over));
		TestTrue(TEXT("Pointer mode: the row under the pointer draws hover"),
			Combo->GetRowBrush(HoverRow) == &RowStyle.EvenRowBackgroundHoveredBrush);
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		TestTrue(TEXT("Controller mode: no row draws hover"),
			Combo->GetRowBrush(HoverRow) == &RowStyle.EvenRowBackgroundBrush);
		TestTrue(TEXT("the highlighted row still draws its highlight"),
			Combo->GetRowBrush(Combo->GetHighlightedIndex()) == &RowStyle.ActiveHoveredBrush);
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	}
	Combo->Close();
	TestFalse(TEXT("Close closes it"), Combo->IsOpen());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsSettingsDropdownSpec,
	"WoT.FrontEnd.ControllerBack.SettingsDropdown", WoTUITest_Flags)

bool FWoTUIWidgetsSettingsDropdownSpec::RunTest(const FString&)
{
	FWoTUITest_Preferences Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady))
	{
		return false;
	}
	TArray<float> Applied;
	FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting(
		[&Applied](float Brightness) { Applied.Add(Brightness); });
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority,
		WoTUITest_MakeRoot(Authority, nullptr, false, Fixture.Owner), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	FSlateApplication& Slate = FSlateApplication::Get();
	auto OpenHardware = [&Root, &FrontEnd]() -> TSharedPtr<SWoTSettingsScreen>
	{
		Root->ActivateMainMenuEntryForTesting(2);
		FrontEnd.Settle();
		return Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware
			? Root->SettingsScreenForTesting() : nullptr;
	};
	auto Press = [&FrontEnd](FKey Key)
	{
		const bool bHandled = FrontEnd.Press(Key);
		FrontEnd.Settle();
		return bHandled;
	};
	TSharedPtr<SWoTSettingsScreen> Screen = OpenHardware();
	if (!TestTrue(TEXT("Hardware is open"), Screen.IsValid()))
	{
		return false;
	}
	const TArray<FWoTNavRow>& Rows = Screen->NavigationRowsForTesting();
	if (!TestTrue(TEXT("Hardware's first row is the brightness dropdown"), Rows.Num() > 2
		&& Rows[0][0].IsValid() && Rows[0][0]->GetType() == FName(TEXT("SWoTListCombo"))))
	{
		return false;
	}
	TSharedPtr<SWoTListCombo> Brightness = StaticCastSharedPtr<SWoTListCombo>(Rows[0][0]);
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Slate.SetKeyboardFocus(Brightness, EFocusCause::SetDirectly);
	const float Before = Screen->DraftForTesting().Brightness;

	// Closed: the D-pad passes over it without applying a brightness change.
	Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("D-pad Down moves to the next setting"),
		Slate.GetKeyboardFocusedWidget() == Rows[1][0]);
	Press(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("and Up comes back"), Slate.GetKeyboardFocusedWidget() == Rows[0][0]);
	TestTrue(TEXT("passing over the closed dropdown applies nothing"),
		Applied.IsEmpty() && Screen->DraftForTesting().Brightness == Before);

	// B on the open dropdown closes only the dropdown.
	Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("A opens it"), Brightness->IsOpen());
	TestEqual(TEXT("the prompt strip shows the dropdown's set"),
		FWoTButtonPrompt::Describe(Root->ControllerPromptsForTesting()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Dropdown()));
	Press(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("moving the highlight applies nothing"), Applied.IsEmpty());
	TestTrue(TEXT("B is handled"), Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B closes the dropdown"), Brightness->IsOpen());
	TestTrue(TEXT("and stays on Hardware"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware);
	TestTrue(TEXT("with nothing applied"), Applied.IsEmpty());

	// Escape on the open dropdown closes only the dropdown; a second backs.
	Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("reopened"), Brightness->IsOpen());
	TestTrue(TEXT("Escape is handled"), Press(EKeys::Escape));
	TestFalse(TEXT("Escape closes the dropdown"), Brightness->IsOpen());
	TestTrue(TEXT("Escape on an open dropdown keeps the page"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware);
	TestTrue(TEXT("focus is back on the dropdown"),
		Slate.GetKeyboardFocusedWidget() == Rows[0][0]);
	Press(EKeys::Escape);
	TestTrue(TEXT("a second Escape backs the page"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);

	// A picks: the data model and callback are the ones the old list used.
	Screen = OpenHardware();
	if (!TestTrue(TEXT("Hardware reopens"), Screen.IsValid()))
	{
		return false;
	}
	Brightness = StaticCastSharedPtr<SWoTListCombo>(Screen->NavigationRowsForTesting()[0][0]);
	Slate.SetKeyboardFocus(Brightness, EFocusCause::SetDirectly);
	const int32 Committed = Brightness->GetHighlightedIndex();
	Press(EKeys::Gamepad_FaceButton_Bottom);
	const int32 Highlight = Brightness->GetHighlightedIndex();
	Press(EKeys::Gamepad_DPad_Down);
	Press(EKeys::Gamepad_FaceButton_Bottom);
	const int32 Expected = (Highlight + 1) % 10;
	AddInfo(FString::Printf(TEXT("committed %d, highlight %d"), Committed, Highlight));
	TestEqual(TEXT("A previews the picked brightness"), Applied.Num(), 1);
	TestTrue(TEXT("the draft takes the picked step"), FMath::IsNearlyEqual(
		Screen->DraftForTesting().Brightness, static_cast<float>(Expected) / 10.0f));
	Press(EKeys::Gamepad_FaceButton_Right);
	TestTrue(TEXT("B on the closed dropdown backs the page"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsVideoKeepSpec,
	"WoT.FrontEnd.ControllerFocus.VideoStripKeep", WoTUITest_Flags)

bool FWoTUIWidgetsVideoKeepSpec::RunTest(const FString&)
{
	FWoTUITest_Preferences Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady))
	{
		return false;
	}
	FWoTGameSettingsAdapter::SetDisplayProjectionForTesting(
		[](const FWoTGameSettings&, bool, bool) {});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetDisplayProjectionForTesting({}); };
	FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting([](float) {});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting({}); };
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	Authority->PrepareBrowserResources();
	const TSharedRef<SWoTSettingsScreen> Screen = SNew(SWoTSettingsScreen)
		.Page(EWoTSettingsPage::Hardware)
		.Preferences(Fixture.Owner)
		.PresentationAuthority(Authority)
		.HeadingFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30))
		.BodyFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 20));
	FWoTUITest_Window Shown(Screen, FVector2D(1280.0f, 720.0f));
	FWoTUITest_InputScope Scope(Shown.Window);
	FSlateApplication& Slate = FSlateApplication::Get();
	auto Focused = [&Slate]() { return Slate.GetKeyboardFocusedWidget(); };
	Slate.SetKeyboardFocus(Screen->SaveButtonForTesting(), EFocusCause::SetDirectly);
	Shown.Settle();

	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	FWoTGameSettings Draft = Screen->DraftForTesting();
	Draft.Width = static_cast<uint16>(Draft.Width == 1920 ? 1280 : 1920);
	Draft.Height = static_cast<uint16>(Draft.Height == 1080 ? 720 : 1080);
	Screen->SetDraftForTesting(Draft);
	Screen->SaveForTesting();
	Shown.Settle();
	if (!TestTrue(TEXT("Save waits for Keep"), Screen->IsAwaitingVideoConfirmationForTesting()))
	{
		return false;
	}
	TestTrue(TEXT("the strip appears with focus on Keep"), Focused() == Screen->KeepButtonForTesting());
	TestEqual(TEXT("its prompts are Keep and Revert"),
		FWoTButtonPrompt::Describe(Screen->ControllerPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::KeepRevert()));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Right);
	TestTrue(TEXT("Right moves to Revert"), Focused() == Screen->RevertButtonForTesting());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("Up cannot leave the strip"), Focused() == Screen->RevertButtonForTesting());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Left);
	TestTrue(TEXT("Left returns to Keep"), Focused() == Screen->KeepButtonForTesting());

	// A on Keep keeps the settings and focus is restored to Save.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestFalse(TEXT("A on Keep closes the strip"), Screen->IsAwaitingVideoConfirmationForTesting());
	TestEqual(TEXT("the kept width is saved"),
		static_cast<int32>(Fixture.Owner->GetGameSettings().Width), static_cast<int32>(Draft.Width));
	TestTrue(TEXT("focus is restored to Save"), Focused() == Screen->SaveButtonForTesting());
	TestEqual(TEXT("the prompts return to the page's set"),
		FWoTButtonPrompt::Describe(Screen->ControllerPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Settings()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsFocusOutlineSpec,
	"WoT.UI.Widgets.FocusOutline", WoTUITest_Flags)

bool FWoTUIWidgetsFocusOutlineSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	float SliderValue = 0.5f;
	const TSharedRef<FWoTWidgetTest_ComboState> State = MakeShared<FWoTWidgetTest_ComboState>();
	const TSharedRef<SWoTListCombo> Combo = WoTWidgetTest_MakeCombo(State, 4);
	TArray<TSharedPtr<int32>> ListItems = {MakeShared<int32>(0), MakeShared<int32>(1)};
	TSharedPtr<SWoTMenuButton> MenuButton;
	TSharedPtr<SButton> Button;
	TSharedPtr<SCheckBox> Check;
	TSharedPtr<SEditableTextBox> TextBox;
	TSharedPtr<SWoTRetailSlider> Slider;
	TSharedPtr<SListView<TSharedPtr<int32>>> List;
	TSharedPtr<SWoTFocusOutlineLayer> Layer;
	auto Row = [](const TSharedRef<SWidget>& Control)
	{
		return SNew(SBox).WidthOverride(300.0f).HeightOverride(40.0f)[Control];
	};
	FWoTUITest_Window Shown(SAssignNew(Layer, SWoTFocusOutlineLayer)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
				[Row(SAssignNew(MenuButton, SWoTMenuButton)[SNew(STextBlock).Text(FText::FromString(TEXT("Menu")))])]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)[Row(SAssignNew(Button, SButton))]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)[Row(SAssignNew(Check, SCheckBox))]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)[Row(SAssignNew(TextBox, SEditableTextBox))]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
				[Row(SAssignNew(Slider, SWoTRetailSlider)
					.Value_Lambda([&SliderValue]() { return SliderValue; }))]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)[Row(Combo)]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
				[Row(SAssignNew(List, SListView<TSharedPtr<int32>>)
					.ListItemsSource(&ListItems)
					.OnGenerateRow_Lambda([](TSharedPtr<int32> Item,
						const TSharedRef<STableViewBase>& OwnerTable) -> TSharedRef<ITableRow>
					{
						return SNew(STableRow<TSharedPtr<int32>>, OwnerTable)
							[SNew(STextBlock).Text(FText::AsNumber(Item.IsValid() ? *Item : 0))];
					}))]
		]);
	struct FCase
	{
		const TCHAR* Type;
		TSharedPtr<SWidget> Control;
	};
	const TArray<FCase> Cases = {
		{TEXT("menu button"), MenuButton},
		{TEXT("button"), Button},
		{TEXT("check box"), Check},
		{TEXT("text box"), TextBox},
		{TEXT("slider"), Slider},
		{TEXT("dropdown"), StaticCastSharedRef<SWidget>(Combo)},
		{TEXT("list"), List},
	};
	for (const FCase& Case : Cases)
	{
		Slate.SetKeyboardFocus(Case.Control, EFocusCause::Navigation);
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
		Shown.Settle();
		TestFalse(FString::Printf(TEXT("Pointer mode: no outline on the %s"), Case.Type),
			Layer->GetOutlinedWidget().IsValid());
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		Shown.Settle();
		TestTrue(FString::Printf(TEXT("Controller mode: the %s wears the outline"), Case.Type),
			Layer->GetOutlinedWidget() == Case.Control);
	}
	Slate.SetKeyboardFocus(TextBox, EFocusCause::Navigation);
	const TSharedPtr<SWidget> TextFocus = Slate.GetKeyboardFocusedWidget();
	AddInfo(FString::Printf(TEXT("text box focus lands on %s"),
		TextFocus.IsValid() ? *TextFocus->GetType().ToString() : TEXT("<none>")));
	TestTrue(TEXT("a text box's outline surrounds the box, not its inner text"),
		Layer->FindOutlineTarget(TextFocus) == TextBox);
	TestTrue(TEXT("Controller mode replaces Slate's own dashed focus rectangle"),
		Layer->OnQueryShowFocus(EFocusCause::Navigation) == TOptional<bool>(false));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	TestFalse(TEXT("Pointer mode leaves Slate's focus look as it was"),
		Layer->OnQueryShowFocus(EFocusCause::Navigation).IsSet());

	// The open dropdown draws its own focus state on the highlighted row.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Slate.SetKeyboardFocus(Combo, EFocusCause::Navigation);
	Combo->Open();
	WoTWidgetTest_SettlePopup(Shown);
	TestTrue(TEXT("the open list holds focus"),
		Combo->IsOpen() && Slate.GetKeyboardFocusedWidget() == Combo->GetPopupWidget());
	TestFalse(TEXT("the layer draws no outline around the open list"),
		Layer->GetOutlinedWidget().IsValid());
	Combo->Close();
	Shown.Settle();
	TestTrue(TEXT("closing it outlines the dropdown again"), Layer->GetOutlinedWidget() == Combo);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsPromptStripSpec,
	"WoT.UI.ButtonPrompts.Strip", WoTUITest_Flags)

bool FWoTUIWidgetsPromptStripSpec::RunTest(const FString&)
{
	FWoTUITest_ModeGuard ModeGuard;
	TArray<FWoTButtonPrompt> Bound = FWoTButtonPromptSets::Settings();
	TSharedPtr<SWoTButtonPrompts> Strip;
	FWoTUITest_Window Shown(SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(Strip, SWoTButtonPrompts)
			.Prompts_Lambda([&Bound]() { return Bound; })
		]);
	TestTrue(TEXT("Pointer mode shows no prompts"), Strip->GetShownPrompts().IsEmpty());
	TestFalse(TEXT("Pointer mode collapses the strip"), Strip->GetVisibility().IsVisible());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Shown.Settle();
	TestEqual(TEXT("Controller mode shows the bound set"),
		FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
		FString(TEXT("A Select | B Back | D-pad Left/Right Adjust")));
	TestTrue(TEXT("visible, and never in the pointer's way"),
		Strip->GetVisibility() == EVisibility::HitTestInvisible);
	TestTrue(TEXT("it takes room"), Strip->GetCachedGeometry().GetLocalSize().Y > 0.0f);
	Bound = FWoTButtonPromptSets::Dropdown();
	Shown.Settle();
	TestEqual(TEXT("it follows the window's set"),
		FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Dropdown()));
	// Every glyph draws (they are Slate shapes, not textures).
	Bound = {
		{EWoTPromptButton::A, FText::FromString(TEXT("a"))},
		{EWoTPromptButton::B, FText::FromString(TEXT("b"))},
		{EWoTPromptButton::X, FText::FromString(TEXT("x"))},
		{EWoTPromptButton::Y, FText::FromString(TEXT("y"))},
		{EWoTPromptButton::View, FText::FromString(TEXT("view"))},
		{EWoTPromptButton::Menu, FText::FromString(TEXT("menu"))},
		{EWoTPromptButton::DPadLeftRight, FText::FromString(TEXT("pad"))},
		{EWoTPromptButton::DPadUp, FText::FromString(TEXT("up"))}};
	Shown.Settle();
	TestEqual(TEXT("at most six prompts show"), Strip->GetShownPrompts().Num(),
		SWoTButtonPrompts::MaxPrompts);
	Bound = {{EWoTPromptButton::DPadUp, FText::FromString(TEXT("up"))},
		{EWoTPromptButton::Menu, FText::FromString(TEXT("menu"))}};
	Shown.Settle();
	TestEqual(TEXT("the other glyphs draw too"), Strip->GetShownPrompts().Num(), 2);
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	Shown.Settle();
	TestTrue(TEXT("back in Pointer mode the strip is gone"),
		Strip->GetShownPrompts().IsEmpty() && !Strip->GetVisibility().IsVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsPromptWindowsSpec,
	"WoT.FrontEnd.ButtonPrompts.Windows", WoTUITest_Flags)

bool FWoTUIWidgetsPromptWindowsSpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority,
		WoTUITest_MakeRoot(Authority, nullptr, /*bShowDisclosure*/ true),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	const TSharedPtr<SWoTButtonPrompts> Strip = Root->ButtonPromptsForTesting();
	if (!TestTrue(TEXT("the front end has a prompt strip"), Strip.IsValid()))
	{
		return false;
	}
	auto Expect = [this, &FrontEnd, &Strip](const TCHAR* Window,
		const TArray<FWoTButtonPrompt>& Set)
	{
		FrontEnd.Settle();
		TestEqual(FString::Printf(TEXT("%s: its prompts"), Window),
			FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
			FWoTButtonPrompt::Describe(Set));
		TestTrue(FString::Printf(TEXT("%s: the strip shows"), Window),
			Strip->GetVisibility().IsVisible());
	};
	auto ExpectHidden = [this, &FrontEnd, &Strip](const TCHAR* Window)
	{
		FrontEnd.Settle();
		TestTrue(FString::Printf(TEXT("%s, Pointer mode: no strip"), Window),
			Strip->GetShownPrompts().IsEmpty() && !Strip->GetVisibility().IsVisible());
	};

	ExpectHidden(TEXT("Main"));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Expect(TEXT("Main"), FWoTButtonPromptSets::Select());
	Root->ShowUpdateFailedNoticeForFocusTesting();
	Expect(TEXT("the update-failed notice"), FWoTButtonPromptSets::Notice());
	Root->ContinueUpdateFailedForTesting();
	FrontEnd.Settle();

	Root->ActivateMainMenuEntryForTesting(0);
	Expect(TEXT("the online disclosure"),
		FWoTButtonPromptSets::Overlay(FText::FromString(TEXT("Cancel"))));
	Root->CancelOnlineDisclosureForTesting();
	Expect(TEXT("Play"), FWoTButtonPromptSets::SelectBack());
	if (const TSharedPtr<SWoTListCombo> MapList = Root->MapListComboForTesting())
	{
		MapList->Open();
		FrontEnd.Settle();
		Expect(TEXT("the open map list"), FWoTButtonPromptSets::Dropdown());
		MapList->Close();
	}
	Root->SetKeychainAccessForTesting(EWoTKeychainAccess::WillPrompt);
	Root->SetInternetModeForTesting();
	Root->RefreshSessionsForTesting();
	FrontEnd.Settle();
	if (Root->IsKeychainHeadsUpVisibleForTesting())
	{
		Expect(TEXT("the keychain heads-up"),
			FWoTButtonPromptSets::Overlay(FText::FromString(TEXT("Not Now"))));
		Root->CancelKeychainHeadsUpForTesting();
	}
	Root->SetKeychainAccessForTesting({});
	Root->OpenPlayerConfigForTesting();
	Expect(TEXT("Player"), FWoTButtonPromptSets::SelectBack());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	ExpectHidden(TEXT("Player"));

	FrontEnd.Press(EKeys::Escape);
	FrontEnd.Press(EKeys::Escape);
	Root->ActivateMainMenuEntryForTesting(1);
	FrontEnd.Settle();
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Expect(TEXT("Controls"), FWoTButtonPromptSets::Settings());
	if (const TSharedPtr<SWoTSettingsScreen> Controls = Root->SettingsScreenForTesting())
	{
		Controls->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
		Expect(TEXT("a key capture"), FWoTButtonPromptSets::Capture());
		FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
	}
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	ExpectHidden(TEXT("Controls"));

	{
		// Help: B Close.
		FWoTHelpContent Content;
		Content.Title = TEXT("Help");
		const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
			.Content(Content)
			.RequiredFontsForTesting(WoTUITest_Fonts());
		FWoTUITest_Window Shown(Help);
		TestTrue(TEXT("Help, Pointer mode: no strip"),
			Help->ButtonPromptsForTesting()->GetShownPrompts().IsEmpty());
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		Shown.Settle();
		TestEqual(TEXT("Help: its prompts"),
			FWoTButtonPrompt::Describe(Help->ButtonPromptsForTesting()->GetShownPrompts()),
			FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Close()));
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	}
	{
		// The bootstrap screen: A Select.
		const TSharedRef<SWoTBootstrapScreen> Bootstrap = SNew(SWoTBootstrapScreen)
			.Snapshot(FWoTBootstrapSnapshot());
		FWoTUITest_Window Shown(Bootstrap);
		TestTrue(TEXT("Bootstrap, Pointer mode: no strip"),
			Bootstrap->ButtonPromptsForTesting()->GetShownPrompts().IsEmpty());
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		Shown.Settle();
		TestEqual(TEXT("Bootstrap: its prompts"),
			FWoTButtonPrompt::Describe(Bootstrap->ButtonPromptsForTesting()->GetShownPrompts()),
			FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Select()));
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsInGamePromptsSpec,
	"WoT.UI.ButtonPrompts.InGameMenu", WoTUITest_Flags)

bool FWoTUIWidgetsInGamePromptsSpec::RunTest(const FString&)
{
	FWoTUITest_ModeGuard ModeGuard;
	UWorld* World = WoTUITest_CreateWorld();
	if (!TestNotNull(TEXT("world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTUITest_DestroyWorld(World); };
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("controller exists"), Controller))
	{
		return false;
	}
	Controller->ToggleInGameMenu();
	// Real settings, so the Hardware page has its dropdowns.
	FWoTUITest_Preferences Fixture;
	const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.Preferences(Fixture.bReady ? Fixture.Owner : nullptr)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	FWoTUITest_Window Shown(Overlay);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform()),
		0.0, 0.0f);
	Shown.Settle();
	const TSharedPtr<SWoTButtonPrompts> Strip = Overlay->ButtonPromptsForTesting();
	if (!TestTrue(TEXT("the game menu has a prompt strip"), Strip.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Pointer mode: no strip"), Strip->GetShownPrompts().IsEmpty());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Shown.Settle();
	TestEqual(TEXT("the game menu: A Select, B Back"),
		FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::SelectBack()));
	Overlay->OpenSettingsForTesting(EWoTSettingsPage::Controls);
	Shown.Settle();
	TestEqual(TEXT("its settings page: the settings set"),
		FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Settings()));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	Shown.Settle();
	TestTrue(TEXT("back in Pointer mode: no strip"), Strip->GetShownPrompts().IsEmpty());

	// Escape on an open dropdown in the game menu closes only the dropdown;
	// a second Escape leaves the page as before (spec r3 [N1]).
	FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting([](float) {});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting({}); };
	Overlay->OpenSettingsForTesting(EWoTSettingsPage::Hardware);
	Shown.Settle();
	const TSharedPtr<SWoTSettingsScreen> Hardware = Overlay->SettingsScreenForTesting();
	const TSharedPtr<SWidget> First = Hardware.IsValid()
		&& !Hardware->NavigationRowsForTesting().IsEmpty()
		? Hardware->NavigationRowsForTesting()[0][0] : nullptr;
	if (!TestTrue(TEXT("the game menu's Hardware page starts with its brightness dropdown"),
		First.IsValid() && First->GetType() == FName(TEXT("SWoTListCombo"))))
	{
		return false;
	}
	const TSharedRef<SWoTListCombo> Dropdown = StaticCastSharedRef<SWoTListCombo>(First.ToSharedRef());
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.SetKeyboardFocus(Dropdown, EFocusCause::SetDirectly);
	Dropdown->Open();
	WoTWidgetTest_SettlePopup(Shown);
	TestTrue(TEXT("the game menu's dropdown opens"), Dropdown->IsOpen());
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestEqual(TEXT("the game menu's strip shows the dropdown's set"),
		FWoTButtonPrompt::Describe(Strip->GetShownPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Dropdown()));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	TestTrue(TEXT("Escape is handled"), WoTWidgetTest_Press(Shown, EKeys::Escape));
	TestFalse(TEXT("Escape closes the dropdown"), Dropdown->IsOpen());
	TestTrue(TEXT("and stays on the page"), Overlay->SettingsScreenForTesting().IsValid());
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestTrue(TEXT("a second Escape leaves the page"), Overlay->IsShowingMainMenuForTesting());
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUIWidgetsPromptNoLayoutShiftSpec,
	"WoT.FrontEnd.ButtonPrompts.NoLayoutShift", WoTUITest_Flags)

bool FWoTUIWidgetsPromptNoLayoutShiftSpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	// Root -> outline layer -> overlay -> the screens' content host (slot 0).
	FChildren* RootChildren = Root->GetChildren();
	FChildren* LayerChildren = RootChildren && RootChildren->Num() == 1
		? RootChildren->GetChildAt(0)->GetChildren() : nullptr;
	FChildren* OverlayChildren = LayerChildren && LayerChildren->Num() == 1
		? LayerChildren->GetChildAt(0)->GetChildren() : nullptr;
	if (!TestTrue(TEXT("the front end has its content host"),
		OverlayChildren != nullptr && OverlayChildren->Num() == 2))
	{
		return false;
	}
	const TSharedRef<SWidget> Host = OverlayChildren->GetChildAt(0);
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	FrontEnd.Settle();
	const FVector2D PointerSize = Host->GetCachedGeometry().GetLocalSize();
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	FrontEnd.Settle();
	TestFalse(TEXT("Controller mode shows Main's prompts"),
		Root->ButtonPromptsForTesting()->GetShownPrompts().IsEmpty());
	const FVector2D ControllerSize = Host->GetCachedGeometry().GetLocalSize();
	TestTrue(TEXT("the content host has a size"), PointerSize.Y > 0.0);
	TestEqual(TEXT("showing the prompt strip never resizes the screens"),
		ControllerSize, PointerSize);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
