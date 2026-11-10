#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingModule.h"
#include "WoTOnboardingPickupCards.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Text/STextBlock.h"

// The first-pickup card (plan Task 6). Synthetic fixtures with invented names
// and invented prose. Specs that drive the live settings restore every value
// they touch, the seen list included, through the public API. Helper names
// carry the WoTOnboardingPickup prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingPickupFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

UWoTAngrealGuide* WoTOnboardingPickupMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	for (const TCHAR* Name : {TEXT("EmberLob"), TEXT("FrostPin"), TEXT("GustWard")})
	{
		FWoTAngrealGuideRow Row;
		Row.ClassName = FName(Name);
		Row.Rarity = FName(TEXT("Common"));
		Guide->Rows.Add(Row);
	}
	return Guide;
}

/** Pins the live toggles and seen list; restores them through the setters. */
struct FWoTOnboardingPickupSettingsSnapshot
{
	bool bMaster;
	bool bCards;
	TArray<FName> Seen;

	FWoTOnboardingPickupSettingsSnapshot()
	{
		const UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		bMaster = Live.bEnableOnboardingAids;
		bCards = Live.bShowFirstPickupCards;
		Seen = Live.SeenPickupCards;
	}

	~FWoTOnboardingPickupSettingsSnapshot()
	{
		UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		Live.SetEnableOnboardingAids(bMaster);
		Live.SetShowFirstPickupCards(bCards);
		Live.SetSeenPickupCards(Seen);
	}
};

void WoTOnboardingPickupCollectText(
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
			WoTOnboardingPickupCollectText(Children->GetChildAt(Index), Out);
		}
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingPickupCardVerdictsSpec,
	"WoT.Onboarding.PickupCardVerdicts", WoTOnboardingPickupFlags)

