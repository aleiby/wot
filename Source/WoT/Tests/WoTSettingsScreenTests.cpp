#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Settings/WoTGameSettings.h"
#include "../Settings/WoTSettingsSlate.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTMenuStyle.h"
#include "../WoTPreferencesSubsystem.h"
#include "../Payload/WoTPayloadActivation.h"
#include "WoTFrontEndTestData.h"

#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "HAL/FileManager.h"
#include "Input/HittestGrid.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr EAutomationTestFlags ScreenFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FSettingsFeedbackRecorder final : public IWoTFrontEndFeedbackSink
{
public:
	virtual bool IsReady() const override { return true; }
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

TArray<uint8> Utf8(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

FString SettingsFixtureLabel(int32 Length, TCHAR Character)
{
	return FString::ChrN(Length, Character);
}

TSharedRef<FWoTFrontEndPresentationAuthority> MakeSettingsAuthority(int32 BodyFontSize = 14)
{
	TMap<FName, FString> Labels;
	Labels.Add(FName(TEXT("menuMain.MenuList[2]")), SettingsFixtureLabel(11, TEXT('M')));
	Labels.Add(FName(TEXT("menuMain.MenuList[3]")), SettingsFixtureLabel(8, TEXT('C')));
	Labels.Add(FName(TEXT("menuMain.MenuList[4]")), SettingsFixtureLabel(8, TEXT('H')));
	Labels.Add(FName(TEXT("menuMain.MenuList[7]")), SettingsFixtureLabel(4, TEXT('Q')));
	Labels.Add(FName(TEXT("menuMultiPlayer.HelpMessage[1]")), SettingsFixtureLabel(20, TEXT('J')));
	Labels.Add(FName(TEXT("menuMultiPlayer.MenuList[2]")), SettingsFixtureLabel(13, TEXT('P')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuTitle")), SettingsFixtureLabel(12, TEXT('A')));
	Labels.Add(FName(TEXT("menuStartBattleServer.MenuTitle")), SettingsFixtureLabel(14, TEXT('B')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[1]")), SettingsFixtureLabel(10, TEXT('S')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[2]")), SettingsFixtureLabel(22, TEXT('K')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[3]")), SettingsFixtureLabel(10, TEXT('T')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[5]")), SettingsFixtureLabel(18, TEXT('N')));
	Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[7]")), SettingsFixtureLabel(11, TEXT('V')));
	Labels.Add(FName(TEXT("menuPlayer.MenuTitle")), SettingsFixtureLabel(13, TEXT('R')));
	Labels.Add(FName(TEXT("menuPlayer.MenuList[1]")), SettingsFixtureLabel(5, TEXT('N')));
	Labels.Add(FName(TEXT("menuPlayer.HelpMessage[1]")), SettingsFixtureLabel(84, TEXT('I')));
	Labels.Add(FName(TEXT("menuPlayer.HelpMessage[2]")), SettingsFixtureLabel(29, TEXT('D')));
	Labels.Add(FName(TEXT("menuOptions.MenuTitle")), SettingsFixtureLabel(8, TEXT('O')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[1]")), SettingsFixtureLabel(18, TEXT('U')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[2]")), SettingsFixtureLabel(12, TEXT('I')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[3]")), SettingsFixtureLabel(17, TEXT('E')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[5]")), SettingsFixtureLabel(15, TEXT('S')));
	Labels.Add(FName(TEXT("menuOptions.MenuList[11]")), SettingsFixtureLabel(9, TEXT('B')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[1]")), SettingsFixtureLabel(4, TEXT('F')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[2]")), SettingsFixtureLabel(7, TEXT('J')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[3]")), SettingsFixtureLabel(12, TEXT('W')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[4]")), SettingsFixtureLabel(13, TEXT('X')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[5]")), SettingsFixtureLabel(11, TEXT('L')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[6]")), SettingsFixtureLabel(12, TEXT('R')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[9]")), SettingsFixtureLabel(9, TEXT('N')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[10]")), SettingsFixtureLabel(13, TEXT('P')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[11]")), SettingsFixtureLabel(9, TEXT('D')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[13]")), SettingsFixtureLabel(7, TEXT('U')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[14]")), SettingsFixtureLabel(9, TEXT('W')));
	Labels.Add(FName(TEXT("menuKeyboard.MenuList[18]")), SettingsFixtureLabel(17, TEXT('Z')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuTitle")), SettingsFixtureLabel(8, TEXT('G')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuList[2]")), SettingsFixtureLabel(22, TEXT('Y')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuList[3]")), SettingsFixtureLabel(17, TEXT('R')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuList[5]")), SettingsFixtureLabel(5, TEXT('V')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuList[6]")), SettingsFixtureLabel(21, TEXT('D')));
	Labels.Add(FName(TEXT("menuConfiguration.MenuList[15]")), SettingsFixtureLabel(13, TEXT('A')));
	return FWoTFrontEndPresentationAuthority::CreateForTesting(
		TEXT("generation-input"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), BodyFontSize),
		MoveTemp(Labels), MakeShared<FSettingsFeedbackRecorder>());
}

void CollectText(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<STextBlock>>& Out)
{
	// Collapsed subtrees are never laid out, so their bound fonts stay stale.
	if (!Widget->GetVisibility().IsVisible()) return;
	if (Widget->GetType() == FName(TEXT("STextBlock")))
		Out.Add(StaticCastSharedRef<STextBlock>(Widget));
	if (FChildren* Children = Widget->GetChildren())
		for (int32 Index = 0; Index < Children->Num(); ++Index)
			CollectText(Children->GetChildAt(Index), Out);
}

