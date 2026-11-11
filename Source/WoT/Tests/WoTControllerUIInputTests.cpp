// Automation specs for controller sub-project 3, PR 1: the controller UI input
// foundation (last input, cursor lease, navigation repeat, back stack, focus
// graphs, initial focus). Spec r3 sections 1, 1a and Testing. Every spec runs
// headless through injected seams: a recording cursor policy and a fake clock.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Gamepad/WoTGamepadLayer.h"
#include "../UI/SWoTBootstrapScreen.h"
#include "../UI/SWoTButtonPrompts.h"
#include "../UI/SWoTFocusOutline.h"
#include "../UI/SWoTListCombo.h"
#include "../UI/WoTBackStack.h"
#include "../UI/WoTControllerUIInput.h"
#include "../UI/WoTMenuNavigation.h"
#include "../UI/SWoTOnScreenKeyboard.h"
#include "../UI/SWoTTextFieldActivator.h"
#include "../UI/WoTTextEntry.h"
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
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "WoTTestWorld.h"
#include "WoTTestBattleClasses.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerState.h"
#include "WoTBootstrapCore/NativeHandles.h"

#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Input/HittestGrid.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Types/PaintArgs.h"
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
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "Misc/FileHelper.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

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
	// B on the closed dropdown is the page's back action: with the picked
	// brightness unsaved it asks first (the owner's Steam Deck session,
	// 2026-11-09), and Discard backs the page.
	Press(EKeys::Gamepad_FaceButton_Right);
	TestTrue(TEXT("B on the closed dropdown asks before dropping the pick"),
		Screen->IsConfirmingUnsaved()
		&& Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware);
	Press(EKeys::Gamepad_DPad_Right);
	Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("and Discard backs the page"),
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

// ---------------------------------------------------------------------------
// Controller sub-project 3, PR 3: text entry (spec r3 section 3): the QWERTY
// panel, sessions, the Steam route through its seams, the side-file toggle,
// the eight fields and chat.

namespace
{
/** No session and no seam outlives a spec; Wine and SteamDeck read native. */
struct FWoTEntryTest_Guard
{
	FWoTEntryTest_Guard()
	{
		FWoTTextEntry::ResetForTesting();
		WoTSetRunningUnderWineForTesting(false);
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
	}
	~FWoTEntryTest_Guard()
	{
		FWoTTextEntry::ResetForTesting();
		WoTSetRunningUnderWineForTesting(TOptional<bool>());
	}
};

/** The Deck with the player's toggle on and a launcher that records. */
struct FWoTEntryTest_Steam
{
	TArray<FString> Launched;
	FString LaunchError;

	FWoTEntryTest_Steam()
	{
		WoTSetRunningUnderWineForTesting(true);
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(true);
		FWoTTextEntry::SetSteamKeyboardEnabledForTesting(true);
		FWoTTextEntry::SetLauncherForTesting([this](const FString& Url, FString& OutError)
		{
			Launched.Add(Url);
			OutError = LaunchError;
			return LaunchError.IsEmpty();
		});
	}
	~FWoTEntryTest_Steam()
	{
		FWoTTextEntry::SetLauncherForTesting(nullptr);
		FWoTTextEntry::SetSteamKeyboardEnabledForTesting(TOptional<bool>());
		FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
		WoTSetRunningUnderWineForTesting(false);
	}
};

/** One field in a layer, with a request that records what it is asked to do. */
struct FWoTEntryTest_Field
{
	FString Text = TEXT("abc");
	TArray<FString> Calls;
	int32 OtherClicks = 0;
	TSharedPtr<SEditableTextBox> Box;
	TSharedPtr<SButton> Other;
	TSharedPtr<SWoTTextFieldActivator> Activator;
	TSharedPtr<SWoTTextEntryLayer> Layer;

	TSharedRef<SWidget> Build()
	{
		FWoTTextEntryRequest Request;
		Request.Title = FText::FromString(TEXT("Name"));
		Request.MaxLength = 8;
		Request.GetText = [this]() { return Text; };
		Request.SetText = [this](const FString& Value)
		{
			Text = Value;
			Calls.Add(TEXT("Set:") + Value);
		};
		Request.Validate = [](const FString& Value)
		{
			return Value.Contains(TEXT(" "))
				? FWoTTextValidation::Reject(FText::FromString(TEXT("No spaces.")))
				: FWoTTextValidation::Accept();
		};
		Request.Commit = [this]() { Calls.Add(TEXT("Commit")); };
		Request.Cancel = [this]() { Calls.Add(TEXT("Cancel")); };
		return SAssignNew(Layer, SWoTTextEntryLayer)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
				[
					SNew(SBox).WidthOverride(300.0f)
					[
						SAssignNew(Activator, SWoTTextFieldActivator)
						.Request(Request)
						[
							SAssignNew(Box, SEditableTextBox)
							// Bound and stored live, as the host fields are.
							.Text_Lambda([this]() { return FText::FromString(Text); })
							.OnTextChanged_Lambda([this](const FText& Value) { Text = Value.ToString(); })
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
				[
					SNew(SBox).WidthOverride(200.0f).HeightOverride(40.0f)
					[
						SAssignNew(Other, SButton)
						.OnClicked_Lambda([this]() { ++OtherClicks; return FReply::Handled(); })
					]
				]
			];
	}
};

/** Physical typing: the key and its character, as the OS delivers them. */
void WoTEntryTest_Type(FWoTUITest_Window& Shown, const FString& Text)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	for (const TCHAR Character : Text)
	{
		Slate.ProcessKeyCharEvent(FCharacterEvent(Character, FModifierKeysState(), 0, false));
	}
	Shown.Settle();
}

FString WoTEntryTest_Prompts(const TArray<FWoTButtonPrompt>& Prompts)
{
	return FWoTButtonPrompt::Describe(Prompts);
}

/** Starts entry on a front-end field the way a controller does: focus, A. */
TSharedPtr<SWoTOnScreenKeyboard> WoTEntryTest_BeginOn(FWoTUITest_FrontEnd& FrontEnd,
	const TSharedPtr<SWidget>& Field)
{
	if (!Field.IsValid())
	{
		return nullptr;
	}
	FSlateApplication::Get().SetKeyboardFocus(Field, EFocusCause::SetDirectly);
	FrontEnd.Settle();
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Bottom);
	return FWoTTextEntry::GetPanel();
}

FString WoTEntryTest_BoxText(const TSharedPtr<SWidget>& Field)
{
	return Field.IsValid() && Field->GetType() == FName(TEXT("SEditableTextBox"))
		? StaticCastSharedPtr<SEditableTextBox>(Field)->GetText().ToString() : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUITextEntryPanelSpec,
	"WoT.UI.TextEntry.Panel", WoTUITest_Flags)

bool FWoTUITextEntryPanelSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	int32 Dones = 0;
	int32 Cancels = 0;
	TSharedPtr<SWoTOnScreenKeyboard> Panel;
	FWoTUITest_Window Shown(SAssignNew(Panel, SWoTOnScreenKeyboard)
		.Title(FText::FromString(TEXT("Name")))
		.MaxLength(6)
		.OnDone_Lambda([&Dones]() { ++Dones; })
		.OnCancel_Lambda([&Cancels]() { ++Cancels; }));
	Slate.SetKeyboardFocus(Panel, EFocusCause::SetDirectly);
	Shown.Settle();
	TestEqual(TEXT("the panel opens on q (spec r3 section 1a)"), Panel->GetHighlightedLabel(),
		FString(TEXT("q")));

	// Every button (the owner's Steam Deck ruling, 2026-11-09): A types the
	// highlighted key, Y is Space, X is Backspace, Menu is Done, B cancels and
	// View does nothing. Shift has no button: it is the on-screen Shift key.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A types the highlighted key"), Panel->GetDraft(), FString(TEXT("q")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Right);
	TestEqual(TEXT("the D-pad moves the highlight"), Panel->GetHighlightedLabel(), FString(TEXT("w")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Top);
	TestEqual(TEXT("Y is Space"), Panel->GetDraft(), FString(TEXT("q ")));
	TestFalse(TEXT("Y never toggles Shift"), Panel->IsShifted());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Left);
	TestEqual(TEXT("X is Backspace"), Panel->GetDraft(), FString(TEXT("q")));
	const FIntPoint ShiftKey = SWoTOnScreenKeyboard::FindKey(TEXT("Shift"));
	Panel->SetHighlight(ShiftKey.Y, ShiftKey.X);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("A on the Shift key turns Shift on"), Panel->IsShifted());
	Panel->SetHighlight(1, 1);
	TestEqual(TEXT("Shift shows capitals"), Panel->GetHighlightedLabel(), FString(TEXT("W")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("a capital was typed"), Panel->GetDraft(), FString(TEXT("qW")));
	Panel->SetHighlight(ShiftKey.Y, ShiftKey.X);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestFalse(TEXT("A on the Shift key turns Shift off"), Panel->IsShifted());
	Panel->SetHighlight(1, 1);
	for (int32 Press = 0; Press < 6; ++Press)
	{
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	}
	TestEqual(TEXT("MaxLength stops the draft"), Panel->GetDraft(), FString(TEXT("qWwwww")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Right);
	TestEqual(TEXT("Menu is Done"), Dones, 1);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("B cancels"), Cancels, 1);
	TestTrue(TEXT("View is handled"), WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Left));
	TestTrue(TEXT("and does nothing"), Dones == 1 && Cancels == 1
		&& Panel->GetDraft() == TEXT("qWwwww") && !Panel->IsShifted());
	TestEqual(TEXT("the panel's prompts name that map, without View"),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::KeyboardPanel()),
		FString(TEXT("A Type | B Cancel | X Backspace | Y Space | Menu Done")));

	// The grid wraps; Up and Down land on the nearest key of the next row.
	Panel->SetHighlight(1, 0);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Left);
	TestEqual(TEXT("Left from q wraps to p"), Panel->GetHighlightedLabel(), FString(TEXT("p")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("Down from p lands on l"), Panel->GetHighlightedLabel(), FString(TEXT("l")));
	Panel->SetHighlight(0, 0);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Up);
	TestEqual(TEXT("Up from 1 wraps to the bottom row's Shift"), Panel->GetHighlightedLabel(),
		FString(TEXT("Shift")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("Down from the bottom row wraps to the top"), Panel->GetHighlightedLabel(),
		FString(TEXT("1")));
	const FIntPoint Done = SWoTOnScreenKeyboard::FindKey(TEXT("Done"));
	Panel->SetHighlight(3, 10);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestEqual(TEXT("Down from : lands on Done"), Panel->GetHighlight(), Done);
	TestEqual(TEXT("the rows are 1-0, q-p, a-l, z-m . - _ :, and the controls"),
		SWoTOnScreenKeyboard::FindKey(TEXT(":")), FIntPoint(10, 3));
	Panel->SetHighlight(Done.Y, Done.X);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A on Done is Done"), Dones, 2);

	// Physical typing edits the draft; Backspace, Enter and Escape work.
	Panel->SetDraft(FString());
	WoTEntryTest_Type(Shown, TEXT("Hi!"));
	TestEqual(TEXT("physical typing edits the draft"), Panel->GetDraft(), FString(TEXT("Hi!")));
	Slate.ProcessKeyCharEvent(FCharacterEvent(TCHAR(8), FModifierKeysState(), 0, false));
	TestEqual(TEXT("a control character types nothing"), Panel->GetDraft(), FString(TEXT("Hi!")));
	WoTWidgetTest_Press(Shown, EKeys::BackSpace);
	TestEqual(TEXT("the Backspace key deletes"), Panel->GetDraft(), FString(TEXT("Hi")));
	WoTWidgetTest_Press(Shown, EKeys::Enter);
	TestEqual(TEXT("Enter is Done"), Dones, 3);
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestEqual(TEXT("Escape cancels"), Cancels, 2);
	WoTWidgetTest_Press(Shown, EKeys::Left);
	TestEqual(TEXT("the arrows move the highlight too"), Panel->GetHighlightedLabel(),
		FString(TEXT("Backspace")));

	// Taps and clicks type the key under them.
	const FIntPoint E = SWoTOnScreenKeyboard::FindKey(TEXT("e"));
	const TSharedPtr<SWidget> EKey = Panel->GetKeyWidget(E.Y, E.X);
	if (TestTrue(TEXT("the e key is built"), EKey.IsValid()))
	{
		const FVector2D At = Shown.ScreenCenter(EKey);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		Shown.Settle();
		TestEqual(TEXT("a click types the key"), Panel->GetDraft(), FString(TEXT("Hie")));
		TestTrue(TEXT("and the panel keeps focus"), Panel->HasKeyboardFocus());
		Slate.ProcessTouchStartedEvent(nullptr, WoTUITest_Touch(At));
		Slate.ProcessTouchEndedEvent(WoTWidgetTest_TouchEnd(At));
		Shown.Settle();
		TestEqual(TEXT("a tap types the key"), Panel->GetDraft(), FString(TEXT("Hiee")));
	}

	// Passwords are masked in the preview.
	const TSharedRef<SWoTOnScreenKeyboard> Masked = SNew(SWoTOnScreenKeyboard)
		.IsPassword(true).InitialText(TEXT("abc"));
	TestEqual(TEXT("a password's preview is masked"), Masked->GetPreviewText(), FString(TEXT("***")));
	TestEqual(TEXT("its draft is not"), Masked->GetDraft(), FString(TEXT("abc")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUITextEntryPanelRepeatSpec,
	"WoT.UI.TextEntry.PanelRepeat", WoTUITest_Flags)

bool FWoTUITextEntryPanelRepeatSpec::RunTest(const FString&)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTEntryTest_Guard EntryGuard;
	TSharedPtr<SWoTOnScreenKeyboard> Panel;
	FWoTUITest_Window Shown(SAssignNew(Panel, SWoTOnScreenKeyboard));
	FWoTUITest_InputScope Scope(Shown.Window);
	Slate.SetKeyboardFocus(Panel, EFocusCause::SetDirectly);
	Shown.Settle();

	// A held A types repeatedly on the navigation clock; XInput's own repeats
	// are ignored [N5].
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	Shown.Settle();
	TestEqual(TEXT("the press types once"), Panel->GetDraft(), FString(TEXT("q")));
	for (int32 Repeat = 0; Repeat < 3; ++Repeat)
	{
		Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom, true));
	}
	Shown.Settle();
	TestEqual(TEXT("XInput's native repeats are ignored"), Panel->GetDraft(), FString(TEXT("q")));
	*Scope.Time += 0.34;
	Shown.Settle();
	TestEqual(TEXT("no repeat before 0.35 s"), Panel->GetDraft(), FString(TEXT("q")));
	*Scope.Time += 0.02;
	Shown.Settle();
	TestEqual(TEXT("the first repeat at 0.35 s"), Panel->GetDraft(), FString(TEXT("qq")));
	*Scope.Time += 0.08;
	Shown.Settle();
	TestEqual(TEXT("then every 0.08 s"), Panel->GetDraft(), FString(TEXT("qqq")));
	*Scope.Time += 0.08;
	Shown.Settle();
	TestEqual(TEXT("and again"), Panel->GetDraft(), FString(TEXT("qqqq")));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	*Scope.Time += 1.0;
	Shown.Settle();
	TestEqual(TEXT("the release ends the repeat"), Panel->GetDraft(), FString(TEXT("qqqq")));
	TestFalse(TEXT("nothing is held"), Panel->IsRepeating());

	// A held X (Backspace) deletes repeatedly the same way.
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Left));
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Left, true));
	Shown.Settle();
	TestEqual(TEXT("X deletes once, its native repeat nothing"), Panel->GetDraft(),
		FString(TEXT("qqq")));
	*Scope.Time += 0.36;
	Shown.Settle();
	TestEqual(TEXT("a held X repeats at 0.35 s"), Panel->GetDraft(), FString(TEXT("qq")));
	*Scope.Time += 0.08;
	Shown.Settle();
	TestEqual(TEXT("and every 0.08 s"), Panel->GetDraft(), FString(TEXT("q")));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Left));
	*Scope.Time += 1.0;
	Shown.Settle();
	TestEqual(TEXT("its release ends it"), Panel->GetDraft(), FString(TEXT("q")));

	// A held Y (Space) types one space; it never repeats.
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Top));
	*Scope.Time += 1.0;
	Shown.Settle();
	TestEqual(TEXT("a held Y types one space"), Panel->GetDraft(), FString(TEXT("q ")));
	TestFalse(TEXT("and arms no repeat"), Panel->IsRepeating());
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Top));
	Shown.Settle();

	// A held A on Shift or Done never repeats it.
	const FIntPoint Shift = SWoTOnScreenKeyboard::FindKey(TEXT("Shift"));
	Panel->SetHighlight(Shift.Y, Shift.X);
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	*Scope.Time += 1.0;
	Shown.Settle();
	TestTrue(TEXT("Shift toggles once for a held A"), Panel->IsShifted());
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_FaceButton_Bottom));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUITextEntrySessionSpec,
	"WoT.UI.TextEntry.Session", WoTUITest_Flags)

bool FWoTUITextEntrySessionSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTEntryTest_Guard EntryGuard;
	FWoTEntryTest_Field Field;
	FWoTUITest_Window Shown(Field.Build());
	FWoTUITest_InputScope Scope(Shown.Window);
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	Shown.Settle();
	TestFalse(TEXT("no entry at rest"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("the panel is the route off the Deck"), FWoTTextEntry::ChooseRoute(),
		EWoTTextEntryRoute::Panel);

	// A on the focused field starts entry with the panel.
	TestTrue(TEXT("A on the field is handled"),
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom));
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("A starts entry with the panel"), FWoTTextEntry::IsActive() && Panel.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the layer shows it"), Field.Layer->IsPanelShown());
	TestTrue(TEXT("the panel takes focus"), Panel->HasKeyboardFocus());
	TestEqual(TEXT("the draft starts from the field's text"), Panel->GetDraft(), FString(TEXT("abc")));
	TestTrue(TEXT("the entry is active for the input mode"), FWoTControllerUIInput::IsTextEntryActive());
	TestEqual(TEXT("A is Controller input"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Controller);
	TestTrue(TEXT("the entry sits on the back stack"),
		FWoTBackStack::Get().FindHandlerId(Slate, 0) != 0);
	TestEqual(TEXT("the panel's MaxLength is the request's"), Panel->GetMaxLength(), 8);

	// Keyboard keys during an entry never flip the input mode [I8].
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::D));
	WoTEntryTest_Type(Shown, TEXT("d"));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::D));
	TestEqual(TEXT("physical typing edits the draft"), Panel->GetDraft(), FString(TEXT("abcd")));
	TestEqual(TEXT("and keeps Controller mode"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Controller);
	TestEqual(TEXT("the field is untouched while the panel edits"), Field.Text, FString(TEXT("abc")));

	// X on the panel is Backspace; View does nothing and is never a back.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Left);
	TestEqual(TEXT("X is Backspace"), Panel->GetDraft(), FString(TEXT("abc")));
	TestTrue(TEXT("and the entry stays"), FWoTTextEntry::IsActive());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Left);
	TestTrue(TEXT("View leaves the entry open"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("and its draft as it was"), Panel->GetDraft(), FString(TEXT("abc")));

	// Done runs Validate: Reject keeps the panel open with the message.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Top);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Right);
	TestTrue(TEXT("a rejected Done keeps the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("and shows the rule"), Panel->GetMessage().ToString(), FString(TEXT("No spaces.")));
	TestTrue(TEXT("nothing is set or committed"), Field.Calls.IsEmpty());
	TestEqual(TEXT("the field keeps its text"), Field.Text, FString(TEXT("abc")));

	// Accept sets the text and commits.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Left);
	WoTEntryTest_Type(Shown, TEXT("e"));
	TestTrue(TEXT("an edit clears the message"), Panel->GetMessage().IsEmpty());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Right);
	TestFalse(TEXT("Done ends the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("Done sets the text, then commits"), Field.Calls,
		TArray<FString>{TEXT("Set:abce"), TEXT("Commit")});
	TestEqual(TEXT("the field has the text"), Field.Text, FString(TEXT("abce")));
	TestFalse(TEXT("the panel closes"), Field.Layer->IsPanelShown());
	TestTrue(TEXT("focus returns to the field"), Field.Box->HasKeyboardFocus());
	TestFalse(TEXT("the input mode is free again"), FWoTControllerUIInput::IsTextEntryActive());
	TestTrue(TEXT("the entry left the back stack"), FWoTBackStack::Get().FindHandlerId(Slate, 0) == 0);

	// Cancel restores the snapshot and reports the cancel.
	Field.Calls.Reset();
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	WoTEntryTest_Type(Shown, TEXT("zz"));
	TestTrue(TEXT("B on the panel is handled"),
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B cancels the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("a cancel keeps the field's text"), Field.Text, FString(TEXT("abce")));
	TestEqual(TEXT("and only reports the cancel"), Field.Calls, TArray<FString>{TEXT("Cancel")});

	// Escape cancels the panel first.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestFalse(TEXT("Escape cancels the panel"), FWoTTextEntry::IsActive());

	// A press outside the panel cancels; nothing behind the backdrop is hit.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	{
		const FVector2D At = Shown.ScreenCenter(Field.Other);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		Shown.Settle();
	}
	TestFalse(TEXT("a press outside the panel cancels it"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("and never reaches the control behind"), Field.OtherClicks, 0);

	// Focus moving to somewhere other than the field or the panel ends the
	// entry without a commit [N2]; focus going nowhere does not.
	Field.Calls.Reset();
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	Slate.ClearKeyboardFocus(EFocusCause::SetDirectly);
	Shown.Settle();
	TestTrue(TEXT("focus going nowhere keeps the entry"), FWoTTextEntry::IsActive());
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	Shown.Settle();
	TestTrue(TEXT("focus on the field keeps the entry"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and returns to the panel, which takes the typing"),
		FWoTTextEntry::GetPanel().IsValid() && FWoTTextEntry::GetPanel()->HasKeyboardFocus());
	Slate.SetKeyboardFocus(Field.Other, EFocusCause::SetDirectly);
	Shown.Settle();
	TestFalse(TEXT("focus on another control ends the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("without a commit"), Field.Calls, TArray<FString>{TEXT("Cancel")});

	// The field going away ends the entry.
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	Field.Activator->SetVisibility(EVisibility::Collapsed);
	Shown.Settle();
	TestFalse(TEXT("a hidden field ends the entry"), FWoTTextEntry::IsActive());
	Field.Activator->SetVisibility(EVisibility::Visible);
	Shown.Settle();
	TestFalse(TEXT("no entry outlives its spec"), FWoTControllerUIInput::IsTextEntryActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUITextEntrySteamSpec,
	"WoT.UI.TextEntry.SteamRoute", WoTUITest_Flags)

bool FWoTUITextEntrySteamSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTEntryTest_Guard EntryGuard;
	FWoTEntryTest_Steam Steam;

	// The route: Steam's keyboard only under Wine with SteamDeck=1 and the
	// toggle on.
	TestEqual(TEXT("the Deck with the toggle on uses Steam's keyboard"),
		FWoTTextEntry::ChooseRoute(), EWoTTextEntryRoute::SteamKeyboard);
	WoTSetRunningUnderWineForTesting(false);
	TestEqual(TEXT("native Windows uses the panel"), FWoTTextEntry::ChooseRoute(),
		EWoTTextEntryRoute::Panel);
	WoTSetRunningUnderWineForTesting(true);
	FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
	TestEqual(TEXT("Wine without SteamDeck=1 uses the panel"), FWoTTextEntry::ChooseRoute(),
		EWoTTextEntryRoute::Panel);
	FWoTTextEntry::SetSteamDeckEnvironmentForTesting(true);
	FWoTTextEntry::SetSteamKeyboardEnabledForTesting(false);
	TestEqual(TEXT("the toggle off uses the panel"), FWoTTextEntry::ChooseRoute(),
		EWoTTextEntryRoute::Panel);
	FWoTTextEntry::SetSteamKeyboardEnabledForTesting(true);

	FWoTEntryTest_Field Field;
	FWoTUITest_Window Shown(Field.Build());
	FWoTUITest_InputScope Scope(Shown.Window);
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	Shown.Settle();

	// A opens Steam's keyboard through the launcher seam; the field keeps focus.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("A opens Steam's keyboard"), Steam.Launched,
		TArray<FString>{TEXT("steam://open/keyboard")});
	TestTrue(TEXT("on Steam's route"),
		FWoTTextEntry::GetActiveRoute() == TOptional<EWoTTextEntryRoute>(EWoTTextEntryRoute::SteamKeyboard));
	TestFalse(TEXT("no panel shows"), Field.Layer->IsPanelShown());
	TestTrue(TEXT("the field keeps focus for Steam's keys"), Field.Box->HasKeyboardFocus());
	TestEqual(TEXT("the prompts offer the panel and Cancel"),
		WoTEntryTest_Prompts(FWoTTextEntry::ControllerPrompts()),
		WoTEntryTest_Prompts(FWoTButtonPromptSets::SteamKeyboard()));

	// Steam's typing edits the live field and never flips the input mode.
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::X));
	WoTEntryTest_Type(Shown, TEXT("x"));
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::X));
	TestEqual(TEXT("Steam's keys type into the field"), Field.Text, FString(TEXT("abcx")));
	TestEqual(TEXT("Steam's typing keeps Controller mode"), FWoTControllerUIInput::GetLastInput(),
		EWoTLastInput::Controller);

	// Steam's Enter is Done.
	WoTWidgetTest_Press(Shown, EKeys::Enter);
	TestFalse(TEXT("Steam's Enter ends the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("and commits"), Field.Calls, TArray<FString>{TEXT("Commit")});
	TestEqual(TEXT("with Steam's text"), Field.Text, FString(TEXT("abcx")));

	// B cancels the entry and restores the snapshot, undoing Steam's typing.
	Field.Calls.Reset();
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	WoTEntryTest_Type(Shown, TEXT("y"));
	TestEqual(TEXT("Steam typed again"), Field.Text, FString(TEXT("abcxy")));
	TestTrue(TEXT("B in an active entry is handled"),
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B cancels the entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("cancel restores the snapshot"), Field.Text, FString(TEXT("abcx")));
	TestEqual(TEXT("and reports the cancel"),
		Field.Calls.IsEmpty() ? FString() : Field.Calls.Last(), FString(TEXT("Cancel")));
	TestEqual(TEXT("the box shows the restored text"), Field.Box->GetText().ToString(),
		FString(TEXT("abcx")));

	// Reject on Steam's Enter opens the panel with the message.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	WoTEntryTest_Type(Shown, TEXT(" "));
	WoTWidgetTest_Press(Shown, EKeys::Enter);
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	TestTrue(TEXT("a rejected Enter keeps the entry, now on the panel"),
		FWoTTextEntry::IsActive() && Panel.IsValid()
		&& FWoTTextEntry::GetActiveRoute() == TOptional<EWoTTextEntryRoute>(EWoTTextEntryRoute::Panel));
	if (Panel.IsValid())
	{
		TestEqual(TEXT("the panel names the rule"), Panel->GetMessage().ToString(),
			FString(TEXT("No spaces.")));
		TestEqual(TEXT("and continues from Steam's text"), Panel->GetDraft(), FString(TEXT("abcx ")));
	}
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("cancelling it restores the text from before the entry"), Field.Text,
		FString(TEXT("abcx")));

	// Y switches to the panel at any time, with the text so far.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	WoTEntryTest_Type(Shown, TEXT("z"));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Top);
	Panel = FWoTTextEntry::GetPanel();
	TestTrue(TEXT("Y switches to the panel"), Panel.IsValid() && Field.Layer->IsPanelShown());
	if (Panel.IsValid())
	{
		TestEqual(TEXT("with Steam's text so far"), Panel->GetDraft(), FString(TEXT("abcxz")));
		TestEqual(TEXT("the panel's prompts"), WoTEntryTest_Prompts(FWoTTextEntry::ControllerPrompts()),
			WoTEntryTest_Prompts(FWoTButtonPromptSets::KeyboardPanel()));
	}
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Right);
	TestEqual(TEXT("Done commits the panel's text"), Field.Text, FString(TEXT("abcxz")));

	// Y on a field with no entry opens the panel instead of Steam's keyboard.
	const int32 LaunchesBefore = Steam.Launched.Num();
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Top);
	TestTrue(TEXT("Y opens the panel"), FWoTTextEntry::GetPanel().IsValid());
	TestEqual(TEXT("without Steam's keyboard"), Steam.Launched.Num(), LaunchesBefore);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);

	// A launcher error falls back to the panel and is kept for diagnosis.
	Steam.LaunchError = TEXT("ShellExecute failed (2)");
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("a launch error falls back to the panel"),
		FWoTTextEntry::GetPanel().IsValid() && Field.Layer->IsPanelShown());
	TestEqual(TEXT("and the error is captured"), FWoTTextEntry::GetLastLaunchError(),
		FString(TEXT("ShellExecute failed (2)")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);
	Steam.LaunchError.Reset();

	// Focus moving to another control keeps what Steam typed, like a physical
	// keyboard (owner ruling, 2026-11-09): nothing is restored or committed.
	Slate.SetKeyboardFocus(Field.Box, EFocusCause::SetDirectly);
	Shown.Settle();
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("A starts a Steam entry again"),
		FWoTTextEntry::GetActiveRoute() == TOptional<EWoTTextEntryRoute>(EWoTTextEntryRoute::SteamKeyboard));
	const FString BeforeFocusMove = Field.Text;
	WoTEntryTest_Type(Shown, TEXT("z"));
	Field.Calls.Reset();
	Slate.SetKeyboardFocus(Field.Other, EFocusCause::SetDirectly);
	Shown.Settle();
	TestFalse(TEXT("moving focus away ends the Steam entry"), FWoTTextEntry::IsActive());
	TestEqual(TEXT("and keeps what Steam typed"), Field.Text, BeforeFocusMove + TEXT("z"));
	TestTrue(TEXT("with no commit or cancel"), Field.Calls.IsEmpty());

	// With SteamDeck=1 a tap on a field starts entry [I8]; without it a click
	// only places the caret.
	Slate.SetKeyboardFocus(Field.Other, EFocusCause::SetDirectly);
	Shown.Settle();
	{
		const FVector2D At = Shown.ScreenCenter(Field.Box);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		Shown.Settle();
	}
	TestTrue(TEXT("a tap starts entry on the Deck"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and focuses the field for Steam's keys"), Field.Box->HasKeyboardFocus());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);
	FWoTTextEntry::SetSteamKeyboardEnabledForTesting(false);
	Slate.SetKeyboardFocus(Field.Other, EFocusCause::SetDirectly);
	Shown.Settle();
	{
		const FVector2D At = Shown.ScreenCenter(Field.Box);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		Shown.Settle();
	}
	TestTrue(TEXT("with the toggle off a tap opens the panel"),
		FWoTTextEntry::GetPanel().IsValid() && Field.Layer->IsPanelShown());
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right);
	FWoTTextEntry::SetSteamDeckEnvironmentForTesting(false);
	Slate.SetKeyboardFocus(Field.Other, EFocusCause::SetDirectly);
	Shown.Settle();
	{
		const FVector2D At = Shown.ScreenCenter(Field.Box);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		Shown.Settle();
	}
	TestFalse(TEXT("off the Deck a click starts no entry"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("it focuses the field as always"), Field.Box->HasKeyboardFocus());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUITextEntryToggleSpec,
	"WoT.UI.TextEntry.Toggle", WoTUITest_Flags)

bool FWoTUITextEntryToggleSpec::RunTest(const FString&)
{
	FWoTEntryTest_Guard EntryGuard;
	// The stored line is the player's explicit choice; anything else is no
	// choice, so the default applies (on: the owner's Steam Deck session).
	TestTrue(TEXT("the default is on"), FWoTTextEntry::SteamKeyboardDefault);
	auto Parse = [](const ANSICHAR* Text)
	{
		return FWoTTextEntry::ParseToggle(WoTUITest_Utf8(Text));
	};
	auto Is = [](const TOptional<bool>& Stored, bool bValue)
	{
		return Stored.IsSet() && Stored.GetValue() == bValue;
	};
	TestFalse(TEXT("an empty file is no choice"),
		FWoTTextEntry::ParseToggle(TArray<uint8>()).IsSet());
	TestTrue(TEXT("the on line"), Is(Parse("WoTTextEntry-v1 steam-keyboard=on"), true));
	TestTrue(TEXT("with its newline"), Is(Parse("WoTTextEntry-v1 steam-keyboard=on\n"), true));
	TestTrue(TEXT("or a Windows newline"), Is(Parse("WoTTextEntry-v1 steam-keyboard=on\r\n"), true));
	TestTrue(TEXT("the off line is an explicit off"),
		Is(Parse("WoTTextEntry-v1 steam-keyboard=off\n"), false));
	TestFalse(TEXT("another case is invalid, so no choice"),
		Parse("WoTTextEntry-v1 steam-keyboard=ON").IsSet());
	TestFalse(TEXT("another version is invalid"), Parse("WoTTextEntry-v2 steam-keyboard=on").IsSet());
	TestFalse(TEXT("leading space is invalid"), Parse(" WoTTextEntry-v1 steam-keyboard=on").IsSet());
	TestFalse(TEXT("non-ASCII is invalid"),
		Parse("WoTTextEntry-v1 steam-keyboard=on\xC3\xA9").IsSet());
	TestFalse(TEXT("a long file is invalid"),
		Parse("WoTTextEntry-v1 steam-keyboard=on                                        "
			"                                ").IsSet());
	TestEqual(TEXT("the line it writes"), FWoTTextEntry::FormatToggle(true),
		FString(TEXT("WoTTextEntry-v1 steam-keyboard=on")));

	// Automation never touches the player's file.
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	TArray<FString> Touched;
	UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting(
		[&Touched](const FString& Path, bool) -> TOptional<bool>
		{
			Touched.Add(Path);
			return false;
		});
	TestTrue(TEXT("the default without a test preference path"),
		FWoTTextEntry::IsSteamKeyboardEnabled());
	TestFalse(TEXT("and never saved"), FWoTTextEntry::SetSteamKeyboardEnabled(true));
	TestTrue(TEXT("the player's side file is never touched"), Touched.IsEmpty());
	UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting({});

	// A test profile: missing is the default (on), and an explicit choice,
	// either way, round-trips its line and wins.
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
		FString(TEXT("TextEntry-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	FString Path;
	TestTrue(TEXT("the side file resolves"),
		UWoTPreferencesSubsystem::SideFilePath(FWoTTextEntry::ToggleSideFileName, Path));
	TestEqual(TEXT("as the TextEntry side file"), Path, Leaf + TEXT(".TextEntry"));
	TestTrue(TEXT("a missing file is on (the default)"), FWoTTextEntry::IsSteamKeyboardEnabled());
	TestTrue(TEXT("off saves"), FWoTTextEntry::SetSteamKeyboardEnabled(false));
	FString Written;
	FFileHelper::LoadFileToString(Written, *Path);
	TestEqual(TEXT("as its off line"), Written, FString(TEXT("WoTTextEntry-v1 steam-keyboard=off\n")));
	TestFalse(TEXT("and an explicit off wins over the default"),
		FWoTTextEntry::IsSteamKeyboardEnabled());
	TestTrue(TEXT("the toggle saves on"), FWoTTextEntry::SetSteamKeyboardEnabled(true));
	FFileHelper::LoadFileToString(Written, *Path);
	TestEqual(TEXT("one ASCII line"), Written, FString(TEXT("WoTTextEntry-v1 steam-keyboard=on\n")));
	TestTrue(TEXT("and reads back on"), FWoTTextEntry::IsSteamKeyboardEnabled());
	FFileHelper::SaveStringToFile(FString(TEXT("steam-keyboard=off")), *Path);
	TestTrue(TEXT("an invalid file is no choice: the default, on"),
		FWoTTextEntry::IsSteamKeyboardEnabled());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsSteamKeyboardToggleSpec,
	"WoT.Settings.SteamKeyboardToggle", WoTUITest_Flags)

bool FWoTSettingsSteamKeyboardToggleSpec::RunTest(const FString&)
{
	FWoTEntryTest_Guard EntryGuard;
	FWoTUITest_SettingsRowsGuard RowsGuard;
	FWoTUITest_Preferences Fixture;
	if (!TestTrue(TEXT("a test preferences owner"), Fixture.bReady))
	{
		return false;
	}
	auto MakeControls = [&Fixture]()
	{
		return SNew(SWoTSettingsScreen)
			.Page(EWoTSettingsPage::Controls)
			.Preferences(Fixture.Owner)
			.BodyFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14))
			.HeadingFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 20));
	};
	TestFalse(TEXT("off the Deck the Controls page has no Steam keyboard row"),
		MakeControls()->SteamKeyboardCheckBoxForTesting().IsValid());

	WoTSetRunningUnderWineForTesting(true);
	FWoTTextEntry::SetSteamDeckEnvironmentForTesting(true);
	TSharedRef<SWoTSettingsScreen> Controls = MakeControls();
	const TSharedPtr<SCheckBox> Toggle = Controls->SteamKeyboardCheckBoxForTesting();
	if (!TestTrue(TEXT("on the Deck the row is there"), Toggle.IsValid()))
	{
		return false;
	}
	const TArray<TArray<TSharedPtr<SWidget>>>& Rows = Controls->NavigationRowsForTesting();
	const int32 InvertRow = Rows.IndexOfByPredicate([&Controls](const TArray<TSharedPtr<SWidget>>& Row)
	{
		return Row.Num() == 1 && Row[0] == Controls->ControllerInvertCheckBoxForTesting();
	});
	TestTrue(TEXT("next to the controller options, in the focus graph"),
		Rows.IsValidIndex(InvertRow + 1) && Rows[InvertRow + 1].Num() == 1
		&& Rows[InvertRow + 1][0] == Toggle);
	// On by default for a player who never set it (the owner's Steam Deck
	// session, 2026-11-09); a stored choice wins.
	TestTrue(TEXT("it starts on, with nothing stored"), Toggle->IsChecked());
	TestTrue(TEXT("and the route is Steam's keyboard"),
		FWoTTextEntry::ChooseRoute() == EWoTTextEntryRoute::SteamKeyboard);
	TestFalse(TEXT("an untouched page has nothing to save"), Controls->HasUnsavedChanges());
	Toggle->ToggleCheckedState();
	TestFalse(TEXT("a toggle changes the draft"), Controls->SteamKeyboardDraftForTesting());
	TestTrue(TEXT("which is an unsaved change"), Controls->HasUnsavedChanges());
	TestTrue(TEXT("nothing is stored before Save"), FWoTTextEntry::IsSteamKeyboardEnabled());
	Controls->SaveForTesting();
	TestFalse(TEXT("Save stores the off choice in its side file"),
		FWoTTextEntry::IsSteamKeyboardEnabled());
	TestTrue(TEXT("and the route follows it"),
		FWoTTextEntry::ChooseRoute() == EWoTTextEntryRoute::Panel);
	TSharedRef<SWoTSettingsScreen> Reopened = MakeControls();
	TestTrue(TEXT("the page reopens with it off: the stored choice wins"),
		Reopened->SteamKeyboardCheckBoxForTesting().IsValid()
		&& !Reopened->SteamKeyboardCheckBoxForTesting()->IsChecked());
	Reopened->SteamKeyboardCheckBoxForTesting()->ToggleCheckedState();
	// The Back button drops the draft at once, as it always has.
	Reopened->BackForTesting();
	TestFalse(TEXT("Back discards the change"), FWoTTextEntry::IsSteamKeyboardEnabled());
	TSharedRef<SWoTSettingsScreen> Third = MakeControls();
	Third->SteamKeyboardCheckBoxForTesting()->ToggleCheckedState();
	Third->SaveForTesting();
	TestTrue(TEXT("an explicit on is stored too"), FWoTTextEntry::IsSteamKeyboardEnabled());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTextEntryFieldsSpec,
	"WoT.FrontEnd.TextEntry.Fields", WoTUITest_Flags)

bool FWoTFrontEndTextEntryFieldsSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	auto Done = [&FrontEnd]() { FrontEnd.Press(EKeys::Gamepad_Special_Right); };

	// Server name: the new 32 limit; Done stores it in the host request.
	TSharedPtr<SWoTOnScreenKeyboard> Panel =
		WoTEntryTest_BeginOn(FrontEnd, Root->ServerNameTextBoxForTesting());
	if (TestTrue(TEXT("A on the server name opens the panel"), Panel.IsValid()))
	{
		TestEqual(TEXT("the server name takes 32 characters"), Panel->GetMaxLength(), 32);
		Panel->SetDraft(FString());
		for (int32 Index = 0; Index < 40; ++Index)
		{
			Panel->TypeCharacter(TEXT('n'));
		}
		Done();
		TestEqual(TEXT("Done stores the server name, capped at 32"),
			Root->ViewModelForTesting().GetHostRequest().ServerName, FString::ChrN(32, TEXT('n')));
	}

	// Admin and game passwords: masked; SanitizeHostPassword's rule on Done.
	for (const bool bAdmin : {true, false})
	{
		const TCHAR* Which = bAdmin ? TEXT("admin") : TEXT("game");
		Panel = WoTEntryTest_BeginOn(FrontEnd, bAdmin
			? Root->AdminPasswordTextBoxForTesting() : Root->GamePasswordTextBoxForTesting());
		if (!TestTrue(FString::Printf(TEXT("A on the %s password opens the panel"), Which),
			Panel.IsValid()))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("the %s password is masked"), Which), Panel->IsPassword());
		TestEqual(FString::Printf(TEXT("the %s password takes 32"), Which), Panel->GetMaxLength(), 32);
		Panel->SetDraft(TEXT("bad pass"));
		Done();
		TestTrue(FString::Printf(TEXT("a %s password with a space is rejected"), Which),
			FWoTTextEntry::IsActive() && !Panel->GetMessage().IsEmpty());
		const FWoTHostRequest& Host = Root->ViewModelForTesting().GetHostRequest();
		TestTrue(FString::Printf(TEXT("nothing is stored for the %s password"), Which),
			(bAdmin ? Host.AdminPassword : Host.GamePassword).IsEmpty());
		Panel->SetDraft(TEXT("Secret1"));
		Done();
		TestFalse(FString::Printf(TEXT("a valid %s password ends the entry"), Which),
			FWoTTextEntry::IsActive());
		TestEqual(FString::Printf(TEXT("the %s password is stored"), Which),
			bAdmin ? Root->ViewModelForTesting().GetHostRequest().AdminPassword
				: Root->ViewModelForTesting().GetHostRequest().GamePassword,
			FString(TEXT("Secret1")));
	}

	// Direct address: the new 64 limit; Done connects (on a Mac-like OS the
	// local-network heads-up comes first, so it shows Connect ran).
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(true);
	Panel = WoTEntryTest_BeginOn(FrontEnd, Root->DirectAddressTextBoxForTesting());
	if (TestTrue(TEXT("A on the direct address opens the panel"), Panel.IsValid()))
	{
		TestEqual(TEXT("the direct address takes 64 characters"), Panel->GetMaxLength(), 64);
		Panel->SetDraft(TEXT("10.0.0.5:7777"));
		Done();
		TestEqual(TEXT("Done sets the direct address"), Root->DirectAddressForTesting(),
			FString(TEXT("10.0.0.5:7777")));
		TestEqual(TEXT("and the box shows it"),
			WoTEntryTest_BoxText(Root->DirectAddressTextBoxForTesting()), FString(TEXT("10.0.0.5:7777")));
		TestTrue(TEXT("Done then connects"), Root->IsLocalNetworkHeadsUpVisibleForTesting());
		Root->CancelLocalNetworkHeadsUpForTesting();
		FrontEnd.Settle();
	}
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);

	// Player name (front-end Player): 20 characters and the name filter.
	Root->OpenPlayerConfigForTesting();
	FrontEnd.Settle();
	Panel = WoTEntryTest_BeginOn(FrontEnd, Root->PlayerNameTextBoxForTesting());
	if (TestTrue(TEXT("A on the player name opens the panel"), Panel.IsValid()))
	{
		TestEqual(TEXT("the player name takes 20"), Panel->GetMaxLength(), 20);
		Panel->SetDraft(FString());
		for (const TCHAR Character : FString(TEXT("Sam?#=Brightwater of Lakeside")))
		{
			Panel->TypeCharacter(Character);
		}
		TestEqual(TEXT("the name filter runs on every key"), Panel->GetDraft(),
			FString(TEXT("SamBrightwater of La")));
		Done();
		TestEqual(TEXT("Done commits the name through CommitPlayerName"),
			Root->ViewModelForTesting().GetPendingPlayerName(), FString(TEXT("SamBrightwater of La")));
		TestEqual(TEXT("and the box shows it"),
			WoTEntryTest_BoxText(Root->PlayerNameTextBoxForTesting()), FString(TEXT("SamBrightwater of La")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTextEntryPasswordSpec,
	"WoT.FrontEnd.TextEntry.PasswordChallenge", WoTUITest_Flags)

bool FWoTFrontEndTextEntryPasswordSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	UWorld* World = WoTUITest_CreateWorld();
	if (!TestNotNull(TEXT("world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTUITest_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions = World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	AWoTFrontEndPlayerController* Controller = World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("sessions"), Sessions) || !TestNotNull(TEXT("controller"), Controller))
	{
		return false;
	}
	TArray<FString> Travels;
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetClientTravelHookForTesting(
		[&Travels](APlayerController*, const FString& URL, ETravelType) { Travels.Add(URL); });
	Sessions->SetServerTravelHookForTesting([](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT
	{
		Sessions->SetClientTravelHookForTesting({});
		Sessions->SetClientTravelControllerForTesting(nullptr);
		Sessions->SetServerTravelHookForTesting({});
	};
	Sessions->SetPasswordChallengeForTesting(EWoTPasswordChallenge::NeedPassword,
		TEXT("127.0.0.1:7777"));
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority, Controller),
		FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	TestTrue(TEXT("the challenge's box has focus"), Root->PasswordTextBoxForTesting().IsValid()
		&& Root->PasswordTextBoxForTesting()->HasKeyboardFocus());
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Bottom);
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("A on the challenge's box opens the panel"), Panel.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("masked"), Panel->IsPassword());
	TestEqual(TEXT("32 characters"), Panel->GetMaxLength(), 32);
	TestEqual(TEXT("the prompt strip shows the panel's set"),
		WoTEntryTest_Prompts(Root->ControllerPromptsForTesting()),
		WoTEntryTest_Prompts(FWoTButtonPromptSets::KeyboardPanel()));
	Panel->SetDraft(TEXT("no=way"));
	FrontEnd.Press(EKeys::Gamepad_Special_Right);
	TestTrue(TEXT("SanitizeHostPassword rejects, and the panel stays"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("naming the rule"), Panel->GetMessage().ToString().Contains(TEXT("32 characters")));
	TestTrue(TEXT("nothing is submitted"), Travels.IsEmpty());
	Panel->SetDraft(TEXT("Secret1"));
	FrontEnd.Press(EKeys::Gamepad_Special_Right);
	TestFalse(TEXT("Done ends the entry"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("Done submits the password: the travel carries its token"),
		Travels.Num() == 1 && Travels[0].Contains(TEXT("PasswordToken=")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTextEntryRebuildSpec,
	"WoT.FrontEnd.TextEntry.RebuildKeepsDraft", WoTUITest_Flags)

bool FWoTFrontEndTextEntryRebuildSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();
	const TSharedPtr<SWidget> OldField = Root->ServerNameTextBoxForTesting();
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = WoTEntryTest_BeginOn(FrontEnd, OldField);
	if (!TestTrue(TEXT("the panel opens on the server name"), Panel.IsValid()))
	{
		return false;
	}
	Panel->SetDraft(TEXT("Dra"));
	const uint64 Rebuilds = Root->ScreenRebuildCountForTesting();
	Root->RebuildScreenForTesting();
	FrontEnd.Settle();
	TestEqual(TEXT("Play rebuilt"), Root->ScreenRebuildCountForTesting(), Rebuilds + 1);
	TestTrue(TEXT("the field is a new widget"), Root->ServerNameTextBoxForTesting() != OldField);
	TestTrue(TEXT("the entry survives the rebuild [M6]"), FWoTTextEntry::IsActive()
		&& FWoTTextEntry::GetPanel() == Panel);
	TestEqual(TEXT("with its draft"), Panel->GetDraft(), FString(TEXT("Dra")));
	TestTrue(TEXT("and focus"), Panel->HasKeyboardFocus());
	Panel->TypeCharacter(TEXT('f'));
	FrontEnd.Press(EKeys::Gamepad_Special_Right);
	TestEqual(TEXT("Done commits the draft to the view model"),
		Root->ViewModelForTesting().GetHostRequest().ServerName, FString(TEXT("Draf")));
	FrontEnd.Settle();
	TestEqual(TEXT("the new field shows it"),
		WoTEntryTest_BoxText(Root->ServerNameTextBoxForTesting()), FString(TEXT("Draf")));
	TestTrue(TEXT("focus returns to the new field"),
		Root->ServerNameTextBoxForTesting()->HasKeyboardFocus());

	// On Steam's route a rebuild hands focus to the rebuilt field, so Steam's
	// keys keep landing in it.
	FWoTEntryTest_Steam Steam;
	WoTEntryTest_BeginOn(FrontEnd, Root->ServerNameTextBoxForTesting());
	TestTrue(TEXT("Steam's route on the server name"),
		FWoTTextEntry::GetActiveRoute() == TOptional<EWoTTextEntryRoute>(EWoTTextEntryRoute::SteamKeyboard));
	Root->RebuildScreenForTesting();
	FrontEnd.Settle();
	TestTrue(TEXT("Steam's entry survives the rebuild"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and the rebuilt field has focus"),
		Root->ServerNameTextBoxForTesting()->HasKeyboardFocus());
	WoTEntryTest_Type(FrontEnd.Shown, TEXT("X"));
	TestEqual(TEXT("Steam's keys type into it"),
		Root->ViewModelForTesting().GetHostRequest().ServerName, FString(TEXT("DrafX")));
	FrontEnd.Press(EKeys::Enter);
	TestFalse(TEXT("Steam's Enter ends it"), FWoTTextEntry::IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTextEntryBackSpec,
	"WoT.FrontEnd.TextEntry.EscapeAndBack", WoTUITest_Flags)

bool FWoTFrontEndTextEntryBackSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority, WoTUITest_MakeRoot(Authority), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	auto Screen = [&Root]() { return Root->CurrentScreenForTesting(); };
	Root->ActivateMainMenuEntryForTesting(0);
	FrontEnd.Settle();

	// Escape with the panel open cancels the panel first [M3].
	TSharedPtr<SWoTOnScreenKeyboard> Panel =
		WoTEntryTest_BeginOn(FrontEnd, Root->ServerNameTextBoxForTesting());
	if (!TestTrue(TEXT("the panel opens"), Panel.IsValid()))
	{
		return false;
	}
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	FrontEnd.Settle();
	TestEqual(TEXT("the strip shows the panel's prompts"),
		WoTEntryTest_Prompts(Root->ButtonPromptsForTesting()->GetShownPrompts()),
		WoTEntryTest_Prompts(FWoTButtonPromptSets::KeyboardPanel()));
	const uint64 Rebuilds = Root->ScreenRebuildCountForTesting();
	TestTrue(TEXT("Escape is handled"), FrontEnd.Press(EKeys::Escape));
	TestFalse(TEXT("Escape cancels the panel"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and no screen back happens"), Screen() == EWoTFrontEndScreen::Play);
	TestEqual(TEXT("nothing rebuilds"), Root->ScreenRebuildCountForTesting(), Rebuilds);
	TestTrue(TEXT("focus returns to the field"), Root->ServerNameTextBoxForTesting()->HasKeyboardFocus());

	// On the panel View does nothing, X deletes and B cancels; none of them is
	// ever a screen back (the owner's Steam Deck ruling, 2026-11-09).
	Panel = WoTEntryTest_BeginOn(FrontEnd, Root->ServerNameTextBoxForTesting());
	if (Panel.IsValid())
	{
		Panel->SetDraft(TEXT("ab"));
		FrontEnd.Press(EKeys::Gamepad_Special_Left);
		TestTrue(TEXT("View leaves the panel open on Play"), Screen() == EWoTFrontEndScreen::Play
			&& FWoTTextEntry::IsActive() && Panel->GetDraft() == TEXT("ab"));
		FrontEnd.Press(EKeys::Gamepad_FaceButton_Left);
		TestEqual(TEXT("X is Backspace on the panel"), Panel->GetDraft(), FString(TEXT("a")));
		FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
		TestFalse(TEXT("B cancels the panel"), FWoTTextEntry::IsActive());
		TestTrue(TEXT("still on Play"), Screen() == EWoTFrontEndScreen::Play);
	}

	// With Steam's keyboard typing into the field, B cancels the entry and
	// restores the field; with no entry, B backs the screen as before [I3].
	{
		FWoTEntryTest_Steam Steam;
		Panel = WoTEntryTest_BeginOn(FrontEnd, Root->DirectAddressTextBoxForTesting());
		TestTrue(TEXT("Steam's route on the direct address"),
			!Panel.IsValid() && FWoTTextEntry::IsActive());
		WoTEntryTest_Type(FrontEnd.Shown, TEXT("10.0"));
		TestEqual(TEXT("Steam typed into the direct address"), Root->DirectAddressForTesting(),
			FString(TEXT("10.0")));
		TestEqual(TEXT("the strip shows Steam's prompts"),
			WoTEntryTest_Prompts(Root->ButtonPromptsForTesting()->GetShownPrompts()),
			WoTEntryTest_Prompts(FWoTButtonPromptSets::SteamKeyboard()));
		TestTrue(TEXT("B is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
		TestFalse(TEXT("B cancels the active entry"), FWoTTextEntry::IsActive());
		TestTrue(TEXT("restoring the field"), Root->DirectAddressForTesting().IsEmpty());
		TestTrue(TEXT("without backing the screen"), Screen() == EWoTFrontEndScreen::Play);
	}
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
	TestTrue(TEXT("B in the field with no entry backs Play as before"),
		Screen() == EWoTFrontEndScreen::Main);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTextEntryInGameSpec,
	"WoT.FrontEnd.TextEntry.InGamePlayerName", WoTUITest_Flags)

bool FWoTFrontEndTextEntryInGameSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	if (!TestNotNull(TEXT("world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("controller exists"), Controller))
	{
		return false;
	}
	Controller->InitPlayerState();
	Controller->PlayerState->SetPlayerName(TEXT("Avery"));
	const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	FWoTUITest_Window Shown(Overlay);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform()), 0.0, 0.0f);
	Shown.Settle();
	Overlay->OpenPlayerConfigForTesting();
	Shown.Settle();
	const TSharedPtr<SWidget> Field = Overlay->PlayerNameInputForTesting();
	if (!TestTrue(TEXT("the in-game Player page has its name field"), Field.IsValid()))
	{
		return false;
	}
	Slate.SetKeyboardFocus(Field, EFocusCause::SetDirectly);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("A on the name opens the panel"), Panel.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("from the live name"), Panel->GetDraft(), FString(TEXT("Avery")));
	TestEqual(TEXT("20 characters"), Panel->GetMaxLength(), 20);
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestEqual(TEXT("the game menu's strip shows the panel's prompts"),
		WoTEntryTest_Prompts(Overlay->ControllerPromptsForTesting()),
		WoTEntryTest_Prompts(FWoTButtonPromptSets::KeyboardPanel()));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Left);
	TestEqual(TEXT("X is Backspace"), Panel->GetDraft(), FString(TEXT("Aver")));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Left);
	TestTrue(TEXT("View does nothing on the panel"), FWoTTextEntry::IsActive());
	TestFalse(TEXT("not a page back"), Overlay->IsShowingMainMenuForTesting());
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestFalse(TEXT("Escape cancels the panel first"), FWoTTextEntry::IsActive());
	TestFalse(TEXT("and the page stays"), Overlay->IsShowingMainMenuForTesting());
	TestEqual(TEXT("cancel keeps the name"), Overlay->PendingPlayerNameForTesting(), FString(TEXT("Avery")));

	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	Panel = FWoTTextEntry::GetPanel();
	if (Panel.IsValid())
	{
		Panel->SetDraft(TEXT("Morgan"));
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_Special_Right);
	}
	TestEqual(TEXT("Done sets the page's pending name"), Overlay->PendingPlayerNameForTesting(),
		FString(TEXT("Morgan")));
	Overlay->SavePlayerConfigForTesting();
	TestEqual(TEXT("the page's Save path commits it"), Controller->PlayerState->GetPlayerName(),
		FString(TEXT("Morgan")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatLabelSpec,
	"WoT.Chat.Input.Label", WoTUITest_Flags)

bool FWoTChatLabelSpec::RunTest(const FString&)
{
	// Owner ruling 2026-11-08: keyboard chat shows "Say:" / "Team say:" too;
	// the line it sends is unchanged.
	const TSharedRef<SWoTChatInputOverlay> Say = SNew(SWoTChatInputOverlay)
		.PlayerController(nullptr)
		.InitialText(TEXT("Say "));
	TestEqual(TEXT("Talk shows Say:"), Say->GetLabel().ToString(), FString(TEXT("Say:")));
	TestEqual(TEXT("the line keeps the prefix"), Say->GetText(), FString(TEXT("Say ")));
	Say->TextWidget()->OnKeyChar(FGeometry(), FCharacterEvent(TEXT('h'), FModifierKeysState(), 0, false));
	TestEqual(TEXT("typing follows the label"), Say->GetText(), FString(TEXT("Say h")));
	const TSharedRef<SWoTChatInputOverlay> Team = SNew(SWoTChatInputOverlay)
		.PlayerController(nullptr)
		.InitialText(TEXT("TeamSay "));
	TestEqual(TEXT("TeamTalk shows Team say:"), Team->GetLabel().ToString(),
		FString(TEXT("Team say:")));
	TestEqual(TEXT("its line keeps its prefix"), Team->GetText(), FString(TEXT("TeamSay ")));
	const TSharedRef<SWoTChatInputOverlay> Bare = SNew(SWoTChatInputOverlay)
		.PlayerController(nullptr)
		.InitialText(FString());
	TestTrue(TEXT("bare Type has no label"), Bare->GetLabel().IsEmpty());
	// Backspace on an empty line takes the label back, as retail's Backspace
	// over the "Say " prefill did.
	Team->TextWidget()->OnKeyChar(FGeometry(), FCharacterEvent(TCHAR(8), FModifierKeysState(), 0, false));
	TestTrue(TEXT("Backspace on an empty line drops the label"), Team->GetLabel().IsEmpty());
	TestEqual(TEXT("leaving the prefill as text, as retail"), Team->GetText(), FString(TEXT("TeamSay")));
	Say->SetText(TEXT("TeamSay hi"));
	TestEqual(TEXT("a set line shows its label"), Say->GetLabel().ToString(), FString(TEXT("Team say:")));
	TestEqual(TEXT("and keeps the whole line"), Say->GetText(), FString(TEXT("TeamSay hi")));
	return !HasAnyErrors();
}

#if WITH_EDITOR
namespace
{
/** A real game viewport and a local chat player (the Fix14 chat fixture). */
struct FWoTEntryTest_ChatFixture
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;
	UGameInstance* GameInstance = nullptr;
	UGameViewportClient* ViewportClient = nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<FSceneViewport> SceneViewport;
	TSharedPtr<SWindow> Window;
	UGameViewportClient* PreviousEngineViewport = nullptr;
	AWoTTestLeftMessageCaptureController* Controller = nullptr;

	~FWoTEntryTest_ChatFixture()
	{
		FWoTTextEntry::ResetForTesting();
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
			? NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass) : nullptr;
		PreviousEngineViewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
		if (!Test.TestNotNull(TEXT("listen world"), World)
			|| !Test.TestNotNull(TEXT("world context"), Context)
			|| !Test.TestNotNull(TEXT("game instance"), GameInstance)
			|| !Test.TestNotNull(TEXT("game viewport client"), ViewportClient))
		{
			return false;
		}
		ViewportClient->Init(*Context, GameInstance);
		Context->GameViewport = ViewportClient;
		GEngine->GameViewport = ViewportClient;
		ViewportWidget = SNew(SViewport);
		SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
		SceneViewport->SetInitialSize(FIntPoint(1024, 768));
		ViewportClient->AddAssociation(*SceneViewport);
		ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);
		Window = SNew(SWindow)
			.ScreenPosition(FVector2D(100.0f, 100.0f))
			.ClientSize(FVector2D(1024.0f, 768.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[ViewportWidget.ToSharedRef()];
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.AddWindow(Window.ToSharedRef(), false);
		Slate.Tick();
		Window->SlatePrepass(1.0f);
		Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
		FString PlayerError;
		ULocalPlayer* LocalPlayer = GameInstance->CreateLocalPlayer(0, PlayerError,
			/*bSpawnPlayerController*/ false);
		Controller = World->SpawnActor<AWoTTestLeftMessageCaptureController>();
		if (!Test.TestNotNull(TEXT("local player"), LocalPlayer)
			|| !Test.TestNotNull(TEXT("local controller"), Controller))
		{
			return false;
		}
		Controller->SetPlayer(LocalPlayer);
		Controller->EnableChatWidgetForTesting();
		if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
		{
			State->SetPlayerName(TEXT("Avery"));
		}
		const bool bHasTalk = Controller->PlayerInput->ActionMappings.ContainsByPredicate(
			[](const FInputActionKeyMapping& Mapping)
			{
				return Mapping.ActionName == TEXT("Talk") && Mapping.Key == EKeys::T;
			});
		if (!bHasTalk)
		{
			Controller->PlayerInput->AddActionMapping(FInputActionKeyMapping(TEXT("Talk"), EKeys::T));
		}
		return true;
	}

	void Settle()
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			Slate.Tick();
			Window->SlatePrepass(1.0f);
			FSlateWindowElementList Elements(Window.ToSharedRef());
			Window->PaintWindow(Slate.GetCurrentTime(), 0.0f, Elements, FWidgetStyle(), true);
		}
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}

	void Press(FKey Key)
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.ProcessKeyDownEvent(WoTUITest_Key(Key));
		Slate.ProcessKeyUpEvent(WoTUITest_Key(Key));
		Settle();
	}

	void Type(const FString& Text)
	{
		for (const TCHAR Character : Text)
		{
			FSlateApplication::Get().ProcessKeyCharEvent(
				FCharacterEvent(Character, FModifierKeysState(), 0, false));
		}
		Settle();
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatTextEntrySpec,
	"WoT.Chat.TextEntry.PanelOverChat", WoTUITest_Flags)

bool FWoTChatTextEntrySpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	FWoTEntryTest_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	AWoTTestLeftMessageCaptureController* Controller = Fixture.Controller;

	// A key held in gameplay, then keyboard chat (T): chat keeps it held, and
	// shows the new label.
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::W));
	Fixture.Settle();
	TestTrue(TEXT("W is held in gameplay"), Controller->PlayerInput->IsPressed(EKeys::W));
	Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::T));
	Fixture.Settle();
	const TSharedPtr<SWoTChatInputOverlay> Chat = Controller->GetChatInputWidget();
	if (!TestTrue(TEXT("T opens keyboard chat"), Controller->IsChatTyping() && Chat.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("keyboard chat shows Say:"), Chat->GetLabel().ToString(), FString(TEXT("Say:")));
	TestTrue(TEXT("keyboard chat keeps the held key (unchanged)"),
		Controller->PlayerInput->IsPressed(EKeys::W));
	TestTrue(TEXT("chat's input router runs"), Controller->IsChatInputRouterActiveForTesting());

	// A on the chat line opens the panel over chat: held keys are flushed,
	// the router is suspended and input is UI-only [I7].
	Fixture.Press(EKeys::Gamepad_FaceButton_Bottom);
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("A on the chat line opens the panel"), Panel.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the draft is the text after the Say: label"), FWoTTextEntry::GetPendingText(),
		FString());
	TestTrue(TEXT("chat input is suspended"), Controller->IsChatInputSuspendedForTextEntry());
	TestFalse(TEXT("the router passes everything to the panel"),
		Controller->IsChatInputRouterActiveForTesting());
	TestFalse(TEXT("the held key is released for gameplay"),
		Controller->PlayerInput->IsPressed(EKeys::W));
	TestTrue(TEXT("the panel has focus"), Panel->HasKeyboardFocus());
	TestTrue(TEXT("the pointer may show for touch"), Controller->bShowMouseCursor);
	TestEqual(TEXT("the panel takes 128 characters"), Panel->GetMaxLength(),
		AWoTPlayerController::MaxChatMessageLength);
	Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::W));
	Fixture.Settle();
	TestFalse(TEXT("the late release changes nothing"), Controller->PlayerInput->IsPressed(EKeys::W));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestEqual(TEXT("chat shows the panel's prompts"),
		WoTEntryTest_Prompts(Chat->ButtonPromptsForTesting()->GetShownPrompts()),
		WoTEntryTest_Prompts(FWoTButtonPromptSets::KeyboardPanel()));

	// Cancel: chat's input mode comes back exactly, and chat stays open.
	Fixture.Type(TEXT("xy"));
	Fixture.Press(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("B cancels the panel"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("chat stays open"), Controller->IsChatTyping());
	TestFalse(TEXT("chat input resumes"), Controller->IsChatInputSuspendedForTextEntry());
	TestTrue(TEXT("its router runs again"), Controller->IsChatInputRouterActiveForTesting());
	TestTrue(TEXT("the chat line has focus again"), Slate.GetKeyboardFocusedWidget() == Chat->TextWidget());
	TestFalse(TEXT("the pointer hides again, as chat set it"), Controller->bShowMouseCursor);
	TestEqual(TEXT("cancel leaves the line as it was"), Controller->GetChatTextForTesting(),
		FString(TEXT("Say ")));

	// Escape with the panel open cancels the panel first; chat stays.
	Fixture.Press(EKeys::Gamepad_FaceButton_Bottom);
	Fixture.Press(EKeys::Escape);
	TestFalse(TEXT("Escape cancels the panel over chat"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and chat stays open"), Controller->IsChatTyping());

	// Done sends the line with its prefix.
	Fixture.Press(EKeys::Gamepad_FaceButton_Bottom);
	Fixture.Type(TEXT("hi"));
	Fixture.Press(EKeys::Gamepad_Special_Right);
	TestFalse(TEXT("Done ends the entry"), FWoTTextEntry::IsActive());
	TestFalse(TEXT("and sends the chat line"), Controller->IsChatTyping());
	TestTrue(TEXT("as Say"), Controller->LeftMessages.Contains(TEXT("Avery: hi")));

	// Keyboard chat is unchanged: T, type, Enter.
	Fixture.Press(EKeys::T);
	Fixture.Type(TEXT("ok"));
	TestEqual(TEXT("keyboard typing edits the line"), Controller->GetChatTextForTesting(),
		FString(TEXT("Say ok")));
	Fixture.Press(EKeys::Enter);
	TestFalse(TEXT("Enter sends"), Controller->IsChatTyping());
	TestTrue(TEXT("the keyboard line"), Controller->LeftMessages.Contains(TEXT("Avery: ok")));
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Controller sub-project 3, PR 4: chat and taunts in a match (spec r3
// section 4): D-pad up chat, and the in-game menu's Chat, Team Chat and Taunts.

namespace
{
/** A gameplay key edge, delivered to the controller as the viewport does. */
void WoTChatTest_Send(AWoTPlayerController* Controller, FKey Key, EInputEvent Event)
{
	Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, Event,
		Event == IE_Released ? 0.0f : 1.0f));
}

/** World frames at 60 Hz, each with a settled Slate pass. */
void WoTChatTest_Frames(FWoTEntryTest_ChatFixture& Fixture, int32 Count)
{
	for (int32 Frame = 0; Frame < Count; ++Frame)
	{
		Fixture.Settle();
	}
}

/** Frames enough for a D-pad up hold to pass the hold time. */
int32 WoTChatTest_HoldFrames()
{
	return FMath::CeilToInt((WoTGamepadTuning().HoldSeconds + 0.1f) * 60.0f);
}

/** TeamTalk on Y, as DefaultInput.ini binds it (the fixture adds Talk on T). */
void WoTChatTest_EnsureTeamTalkKey(AWoTPlayerController* Controller)
{
	const bool bHasTeamTalk = Controller->PlayerInput->ActionMappings.ContainsByPredicate(
		[](const FInputActionKeyMapping& Mapping)
		{
			return Mapping.ActionName == TEXT("TeamTalk") && Mapping.Key == EKeys::Y;
		});
	if (!bHasTeamTalk)
	{
		Controller->PlayerInput->AddActionMapping(FInputActionKeyMapping(TEXT("TeamTalk"), EKeys::Y));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatControllerDPadUpSpec,
	"WoT.Chat.Controller.DPadUp", WoTUITest_Flags)

bool FWoTChatControllerDPadUpSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	FWoTEntryTest_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	AWoTTestLeftMessageCaptureController* Controller = Fixture.Controller;
	WoTChatTest_EnsureTeamTalkKey(Controller);
	WoTChatTest_Frames(Fixture, 2);   // the controller layer installs its context
	const UWoTGamepadLayer* Layer = Controller->GetGamepadLayerForTest();
	if (!TestTrue(TEXT("the controller layer is in gameplay"),
		Layer && Layer->GetMode() == EWoTGamepadMode::Gameplay))
	{
		return false;
	}

	// A tap opens Talk's line and its keyboard panel in one step.
	WoTChatTest_Send(Controller, EKeys::Gamepad_DPad_Up, IE_Pressed);
	WoTChatTest_Frames(Fixture, 6);
	WoTChatTest_Send(Controller, EKeys::Gamepad_DPad_Up, IE_Released);
	WoTChatTest_Frames(Fixture, 2);
	TSharedPtr<SWoTChatInputOverlay> Chat = Controller->GetChatInputWidget();
	if (!TestTrue(TEXT("a D-pad up tap opens chat"), Controller->IsChatTyping() && Chat.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("as T opens it: Say:"), Chat->GetLabel().ToString(), FString(TEXT("Say:")));
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("the keyboard panel opens with it"),
		Panel.IsValid() && FWoTTextEntry::IsActiveWithin(Chat.ToSharedRef())))
	{
		return false;
	}
	TestTrue(TEXT("the panel has focus"), Panel->HasKeyboardFocus());
	TestTrue(TEXT("chat input is suspended for the panel"),
		Controller->IsChatInputSuspendedForTextEntry());
	Fixture.Type(TEXT("hi"));
	Fixture.Press(EKeys::Gamepad_Special_Right);
	TestFalse(TEXT("Done sends the line"), Controller->IsChatTyping());
	TestEqual(TEXT("the line is Talk's"), Controller->GetLastSubmittedChatLineForTesting(),
		FString(TEXT("Say hi")));
	TestTrue(TEXT("as Say"), Controller->LeftMessages.Contains(TEXT("Avery: hi")));
	Fixture.Press(EKeys::T);
	Fixture.Type(TEXT("hi"));
	Fixture.Press(EKeys::Enter);
	TestEqual(TEXT("keyboard chat (T) sends the same line"),
		Controller->GetLastSubmittedChatLineForTesting(), FString(TEXT("Say hi")));
	WoTChatTest_Frames(Fixture, 2);

	// A hold opens TeamTalk's line and the panel while D-pad up is still held.
	{
		FWoTUITest_InputScope Scope(Fixture.Window.ToSharedRef());
		WoTChatTest_Send(Controller, EKeys::Gamepad_DPad_Up, IE_Pressed);
		WoTChatTest_Frames(Fixture, WoTChatTest_HoldFrames());
		Chat = Controller->GetChatInputWidget();
		Panel = FWoTTextEntry::GetPanel();
		if (!TestTrue(TEXT("a D-pad up hold opens chat and the panel before the release"),
			Controller->IsChatTyping() && Chat.IsValid() && Panel.IsValid()))
		{
			return false;
		}
		TestEqual(TEXT("as Y opens it: Team say:"), Chat->GetLabel().ToString(),
			FString(TEXT("Team say:")));
		// XInput's own repeats of the held D-pad up now reach the panel: a press
		// that began in gameplay never repeats there [I1].
		const FIntPoint Highlight = Panel->GetHighlight();
		for (int32 Repeat = 0; Repeat < 3; ++Repeat)
		{
			Slate.ProcessKeyDownEvent(WoTUITest_Key(EKeys::Gamepad_DPad_Up, /*bRepeat*/ true));
			Fixture.Settle();
		}
		*Scope.Time += FWoTControllerUIInput::FirstRepeatDelay + 0.5;
		Scope.Input->TickRepeat(Slate);
		Fixture.Settle();
		TestTrue(TEXT("the held press does not move the panel's highlight"),
			Panel->GetHighlight() == Highlight);
		TestFalse(TEXT("and arms no repeat"), Scope.Input->IsRepeatArmed(0));
		Slate.ProcessKeyUpEvent(WoTUITest_Key(EKeys::Gamepad_DPad_Up));
		Fixture.Settle();
		TestTrue(TEXT("the release leaves the panel open"), FWoTTextEntry::IsActive());
		Fixture.Type(TEXT("gg"));
		Fixture.Press(EKeys::Gamepad_Special_Right);
		TestEqual(TEXT("the line is TeamTalk's"), Controller->GetLastSubmittedChatLineForTesting(),
			FString(TEXT("TeamSay gg")));
	}
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	Fixture.Press(EKeys::Y);
	Fixture.Type(TEXT("gg"));
	Fixture.Press(EKeys::Enter);
	TestEqual(TEXT("keyboard team chat (Y) sends the same line"),
		Controller->GetLastSubmittedChatLineForTesting(), FString(TEXT("TeamSay gg")));
	WoTChatTest_Frames(Fixture, 2);

	// With Steam's keyboard turned on (the Deck), the tap opens it instead.
	{
		FWoTEntryTest_Steam Steam;
		WoTChatTest_Send(Controller, EKeys::Gamepad_DPad_Up, IE_Pressed);
		WoTChatTest_Frames(Fixture, 6);
		WoTChatTest_Send(Controller, EKeys::Gamepad_DPad_Up, IE_Released);
		WoTChatTest_Frames(Fixture, 2);
		Chat = Controller->GetChatInputWidget();
		TestTrue(TEXT("the tap opens chat"), Controller->IsChatTyping() && Chat.IsValid());
		TestTrue(TEXT("on Steam's keyboard"), FWoTTextEntry::GetActiveRoute().IsSet()
			&& FWoTTextEntry::GetActiveRoute().GetValue() == EWoTTextEntryRoute::SteamKeyboard);
		TestEqual(TEXT("Steam's keyboard was asked for once"), Steam.Launched.Num(), 1);
		TestTrue(TEXT("Steam's keyboard types into the chat line"),
			Chat.IsValid() && Slate.GetKeyboardFocusedWidget() == Chat->TextWidget());
		Fixture.Type(TEXT("ok"));
		Fixture.Press(EKeys::Enter);
		TestEqual(TEXT("Steam's Enter sends Talk's line"),
			Controller->GetLastSubmittedChatLineForTesting(), FString(TEXT("Say ok")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChatControllerInGameMenuSpec,
	"WoT.Chat.Controller.InGameMenu", WoTUITest_Flags)

bool FWoTChatControllerInGameMenuSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FSlateApplication& Slate = FSlateApplication::Get();
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	FWoTEntryTest_ChatFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	AWoTTestLeftMessageCaptureController* Controller = Fixture.Controller;
	Controller->ToggleInGameMenu();
	const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	// Positions come from the runtime list.
	const int32 ChatIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Chat);
	const int32 TeamChatIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::TeamChat);
	if (!TestTrue(TEXT("the menu has Chat and Team Chat"),
		ChatIndex != INDEX_NONE && TeamChatIndex != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("labelled Chat"), Overlay->MenuLabelsForTesting()[ChatIndex],
		FString(TEXT("Chat")));
	TestEqual(TEXT("labelled Team Chat"), Overlay->MenuLabelsForTesting()[TeamChatIndex],
		FString(TEXT("Team Chat")));

	// Pointer mode: Chat opens plain keyboard chat, with no panel [M6].
	Overlay->ActivateVisibleMainMenuEntryForTesting(ChatIndex);
	Fixture.Settle();
	TestFalse(TEXT("Chat closes the menu"), Controller->IsInGameMenuOpen());
	TSharedPtr<SWoTChatInputOverlay> Chat = Controller->GetChatInputWidget();
	if (!TestTrue(TEXT("and opens chat"), Controller->IsChatTyping() && Chat.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("as T opens it: Say:"), Chat->GetLabel().ToString(), FString(TEXT("Say:")));
	TestFalse(TEXT("with the pointer, no keyboard panel"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("keyboard chat's input router runs"),
		Controller->IsChatInputRouterActiveForTesting());
	TestTrue(TEXT("the chat line has focus"), Slate.GetKeyboardFocusedWidget() == Chat->TextWidget());
	Fixture.Type(TEXT("ok"));
	Fixture.Press(EKeys::Enter);
	TestEqual(TEXT("the keyboard line is Talk's"), Controller->GetLastSubmittedChatLineForTesting(),
		FString(TEXT("Say ok")));

	// Controller mode: Team Chat opens TeamTalk's line and its panel together.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Controller->ToggleInGameMenu();
	TestTrue(TEXT("the menu is open again"), Controller->IsInGameMenuOpen());
	Overlay->ActivateVisibleMainMenuEntryForTesting(TeamChatIndex);
	Fixture.Settle();
	TestFalse(TEXT("Team Chat closes the menu"), Controller->IsInGameMenuOpen());
	Chat = Controller->GetChatInputWidget();
	TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!TestTrue(TEXT("and opens chat with the keyboard panel"),
		Controller->IsChatTyping() && Chat.IsValid() && Panel.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("as Y opens it: Team say:"), Chat->GetLabel().ToString(),
		FString(TEXT("Team say:")));
	TestTrue(TEXT("the panel has focus"), Panel->HasKeyboardFocus());
	Fixture.Type(TEXT("gg"));
	Fixture.Press(EKeys::Gamepad_Special_Right);
	TestEqual(TEXT("the line is TeamTalk's"), Controller->GetLastSubmittedChatLineForTesting(),
		FString(TEXT("TeamSay gg")));

	// Controller mode Chat: Talk's line and the panel; B cancels only the
	// panel, and View on the panel does nothing.
	Controller->ToggleInGameMenu();
	Overlay->ActivateVisibleMainMenuEntryForTesting(ChatIndex);
	Fixture.Settle();
	Chat = Controller->GetChatInputWidget();
	TestTrue(TEXT("Chat opens Talk's line with the panel"),
		Chat.IsValid() && Chat->GetLabel().ToString() == TEXT("Say:")
		&& FWoTTextEntry::GetPanel().IsValid());
	Fixture.Press(EKeys::Gamepad_Special_Left);
	TestTrue(TEXT("View on the panel does nothing"),
		FWoTTextEntry::IsActive() && Controller->IsChatTyping());
	Fixture.Press(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("B cancels the panel"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and chat stays open"), Controller->IsChatTyping());
	// On the bare line the prompts say how to type or leave.
	if (const TSharedPtr<SWoTChatInputOverlay> Bare = Controller->GetChatInputWidget())
	{
		TestEqual(TEXT("the bare chat line prompts A Type and B Cancel"),
			Bare->ButtonPromptsForTesting().IsValid()
				? FWoTButtonPrompt::Describe(Bare->ButtonPromptsForTesting()->GetShownPrompts())
				: FString(),
			FWoTButtonPrompt::Describe(FWoTButtonPromptSets::Password()));
	}
	// A second View (or B) closes chat without sending anything.
	Controller->ResetChatCaptureForTesting();
	Fixture.Press(EKeys::Gamepad_Special_Left);
	TestFalse(TEXT("a second View closes chat"), Controller->IsChatTyping());
	TestTrue(TEXT("and sends nothing"), Controller->GetLastSubmittedChatLineForTesting().IsEmpty());

	// B does the same after a D-pad-style open: the panel first, then chat.
	Controller->ChatByController(/*bTeam*/ false);
	Fixture.Settle();
	TestTrue(TEXT("chat reopens with the panel"),
		Controller->IsChatTyping() && FWoTTextEntry::IsActive());
	Fixture.Press(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("B closes the panel"), FWoTTextEntry::IsActive());
	TestTrue(TEXT("and chat stays"), Controller->IsChatTyping());
	Fixture.Press(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("then B closes chat"), Controller->IsChatTyping());
	TestTrue(TEXT("still nothing sent"), Controller->GetLastSubmittedChatLineForTesting().IsEmpty());
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInGameMenuTauntsSpec,
	"WoT.FrontEnd.InGameMenu.Taunts", WoTUITest_Flags)

bool FWoTInGameMenuTauntsSpec::RunTest(const FString&)
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
	const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	FWoTUITest_Window Shown(Overlay);
	FSlateApplication& Slate = FSlateApplication::Get();
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform()),
		0.0, 0.0f);
	Shown.Settle();
	Overlay->FocusResumeButton();
	Shown.Settle();

	// The fixed order (spec r3 section 4 [I9]): Chat, Team Chat and Taunts right
	// after Help, before Quit. Positions come from the runtime list.
	const TArray<FString> Labels = Overlay->MenuLabelsForTesting();
	const int32 HelpIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Help);
	const int32 ChatIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Chat);
	const int32 TeamChatIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::TeamChat);
	const int32 TauntsIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Taunts);
	const int32 QuitIndex = Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Quit);
	if (!TestTrue(TEXT("Chat, Team Chat and Taunts follow Help in that order"),
		HelpIndex != INDEX_NONE && ChatIndex == HelpIndex + 1
		&& TeamChatIndex == HelpIndex + 2 && TauntsIndex == HelpIndex + 3))
	{
		return false;
	}
	TestTrue(TEXT("before Quit, which stays last"),
		TauntsIndex < QuitIndex && QuitIndex == Labels.Num() - 1);
	TestEqual(TEXT("labelled Taunts"), Labels[TauntsIndex], FString(TEXT("Taunts")));

	// The D-pad walks to Taunts; A opens its page on Taunt 1.
	for (int32 Step = 0; Step < TauntsIndex; ++Step)
	{
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	}
	TestEqual(TEXT("the D-pad reaches Taunts"), Overlay->SelectedMainMenuIndexForTesting(),
		TauntsIndex);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	if (!TestTrue(TEXT("A opens the Taunts page"), Overlay->IsShowingTauntsForTesting()))
	{
		return false;
	}
	const TArray<FString> ExpectedTaunts = {TEXT("Taunt 1"), TEXT("Taunt 2"), TEXT("Taunt 3"),
		TEXT("Taunt 4"), TEXT("Taunt 5"), TEXT("Taunt 6"), TEXT("Random")};
	TestTrue(TEXT("Taunt 1 to Taunt 6, then Random"),
		Overlay->TauntLabelsForTesting() == ExpectedTaunts);
	auto TauntFocused = [&Slate, &Overlay](int32 Index)
	{
		return Overlay->SelectedTauntIndexForTesting() == Index
			&& Slate.GetKeyboardFocusedWidget() == Overlay->TauntButtonForTesting(Index);
	};
	TestTrue(TEXT("the page opens on Taunt 1"), TauntFocused(0));
	TestTrue(TEXT("the menu stays open on the page"), Controller->IsInGameMenuOpen());
	TestTrue(TEXT("opening the page plays nothing"),
		Controller->GetPlayTauntRequestsForTesting().IsEmpty());

	// Its prompts (spec r3 section 1a): A Taunt, B Back, in Controller mode.
	const TSharedPtr<SWoTButtonPrompts> Strip = Overlay->ButtonPromptsForTesting();
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	Shown.Settle();
	TestEqual(TEXT("the Taunts page's prompts"),
		Strip.IsValid() ? FWoTButtonPrompt::Describe(Strip->GetShownPrompts()) : FString(),
		FString(TEXT("A Taunt | B Back")));

	// Seven entries wrap, for the D-pad, the arrows and the stick.
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("D-pad up wraps from Taunt 1 to Random"), TauntFocused(6));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("D-pad down wraps from Random to Taunt 1"), TauntFocused(0));
	WoTWidgetTest_Press(Shown, EKeys::Down);
	TestTrue(TEXT("Down moves to Taunt 2"), TauntFocused(1));
	WoTWidgetTest_Press(Shown, EKeys::Up);
	WoTWidgetTest_Press(Shown, EKeys::Up);
	TestTrue(TEXT("Up wraps to Random"), TauntFocused(6));
	const FNavigationEvent StickDown(FModifierKeysState(), 0, EUINavigation::Down,
		ENavigationGenesis::Controller);
	Overlay->OnNavigation(Overlay->GetCachedGeometry(), StickDown);
	Shown.Settle();
	TestTrue(TEXT("the stick wraps the same way"), TauntFocused(0));

	// Escape returns to the menu, matching B [M3]; B and View back out too.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	TestTrue(TEXT("Escape is handled"), WoTWidgetTest_Press(Shown, EKeys::Escape));
	TestTrue(TEXT("Escape on Taunts returns to the menu"),
		Overlay->IsShowingMainMenuForTesting() && !Overlay->IsShowingTauntsForTesting());
	TestTrue(TEXT("and does not resume"), Controller->IsInGameMenuOpen());
	for (const FKey& Back : {EKeys::Gamepad_FaceButton_Right, EKeys::Gamepad_Special_Left})
	{
		Overlay->OpenTauntsForTesting();
		Shown.Settle();
		TestTrue(*FString::Printf(TEXT("%s is handled on Taunts"), *Back.ToString()),
			WoTWidgetTest_Press(Shown, Back));
		TestTrue(*FString::Printf(TEXT("%s returns to the menu"), *Back.ToString()),
			Overlay->IsShowingMainMenuForTesting());
		TestTrue(*FString::Printf(TEXT("%s does not resume"), *Back.ToString()),
			Controller->IsInGameMenuOpen());
	}

	// A on an entry: the menu closes and PlayTaunt gets that entry's argument.
	Overlay->OpenTauntsForTesting();
	Shown.Settle();
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("Taunt 3 is selected"), TauntFocused(2));
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestFalse(TEXT("a taunt closes the menu"), Controller->IsInGameMenuOpen());
	TestTrue(TEXT("Taunt 3 calls PlayTaunt(3)"),
		Controller->GetPlayTauntRequestsForTesting() == TArray<int32>({3}));
	for (int32 Entry = 0; Entry < ExpectedTaunts.Num(); ++Entry)
	{
		Controller->ToggleInGameMenu();
		Overlay->OpenTauntsForTesting();
		Overlay->ActivateTauntEntryForTesting(Entry);
		TestFalse(*FString::Printf(TEXT("%s closes the menu"), *ExpectedTaunts[Entry]),
			Controller->IsInGameMenuOpen());
	}
	TestTrue(TEXT("Taunt 1..6 call PlayTaunt(1..6) and Random PlayTaunt(-1)"),
		Controller->GetPlayTauntRequestsForTesting()
			== TArray<int32>({3, 1, 2, 3, 4, 5, 6, -1}));
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Controller sub-project 3, PR 6: the owner's first Steam Deck session
// (2026-11-09): the prompt glyphs and sizes, the keyboard panel's size and
// its dark strip, the settings' "Save changes?" prompt and the game menu's
// fit. Layouts are measured as the game viewport draws them: at each
// resolution's UI DPI scale (UUserInterfaceSettings::GetDPIScaleBasedOnSize).

