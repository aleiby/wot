#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Settings/WoTGameSettings.h"
#include "../Settings/WoTSettingsSlate.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPreferencesSubsystem.h"
#include "../Payload/WoTPayloadActivation.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

// The settings screen's seam extension group (plan Task 4): rows registered
// in FWoTOnboardingSeams::SettingsRows render appended to the Controls page,
// and an empty registry renders exactly today's screen. The specs swap the
// live registry for fixture rows and restore it, so they hold with or
// without the onboarding plugin (and whatever it registered). All fixture
// vocabulary is invented; the construction mirrors the existing settings
// screen specs (activated preferences owner + testing authority), with
// file-unique helper names for the unity build.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingScreenFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FWoTOnboardingScreenFeedbackSink final : public IWoTFrontEndFeedbackSink
{
public:
	virtual bool IsReady() const override { return true; }
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

TArray<uint8> WoTOnboardingScreenUtf8(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

TSharedRef<FWoTFrontEndPresentationAuthority> MakeWoTOnboardingScreenAuthority()
{
	// No injected labels: the extension group and the literal-labeled rows the
	// specs assert on never resolve retail text.
	TMap<FName, FString> Labels;
	return FWoTFrontEndPresentationAuthority::CreateForTesting(
		TEXT("generation-input"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
		MoveTemp(Labels), MakeShared<FWoTOnboardingScreenFeedbackSink>());
}

TSharedRef<FWoTFrontEndPresentationAuthority>
MakeWoTOnboardingScreenMenuAuthority()
{
	// The front-end root hides its main menu until every retail entry label
	// measures, so the main-menu specs inject the four bounded fixture
	// labels (invented text, as the settings-screen specs inject theirs).
	TMap<FName, FString> Labels;
	Labels.Add(FName(TEXT("menuMain.MenuList[2]")), TEXT("FIXTUREPLAY"));
	Labels.Add(FName(TEXT("menuMain.MenuList[3]")), TEXT("FIXTURECONTROL"));
	Labels.Add(FName(TEXT("menuMain.MenuList[4]")), TEXT("FIXTUREHARDWARE"));
	Labels.Add(FName(TEXT("menuMain.MenuList[7]")), TEXT("FIXTUREQUIT"));
	return FWoTFrontEndPresentationAuthority::CreateForTesting(
		TEXT("generation-input"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
		MoveTemp(Labels), MakeShared<FWoTOnboardingScreenFeedbackSink>());
}

void WoTOnboardingScreenCollectText(
	const TSharedRef<SWidget>& Widget, TArray<TSharedRef<STextBlock>>& Out)
{
	if (!Widget->GetVisibility().IsVisible()) return;
	if (Widget->GetType() == FName(TEXT("STextBlock")))
		Out.Add(StaticCastSharedRef<STextBlock>(Widget));
	if (FChildren* Children = Widget->GetChildren())
		for (int32 Index = 0; Index < Children->Num(); ++Index)
			WoTOnboardingScreenCollectText(Children->GetChildAt(Index), Out);
}

bool WoTOnboardingScreenHasText(
	const TArray<TSharedRef<STextBlock>>& Texts, const TCHAR* Expected)
{
	for (const TSharedRef<STextBlock>& Text : Texts)
		if (Text->GetText().ToString() == Expected) return true;
	return false;
}

void WoTOnboardingScreenCollectButtons(
	const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SButton>>& Out)
{
	if (!Widget->GetVisibility().IsVisible()) return;
	if (Widget->GetType() == FName(TEXT("SWoTMenuButton")))
		Out.Add(StaticCastSharedRef<SButton>(Widget));
	if (FChildren* Children = Widget->GetChildren())
		for (int32 Index = 0; Index < Children->Num(); ++Index)
			WoTOnboardingScreenCollectButtons(Children->GetChildAt(Index), Out);
}

/** The menu button whose own text is exactly Text; null when absent. */
TSharedPtr<SButton> WoTOnboardingScreenFindButton(
	const TSharedRef<SWidget>& Root, const TCHAR* Text)
{
	TArray<TSharedRef<SButton>> Buttons;
	WoTOnboardingScreenCollectButtons(Root, Buttons);
	for (const TSharedRef<SButton>& Button : Buttons)
	{
		TArray<TSharedRef<STextBlock>> Texts;
		WoTOnboardingScreenCollectText(Button, Texts);
		if (WoTOnboardingScreenHasText(Texts, Text)) return Button;
	}
	return nullptr;
}

/** A real, activated preferences owner writing to a temporary file — the
    same mechanics as the settings screen specs' fixture. */
struct FWoTOnboardingScreenPrefsFixture
{
	FString PreviousCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	float PreviousScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale() : 1.0f;
	float PreviousVolume = FApp::GetVolumeMultiplier();
	FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("OnboardingScreen-")) + FGuid::NewGuid().ToString());
	FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	UWoTGameInstance* Instance = nullptr;
	UWoTPreferencesSubsystem* Owner = nullptr;
	bool bReady = false;

	FWoTOnboardingScreenPrefsFixture()
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
		ResetWoTPayloadActivationForTesting();
		Instance = NewObject<UWoTGameInstance>();
		Instance->AddToRoot();
		Owner = NewObject<UWoTPreferencesSubsystem>(Instance);
		Owner->AddToRoot();
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
			WoTOnboardingScreenUtf8("Schema=EarlyUI-v1\nUIScale=1.25\nLocale=fr-FR\n"),
			FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
		if (Parsed.IsError()) return;
		if (Owner->AdoptAuthenticatedEarlyUI(MoveTemp(Parsed).GetValue()).IsError()) return;
		if (PublishWoTPayloadBootstrapActivationForTesting().IsError()) return;
		bReady = !Owner->ApplyFullPreferencesAfterCacheActivation().IsError();
	}

	~FWoTOnboardingScreenPrefsFixture()
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

/** Empties the live seam settings registry for the spec; restores on exit. */
struct FWoTOnboardingScreenRowsGuard
{
	TArray<FWoTOnboardingRow> Saved;

	FWoTOnboardingScreenRowsGuard()
	{
		Saved = MoveTemp(FWoTOnboardingSeams::Get().SettingsRows);
		FWoTOnboardingSeams::Get().SettingsRows.Reset();
	}

	~FWoTOnboardingScreenRowsGuard()
	{
		FWoTOnboardingSeams::Get().SettingsRows = MoveTemp(Saved);
	}
};

/** Empties the live seam MENU registry for the spec; restores on exit. */
struct FWoTOnboardingScreenMenuRowsGuard
{
	TArray<FWoTOnboardingRow> Saved;

	FWoTOnboardingScreenMenuRowsGuard()
	{
		Saved = MoveTemp(FWoTOnboardingSeams::Get().MenuRows);
		FWoTOnboardingSeams::Get().MenuRows.Reset();
	}

	~FWoTOnboardingScreenMenuRowsGuard()
	{
		FWoTOnboardingSeams::Get().MenuRows = MoveTemp(Saved);
	}
};

/** The injected fonts the in-game menu overlay specs construct with. */
TSharedRef<FWoTRequiredMenuFonts> MakeWoTOnboardingScreenFonts(
	const TSharedRef<FWoTRequiredMenuFontGeneration>& Generation)
{
	TSharedRef<FWoTRequiredMenuFonts> Fonts =
		MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	return Fonts;
}

TSharedRef<SWoTSettingsScreen> MakeWoTOnboardingScreen(
	UWoTPreferencesSubsystem* Owner,
	const TSharedRef<FWoTFrontEndPresentationAuthority>& Authority)
{
	Authority->PrepareBrowserResources();
	return SNew(SWoTSettingsScreen)
		.Page(EWoTSettingsPage::Controls)
		.Preferences(Owner)
		.PresentationAuthority(Authority)
		.HeadingFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30))
		.BodyFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 28))
		.TextResolver(FWoTSettingsTextResolver::CreateLambda(
			[Authority](FName Key) { return Authority->Text(Key); }));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingScreenExtensionParitySpec,
	"WoT.Onboarding.ScreenExtensionParity", WoTOnboardingScreenFlags)

