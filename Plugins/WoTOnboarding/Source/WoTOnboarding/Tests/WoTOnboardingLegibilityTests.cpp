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

// Steam Deck legibility for every line the onboarding widgets draw, and the
// window scale they draw at. Valve's compatibility checklist rejects text
// under 9 px tall at 1280x800 and recommends 12 px. These specs hold every
// onboarding line to 12 px on that screen, and to the 9 px floor at 1280x720
// (the Deck's 16:9 mode). They cover the engine stand-in faces and published
// faces that are all too small. Synthetic fixtures with invented names and
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
    charges line, the gem bar, description, divider, quote, hint, both icon
    rows, and the trap group. */
UWoTAngrealGuide* WoTOnboardingLegibilityMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow Spark;
	Spark.ClassName = TEXT("SparkFlail");
	Spark.Rarity = TEXT("Common");
	Spark.Elements = {FName(TEXT("Fire")), FName(TEXT("Air"))};
	Spark.MaxInitialCharges = 4;
	Spark.MaxCharges = 10;
	Spark.HintText = TEXT("{counter:DamperDisc} smothers it.");
	Spark.Counters = {FName(TEXT("SnareCrate"))};
	Guide->Rows.Add(Spark);

	FWoTAngrealGuideRow Damper;
	Damper.ClassName = TEXT("DamperDisc");
	Damper.Rarity = TEXT("Rare");
	Damper.Elements = {FName(TEXT("Water"))};
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

/** Published faces that all answer far below the Deck minimum, at every
    strike scale. */
TOptional<FWoTOnboardingMenuChrome> WoTOnboardingLegibilityTinyChrome()
{
	FWoTOnboardingMenuChrome Chrome;
	Chrome.PixelFont = [](FName, int32)
	{
		return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 6);
	};
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
		{TEXT("engine stand-in faces"), nullptr},
		{TEXT("undersized published faces"), &WoTOnboardingLegibilityTinyChrome},
	};
}

/** Errors for every line under the Deck minimum; returns the line count. */
int32 WoTOnboardingLegibilityExpectLines(FAutomationTestBase& Test,
	const FString& Context, const TSharedRef<SWidget>& Widget,
	TArray<FString>& OutTexts)
{
	const float DeckScale = WoTOnboardingChrome::WindowScaleAt(1280, 800);
	const float WideScale = WoTOnboardingChrome::WindowScaleAt(1280, 720);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingLegibilityWindowScaleSpec,
	"WoT.Onboarding.LegibilityWindowScale", WoTOnboardingLegibilityFlags)

