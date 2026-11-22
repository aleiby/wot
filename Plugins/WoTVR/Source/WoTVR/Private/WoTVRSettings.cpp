#include "WoTVRSettings.h"

#include "Misc/AutomationTest.h"

UWoTVRSettings& UWoTVRSettings::Get()
{
	return *GetMutableDefault<UWoTVRSettings>();
}

void UWoTVRSettings::Persist()
{
	// Under automation nothing reaches disk: a spec that left the master enable
	// on would start a headset runtime at the developer's next launch.
	if (!GIsAutomationTesting)
	{
		SaveConfig();
	}
	OnChanged.Broadcast();
}

void UWoTVRSettings::SetEnableVR(bool bValue)
{
	if (bEnableVR != bValue)
	{
		bEnableVR = bValue;
		Persist();
	}
}

void UWoTVRSettings::SetStartupMode(EWoTVRStartupMode Value)
{
	if (StartupMode != Value)
	{
		StartupMode = Value;
		Persist();
	}
}

void UWoTVRSettings::SetPosture(EWoTVRPosture Value)
{
	if (Posture != Value)
	{
		Posture = Value;
		Persist();
	}
}

void UWoTVRSettings::SetStandingEyeHeightMeters(float Value)
{
	const float Clamped = FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0f, 3.0f) : 0.0f;
	if (StandingEyeHeightMeters != Clamped)
	{
		StandingEyeHeightMeters = Clamped;
		Persist();
	}
}

void UWoTVRSettings::SetTurnMode(EWoTVRTurnMode Value)
{
	if (TurnMode != Value)
	{
		TurnMode = Value;
		Persist();
	}
}

const FKey& UWoTVRSettings::GetKey(EWoTVRKeyAction Action) const
{
	switch (Action)
	{
	case EWoTVRKeyAction::Recentre: return RecentreKey;
	case EWoTVRKeyAction::TurnLeft: return TurnLeftKey;
	case EWoTVRKeyAction::TurnRight: return TurnRightKey;
	case EWoTVRKeyAction::ModeSwitch:
	default: return ModeSwitchKey;
	}
}

void UWoTVRSettings::SetKey(EWoTVRKeyAction Action, const FKey& Key)
{
	FKey* Slot = &ModeSwitchKey;
	switch (Action)
	{
	case EWoTVRKeyAction::Recentre: Slot = &RecentreKey; break;
	case EWoTVRKeyAction::TurnLeft: Slot = &TurnLeftKey; break;
	case EWoTVRKeyAction::TurnRight: Slot = &TurnRightKey; break;
	case EWoTVRKeyAction::ModeSwitch:
	default: break;
	}
	const FKey Value = Key.IsValid() ? Key : EKeys::Invalid;
	if (*Slot != Value)
	{
		*Slot = Value;
		Persist();
	}
}
