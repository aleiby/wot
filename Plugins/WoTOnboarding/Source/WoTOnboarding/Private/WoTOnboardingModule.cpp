#include "WoTOnboardingModule.h"

#include "SWoTGuideBrowser.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/InputSettings.h"
#include "Modules/ModuleManager.h"

namespace
{
/** This module's name on the CloseClientWindows seam: its broadcast closes
    the retail windows, and any other keeper closes the guide. */
FName WoTOnboardingGuideKeeper()
{
	return FName(TEXT("WoTOnboardingGuide"));
}

/** The keys bound to the ShowGuide action (Task 4's F3 by default). The
    browser takes them as close keys, because UI-only input never routes a
    second press to the action binding — the window handles it, as the retail
    help window handles its own. */
TArray<FKey> WoTOnboardingShowGuideKeys()
{
	TArray<FKey> Keys;
	if (const UInputSettings* Input = UInputSettings::GetInputSettings())
	{
		TArray<FInputActionKeyMapping> Mappings;
		Input->GetActionMappingByName(FName(TEXT("ShowGuide")), Mappings);
		for (const FInputActionKeyMapping& Mapping : Mappings)
		{
			Keys.Add(Mapping.Key);
		}
	}
	return Keys;
}

FString WoTOnboardingGuideMenuLabel()
{
	// The in-game menu labels its windows "Name (Key)"; the guide row reads
	// its live binding the same way.
	const TArray<FKey> Keys = WoTOnboardingShowGuideKeys();
	return Keys.IsEmpty() || !Keys[0].IsValid()
		? FString(TEXT("Field Guide"))
		: FString::Printf(TEXT("Field Guide (%s)"),
			*Keys[0].GetDisplayName(false).ToString());
}

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

	// The guide (Task 5): the menu row follows the master toggle live — it is
	// an aid, unlike the settings rows above — and the F3 seam toggles the
	// browser. Remove-before-add keeps a restart (as the settings specs
	// perform) from doubling bindings.
	RefreshMenuRowRegistration();
	if (ShowGuideHandle.IsValid())
	{
		Seams.ShowGuideRequested.Remove(ShowGuideHandle);
	}
	ShowGuideHandle = Seams.ShowGuideRequested.AddRaw(
		this, &FWoTOnboardingModule::HandleShowGuideRequested);
	if (CloseWindowsHandle.IsValid())
	{
		Seams.CloseClientWindows.Remove(CloseWindowsHandle);
	}
	CloseWindowsHandle = Seams.CloseClientWindows.AddRaw(
		this, &FWoTOnboardingModule::HandleCloseClientWindows);
	UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
	if (SettingsChangedHandle.IsValid())
	{
		Settings.OnChanged.Remove(SettingsChangedHandle);
	}
	SettingsChangedHandle = Settings.OnChanged.AddRaw(
		this, &FWoTOnboardingModule::HandleSettingsChanged);
}

void FWoTOnboardingModule::ShutdownModule()
{
	CloseGuideBrowser();
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	if (ShowGuideHandle.IsValid())
	{
		Seams.ShowGuideRequested.Remove(ShowGuideHandle);
		ShowGuideHandle.Reset();
	}
	if (CloseWindowsHandle.IsValid())
	{
		Seams.CloseClientWindows.Remove(CloseWindowsHandle);
		CloseWindowsHandle.Reset();
	}
	if (SettingsChangedHandle.IsValid())
	{
		// The settings default object outlives module cycling; never leave a
		// handler into unloaded code behind. UObjects are gone during exit
		// purge, when the delegate dies with the object anyway.
		if (UObjectInitialized())
		{
			UWoTOnboardingSettings::Get().OnChanged.Remove(SettingsChangedHandle);
		}
		SettingsChangedHandle.Reset();
	}
	WoTOnboardingRemoveOwnedRows(Seams.SettingsRows);
	WoTOnboardingRemoveOwnedRows(Seams.MenuRows);
}

