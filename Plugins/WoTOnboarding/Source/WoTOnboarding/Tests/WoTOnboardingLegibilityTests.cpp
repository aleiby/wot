#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTGuideBrowser.h"
#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingChrome.h"
#include "WoTOnboardingSeams.h"

#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"
#include "Widgets/Text/STextBlock.h"

// Steam Deck legibility for every line the onboarding widgets draw. Valve's
// compatibility checklist rejects text under 9 px tall at 1280x800 and
// recommends 12 px. These specs hold onboarding text to 12 px on that screen,
// and to the 9 px floor at 1280x720 (the Deck's 16:9 mode), on the project's
// own UI DPI curve. They cover the fallback chrome and a published chrome
// whose faces are all too small. Synthetic fixtures with invented names and
// prose; helper names carry the WoTOnboardingLegibility prefix for the shared
// unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingLegibilityFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** The Deck's hard floor at 1280x720, where Valve sets no recommendation. */
constexpr float WoTOnboardingLegibilityFloorPixels = 9.0f;

/** Rounding slack for the float conversions between points and pixels. */
constexpr float WoTOnboardingLegibilityTolerance = 0.01f;

/** A text block a widget draws, with the font it draws in. */
struct FWoTOnboardingLegibilityLine
{
	FString Text;
	FSlateFontInfo Font;
};

void WoTOnboardingLegibilityCollect(const TSharedRef<SWidget>& Widget,
	TArray<FWoTOnboardingLegibilityLine>& Out)
{
	if (Widget->GetType() == FName(TEXT("STextBlock")))
	{
		const TSharedRef<STextBlock> Block = StaticCastSharedRef<STextBlock>(Widget);
		Out.Add({Block->GetText().ToString(), Block->GetFont()});
	}
	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			WoTOnboardingLegibilityCollect(Children->GetChildAt(Index), Out);
		}
	}
}

/** Rows that make the browser build every pane it has: the rarity and
    charges line, element gems, roles, description, quote, hint, both link
    lists, and the trap group. */
UWoTAngrealGuide* WoTOnboardingLegibilityMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow Spark;
	Spark.ClassName = TEXT("SparkFlail");
	Spark.Rarity = TEXT("Common");
	Spark.Elements = {FName(TEXT("Fire")), FName(TEXT("Air"))};
	Spark.Roles = {FName(TEXT("Combat")), FName(TEXT("Offensive"))};
	Spark.MaxInitialCharges = 4;
	Spark.MaxCharges = 10;
	Spark.HintText = TEXT("{counter:DamperDisc} smothers it.");
	Spark.Counters = {FName(TEXT("SnareCrate"))};
	Guide->Rows.Add(Spark);

	FWoTAngrealGuideRow Damper;
	Damper.ClassName = TEXT("DamperDisc");
	Damper.Rarity = TEXT("Rare");
	Damper.Elements = {FName(TEXT("Water"))};
	Damper.Roles = {FName(TEXT("Defensive"))};
	Damper.Counters = {FName(TEXT("SparkFlail"))};
	Guide->Rows.Add(Damper);

	FWoTAngrealGuideRow Snare;
	Snare.ClassName = TEXT("SnareCrate");
	Guide->Rows.Add(Snare);
	return Guide;
}

FString WoTOnboardingLegibilityText(const FString& Section,
	const FString& Property)
{
	if (Property == TEXT("Title"))
	{
		if (Section == TEXT("SparkFlail")) return TEXT("Spark Flail");
		if (Section == TEXT("DamperDisc")) return TEXT("Damper Disc");
		if (Section == TEXT("SnareCrate")) return TEXT("Snare Crate");
	}
	if (Section == TEXT("SparkFlail"))
	{
		if (Property == TEXT("Description")) return TEXT("An invented flail of sparks.");
		if (Property == TEXT("Quote")) return TEXT("An invented line of verse.");
	}
	return FString();
}

