#include "WoTOnboardingModule.h"

#include "Modules/ModuleManager.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

namespace
{
FString WoTOnboardingAidRowLabel(const TCHAR* Name, bool bOn, bool bMasterOn)
{
	FString Label = FString::Printf(
		TEXT("%s: %s"), Name, bOn ? TEXT("On") : TEXT("Off"));
	if (!bMasterOn)
	{
		// The aid keeps its own value while the master suppresses it; the
		// label says so instead of claiming the aid is active.
		Label += TEXT(" (aids off)");
	}
	return Label;
}

void WoTOnboardingRemoveOwnedRows(TArray<FWoTOnboardingRow>& Rows)
{
	Rows.RemoveAll([](const FWoTOnboardingRow& Row)
	{
		return Row.Owner == FWoTOnboardingModule::SettingsRowOwner();
	});
}
} // namespace

FName FWoTOnboardingModule::SettingsRowOwner()
{
	return FName(TEXT("WoTOnboarding"));
}

void FWoTOnboardingModule::StartupModule()
{
	// The three rows register UNCONDITIONALLY: the settings screen is the only
	// in-game way to turn the aids back on, so the master switch must never
	// hide them. Only aid BEHAVIOR gates on the toggles (Tasks 5-7). Labels
	// resolve lazily at draw time, so they always show the live values.
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	// Restart-safe: a reload never leaves a previous registration to double
	// the rows.
	WoTOnboardingRemoveOwnedRows(Seams.SettingsRows);
	Seams.SettingsRows.Add({
		[]()
		{
			return FString::Printf(TEXT("Onboarding Aids: %s"),
				UWoTOnboardingSettings::Get().bEnableOnboardingAids
					? TEXT("On") : TEXT("Off"));
		},
		[]() { UWoTOnboardingSettings::Get().ToggleEnableOnboardingAids(); },
		SettingsRowOwner()});
	Seams.SettingsRows.Add({
		[]()
		{
			const UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
			return WoTOnboardingAidRowLabel(TEXT("First-Pickup Cards"),
				Settings.bShowFirstPickupCards, Settings.bEnableOnboardingAids);
		},
		[]() { UWoTOnboardingSettings::Get().ToggleShowFirstPickupCards(); },
		SettingsRowOwner()});
	Seams.SettingsRows.Add({
		[]()
		{
			const UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
			return WoTOnboardingAidRowLabel(TEXT("Death Counter Hints"),
				Settings.bShowDeathCounterHints, Settings.bEnableOnboardingAids);
		},
		[]() { UWoTOnboardingSettings::Get().ToggleShowDeathCounterHints(); },
		SettingsRowOwner()});
}

void FWoTOnboardingModule::ShutdownModule()
{
	WoTOnboardingRemoveOwnedRows(FWoTOnboardingSeams::Get().SettingsRows);
}

IMPLEMENT_MODULE(FWoTOnboardingModule, WoTOnboarding)
