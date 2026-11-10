#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingDeathRecap.h"
#include "WoTOnboardingModule.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Text/STextBlock.h"

// The death recap (plan Task 7). Synthetic fixtures with invented names and
// invented prose. Specs that drive the live settings restore every value
// they touch through the public API. Helper names carry the
// WoTOnboardingRecap prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingRecapFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTAngrealGuideRow WoTOnboardingRecapRow(const TCHAR* ClassName,
	const TCHAR* Element, const TCHAR* HintText)
{
	FWoTAngrealGuideRow Row;
	Row.ClassName = FName(ClassName);
	Row.Rarity = FName(TEXT("Common"));
	Row.Elements = {FName(Element)};
	Row.HintText = HintText;
	return Row;
}

/** EmberLob's line names its counter (CinderWard counters it alone);
    VoidLance's line needs none; FrostPin's line names a counter that does
    not counter it; GustWard carries no line at all. */
UWoTAngrealGuide* WoTOnboardingRecapMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	Guide->Rows.Add(WoTOnboardingRecapRow(TEXT("EmberLob"), TEXT("Fire"),
		TEXT("{counter:CinderWard} quenches it.")));
	FWoTAngrealGuideRow Ward =
		WoTOnboardingRecapRow(TEXT("CinderWard"), TEXT("Fire"), TEXT(""));
	Ward.Counters = {FName(TEXT("EmberLob"))};
	Guide->Rows.Add(Ward);
	Guide->Rows.Add(WoTOnboardingRecapRow(TEXT("VoidLance"), TEXT("Spirit"),
		TEXT("{title} cannot be turned; strike first.")));
	Guide->Rows.Add(WoTOnboardingRecapRow(TEXT("FrostPin"), TEXT("Water"),
		TEXT("{counter:CinderWard} melts it.")));
	Guide->Rows.Add(WoTOnboardingRecapRow(TEXT("GustWard"), TEXT("Air"), TEXT("")));
	return Guide;
}

/** The invented runtime titles; VoidLance resolves none. */
FString WoTOnboardingRecapTitle(const FWoTAngrealGuideRow& Row)
{
	if (Row.ClassName == FName(TEXT("EmberLob")))
	{
		return TEXT("Ember Lob");
	}
	if (Row.ClassName == FName(TEXT("CinderWard")))
	{
		return TEXT("Cinder Ward");
	}
	return FString();
}

/** Pins the live toggles; restores them through the setters. */
struct FWoTOnboardingRecapSettingsSnapshot
{
	bool bMaster;
	bool bCards;
	bool bRecaps;
	TArray<FName> Seen;

	FWoTOnboardingRecapSettingsSnapshot()
	{
		const UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		bMaster = Live.bEnableOnboardingAids;
		bCards = Live.bShowFirstPickupCards;
		bRecaps = Live.bShowDeathCounterHints;
		Seen = Live.SeenPickupCards;
	}

	~FWoTOnboardingRecapSettingsSnapshot()
	{
		UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		Live.SetEnableOnboardingAids(bMaster);
		Live.SetShowFirstPickupCards(bCards);
		Live.SetShowDeathCounterHints(bRecaps);
		Live.SetSeenPickupCards(Seen);
	}
};

void WoTOnboardingRecapCollectText(
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
			WoTOnboardingRecapCollectText(Children->GetChildAt(Index), Out);
		}
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingDeathRecapVerdictsSpec,
	"WoT.Onboarding.DeathRecapVerdicts", WoTOnboardingRecapFlags)