/** A published chrome whose every face is far below the Deck minimum. */
TOptional<FWoTOnboardingMenuChrome> WoTOnboardingLegibilityTinyChrome()
{
	FWoTOnboardingMenuChrome Chrome;
	Chrome.TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 6);
	Chrome.BodyFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 6);
	Chrome.ItalicFont = FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 6);
	Chrome.SmallFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 6);
	const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	Chrome.Background = White;
	Chrome.Frame = White;
	Chrome.Blackout = White;
	Chrome.Separator = White;
	Chrome.BindWidget = [](const TSharedRef<SWidget>&) {};
	return Chrome;
}

struct FWoTOnboardingLegibilityChromeCase
{
	const TCHAR* Name;
	TFunction<TOptional<FWoTOnboardingMenuChrome>()> Resolver;
};

TArray<FWoTOnboardingLegibilityChromeCase> WoTOnboardingLegibilityChromeCases()
{
	return {
		{TEXT("fallback chrome"), nullptr},
		{TEXT("undersized published chrome"), &WoTOnboardingLegibilityTinyChrome},
	};
}

/** Errors for every line under the Deck minimum; returns the line count. */
int32 WoTOnboardingLegibilityExpectLines(FAutomationTestBase& Test,
	const FString& Context, const TSharedRef<SWidget>& Widget,
	TArray<FString>& OutTexts)
{
	const float DeckScale = WoTOnboardingChrome::LayoutScaleAt(1280, 800);
	const float WideScale = WoTOnboardingChrome::LayoutScaleAt(1280, 720);
	TArray<FWoTOnboardingLegibilityLine> Lines;
	WoTOnboardingLegibilityCollect(Widget, Lines);
	for (const FWoTOnboardingLegibilityLine& Line : Lines)
	{
		OutTexts.Add(Line.Text);
		const float DeckPixels =
			WoTOnboardingChrome::PhysicalPixels(Line.Font, DeckScale);
		if (DeckPixels + WoTOnboardingLegibilityTolerance
			< WoTOnboardingChrome::DeckMinimumTextPixels)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: \"%s\" draws %.1f px tall at 1280x800 (minimum %.0f)"),
				*Context, *Line.Text, DeckPixels,
				WoTOnboardingChrome::DeckMinimumTextPixels));
		}
		const float WidePixels =
			WoTOnboardingChrome::PhysicalPixels(Line.Font, WideScale);
		if (WidePixels + WoTOnboardingLegibilityTolerance
			< WoTOnboardingLegibilityFloorPixels)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: \"%s\" draws %.1f px tall at 1280x720 (floor %.0f)"),
				*Context, *Line.Text, WidePixels,
				WoTOnboardingLegibilityFloorPixels));
		}
	}
	return Lines.Num();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingLegibilityFloorSpec,
	"WoT.Onboarding.LegibilityFloor", WoTOnboardingLegibilityFlags)