bool FWoTOnboardingPickupCardVerdictsSpec::RunTest(const FString&)
{
	// A detached settings object, written field by field and never saved, so
	// the verdict matrix touches no persisted state.
	UWoTOnboardingSettings* Settings =
		NewObject<UWoTOnboardingSettings>(GetTransientPackage());
	Settings->bEnableOnboardingAids = true;
	Settings->bShowFirstPickupCards = true;
	Settings->SeenPickupCards.Reset();
	const UWoTAngrealGuide* Guide = WoTOnboardingPickupMakeGuide();
	const FName Ember(TEXT("EmberLob"));
	using WoTOnboardingPickupCards::Decide;

	TestTrue(TEXT("an unseen guide class on a free screen shows"),
		Decide(*Settings, Guide, Ember, false) == EWoTPickupCardVerdict::Show);
	TestTrue(TEXT("a busy screen waits"),
		Decide(*Settings, Guide, Ember, true) == EWoTPickupCardVerdict::ScreenBusy);
	TestTrue(TEXT("a class the guide lacks never shows"),
		Decide(*Settings, Guide, FName(TEXT("NotARow")), false)
			== EWoTPickupCardVerdict::NotInGuide);
	TestTrue(TEXT("None never shows"),
		Decide(*Settings, Guide, NAME_None, false)
			== EWoTPickupCardVerdict::NotInGuide);
	TestTrue(TEXT("no guide never shows"),
		Decide(*Settings, nullptr, Ember, false)
			== EWoTPickupCardVerdict::NotInGuide);

	Settings->SeenPickupCards.Add(Ember);
	TestTrue(TEXT("a seen class never shows again"),
		Decide(*Settings, Guide, Ember, false) == EWoTPickupCardVerdict::AlreadySeen);
	TestTrue(TEXT("seen outranks a busy screen (nothing waits for it)"),
		Decide(*Settings, Guide, Ember, true) == EWoTPickupCardVerdict::AlreadySeen);
	TestTrue(TEXT("other classes are unaffected"),
		Decide(*Settings, Guide, FName(TEXT("FrostPin")), false)
			== EWoTPickupCardVerdict::Show);

	Settings->bShowFirstPickupCards = false;
	TestTrue(TEXT("the aid off never shows"),
		Decide(*Settings, Guide, FName(TEXT("FrostPin")), false)
			== EWoTPickupCardVerdict::CardsOff);
	Settings->bShowFirstPickupCards = true;
	Settings->bEnableOnboardingAids = false;
	TestTrue(TEXT("the master off never shows"),
		Decide(*Settings, Guide, FName(TEXT("FrostPin")), false)
			== EWoTPickupCardVerdict::AidsOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingPickupCardTextSpec,
	"WoT.Onboarding.PickupCardText", WoTOnboardingPickupFlags)

bool FWoTOnboardingPickupCardTextSpec::RunTest(const FString&)
{
	using WoTOnboardingPickupCards::FirstSentence;
	TestEqual(TEXT("two sentences"), FirstSentence(TEXT("One. Two.")),
		FString(TEXT("One.")));
	TestEqual(TEXT("no terminator keeps the whole text"),
		FirstSentence(TEXT("No terminator here")),
		FString(TEXT("No terminator here")));
	TestEqual(TEXT("trimmed, exclamation"),
		FirstSentence(TEXT("  Padded!  Next")), FString(TEXT("Padded!")));
	TestEqual(TEXT("question"), FirstSentence(TEXT("Really? Yes.")),
		FString(TEXT("Really?")));
	TestEqual(TEXT("a decimal point is not a sentence end"),
		FirstSentence(TEXT("Lasts 2.5 seconds. Then fades.")),
		FString(TEXT("Lasts 2.5 seconds.")));
	TestEqual(TEXT("an ellipsis ends at its last dot"),
		FirstSentence(TEXT("Wait... then go. More")), FString(TEXT("Wait...")));
	TestEqual(TEXT("a terminator at the very end"),
		FirstSentence(TEXT("Ends here.")), FString(TEXT("Ends here.")));
	TestEqual(TEXT("empty stays empty"), FirstSentence(FString()), FString());

	FWoTAngrealGuideRow Row;
	Row.ClassName = FName(TEXT("AngrealInvEmberLob"));
	const FWoTPickupCardText Text = WoTOnboardingPickupCards::ComposeText(Row,
		[](const FString& Section, const FString& Property) -> FString
		{
			if (Section == TEXT("AngrealInvEmberLob"))
			{
				if (Property == TEXT("Title")) return TEXT("  Ember Lob Rod ");
				if (Property == TEXT("Description"))
				{
					return TEXT("Lobs an invented ember. It arcs for 2.5 seconds.");
				}
			}
			return FString();
		},
		TEXT("F3"));
	TestEqual(TEXT("title resolves, trimmed"), Text.Title,
		FString(TEXT("Ember Lob Rod")));
	TestEqual(TEXT("summary is the first sentence"), Text.Summary,
		FString(TEXT("Lobs an invented ember.")));
	TestEqual(TEXT("key hint names the live key"), Text.KeyHint,
		FString(TEXT("F3  Field Guide")));
	TestEqual(TEXT("the card carries its item for the icon"), Text.ItemClass,
		FName(TEXT("AngrealInvEmberLob")));
	TestTrue(TEXT("a pickup card is not a recap"),
		Text.Kicker.IsEmpty() && Text.SummaryKicker.IsEmpty()
		&& Text.CounterClass.IsNone());

	const FWoTPickupCardText Bare = WoTOnboardingPickupCards::ComposeText(Row,
		[](const FString&, const FString&) { return FString(); }, FString());
	TestEqual(TEXT("unresolved title falls back to the spaced class name"),
		Bare.Title, FString(TEXT("Ember Lob")));
	TestTrue(TEXT("unresolved description leaves no summary"),
		Bare.Summary.IsEmpty());
	TestTrue(TEXT("an unbound guide key leaves no hint"), Bare.KeyHint.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingPickupCardOncePerClassSpec,
	"WoT.Onboarding.PickupCardOncePerClass", WoTOnboardingPickupFlags)

bool FWoTOnboardingPickupCardOncePerClassSpec::RunTest(const FString&)
{
	FWoTOnboardingModule& Module =
		FModuleManager::GetModuleChecked<FWoTOnboardingModule>(
			TEXT("WoTOnboarding"));
	FWoTOnboardingPickupSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	Live.SetEnableOnboardingAids(true);
	Live.SetShowFirstPickupCards(true);
	Live.SetSeenPickupCards({});

	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(WoTOnboardingPickupMakeGuide());
	// No retail window open, whatever an earlier spec's world left installed.
	TFunction<bool()> SavedWindowQuery = MoveTemp(Seams.IsClientWindowOpen);
	Seams.IsClientWindowOpen = nullptr;
	Module.SetPickupCardGuideForTesting(Guide.Get());
	// Declared after the snapshot, so this runs first: the handler goes inert
	// again before the settings restore.
	ON_SCOPE_EXIT
	{
		Module.SetPickupCardGuideForTesting(nullptr);
		FWoTOnboardingSeams::Get().IsClientWindowOpen = MoveTemp(SavedWindowQuery);
	};

	const int32 Before = Module.GetPickupCardsShownForTesting();
	auto Shown = [&Module, Before]()
	{
		return Module.GetPickupCardsShownForTesting() - Before;
	};
	const FName Ember(TEXT("EmberLob"));
	const FName Frost(TEXT("FrostPin"));
	const FName Gust(TEXT("GustWard"));

	Seams.ClientItemAdded.Broadcast(Ember);
	TestEqual(TEXT("the first add of a class shows its card"), Shown(), 1);
	TestTrue(TEXT("and records the class as seen"), Live.HasSeenPickupCard(Ember));
	Seams.ClientItemAdded.Broadcast(Ember);
	TestEqual(TEXT("a later add of the same class shows nothing"), Shown(), 1);
	Seams.ClientItemAdded.Broadcast(Frost);
	TestEqual(TEXT("another class gets its own card"), Shown(), 2);
	Seams.ClientItemAdded.Broadcast(FName(TEXT("NotARow")));
	TestEqual(TEXT("a class the guide lacks shows nothing"), Shown(), 2);
	TestFalse(TEXT("and stays unrecorded"),
		Live.HasSeenPickupCard(FName(TEXT("NotARow"))));

	Seams.IsClientWindowOpen = []() { return true; };
	Seams.ClientItemAdded.Broadcast(Gust);
	TestEqual(TEXT("a retail window holding the screen shows nothing"), Shown(), 2);
	TestFalse(TEXT("and leaves the class unseen for a later add"),
		Live.HasSeenPickupCard(Gust));
	Seams.IsClientWindowOpen = nullptr;

	Live.SetShowFirstPickupCards(false);
	Seams.ClientItemAdded.Broadcast(Gust);
	TestEqual(TEXT("the aid off shows nothing"), Shown(), 2);
	Live.SetShowFirstPickupCards(true);
	Live.SetEnableOnboardingAids(false);
	Seams.ClientItemAdded.Broadcast(Gust);
	TestEqual(TEXT("the master off shows nothing"), Shown(), 2);
	TestFalse(TEXT("and records nothing"), Live.HasSeenPickupCard(Gust));
	Live.SetEnableOnboardingAids(true);

	Seams.ClientItemAdded.Broadcast(Gust);
	TestEqual(TEXT("the class that waited shows on its next first add"),
		Shown(), 3);

	// Without a spec's guide the handler is inert under automation, so no
	// other suite's inventory traffic writes the persisted seen list.
	Module.SetPickupCardGuideForTesting(nullptr);
	Live.SetSeenPickupCards({});
	Seams.ClientItemAdded.Broadcast(Ember);
	TestEqual(TEXT("inert under automation without a spec guide"), Shown(), 3);
	TestFalse(TEXT("and writes nothing"), Live.HasSeenPickupCard(Ember));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingPickupCardSeenPersistsSpec,
	"WoT.Onboarding.PickupCardSeenPersists", WoTOnboardingPickupFlags)

bool FWoTOnboardingPickupCardSeenPersistsSpec::RunTest(const FString&)
{
	FWoTOnboardingPickupSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	const FName Ember(TEXT("EmberLob"));
	const FName Frost(TEXT("FrostPin"));
	Live.SetSeenPickupCards({});

	TestTrue(TEXT("a first mark records"), Live.MarkPickupCardSeen(Ember));
	TestFalse(TEXT("a repeat mark is a no-op"), Live.MarkPickupCardSeen(Ember));
	TestFalse(TEXT("None is never recorded"), Live.MarkPickupCardSeen(NAME_None));
	TestTrue(TEXT("a second class records"), Live.MarkPickupCardSeen(Frost));

	// Replace the in-memory list without saving, then reload from the config
	// store: what the marks persisted comes back exactly.
	Live.SeenPickupCards = {FName(TEXT("UnsavedStale"))};
	Live.LoadConfig();
	TestEqual(TEXT("the persisted list reloads"), Live.SeenPickupCards,
		TArray<FName>({Ember, Frost}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingPickupCardWidgetSpec,
	"WoT.Onboarding.PickupCardWidget", WoTOnboardingPickupFlags)

bool FWoTOnboardingPickupCardWidgetSpec::RunTest(const FString&)
{
	FWoTPickupCardText Text;
	Text.ItemClass = FName(TEXT("EmberLob"));
	Text.Title = TEXT("Ember Lob Rod");
	Text.Summary = TEXT("Lobs an invented ember.");
	Text.KeyHint = TEXT("F3  Field Guide");
	const TSharedRef<SWoTPickupCard> Card = SNew(SWoTPickupCard).Text(Text);
	TestFalse(TEXT("a pickup card uses the card layout"), Card->IsRecap());
	TestFalse(TEXT("the card never takes focus"), Card->SupportsKeyboardFocus());
	TestFalse(TEXT("the card never hit-tests"),
		Card->GetVisibility().IsHitTestVisible());
	TArray<FString> Texts;
	WoTOnboardingPickupCollectText(Card, Texts);
	TestEqual(TEXT("title, summary and key hint render, in order"), Texts,
		TArray<FString>({Text.Title, Text.Summary, Text.KeyHint}));

	FWoTPickupCardText Bare;
	Bare.Title = TEXT("Ember Lob");
	TArray<FString> BareTexts;
	WoTOnboardingPickupCollectText(SNew(SWoTPickupCard).Text(Bare), BareTexts);
	TestEqual(TEXT("missing parts render nothing, not placeholders"), BareTexts,
		TArray<FString>({Bare.Title}));

	TestEqual(TEXT("invisible at the start"), SWoTPickupCard::FadeAlpha(0.0f, 4.0f),
		0.0f);
	TestEqual(TEXT("half way through the fade in"),
		SWoTPickupCard::FadeAlpha(0.1f, 4.0f), 0.5f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("fully shown mid-life"), SWoTPickupCard::FadeAlpha(2.0f, 4.0f),
		1.0f);
	TestEqual(TEXT("half way through the fade out"),
		SWoTPickupCard::FadeAlpha(3.7f, 4.0f), 0.5f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("gone at the end"), SWoTPickupCard::FadeAlpha(4.0f, 4.0f), 0.0f);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
