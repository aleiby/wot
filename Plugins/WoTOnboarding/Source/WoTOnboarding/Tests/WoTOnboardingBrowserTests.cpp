#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTGuideBrowser.h"
#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingSeams.h"

#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Geometry.h"
#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

// The guide browser (plan Tasks 5 and 8). Synthetic fixtures with invented
// names and invented prose; the real compiled rows are touched only by the
// final spec, which asserts construction and navigation, never retail prose.
// Helper names carry the WoTOnboardingBrowser prefix for the shared unity
// translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingBrowserFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

UWoTAngrealGuide* WoTOnboardingBrowserMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow CinderJet;
	CinderJet.ClassName = TEXT("CinderJet");
	CinderJet.Rarity = TEXT("Common");
	CinderJet.Elements = {FName(TEXT("Fire"))};
	CinderJet.Roles = {FName(TEXT("Combat")), FName(TEXT("Offensive"))};
	CinderJet.MaxInitialCharges = 3;
	CinderJet.MaxCharges = 8;
	CinderJet.HintText = TEXT("{counter:CinderWarden} quells it.");
	Guide->Rows.Add(CinderJet);

	FWoTAngrealGuideRow MistVeil;
	MistVeil.ClassName = TEXT("MistVeil");
	MistVeil.Rarity = TEXT("Uncommon");
	MistVeil.Elements = {FName(TEXT("Water"))};
	// Never runs down: the item-info window's unlimited case.
	MistVeil.MaxInitialCharges = 999;
	MistVeil.MaxCharges = 999;
	MistVeil.ChargeCost = 0;
	Guide->Rows.Add(MistVeil);

	FWoTAngrealGuideRow CinderWarden;
	CinderWarden.ClassName = TEXT("CinderWarden");
	CinderWarden.Rarity = TEXT("Rare");
	CinderWarden.Elements = {FName(TEXT("Fire"))};
	CinderWarden.Counters = {FName(TEXT("CinderJet"))};
	Guide->Rows.Add(CinderWarden);

	FWoTAngrealGuideRow SnareBox;
	SnareBox.ClassName = TEXT("SnareBox");
	Guide->Rows.Add(SnareBox);

	FWoTAngrealGuideRow NetBox;
	NetBox.ClassName = TEXT("NetBox");
	Guide->Rows.Add(NetBox);
	return Guide;
}

/** The invented runtime text a localized tree would supply. */
FString WoTOnboardingBrowserFixtureText(const FString& Section,
	const FString& Property)
{
	if (Section == TEXT("CinderJet"))
	{
		if (Property == TEXT("Title")) return TEXT("Cinder Jet Rod");
		if (Property == TEXT("Description")) return TEXT("An invented rod of embers.");
		if (Property == TEXT("Quote")) return TEXT("An invented flavor line.");
	}
	if (Section == TEXT("MistVeil") && Property == TEXT("Title"))
	{
		return TEXT("Mist Veil Hood");
	}
	if (Section == TEXT("CinderWarden"))
	{
		if (Property == TEXT("Title")) return TEXT("Cinder Warden Disc");
		if (Property == TEXT("Description")) return TEXT("An invented warding disc.");
	}
	if (Section == TEXT("SnareBox") && Property == TEXT("Title"))
	{
		return TEXT("Snare Crate");
	}
	if (Section == TEXT("InventoryInfoWindow"))
	{
		// Invented window strings, so the spec can tell them from the
		// built-in fallbacks.
		if (Property == TEXT("RarityCommonStr")) return TEXT("Kind: plain");
		if (Property == TEXT("ChargesStr")) return TEXT("Uses:");
		if (Property == TEXT("FireStr")) return TEXT("BLAZE");
	}
	return FString();
}

void WoTOnboardingBrowserCollectText(
	const TSharedRef<SWidget>& Widget, TArray<FString>& Out)
{
	if (!Widget->GetVisibility().IsVisible()) return;
	if (Widget->GetType() == FName(TEXT("STextBlock")))
	{
		Out.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
	}
	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			WoTOnboardingBrowserCollectText(Children->GetChildAt(Index), Out);
		}
	}
}

