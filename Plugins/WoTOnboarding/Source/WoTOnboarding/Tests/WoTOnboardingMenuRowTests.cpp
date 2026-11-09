#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTOnboardingModule.h"
#include "WoTOnboardingSettings.h"

#include "WoTOnboardingSeams.h"

#include "Modules/ModuleManager.h"

// The Field Guide menu row (plan Task 5): unlike the settings rows, which are
// the way back on and register unconditionally, the guide row is an aid: it
// registers while the master toggle is on and unregisters live when it turns
// off, through the settings OnChanged delegate. The spec drives the public
// settings API only and restores every value it changes. Helper names carry
// the WoTOnboardingMenuRow prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingMenuRowFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** Pins the master toggle and restores it (via the setter) on exit. */
struct FWoTOnboardingMenuRowMasterSnapshot
{
	bool bMaster;

	FWoTOnboardingMenuRowMasterSnapshot()
		: bMaster(UWoTOnboardingSettings::Get().bEnableOnboardingAids)
	{
	}

	~FWoTOnboardingMenuRowMasterSnapshot()
	{
		UWoTOnboardingSettings::Get().SetEnableOnboardingAids(bMaster);
	}
};

/** This module's rows in the live menu-row registry, in registration order. */
TArray<const FWoTOnboardingRow*> WoTOnboardingMenuRowCollect()
{
	TArray<const FWoTOnboardingRow*> Rows;
	for (const FWoTOnboardingRow& Row : FWoTOnboardingSeams::Get().MenuRows)
	{
		if (Row.Owner == FWoTOnboardingModule::SettingsRowOwner())
		{
			Rows.Add(&Row);
		}
	}
	return Rows;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideMenuRowToggleSpec,
	"WoT.Onboarding.GuideMenuRowToggle", WoTOnboardingMenuRowFlags)

bool FWoTOnboardingGuideMenuRowToggleSpec::RunTest(const FString&)
{
	FWoTOnboardingMenuRowMasterSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();

	Live.SetEnableOnboardingAids(true);
	TArray<const FWoTOnboardingRow*> Rows = WoTOnboardingMenuRowCollect();
	if (!TestEqual(TEXT("master on registers exactly one menu row"),
		Rows.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the row is owner-tagged"),
		Rows[0]->Owner == FWoTOnboardingModule::SettingsRowOwner());
	TestTrue(TEXT("the row labels the field guide"),
		Rows[0]->Label && Rows[0]->Label().Contains(TEXT("Field Guide")));
	TestTrue(TEXT("the row carries an invoke"),
		static_cast<bool>(Rows[0]->Invoke));

	// The master toggle unregisters the row live, through OnChanged, with no
	// module restart involved.
	Live.SetEnableOnboardingAids(false);
	TestEqual(TEXT("master off unregisters the menu row"),
		WoTOnboardingMenuRowCollect().Num(), 0);
	TestEqual(TEXT("the settings rows stay (they are the way back on)"),
		FWoTOnboardingSeams::Get().SettingsRows.FilterByPredicate(
			[](const FWoTOnboardingRow& Row)
			{
				return Row.Owner == FWoTOnboardingModule::SettingsRowOwner();
			}).Num(), 3);

	Live.SetEnableOnboardingAids(true);
	TestEqual(TEXT("master on re-registers the menu row"),
		WoTOnboardingMenuRowCollect().Num(), 1);

	// A module restart (as a reload performs) never doubles the row.
	FModuleManager::GetModuleChecked<FWoTOnboardingModule>(
		TEXT("WoTOnboarding")).StartupModule();
	TestEqual(TEXT("re-registration keeps exactly one row"),
		WoTOnboardingMenuRowCollect().Num(), 1);
	// And the restarted handler still follows the toggle.
	Live.SetEnableOnboardingAids(false);
	TestEqual(TEXT("the restarted module still unregisters on master off"),
		WoTOnboardingMenuRowCollect().Num(), 0);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