bool AnyTextStartsWith(const TArray<TSharedRef<STextBlock>>& Texts, const TCHAR* Prefix)
{
	for (const TSharedRef<STextBlock>& Text : Texts)
		if (Text->GetText().ToString().StartsWith(Prefix)) return true;
	return false;
}

bool HasText(const TArray<TSharedRef<STextBlock>>& Texts, const TCHAR* Expected)
{
	for (const TSharedRef<STextBlock>& Text : Texts)
		if (Text->GetText().ToString() == Expected) return true;
	return false;
}

FKeyEvent KeyEvent(FKey Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0);
}

/** A real, activated preferences owner writing to a temporary file. */
struct FActivatedPreferencesFixture
{
	FString PreviousCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	float PreviousScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale() : 1.0f;
	float PreviousVolume = FApp::GetVolumeMultiplier();
	FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("SettingsScreen-")) + FGuid::NewGuid().ToString());
	FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	UWoTGameInstance* Instance = nullptr;
	UWoTPreferencesSubsystem* Owner = nullptr;
	bool bReady = false;

	FActivatedPreferencesFixture()
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
		ResetWoTPayloadActivationForTesting();
		Instance = NewObject<UWoTGameInstance>();
		Instance->AddToRoot();
		Owner = NewObject<UWoTPreferencesSubsystem>(Instance);
		Owner->AddToRoot();
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
			Utf8("Schema=EarlyUI-v1\nUIScale=1.25\nLocale=fr-FR\n"),
			FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
		if (Parsed.IsError()) return;
		if (Owner->AdoptAuthenticatedEarlyUI(MoveTemp(Parsed).GetValue()).IsError()) return;
		if (PublishWoTPayloadBootstrapActivationForTesting().IsError()) return;
		bReady = !Owner->ApplyFullPreferencesAfterCacheActivation().IsError();
	}
	~FActivatedPreferencesFixture()
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

struct FScreenCallbacks
{
	int32 BackCount = 0;
	int32 SavedCount = 0;
};

TSharedRef<SWoTSettingsScreen> MakeScreen(EWoTSettingsPage Page,
	UWoTPreferencesSubsystem* Owner,
	const TSharedRef<FWoTFrontEndPresentationAuthority>& Authority,
	FScreenCallbacks& Callbacks)
{
	Authority->PrepareBrowserResources();
	return SNew(SWoTSettingsScreen)
		.Page(Page)
		.Preferences(Owner)
		.PresentationAuthority(Authority)
		.HeadingFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30))
		.BodyFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 28))
		.TextResolver(FWoTSettingsTextResolver::CreateLambda(
			[Authority](FName Key) { return Authority->Text(Key); }))
		.OnBack(FSimpleDelegate::CreateLambda([&Callbacks]() { ++Callbacks.BackCount; }))
		.OnSaved(FSimpleDelegate::CreateLambda([&Callbacks]() { ++Callbacks.SavedCount; }));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsNavigationSpec,
	"WoT.FrontEnd.Settings.Navigation", ScreenFlags)

