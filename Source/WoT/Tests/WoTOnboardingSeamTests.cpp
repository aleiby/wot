#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTOnboardingSeams.h"

// The onboarding seams' contract is that they are inert when unbound: the
// plan's parity-off gate (Task 7) leans on exactly this, so a build without
// the plugin behaves like one with it disabled. These specs exercise fresh
// local instances — the struct is plain — so they stay true after later
// tasks bind handlers and register rows on the live singleton. No worlds.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingSeamFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamUnboundBroadcastSpec,
	"WoT.Onboarding.SeamUnboundBroadcast", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamUnboundBroadcastSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestFalse(TEXT("no item listener"), Seams.ClientItemAdded.IsBound());
	TestFalse(TEXT("no death listener"), Seams.ClientDeathMessage.IsBound());
	TestFalse(TEXT("no guide listener"), Seams.ShowGuideRequested.IsBound());
	// Broadcasting into the void must be safe and effect-free; the hook sites
	// in the inventory component and the player controller rely on it.
	Seams.ClientItemAdded.Broadcast(FName(TEXT("EmberLob")));
	Seams.ClientDeathMessage.Broadcast(TEXT("Wanderer fell from a ledge."));
	Seams.ShowGuideRequested.Broadcast();
	TestTrue(TEXT("the seam registry is one process-wide instance"),
		&FWoTOnboardingSeams::Get() == &FWoTOnboardingSeams::Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamRegistriesEmptySpec,
	"WoT.Onboarding.SeamRegistriesEmpty", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamRegistriesEmptySpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestEqual(TEXT("menu registry starts empty"), Seams.MenuRows.Num(), 0);
	TestEqual(TEXT("settings registry starts empty"),
		Seams.SettingsRows.Num(), 0);
	// A registered row round-trips both callbacks (consumed by Tasks 4-5).
	int32 Invocations = 0;
	Seams.MenuRows.Add({
		[]() { return FString(TEXT("Fixture Row")); },
		[&Invocations]() { ++Invocations; }});
	TestEqual(TEXT("row label resolves lazily"), Seams.MenuRows[0].Label(),
		FString(TEXT("Fixture Row")));
	Seams.MenuRows[0].Invoke();
	TestEqual(TEXT("row invoke ran once"), Invocations, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSeamResolverRoundTripSpec,
	"WoT.Onboarding.SeamResolverRoundTrip", WoTOnboardingSeamFlags)

bool FWoTOnboardingSeamResolverRoundTripSpec::RunTest(const FString&)
{
	FWoTOnboardingSeams Seams;
	TestEqual(TEXT("unbound resolver answers empty"),
		Seams.ResolveText(TEXT("f"), TEXT("s"), TEXT("p")), FString());
	Seams.ResolveRetailText = [](const FString& File, const FString& Section,
		const FString& Property)
	{
		return File + TEXT("/") + Section + TEXT(".") + Property;
	};
	TestEqual(TEXT("installed resolver sees every address part"),
		Seams.ResolveText(TEXT("Pack"), TEXT("FixtureSection"),
			TEXT("Title")),
		FString(TEXT("Pack/FixtureSection.Title")));

	// The live singleton: the game module installed the retail-text wrapper
	// at startup. A section no retail file defines resolves empty through the
	// whole stack, with or without retail data present, so no retail prose is
	// pinned here.
	TestTrue(TEXT("game module installed the retail resolver"),
		static_cast<bool>(FWoTOnboardingSeams::Get().ResolveRetailText));
	TestEqual(TEXT("unknown retail section resolves empty"),
		FWoTOnboardingSeams::Get().ResolveText(TEXT("WoT"),
			TEXT("WoTOnboardingNoSuchSection"), TEXT("Title")),
		FString());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