namespace
{
// The Steam Deck, its 720p dock and TV output, and the 1080p desktop baseline.
const FVector2D WoTDeckTest_Resolutions[] = {
	FVector2D(1280.0, 800.0), FVector2D(1280.0, 720.0), FVector2D(1920.0, 1080.0)};

FString WoTDeckTest_Label(const FVector2D& Resolution)
{
	return FString::Printf(TEXT("%gx%g"), Resolution.X, Resolution.Y);
}

/** Every visible widget of Type under Widget (Widget included). */
void WoTDeckTest_Collect(const TSharedRef<SWidget>& Widget, FName Type,
	TArray<TSharedRef<SWidget>>& Out)
{
	if (!Widget->GetVisibility().IsVisible())
	{
		return;
	}
	if (Widget->GetType() == Type)
	{
		Out.Add(Widget);
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		WoTDeckTest_Collect(Children->GetChildAt(Index), Type, Out);
	}
}

/**
 * A host laid out as the game viewport lays it out at Resolution: its UI DPI
 * scale, in a real window, ticked and painted (painting arranges every widget,
 * so cached geometry is in physical pixels).
 */
struct FWoTDeckTest_Screen
{
	TSharedRef<SWidget> Root;
	FVector2D Resolution;
	float Dpi = 1.0f;
	TSharedRef<SWindow> Window;
	FGeometry Geometry;