bool FWoTSettingsNavigationSpec::RunTest(const FString&)
{
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority)
		.PreferencesForTesting(Fixture.Owner);
	const FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform());
	Root->Tick(Geometry, 0.0, 0.0f);

	// Controls: its own screen with the injected title, framed.
	TestTrue(TEXT("Controls entry handled"),
		Root->ActivateMainMenuEntryForTesting(1).IsEventHandled());
	TestTrue(TEXT("Controls screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Controls);
	// Bound Slate attributes (fonts) refresh during prepass, not on read.
	Root->Tick(Geometry, 0.0, 0.0f);
	Root->SlatePrepass(1.0f);
	TArray<TSharedRef<STextBlock>> Texts;
	CollectText(Root, Texts);
	TestTrue(TEXT("Controls shows the injected title"),
		HasText(Texts, *SettingsFixtureLabel(8, TEXT('O'))));
	TestFalse(TEXT("Controls is not the Hardware page"),
		HasText(Texts, *SettingsFixtureLabel(8, TEXT('G'))));
	TestTrue(TEXT("Controls exposes editable mouse settings"),
		HasText(Texts, *SettingsFixtureLabel(17, TEXT('E')))
		&& HasText(Texts, *SettingsFixtureLabel(12, TEXT('I'))));
	TestTrue(TEXT("Controls exposes the binding list"),
		HasText(Texts, *SettingsFixtureLabel(12, TEXT('W')))
		&& HasText(Texts, *SettingsFixtureLabel(17, TEXT('Z'))));
	TestTrue(TEXT("Controls exposes every new retail action"),
		HasText(Texts, TEXT("Turn Left"))
		&& HasText(Texts, *SettingsFixtureLabel(13, TEXT('P')))
		&& HasText(Texts, *SettingsFixtureLabel(9, TEXT('D')))
		&& HasText(Texts, TEXT("Drop Tainted Items"))
		&& HasText(Texts, TEXT("Screenshot"))
		&& HasText(Texts, TEXT("Toggle Fullscreen"))
		&& HasText(Texts, TEXT("Edit Citadel"))
		&& HasText(Texts, TEXT("Help"))
		&& HasText(Texts, TEXT("Type Chat"))
		&& HasText(Texts, TEXT("All Chat"))
		&& HasText(Texts, TEXT("Team Chat")));
	TestFalse(TEXT("no localization identifier leaks into Controls"),
		AnyTextStartsWith(Texts, TEXT("menu")));
	TestTrue(TEXT("Controls body uses the exact 2x body face"), [&]()
	{
		// Sizes are Slate points after the authority's pixel-to-point step, so
		// compare against the root's own faces: body is exactly 2x the native
		// 14-pixel face, headings keep the 30-pixel face.
		const int32 BodySize = FMath::RoundToInt(Root->LargeBodyFontForTesting().Size);
		const int32 HeadingSize = FMath::RoundToInt(Root->PlayHeadingFontForTesting().Size);
		const float NativeBody = Root->PlayBodyFontForTesting().Size;
		bool bAllMatch = NativeBody > 0.0f
			&& FMath::IsNearlyEqual(Root->LargeBodyFontForTesting().Size, NativeBody * 2.0f, 0.01f)
			&& HeadingSize > BodySize;
		for (const TSharedRef<STextBlock>& Text : Texts)
		{
			const int32 Size = FMath::RoundToInt(Text->GetFont().Size);
			if (Size != BodySize && Size != HeadingSize)
			{
				AddInfo(FString::Printf(TEXT("font %d (body %d heading %d): '%s'"),
					Size, BodySize, HeadingSize, *Text->GetText().ToString()));
			}
			bAllMatch = bAllMatch && (Size == BodySize || Size == HeadingSize);
		}
		return bAllMatch;
	}());
	TestTrue(TEXT("Escape leaves Controls"),
		Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape)).IsEventHandled());
	TestTrue(TEXT("Escape returns to Main"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);

	// Hardware: distinct screen, distinct content.
	TestTrue(TEXT("Hardware entry handled"),
		Root->ActivateMainMenuEntryForTesting(2).IsEventHandled());
	TestTrue(TEXT("Hardware screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware);
	Root->Tick(Geometry, 0.0, 0.0f);
	Root->SlatePrepass(1.0f);
	Texts.Reset();
	CollectText(Root, Texts);
	TestTrue(TEXT("Hardware shows the injected title"),
		HasText(Texts, *SettingsFixtureLabel(8, TEXT('G'))));
	TestTrue(TEXT("Hardware exposes display and audio rows"),
		HasText(Texts, *SettingsFixtureLabel(17, TEXT('R')))
		&& HasText(Texts, *SettingsFixtureLabel(5, TEXT('V')))
		&& HasText(Texts, *SettingsFixtureLabel(13, TEXT('A')))
		&& HasText(Texts, TEXT("Frame Rate Limit")));
	TestFalse(TEXT("Hardware has no binding rows"),
		HasText(Texts, *SettingsFixtureLabel(12, TEXT('W'))));
	TestFalse(TEXT("no localization identifier leaks into Hardware"),
		AnyTextStartsWith(Texts, TEXT("menu")));

	// Key capture on Controls: Escape cancels capture and stays on the screen.
	Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape));
	Root->ActivateMainMenuEntryForTesting(1);
	TSharedPtr<SWoTSettingsScreen> Controls = Root->SettingsScreenForTesting();
	if (!TestTrue(TEXT("Controls screen widget exposed"), Controls.IsValid())) return false;
	Controls->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
	TestTrue(TEXT("capture is live"), Controls->IsCapturingBinding());
	TestTrue(TEXT("root Escape routed to the capture"),
		Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape)).IsEventHandled());
	TestFalse(TEXT("Escape cancelled the capture"), Controls->IsCapturingBinding());
	TestTrue(TEXT("Escape did not leave Controls while capturing"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Controls);
	TestTrue(TEXT("second Escape leaves Controls"),
		Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape)).IsEventHandled()
		&& Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);

	// Player Config stays reachable under Multiplayer and returns to Play.
	TestTrue(TEXT("Multiplayer entry handled"),
		Root->ActivateMainMenuEntryForTesting(0).IsEventHandled());
	TestTrue(TEXT("Play screen"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	Texts.Reset();
	CollectText(Root, Texts);
	TestTrue(TEXT("Play offers Player Config"),
		HasText(Texts, *SettingsFixtureLabel(13, TEXT('P'))));
	TSharedPtr<SWidget> PlayerConfig = Root->PlayerConfigButtonForTesting();
	if (!TestTrue(TEXT("Player Config button exposed"), PlayerConfig.IsValid())) return false;
	StaticCastSharedPtr<SButton>(PlayerConfig)->SimulateClick();
	TestTrue(TEXT("Player Config opens the faction page"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::PlayerConfig);
	Texts.Reset();
	CollectText(Root, Texts);
	TestTrue(TEXT("Player Config keeps the injected title"),
		HasText(Texts, *SettingsFixtureLabel(13, TEXT('R'))));
	TestTrue(TEXT("Escape leaves Player Config"),
		Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape)).IsEventHandled());
	TestTrue(TEXT("Player Config returns to Multiplayer, not Main"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CSettingsOptionsVisibleRowsSpec,
	"WoT.Parity.P1C.SettingsOptions.VisibleRows", ScreenFlags)

bool FWoTP1CSettingsOptionsVisibleRowsSpec::RunTest(const FString&)
{
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	FScreenCallbacks Callbacks;
	const TSharedRef<SWoTSettingsScreen> Screen = MakeScreen(
		EWoTSettingsPage::Controls, Fixture.Owner, Authority, Callbacks);
	Screen->SlatePrepass(1.0f);
	TArray<TSharedRef<STextBlock>> Texts;
	CollectText(Screen, Texts);
	TestTrue(TEXT("Controls exposes the localized Auto Slope Look row"),
		HasText(Texts, *SettingsFixtureLabel(15, TEXT('S'))));
	TestTrue(TEXT("Controls exposes the localized Player ID row"),
		HasText(Texts, *SettingsFixtureLabel(9, TEXT('B'))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsControlsCaptureSpec,
	"WoT.FrontEnd.Settings.ControlsCapture", ScreenFlags)

bool FWoTSettingsControlsCaptureSpec::RunTest(const FString&)
{
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	FScreenCallbacks Callbacks;
	TSharedRef<SWoTSettingsScreen> Screen = MakeScreen(
		EWoTSettingsPage::Controls, Fixture.Owner, Authority, Callbacks);
	TestTrue(TEXT("draft starts from the owner's settings"),
		Screen->DraftForTesting() == Fixture.Owner->GetGameSettings());

	// Escape cancels capture without navigating back.
	Screen->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
	TestTrue(TEXT("capturing"), Screen->IsCapturingBinding());
	TestTrue(TEXT("Escape handled by capture"),
		Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape)).IsEventHandled());
	TestFalse(TEXT("capture cancelled"), Screen->IsCapturingBinding());
	TestEqual(TEXT("no back navigation on capture cancel"), Callbacks.BackCount, 0);
	TestTrue(TEXT("Jump unchanged after cancel"),
		Screen->DraftForTesting().FindBinding(EWoTManagedBinding::Jump)->Primary == EKeys::SpaceBar);

	// A real key rebinds the slot.
	Screen->BeginCaptureForTesting(EWoTManagedBinding::Jump, EWoTBindingSlot::Primary);
	Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::F5));
	TestFalse(TEXT("capture ends on key"), Screen->IsCapturingBinding());
	TestTrue(TEXT("Jump rebound to F5"),
		Screen->DraftForTesting().FindBinding(EWoTManagedBinding::Jump)->Primary == EKeys::F5);
	TestTrue(TEXT("no error after a valid rebind"), Screen->ErrorTextForTesting().IsEmpty());

	// A conflicting key is refused with a readable error and no change.
	Screen->BeginCaptureForTesting(EWoTManagedBinding::Fire, EWoTBindingSlot::Primary);
	Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::W));
	TestTrue(TEXT("Fire keeps its key on conflict"),
		Screen->DraftForTesting().FindBinding(EWoTManagedBinding::Fire)->Primary == EKeys::LeftMouseButton);
	TestTrue(TEXT("conflict error is readable"),
		Screen->ErrorTextForTesting().Contains(TEXT("already bound")));

	// A gamepad key is refused as not editable.
	Screen->BeginCaptureForTesting(EWoTManagedBinding::Fire, EWoTBindingSlot::Secondary);
	Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Gamepad_FaceButton_Bottom));
	TestTrue(TEXT("gamepad refused, still capturing"), Screen->IsCapturingBinding());
	TestTrue(TEXT("refusal explains keyboard/mouse"),
		Screen->ErrorTextForTesting().Contains(TEXT("keyboard or mouse")));
	Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape));

	// Retail binds bare modifier keys (Shift/Ctrl/Alt). They are ordinary
	// editable keys here, not chord flags.
	Screen->BeginCaptureForTesting(EWoTManagedBinding::SShot,
		EWoTBindingSlot::Secondary);
	// Left Ctrl now owns retail Drop, so use the otherwise-unbound right key to
	// keep this capture test about bare modifiers rather than conflict policy.
	Screen->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::RightControl));
	TestFalse(TEXT("a retail-style modifier key completes capture"),
		Screen->IsCapturingBinding());
	TestTrue(TEXT("bare Ctrl is retained as the selected key"),
		Screen->DraftForTesting().FindBinding(EWoTManagedBinding::SShot)->Secondary
			== EKeys::RightControl);

	// Save submits the draft through the owner; Back discards.
	TestTrue(TEXT("save handled"), Screen->SaveForTesting().IsEventHandled());
	TestEqual(TEXT("save closes the screen"), Callbacks.SavedCount, 1);
	TestTrue(TEXT("owner persisted the rebind"),
		Fixture.Owner->GetGameSettings().FindBinding(EWoTManagedBinding::Jump)->Primary == EKeys::F5);
	FScreenCallbacks BackCallbacks;
	TSharedRef<SWoTSettingsScreen> Second = MakeScreen(
		EWoTSettingsPage::Controls, Fixture.Owner, Authority, BackCallbacks);
	Second->BeginCaptureForTesting(EWoTManagedBinding::ShowScores, EWoTBindingSlot::Primary);
	Second->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::F9));
	TestTrue(TEXT("Back handled"), Second->HandleBack());
	TestEqual(TEXT("Back navigates"), BackCallbacks.BackCount, 1);
	TestTrue(TEXT("Back discarded the pending rebind"),
		Fixture.Owner->GetGameSettings().FindBinding(EWoTManagedBinding::ShowScores)->Primary == EKeys::F4);
	TestTrue(TEXT("reset restores the closed defaults"),
		Second->ResetBindingsForTesting().IsEventHandled()
		&& Second->DraftForTesting().Bindings == FWoTGameSettings::Defaults().Bindings);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsControllerLookSpec,
	"WoT.FrontEnd.Settings.ControllerLook", ScreenFlags)