bool FWoTOnboardingLegibilityWindowScaleSpec::RunTest(const FString&)
{
	using WoTOnboardingChrome::PixelScaleFor;
	using WoTOnboardingChrome::WindowScaleAt;
	// Whole screen pixels per unit wherever the 960x540 window fits.
	TestEqual(TEXT("the Deck draws the window at 1x"), WindowScaleAt(1280, 800), 1.0f);
	TestEqual(TEXT("so does the Deck's 16:9 mode"), WindowScaleAt(1280, 720), 1.0f);
	TestEqual(TEXT("1080p draws at 2x"), WindowScaleAt(1920, 1080), 2.0f);
	TestEqual(TEXT("1440p stays at 2x"), WindowScaleAt(2560, 1440), 2.0f);
	TestEqual(TEXT("4K stays at 2x, the largest strike"), WindowScaleAt(3840, 2160), 2.0f);
	TestEqual(TEXT("a screen narrower than the window fits by its width"),
		WindowScaleAt(800, 600), 800.0f / 960.0f, WoTOnboardingLegibilityTolerance);
	TestEqual(TEXT("no screen yet keeps 1x"), WindowScaleAt(0, 0), 1.0f);
	TestEqual(TEXT("the strike for a small screen is 1x"),
		PixelScaleFor(FVector2D(800.0, 600.0)), 1);
	TestEqual(TEXT("the strike at 1080p is 2x"),
		PixelScaleFor(FVector2D(1920.0, 1080.0)), 2);

	// A window opened at 1080p under a 1.5 UI DPI scale still lands on 2
	// screen pixels per unit: the scale box divides the DPI scale back out.
	FWoTOnboardingWindowScale Scale;
	TestEqual(TEXT("an uninitialized window draws at 1x"), Scale.SlateScale(), 1.0f);
	Scale.Initialize(FVector2D(1920.0, 1080.0), 1.5f);
	TestEqual(TEXT("1080p picks the 2x strike"), Scale.GetPixelScale(), 2);
	TestEqual(TEXT("1080p draws 2 pixels per unit"), Scale.GetWindowScale(), 2.0f);
	TestEqual(TEXT("the scale box undoes the DPI scale"), Scale.SlateScale(),
		2.0f / 1.5f, WoTOnboardingLegibilityTolerance);
	Scale.Initialize(FVector2D::ZeroVector, 3.0f);
	TestEqual(TEXT("a zero viewport keeps the last scale"), Scale.GetWindowScale(), 2.0f);

	// The faces follow the strike scale: the published faces are asked for
	// the strike the window opened at.
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	TSharedRef<TArray<int32>> Asked = MakeShared<TArray<int32>>();
	Seams.ResolveMenuChrome = [Asked]()
	{
		FWoTOnboardingMenuChrome Chrome;
		Chrome.PixelFont = [Asked](FName, int32 PixelScale)
		{
			Asked->AddUnique(PixelScale);
			return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
		};
		return TOptional<FWoTOnboardingMenuChrome>(Chrome);
	};
	FWoTPickupCardText CardText;
	CardText.Title = TEXT("Spark Flail");
	const TSharedRef<SWoTPickupCard> Card = SNew(SWoTPickupCard)
		.Text(CardText)
		.InitialPhysicalSize(FVector2D(1920.0, 1080.0));
	TestEqual(TEXT("the card holds its text"), Card->GetCardText().Title,
		CardText.Title);
	TestTrue(TEXT("a card opened at 1080p draws the 2x strike"), Asked->Contains(2));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingLegibilityFloorSpec,
	"WoT.Onboarding.LegibilityFloor", WoTOnboardingLegibilityFlags)