	FWoTDeckTest_Screen(const TSharedRef<SWidget>& InRoot, const FVector2D& InResolution)
		: Root(InRoot)
		, Resolution(InResolution)
		, Dpi(FWoTMenuStyle::ViewportDpiScale(InResolution))
		, Window(SNew(SWindow)
			.ClientSize(InResolution)
			.CreateTitleBar(false)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[InRoot])
		, Geometry(FGeometry::MakeRoot(InResolution / Dpi, FSlateLayoutTransform(Dpi)))
	{
		FSlateApplication::Get().AddWindow(Window, false);
		Settle();
	}

	~FWoTDeckTest_Screen()
	{
		FSlateApplication::Get().RequestDestroyWindow(Window);
		FSlateApplication::Get().Tick();
	}

	void Settle(int32 Passes = 4)
	{
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			Root->Tick(Geometry, FApp::GetCurrentTime(), 0.0f);
			Root->SlatePrepass(Dpi);
			FHittestGrid Grid;
			FSlateWindowElementList Elements(Window);
			const FPaintArgs Args(&Window.Get(), Grid, FVector2f::ZeroVector,
				FApp::GetCurrentTime(), 0.0f);
			Root->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(), Elements, 0,
				FWidgetStyle(), true);
		}
	}

	/** A widget's rectangle on screen, in physical pixels. */
	static FSlateRect Rect(const TSharedPtr<SWidget>& Widget)
	{
		return Widget.IsValid() ? Widget->GetCachedGeometry().GetLayoutBoundingRect()
			: FSlateRect(0.0f, 0.0f, 0.0f, 0.0f);
	}

	bool Contains(const FSlateRect& Inner, float Margin = 0.0f) const
	{
		return Inner.Left >= Margin - 0.5f && Inner.Top >= Margin - 0.5f
			&& Inner.Right <= Resolution.X - Margin + 0.5f
			&& Inner.Bottom <= Resolution.Y - Margin + 0.5f;
	}
};

