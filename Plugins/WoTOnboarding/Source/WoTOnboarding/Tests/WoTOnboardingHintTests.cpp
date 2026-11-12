#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTAngrealGuide.h"
#include "WoTOnboardingHints.h"

#include "UObject/Package.h"

#include <initializer_list>

// The shared counter-hint resolver (plan Task 5; the Task 7 death recap
// reuses it). Synthetic fixtures throughout: invented class names, titles and
// hint wording only. Helper names carry the WoTOnboardingHint prefix — the
// plugin test files share one unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingHintFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTAngrealGuideRow WoTOnboardingHintRow(const TCHAR* ClassName,
	std::initializer_list<const TCHAR*> Elements,
	std::initializer_list<const TCHAR*> Counters,
	const TCHAR* HintText = TEXT(""))
{
	FWoTAngrealGuideRow Row;
	Row.ClassName = ClassName;
	for (const TCHAR* Element : Elements)
	{
		Row.Elements.Add(Element);
	}
	for (const TCHAR* Counter : Counters)
	{
		Row.Counters.Add(Counter);
	}
	Row.HintText = HintText;
	return Row;
}

/** The selection fixture: offenses with element flags, then counterers in a
    deliberate registration order — shield first, then the reflect-like row,
    then the dedicated rows — so tier preference has to beat Rows order. */
UWoTAngrealGuide* WoTOnboardingHintMakeGuide(bool bWithReflect,
	bool bWithDedicated, bool bWithSecondDedicated)
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("GaleJet"), {TEXT("Air")}, {},
		TEXT("{title} - {counter} tames it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("EmberShot"), {TEXT("Fire")}, {}));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("DualBolt"),
		{TEXT("Air"), TEXT("Fire")}, {}));
	// Shield-like: both countered rows share Air.
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("StoneWard"), {},
		{TEXT("GaleJet"), TEXT("DualBolt")}));
	if (bWithReflect)
	{
		// Reflect-like: its countered rows share no element.
		Guide->Rows.Add(WoTOnboardingHintRow(TEXT("MirrorBand"), {},
			{TEXT("GaleJet"), TEXT("EmberShot")}));
	}
	if (bWithDedicated)
	{
		Guide->Rows.Add(WoTOnboardingHintRow(TEXT("HushCharm"), {},
			{TEXT("GaleJet")}));
	}
	if (bWithSecondDedicated)
	{
		Guide->Rows.Add(WoTOnboardingHintRow(TEXT("QuietBead"), {},
			{TEXT("GaleJet")}));
	}
	// Countered by nothing: one template that wants a counter, one that
	// deliberately names none (the Balefire shape).
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("VoidLance"), {}, {},
		TEXT("{title} - {counter} stops it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("SunLance"), {}, {},
		TEXT("{title} - strike first.")));
	return Guide;
}

FString WoTOnboardingHintFixtureTitle(const FWoTAngrealGuideRow& Row)
{
	if (Row.ClassName == FName(TEXT("GaleJet")))
	{
		return TEXT("Gale Jet Rod");
	}
	if (Row.ClassName == FName(TEXT("HushCharm")))
	{
		return TEXT("Hush Charm Band");
	}
	if (Row.ClassName == FName(TEXT("MirrorBand")))
	{
		return TEXT("Mirror Band Ring");
	}
	if (Row.ClassName == FName(TEXT("StoneWard")))
	{
		return TEXT("Stone Ward Disc");
	}
	if (Row.ClassName == FName(TEXT("SunLance")))
	{
		return TEXT("Sun Lance Rod");
	}
	return FString();
}

const FWoTAngrealGuideRow* WoTOnboardingHintFindRow(
	const UWoTAngrealGuide& Guide, const TCHAR* ClassName)
{
	return Guide.FindByClassName(FName(ClassName));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintSelectDedicatedSpec,
	"WoT.Onboarding.HintSelectDedicated", WoTOnboardingHintFlags)