void FWoTOnboardingModule::RefreshMenuRowRegistration()
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	WoTOnboardingRemoveOwnedRows(Seams.MenuRows);
	if (!UWoTOnboardingSettings::Get().bEnableOnboardingAids)
	{
		return;
	}
	Seams.MenuRows.Add({
		[]() { return WoTOnboardingGuideMenuLabel(); },
		[this]() { ToggleGuideBrowser(); },
		SettingsRowOwner()});
}

void FWoTOnboardingModule::HandleShowGuideRequested()
{
	if (!UWoTOnboardingSettings::Get().bEnableOnboardingAids)
	{
		return;
	}
	ToggleGuideBrowser();
}

void FWoTOnboardingModule::HandleCloseClientWindows(FName Keeper)
{
	if (Keeper != WoTOnboardingGuideKeeper())
	{
		CloseGuideBrowser();
	}
}

void FWoTOnboardingModule::HandleSettingsChanged()
{
	RefreshMenuRowRegistration();
	if (!UWoTOnboardingSettings::Get().bEnableOnboardingAids)
	{
		// Master off tears down every aid, the open browser included.
		CloseGuideBrowser();
	}
}

void FWoTOnboardingModule::ToggleGuideBrowser()
{
	if (GuideBrowser.IsValid())
	{
		CloseGuideBrowser();
	}
	else
	{
		OpenGuideBrowser();
	}
}

void FWoTOnboardingModule::OpenGuideBrowser()
{
	// The same hosting gates as the retail item-info window: a real local
	// viewport only, and never under automation (specs construct the widget
	// directly instead).
	if (GuideBrowser.IsValid() || GIsAutomationTesting
		|| IsRunningDedicatedServer() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}
	UWoTAngrealGuide* Guide = UWoTAngrealGuide::Load();
	if (!Guide || Guide->Rows.IsEmpty())
	{
		// A tree whose import pipeline has not run carries no guide; the aid
		// quietly does nothing (the asset loader already logged it once).
		return;
	}
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	// One client window at a time, as the original kept it: the retail
	// windows close before the guide takes the screen.
	Seams.CloseClientWindows.Broadcast(WoTOnboardingGuideKeeper());
	GuideViewport = GEngine->GameViewport;
	GuideBrowser = SNew(SWoTGuideBrowser)
		.Guide(Guide)
		.CloseKeys(WoTOnboardingShowGuideKeys())
		.OnClose(FSimpleDelegate::CreateRaw(
			this, &FWoTOnboardingModule::CloseGuideBrowser));
	// Above the item-info window (1900), below the in-game menu (2000) and
	// the help window (2100) — though the close broadcasts keep these from
	// stacking in the first place.
	GuideViewport->AddViewportWidgetContent(GuideBrowser.ToSharedRef(), 1950);
	if (Seams.SetOverlayInput)
	{
		// In a match: the gameplay controller hosts the input switch, the
		// same UI-only + cursor + focus steps the retail overlays run.
		Seams.SetOverlayInput(true, GuideBrowser);
	}
	else if (FSlateApplication::IsInitialized())
	{
		// Front end (or any UI-input context): focus is enough.
		FSlateApplication::Get().SetKeyboardFocus(
			GuideBrowser, EFocusCause::SetDirectly);
	}
}

void FWoTOnboardingModule::CloseGuideBrowser()
{
	if (!GuideBrowser.IsValid())
	{
		return;
	}
	if (UGameViewportClient* Viewport = GuideViewport.Get())
	{
		Viewport->RemoveViewportWidgetContent(GuideBrowser.ToSharedRef());
	}
	GuideBrowser.Reset();
	GuideViewport.Reset();
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	if (Seams.SetOverlayInput)
	{
		Seams.SetOverlayInput(false, nullptr);
	}
	else if (FSlateApplication::IsInitialized())
	{
		// Front end: hand focus back to the viewport so its UI keeps keyboard
		// navigation.
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

IMPLEMENT_MODULE(FWoTOnboardingModule, WoTOnboarding)