bool FWoTOnboardingLegibilityFloorSpec::RunTest(const FString&)
{
	const float DeckScale = WoTOnboardingChrome::LayoutScaleAt(1280, 800);
	const float WideScale = WoTOnboardingChrome::LayoutScaleAt(1280, 720);
	TestTrue(TEXT("the Deck's layout scale is a downscale on the DPI curve"),
		DeckScale > 0.5f && DeckScale < 1.0f);
	TestTrue(TEXT("the 16:9 mode lays out smaller still"), WideScale < DeckScale);

	FSlateFontInfo AtMinimum = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),
		WoTOnboardingChrome::DeckMinimumFontSize());
	TestEqual(TEXT("the minimum font size draws exactly the Deck minimum"),
		WoTOnboardingChrome::PhysicalPixels(AtMinimum, DeckScale),
		WoTOnboardingChrome::DeckMinimumTextPixels,
		WoTOnboardingLegibilityTolerance);

	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };

	for (const FWoTOnboardingLegibilityChromeCase& Case :
		WoTOnboardingLegibilityChromeCases())
	{
		Seams.ResolveMenuChrome = Case.Resolver;
		const FWoTOnboardingMenuChrome Chrome = WoTOnboardingChrome::Resolve();
		const TPair<const TCHAR*, const FSlateFontInfo*> Faces[] = {
			{TEXT("title"), &Chrome.TitleFont},
			{TEXT("body"), &Chrome.BodyFont},
			{TEXT("italic"), &Chrome.ItalicFont},
			{TEXT("small"), &Chrome.SmallFont}};
		for (const TPair<const TCHAR*, const FSlateFontInfo*>& Face : Faces)
		{
			const float DeckPixels =
				WoTOnboardingChrome::PhysicalPixels(*Face.Value, DeckScale);
			TestTrue(FString::Printf(
				TEXT("%s: the %s face draws at least %.0f px at 1280x800 (%.1f)"),
				Case.Name, Face.Key, WoTOnboardingChrome::DeckMinimumTextPixels,
				DeckPixels),
				DeckPixels + WoTOnboardingLegibilityTolerance
					>= WoTOnboardingChrome::DeckMinimumTextPixels);
		}
	}

	// The floor only lifts: faces already above it keep their sizes.
	Seams.ResolveMenuChrome = nullptr;
	const FWoTOnboardingMenuChrome Fallback = WoTOnboardingChrome::Resolve();
	TestEqual(TEXT("the fallback title keeps its size"),
		Fallback.TitleFont.Size, 26.0f);
	TestEqual(TEXT("the fallback body keeps its size"),
		Fallback.BodyFont.Size, 14.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingLegibilityWidgetsSpec,
	"WoT.Onboarding.LegibilityWidgets", WoTOnboardingLegibilityFlags)

bool FWoTOnboardingLegibilityWidgetsSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };

	UWoTAngrealGuide* Guide = WoTOnboardingLegibilityMakeGuide();
	for (const FWoTOnboardingLegibilityChromeCase& Case :
		WoTOnboardingLegibilityChromeCases())
	{
		Seams.ResolveMenuChrome = Case.Resolver;

		// The browser, once per detail page so every pane is built.
		const TSharedRef<SWoTGuideBrowser> Browser = SNew(SWoTGuideBrowser)
			.Guide(Guide)
			.TextResolver(&WoTOnboardingLegibilityText);
		TArray<FString> Texts;
		int32 Lines = WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, browser"), Case.Name), Browser, Texts);
		Browser->SelectRow(TEXT("DamperDisc"));
		Lines += WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, browser"), Case.Name), Browser, Texts);
		TestTrue(FString::Printf(TEXT("%s: the browser drew text"), Case.Name),
			Lines >= 20);
		// The lines that once used the menu's small face are all present,
		// so the size checks above covered them.
		for (const TCHAR* Expected : {TEXT("COMMON"), TEXT("RARE"),
			TEXT("TRAPS"), TEXT("COUNTERS"), TEXT("COUNTERED BY"),
			TEXT("[ON] FIRE"), TEXT("[--] EARTH"), TEXT("Combat, Offensive"),
			TEXT("An invented flail of sparks."), TEXT("An invented line of verse."),
			TEXT("Damper Disc smothers it.")})
		{
			TestTrue(FString::Printf(TEXT("%s: the browser shows \"%s\""),
				Case.Name, Expected), Texts.Contains(FString(Expected)));
		}

		// The first-pickup card.
		FWoTPickupCardText CardText;
		CardText.Title = TEXT("Spark Flail");
		CardText.Summary = TEXT("An invented flail of sparks.");
		CardText.KeyHint = TEXT("Press F3 for the field guide");
		TArray<FString> CardTexts;
		const int32 CardLines = WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, pickup card"), Case.Name),
			SNew(SWoTPickupCard).Text(CardText), CardTexts);
		TestEqual(FString::Printf(TEXT("%s: the card drew its three lines"),
			Case.Name), CardLines, 3);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