bool FWoTSettingsControllerLookSpec::RunTest(const FString&)
{
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	FScreenCallbacks Callbacks;
	const TSharedRef<SWoTSettingsScreen> Screen = MakeScreen(
		EWoTSettingsPage::Controls, Fixture.Owner, Authority, Callbacks);
	Screen->SlatePrepass(1.0f);
	TArray<TSharedRef<STextBlock>> Texts;
	CollectText(Screen, Texts);
	TestTrue(TEXT("Controls shows the controller sensitivity row"),
		HasText(Texts, TEXT("Controller Sensitivity")));
	TestTrue(TEXT("Controls shows the controller invert row"),
		HasText(Texts, TEXT("Invert Controller Look")));

	const TSharedPtr<SWidget> Slider = Screen->ControllerSensitivitySliderForTesting();
	const TSharedPtr<SCheckBox> Invert = Screen->ControllerInvertCheckBoxForTesting();
	if (!TestTrue(TEXT("both controller controls exist"), Slider.IsValid() && Invert.IsValid()))
		return false;
	// One step right on the shared sensitivity scale is 2x; the mouse is untouched.
	TestTrue(TEXT("the slider takes Right"),
		Slider->OnKeyDown(FGeometry(), KeyEvent(EKeys::Right)).IsEventHandled());
	TestEqual(TEXT("controller sensitivity steps to 2x"),
		Screen->DraftForTesting().ControllerLookSensitivity, 2.0f);
	TestEqual(TEXT("mouse sensitivity is unchanged"),
		Screen->DraftForTesting().MouseSensitivity, Fixture.Owner->GetGameSettings().MouseSensitivity);
	Invert->ToggleCheckedState();
	TestTrue(TEXT("the checkbox inverts the controller"), Screen->DraftForTesting().bControllerInvertY);
	TestEqual(TEXT("the mouse invert is unchanged"),
		Screen->DraftForTesting().bInvertMouseY, Fixture.Owner->GetGameSettings().bInvertMouseY);

	TestTrue(TEXT("save handled"), Screen->SaveForTesting().IsEventHandled());
	TestEqual(TEXT("owner persisted the controller sensitivity"),
		Fixture.Owner->GetGameSettings().ControllerLookSensitivity, 2.0f);
	TestTrue(TEXT("owner persisted the controller invert"),
		Fixture.Owner->GetGameSettings().bControllerInvertY);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsHardwareConfirmationSpec,
	"WoT.FrontEnd.Settings.HardwareConfirmation", ScreenFlags)

