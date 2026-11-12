#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTGuideBrowser.h"
#include "WoTAngrealGuide.h"

#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

// The guide browser (plan Task 5). Synthetic fixtures with invented names and
// invented prose; the real imported asset is touched only by the final spec,
// which asserts construction and navigation, never retail prose. Helper names
// carry the WoTOnboardingBrowser prefix for the shared unity translation unit.

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
	CinderJet.HintText = TEXT("{title} - {counter} quells it.");
	Guide->Rows.Add(CinderJet);

	FWoTAngrealGuideRow MistVeil;
	MistVeil.ClassName = TEXT("MistVeil");
	MistVeil.Rarity = TEXT("Uncommon");
	MistVeil.Elements = {FName(TEXT("Water"))};
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

/** The first button whose own text is exactly Text; null when absent. */
TSharedPtr<SButton> WoTOnboardingBrowserFindButton(
	const TSharedRef<SWidget>& Widget, const TCHAR* Text)
{
	if (Widget->GetType() == FName(TEXT("SButton")))
	{
		TArray<FString> Texts;
		WoTOnboardingBrowserCollectText(Widget, Texts);
		if (Texts.Contains(Text))
		{
			return StaticCastSharedRef<SButton>(Widget);
		}
	}
	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SButton> Found = WoTOnboardingBrowserFindButton(
				Children->GetChildAt(Index), Text))
			{
				return Found;
			}
		}
	}
	return nullptr;
}