FString WoTDeckTest_RectText(const FSlateRect& Rect)
{
	return FString::Printf(TEXT("(%.0f,%.0f)-(%.0f,%.0f)"), Rect.Left, Rect.Top, Rect.Right,
		Rect.Bottom);
}

/** One host's prompt strip on screen: labels at least 16 physical pixels,
 * glyphs at least 1.5 cap heights (Roboto's cap height is 0.72 em), and the
 * strip inside the screen. Logs the sizes before and after the floor. */
void WoTDeckTest_CheckStrip(FAutomationTestBase& Test, const FString& Label,
	const TSharedPtr<SWoTButtonPrompts>& Strip, const FWoTDeckTest_Screen& Screen)
{
	if (!Test.TestTrue(Label + TEXT(": the prompt strip shows"), Strip.IsValid()
		&& Strip->GetVisibility().IsVisible() && !Strip->GetShownPrompts().IsEmpty()))
	{
		return;
	}
	TArray<TSharedRef<SWidget>> Texts;
	TArray<TSharedRef<SWidget>> Glyphs;
	WoTDeckTest_Collect(Strip.ToSharedRef(), FName(TEXT("STextBlock")), Texts);
	WoTDeckTest_Collect(Strip.ToSharedRef(), FName(TEXT("SWoTPromptGlyph")), Glyphs);
	const int32 Shown = Strip->GetShownPrompts().Num();
	Test.TestEqual(Label + TEXT(": one label per prompt"), Texts.Num(), Shown);
	Test.TestEqual(Label + TEXT(": one glyph per prompt"), Glyphs.Num(), Shown);
	float SmallestLabel = TNumericLimits<float>::Max();
	float LargestEm = 0.0f;
	for (const TSharedRef<SWidget>& Text : Texts)
	{
		const float Scale = Text->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale();
		const float Pixels = FWoTMenuStyle::PhysicalTextPixels(
			StaticCastSharedRef<STextBlock>(Text)->GetFont().Size, Scale);
		SmallestLabel = FMath::Min(SmallestLabel, Pixels);
		LargestEm = FMath::Max(LargestEm, Pixels);
	}
	float SmallestGlyph = TNumericLimits<float>::Max();
	for (const TSharedRef<SWidget>& Glyph : Glyphs)
	{
		const FSlateRect Rect = FWoTDeckTest_Screen::Rect(Glyph);
		SmallestGlyph = FMath::Min(SmallestGlyph,
			FMath::Min(Rect.GetSize().X, Rect.GetSize().Y));
	}
	Test.TestTrue(FString::Printf(TEXT("%s: labels are at least %g physical pixels (%.2f)"),
		*Label, SWoTButtonPrompts::MinimumLabelPixels, SmallestLabel),
		SmallestLabel >= SWoTButtonPrompts::MinimumLabelPixels - 0.01f);
	Test.TestTrue(FString::Printf(
		TEXT("%s: glyphs are at least 1.5 label cap heights (%.1f px for %.1f px text)"),
		*Label, SmallestGlyph, LargestEm), SmallestGlyph >= 1.5f * 0.72f * LargestEm - 0.5f);
	const FSlateRect StripRect = FWoTDeckTest_Screen::Rect(Strip);
	Test.TestTrue(FString::Printf(TEXT("%s: the strip stays on screen %s"), *Label,
		*WoTDeckTest_RectText(StripRect)), Screen.Contains(StripRect));
	// What the strip drew before PR 6: the bound face unfloored, 1.4 per point.
	const float Scale = Strip->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale();
	const FSlateFontInfo Bound = Strip->BoundFontForTesting();
	const float BoundSize = Bound.HasValidFont() ? Bound.Size : 14.0f;
	Test.AddInfo(FString::Printf(
		TEXT("%s: prompt labels %.1f px (were %.1f px), glyphs %.1f px (were %.1f px), strip %s"),
		*Label, SmallestLabel, FWoTMenuStyle::PhysicalTextPixels(BoundSize, Scale), SmallestGlyph,
		FMath::Max(14.0f, BoundSize * 1.4f) * Scale, *WoTDeckTest_RectText(StripRect)));
}

/** The keyboard panel open in a host: full doubled size where it fits, inside
 * the screen with a margin, directly above a full-width dark strip that holds
 * the host's prompt strip, labels at least 16 physical pixels, and the draft
 * visible however long it grows. */
void WoTDeckTest_CheckPanel(FAutomationTestBase& Test, const FString& Label,
	FWoTDeckTest_Screen& Screen, const TSharedPtr<SWoTTextEntryLayer>& Layer,
	const TSharedPtr<SWoTButtonPrompts>& Prompts)
{
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
	if (!Test.TestTrue(Label + TEXT(": the panel shows in the host's layer"),
		Panel.IsValid() && Layer.IsValid() && Layer->IsPanelShown()))
	{
		return;
	}
	const FVector2D Resolution = Screen.Resolution;
	const float Dpi = Screen.Dpi;
	constexpr float Margin = 8.0f;
	const FSlateRect PanelRect = FWoTDeckTest_Screen::Rect(Panel);
	const FSlateRect StripRect = FWoTDeckTest_Screen::Rect(Layer->BottomStripForTesting());
	const FIntPoint Q = SWoTOnScreenKeyboard::FindKey(TEXT("q"));
	const FSlateRect KeyRect = FWoTDeckTest_Screen::Rect(Panel->GetKeyWidget(Q.Y, Q.X));
	// The layer's fit: 1 is the panel's full (doubled) size.
	const float Fit = PanelRect.GetSize().X / (SWoTOnScreenKeyboard::PanelWidth() * Dpi);
	const float LabelPixels = FWoTMenuStyle::PhysicalTextPixels(Panel->KeyFontForTesting().Size,
		Panel->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale());
	Test.AddInfo(FString::Printf(TEXT("%s: UI DPI scale %.4f; panel %.0f x %.0f px at %.3f of its ")
		TEXT("doubled size; keys %.1f x %.1f px (first layout %.1f x %.1f); key labels %.1f px; ")
		TEXT("dark strip %.1f px"), *Label, Dpi, PanelRect.GetSize().X, PanelRect.GetSize().Y, Fit,
		KeyRect.GetSize().X, KeyRect.GetSize().Y, 56.0f * Dpi, 50.0f * Dpi, LabelPixels,
		StripRect.GetSize().Y));
	if (Resolution.X >= 1280.0)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: the panel keeps its full doubled size (%.3f)"),
			*Label, Fit), FMath::IsNearlyEqual(Fit, 1.0f, 0.01f));
		Test.TestTrue(FString::Printf(TEXT("%s: keys are twice the first layout's (%.1f px tall)"),
			*Label, KeyRect.GetSize().Y),
			FMath::IsNearlyEqual(KeyRect.GetSize().Y, 2.0f * 50.0f * Dpi, 1.0f)
			&& FMath::IsNearlyEqual(KeyRect.GetSize().X, 2.0f * 56.0f * Dpi, 1.0f));
	}
	Test.TestTrue(FString::Printf(TEXT("%s: the panel fits the screen with a margin %s"), *Label,
		*WoTDeckTest_RectText(PanelRect)), Screen.Contains(PanelRect, Margin));
	Test.TestTrue(FString::Printf(TEXT("%s: key labels are at least 16 physical pixels (%.2f)"),
		*Label, LabelPixels), LabelPixels >= 16.0f - 0.01f);

	// The dark strip: full width along the screen's bottom, near opaque, with
	// the panel directly above it and the host's prompts on it.
	Test.TestTrue(FString::Printf(TEXT("%s: the dark strip spans the screen's bottom %s"), *Label,
		*WoTDeckTest_RectText(StripRect)),
		StripRect.Left <= 0.5f && StripRect.Right >= Resolution.X - 0.5f
		&& FMath::IsNearlyEqual(StripRect.Bottom, static_cast<float>(Resolution.Y), 0.5f)
		&& StripRect.GetSize().Y > 0.0f);
	Test.TestTrue(TEXT("the dark strip is at least 90% opaque"),
		SWoTTextEntryLayer::BottomStripColor().A >= 0.9f);
	Test.TestTrue(FString::Printf(TEXT("%s: the panel sits directly above the strip (%.1f px gap)"),
		*Label, StripRect.Top - PanelRect.Bottom),
		PanelRect.Bottom <= StripRect.Top + 0.5f
		&& StripRect.Top - PanelRect.Bottom <= 8.0f * Dpi + 1.0f);
	if (Test.TestTrue(Label + TEXT(": the host's prompts show the panel's set"), Prompts.IsValid()
		&& FWoTButtonPrompt::Describe(Prompts->GetShownPrompts())
			== FWoTButtonPrompt::Describe(FWoTButtonPromptSets::KeyboardPanel())))
	{
		const FSlateRect PromptRect = FWoTDeckTest_Screen::Rect(Prompts);
		Test.TestTrue(FString::Printf(TEXT("%s: the prompts lie on the strip %s in %s"), *Label,
			*WoTDeckTest_RectText(PromptRect), *WoTDeckTest_RectText(StripRect)),
			PromptRect.Top >= StripRect.Top - 0.5f && PromptRect.Bottom <= StripRect.Bottom + 0.5f
			&& Screen.Contains(PromptRect));
		WoTDeckTest_CheckStrip(Test, Label + TEXT(" keyboard prompts"), Prompts, Screen);
	}

	// The draft stays visible: a full-length draft wraps inside the panel,
	// which keeps its width and stays on screen.
	const TSharedPtr<SWidget> Preview = Panel->PreviewForTesting();
	Test.TestTrue(Label + TEXT(": the draft's preview is on the panel"),
		FWoTDeckTest_Screen::Rect(Preview).GetSize().Y > 0.0f
		&& FWoTDeckTest_Screen::Rect(Preview).Top >= PanelRect.Top - 0.5f);
	const int32 Longest = Panel->GetMaxLength() > 0 ? Panel->GetMaxLength() : 64;
	Panel->SetDraft(FString::ChrN(Longest, TEXT('W')));
	Screen.Settle();
	const FSlateRect LongPanel = FWoTDeckTest_Screen::Rect(Panel);
	const FSlateRect LongPreview = FWoTDeckTest_Screen::Rect(Preview);
	Test.TestTrue(FString::Printf(TEXT("%s: a %d-character draft keeps the panel's width and the ")
		TEXT("screen %s"), *Label, Longest, *WoTDeckTest_RectText(LongPanel)),
		FMath::IsNearlyEqual(LongPanel.GetSize().X, PanelRect.GetSize().X, 1.0f)
		&& Screen.Contains(LongPanel, Margin));
	Test.TestTrue(FString::Printf(TEXT("%s: and wraps inside the panel %s"), *Label,
		*WoTDeckTest_RectText(LongPreview)),
		LongPreview.Left >= LongPanel.Left - 0.5f && LongPreview.Right <= LongPanel.Right + 0.5f
		&& LongPreview.Top >= LongPanel.Top - 0.5f && LongPreview.Bottom <= LongPanel.Bottom + 0.5f);
}