bool FWoTSettingsHardwareConfirmationSpec::RunTest(const FString&)
{
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	TArray<FWoTGameSettings> Projected;
	FWoTGameSettingsAdapter::SetDisplayProjectionForTesting(
		[&Projected](const FWoTGameSettings& Settings, bool, bool)
		{
			Projected.Add(Settings);
		});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetDisplayProjectionForTesting({}); };
	TArray<float> ProjectedBrightness;
	FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting(
		[&ProjectedBrightness](float RampInvGamma)
		{
			ProjectedBrightness.Add(RampInvGamma);
		});
	ON_SCOPE_EXIT { FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting({}); };
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	FScreenCallbacks Callbacks;
	TSharedRef<SWoTSettingsScreen> Screen = MakeScreen(
		EWoTSettingsPage::Hardware, Fixture.Owner, Authority, Callbacks);
	const FWoTGameSettings Confirmed = Fixture.Owner->GetGameSettings();
	Screen->Tick(FGeometry(), 100.0, 0.0f);

	// Save with a video change previews and waits for confirmation.
	FWoTGameSettings Draft = Confirmed;
	Draft.Width = 1920;
	Draft.Height = 1080;
	Draft.bVSync = true;
	Screen->SetDraftForTesting(Draft);
	Screen->SaveForTesting();
	TestEqual(TEXT("preview projected once"), Projected.Num(), 1);
	TestTrue(TEXT("awaiting confirmation"), Screen->IsAwaitingVideoConfirmationForTesting());
	TestTrue(TEXT("deadline is fifteen seconds out"),
		FMath::IsNearlyEqual(Screen->VideoConfirmDeadlineForTesting(), 115.0, 0.01));
	TestTrue(TEXT("nothing persisted before confirmation"),
		Fixture.Owner->GetGameSettings() == Confirmed);
	TestEqual(TEXT("screen stays open"), Callbacks.SavedCount, 0);

	// Timeout reverts the display and the draft; still nothing persisted.
	Screen->Tick(FGeometry(), 114.0, 0.0f);
	TestTrue(TEXT("still waiting before the deadline"), Screen->IsAwaitingVideoConfirmationForTesting());
	Screen->Tick(FGeometry(), 115.5, 0.0f);
	TestFalse(TEXT("timeout ends confirmation"), Screen->IsAwaitingVideoConfirmationForTesting());
	TestEqual(TEXT("timeout projected the confirmed display"), Projected.Num(), 2);
	if (Projected.Num() == 2)
		TestTrue(TEXT("rollback restores every display field"), Projected[1] == Confirmed);
	TestTrue(TEXT("draft video reverted"),
		Screen->DraftForTesting().Width == Confirmed.Width
		&& Screen->DraftForTesting().bVSync == Confirmed.bVSync);
	TestTrue(TEXT("still nothing persisted"), Fixture.Owner->GetGameSettings() == Confirmed);
	TestTrue(TEXT("revert is announced"),
		Screen->StatusTextForTesting().Contains(TEXT("reverted")));

	// Keep persists through the owner.
	Screen->SetDraftForTesting(Draft);
	Screen->SaveForTesting();
	TestEqual(TEXT("second preview"), Projected.Num(), 3);
	Screen->KeepVideoForTesting();
	TestTrue(TEXT("Keep persisted the video settings"),
		Fixture.Owner->GetGameSettings().Width == 1920 && Fixture.Owner->GetGameSettings().bVSync);
	TestEqual(TEXT("Keep closes the screen"), Callbacks.SavedCount, 1);
	TestFalse(TEXT("no confirmation pending after Keep"), Screen->IsAwaitingVideoConfirmationForTesting());

	// A failed save keeps the confirmed state and the user's draft visible.
	FScreenCallbacks FailCallbacks;
	TSharedRef<SWoTSettingsScreen> Failing = MakeScreen(
		EWoTSettingsPage::Hardware, Fixture.Owner, Authority, FailCallbacks);
	const FWoTGameSettings BeforeFailure = Fixture.Owner->GetGameSettings();
	const FString Blocker = FPaths::Combine(Fixture.Directory, TEXT("blocker"));
	TestTrue(TEXT("create storage blocker"), FFileHelper::SaveStringToFile(TEXT("x"), *Blocker));
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FPaths::Combine(Blocker, TEXT("settings")));
	ON_SCOPE_EXIT { UWoTPreferencesSubsystem::SetPreferencePathForTesting(Fixture.Leaf); };
	FWoTGameSettings VolumeDraft = BeforeFailure;
	VolumeDraft.MasterVolume = 0.4f;
	Failing->SetDraftForTesting(VolumeDraft);
	Failing->SaveForTesting();
	TestEqual(TEXT("failed save keeps the screen open"), FailCallbacks.SavedCount, 0);
	TestTrue(TEXT("failed save shows a readable error"),
		Failing->ErrorTextForTesting().Contains(TEXT("could not be saved")));
	TestTrue(TEXT("failed save keeps the pending draft"),
		FMath::IsNearlyEqual(Failing->DraftForTesting().MasterVolume, 0.4f));
	TestTrue(TEXT("failed save keeps the confirmed owner state"),
		Fixture.Owner->GetGameSettings() == BeforeFailure);

	// Brightness is a live hardware-page preview, so Back must restore the
	// confirmed display ramp even when no video-confirmation dialog is active.
	FScreenCallbacks BrightnessCallbacks;
	TSharedRef<SWoTSettingsScreen> BrightnessScreen = MakeScreen(
		EWoTSettingsPage::Hardware, Fixture.Owner, Authority, BrightnessCallbacks);
	FWoTGameSettings BrightnessDraft = BeforeFailure;
	BrightnessDraft.Brightness = 0.9f;
	BrightnessScreen->SetDraftForTesting(BrightnessDraft);
	FWoTGameSettingsAdapter::ApplyBrightness(BrightnessDraft.Brightness);
	ProjectedBrightness.Reset();
	TestTrue(TEXT("Back handles a live brightness preview"),
		BrightnessScreen->HandleBack());
	TestEqual(TEXT("Back reapplies brightness once"), ProjectedBrightness.Num(), 1);
	if (ProjectedBrightness.Num() == 1)
	{
		TestTrue(TEXT("Back restores the confirmed display ramp"), FMath::IsNearlyEqual(
			ProjectedBrightness[0], FWoTGameSettingsAdapter::RampInvGammaForBrightness(
				BeforeFailure.Brightness)));
	}
	TestTrue(TEXT("Back restores the confirmed brightness draft"), FMath::IsNearlyEqual(
		BrightnessScreen->DraftForTesting().Brightness, BeforeFailure.Brightness));

	ProjectedBrightness.Reset();
	{
		FScreenCallbacks DestructionCallbacks;
		TSharedPtr<SWoTSettingsScreen> DestructionScreen = MakeScreen(
			EWoTSettingsPage::Hardware, Fixture.Owner, Authority, DestructionCallbacks);
		FWoTGameSettings DestructionDraft = BeforeFailure;
		DestructionDraft.Brightness = 0.8f;
		DestructionScreen->SetDraftForTesting(DestructionDraft);
		FWoTGameSettingsAdapter::ApplyBrightness(DestructionDraft.Brightness);
		ProjectedBrightness.Reset();
	}
	TestEqual(TEXT("teardown reapplies brightness once"), ProjectedBrightness.Num(), 1);
	if (ProjectedBrightness.Num() == 1)
	{
		TestTrue(TEXT("teardown restores the confirmed display ramp"),
			FMath::IsNearlyEqual(ProjectedBrightness[0],
				FWoTGameSettingsAdapter::RampInvGammaForBrightness(
					BeforeFailure.Brightness)));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsGeometrySpec,
	"WoT.FrontEnd.Settings.Geometry", ScreenFlags)

