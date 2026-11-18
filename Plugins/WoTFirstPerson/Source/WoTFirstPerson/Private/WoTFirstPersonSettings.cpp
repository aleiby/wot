#include "WoTFirstPersonSettings.h"

UWoTFirstPersonSettings& UWoTFirstPersonSettings::Get()
{
	return *GetMutableDefault<UWoTFirstPersonSettings>();
}

void UWoTFirstPersonSettings::PersistChange()
{
	SaveConfig();
	OnChanged.Broadcast();
}

void UWoTFirstPersonSettings::SetShowHeldArtifact(bool bValue)
{
	if (bShowHeldArtifact != bValue)
	{
		bShowHeldArtifact = bValue;
		PersistChange();
	}
}

void UWoTFirstPersonSettings::SetMotion(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return;
	}
	const float Clamped = FMath::Clamp(Value, 0.0f, 1.0f);
	if (Motion != Clamped)
	{
		Motion = Clamped;
		PersistChange();
	}
}

void UWoTFirstPersonSettings::SetModelFieldOfView(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return;
	}
	const float Clamped = FMath::Clamp(Value, 40.0f, 120.0f);
	if (ModelFieldOfView != Clamped)
	{
		ModelFieldOfView = Clamped;
		PersistChange();
	}
}

void UWoTFirstPersonSettings::ToggleShowHeldArtifact()
{
	SetShowHeldArtifact(!bShowHeldArtifact);
}

void UWoTFirstPersonSettings::StepMotion()
{
	SetMotion(Motion >= 1.0f ? 0.0f : Motion + 0.25f);
}

void UWoTFirstPersonSettings::StepModelFieldOfView()
{
	SetModelFieldOfView(ModelFieldOfView >= 120.0f ? 40.0f : ModelFieldOfView + 5.0f);
}