/** The button tagged Tag; null when absent. */
TSharedPtr<SButton> WoTOnboardingBrowserFindTagged(
	const TSharedRef<SWidget>& Widget, const FName Tag)
{
	if (Widget->GetType() == FName(TEXT("SButton")) && Widget->GetTag() == Tag)
	{
		return StaticCastSharedRef<SButton>(Widget);
	}
	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SButton> Found = WoTOnboardingBrowserFindTagged(
				Children->GetChildAt(Index), Tag))
			{
				return Found;
			}
		}
	}
	return nullptr;
}

TSharedPtr<SWidget> WoTOnboardingBrowserFindWidget(
	const TSharedRef<SWidget>& Root,
	TFunctionRef<bool(const TSharedRef<SWidget>&)> Matches)
{
	if (Matches(Root)) return Root;
	if (FChildren* ChildWidgets = Root->GetChildren())
	{
		for (int32 ChildIndex = 0; ChildIndex < ChildWidgets->Num(); ++ChildIndex)
		{
			if (TSharedPtr<SWidget> Match = WoTOnboardingBrowserFindWidget(
				ChildWidgets->GetChildAt(ChildIndex), Matches))
			{
				return Match;
			}
		}
	}
	return nullptr;
}

TSharedPtr<STextBlock> WoTOnboardingBrowserFindText(
	const TSharedRef<SWidget>& Root, const FString& ExpectedText)
{
	const TSharedPtr<SWidget> Match = WoTOnboardingBrowserFindWidget(Root,
		[&ExpectedText](const TSharedRef<SWidget>& Candidate)
		{
			return Candidate->GetType() == FName(TEXT("STextBlock"))
				&& StaticCastSharedRef<STextBlock>(Candidate)->GetText().ToString()
					== ExpectedText;
		});
	return StaticCastSharedPtr<STextBlock>(Match);
}

void WoTOnboardingBrowserArrangeTree(const TSharedRef<SWidget>& Root,
	const FGeometry& RootGeometry, TArray<FArrangedWidget>& OutWidgets)
{
	OutWidgets.Emplace(Root, RootGeometry);
	FArrangedChildren ArrangedChildren(EVisibility::Visible);
	Root->ArrangeChildren(RootGeometry, ArrangedChildren);
	for (int32 ChildIndex = 0; ChildIndex < ArrangedChildren.Num(); ++ChildIndex)
	{
		const FArrangedWidget& ChildWidget = ArrangedChildren[ChildIndex];
		WoTOnboardingBrowserArrangeTree(ChildWidget.Widget, ChildWidget.Geometry,
			OutWidgets);
	}
}

TSharedRef<SWoTGuideBrowser> WoTOnboardingBrowserMake(UWoTAngrealGuide* Guide,
	bool bResolveText, FName InitialRow = NAME_None)
{
	return SNew(SWoTGuideBrowser)
		.Guide(Guide)
		.InitialRow(InitialRow)
		.TextResolver(bResolveText
			? TFunction<FString(const FString&, const FString&)>(
				&WoTOnboardingBrowserFixtureText)
			: TFunction<FString(const FString&, const FString&)>(
				[](const FString&, const FString&) { return FString(); }));
}

