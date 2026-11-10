#include "WoTOnboardingModule.h"

#include "SWoTGuideBrowser.h"
#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingDeathRecap.h"
#include "WoTOnboardingPickupCards.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Modules/ModuleManager.h"

namespace
{
/** This module's name on the CloseClientWindows seam: its broadcast closes
    the retail windows, and any other keeper closes the guide. */
FName WoTOnboardingGuideKeeper()
{
	return FName(TEXT("WoTOnboardingGuide"));
}

/** Card z-order: under every client window (item info 1900, guide 1950, the
    in-game menu 2000, help 2100), so a window that opens over a card covers
    it even before the close broadcast removes it. */
constexpr int32 WoTOnboardingPickupCardZOrder = 1850;

/** Seconds on screen, fades included. A recap carries one more line to read
    than a pickup card, and the player is waiting to respawn anyway. */
constexpr float WoTOnboardingPickupCardLifetime = 4.0f;
constexpr float WoTOnboardingDeathRecapLifetime = 6.0f;

FName WoTOnboardingShowGuideAction()
{
	return FName(TEXT("ShowGuide"));
}

/** The live keys for an input action: the local player's own mappings, where
    the game applies its managed rebinds, else the project input settings
    (no player yet, as in the front end's first frames or under automation). */
TArray<FKey> WoTOnboardingActionKeys(FName Action)
{
	TArray<FKey> Keys;
	const UWorld* World = GEngine && GEngine->GameViewport
		? GEngine->GameViewport->GetWorld() : nullptr;
	const APlayerController* Controller =
		World ? World->GetFirstPlayerController() : nullptr;
	if (Controller && Controller->PlayerInput)
	{
		for (const FInputActionKeyMapping& Mapping :
			Controller->PlayerInput->GetKeysForAction(Action))
		{
			Keys.AddUnique(Mapping.Key);
		}
		return Keys;
	}
	if (const UInputSettings* Input = UInputSettings::GetInputSettings())
	{
		TArray<FInputActionKeyMapping> Mappings;
		Input->GetActionMappingByName(Action, Mappings);
		for (const FInputActionKeyMapping& Mapping : Mappings)
		{
			Keys.AddUnique(Mapping.Key);
		}
	}
	return Keys;
}

/** The first ShowGuide key's display name; empty while the action is unbound. */
FString WoTOnboardingGuideKeyName()
{
	for (const FKey& Key : WoTOnboardingActionKeys(WoTOnboardingShowGuideAction()))
	{
		if (Key.IsValid())
		{
			return Key.GetDisplayName(false).ToString();
		}
	}
	return FString();
}

FString WoTOnboardingGuideMenuLabel()
{
	// The in-game menu labels its windows "Name (Key)"; the guide row reads
	// its live binding the same way.
	const FString KeyName = WoTOnboardingGuideKeyName();
	return KeyName.IsEmpty() ? FString(TEXT("Field Guide"))
		: FString::Printf(TEXT("Field Guide (%s)"), *KeyName);
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

	// Every aid (the guide's menu row and every seam handler) follows the
	// master switch live: with it off the module holds no binding on any
	// seam, so a disabled plugin and a switched-off one leave the game the
	// same. Both refreshes are idempotent, so a restart (as the settings
	// specs perform) never doubles anything.
	RefreshMenuRowRegistration();
	RefreshSeamBindings();
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
	ClosePickupCard();
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	UnbindSeamHandlers();
	PickupCardTestGuide.Reset();
	bGuideAbsent = false;
	if (UObjectInitialized())
	{
		CachedGuide.Reset();
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

void FWoTOnboardingModule::RefreshSeamBindings()
{
	UnbindSeamHandlers();
	if (!UWoTOnboardingSettings::Get().bEnableOnboardingAids)
	{
		return;
	}
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	ShowGuideHandle = Seams.ShowGuideRequested.AddRaw(
		this, &FWoTOnboardingModule::HandleShowGuideRequested);
	CloseWindowsHandle = Seams.CloseClientWindows.AddRaw(
		this, &FWoTOnboardingModule::HandleCloseClientWindows);
	ItemAddedHandle = Seams.ClientItemAdded.AddRaw(
		this, &FWoTOnboardingModule::HandleClientItemAdded);
	KilledByHandle = Seams.ClientKilledBy.AddRaw(
		this, &FWoTOnboardingModule::HandleClientKilledBy);
}

void FWoTOnboardingModule::UnbindSeamHandlers()
{
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
	if (ItemAddedHandle.IsValid())
	{
		Seams.ClientItemAdded.Remove(ItemAddedHandle);
		ItemAddedHandle.Reset();
	}
	if (KilledByHandle.IsValid())
	{
		Seams.ClientKilledBy.Remove(KilledByHandle);
		KilledByHandle.Reset();
	}
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
	// The card yields to every window, the guide included.
	ClosePickupCard();
	if (Keeper != WoTOnboardingGuideKeeper())
	{
		CloseGuideBrowser();
	}
}

void FWoTOnboardingModule::HandleSettingsChanged()
{
	RefreshMenuRowRegistration();
	const UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
	if (!Settings.bEnableOnboardingAids)
	{
		// Master off tears down every aid: the open browser and any card.
		CloseGuideBrowser();
		ClosePickupCard();
	}
	else if (bCardIsDeathRecap ? !Settings.bShowDeathCounterHints
	                           : !Settings.bShowFirstPickupCards)
	{
		// The card slot serves both aids; the visible card goes with its own.
		ClosePickupCard();
	}
	RefreshSeamBindings();
}

void FWoTOnboardingModule::HandleClientItemAdded(FName ClassName)
{
	UWoTAngrealGuide* Guide = nullptr;
	if (GIsAutomationTesting)
	{
		// Inert under automation unless a pickup-card spec opted in: other
		// suites' inventory traffic must never write the persisted seen list.
		Guide = PickupCardTestGuide.Get();
		if (!Guide)
		{
			return;
		}
	}
	UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
	if (!Settings.bEnableOnboardingAids || !Settings.bShowFirstPickupCards
		|| Settings.HasSeenPickupCard(ClassName))
	{
		// The cheap verdicts first: no guide load on every inventory change.
		return;
	}
	if (!Guide)
	{
		Guide = ResolveGuide();
	}
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const bool bScreenBusy = PickupCard.IsValid() || GuideBrowser.IsValid()
		|| (Seams.IsClientWindowOpen && Seams.IsClientWindowOpen());
	if (WoTOnboardingPickupCards::Decide(Settings, Guide, ClassName, bScreenBusy)
		!= EWoTPickupCardVerdict::Show)
	{
		return;
	}
	Settings.MarkPickupCardSeen(ClassName);
	++PickupCardsShown;
	ShowPickupCard(*Guide->FindByClassName(ClassName));
}

void FWoTOnboardingModule::HandleClientKilledBy(FName ClassName)
{
	UWoTAngrealGuide* Guide = nullptr;
	if (GIsAutomationTesting)
	{
		// Inert under automation unless a spec opted in, as for the pickup
		// card: other suites' kills must not load the guide.
		Guide = PickupCardTestGuide.Get();
		if (!Guide)
		{
			return;
		}
	}
	const UWoTOnboardingSettings& Settings = UWoTOnboardingSettings::Get();
	if (!Settings.bEnableOnboardingAids || !Settings.bShowDeathCounterHints)
	{
		return;
	}
	if (!Guide)
	{
		Guide = ResolveGuide();
	}
	// A pickup card does not hold the screen against a recap, which
	// replaces it.
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const bool bScreenBusy = GuideBrowser.IsValid()
		|| (Seams.IsClientWindowOpen && Seams.IsClientWindowOpen());
	if (WoTOnboardingDeathRecap::Decide(Settings, Guide, ClassName, bScreenBusy)
		!= EWoTDeathRecapVerdict::Show)
	{
		return;
	}
	++DeathRecapsShown;
	ShowDeathRecap(*Guide, *Guide->FindByClassName(ClassName));
}

UWoTAngrealGuide* FWoTOnboardingModule::ResolveGuide()
{
	if (!CachedGuide.IsValid() && !bGuideAbsent)
	{
		if (UWoTAngrealGuide* Loaded = UWoTAngrealGuide::Load())
		{
			CachedGuide.Reset(Loaded);
		}
		else
		{
			// One quiet answer per session (the loader logged it).
			bGuideAbsent = true;
		}
	}
	return CachedGuide.Get();
}

bool FWoTOnboardingModule::CanHostCard()
{
	// The guide's hosting gates: a real local viewport, never under
	// automation (specs construct the widget directly).
	return !GIsAutomationTesting && !IsRunningDedicatedServer() && GEngine
		&& GEngine->GameViewport;
}

void FWoTOnboardingModule::ShowPickupCard(const FWoTAngrealGuideRow& Row)
{
	if (!CanHostCard())
	{
		return;
	}
	HostCard(WoTOnboardingPickupCards::ComposeText(Row,
		[](const FString& Section, const FString& Property)
		{
			// The resolver derives the owning file from the section.
			return FWoTOnboardingSeams::Get().ResolveText(
				FString(), Section, Property);
		},
		WoTOnboardingGuideKeyName()),
		Row.ClassName, /*bDeathRecap*/ false);
}

void FWoTOnboardingModule::ShowDeathRecap(const UWoTAngrealGuide& Guide,
	const FWoTAngrealGuideRow& Row)
{
	if (!CanHostCard())
	{
		return;
	}
	HostCard(WoTOnboardingDeathRecap::ComposeText(Guide, Row,
		[](const FWoTAngrealGuideRow& TitleRow)
		{
			// The retail localization address: section = the class name.
			return FWoTOnboardingSeams::Get().ResolveText(
				FString(), TitleRow.ClassName.ToString(), TEXT("Title"));
		},
		WoTOnboardingGuideKeyName()),
		Row.ClassName, /*bDeathRecap*/ true);
}

void FWoTOnboardingModule::HostCard(const FWoTPickupCardText& Text,
	FName ClassName, bool bDeathRecap)
{
	// One card slot: a newer card replaces the visible one.
	ClosePickupCard();
	PickupCardViewport = GEngine->GameViewport;
	PickupCardClass = ClassName;
	bCardIsDeathRecap = bDeathRecap;
	PickupCard = SNew(SWoTPickupCard)
		.Text(Text)
		.Lifetime(bDeathRecap ? WoTOnboardingDeathRecapLifetime
		                      : WoTOnboardingPickupCardLifetime)
		.OnExpired(FSimpleDelegate::CreateRaw(
			this, &FWoTOnboardingModule::ClosePickupCardDeferred));
	// Passive: no input mode change and no focus; play carries on.
	PickupCardViewport->AddViewportWidgetContent(
		PickupCard.ToSharedRef(), WoTOnboardingPickupCardZOrder);
}

void FWoTOnboardingModule::ClosePickupCard()
{
	if (PickupCardCloseTicker.IsValid())
	{
		// A direct close supersedes a pending deferred one, which could
		// otherwise remove a later card.
		FTSTicker::RemoveTicker(PickupCardCloseTicker);
		PickupCardCloseTicker.Reset();
	}
	if (!PickupCard.IsValid())
	{
		return;
	}
	if (UGameViewportClient* Viewport = PickupCardViewport.Get())
	{
		Viewport->RemoveViewportWidgetContent(PickupCard.ToSharedRef());
	}
	PickupCard.Reset();
	PickupCardViewport.Reset();
	PickupCardClass = NAME_None;
	bCardIsDeathRecap = false;
}

void FWoTOnboardingModule::ClosePickupCardDeferred()
{
	if (PickupCardCloseTicker.IsValid())
	{
		return;
	}
	PickupCardCloseTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float)
		{
			PickupCardCloseTicker.Reset();
			ClosePickupCard();
			return false;
		}));
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
	UWoTAngrealGuide* Guide = ResolveGuide();
	if (!Guide || Guide->Rows.IsEmpty())
	{
		// Compiled rows that fail to parse leave no guide; the aid quietly
		// does nothing (the loader already logged it once).
		return;
	}
	// The card's suggestion followed: the guide opens at the card's item.
	// Captured first, because the close broadcast below removes the card.
	const FName CardClass = PickupCardClass;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	// One client window at a time, as the original kept it: the retail
	// windows close before the guide takes the screen.
	Seams.CloseClientWindows.Broadcast(WoTOnboardingGuideKeeper());
	GuideViewport = GEngine->GameViewport;
	GuideBrowser = SNew(SWoTGuideBrowser)
		.Guide(Guide)
		.CloseKeys(WoTOnboardingActionKeys(WoTOnboardingShowGuideAction()))
		.OnClose(FSimpleDelegate::CreateRaw(
			this, &FWoTOnboardingModule::CloseGuideBrowser));
	if (!CardClass.IsNone())
	{
		GuideBrowser->SelectRow(CardClass);
	}
	// Above the item-info window (1900), below the in-game menu (2000) and
	// the help window (2100), though the close broadcasts keep these from
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