/** A Battle citadel edit phase: the game menu's longest entry list. */
AWoTBattleInfo* WoTDeckTest_SpawnEditPhase(UWorld* World)
{
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	if (Battle == nullptr)
	{
		return nullptr;
	}
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTCharacter::StaticClass();
	Battle->TeamClass[1] = AWoTCharacter::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	Battle->bTeleportersDisabled = true;
	return Battle;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeckPromptGlyphFramesSpec,
	"WoT.UI.ButtonPrompts.GlyphFrames", WoTUITest_Flags)

bool FWoTDeckPromptGlyphFramesSpec::RunTest(const FString&)
{
	// On the Deck the Menu and View glyphs drew as solid white boxes:
	// FSlateDrawElement::MakeBox fills a rounded box with its tint argument,
	// never the brush's own tint, so the frame's "transparent" fill came out
	// opaque over the lines inside. Each glyph is painted headless here and
	// what it drew is read back from the element list.
	const float Side = 48.0f;
	const TSharedRef<SWindow> Window = SNew(SWindow).ClientSize(FVector2D(128.0f, 128.0f));
	struct FDrawn
	{
		FSlateRect Rect;
		float Alpha = 0.0f;
	};
	auto Paint = [&](EWoTPromptButton Button)
	{
		const TSharedRef<SWidget> Glyph = SWoTButtonPrompts::MakeGlyphForTesting(Button, Side);
		const FGeometry Geometry = FGeometry::MakeRoot(FVector2D(Side, Side), FSlateLayoutTransform());
		Glyph->SlatePrepass(1.0f);
		FHittestGrid Grid;
		FSlateWindowElementList Elements(Window);
		const FPaintArgs Args(&Window.Get(), Grid, FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
		Glyph->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(), Elements, 0,
			FWidgetStyle(), true);
		TArray<FDrawn> Drawn;
		auto Collect = [&Drawn](const auto& Array)
		{
			for (const auto& Element : Array)
			{
				const FVector2f Size = Element.GetLocalSize();
				const FVector2f TopLeft = Element.GetRenderTransform().TransformPoint(FVector2f::ZeroVector);
				const FVector2f BottomRight = Element.GetRenderTransform().TransformPoint(Size);
				FDrawn& Entry = Drawn.AddDefaulted_GetRef();
				Entry.Rect = FSlateRect(TopLeft.X, TopLeft.Y, BottomRight.X, BottomRight.Y);
				Entry.Alpha = Element.GetTint().A;
			}
		};
		const FSlateDrawElementMap& Map = Elements.GetUncachedDrawElements();
		Collect(Map.Get<(uint8)EElementType::ET_Box>());
		Collect(Map.Get<(uint8)EElementType::ET_RoundedBox>());
		return Drawn;
	};
	auto Area = [](const FSlateRect& Rect) { return Rect.GetSize().X * Rect.GetSize().Y; };

	// The control: A's disc is one filled shape over its whole face.
	const TArray<FDrawn> A = Paint(EWoTPromptButton::A);
	TestTrue(TEXT("the capture sees A's filled disc"), A.ContainsByPredicate(
		[&](const FDrawn& Drawn) { return Drawn.Alpha > 0.5f && Area(Drawn.Rect) >= 0.8f * Side * Side; }));

	// Menu and View: a hollow frame of four edges, the lines or windows inside
	// it, and no opaque shape over the frame's whole area.
	const FSlateRect Frame(Side * 0.08f, Side * 0.2f, Side * 0.92f, Side * 0.8f);
	for (const EWoTPromptButton Button : {EWoTPromptButton::Menu, EWoTPromptButton::View})
	{
		const FString Name = FWoTButtonPrompt::ButtonName(Button);
		const TArray<FDrawn> Drawn = Paint(Button);
		int32 Inside = 0;
		for (const FDrawn& Shape : Drawn)
		{
			const bool bCovers = Shape.Alpha > 0.1f && Area(Shape.Rect) >= 0.5f * Area(Frame);
			TestFalse(FString::Printf(TEXT("%s: no opaque shape covers the frame %s (alpha %.2f)"),
				*Name, *WoTDeckTest_RectText(Shape.Rect), Shape.Alpha), bCovers);
			const bool bInterior = Shape.Rect.Left > Frame.Left + 1.0f && Shape.Rect.Right < Frame.Right - 1.0f
				&& Shape.Rect.Top > Frame.Top + 1.0f && Shape.Rect.Bottom < Frame.Bottom - 1.0f;
			Inside += bInterior && Shape.Alpha > 0.5f ? 1 : 0;
		}
		TestTrue(FString::Printf(TEXT("%s: the frame and what is inside it draw (%d shapes)"),
			*Name, Drawn.Num()), Drawn.Num() >= 7);
		TestTrue(FString::Printf(TEXT("%s: its inner shapes are drawn inside the frame (%d)"),
			*Name, Inside), Inside >= 3);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeckPromptLegibilitySpec,
	"WoT.UI.ButtonPrompts.Legibility", WoTUITest_Flags)

bool FWoTDeckPromptLegibilitySpec::RunTest(const FString&)
{
	// Every prompt strip's host, as the game lays it out at the Deck's
	// 1280x800, its 720p output and 1080p.
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
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
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	for (const FVector2D& Resolution : WoTDeckTest_Resolutions)
	{
		const FString Res = WoTDeckTest_Label(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		{
			const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
			const TSharedRef<SWoTFrontEndRoot> Root = WoTUITest_MakeRoot(Authority);
			FWoTDeckTest_Screen Screen(Root, Resolution);
			WoTDeckTest_CheckStrip(*this, Res + TEXT(" front end"), Root->ButtonPromptsForTesting(),
				Screen);
		}
		{
			const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
				.PlayerController(Controller)
				.RequiredFontsForTesting(WoTUITest_Fonts());
			FWoTDeckTest_Screen Screen(Overlay, Resolution);
			WoTDeckTest_CheckStrip(*this, Res + TEXT(" game menu"), Overlay->ButtonPromptsForTesting(),
				Screen);
		}
		{
			FWoTHelpContent Content;
			Content.Title = TEXT("Help");
			const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
				.Content(Content)
				.InitialPhysicalSize(Resolution)
				.InitialDpiScale(Dpi)
				.RequiredFontsForTesting(WoTUITest_Fonts());
			FWoTDeckTest_Screen Screen(Help, Resolution);
			WoTDeckTest_CheckStrip(*this, Res + TEXT(" Help"), Help->ButtonPromptsForTesting(), Screen);
		}
		{
			// The chat line, on the bare line and with Steam's keyboard typing
			// (the "Keyboard panel" and "Cancel" prompts the owner saw).
			const TSharedRef<SWoTChatInputOverlay> Chat = SNew(SWoTChatInputOverlay)
				.PlayerController(nullptr)
				.InitialText(TEXT("Say "));
			FWoTDeckTest_Screen Screen(Chat, Resolution);
			WoTDeckTest_CheckStrip(*this, Res + TEXT(" chat line"), Chat->ButtonPromptsForTesting(),
				Screen);
			FWoTEntryTest_Steam Steam;
			if (TestTrue(Res + TEXT(": Steam's keyboard opens on the chat line"),
				Chat->BeginTextEntry(0)
				&& FWoTTextEntry::GetActiveRoute() == TOptional<EWoTTextEntryRoute>(
					EWoTTextEntryRoute::SteamKeyboard)))
			{
				Screen.Settle();
				TestEqual(Res + TEXT(": the chat line's Steam prompts"),
					FWoTButtonPrompt::Describe(Chat->ButtonPromptsForTesting()->GetShownPrompts()),
					FWoTButtonPrompt::Describe(FWoTButtonPromptSets::SteamKeyboard()));
				WoTDeckTest_CheckStrip(*this, Res + TEXT(" chat line with Steam's keyboard"),
					Chat->ButtonPromptsForTesting(), Screen);
			}
			FWoTTextEntry::ResetForTesting();
		}
		{
			const TSharedRef<SWoTBootstrapScreen> Bootstrap = SNew(SWoTBootstrapScreen)
				.Snapshot(FWoTBootstrapSnapshot());
			FWoTDeckTest_Screen Screen(Bootstrap, Resolution);
			WoTDeckTest_CheckStrip(*this, Res + TEXT(" setup screen"),
				Bootstrap->ButtonPromptsForTesting(), Screen);
		}
	}
	// The floor itself: 16 physical pixels at the live scale, never smaller
	// than the bound face, a whole point size.
	for (const FVector2D& Resolution : WoTDeckTest_Resolutions)
	{
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const FSlateFontInfo Small = SWoTButtonPrompts::LegibleLabelFont(
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8), Dpi);
		TestTrue(WoTDeckTest_Label(Resolution) + TEXT(": a small face is raised to the floor"),
			FWoTMenuStyle::PhysicalTextPixels(Small.Size, Dpi) >= 16.0f
			&& FWoTMenuStyle::PhysicalTextPixels(Small.Size - 1.0f, Dpi) < 16.0f);
		TestEqual(WoTDeckTest_Label(Resolution) + TEXT(": a large face is kept"),
			SWoTButtonPrompts::LegibleLabelFont(
				FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 40), Dpi).Size, 40.0f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeckKeyboardPanelFitSpec,
	"WoT.UI.TextEntry.PanelFit", WoTUITest_Flags)

bool FWoTDeckKeyboardPanelFitSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	for (const FVector2D& Resolution : WoTDeckTest_Resolutions)
	{
		const FString Res = WoTDeckTest_Label(Resolution);
		{
			// The front end's Play screen, where the owner saw the join list's
			// text through the prompts.
			const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
			const TSharedRef<SWoTFrontEndRoot> Root = WoTUITest_MakeRoot(Authority);
			FWoTDeckTest_Screen Screen(Root, Resolution);
			Root->ActivateMainMenuEntryForTesting(0);
			Screen.Settle();
			const TSharedPtr<SWidget> Field = Root->ServerNameTextBoxForTesting();
			FString Text;
			FWoTTextEntryRequest Request;
			Request.Title = FText::FromString(TEXT("Server Name"));
			Request.MaxLength = 32;
			Request.GetText = [&Text]() { return Text; };
			Request.SetText = [&Text](const FString& Value) { Text = Value; };
			if (TestTrue(Res + TEXT(": the panel opens over Play"), Field.IsValid()
				&& FWoTTextEntry::Begin(Request, Field.ToSharedRef(), 0, EWoTTextEntryRoute::Panel)))
			{
				Screen.Settle();
				WoTDeckTest_CheckPanel(*this, Res + TEXT(" front end"), Screen,
					Root->TextEntryLayerForTesting(), Root->ButtonPromptsForTesting());
			}
			FWoTTextEntry::ResetForTesting();
		}
		{
			// The chat line: the smallest host face, and the longest draft.
			const TSharedRef<SWoTChatInputOverlay> Chat = SNew(SWoTChatInputOverlay)
				.PlayerController(nullptr)
				.InitialText(TEXT("Say "));
			FWoTDeckTest_Screen Screen(Chat, Resolution);
			if (TestTrue(Res + TEXT(": the panel opens over chat"), Chat->BeginTextEntry(0)))
			{
				Screen.Settle();
				WoTDeckTest_CheckPanel(*this, Res + TEXT(" chat line"), Screen,
					Chat->TextEntryLayer(), Chat->ButtonPromptsForTesting());
			}
			FWoTTextEntry::ResetForTesting();
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityChatLineSpec,
	"WoT.UI.Legibility.ChatLine", WoTUITest_Flags)

bool FWoTLegibilityChatLineSpec::RunTest(const FString&)
{
	// SP3 PR 7: on Deck-class screens the chat line, its labels, its prompts
	// and the keyboard panel over it draw the list face's crisp 2x strike (28
	// physical pixels, the front end's size), and the line's strip holds the
	// prompts beside the text. From 1080p up nothing changes.
#if PLATFORM_MAC
	// Headless editable-text focus has no native NSWindow for the macOS IME.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FWoTUITest_ModeGuard ModeGuard;
	FWoTEntryTest_Guard EntryGuard;
	const FVector2D Resolutions[] = {FVector2D(1280.0, 800.0), FVector2D(1280.0, 720.0),
		FVector2D(1920.0, 1080.0), FVector2D(2560.0, 1440.0)};
	for (const FVector2D& Resolution : Resolutions)
	{
		const FString Res = WoTDeckTest_Label(Resolution);
		const bool bDeck = Resolution.Y < 1080.0;
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
		const TSharedRef<SWoTChatInputOverlay> Chat = SNew(SWoTChatInputOverlay)
			.PlayerController(nullptr)
			.InitialText(TEXT("Say "))
			.InitialPhysicalSize(Resolution);
		FWoTDeckTest_Screen Screen(Chat, Resolution);
		const FSlateFontInfo LineFont = Chat->LineFontForTesting();
		const float LinePixels = FWoTMenuStyle::PhysicalTextPixels(LineFont.Size, Screen.Dpi);
		AddInfo(FString::Printf(TEXT("%s: chat line %.1f px (size %.2f)"), *Res, LinePixels,
			LineFont.Size));
		if (bDeck)
		{
			TestTrue(FString::Printf(TEXT("%s: the chat line draws the 2x strike, 28 px (%.2f)"),
				*Res, LinePixels), FMath::IsNearlyEqual(LinePixels, 28.0f, 0.01f));
		}
		else
		{
			TestEqual(Res + TEXT(": the chat line keeps the native list face"),
				LineFont.Size, 14.0f);
		}
		const FSlateRect PointerStrip = FWoTDeckTest_Screen::Rect(Chat->LineStripForTesting());

		// Controller mode: the prompts show at the line's right end.
		FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
		Screen.Settle();
		const FSlateRect Strip = FWoTDeckTest_Screen::Rect(Chat->LineStripForTesting());
		const FSlateRect Prompts = FWoTDeckTest_Screen::Rect(Chat->ButtonPromptsForTesting());
		const FSlateRect Typed = FWoTDeckTest_Screen::Rect(Chat->TextWidget());
		AddInfo(FString::Printf(TEXT("%s: line strip %s (%s with the mouse), prompts %s"), *Res,
			*WoTDeckTest_RectText(Strip), *WoTDeckTest_RectText(PointerStrip),
			*WoTDeckTest_RectText(Prompts)));
		if (bDeck)
		{
			TestTrue(FString::Printf(TEXT("%s: the prompts sit on the line's strip %s in %s"),
				*Res, *WoTDeckTest_RectText(Prompts), *WoTDeckTest_RectText(Strip)),
				Prompts.Top >= Strip.Top - 0.5f && Prompts.Bottom <= Strip.Bottom + 0.5f);
			TestTrue(FString::Printf(TEXT("%s: the typed text stays clear of the prompts (%.0f, %.0f)"),
				*Res, Typed.Right, Prompts.Left), Typed.Right <= Prompts.Left + 0.5f);
			const float LabelPixels = FWoTMenuStyle::PhysicalTextPixels(
				Chat->ButtonPromptsForTesting()->LabelFontForTesting().Size, Screen.Dpi);
			TestTrue(FString::Printf(TEXT("%s: the prompt labels are 28 px like the front end's (%.2f)"),
				*Res, LabelPixels), FMath::IsNearlyEqual(LabelPixels, 28.0f, 0.01f));
		}
		else
		{
			TestTrue(FString::Printf(TEXT("%s: the line's strip is as before %s"), *Res,
				*WoTDeckTest_RectText(Strip)),
				FMath::IsNearlyEqual(Strip.Top, PointerStrip.Top, 0.5f)
				&& FMath::IsNearlyEqual(Strip.Bottom, PointerStrip.Bottom, 0.5f));
		}
		WoTDeckTest_CheckStrip(*this, Res + TEXT(" chat line"), Chat->ButtonPromptsForTesting(),
			Screen);

		// The keyboard panel over chat: its key labels match the front end's.
		if (TestTrue(Res + TEXT(": the panel opens over chat"), Chat->BeginTextEntry(0)))
		{
			Screen.Settle();
			const TSharedPtr<SWoTOnScreenKeyboard> Panel = FWoTTextEntry::GetPanel();
			if (TestTrue(Res + TEXT(": the panel shows"), Panel.IsValid()))
			{
				const float KeyPixels = FWoTMenuStyle::PhysicalTextPixels(
					Panel->KeyFontForTesting().Size, Screen.Dpi);
				AddInfo(FString::Printf(TEXT("%s: keyboard panel key labels %.1f px"), *Res,
					KeyPixels));
				if (bDeck)
				{
					TestTrue(FString::Printf(TEXT("%s: key labels draw the 2x strike, 28 px (%.2f)"),
						*Res, KeyPixels), FMath::IsNearlyEqual(KeyPixels, 28.0f, 0.01f));
				}
				else
				{
					TestEqual(Res + TEXT(": key labels keep their PR 6 size"),
						Panel->KeyFontForTesting().Size,
						SWoTOnScreenKeyboard::MinimumKeyLabelSize);
				}
			}
		}
		FWoTTextEntry::ResetForTesting();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeckSettingsSaveChangesSpec,
	"WoT.Settings.SaveChangesPrompt", WoTUITest_Flags)

bool FWoTDeckSettingsSaveChangesSpec::RunTest(const FString&)
{
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTUITest_ModeGuard ModeGuard;
	FWoTUITest_SettingsRowsGuard RowsGuard;
	FWoTUITest_Preferences Fixture;
	if (!TestTrue(TEXT("a test preferences owner"), Fixture.bReady))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = WoTUITest_Authority();
	FWoTUITest_FrontEnd FrontEnd(Authority,
		WoTUITest_MakeRoot(Authority, nullptr, false, Fixture.Owner), FVector2D(1280.0f, 720.0f));
	const TSharedRef<SWoTFrontEndRoot>& Root = FrontEnd.Root;
	auto Screen = [&Root]() { return Root->CurrentScreenForTesting(); };
	auto OpenControls = [&Root, &FrontEnd]()
	{
		Root->ActivateMainMenuEntryForTesting(1);
		FrontEnd.Settle();
		return Root->SettingsScreenForTesting();
	};
	auto Focused = [&Slate]() { return Slate.GetKeyboardFocusedWidget(); };
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);

	// With nothing changed, B leaves at once, as before.
	TSharedPtr<SWoTSettingsScreen> Controls = OpenControls();
	if (!TestTrue(TEXT("Controls opens"), Controls.IsValid()
		&& Screen() == EWoTFrontEndScreen::Controls))
	{
		return false;
	}
	TestFalse(TEXT("an untouched page has no unsaved changes"), Controls->HasUnsavedChanges());
	TestTrue(TEXT("B is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("with nothing changed, B leaves Controls at once"),
		Screen() == EWoTFrontEndScreen::Main);
	TestFalse(TEXT("and asks nothing"), Controls->IsConfirmingUnsaved());

	// With an unsaved change, B asks first.
	const FWoTGameSettings Saved = Fixture.Owner->GetGameSettings();
	Controls = OpenControls();
	FWoTGameSettings Changed = Controls->DraftForTesting();
	Changed.bInvertMouseY = !Changed.bInvertMouseY;
	Controls->SetDraftForTesting(Changed);
	const TSharedPtr<SWidget> Editing = Controls->NavigationRowsForTesting().IsValidIndex(1)
		? Controls->NavigationRowsForTesting()[1][0] : nullptr;
	Slate.SetKeyboardFocus(Editing, EFocusCause::SetDirectly);
	FrontEnd.Settle();
	TestTrue(TEXT("an edit is an unsaved change"), Controls->HasUnsavedChanges());
	TestTrue(TEXT("B is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestTrue(TEXT("B with unsaved changes opens \"Save changes?\""), Controls->IsConfirmingUnsaved());
	TestTrue(TEXT("and stays on Controls"), Screen() == EWoTFrontEndScreen::Controls);
	TestTrue(TEXT("the prompt shows over the page"),
		Controls->ConfirmPromptForTesting()->GetVisibility().IsVisible());
	TestTrue(TEXT("focus opens on its Save"), Focused() == Controls->ConfirmSaveButtonForTesting());
	TestFalse(TEXT("the page behind is out of the D-pad's reach"), WoTIsNavigable(Editing));
	TestEqual(TEXT("the prompt strip shows the prompt's own set"),
		FWoTButtonPrompt::Describe(Root->ButtonPromptsForTesting()->GetShownPrompts()),
		FWoTButtonPrompt::Describe(FWoTButtonPromptSets::SaveChanges()));
	TestTrue(TEXT("Right moves to Discard"),
		WoTUITest_Navigate(Controls->ConfirmSaveButtonForTesting(), EUINavigation::Right)
			== Controls->ConfirmDiscardButtonForTesting());
	TestTrue(TEXT("Up cannot leave the prompt"),
		WoTUITest_Navigate(Controls->ConfirmSaveButtonForTesting(), EUINavigation::Up) == nullptr);

	// B on the prompt: back to editing, nothing lost.
	TestTrue(TEXT("B on the prompt is handled"), FrontEnd.Press(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B closes the prompt"), Controls->IsConfirmingUnsaved());
	TestTrue(TEXT("still on Controls"), Screen() == EWoTFrontEndScreen::Controls);
	TestTrue(TEXT("with the edit kept"), Controls->DraftForTesting() == Changed);
	TestTrue(TEXT("and focus back where the player was"), Focused() == Editing);

	// Escape is the same back action, on the page and on the prompt.
	FrontEnd.Press(EKeys::Escape);
	TestTrue(TEXT("Escape with unsaved changes asks too"), Controls->IsConfirmingUnsaved());
	FrontEnd.Press(EKeys::Escape);
	TestFalse(TEXT("Escape on the prompt keeps editing"), Controls->IsConfirmingUnsaved());
	TestTrue(TEXT("on Controls"), Screen() == EWoTFrontEndScreen::Controls);

	// Discard drops the draft and leaves, as the Back button does.
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
	FrontEnd.Press(EKeys::Gamepad_DPad_Right);
	TestTrue(TEXT("the D-pad reaches Discard"), Focused() == Controls->ConfirmDiscardButtonForTesting());
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("Discard leaves Controls"), Screen() == EWoTFrontEndScreen::Main);
	TestTrue(TEXT("and saves nothing"), Fixture.Owner->GetGameSettings() == Saved);

	// Save applies and leaves, as the Save button does.
	Controls = OpenControls();
	Controls->SetDraftForTesting(Changed);
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Right);
	TestTrue(TEXT("the prompt opens again"), Controls->IsConfirmingUnsaved()
		&& Focused() == Controls->ConfirmSaveButtonForTesting());
	FrontEnd.Press(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("Save leaves Controls"), Screen() == EWoTFrontEndScreen::Main);
	TestTrue(TEXT("having saved the change"),
		Fixture.Owner->GetGameSettings().bInvertMouseY == Changed.bInvertMouseY);

	// Mouse clicks on the page's own Save and Back are unchanged.
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Pointer);
	auto Click = [&Slate, &FrontEnd](const TSharedPtr<SWidget>& Widget)
	{
		const FVector2D At = FrontEnd.Shown.ScreenCenter(Widget);
		// A real pointer: over the button first, then down and up.
		Slate.ProcessMouseMoveEvent(WoTUITest_MouseMove(At, At + FVector2D(1.0, 1.0)), false);
		Slate.ProcessMouseButtonDownEvent(nullptr, WoTUITest_MouseDown(At));
		Slate.ProcessMouseButtonUpEvent(WoTWidgetTest_MouseUp(At));
		FrontEnd.Settle();
	};
	const FWoTGameSettings SavedAgain = Fixture.Owner->GetGameSettings();
	FWoTGameSettings Reverted = SavedAgain;
	Reverted.bInvertMouseY = !Reverted.bInvertMouseY;
	Controls = OpenControls();
	Controls->SetDraftForTesting(Reverted);
	Click(Controls->BackButtonForTesting());
	TestFalse(TEXT("a click on Back asks nothing"), Controls->IsConfirmingUnsaved());
	TestTrue(TEXT("it leaves at once"), Screen() == EWoTFrontEndScreen::Main);
	TestTrue(TEXT("dropping the draft"), Fixture.Owner->GetGameSettings() == SavedAgain);
	Controls = OpenControls();
	Controls->SetDraftForTesting(Reverted);
	Click(Controls->SaveButtonForTesting());
	TestTrue(TEXT("a click on Save saves and leaves"), Screen() == EWoTFrontEndScreen::Main
		&& Fixture.Owner->GetGameSettings().bInvertMouseY == Reverted.bInvertMouseY);

	// The game menu's settings pages take the same back action (Escape here).
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
		.Preferences(Fixture.Owner)
		.RequiredFontsForTesting(WoTUITest_Fonts());
	FWoTUITest_Window Shown(Overlay);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f), FSlateLayoutTransform()), 0.0, 0.0f);
	Shown.Settle();
	Overlay->OpenSettingsForTesting(EWoTSettingsPage::Controls);
	Shown.Settle();
	const TSharedPtr<SWoTSettingsScreen> Page = Overlay->SettingsScreenForTesting();
	if (!TestTrue(TEXT("the game menu's Controls opens"), Page.IsValid()))
	{
		return false;
	}
	FWoTGameSettings InGame = Page->DraftForTesting();
	InGame.bLookSpring = !InGame.bLookSpring;
	Page->SetDraftForTesting(InGame);
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestTrue(TEXT("the game menu: Escape with unsaved changes asks"), Page->IsConfirmingUnsaved()
		&& Overlay->SettingsScreenForTesting() == Page);
	WoTWidgetTest_Press(Shown, EKeys::Escape);
	TestTrue(TEXT("the game menu: Escape again keeps editing"), !Page->IsConfirmingUnsaved()
		&& Overlay->SettingsScreenForTesting() == Page);
	TestTrue(TEXT("the game menu: B asks too"),
		WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Right) && Page->IsConfirmingUnsaved());
	Slate.SetKeyboardFocus(Page->ConfirmDiscardButtonForTesting(), EFocusCause::SetDirectly);
	WoTWidgetTest_Press(Shown, EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("the game menu: Discard returns to the menu"), Overlay->IsShowingMainMenuForTesting());
	TestTrue(TEXT("the game menu: and saves nothing"),
		Fixture.Owner->GetGameSettings().bLookSpring != InGame.bLookSpring);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeckInGameMenuFitsSpec,
	"WoT.InGame.MenuFits", WoTUITest_Flags)

