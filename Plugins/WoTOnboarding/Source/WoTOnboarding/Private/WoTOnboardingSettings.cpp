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

bool UWoTOnboardingSettings::HasSeenPickupCard(FName ClassName) const
{
	return SeenPickupCards.Contains(ClassName);
}

bool UWoTOnboardingSettings::MarkPickupCardSeen(FName ClassName)
{
	if (ClassName.IsNone() || SeenPickupCards.Contains(ClassName))
	{
		return false;
	}
	SeenPickupCards.Add(ClassName);
	SaveConfig();
	return true;
}

void UWoTOnboardingSettings::SetSeenPickupCards(const TArray<FName>& ClassNames)
{
	if (SeenPickupCards == ClassNames)
	{
		return;
	}
	SeenPickupCards = ClassNames;
	SaveConfig();
}
