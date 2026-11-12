#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTAngrealGuide.h"
#include "WoTOnboardingHints.h"

#include "UObject/Package.h"

#include <initializer_list>

// The shared counter-hint resolver (plan Task 5; the Task 7 death recap
// reuses it). Synthetic fixtures throughout: invented class names, titles and
// hint wording only. Helper names carry the WoTOnboardingHint prefix: the
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

/** Offenses whose templates each name one counter, and the counterers. The
    shield registers first and counters GaleJet too, so a hint naming the
    later HushCharm proves the named counter wins over registration order. */
UWoTAngrealGuide* WoTOnboardingHintMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("GaleJet"), {TEXT("Air")}, {},
		TEXT("{counter:HushCharm} tames it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("EmberShot"), {TEXT("Fire")}, {}));
	// Names a real counterer that does not counter this row.
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("DualBolt"),
		{TEXT("Air"), TEXT("Fire")}, {}, TEXT("{counter:MirrorBand} turns it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("StoneWard"), {},
		{TEXT("GaleJet"), TEXT("DualBolt")}));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("MirrorBand"), {},
		{TEXT("GaleJet"), TEXT("EmberShot")}));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("HushCharm"), {},
		{TEXT("GaleJet")}));
	// Countered by nothing: one template naming a counter anyway, one naming
	// a class the guide does not carry, and one naming none (the Balefire
	// shape, which always renders).
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("VoidLance"), {}, {},
		TEXT("{counter:HushCharm} stops it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("GhostLance"), {}, {},
		TEXT("{counter:NoSuchCharm} stops it.")));
	Guide->Rows.Add(WoTOnboardingHintRow(TEXT("SunLance"), {}, {},
		TEXT("Strike first.")));
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

FString WoTOnboardingHintLineFor(const UWoTAngrealGuide& Guide,
	const FWoTAngrealGuideRow& Row)
{
	return WoTOnboardingHints::ResolveHintLine(Guide, Row,
		[](const FWoTAngrealGuideRow& TitleRow)
		{
			return WoTOnboardingHintFixtureTitle(TitleRow);
		});
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintCounterNamedSpec,
	"WoT.Onboarding.HintCounterNamed", WoTOnboardingHintFlags)

bool FWoTOnboardingHintCounterNamedSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide();
	const FWoTAngrealGuideRow* Row =
		WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet"));
	const FWoTAngrealGuideRow* Counter =
		WoTOnboardingHints::HintCounter(*Guide, *Row);
	if (TestNotNull(TEXT("the named counter resolves"), Counter))
	{
		// StoneWard and MirrorBand also counter GaleJet and register first;
		// the template's own counter is the one its sentence describes.
		TestEqual(TEXT("the named counter, not the first counterer"),
			Counter->ClassName, FName(TEXT("HushCharm")));
	}
	TestEqual(TEXT("the line names the counter's title"),
		WoTOnboardingHintLineFor(*Guide, *Row),
		FString(TEXT("Hush Charm Band tames it.")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintCounterUnbackedSpec,
	"WoT.Onboarding.HintCounterUnbacked", WoTOnboardingHintFlags)

bool FWoTOnboardingHintCounterUnbackedSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide();
	for (const TCHAR* ClassName :
		{TEXT("DualBolt"), TEXT("VoidLance"), TEXT("GhostLance")})
	{
		const FWoTAngrealGuideRow* Row =
			WoTOnboardingHintFindRow(*Guide, ClassName);
		TestNull(FString::Printf(TEXT("%s: an unbacked counter resolves to nothing"),
			ClassName), WoTOnboardingHints::HintCounter(*Guide, *Row));
		TestEqual(FString::Printf(TEXT("%s: an unbacked counter renders no line"),
			ClassName), WoTOnboardingHintLineFor(*Guide, *Row), FString());
	}
	TestNull(TEXT("a counter-free template names no counter"),
		WoTOnboardingHints::HintCounter(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("SunLance"))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintBareCounterRefusedSpec,
	"WoT.Onboarding.HintBareCounterRefused", WoTOnboardingHintFlags)

bool FWoTOnboardingHintBareCounterRefusedSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide();
	FWoTAngrealGuideRow Row =
		*WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet"));
	for (const TCHAR* Template : {
		TEXT("{counter} tames it."),           // a rule would have to pick one
		TEXT("{boom} tames it."),              // unknown placeholder
		TEXT("{counter:HushCharm tames it.")}) // unterminated
	{
		Row.HintText = Template;
		TestEqual(FString::Printf(TEXT("'%s' renders no line"), Template),
			WoTOnboardingHintLineFor(*Guide, Row), FString());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHintRenderOmissionSpec,
	"WoT.Onboarding.HintRenderOmission", WoTOnboardingHintFlags)

bool FWoTOnboardingHintRenderOmissionSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide();
	TestEqual(TEXT("a counter-free template renders as written (Balefire)"),
		WoTOnboardingHintLineFor(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("SunLance"))),
		FString(TEXT("Strike first.")));
	TestEqual(TEXT("a row without a hint template renders nothing"),
		WoTOnboardingHintLineFor(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("EmberShot"))),
		FString());
	FWoTAngrealGuideRow Titled =
		*WoTOnboardingHintFindRow(*Guide, TEXT("SunLance"));
	Titled.HintText = TEXT("{title} burns bright.");
	TestEqual(TEXT("{title} still resolves to the row's own title"),
		WoTOnboardingHintLineFor(*Guide, Titled),
		FString(TEXT("Sun Lance Rod burns bright.")));
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

	UWoTAngrealGuide* Guide = WoTOnboardingHintMakeGuide();
	// No titles resolve: the counter falls back to its spaced class name,
	// never placeholder prose.
	TestEqual(TEXT("an unresolved counter title falls back to its class name"),
		WoTOnboardingHints::ResolveHintLine(*Guide,
			*WoTOnboardingHintFindRow(*Guide, TEXT("GaleJet")),
			[](const FWoTAngrealGuideRow&) { return FString(); }),
		FString(TEXT("Hush Charm tames it.")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