bool FWoTDeckInGameMenuFitsSpec::RunTest(const FString&)
{
	// In a Battle edit phase the game menu's background stopped short of its
	// entries and "Quit to Main Menu" hung off the bottom of the Deck's screen.
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
	for (const bool bBattle : {false, true})
	{
		if (bBattle && !TestNotNull(TEXT("a Battle edit phase"), WoTDeckTest_SpawnEditPhase(World)))
		{
			return false;
		}
		const TCHAR* Mode = bBattle ? TEXT("Battle edit phase") : TEXT("Arena");
		for (const FVector2D& Resolution : WoTDeckTest_Resolutions)
		{
			const FString Label = FString::Printf(TEXT("%s %s"), *WoTDeckTest_Label(Resolution), Mode);
			const TSharedRef<SWoTInGameMenuOverlay> Overlay = SNew(SWoTInGameMenuOverlay)
				.PlayerController(Controller)
				.RequiredFontsForTesting(WoTUITest_Fonts());
			FWoTDeckTest_Screen Screen(Overlay, Resolution);
			const int32 Count = Overlay->RenderedMainMenuEntryCountForTesting();
			TestEqual(Label + TEXT(": every entry"), Count, bBattle ? 11 : 10);
			TestEqual(Label + TEXT(": Quit is last"),
				Overlay->MainMenuIndexForTesting(EWoTInGameMenuEntry::Quit), Count - 1);
			const FSlateRect Background = FWoTDeckTest_Screen::Rect(Overlay->MenuBackgroundForTesting());
			const FSlateRect First = FWoTDeckTest_Screen::Rect(Overlay->MainMenuButtonForTesting(0));
			const FSlateRect Last = FWoTDeckTest_Screen::Rect(Overlay->MainMenuButtonForTesting(Count - 1));
			AddInfo(FString::Printf(TEXT("%s: background %s, first entry %s, last entry %s"), *Label,
				*WoTDeckTest_RectText(Background), *WoTDeckTest_RectText(First),
				*WoTDeckTest_RectText(Last)));
			TestTrue(Label + TEXT(": the last entry ends inside the background"),
				Last.GetSize().Y > 0.0f && Last.Bottom <= Background.Bottom + 0.5f);
			TestTrue(Label + TEXT(": the first entry starts inside it"), First.Top >= Background.Top - 0.5f);
			TestTrue(Label + TEXT(": the entries lie within its width"),
				First.Left >= Background.Left - 0.5f && First.Right <= Background.Right + 0.5f);
			TestTrue(Label + TEXT(": the background ends on screen"),
				Background.Bottom <= Resolution.Y + 0.5f);
			TestTrue(Label + TEXT(": and starts on screen"), Background.Top >= -0.5f);
			// The text is never shrunk to fit: the 2x strike of the 14-pixel
			// face, 28 physical pixels, as before.
			TestTrue(FString::Printf(TEXT("%s: entries keep their 28-pixel text (%.2f)"), *Label,
				FWoTMenuStyle::PhysicalTextPixels(Overlay->ItemFontForTesting().Size, Screen.Dpi)),
				FMath::IsNearlyEqual(FWoTMenuStyle::PhysicalTextPixels(
					Overlay->ItemFontForTesting().Size, Screen.Dpi), 28.0f, 0.05f));
			TArray<TSharedRef<SWidget>> Texts;
			WoTDeckTest_Collect(Overlay->MainMenuButtonForTesting(Count - 1).ToSharedRef(),
				FName(TEXT("STextBlock")), Texts);
			if (TestEqual(Label + TEXT(": the last entry has its label"), Texts.Num(), 1))
			{
				const FSlateRect Text = FWoTDeckTest_Screen::Rect(Texts[0]);
				TestTrue(Label + TEXT(": its label lies inside the background"),
					Text.Left >= Background.Left - 0.5f && Text.Right <= Background.Right + 0.5f
					&& Text.Bottom <= Background.Bottom + 0.5f);
			}
		}
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