TSharedRef<SWoTGuideBrowser> WoTOnboardingBrowserMake(UWoTAngrealGuide* Guide,
	bool bResolveText)
{
	return SNew(SWoTGuideBrowser)
		.Guide(Guide)
		.TextResolver(bResolveText
			? TFunction<FString(const FString&, const FString&)>(
				&WoTOnboardingBrowserFixtureText)
			: TFunction<FString(const FString&, const FString&)>(
				[](const FString&, const FString&) { return FString(); }));
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
	TestTrue(TEXT("all four group headers render"), CommonIndex != INDEX_NONE
		&& UncommonIndex != INDEX_NONE && RareIndex != INDEX_NONE
		&& TrapsIndex != INDEX_NONE);
	TestTrue(TEXT("groups keep rarity order with traps last"),
		CommonIndex < UncommonIndex && UncommonIndex < RareIndex
		&& RareIndex < TrapsIndex);

	// Rows land in their groups: resolved titles between the right headers,
	// and the no-rarity rows after the trap header (NetBox resolves no title,
	// so its list label is the spaced class name).
	const int32 JetIndex = Texts.IndexOfByKey(TEXT("Cinder Jet Rod"));
	const int32 VeilIndex = Texts.IndexOfByKey(TEXT("Mist Veil Hood"));
	const int32 SnareIndex = Texts.IndexOfByKey(TEXT("Snare Crate"));
	const int32 NetIndex = Texts.IndexOfByKey(TEXT("Net Box"));
	TestTrue(TEXT("the common row lists under COMMON"),
		JetIndex > CommonIndex && JetIndex < UncommonIndex);
	TestTrue(TEXT("the uncommon row lists under UNCOMMON"),
		VeilIndex > UncommonIndex && VeilIndex < RareIndex);
	TestTrue(TEXT("trap rows list under TRAPS"),
		SnareIndex > TrapsIndex && NetIndex > TrapsIndex);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserDeepLinkSpec,
	"WoT.Onboarding.BrowserDeepLink", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserDeepLinkSpec::RunTest(const FString&)
{
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), true);
	Browser->SlatePrepass(1.0f);
	TestEqual(TEXT("the first row starts selected"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderJet")));
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the detail pane resolves the description"),
		Texts.Contains(TEXT("An invented rod of embers.")));
	TestTrue(TEXT("the detail pane resolves the quote"),
		Texts.Contains(TEXT("An invented flavor line.")));
	TestTrue(TEXT("the hint line resolves through the shared resolver"),
		Texts.Contains(TEXT("Cinder Jet Rod - Cinder Warden Disc quells it.")));

	// The countered-by row deep-links: clicking it selects the counter.
	const TSharedPtr<SButton> CounterRow = WoTOnboardingBrowserFindButton(
		Browser, TEXT("Cinder Warden Disc"));
	if (!TestTrue(TEXT("the countered-by row renders as a button"),
		CounterRow.IsValid()))
	{
		return false;
	}
	CounterRow->SimulateClick();
	TestEqual(TEXT("the deep link selected the counter row"),
		Browser->SelectedRowForTesting(), FName(TEXT("CinderWarden")));
	Texts.Reset();
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the detail pane followed the deep link"),
		Texts.Contains(TEXT("An invented warding disc.")));
	TestTrue(TEXT("the counters list links back to the countered row"),
		Texts.Contains(TEXT("COUNTERS")));

	// SelectRow is the same path, addressable directly; unknown names are
	// ignored.
	Browser->SelectRow(FName(TEXT("MistVeil")));
	TestEqual(TEXT("SelectRow lands"), Browser->SelectedRowForTesting(),
		FName(TEXT("MistVeil")));
	Browser->SelectRow(FName(TEXT("NeverARow")));
	TestEqual(TEXT("an unknown class leaves the selection unchanged"),
		Browser->SelectedRowForTesting(), FName(TEXT("MistVeil")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserFactsOnlySpec,
	"WoT.Onboarding.BrowserFactsOnly", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserFactsOnlySpec::RunTest(const FString&)
{
	// No retail text resolves anywhere: the detail pane shows the facts under
	// the spaced-class-name stand-in and omits the prose panes entirely.
	const TSharedRef<SWoTGuideBrowser> Browser =
		WoTOnboardingBrowserMake(WoTOnboardingBrowserMakeGuide(), false);
	Browser->SlatePrepass(1.0f);
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the heading falls back to the spaced class name"),
		Texts.Contains(TEXT("Cinder Jet")));
	TestTrue(TEXT("the facts render: charges"),
		Texts.Contains(TEXT("Charges:  3 / 8")));
	TestTrue(TEXT("the facts render: rarity fallback"),
		Texts.Contains(TEXT("Common artifact")));
	TestTrue(TEXT("the facts render: roles"),
		Texts.Contains(TEXT("Combat, Offensive")));
	TestTrue(TEXT("the hint still renders from spaced names"),
		Texts.Contains(TEXT("Cinder Jet - Cinder Warden quells it.")));
	TestFalse(TEXT("no description pane without resolved text"),
		Texts.Contains(TEXT("An invented rod of embers.")));
	TestFalse(TEXT("no quote pane without resolved text"),
		Texts.Contains(TEXT("An invented flavor line.")));
	for (const FString& Text : Texts)
	{
		TestFalse(TEXT("no empty prose block renders"), Text.Contains(
			TEXT("lorem"), ESearchCase::IgnoreCase));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserRealAssetSpec,
	"WoT.Onboarding.BrowserRealAsset", WoTOnboardingBrowserFlags)

bool FWoTOnboardingBrowserRealAssetSpec::RunTest(const FString&)
{
	// Skip semantics mirror GuideAssetLoad: a tree whose import pipeline has
	// not run carries no asset, and that is a pass.
	UWoTAngrealGuide* Guide = UWoTAngrealGuide::Load();
	if (!Guide)
	{
		AddInfo(FString::Printf(
			TEXT("%s is not imported in this tree; skipping the browser pins."),
			UWoTAngrealGuide::AssetPath()));
		return true;
	}
	// The default seam resolver: whatever retail text this box resolves is
	// drawn, and whatever does not resolve falls back to facts-only — both
	// are correct renders, so the spec asserts structure, never prose.
	const TSharedRef<SWoTGuideBrowser> Browser =
		SNew(SWoTGuideBrowser).Guide(Guide);
	Browser->SlatePrepass(1.0f);
	TArray<FString> Texts;
	WoTOnboardingBrowserCollectText(Browser, Texts);
	TestTrue(TEXT("the imported roster renders a list"), Texts.Num() > 10);
	for (const FWoTAngrealGuideRow& Row : Guide->Rows)
	{
		Browser->SelectRow(Row.ClassName);
		TestEqual(TEXT("every row deep-links"),
			Browser->SelectedRowForTesting(), Row.ClassName);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