bool FWoTOnboardingScreenExtensionParitySpec::RunTest(const FString&)
{
	FWoTOnboardingScreenPrefsFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	FWoTOnboardingScreenRowsGuard RegistryGuard;
	const TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		MakeWoTOnboardingScreenAuthority();

	// Empty registry: no extension header, no extension rows — the screen is
	// today's screen.
	const TSharedRef<SWoTSettingsScreen> Baseline =
		MakeWoTOnboardingScreen(Fixture.Owner, Authority);
	Baseline->SlatePrepass(1.0f);
	TArray<TSharedRef<STextBlock>> BaselineTexts;
	WoTOnboardingScreenCollectText(Baseline, BaselineTexts);
	TestTrue(TEXT("the screen built its full Controls body"),
		WoTOnboardingScreenHasText(BaselineTexts, TEXT("Controller Sensitivity")));
	TestFalse(TEXT("an empty registry renders no extension header"),
		WoTOnboardingScreenHasText(BaselineTexts, TEXT("ONBOARDING")));

	// Two registered rows add exactly one group header and one label each.
	FWoTOnboardingSeams::Get().SettingsRows.Add({
		[]() { return FString(TEXT("Fixture Aid Alpha")); }, []() {}});
	FWoTOnboardingSeams::Get().SettingsRows.Add({
		[]() { return FString(TEXT("Fixture Aid Beta")); }, []() {}});
	const TSharedRef<SWoTSettingsScreen> Extended =
		MakeWoTOnboardingScreen(Fixture.Owner, Authority);
	Extended->SlatePrepass(1.0f);
	TArray<TSharedRef<STextBlock>> ExtendedTexts;
	WoTOnboardingScreenCollectText(Extended, ExtendedTexts);
	TestEqual(TEXT("registered rows add exactly the header plus one label each"),
		ExtendedTexts.Num(), BaselineTexts.Num() + 3);
	TestTrue(TEXT("the extension header renders"),
		WoTOnboardingScreenHasText(ExtendedTexts, TEXT("ONBOARDING")));
	TestTrue(TEXT("the first registered row renders"),
		WoTOnboardingScreenHasText(ExtendedTexts, TEXT("Fixture Aid Alpha")));
	TestTrue(TEXT("the second registered row renders"),
		WoTOnboardingScreenHasText(ExtendedTexts, TEXT("Fixture Aid Beta")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingScreenExtensionRowsSpec,
	"WoT.Onboarding.ScreenExtensionRows", WoTOnboardingScreenFlags)

bool FWoTOnboardingScreenExtensionRowsSpec::RunTest(const FString&)
{
	FWoTOnboardingScreenPrefsFixture Fixture;
	if (!TestTrue(TEXT("preferences fixture activates"), Fixture.bReady)) return false;
	bool bAidOn = false;
	int32 Invocations = 0;
	FWoTOnboardingScreenRowsGuard RegistryGuard;
	FWoTOnboardingSeams::Get().SettingsRows.Add({
		[&bAidOn]()
		{
			return FString::Printf(TEXT("Fixture Aid Gamma: %s"),
				bAidOn ? TEXT("On") : TEXT("Off"));
		},
		[&bAidOn, &Invocations]()
		{
			bAidOn = !bAidOn;
			++Invocations;
		}});

	const TSharedRef<SWoTSettingsScreen> Screen = MakeWoTOnboardingScreen(
		Fixture.Owner, MakeWoTOnboardingScreenAuthority());
	Screen->SlatePrepass(1.0f);
	const TSharedPtr<SButton> Row = WoTOnboardingScreenFindButton(
		Screen, TEXT("Fixture Aid Gamma: Off"));
	if (!TestTrue(TEXT("the registered row renders as a menu button"),
		Row.IsValid()))
	{
		return false;
	}

	Row->SimulateClick();
	TestEqual(TEXT("clicking the row fired its invoke"), Invocations, 1);
	TestTrue(TEXT("the invoke flipped the fixture state"), bAidOn);

	// The label resolves lazily: the same widget now reads the new state.
	TArray<TSharedRef<STextBlock>> Texts;
	WoTOnboardingScreenCollectText(Screen, Texts);
	TestTrue(TEXT("the row label follows the live state"),
		WoTOnboardingScreenHasText(Texts, TEXT("Fixture Aid Gamma: On")));
	TestFalse(TEXT("the stale label is gone"),
		WoTOnboardingScreenHasText(Texts, TEXT("Fixture Aid Gamma: Off")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingInGameMenuExtensionSpec,
	"WoT.Onboarding.InGameMenuExtension", WoTOnboardingScreenFlags)

bool FWoTOnboardingInGameMenuExtensionSpec::RunTest(const FString&)
{
	FWoTOnboardingScreenMenuRowsGuard RegistryGuard;
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	const TSharedRef<FWoTRequiredMenuFonts> Fonts =
		MakeWoTOnboardingScreenFonts(Generation);

	// Empty registry: the overlay renders exactly today's entry list.
	const TSharedRef<SWoTInGameMenuOverlay> Baseline =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(nullptr)
		.RequiredFontsForTesting(Fonts);
	const TArray<FString> BaselineLabels = Baseline->MenuLabelsForTesting();
	TestTrue(TEXT("the overlay built the retail entry list"),
		BaselineLabels.Contains(TEXT("Resume"))
		&& BaselineLabels.Contains(TEXT("Quit to Main Menu")));
	TestFalse(TEXT("an empty registry appends no extension entry"),
		BaselineLabels.Contains(TEXT("Fixture Guide Row")));

	// One registered row adds exactly one entry, just above Quit, which stays
	// last; its activation runs the row's invoke.
	int32 Invocations = 0;
	FWoTOnboardingSeams::Get().MenuRows.Add({
		[]() { return FString(TEXT("Fixture Guide Row")); },
		[&Invocations]() { ++Invocations; },
		FName(TEXT("FixtureOwner"))});
	const TSharedRef<SWoTInGameMenuOverlay> Extended =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(nullptr)
		.RequiredFontsForTesting(Fonts);
	const int32 QuitIndex = BaselineLabels.Num() - 1;
	if (!TestTrue(TEXT("today's list ends with Quit"), QuitIndex >= 0
		&& BaselineLabels[QuitIndex] == TEXT("Quit to Main Menu")))
	{
		return false;
	}
	TArray<FString> ExpectedLabels = BaselineLabels;
	ExpectedLabels.Insert(TEXT("Fixture Guide Row"), QuitIndex);
	TestTrue(TEXT("the registered row sits just above Quit"),
		Extended->MenuLabelsForTesting() == ExpectedLabels);
	Extended->ActivateVisibleMainMenuEntryForTesting(QuitIndex);
	TestEqual(TEXT("activating the row's entry fires its invoke"),
		Invocations, 1);

	// The live/snapshot comparison rebuilds the open menu when the registry
	// drifts (the guide row follows the master toggle live).
	FWoTOnboardingSeams::Get().MenuRows.Reset();
	Extended->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	TestTrue(TEXT("an unregistered row leaves the open menu on the next tick"),
		Extended->MenuLabelsForTesting() == BaselineLabels);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingFrontEndMenuExtensionSpec,
	"WoT.Onboarding.FrontEndMenuExtension", WoTOnboardingScreenFlags)

bool FWoTOnboardingFrontEndMenuExtensionSpec::RunTest(const FString&)
{
	FWoTOnboardingScreenMenuRowsGuard RegistryGuard;
	const FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform());

	// Empty registry: exactly the four retail entries.
	const TSharedRef<SWoTFrontEndRoot> Baseline = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeWoTOnboardingScreenMenuAuthority());
	Baseline->Tick(Geometry, 0.0, 0.0f);
	TestTrue(TEXT("the retail main menu renders its four entries"),
		Baseline->MainMenuButtonForTesting(3).IsValid());
	TestFalse(TEXT("an empty registry appends no fifth entry"),
		Baseline->MainMenuButtonForTesting(4).IsValid());

	int32 Invocations = 0;
	FWoTOnboardingSeams::Get().MenuRows.Add({
		[]() { return FString(TEXT("Fixture Guide Row")); },
		[&Invocations]() { ++Invocations; },
		FName(TEXT("FixtureOwner"))});
	const TSharedRef<SWoTFrontEndRoot> Extended = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeWoTOnboardingScreenMenuAuthority());
	Extended->Tick(Geometry, 0.0, 0.0f);
	TestTrue(TEXT("the registered row renders as a fifth entry"),
		Extended->MainMenuButtonForTesting(4).IsValid());
	TArray<TSharedRef<STextBlock>> Texts;
	WoTOnboardingScreenCollectText(Extended, Texts);
	TestTrue(TEXT("the appended entry carries the row's label"),
		WoTOnboardingScreenHasText(Texts, TEXT("Fixture Guide Row")));
	TestTrue(TEXT("activating the appended entry is handled"),
		Extended->ActivateMainMenuEntryForTesting(4).IsEventHandled());
	TestEqual(TEXT("activation fired the row's invoke"), Invocations, 1);

	// Registry drift while the main screen shows rebuilds it from the live
	// rows on the next tick.
	FWoTOnboardingSeams::Get().MenuRows.Reset();
	Extended->Tick(Geometry, 0.0, 0.0f);
	TestFalse(TEXT("an unregistered row leaves the main screen on the next tick"),
		Extended->MainMenuButtonForTesting(4).IsValid());
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