bool FWoTSettingsGeometrySpec::RunTest(const FString&)
{
	// A 2x body face on a 1280x720 review window must never push Save/Back off
	// screen: the body scrolls, the footer stays.
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority = MakeSettingsAuthority();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority)
		.PreferencesForTesting(Fixture.Owner);
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1280.0f, 720.0f))
		.CreateTitleBar(false).SupportsMaximize(false).SupportsMinimize(false)
		[Root];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window, false);
	ON_SCOPE_EXIT { Slate.RequestDestroyWindow(Window); };
	const FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(1280.0f, 720.0f), FSlateLayoutTransform(1.0f));
	const int32 Entries[] = {1, 2};
	for (const int32 Entry : Entries)
	{
		Root->ActivateMainMenuEntryForTesting(Entry);
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			Root->Tick(Geometry, 0.0, 0.0f);
			Root->SlatePrepass(1.0f);
			FHittestGrid Grid;
			FSlateWindowElementList Elements(Window);
			const FPaintArgs Args(&Window.Get(), Grid,
				FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
			Root->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(),
				Elements, 0, FWidgetStyle(), true);
		}
		const FString Label = Entry == 1 ? TEXT("Controls") : TEXT("Hardware");
		TSharedPtr<SWoTSettingsScreen> Screen = Root->SettingsScreenForTesting();
		if (!TestTrue(Label + TEXT(" screen exists"), Screen.IsValid())) return false;
		const TSharedPtr<SWidget> Save = Screen->SaveButtonForTesting();
		const TSharedPtr<SWidget> Back = Screen->BackButtonForTesting();
		const TSharedPtr<SWidget> Body = Screen->BodyScrollForTesting();
		if (!TestTrue(Label + TEXT(" footer controls exist"),
			Save.IsValid() && Back.IsValid() && Body.IsValid())) return false;
		const FSlateRect SaveBounds = Save->GetCachedGeometry().GetLayoutBoundingRect();
		const FSlateRect BackBounds = Back->GetCachedGeometry().GetLayoutBoundingRect();
		const FSlateRect BodyBounds = Body->GetCachedGeometry().GetLayoutBoundingRect();
		TestTrue(Label + TEXT(" Save is inside the 1280x720 client"),
			SaveBounds.Left >= 0.0f && SaveBounds.Top >= 0.0f
			&& SaveBounds.Right <= 1280.0f && SaveBounds.Bottom <= 720.0f
			&& SaveBounds.GetSize().X > 0.0f && SaveBounds.GetSize().Y > 0.0f);
		TestTrue(Label + TEXT(" Back is inside the 1280x720 client"),
			BackBounds.Left >= 0.0f && BackBounds.Top >= 0.0f
			&& BackBounds.Right <= 1280.0f && BackBounds.Bottom <= 720.0f);
		TestTrue(Label + TEXT(" footer sits below the scrolling body"),
			SaveBounds.Top >= BodyBounds.Bottom - 1.0f);
		TestTrue(Label + TEXT(" body has usable height"),
			BodyBounds.GetSize().Y >= 200.0f);
		if (Entry == 2)
		{
			const TSharedPtr<SWidget> FullscreenLabelWidget =
				Screen->WindowModeLabelForTesting();
			const TSharedPtr<SWidget> FullscreenControlWidget =
				Screen->WindowModeControlForTesting();
			if (!TestTrue(TEXT("Hardware fullscreen row widgets are present"),
				FullscreenLabelWidget.IsValid() && FullscreenControlWidget.IsValid()))
				return false;
			const FSlateRect FullscreenLabel = FullscreenLabelWidget
				->GetCachedGeometry().GetLayoutBoundingRect();
			const FSlateRect FullscreenControl = FullscreenControlWidget
				->GetCachedGeometry().GetLayoutBoundingRect();
			TestTrue(TEXT("fullscreen label and value do not overlap at 1280x720"),
				FullscreenLabel.Right <= FullscreenControl.Left + 1.0f);
			const TSharedPtr<STextBlock> FullscreenLabelText =
				StaticCastSharedPtr<STextBlock>(FullscreenLabelWidget);
			const FVector2D RequiredLabelSize = FSlateApplication::Get()
				.GetRenderer()->GetFontMeasureService()->Measure(
					FullscreenLabelText->GetText(), FullscreenLabelText->GetFont());
			TestTrue(TEXT("fullscreen label fits its allocated column at 1280x720"),
				RequiredLabelSize.X <= FullscreenLabel.GetSize().X + 1.0f);
			TestTrue(TEXT("VSync indicator is at least the body-font size"),
				Screen->VSyncGlyphSizeForTesting()
					>= Root->LargeBodyFontForTesting().Size);
			const TSharedPtr<SScrollBox> Scroll =
				StaticCastSharedPtr<SScrollBox>(Body);
			TestEqual(TEXT("Hardware remains at its no-scroll origin"),
				Scroll->GetScrollOffset(), 0.0f);
			TestTrue(TEXT("Hardware content fits without vertical scrolling"),
				Scroll->GetDesiredSize().Y <= BodyBounds.GetSize().Y + 1.0f);
		}
		Root->OnPreviewKeyDown(FGeometry(), KeyEvent(EKeys::Escape));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSettingsLocalizationSpec,
	"WoT.Menu.Style.SettingsLocalization", ScreenFlags)

