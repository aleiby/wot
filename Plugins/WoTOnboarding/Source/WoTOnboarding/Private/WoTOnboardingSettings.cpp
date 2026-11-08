#include "WoTOnboardingSettings.h"

UWoTOnboardingSettings& UWoTOnboardingSettings::Get()
{
	return *GetMutableDefault<UWoTOnboardingSettings>();
}

void UWoTOnboardingSettings::Commit(bool& Field, bool bValue)
{
	if (Field == bValue)
	{
		return;
	}
	Field = bValue;
	SaveConfig();
	OnChanged.Broadcast();
}

void UWoTOnboardingSettings::SetEnableOnboardingAids(bool bValue)
{
	Commit(bEnableOnboardingAids, bValue);
}

void UWoTOnboardingSettings::SetShowFirstPickupCards(bool bValue)
{
	Commit(bShowFirstPickupCards, bValue);
}

void UWoTOnboardingSettings::SetShowDeathCounterHints(bool bValue)
{
	Commit(bShowDeathCounterHints, bValue);
}

void UWoTOnboardingSettings::ToggleEnableOnboardingAids()
{
	SetEnableOnboardingAids(!bEnableOnboardingAids);
}

void UWoTOnboardingSettings::ToggleShowFirstPickupCards()
{
	SetShowFirstPickupCards(!bShowFirstPickupCards);
}

void UWoTOnboardingSettings::ToggleShowDeathCounterHints()
{
	SetShowDeathCounterHints(!bShowDeathCounterHints);
}
