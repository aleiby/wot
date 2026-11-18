#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "SWoTGuideBrowser.h"
#include "WoTOnboardingChrome.h"
#include "WoTOnboardingSeams.h"
#include "Layout/Geometry.h"
#include "Misc/ScopeExit.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "UObject/Package.h"

namespace
{
constexpr EAutomationTestFlags WoTOnboardingWholePixelFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName WoTOnboardingWholePixelNames[] = {
	TEXT("F_WOTReg30"), TEXT("F_WOTReg14"), TEXT("F_WOTIta14"),
	TEXT("F_Element"), TEXT("F_Charge")};
constexpr float WoTOnboardingWholePixelHeights[] = {30, 14, 14, 19, 16};

FWoTOnboardingMenuChrome WoTOnboardingWholePixelChrome(TArray<TPair<FName, int32>>& Asked)
{
	FWoTOnboardingMenuChrome Result;
	Result.PixelFont = [&Asked](FName FaceName, int32 Step)
	{
		Asked.Emplace(FaceName, Step);
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(WoTOnboardingWholePixelNames); ++Index)
		{
			if (FaceName == WoTOnboardingWholePixelNames[Index])
				return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),
					WoTOnboardingWholePixelHeights[Index] * 0.75f);
		}
		return FSlateFontInfo();
	};
	return Result;
}

UWoTAngrealGuide* WoTOnboardingWholePixelGuide()
{
	UWoTAngrealGuide* Result = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	for (const TCHAR* Name : {TEXT("Spark"), TEXT("Ward"), TEXT("Snare")})
	{
		FWoTAngrealGuideRow Row;
		Row.ClassName = Name;
		Row.Rarity = TEXT("Common");
		Row.MaxInitialCharges = 1;
		Row.MaxCharges = 2;
		Row.ChargeCost = 1;
		Result->Rows.Add(Row);
	}
	Result->Rows[0].HintText = TEXT("Use {counter:Ward}.");
	Result->Rows[0].Counters = {FName(TEXT("Snare"))};
	Result->Rows[1].Counters = {FName(TEXT("Spark"))};
	Result->Rows[1].Rarity = TEXT("Rare");
	Result->Rows[1].ChargeCost = 0;
	Result->Rows[2].Rarity = NAME_None;
	Result->Rows[2].ChargeCost = 0;
	return Result;
}

FString WoTOnboardingWholePixelText(const FString&, const FString& Property)
{
	if (Property == TEXT("Title")) return TEXT("Spark");
	if (Property == TEXT("Description")) return TEXT("A spark.");
	if (Property == TEXT("Quote")) return TEXT("Light.");
	if (Property == TEXT("RarityCommonStr") || Property == TEXT("RarityRareStr")) return TEXT("Common");
	if (Property == TEXT("ChargesStr")) return TEXT("Uses:");
	return FString();
}

void WoTOnboardingWholePixelWalk(FAutomationTestBase& Test,
	const TSharedRef<SWidget>& Widget, const TArray<float>& Allowed, int32& Count)
{
	if (Widget->GetVisibility() == EVisibility::Collapsed) return;
	if (Widget->GetType() == FName(TEXT("STextBlock")))
	{
		const TSharedRef<STextBlock> Block = StaticCastSharedRef<STextBlock>(Widget);
		const float Size = Block->GetFont().Size;
		Test.TestTrue(Block->GetText().ToString() + TEXT(" uses a whole-pixel face"),
			Allowed.ContainsByPredicate([Size](float Value) { return FMath::IsNearlyEqual(Size, Value, 0.001f); }));
		++Count;
	}
	if (FChildren* Children = Widget->GetChildren())
		for (int32 Index = 0; Index < Children->Num(); ++Index)
			WoTOnboardingWholePixelWalk(Test, Children->GetChildAt(Index), Allowed, Count);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserWholePixelTextSpec,
	"WoT.Onboarding.BrowserWholePixelText", WoTOnboardingWholePixelFlags)
