#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTOnboardingModule.h"
#include "WoTOnboardingSettings.h"

#include "WoTOnboardingSeams.h"

#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

// The persisted toggles and their seam-registered settings rows (plan Task 4).
// The specs drive the live config-backed object through its public API, so
// every value they change is restored on exit through the same API; a box
// whose saved settings already match sees no writes at all (the setters
// no-op on equal values).

namespace
{
constexpr EAutomationTestFlags WoTOnboardingSettingsFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** Pins the three live values and restores them (via the setters) on exit. */
struct FWoTOnboardingSettingsSnapshot
{
	bool bMaster;
	bool bCards;
	bool bHints;

	FWoTOnboardingSettingsSnapshot()
	{
		const UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		bMaster = Live.bEnableOnboardingAids;
		bCards = Live.bShowFirstPickupCards;
		bHints = Live.bShowDeathCounterHints;
	}

	~FWoTOnboardingSettingsSnapshot()
	{
		UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		Live.SetEnableOnboardingAids(bMaster);
		Live.SetShowFirstPickupCards(bCards);
		Live.SetShowDeathCounterHints(bHints);
	}
};

/** This module's rows in the live seam registry, in registration order. */
TArray<const FWoTOnboardingRow*> WoTOnboardingCollectModuleRows()
{
	TArray<const FWoTOnboardingRow*> Rows;
	for (const FWoTOnboardingRow& Row : FWoTOnboardingSeams::Get().SettingsRows)
	{
		if (Row.Owner == FWoTOnboardingModule::SettingsRowOwner())
		{
			Rows.Add(&Row);
		}
	}
	return Rows;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSettingsDefaultsSpec,
	"WoT.Onboarding.SettingsDefaults", WoTOnboardingSettingsFlags)

bool FWoTOnboardingSettingsDefaultsSpec::RunTest(const FString&)
{
	// The compiled-in defaults are the contract: every aid ships on.
	TestTrue(TEXT("master defaults on"),
		UWoTOnboardingSettings::bDefaultEnableOnboardingAids);
	TestTrue(TEXT("pickup cards default on"),
		UWoTOnboardingSettings::bDefaultShowFirstPickupCards);
	TestTrue(TEXT("death hints default on"),
		UWoTOnboardingSettings::bDefaultShowDeathCounterHints);

	TestTrue(TEXT("Get answers one live object"),
		&UWoTOnboardingSettings::Get() == &UWoTOnboardingSettings::Get());

	// The live object may carry this box's saved values. Pin them, drive the
	// object to the compiled defaults through the public API, and prove a
	// fresh object starts from exactly those defaults.
	FWoTOnboardingSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	Live.SetEnableOnboardingAids(
		UWoTOnboardingSettings::bDefaultEnableOnboardingAids);
	Live.SetShowFirstPickupCards(
		UWoTOnboardingSettings::bDefaultShowFirstPickupCards);
	Live.SetShowDeathCounterHints(
		UWoTOnboardingSettings::bDefaultShowDeathCounterHints);

	const UWoTOnboardingSettings* Fresh =
		NewObject<UWoTOnboardingSettings>(GetTransientPackage());
	TestTrue(TEXT("fresh object: master on"), Fresh->bEnableOnboardingAids);
	TestTrue(TEXT("fresh object: pickup cards on"), Fresh->bShowFirstPickupCards);
	TestTrue(TEXT("fresh object: death hints on"), Fresh->bShowDeathCounterHints);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSettingsToggleDelegateSpec,
	"WoT.Onboarding.SettingsToggleDelegate", WoTOnboardingSettingsFlags)

bool FWoTOnboardingSettingsToggleDelegateSpec::RunTest(const FString&)
{
	FWoTOnboardingSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	Live.SetEnableOnboardingAids(true);
	Live.SetShowFirstPickupCards(true);
	Live.SetShowDeathCounterHints(true);

	int32 Broadcasts = 0;
	const FDelegateHandle Handle =
		Live.OnChanged.AddLambda([&Broadcasts]() { ++Broadcasts; });
	// Removed before the snapshot restores, so teardown never reaches the
	// spec-local counter.
	ON_SCOPE_EXIT { Live.OnChanged.Remove(Handle); };

	Live.ToggleEnableOnboardingAids();
	TestFalse(TEXT("toggle flips the master off"), Live.bEnableOnboardingAids);
	TestEqual(TEXT("toggle fired the change delegate"), Broadcasts, 1);

	Live.ToggleEnableOnboardingAids();
	TestTrue(TEXT("toggle flips the master back on"), Live.bEnableOnboardingAids);
	TestEqual(TEXT("the second toggle fired again"), Broadcasts, 2);

	Live.SetEnableOnboardingAids(true);
	TestEqual(TEXT("a no-op set does not fire"), Broadcasts, 2);

	Live.SetShowFirstPickupCards(false);
	TestFalse(TEXT("cards setter lands"), Live.bShowFirstPickupCards);
	TestEqual(TEXT("cards setter fired"), Broadcasts, 3);

	Live.ToggleShowDeathCounterHints();
	TestFalse(TEXT("hints toggle lands"), Live.bShowDeathCounterHints);
	TestEqual(TEXT("hints toggle fired"), Broadcasts, 4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingSettingsRowsRegisteredSpec,
	"WoT.Onboarding.SettingsRowsRegistered", WoTOnboardingSettingsFlags)

bool FWoTOnboardingSettingsRowsRegisteredSpec::RunTest(const FString&)
{
	TArray<const FWoTOnboardingRow*> Rows = WoTOnboardingCollectModuleRows();
	if (!TestEqual(TEXT("the module registered exactly three settings rows"),
		Rows.Num(), 3))
	{
		return false;
	}

	FWoTOnboardingSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	Live.SetEnableOnboardingAids(true);
	Live.SetShowFirstPickupCards(true);
	Live.SetShowDeathCounterHints(true);

	TestEqual(TEXT("master label reads on"), Rows[0]->Label(),
		FString(TEXT("Onboarding Aids: On")));
	TestEqual(TEXT("cards label reads on"), Rows[1]->Label(),
		FString(TEXT("First-Pickup Cards: On")));
	TestEqual(TEXT("hints label reads on"), Rows[2]->Label(),
		FString(TEXT("Death Counter Hints: On")));

	// Invoke round-trips the persisted value, and the lazy label follows.
	Rows[1]->Invoke();
	TestFalse(TEXT("cards invoke turned the setting off"),
		Live.bShowFirstPickupCards);
	TestEqual(TEXT("cards label follows the setting"), Rows[1]->Label(),
		FString(TEXT("First-Pickup Cards: Off")));
	Rows[1]->Invoke();
	TestTrue(TEXT("cards invoke turns it back on"), Live.bShowFirstPickupCards);

	// Master off: every row stays registered (the rows are the only in-game
	// way back on), and the aid labels say the master suppresses them.
	Rows[0]->Invoke();
	TestFalse(TEXT("master invoke turned the aids off"),
		Live.bEnableOnboardingAids);
	TestEqual(TEXT("master label reads off"), Rows[0]->Label(),
		FString(TEXT("Onboarding Aids: Off")));
	TestEqual(TEXT("master off leaves all three rows registered"),
		WoTOnboardingCollectModuleRows().Num(), 3);
	TestEqual(TEXT("cards label notes the master"), Rows[1]->Label(),
		FString(TEXT("First-Pickup Cards: On (aids off)")));
	TestEqual(TEXT("hints label notes the master"), Rows[2]->Label(),
		FString(TEXT("Death Counter Hints: On (aids off)")));

	// A module restart (as a reload performs) never doubles the rows. The
	// re-registration invalidates the collected pointers, so recount fresh.
	FModuleManager::GetModuleChecked<FWoTOnboardingModule>(
		TEXT("WoTOnboarding")).StartupModule();
	TestEqual(TEXT("re-registration keeps exactly three rows"),
		WoTOnboardingCollectModuleRows().Num(), 3);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