bool FWoTOnboardingDeathRecapVerdictsSpec::RunTest(const FString&)
{
	// A detached settings object, written field by field and never saved.
	UWoTOnboardingSettings* Settings =
		NewObject<UWoTOnboardingSettings>(GetTransientPackage());
	Settings->bEnableOnboardingAids = true;
	Settings->bShowDeathCounterHints = true;
	const UWoTAngrealGuide* Guide = WoTOnboardingRecapMakeGuide();
	const FName Ember(TEXT("EmberLob"));
	using WoTOnboardingDeathRecap::Decide;

	TestTrue(TEXT("a guide item with a counter shows"),
		Decide(*Settings, Guide, Ember, false) == EWoTDeathRecapVerdict::Show);
	TestTrue(TEXT("a line that needs no counter shows"),
		Decide(*Settings, Guide, FName(TEXT("VoidLance")), false)
			== EWoTDeathRecapVerdict::Show);
	TestTrue(TEXT("a busy screen shows nothing"),
		Decide(*Settings, Guide, Ember, true) == EWoTDeathRecapVerdict::ScreenBusy);
	TestTrue(TEXT("a line wanting a counter nothing provides shows nothing"),
		Decide(*Settings, Guide, FName(TEXT("FrostPin")), false)
			== EWoTDeathRecapVerdict::NoHint);
	TestTrue(TEXT("a row with no line shows nothing"),
		Decide(*Settings, Guide, FName(TEXT("GustWard")), false)
			== EWoTDeathRecapVerdict::NoHint);
	TestTrue(TEXT("an item the guide lacks shows nothing"),
		Decide(*Settings, Guide, FName(TEXT("NotARow")), false)
			== EWoTDeathRecapVerdict::NotInGuide);
	TestTrue(TEXT("None shows nothing"),
		Decide(*Settings, Guide, NAME_None, false)
			== EWoTDeathRecapVerdict::NotInGuide);
	TestTrue(TEXT("no guide shows nothing"),
		Decide(*Settings, nullptr, Ember, false)
			== EWoTDeathRecapVerdict::NotInGuide);

	Settings->bShowFirstPickupCards = false;
	TestTrue(TEXT("the pickup-card toggle does not govern recaps"),
		Decide(*Settings, Guide, Ember, false) == EWoTDeathRecapVerdict::Show);
	Settings->bShowDeathCounterHints = false;
	TestTrue(TEXT("the recap aid off shows nothing"),
		Decide(*Settings, Guide, Ember, false) == EWoTDeathRecapVerdict::RecapsOff);
	Settings->bShowDeathCounterHints = true;
	Settings->bEnableOnboardingAids = false;
	TestTrue(TEXT("the master off shows nothing"),
		Decide(*Settings, Guide, Ember, false) == EWoTDeathRecapVerdict::AidsOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingDeathRecapTextSpec,
	"WoT.Onboarding.DeathRecapText", WoTOnboardingRecapFlags)

bool FWoTOnboardingDeathRecapTextSpec::RunTest(const FString&)
{
	const UWoTAngrealGuide* Guide = WoTOnboardingRecapMakeGuide();
	const auto Titles = [](const FWoTAngrealGuideRow& Row)
	{
		return WoTOnboardingRecapTitle(Row);
	};
	using WoTOnboardingDeathRecap::ComposeText;

	const FWoTPickupCardText Ember = ComposeText(*Guide,
		*Guide->FindByClassName(FName(TEXT("EmberLob"))), Titles, TEXT("F3"));
	TestEqual(TEXT("the kicker leads"), Ember.Kicker, FString(TEXT("KILLED BY")));
	TestEqual(TEXT("the title names the killing item"), Ember.Title,
		FString(TEXT("Ember Lob")));
	TestEqual(TEXT("the killing item's icon"), Ember.ItemClass,
		FName(TEXT("EmberLob")));
	TestEqual(TEXT("the counter's kicker"), Ember.SummaryKicker,
		FString(TEXT("COUNTER")));
	TestEqual(TEXT("the counter the line names"), Ember.CounterClass,
		FName(TEXT("CinderWard")));
	TestEqual(TEXT("the line names its counter"), Ember.Summary,
		FString(TEXT("Cinder Ward quenches it.")));
	TestEqual(TEXT("the key line reads the guide binding"), Ember.KeyHint,
		FString(TEXT("F3  Field Guide")));

	const FWoTPickupCardText Void = ComposeText(*Guide,
		*Guide->FindByClassName(FName(TEXT("VoidLance"))), Titles, FString());
	TestEqual(TEXT("an unresolved title falls back to the spaced class name"),
		Void.Title, FString(TEXT("Void Lance")));
	TestEqual(TEXT("in the line as well"), Void.Summary,
		FString(TEXT("Void Lance cannot be turned; strike first.")));
	TestTrue(TEXT("a line that names no counter leaves no counter icon"),
		Void.CounterClass.IsNone());
	TestTrue(TEXT("an unbound guide key leaves no key line"), Void.KeyHint.IsEmpty());

	const FWoTPickupCardText Frost = ComposeText(*Guide,
		*Guide->FindByClassName(FName(TEXT("FrostPin"))), Titles, TEXT("F3"));
	TestTrue(TEXT("a row with no line composes nothing"),
		Frost.Kicker.IsEmpty() && Frost.Title.IsEmpty() && Frost.Summary.IsEmpty()
		&& Frost.KeyHint.IsEmpty() && Frost.ItemClass.IsNone());

	// The recap draws in the pickup card's widget, in its recap layout: the
	// killer under its kicker, then the counter under its own.
	const TSharedRef<SWoTPickupCard> Recap =
		SNew(SWoTPickupCard).Text(Ember).Lifetime(6.0f);
	TestTrue(TEXT("a kicker selects the recap layout"), Recap->IsRecap());
	TArray<FString> Lines;
	WoTOnboardingRecapCollectText(Recap, Lines);
	TestEqual(TEXT("the recap draws its five lines in order"), Lines,
		TArray<FString>({Ember.Kicker, Ember.Title, Ember.SummaryKicker,
			Ember.Summary, Ember.KeyHint}));

	Lines.Reset();
	WoTOnboardingRecapCollectText(SNew(SWoTPickupCard).Text(Void).Lifetime(6.0f),
		Lines);
	TestEqual(TEXT("no key line and no counter icon still draws the rest"), Lines,
		TArray<FString>({Void.Kicker, Void.Title, Void.SummaryKicker, Void.Summary}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingDeathRecapHandlerSpec,
	"WoT.Onboarding.DeathRecapHandler", WoTOnboardingRecapFlags)

bool FWoTOnboardingDeathRecapHandlerSpec::RunTest(const FString&)
{
	FWoTOnboardingModule& Module =
		FModuleManager::GetModuleChecked<FWoTOnboardingModule>(
			TEXT("WoTOnboarding"));
	FWoTOnboardingRecapSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	Live.SetEnableOnboardingAids(true);
	Live.SetShowDeathCounterHints(true);
	const TArray<FName> SeenBefore = Live.SeenPickupCards;

	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(WoTOnboardingRecapMakeGuide());
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

	const int32 Before = Module.GetDeathRecapsShownForTesting();
	auto Shown = [&Module, Before]()
	{
		return Module.GetDeathRecapsShownForTesting() - Before;
	};
	const FName Ember(TEXT("EmberLob"));

	TestTrue(TEXT("the recap listens while the aids are on"),
		Seams.ClientKilledBy.IsBoundToObject(&Module));
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("a reported guide item shows a recap"), Shown(), 1);
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("every death recaps, not just the first"), Shown(), 2);
	TestEqual(TEXT("recaps leave the pickup cards' seen list alone"),
		Live.SeenPickupCards, SeenBefore);
	Seams.ClientKilledBy.Broadcast(FName(TEXT("NotARow")));
	TestEqual(TEXT("an item the guide lacks shows nothing"), Shown(), 2);
	Seams.ClientKilledBy.Broadcast(FName(TEXT("GustWard")));
	TestEqual(TEXT("an item with no line shows nothing"), Shown(), 2);

	Seams.IsClientWindowOpen = []() { return true; };
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("a retail window holding the screen shows nothing"), Shown(), 2);
	Seams.IsClientWindowOpen = nullptr;

	Live.SetShowDeathCounterHints(false);
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("the recap aid off shows nothing"), Shown(), 2);
	Live.SetShowDeathCounterHints(true);
	Live.SetEnableOnboardingAids(false);
	TestFalse(TEXT("the master off unbinds the recap"),
		Seams.ClientKilledBy.IsBoundToObject(&Module));
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("the master off shows nothing"), Shown(), 2);
	Live.SetEnableOnboardingAids(true);

	// Without a spec's guide the handler is inert under automation, so no
	// other suite's kills load the guide.
	Module.SetPickupCardGuideForTesting(nullptr);
	Seams.ClientKilledBy.Broadcast(Ember);
	TestEqual(TEXT("inert under automation without a spec guide"), Shown(), 2);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