bool FWoTOnboardingHintSelectDedicatedSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ true, /*dedicated*/ true, /*second*/ false);
	const FWoTAngrealGuideRow* Target =
		WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet"));
	const FWoTAngrealGuideRow* Counter =
		WoTOnboardingHints::SelectCounter(*Guide, *Target);
	if (TestNotNull(TEXT("a counter is selected"), Counter))
	{
		// The dedicated counter wins although the shield and the reflect row
		// both registered before it.
		TestEqual(TEXT("dedicated counter preferred"), Counter->ClassName,
			FName(TEXT("HushCharm")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintSelectReflectFamilySpec,
	"WoT.Onboarding.HintSelectReflectFamily", WoTOnboardingHintFlags)

bool FWoTOnboardingHintSelectReflectFamilySpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ true, /*dedicated*/ false, /*second*/ false);
	const FWoTAngrealGuideRow* Target =
		WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet"));
	const FWoTAngrealGuideRow* Counter =
		WoTOnboardingHints::SelectCounter(*Guide, *Target);
	if (TestNotNull(TEXT("a counter is selected"), Counter))
	{
		// No dedicated counter: the reflect-family row (its countered rows
		// share no element) beats the earlier-registered element shield.
		TestEqual(TEXT("reflect family preferred over the shield"),
			Counter->ClassName, FName(TEXT("MirrorBand")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintSelectShieldFallbackSpec,
	"WoT.Onboarding.HintSelectShieldFallback", WoTOnboardingHintFlags)

bool FWoTOnboardingHintSelectShieldFallbackSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ false, /*dedicated*/ false, /*second*/ false);
	const FWoTAngrealGuideRow* Target =
		WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet"));
	const FWoTAngrealGuideRow* Counter =
		WoTOnboardingHints::SelectCounter(*Guide, *Target);
	if (TestNotNull(TEXT("a counter is selected"), Counter))
	{
		TestEqual(TEXT("the element shield is the last resort"),
			Counter->ClassName, FName(TEXT("StoneWard")));
	}
	TestNull(TEXT("a row countered by nothing selects nothing"),
		WoTOnboardingHints::SelectCounter(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("VoidLance"))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintSelectTieBreakSpec,
	"WoT.Onboarding.HintSelectTieBreak", WoTOnboardingHintFlags)

bool FWoTOnboardingHintSelectTieBreakSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ true, /*dedicated*/ true, /*second*/ true);
	const FWoTAngrealGuideRow* Counter = WoTOnboardingHints::SelectCounter(
		*Guide, *WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet")));
	if (TestNotNull(TEXT("a counter is selected"), Counter))
	{
		// Two dedicated counters: the first in Rows order stays (the
		// documented tie-break).
		TestEqual(TEXT("tie-break keeps Rows order"), Counter->ClassName,
			FName(TEXT("HushCharm")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintRenderOmissionSpec,
	"WoT.Onboarding.HintRenderOmission", WoTOnboardingHintFlags)

bool FWoTOnboardingHintRenderOmissionSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ true, /*dedicated*/ true, /*second*/ false);
	const auto Resolve = [](const FWoTAngrealGuideRow& Row)
	{
		return WoTOnboardingHintFixtureTitle(Row);
	};
	TestEqual(TEXT("a {counter} template on an uncountered row renders nothing"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("VoidLance")), Resolve),
		FString());
	TestEqual(TEXT("a counter-free template renders without counters (Balefire)"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("SunLance")), Resolve),
		FString(TEXT("Sun Lance Rod - strike first.")));
	TestEqual(TEXT("a row without a hint template renders nothing"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("EmberShot")), Resolve),
		FString());
	TestEqual(TEXT("a countered row's template resolves both placeholders"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet")), Resolve),
		FString(TEXT("Gale Jet Rod - Hush Charm Band tames it.")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintRenderTitleFallbackSpec,
	"WoT.Onboarding.HintRenderTitleFallback", WoTOnboardingHintFlags)

bool FWoTOnboardingHintRenderTitleFallbackSpec::RunTest(const FString&)
{
	// Spacing drops the registry affixes and splits camel-case boundaries.
	TestEqual(TEXT("AngrealInv prefix drops and spaces"),
		WoTOnboardingHints::SpacedClassName(FName(TEXT("AngrealInvQuietBead"))),
		FString(TEXT("Quiet Bead")));
	TestEqual(TEXT("Inventory suffix drops"),
		WoTOnboardingHints::SpacedClassName(FName(TEXT("AlarmFixtureInventory"))),
		FString(TEXT("Alarm Fixture")));
	TestEqual(TEXT("consecutive capitals stay together"),
		WoTOnboardingHints::SpacedClassName(FName(TEXT("AngrealInvAMAFixture"))),
		FString(TEXT("AMAFixture")));

	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide(
		/*reflect*/ false, /*dedicated*/ true, /*second*/ false);
	// No titles resolve: both placeholders fall back to spaced class names,
	// never placeholder prose.
	TestEqual(TEXT("unresolved titles fall back to spaced class names"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet")),
			[](const FWoTAngrealGuideRow&) { return FString(); }),
		FString(TEXT("Gale Jet - Hush Charm tames it.")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