bool FWoTSettingsLocalizationSpec::RunTest(const FString&)
{
	// Synthetic labels preserve the source field lengths while exercising projection.
	const ANSICHAR Input[] = "[menuMain]\nMenuList[2]=MMMMMMMMMMM\nMenuList[3]=CCCCCCCC\n"
		"MenuList[4]=HHHHHHHH\nMenuList[7]=QQQQ\n"
		"[menuOptions]\nMenuTitle=OOOOOOOO\nMenuList[1]=UUUUUUUUUUUUUUUUUU\n"
		"MenuList[2]=IIIIIIIIIIII\nMenuList[3]=EEEEEEEEEEEEEEEEE\n"
		"[menuKeyboard]\nMenuList[1]=FFFF\nMenuList[3]=WWWWWWWWWWWW\nMenuList[18]=ZZZZZZZZZZZZZZZZZ\n"
		"[menuConfiguration]\nMenuTitle=GGGGGGGG\nMenuList[3]=RRRRRRRRRRRRRRRRR\n"
		"MenuList[5]=VVVVV\nMenuList[15]=AAAAAAAAAAAAA\n"
		"[menuMultiPlayer]\nMenuList[2]=PPPPPPPPPPPPP\n[menuPlayer]\n"
		"MenuTitle=RRRRRRRRRRRRR\nMenuList[1]=NNNNN\nHelpMessage[1]=IIIIIIII\n";
	TMap<FName, FString> Projected;
	TestTrue(TEXT("settings labels parse"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
			MakeArrayView(reinterpret_cast<const uint8*>(Input), sizeof(Input) - 1), Projected));
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("menuOptions.MenuTitle"), TEXT("OOOOOOOO")},
		{TEXT("menuOptions.MenuList[3]"), TEXT("EEEEEEEEEEEEEEEEE")},
		{TEXT("menuKeyboard.MenuList[3]"), TEXT("WWWWWWWWWWWW")},
		{TEXT("menuKeyboard.MenuList[18]"), TEXT("ZZZZZZZZZZZZZZZZZ")},
		{TEXT("menuConfiguration.MenuTitle"), TEXT("GGGGGGGG")},
		{TEXT("menuConfiguration.MenuList[15]"), TEXT("AAAAAAAAAAAAA")},
		{TEXT("menuMultiPlayer.MenuList[2]"), TEXT("PPPPPPPPPPPPP")},
		{TEXT("menuPlayer.MenuTitle"), TEXT("RRRRRRRRRRRRR")},
		{TEXT("menuPlayer.MenuList[1]"), TEXT("NNNNN")},
		{TEXT("menuPlayer.HelpMessage[1]"), TEXT("IIIIIIII")}};
	for (const auto& Case : Cases)
		TestEqual(Case.Key, Projected.FindRef(FName(Case.Key)), FString(Case.Value));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityFix18SettingsLabelsSpec,
	"WoT.Parity.Fix18.SettingsLabelsFromRetailText", ScreenFlags)