FReply WoTOnboardingBrowserPress(const TSharedRef<SWoTGuideBrowser>& Browser,
	const FKey& Key)
{
	return Browser->OnKeyDown(FGeometry(),
		FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserRarityGroupsSpec,
	"WoT.Onboarding.BrowserRarityGroups", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserRarityGroupsSpec::RunTest(const FString&)
{
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), true);
	Browser->SlatePrepass(1.0f);
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);

	const int32 CommonIndex = Texts.IndexOfByKey(TEXT("COMMON"));
	const int32 UncommonIndex = Texts.IndexOfByKey(TEXT("UNCOMMON"));
	const int32 RareIndex = Texts.IndexOfByKey(TEXT("RARE"));
	const int32 TrapsIndex = Texts.IndexOfByKey(TEXT("TRAPS"));
	TestTrue(TEXT("all four group labels render"), CommonIndex != INDEX_NONE
		&& UncommonIndex != INDEX_NONE && RareIndex != INDEX_NONE
		&& TrapsIndex != INDEX_NONE);
	TestTrue(TEXT("groups keep rarity order with traps last"),
		CommonIndex < UncommonIndex && UncommonIndex < RareIndex
		&& RareIndex < TrapsIndex);

	// Rows land in their groups, in guide order within each.
	TestEqual(TEXT("the grid walks the groups in rarity order, traps last"),
		Browser->GridOrder(), TArray<FName>({FName(TEXT("CinderJet")),
			FName(TEXT("MistVeil")), FName(TEXT("CinderWarden")),
			FName(TEXT("SnareBox")), FName(TEXT("NetBox"))}));
	for (const FName ClassName : Browser->GridOrder())
	{
		TestTrue(FString::Printf(TEXT("%s has a grid icon"), *ClassName.ToString()),
			WoTOnboardingBrowserFindTagged(Browser,
				FName(*(TEXT("Grid.") + ClassName.ToString()))).IsValid());
	}
	// The grid is icons only: the titles appear in the detail pane alone.
	TestFalse(TEXT("an unselected row's title is not drawn"),
		Texts.Contains(TEXT("Mist Veil Hood")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserDeepLinkSpec,
	"WoT.Onboarding.BrowserDeepLink", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserDeepLinkSpec::RunTest(const FString&)
{
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), true);
	Browser->SlatePrepass(1.0f);
	TestEqual(TEXT("the first entry starts selected"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderJet")));
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the detail pane resolves the title"),
		Texts.Contains(TEXT("Cinder Jet Rod")));
	TestTrue(TEXT("the detail pane resolves the description"),
		Texts.Contains(TEXT("An invented rod of embers.")));
	TestTrue(TEXT("the detail pane resolves the quote"),
		Texts.Contains(TEXT("An invented flavor line.")));
	TestTrue(TEXT("the hint line resolves through the shared resolver"),
		Texts.Contains(TEXT("Cinder Warden Disc quells it.")));
	TestTrue(TEXT("the item-info window's own strings label the facts"),
		Texts.Contains(TEXT("Kind: plain")) && Texts.Contains(TEXT("Uses:"))
		&& Texts.Contains(TEXT("3")) && Texts.Contains(TEXT("/"))
		&& Texts.Contains(TEXT("8")));
	TestTrue(TEXT("the item-info window's own strings label the gems"),
		Texts.Contains(TEXT("BLAZE")) && Texts.Contains(TEXT("EARTH")));

	// The countered-by icon deep-links: clicking it selects the counter.
	const TSharedPtr<SButton> CounterIcon = WoTOnboardingBrowserFindTagged(
		Browser, FName(TEXT("CounteredBy.CinderWarden")));
	if (!TestTrue(TEXT("the countered-by icon renders as a button"),
		CounterIcon.IsValid()))
	{
		return false;
	}
	CounterIcon->SimulateClick();
	TestEqual(TEXT("the deep link selected the counter row"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderWarden")));
	Texts.Reset();
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the detail pane followed the deep link"),
		Texts.Contains(TEXT("An invented warding disc.")));
	TestTrue(TEXT("the counters row labels itself"), Texts.Contains(TEXT("COUNTERS")));

	// The counters icon links back to the countered row.
	const TSharedPtr<SButton> BackLink = WoTOnboardingBrowserFindTagged(
		Browser, FName(TEXT("Counters.CinderJet")));
	if (!TestTrue(TEXT("the counters icon renders as a button"), BackLink.IsValid()))
	{
		return false;
	}
	BackLink->SimulateClick();
	TestEqual(TEXT("the counters icon selected the countered row"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderJet")));

	// A grid icon selects its row too.
	const TSharedPtr<SButton> GridIcon = WoTOnboardingBrowserFindTagged(
		Browser, FName(TEXT("Grid.MistVeil")));
	if (!TestTrue(TEXT("the grid icon renders as a button"), GridIcon.IsValid()))
	{
		return false;
	}
	GridIcon->SimulateClick();
	TestEqual(TEXT("the grid icon selected its row"),
		Browser->SelectedRowForTesting(), FName(TEXT("MistVeil")));

	// SelectRow is the same path, addressable directly; unknown names are
	// ignored.
	Browser->SelectRow(FName(TEXT("CinderWarden")));
	TestEqual(TEXT("SelectRow lands"), Browser->SelectedRowForTesting(),
		FName(TEXT("CinderWarden")));
	Browser->SelectRow(FName(TEXT("NeverARow")));
	TestEqual(TEXT("an unknown class leaves the selection unchanged"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderWarden")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserFactsOnlySpec,
	"WoT.Onboarding.BrowserFactsOnly", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserFactsOnlySpec::RunTest(const FString&)
{
	// No retail text resolves anywhere: the detail pane shows the facts under
	// the spaced-class-name stand-in and omits the prose entirely.
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), false);
	Browser->SlatePrepass(1.0f);
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the heading falls back to the spaced class name"),
		Texts.Contains(TEXT("Cinder Jet")));
	TestTrue(TEXT("the facts render: charge label"), Texts.Contains(TEXT("Charges:")));
	TestTrue(TEXT("the facts render: starting charges"), Texts.Contains(TEXT("3")));
	TestTrue(TEXT("the facts render: charge separator"), Texts.Contains(TEXT("/")));
	TestTrue(TEXT("the facts render: charge cap"), Texts.Contains(TEXT("8")));
	TestTrue(TEXT("the facts render: rarity fallback"),
		Texts.Contains(TEXT("Common artifact")));
	for (const TCHAR* Element : {TEXT("EARTH"), TEXT("AIR"), TEXT("FIRE"),
		TEXT("WATER"), TEXT("SPIRIT")})
	{
		TestTrue(FString::Printf(TEXT("the gem bar labels %s"), Element),
			Texts.Contains(FString(Element)));
	}
	TestFalse(TEXT("roles are not drawn"), Texts.Contains(TEXT("Combat, Offensive")));
	TestTrue(TEXT("the hint still renders from spaced names"),
		Texts.Contains(TEXT("Cinder Warden quells it.")));
	TestFalse(TEXT("no description without resolved text"),
		Texts.Contains(TEXT("An invented rod of embers.")));
	TestFalse(TEXT("no quote without resolved text"),
		Texts.Contains(TEXT("An invented flavor line.")));

	// A trap: its own tag, no charges and no gem bar.
	Browser->SelectRow(FName(TEXT("SnareBox")));
	Texts.Reset();
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("a trap is tagged as one"), Texts.Contains(TEXT("Trap")));
	TestFalse(TEXT("a trap shows no gem bar"), Texts.Contains(TEXT("EARTH")));
	for (const FString& Text : Texts)
	{
		TestFalse(TEXT("a trap shows no charges"), Text.StartsWith(TEXT("Charges:")));
	}

	// An angreal that spends no charge shows the infinity mark, never its
	// placeholder envelope. Engine stand-ins draw the sign itself.
	Browser->SelectRow(FName(TEXT("MistVeil")));
	Texts.Reset();
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("unlimited charges keep their label"),
		Texts.Contains(TEXT("Charges:")));
	TestTrue(TEXT("a stand-in face draws the infinity sign"),
		Texts.Contains(TEXT("\u221E")));
	for (const FString& Text : Texts)
	{
		TestFalse(TEXT("unlimited charges show no envelope"),
			Text.Contains(TEXT("999")));
	}

	// The published charge face maps ':' to its infinity mark, as the
	// original window draws it.
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	Seams.ResolveMenuChrome = []() -> TOptional<FWoTOnboardingMenuChrome>
	{
		FWoTOnboardingMenuChrome Chrome;
		Chrome.PixelFont = [](FName, int32)
		{
			return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);
		};
		Chrome.BindWidget = [](const TSharedRef<SWidget>&) {};
		return Chrome;
	};
	const TSharedRef<SWoTGuideBrowser> Published = WoTOnboardingBrowserMake(
		WoTOnboardingBrowserMakeGuide(), false, FName(TEXT("MistVeil")));
	Published->SlatePrepass(1.0f);
	Texts.Reset();
	WoTOnboardingBrowserCollectText(Published, Texts);
	TestTrue(TEXT("the published charge face draws its ':' glyph"),
		Texts.Contains(TEXT(":")));
	TestFalse(TEXT("and not the stand-in sign"), Texts.Contains(TEXT("\u221E")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserChargeFacesSpec,
	"WoT.Onboarding.BrowserChargeFaces", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserChargeFacesSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	Seams.ResolveMenuChrome = []() -> TOptional<FWoTOnboardingMenuChrome>
	{
		FWoTOnboardingMenuChrome Chrome;
		Chrome.PixelFont = [](FName FaceName, int32)
		{
			const int32 FaceSize = FaceName == FName(TEXT("F_Charge")) ? 17
				: FaceName == FName(TEXT("F_WOTReg14")) ? 13 : 19;
			return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), FaceSize);
		};
		Chrome.BindWidget = [](const TSharedRef<SWidget>&) {};
		return Chrome;
	};
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), false);
	Browser->SlatePrepass(1.0f);
	for (const TCHAR* CountText : {TEXT("3"), TEXT("8")})
	{
		const TSharedPtr<STextBlock> CountBlock =
			WoTOnboardingBrowserFindText(Browser, CountText);
		if (TestTrue(FString::Printf(TEXT("count %s renders separately"), CountText),
			CountBlock.IsValid()))
		{
			TestEqual(TEXT("counts use the published charge face"),
				CountBlock->GetFont().Size, 17.0f);
		}
	}
	for (const TCHAR* BodyText : {TEXT("Charges:"), TEXT("/")})
	{
		const TSharedPtr<STextBlock> BodyBlock =
			WoTOnboardingBrowserFindText(Browser, BodyText);
		if (TestTrue(FString::Printf(TEXT("text %s renders separately"), BodyText),
			BodyBlock.IsValid()))
		{
			TestEqual(TEXT("the label and slash use the published body face"),
				BodyBlock->GetFont().Size, 13.0f);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserHeaderOffsetsSpec,
	"WoT.Onboarding.BrowserHeaderOffsets", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserHeaderOffsetsSpec::RunTest(const FString&)
{
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), true);
	for (const FName RowName : {FName(TEXT("CinderJet")), FName(TEXT("SnareBox"))})
	{
		Browser->SelectRow(RowName);
		const TSharedPtr<SWidget> Detail = WoTOnboardingBrowserFindWidget(Browser,
			[](const TSharedRef<SWidget>& Candidate)
			{
				return Candidate->GetTag() == FName(TEXT("Guide.Detail"));
			});
		if (!TestTrue(TEXT("the browser contains its detail pane"), Detail.IsValid()))
		{
			return false;
		}
		Detail->SlatePrepass(1.0f);
		TArray<FArrangedWidget> ArrangedWidgets;
		WoTOnboardingBrowserArrangeTree(Detail.ToSharedRef(),
			FGeometry::MakeRoot(FVector2D(422.0f, 600.0f), FSlateLayoutTransform()),
			ArrangedWidgets);
		const bool bTrapRow = RowName == FName(TEXT("SnareBox"));
		const TSharedPtr<STextBlock> TitleBlock = WoTOnboardingBrowserFindText(
			Detail.ToSharedRef(), bTrapRow ? TEXT("Snare Crate") : TEXT("Cinder Jet Rod"));
		const FArrangedWidget* TitleWidget = ArrangedWidgets.FindByPredicate(
			[&TitleBlock](const FArrangedWidget& Candidate)
			{
				return Candidate.Widget == TitleBlock;
			});
		const auto FindArrangedTag = [&ArrangedWidgets](const TCHAR* WidgetTag)
		{
			return ArrangedWidgets.FindByPredicate(
				[WidgetTag](const FArrangedWidget& Candidate)
				{
					return Candidate.Widget->GetTag() == FName(WidgetTag);
				});
		};
		const FArrangedWidget* IconWidget = FindArrangedTag(TEXT("Guide.Detail.Icon"));
		const FArrangedWidget* FactsWidget = FindArrangedTag(TEXT("Guide.Detail.Facts"));
		const FArrangedWidget* GemWidget = FindArrangedTag(TEXT("Guide.Detail.GemBar"));
		if (!TestNotNull(TEXT("the title is arranged"), TitleWidget)
			|| !TestNotNull(TEXT("the icon box is arranged"), IconWidget)
			|| !TestNotNull(TEXT("the facts line is arranged"), FactsWidget))
		{
			return false;
		}
		const double TitleTop = TitleWidget->Geometry.GetAbsolutePosition().Y;
		const double IconTop = IconWidget->Geometry.GetAbsolutePosition().Y;
		const double FactsTop = FactsWidget->Geometry.GetAbsolutePosition().Y;
		TestEqual(TEXT("the icon starts 28 units below the title"), IconTop - TitleTop, 28.0);
		TestEqual(TEXT("the icon box is 64 units high"),
			static_cast<double>(IconWidget->Geometry.GetLocalSize().Y), 64.0);
		TestEqual(TEXT("the facts start 48 units below the title"), FactsTop - TitleTop, 48.0);
		TestTrue(TEXT("the facts are beside the icon within its vertical span"),
			FactsTop >= IconTop
			&& FactsTop < IconTop + IconWidget->Geometry.GetLocalSize().Y);
		if (bTrapRow)
		{
			TestTrue(TEXT("a trap has no gem bar art"), GemWidget == nullptr);
		}
		else if (TestNotNull(TEXT("the gem bar art is arranged"), GemWidget))
		{
			TestEqual(TEXT("the gem bar art starts 96 units below the title"),
				static_cast<double>(GemWidget->Geometry.GetAbsolutePosition().Y) - TitleTop, 96.0);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserAndCardDefaultSizeSpec,
	"WoT.Onboarding.BrowserAndCardDefaultSize", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserAndCardDefaultSizeSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TFunction<TOptional<FWoTOnboardingMenuChrome>()> SavedResolver =
		Seams.ResolveMenuChrome;
	ON_SCOPE_EXIT { Seams.ResolveMenuChrome = SavedResolver; };
	Seams.ResolveMenuChrome = []() -> TOptional<FWoTOnboardingMenuChrome>
	{
		FWoTOnboardingMenuChrome Chrome;
		Chrome.PixelFont = [](FName, int32 StrikeScale)
		{
			return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 13 * StrikeScale);
		};
		Chrome.BindWidget = [](const TSharedRef<SWidget>&) {};
		return Chrome;
	};
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), true);
	FWoTPickupCardText CardText;
	CardText.Title = TEXT("Fixture pickup");
	const TSharedRef<SWoTPickupCard> Card = SNew(SWoTPickupCard).Text(CardText);
	const TSharedRef<SWidget> Windows[] = {Browser, Card};
	const FGeometry ScreenGeometry = FGeometry::MakeRoot(
		FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform());
	for (const TSharedRef<SWidget>& WindowWidget : Windows)
	{
		const TSharedPtr<SWidget> ScaleBox = WoTOnboardingBrowserFindWidget(WindowWidget,
			[](const TSharedRef<SWidget>& Candidate)
			{
				return Candidate->GetType() == FName(TEXT("SScaleBox"));
			});
		const TSharedPtr<STextBlock> Heading = WoTOnboardingBrowserFindText(WindowWidget,
			WindowWidget == Browser ? TEXT("Cinder Jet Rod") : TEXT("Fixture pickup"));
		if (!TestTrue(TEXT("the window has its scale box and heading"),
			ScaleBox.IsValid() && Heading.IsValid()))
		{
			return false;
		}
		WindowWidget->SlatePrepass(1.0f);
		FArrangedChildren BeforeTick(EVisibility::Visible);
		ScaleBox->ArrangeChildren(ScreenGeometry, BeforeTick);
		if (!TestEqual(TEXT("the default scale lays out the window"), BeforeTick.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("without an initial size the window starts at 1x"),
			BeforeTick[0].Geometry.GetAccumulatedLayoutTransform().GetScale(), 1.0f);
		TestEqual(TEXT("before geometry updates the font uses the default strike"),
			Heading->GetFont().Size, 13.0f);

		WindowWidget->Tick(ScreenGeometry, 0.0, 0.0f);
		WindowWidget->SlatePrepass(1.0f);
		FArrangedChildren AfterTick(EVisibility::Visible);
		ScaleBox->ArrangeChildren(ScreenGeometry, AfterTick);
		if (!TestEqual(TEXT("the first tick keeps the window arranged"), AfterTick.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("the first geometry update sets the window to 2x"),
			AfterTick[0].Geometry.GetAccumulatedLayoutTransform().GetScale(), 2.0f);
		TestEqual(TEXT("the first geometry update selects the 2x font strike"),
			Heading->GetFont().Size, 26.0f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserOpeningRowSpec,
	"WoT.Onboarding.BrowserOpeningRow", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserOpeningRowSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingBrowserMakeGuide();
	// The rare row first in guide order: the opening entry is still the
	// first of the first group.
	Guide->Rows.Swap(0, 2);
	const FName Jet(TEXT("CinderJet"));
	const FName Veil(TEXT("MistVeil"));
	const FName Warden(TEXT("CinderWarden"));
	const FName Unknown(TEXT("NotARow"));
	TestEqual(TEXT("no card and no hand item opens the first entry"),
		SWoTGuideBrowser::OpeningRow(*Guide, NAME_None, NAME_None), Jet);
	TestEqual(TEXT("the card's item wins"),
		SWoTGuideBrowser::OpeningRow(*Guide, Veil, Warden), Veil);
	TestEqual(TEXT("without a card, the selected hand item"),
		SWoTGuideBrowser::OpeningRow(*Guide, NAME_None, Warden), Warden);
	TestEqual(TEXT("a card item the guide lacks falls through to the hand"),
		SWoTGuideBrowser::OpeningRow(*Guide, Unknown, Warden), Warden);
	TestEqual(TEXT("a hand item the guide lacks opens the first entry"),
		SWoTGuideBrowser::OpeningRow(*Guide, NAME_None, Unknown), Jet);
	const UWoTAngrealGuide* Empty =
		NewObject<UWoTAngrealGuide>(GetTransientPackage());
	TestTrue(TEXT("an empty guide has no opening row"),
		SWoTGuideBrowser::OpeningRow(*Empty, Jet, Warden).IsNone());

	TestEqual(TEXT("the browser opens at its initial row"),
		WoTOnboardingBrowserMake(Guide, true, Warden)->SelectedRowForTesting(), Warden);
	TestEqual(TEXT("an unknown initial row opens the first entry"),
		WoTOnboardingBrowserMake(Guide, true, Unknown)->SelectedRowForTesting(), Jet);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserNavigationSpec,
	"WoT.Onboarding.BrowserNavigation", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserNavigationSpec::RunTest(const FString&)
{
	// Ten common rows (two grid lines), two rare and one trap.
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	const auto AddRow = [Guide](const FString& Name, const TCHAR* Rarity)
	{
		FWoTAngrealGuideRow Row;
		Row.ClassName = FName(*Name);
		Row.Rarity = Rarity ? FName(Rarity) : NAME_None;
		Guide->Rows.Add(Row);
	};
	for (int32 Index = 0; Index < 10; ++Index)
	{
		AddRow(FString::Printf(TEXT("Common%d"), Index), TEXT("Common"));
	}
	AddRow(TEXT("Rare0"), TEXT("Rare"));
	AddRow(TEXT("Rare1"), TEXT("Rare"));
	AddRow(TEXT("Trap0"), nullptr);

	int32 Closes = 0;
	const TSharedRef<SWoTGuideBrowser> Browser = SNew(SWoTGuideBrowser)
		.Guide(Guide)
		.CloseKeys(TArray<FKey>({EKeys::F3}))
		.TextResolver([](const FString&, const FString&) { return FString(); })
		.OnClose(FSimpleDelegate::CreateLambda([&Closes]() { ++Closes; }));
	const auto Selected = [&Browser]() { return Browser->SelectedRowForTesting(); };
	const auto Step = [this, &Browser, &Selected](const FKey& Key, const TCHAR* Expected,
		const TCHAR* What)
	{
		TestTrue(FString::Printf(TEXT("%s is handled"), What),
			WoTOnboardingBrowserPress(Browser, Key).IsEventHandled());
		TestEqual(What, Selected(), FName(Expected));
	};

	Step(EKeys::Right, TEXT("Common1"), TEXT("right walks the grid"));
	Step(EKeys::Left, TEXT("Common0"), TEXT("left walks back"));
	Step(EKeys::Left, TEXT("Common0"), TEXT("left stops at the first entry"));
	Step(EKeys::Down, TEXT("Common8"), TEXT("down keeps the column"));
	Step(EKeys::Gamepad_DPad_Down, TEXT("Rare0"), TEXT("the d-pad crosses groups"));
	Step(EKeys::Gamepad_LeftStick_Down, TEXT("Trap0"), TEXT("the stick reaches the traps"));
	Step(EKeys::Down, TEXT("Trap0"), TEXT("down stops at the last line"));
	Step(EKeys::Gamepad_DPad_Up, TEXT("Rare0"), TEXT("up climbs back"));
	Browser->SelectRow(FName(TEXT("Common7")));
	Step(EKeys::Down, TEXT("Common9"), TEXT("a short line clamps the column"));
	Step(EKeys::Gamepad_DPad_Right, TEXT("Rare0"), TEXT("right runs into the next group"));
	Step(EKeys::PageDown, TEXT("Rare0"), TEXT("page keys scroll, not select"));

	TestEqual(TEXT("no close yet"), Closes, 0);
	WoTOnboardingBrowserPress(Browser, EKeys::Escape);
	TestEqual(TEXT("escape closes"), Closes, 1);
	WoTOnboardingBrowserPress(Browser, EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("the controller's back button closes"), Closes, 2);
	WoTOnboardingBrowserPress(Browser, EKeys::F3);
	TestEqual(TEXT("the guide's own key closes"), Closes, 3);
	TestFalse(TEXT("other keys pass through"),
		WoTOnboardingBrowserPress(Browser, EKeys::W).IsEventHandled());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserCompiledRowsSpec,
	"WoT.Onboarding.BrowserCompiledRows", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserCompiledRowsSpec::RunTest(const FString&)
{
	// The compiled rows ship in every build, so this never skips.
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(UWoTAngrealGuide::Load());
	if (!TestNotNull(TEXT("the compiled rows parse"), Guide.Get()))
	{
		return false;
	}
	// The default seam resolver: whatever retail text this box resolves is
	// drawn, and whatever does not resolve falls back to facts-only. Both
	// are correct renders, so the spec asserts structure, never prose.
	const TSharedRef<SWoTGuideBrowser> Browser =
		SNew(SWoTGuideBrowser).Guide(Guide.Get());
	Browser->SlatePrepass(1.0f);
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the compiled roster renders text"), Texts.Num() > 10);
	const TArray<FName> Order = Browser->GridOrder();
	TestEqual(TEXT("every row has one grid entry"), Order.Num(), Guide->Rows.Num());
	for (const FWoTAngrealGuideRow& Row : Guide->Rows)
	{
		TestTrue(TEXT("every row is on the grid"), Order.Contains(Row.ClassName));
		Browser->SelectRow(Row.ClassName);
		TestEqual(TEXT("every row deep-links"),
			Browser->SelectedRowForTesting(), Row.ClassName);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