bool FWoTOnboardingLegibilityFloorSpec::RunTest(const FString&)
{
	const float DeckScale = WoTOnboardingChrome::WindowScaleAt(1280, 800);
	TestEqual(TEXT("the Deck's minimum is 9 points at 1x"),
		WoTOnboardingChrome::DeckMinimumFontSize(), 9.0f,
		WoTOnboardingLegibilityTolerance);
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

	using WoTOnboardingChrome::EFace;
	const TPair<const TCHAR*, EFace> Faces[] = {
		{TEXT("title"), EFace::Title},
		{TEXT("body"), EFace::Body},
		{TEXT("italic"), EFace::Italic},
		{TEXT("element"), EFace::Element},
		{TEXT("charge"), EFace::Charge}};
	for (const FWoTOnboardingLegibilityChromeCase& Case :
		WoTOnboardingLegibilityChromeCases())
	{
		Seams.ResolveMenuChrome = Case.Resolver;
		const FWoTOnboardingMenuChrome Chrome = WoTOnboardingChrome::Resolve();
		const FWoTOnboardingFaces Resolved(Chrome);
		for (const TPair<const TCHAR*, EFace>& Face : Faces)
		{
			// Each face knows whether it drew the published strike, which
			// the charge mark's glyph choice follows.
			TestEqual(FString::Printf(TEXT("%s: the %s face reports its source"),
				Case.Name, Face.Key), Resolved.IsPublished(Face.Value),
				WoTOnboardingChrome::IsRetail(Chrome));
			for (const int32 PixelScale : {1, 2})
			{
				const float DeckPixels = WoTOnboardingChrome::PhysicalPixels(
					WoTOnboardingChrome::Font(Chrome, Face.Value, PixelScale),
					DeckScale);
				TestTrue(FString::Printf(
					TEXT("%s: the %s face at %dx draws at least %.0f px at 1280x800 (%.1f)"),
					Case.Name, Face.Key, PixelScale,
					WoTOnboardingChrome::DeckMinimumTextPixels, DeckPixels),
					DeckPixels + WoTOnboardingLegibilityTolerance
						>= WoTOnboardingChrome::DeckMinimumTextPixels);
			}
		}
	}

	// The floor only lifts: faces already above it keep their sizes.
	const FWoTOnboardingMenuChrome Fallback;
	TestFalse(TEXT("an empty chrome is not the published look"),
		WoTOnboardingChrome::IsRetail(Fallback));
	TestEqual(TEXT("the stand-in title keeps its size"),
		WoTOnboardingChrome::Font(Fallback, EFace::Title, 1).Size, 18.0f);
	TestEqual(TEXT("the stand-in body keeps its size"),
		WoTOnboardingChrome::Font(Fallback, EFace::Body, 1).Size, 10.0f);
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

	// Every widget opens as it would on the Deck.
	const FVector2D Deck(1280.0, 800.0);
	UWoTAngrealGuide* Guide = WoTOnboardingLegibilityMakeGuide();
	for (const FWoTOnboardingLegibilityChromeCase& Case :
		WoTOnboardingLegibilityChromeCases())
	{
		Seams.ResolveMenuChrome = Case.Resolver;

		// The browser, once per detail page so every pane is built.
		const TSharedRef<SWoTGuideBrowser> Browser = SNew(SWoTGuideBrowser)
			.Guide(Guide)
			.InitialPhysicalSize(Deck)
			.TextResolver(&WoTOnboardingLegibilityText);
		TArray<FString> Texts;
		int32 Lines = WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, browser"), Case.Name), Browser, Texts);
		Browser->SelectRow(TEXT("DamperDisc"));
		Lines += WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, browser"), Case.Name), Browser, Texts);
		TestTrue(FString::Printf(TEXT("%s: the browser drew text"), Case.Name),
			Lines >= 20);
		// The labels, the gem names and the prose are all present, so the
		// size checks above covered them.
		for (const TCHAR* Expected : {TEXT("FIELD GUIDE"), TEXT("COMMON"),
			TEXT("RARE"), TEXT("TRAPS"), TEXT("COUNTERS"), TEXT("COUNTERED BY"),
			TEXT("FIRE"), TEXT("EARTH"), TEXT("Common artifact"),
			TEXT("Charges:  4 / 10"), TEXT("An invented flail of sparks."),
			TEXT("An invented line of verse."), TEXT("Damper Disc smothers it.")})
		{
			TestTrue(FString::Printf(TEXT("%s: the browser shows \"%s\""),
				Case.Name, Expected), Texts.Contains(FString(Expected)));
		}

		// The first-pickup card.
		FWoTPickupCardText CardText;
		CardText.ItemClass = TEXT("SparkFlail");
		CardText.Title = TEXT("Spark Flail");
		CardText.Summary = TEXT("An invented flail of sparks.");
		CardText.KeyHint = TEXT("F3  Field Guide");
		TArray<FString> CardTexts;
		const int32 CardLines = WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, pickup card"), Case.Name),
			SNew(SWoTPickupCard).Text(CardText).InitialPhysicalSize(Deck), CardTexts);
		TestEqual(FString::Printf(TEXT("%s: the card drew its three lines"),
			Case.Name), CardLines, 3);

		// The death recap.
		FWoTPickupCardText RecapText = CardText;
		RecapText.Kicker = TEXT("KILLED BY");
		RecapText.CounterClass = TEXT("DamperDisc");
		RecapText.SummaryKicker = TEXT("COUNTER");
		RecapText.Summary = TEXT("Damper Disc smothers it.");
		TArray<FString> RecapTexts;
		const int32 RecapLines = WoTOnboardingLegibilityExpectLines(*this,
			FString::Printf(TEXT("%s, death recap"), Case.Name),
			SNew(SWoTPickupCard).Text(RecapText).InitialPhysicalSize(Deck), RecapTexts);
		TestEqual(FString::Printf(TEXT("%s: the recap drew its five lines"),
			Case.Name), RecapLines, 5);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