bool FWoTParityFix18SettingsLabelsSpec::RunTest(const FString&)
{
	// The same fixed-list projection left Controls rows (Always MouseLook,
	// Crosshair, Drop Item, Walk...) and Hardware's Brightness as raw keys in
	// the neutral package. Drive the real projection into both real pages with
	// the editor-only menu strings asset hidden, as players run them.
	FActivatedPreferencesFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	FWoTMenuStyle::Get().SetEditorMenuStringsHiddenForTesting(true);
	ON_SCOPE_EXIT { FWoTMenuStyle::Get().SetEditorMenuStringsHiddenForTesting(false); };
	TMap<FName, FString> Projected;
	if (!TestTrue(TEXT("the placeholder WoT.int projects"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
			WoTFrontEndTest_PlaceholderMenuLocalization(), Projected)))
	{
		return false;
	}
	struct FPage { int32 Entry; EWoTFrontEndScreen Screen; const TCHAR* Name;
		TArray<const TCHAR*> Expected; };
	// Sections: 5 menuOptions, 6 menuKeyboard, 7 menuConfiguration.
	const FPage Pages[] = {
		{1, EWoTFrontEndScreen::Controls, TEXT("Controls"),
			{TEXT("Label 5 4"), TEXT("Label 5 7"), TEXT("Label 6 11"), TEXT("Label 6 16")}},
		{2, EWoTFrontEndScreen::Hardware, TEXT("Hardware"), {TEXT("Label 7 1")}}};
	for (const FPage& Page : Pages)
	{
		const TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
			FWoTFrontEndPresentationAuthority::CreateForTesting(TEXT("generation-input"),
				FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
				FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
				TMap<FName, FString>(Projected), MakeShared<FSettingsFeedbackRecorder>());
		const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
			.PlayerController(nullptr)
			.PresentationAuthorityForTesting(Authority)
			.PreferencesForTesting(Fixture.Owner);
		const FGeometry Geometry = FGeometry::MakeRoot(
			FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform());
		Root->Tick(Geometry, 0.0, 0.0f);
		Root->ActivateMainMenuEntryForTesting(Page.Entry);
		if (!TestTrue(FString::Printf(TEXT("%s opens"), Page.Name),
			Root->CurrentScreenForTesting() == Page.Screen))
		{
			continue;
		}
		Root->Tick(Geometry, 0.0, 0.0f);
		Root->SlatePrepass(1.0f);
		TArray<TSharedRef<STextBlock>> Texts;
		CollectText(Root, Texts);
		TestTrue(FString::Printf(TEXT("%s has rows to check"), Page.Name), Texts.Num() > 5);
		for (const TSharedRef<STextBlock>& Text : Texts)
		{
			const FString Shown = Text->GetText().ToString();
			TestFalse(FString::Printf(TEXT("%s shows no raw localization key (%s)"),
				Page.Name, *Shown), WoTFrontEndTest_IsRawMenuKey(Shown));
		}
		for (const TCHAR* Label : Page.Expected)
		{
			TestTrue(FString::Printf(TEXT("%s shows the projected label %s"), Page.Name, Label),
				HasText(Texts, Label));
		}
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
