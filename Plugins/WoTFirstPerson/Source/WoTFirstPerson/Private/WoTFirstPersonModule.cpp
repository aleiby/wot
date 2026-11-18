#include "WoTFirstPersonModule.h"

#include "WoTExtensionSeams.h"
#include "WoTFirstPersonCaptureScript.h"
#include "WoTFirstPersonPresenter.h"
#include "WoTFirstPersonSettings.h"

#include "Modules/ModuleManager.h"

namespace
{
void RemoveFirstPersonSection()
{
	FWoTExtensionSeams::Get().SettingsSections.RemoveAll(
		[](const FWoTExtensionSettingsSection& Section)
		{
			return Section.Owner == FWoTFirstPersonModule::SettingsSectionOwner();
		});
}
} // namespace

FName FWoTFirstPersonModule::SettingsSectionOwner()
{
	return FName(TEXT("WoTFirstPerson"));
}

void FWoTFirstPersonModule::StartupModule()
{
	// The section remains available while off, so the player can turn it on.
	// Callbacks resolve the CDO each time, including when a screen copies them.
	RemoveFirstPersonSection();
	FWoTExtensionSettingsSection Section;
	Section.Owner = SettingsSectionOwner();
	Section.Header = []() { return FString(TEXT("FIRST PERSON")); };
	Section.Order = 200;
	Section.Rows.Add({
		[]()
		{
			return FString::Printf(TEXT("Held Artifact: %s"),
				UWoTFirstPersonSettings::Get().bShowHeldArtifact ? TEXT("On") : TEXT("Off"));
		},
		[]() { UWoTFirstPersonSettings::Get().ToggleShowHeldArtifact(); },
		SettingsSectionOwner()});
	Section.Rows.Add({
		[]()
		{
			return FString::Printf(TEXT("Motion: %d%%"),
				FMath::RoundToInt(UWoTFirstPersonSettings::Get().Motion * 100.0f));
		},
		[]() { UWoTFirstPersonSettings::Get().StepMotion(); },
		SettingsSectionOwner()});
	Section.Rows.Add({
		[]()
		{
			return FString::Printf(TEXT("Model Field of View: %.0f"),
				UWoTFirstPersonSettings::Get().ModelFieldOfView);
		},
		[]() { UWoTFirstPersonSettings::Get().StepModelFieldOfView(); },
		SettingsSectionOwner()});
	FWoTExtensionSeams::Get().SettingsSections.Add(MoveTemp(Section));

	UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
	if (SettingsChangedHandle.IsValid())
	{
		Settings.OnChanged.Remove(SettingsChangedHandle);
	}
	SettingsChangedHandle = Settings.OnChanged.AddRaw(this,
		&FWoTFirstPersonModule::RefreshPresenter);
	RefreshPresenter();
#if !UE_BUILD_SHIPPING
	CaptureScript = MakeShared<FWoTFirstPersonCaptureScript>();
#endif
}

void FWoTFirstPersonModule::ShutdownModule()
{
#if !UE_BUILD_SHIPPING
	CaptureScript.Reset();
#endif
	Presenter.Reset();
	if (SettingsChangedHandle.IsValid())
	{
		// During exit purge the CDO and its delegate have already gone away.
		if (UObjectInitialized())
		{
			UWoTFirstPersonSettings::Get().OnChanged.Remove(SettingsChangedHandle);
		}
		SettingsChangedHandle.Reset();
	}
	RemoveFirstPersonSection();
}

void FWoTFirstPersonModule::RefreshPresenter()
{
	if (!UWoTFirstPersonSettings::Get().bShowHeldArtifact || IsRunningDedicatedServer())
	{
		Presenter.Reset();
	}
	else if (!Presenter.IsValid())
	{
		Presenter = MakeShared<FWoTFirstPersonPresenter>();
	}
}

IMPLEMENT_MODULE(FWoTFirstPersonModule, WoTFirstPerson)