bool FWoTOnboardingBrowserWholePixelTextSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const auto SavedResolver = Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	TArray<TPair<FName, int32>> Asked;
	Seams.ResolveMenuChrome = [&Asked]() { return TOptional<FWoTOnboardingMenuChrome>(WoTOnboardingWholePixelChrome(Asked)); };
	struct FScreen { FVector2D Size; float Dpi; int32 Base; int32 Reading; };
	const FScreen Cases[] = {
		{{3024, 1964}, 1, 3, 5}, {{2880, 1800}, 1, 3, 5},
		{{2560, 1440}, 1, 3, 4}, {{1920, 1080}, 1, 2, 3},
		{{1280, 800}, 1, 1, 2}, {{3840, 2160}, 1, 4, 6},
		{{3024, 1964}, 2, 3, 5}};
	for (const FScreen& Screen : Cases)
	{
		Asked.Reset();
		const auto Browser = SNew(SWoTGuideBrowser).Guide(WoTOnboardingWholePixelGuide())
			.InitialPhysicalSize(Screen.Size).InitialDpiScale(Screen.Dpi)
			.TextResolver(&WoTOnboardingWholePixelText);
		const float RenderScale = Browser->WindowScaleForTesting();
		TestEqual(TEXT("base step"), Browser->BaseStepForTesting(), Screen.Base);
		TestEqual(TEXT("highest reading step fits"), Browser->ReadingStepForTesting(), Screen.Reading);
		TestEqual(TEXT("physical fill scale includes DPI exactly once"), RenderScale,
			WoTOnboardingChrome::WindowScaleFor(Screen.Size, WoTOnboardingChrome::EWindowScaleRule::GuideFill), 0.0001f);
		AddInfo(FString::Printf(TEXT("%.0fx%.0f DPI %.1f: S %.6f base %d reading %d"),
			Screen.Size.X, Screen.Size.Y, Screen.Dpi, RenderScale,
			Browser->BaseStepForTesting(), Browser->ReadingStepForTesting()));
		TArray<float> Allowed;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(WoTOnboardingWholePixelNames); ++Index)
		{
			for (const auto Role : {WoTOnboardingChrome::ETextRole::Base, WoTOnboardingChrome::ETextRole::Reading})
			{
				const int32 Step = Role == WoTOnboardingChrome::ETextRole::Base ? Screen.Base : Screen.Reading;
				TestTrue(TEXT("requested face and strike"), Asked.Contains(TPair<FName, int32>(WoTOnboardingWholePixelNames[Index], Step)));
				const float Expected = WoTOnboardingWholePixelHeights[Index] * 0.75f * Step / RenderScale;
				const float Actual = Browser->FacesForTesting()->Get(static_cast<WoTOnboardingChrome::EFace>(Index), Role).Size;
				TestEqual(TEXT("exact face role size"), Actual, Expected, 0.001f);
				TestEqual(TEXT("integer screen pixels per source pixel"), Actual * (96.0f / 72.0f) * RenderScale / WoTOnboardingWholePixelHeights[Index], static_cast<float>(Step), 0.001f);
				Allowed.Add(Expected);
			}
		}
		Browser->SlatePrepass(Screen.Dpi);
		int32 Count = 0;
		WoTOnboardingWholePixelWalk(*this, Browser, Allowed, Count);
		TestTrue(TEXT("grid and every detail pane were walked"), Count >= 20);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserReadingStepSpec,
	"WoT.Onboarding.BrowserReadingStep", WoTOnboardingWholePixelFlags)
bool FWoTOnboardingBrowserReadingStepSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const auto SavedResolver = Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	TArray<TPair<FName, int32>> Asked;
	Seams.ResolveMenuChrome = [&Asked]() { return TOptional<FWoTOnboardingMenuChrome>(WoTOnboardingWholePixelChrome(Asked)); };
	UWoTAngrealGuide* Fixture = WoTOnboardingWholePixelGuide();
	const auto Browser = SNew(SWoTGuideBrowser).Guide(Fixture)
		.InitialPhysicalSize(FVector2D(3024, 1964)).TextResolver(&WoTOnboardingWholePixelText);
	TestEqual(TEXT("short rows fit step five"), Browser->ReadingStepForTesting(), 5);
	TestEqual(TEXT("the fit budget leaves out the scroll box's padding around its content"),
		SWoTGuideBrowser::DetailPaneHeightForTesting(), 470.0f - FAppStyle::Get()
			.GetWidgetStyle<FScrollBoxStyle>("ScrollBox").VerticalScrolledContentPadding
			.GetTotalSpaceAlong<Orient_Vertical>(), 0.0001f);
	for (const FWoTAngrealGuideRow& Row : Fixture->Rows)
	{
		const FVector2D Measured = Browser->MeasureDetailForTesting(Row.ClassName);
		TestTrue(TEXT("every short detail fits vertically"), Measured.Y <= Browser->DetailPaneHeightForTesting());
	}
	const FName Selected = Browser->SelectedRowForTesting();
	const int32 FitCount = Browser->FitChecksForTesting();
	const FGeometry Smaller = FGeometry::MakeRoot(FVector2D(1920, 1080), FSlateLayoutTransform());
	Browser->Tick(Smaller, 10.0, 0.0f);
	TestEqual(TEXT("resize changes base immediately"), Browser->BaseStepForTesting(), 2);
	TestEqual(TEXT("resize clamps reading immediately"), Browser->ReadingStepForTesting(), 3);
	TestEqual(TEXT("resize changes scale immediately"), Browser->WindowScaleForTesting(), 1.9f, 0.0001f);
	TestEqual(TEXT("resize defers fit"), Browser->FitChecksForTesting(), FitCount);
	Browser->Tick(Smaller, 10.3, 0.3f);
	TestEqual(TEXT("settled resize reruns fit"), Browser->FitChecksForTesting(), FitCount + 1);
	TestEqual(TEXT("measurement preserves selection"), Browser->SelectedRowForTesting(), Selected);
	TestEqual(TEXT("resize updates reading face size"), Browser->FacesForTesting()->Get(
		WoTOnboardingChrome::EFace::Body, WoTOnboardingChrome::ETextRole::Reading).Size, 14.0f * 0.75f * 3.0f / 1.9f, 0.001f);
	FString LongText;
	for (int32 Index = 0; Index < 500; ++Index) LongText += TEXT("A long description. ");
	const auto LongBrowser = SNew(SWoTGuideBrowser).Guide(Fixture)
		.InitialPhysicalSize(FVector2D(3024, 1964))
		.TextResolver([LongText](const FString& Section, const FString& Property)
		{
			return Section == TEXT("Ward") && Property == TEXT("Description")
				? LongText : WoTOnboardingWholePixelText(Section, Property);
		});
	TestEqual(TEXT("unselected long entry forces lowest reading step"), LongBrowser->ReadingStepForTesting(), 4);
	TestTrue(TEXT("lowest reading is still above base"), LongBrowser->ReadingStepForTesting() > LongBrowser->BaseStepForTesting());
	TestTrue(TEXT("long entry can scroll"), LongBrowser->MeasureDetailForTesting(TEXT("Ward")).Y > LongBrowser->DetailPaneHeightForTesting());
	const auto WideBrowser = SNew(SWoTGuideBrowser).Guide(Fixture)
		.InitialPhysicalSize(FVector2D(3024, 1964))
		.TextResolver([](const FString& Section, const FString& Property)
		{
			return Property == TEXT("RarityCommonStr")
				? FString(TEXT("A rarity label too wide to fit beside the icon at any reading step"))
				: WoTOnboardingWholePixelText(Section, Property);
		});
	TestTrue(TEXT("wide facts do not exceed pane height"),
		WideBrowser->MeasureDetailForTesting(TEXT("Spark")).Y <= WideBrowser->DetailPaneHeightForTesting());
	TestEqual(TEXT("facts width alone rejects highest step"), WideBrowser->ReadingStepForTesting(), 4);
	const auto SmallBrowser = SNew(SWoTGuideBrowser).Guide(Fixture)
		.InitialPhysicalSize(FVector2D(800, 450)).TextResolver(&WoTOnboardingWholePixelText);
	TestEqual(TEXT("small screen uses strike one"), SmallBrowser->BaseStepForTesting(), 1);
	TestEqual(TEXT("small screen has one reading candidate"), SmallBrowser->ReadingStepForTesting(), 1);
	TestEqual(TEXT("small screen keeps base design size"), SmallBrowser->FacesForTesting()->Get(
		WoTOnboardingChrome::EFace::Body).Size, 14.0f * 0.75f, 0.001f);
	TestEqual(TEXT("small screen uses 1.5x reading design size"), SmallBrowser->FacesForTesting()->Get(
		WoTOnboardingChrome::EFace::Body, WoTOnboardingChrome::ETextRole::Reading).Size, 14.0f * 0.75f * 1.5f, 0.001f);
	FWoTOnboardingMenuChrome Partial = WoTOnboardingWholePixelChrome(Asked);
	const auto CompleteResolver = Partial.PixelFont;
	Partial.PixelFont = [CompleteResolver](FName FaceName, int32 Step)
	{
		return FaceName == FName(TEXT("F_Charge")) && Step == 5
			? FSlateFontInfo() : CompleteResolver(FaceName, Step);
	};
	FWoTOnboardingFaces PartialFaces(Partial);
	PartialFaces.RefreshGuide(3, 5, 2.9925f);
	TestTrue(TEXT("base charge strike remains published"), PartialFaces.IsPublished(WoTOnboardingChrome::EFace::Charge));
	TestFalse(TEXT("reading charge fallback reports its own source"), PartialFaces.IsPublished(
		WoTOnboardingChrome::EFace::Charge, WoTOnboardingChrome::ETextRole::Reading));

	return !HasAnyErrors();
}
#endif
