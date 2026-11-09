#include "UI/WoTTextEntry.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "CoreGlobals.h"
#include "UI/SWoTOnScreenKeyboard.h"
#include "UI/WoTBackStack.h"
#include "UI/WoTControllerUIInput.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTPreferencesSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTTextEntry, Log, All);

namespace
{
struct FWoTTextEntrySession
{
	/** Ties a panel's buttons to the session that opened it. */
	uint64 Id = 0;
	FWoTTextEntryRequest Request;
	FString Snapshot;
	EWoTTextEntryRoute Route = EWoTTextEntryRoute::Panel;
	int32 UserIndex = 0;
	TWeakPtr<SWoTTextEntryLayer> Layer;
	/** Compared in the layer's destructor, where weak pointers no longer pin. */
	const SWoTTextEntryLayer* LayerAddress = nullptr;
	TWeakPtr<SWoTOnScreenKeyboard> Panel;
	TSharedPtr<FWoTBackStackEntryHandle> BackEntry;
	TSharedPtr<FWoTCursorLease> CursorLease;
};

struct FWoTTextEntryGlobals
{
	TUniquePtr<FWoTTextEntrySession> Session;
	uint64 NextSessionId = 1;
	FString LastLaunchError;
#if WITH_AUTOMATION_TESTS
	FWoTTextEntry::FLauncher Launcher;
	TOptional<bool> SteamDeckOverride;
	TOptional<bool> EnabledOverride;
#endif
};

FWoTTextEntryGlobals& WoTTextEntryGlobals()
{
	static FWoTTextEntryGlobals Globals;
	return Globals;
}

FWoTTextEntrySession* WoTTextEntrySession()
{
	return WoTTextEntryGlobals().Session.Get();
}

/** True when Widget is Ancestor or sits inside it. */
bool WoTTextEntry_IsWithin(TSharedPtr<const SWidget> Widget, const SWidget* Ancestor)
{
	for (; Widget.IsValid() && Ancestor != nullptr; Widget = Widget->GetParentWidget())
	{
		if (Widget.Get() == Ancestor)
		{
			return true;
		}
	}
	return false;
}

/** True when the widget, and every ancestor up to its window, is visible. */
bool WoTTextEntry_IsShown(const TSharedPtr<SWidget>& Widget)
{
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (!Current->GetVisibility().IsVisible())
		{
			return false;
		}
		if (Current->Advanced_IsWindow())
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<SWidget> WoTTextEntry_Field(const FWoTTextEntrySession& Session)
{
	return Session.Request.GetField ? Session.Request.GetField() : nullptr;
}

void WoTTextEntry_FocusField(const FWoTTextEntrySession& Session)
{
	if (Session.Request.FocusField)
	{
		Session.Request.FocusField();
		return;
	}
	const TSharedPtr<SWidget> Field = WoTTextEntry_Field(Session);
	if (Field.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetUserFocus(Session.UserIndex, Field, EFocusCause::SetDirectly);
	}
}

/** The side-file rule of every per-user marker: automation never reads or
 * writes the player's real file (the WoTHostBans precedent). */
bool WoTTextEntry_SideFileAllowed()
{
#if WITH_AUTOMATION_TESTS
	if (GIsAutomationTesting && !UWoTPreferencesSubsystem::HasPreferencePathForTesting())
	{
		return false;
	}
#endif
	return true;
}

bool WoTTextEntry_Launch(FString& OutError)
{
	OutError.Reset();
#if WITH_AUTOMATION_TESTS
	if (const FWoTTextEntry::FLauncher& Launcher = WoTTextEntryGlobals().Launcher)
	{
		const bool bOpened = Launcher(FWoTTextEntry::SteamKeyboardUrl, OutError);
		if (!bOpened && OutError.IsEmpty())
		{
			OutError = TEXT("the launcher refused the URL");
		}
		return bOpened && OutError.IsEmpty();
	}
	if (GIsAutomationTesting)
	{
		// Specs never launch a URL.
		OutError = TEXT("launching URLs is disabled under automation");
		return false;
	}
#endif
	// ShellExecute under Wine hands steam:// to the Steam client.
	FPlatformProcess::LaunchURL(FWoTTextEntry::SteamKeyboardUrl, nullptr, &OutError);
	return OutError.IsEmpty();
}

/**
 * Ends the running session and hands it back for the caller's Commit or
 * Cancel. With bAbandon nothing calls into the request or the layer (their
 * owner is going away); with bFocusField focus returns to the field.
 */
TUniquePtr<FWoTTextEntrySession> WoTTextEntry_End(bool bAbandon, bool bFocusField)
{
	TUniquePtr<FWoTTextEntrySession> Ending = MoveTemp(WoTTextEntryGlobals().Session);
	if (!Ending.IsValid())
	{
		return Ending;
	}
	FWoTControllerUIInput::SetTextEntryActive(false);
	Ending->BackEntry.Reset();
	Ending->CursorLease.Reset();
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = Ending->Panel.Pin();
	if (Panel.IsValid())
	{
		Panel->StopRepeat();
	}
	if (bAbandon)
	{
		return Ending;
	}
	if (const TSharedPtr<SWoTTextEntryLayer> Layer = Ending->Layer.Pin())
	{
		Layer->HidePanel();
	}
	if (bFocusField)
	{
		WoTTextEntry_FocusField(*Ending);
	}
	if (Panel.IsValid() && Ending->Request.OnPanelClosed)
	{
		Ending->Request.OnPanelClosed();
	}
	return Ending;
}

/** Shows the panel over the session's field with Draft and takes focus. */
bool WoTTextEntry_OpenPanel(FWoTTextEntrySession& Session, const FString& Draft,
	const FText& Message)
{
	const TSharedPtr<SWoTTextEntryLayer> Layer = Session.Layer.Pin();
	if (!Layer.IsValid())
	{
		return false;
	}
	TSharedPtr<SWoTOnScreenKeyboard> Panel;
	SAssignNew(Panel, SWoTOnScreenKeyboard)
		.Title(Session.Request.Title)
		.InitialText(Draft)
		.MaxLength(Session.Request.MaxLength)
		.IsPassword(Session.Request.bPassword)
		.Filter(Session.Request.Filter)
		.Font(TAttribute<FSlateFontInfo>::CreateLambda(
			[WeakLayer = TWeakPtr<SWoTTextEntryLayer>(Layer)]()
			{
				const TSharedPtr<SWoTTextEntryLayer> Pinned = WeakLayer.Pin();
				return Pinned.IsValid() ? Pinned->GetPanelFont() : FSlateFontInfo();
			}))
		.OnDone_Lambda([Id = Session.Id]()
		{
			const FWoTTextEntrySession* Active = WoTTextEntrySession();
			if (Active != nullptr && Active->Id == Id)
			{
				FWoTTextEntry::Done();
			}
		})
		.OnCancel_Lambda([Id = Session.Id]()
		{
			const FWoTTextEntrySession* Active = WoTTextEntrySession();
			if (Active != nullptr && Active->Id == Id)
			{
				FWoTTextEntry::Cancel();
			}
		});
	Panel->SetMessage(Message);
	Session.Route = EWoTTextEntryRoute::Panel;
	Session.Panel = Panel;
	// The panel is a modal surface: a controller hides the pointer.
	Session.CursorLease = FWoTControllerUIInput::AcquireCursorLease();
	Layer->ShowPanel(Panel.ToSharedRef());
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetUserFocus(Session.UserIndex, Panel, EFocusCause::SetDirectly);
	}
	if (Session.Request.OnPanelOpened)
	{
		Session.Request.OnPanelOpened(Panel.ToSharedRef());
	}
	return true;
}

/** The session's field and panel, for "inside the session" checks. */
bool WoTTextEntry_IsSessionFocus(const FWoTTextEntrySession& Session,
	const TSharedPtr<SWidget>& Focused, const TSharedPtr<SWidget>& Field)
{
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = Session.Panel.Pin();
	return WoTTextEntry_IsWithin(Focused, Field.Get())
		|| (Panel.IsValid() && WoTTextEntry_IsWithin(Focused, Panel.Get()));
}
}

// ---------------------------------------------------------------------------
// Sessions

bool FWoTTextEntry::Begin(const FWoTTextEntryRequest& Request, const TSharedRef<SWidget>& From,
	int32 UserIndex, TOptional<EWoTTextEntryRoute> Route)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWoTTextEntryLayer> Layer = SWoTTextEntryLayer::FindFor(From);
	if (!Layer.IsValid())
	{
		UE_LOG(LogWoTTextEntry, Warning,
			TEXT("Text entry: no SWoTTextEntryLayer hosts the field; entry not started."));
		return false;
	}
	if (IsActive())
	{
		Cancel();
	}
	TUniquePtr<FWoTTextEntrySession> Created = MakeUnique<FWoTTextEntrySession>();
	Created->Id = WoTTextEntryGlobals().NextSessionId++;
	Created->Request = Request;
	if (!Created->Request.GetField)
	{
		const TWeakPtr<SWidget> WeakFrom = From;
		Created->Request.GetField = [WeakFrom]() { return WeakFrom.Pin(); };
	}
	Created->Snapshot = Created->Request.GetText ? Created->Request.GetText() : FString();
	Created->UserIndex = UserIndex;
	Created->Layer = Layer;
	Created->LayerAddress = Layer.Get();
	WoTTextEntryGlobals().Session = MoveTemp(Created);
	FWoTTextEntrySession& Session = *WoTTextEntryGlobals().Session;
	FWoTControllerUIInput::SetTextEntryActive(true);
	// Above every screen and overlay entry: on Steam's route B cancels the
	// entry; on the panel B and View reach the panel itself (Backspace and
	// Cancel), so this entry only keeps them from backing the screen.
	Session.BackEntry = FWoTBackStack::Get().Register(Layer.ToSharedRef(),
		[]() { return FWoTTextEntry::IsActive(); },
		[]()
		{
			const FWoTTextEntrySession* Active = WoTTextEntrySession();
			if (Active == nullptr)
			{
				return false;
			}
			if (Active->Route == EWoTTextEntryRoute::SteamKeyboard)
			{
				FWoTTextEntry::Cancel();
				return true;
			}
			return false;
		});

	const EWoTTextEntryRoute Chosen = Route.IsSet() ? Route.GetValue() : ChooseRoute();
	if (Chosen == EWoTTextEntryRoute::SteamKeyboard)
	{
		// Steam's keys type into the focused field.
		Session.Route = EWoTTextEntryRoute::SteamKeyboard;
		WoTTextEntry_FocusField(Session);
		FString Error;
		if (WoTTextEntry_Launch(Error))
		{
			WoTTextEntryGlobals().LastLaunchError.Reset();
			return true;
		}
		WoTTextEntryGlobals().LastLaunchError = Error;
		UE_LOG(LogWoTTextEntry, Warning,
			TEXT("Text entry: Steam's keyboard did not open (%s); using the keyboard panel."),
			*Error);
	}
	return WoTTextEntry_OpenPanel(Session, Session.Snapshot, FText::GetEmpty());
}

bool FWoTTextEntry::IsActive()
{
	return WoTTextEntrySession() != nullptr;
}

TOptional<EWoTTextEntryRoute> FWoTTextEntry::GetActiveRoute()
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	return Session != nullptr ? TOptional<EWoTTextEntryRoute>(Session->Route)
		: TOptional<EWoTTextEntryRoute>();
}

bool FWoTTextEntry::IsActiveWithin(const TSharedRef<const SWidget>& Ancestor)
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedPtr<SWoTTextEntryLayer> Layer = Session->Layer.Pin();
	return Layer.IsValid() && WoTTextEntry_IsWithin(Layer, &Ancestor.Get());
}

bool FWoTTextEntry::IsActiveForField(const TSharedPtr<const SWidget>& Field)
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr || !Field.IsValid())
	{
		return false;
	}
	const TSharedPtr<SWidget> Current = WoTTextEntry_Field(*Session);
	return Current.IsValid() && (WoTTextEntry_IsWithin(Field, Current.Get())
		|| WoTTextEntry_IsWithin(Current, Field.Get()));
}

TSharedPtr<SWoTOnScreenKeyboard> FWoTTextEntry::GetPanel()
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	return Session != nullptr ? Session->Panel.Pin() : nullptr;
}

FString FWoTTextEntry::GetPendingText()
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr)
	{
		return FString();
	}
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = Session->Panel.Pin();
	if (Session->Route == EWoTTextEntryRoute::Panel && Panel.IsValid())
	{
		return Panel->GetDraft();
	}
	return Session->Request.GetText ? Session->Request.GetText() : FString();
}

bool FWoTTextEntry::Done()
{
	FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr)
	{
		return false;
	}
	const FString Text = GetPendingText();
	const FWoTTextValidation Validation = Session->Request.Validate
		? Session->Request.Validate(Text) : FWoTTextValidation::Accept();
	if (!Validation.bAccepted)
	{
		// Nothing is committed; the entry stays open and names the rule.
		if (Session->Route == EWoTTextEntryRoute::SteamKeyboard)
		{
			SwitchToPanel(Validation.Message);
		}
		else if (const TSharedPtr<SWoTOnScreenKeyboard> Panel = Session->Panel.Pin())
		{
			Panel->SetMessage(Validation.Message);
		}
		return false;
	}
	if (Session->Route == EWoTTextEntryRoute::Panel && Session->Request.SetText)
	{
		Session->Request.SetText(Text);
	}
	// Ended before Commit: committing may close the field's screen (a chat
	// line sent, a password submitted).
	const TUniquePtr<FWoTTextEntrySession> Ended =
		WoTTextEntry_End(/*bAbandon*/ false, /*bFocusField*/ true);
	if (Ended.IsValid() && Ended->Request.Commit)
	{
		Ended->Request.Commit();
	}
	return true;
}

void FWoTTextEntry::Cancel()
{
	FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr)
	{
		return;
	}
	// Steam's keyboard types into the live field: cancel restores it [I8].
	if (Session->Request.GetText && Session->Request.SetText
		&& Session->Request.GetText() != Session->Snapshot)
	{
		if (Session->Route == EWoTTextEntryRoute::SteamKeyboard && FSlateApplication::IsInitialized())
		{
			// A focused text box never reloads its bound text: let go of the
			// field so it reloads the restored text when focus returns below.
			FSlateApplication::Get().ClearUserFocus(Session->UserIndex, EFocusCause::SetDirectly);
		}
		Session->Request.SetText(Session->Snapshot);
	}
	const TUniquePtr<FWoTTextEntrySession> Ended =
		WoTTextEntry_End(/*bAbandon*/ false, /*bFocusField*/ true);
	if (Ended.IsValid() && Ended->Request.Cancel)
	{
		Ended->Request.Cancel();
	}
}

bool FWoTTextEntry::CancelWithin(const TSharedRef<const SWidget>& Ancestor)
{
	if (!IsActiveWithin(Ancestor))
	{
		return false;
	}
	Cancel();
	return true;
}

void FWoTTextEntry::AbandonWithin(const TSharedRef<const SWidget>& Ancestor)
{
	if (IsActiveWithin(Ancestor))
	{
		WoTTextEntry_End(/*bAbandon*/ true, /*bFocusField*/ false);
	}
}

bool FWoTTextEntry::SwitchToPanel(const FText& Message)
{
	FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr || Session->Route != EWoTTextEntryRoute::SteamKeyboard)
	{
		return false;
	}
	// The panel continues from what Steam typed; Cancel still restores the
	// text from before the entry began.
	const FString SoFar = Session->Request.GetText ? Session->Request.GetText() : FString();
	return WoTTextEntry_OpenPanel(*Session, SoFar, Message);
}

void FWoTTextEntry::RestoreFocus()
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedPtr<SWoTOnScreenKeyboard> Panel = Session->Panel.Pin();
	if (Session->Route == EWoTTextEntryRoute::Panel && Panel.IsValid())
	{
		FSlateApplication::Get().SetUserFocus(Session->UserIndex, Panel, EFocusCause::SetDirectly);
		return;
	}
	WoTTextEntry_FocusField(*Session);
}

TArray<FWoTButtonPrompt> FWoTTextEntry::ControllerPrompts()
{
	const TOptional<EWoTTextEntryRoute> Route = GetActiveRoute();
	if (!Route.IsSet())
	{
		return {};
	}
	return Route.GetValue() == EWoTTextEntryRoute::SteamKeyboard
		? FWoTButtonPromptSets::SteamKeyboard() : FWoTButtonPromptSets::KeyboardPanel();
}

void FWoTTextEntry::NotifyLayerDestroyed(const SWoTTextEntryLayer* Layer)
{
	const FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session != nullptr && Session->LayerAddress == Layer)
	{
		WoTTextEntry_End(/*bAbandon*/ true, /*bFocusField*/ false);
	}
}

void FWoTTextEntry::TickLayer(const SWoTTextEntryLayer& Layer)
{
	FWoTTextEntrySession* Session = WoTTextEntrySession();
	if (Session == nullptr || Session->LayerAddress != &Layer || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedPtr<SWidget> Field = WoTTextEntry_Field(*Session);
	if (!Field.IsValid() || !WoTTextEntry_IsShown(Field))
	{
		// The field's screen went away: nothing left to type into.
		const TUniquePtr<FWoTTextEntrySession> Ended =
			WoTTextEntry_End(/*bAbandon*/ false, /*bFocusField*/ false);
		if (Ended.IsValid() && Ended->Request.Cancel)
		{
			Ended->Request.Cancel();
		}
		return;
	}
	FSlateApplication& SlateApp = FSlateApplication::Get();
	const TSharedPtr<SWidget> Focused = SlateApp.GetUserFocusedWidget(Session->UserIndex);
	if (!Focused.IsValid())
	{
		// Focus went nowhere (the window lost OS focus, for example to Steam's
		// keyboard): not a move to somewhere else [N2].
		return;
	}
	if (WoTTextEntry_IsSessionFocus(*Session, Focused, Field))
	{
		const TSharedPtr<SWoTOnScreenKeyboard> Panel = Session->Panel.Pin();
		if (Panel.IsValid() && Session->Route == EWoTTextEntryRoute::Panel
			&& !WoTTextEntry_IsWithin(Focused, Panel.Get()))
		{
			// A click on the field: typing still edits the panel's draft.
			SlateApp.SetUserFocus(Session->UserIndex, Panel, EFocusCause::SetDirectly);
		}
		return;
	}
	// Focus moved to another control: the entry ends without a commit. On
	// Steam's route what Steam typed stays in the field, exactly as with a
	// physical keyboard (owner ruling, 2026-11-09); the panel's draft is
	// dropped, as a cancel.
	const bool bSteamRoute = Session->Route == EWoTTextEntryRoute::SteamKeyboard;
	const TUniquePtr<FWoTTextEntrySession> Ended =
		WoTTextEntry_End(/*bAbandon*/ false, /*bFocusField*/ false);
	if (!bSteamRoute && Ended.IsValid() && Ended->Request.Cancel)
	{
		Ended->Request.Cancel();
	}
}

// ---------------------------------------------------------------------------
// The Steam route

bool FWoTTextEntry::IsSteamDeck()
{
#if WITH_AUTOMATION_TESTS
	if (WoTTextEntryGlobals().SteamDeckOverride.IsSet())
	{
		return WoTTextEntryGlobals().SteamDeckOverride.GetValue();
	}
#endif
	// Steam sets SteamDeck=1 for every game it launches on a Deck.
	return FPlatformMisc::GetEnvironmentVariable(TEXT("SteamDeck")) == TEXT("1");
}

bool FWoTTextEntry::IsSteamKeyboardAvailable()
{
	return WoTIsRunningUnderWine() && IsSteamDeck();
}

bool FWoTTextEntry::ParseToggle(TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty() || Bytes.Num() > 64)
	{
		return false;
	}
	FString Text;
	for (const uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return false;
		}
		Text.AppendChar(static_cast<TCHAR>(Byte));
	}
	Text.TrimEndInline();
	return Text.Equals(FormatToggle(true), ESearchCase::CaseSensitive);
}

FString FWoTTextEntry::FormatToggle(bool bEnabled)
{
	return FString::Printf(TEXT("WoTTextEntry-v1 steam-keyboard=%s"),
		bEnabled ? TEXT("on") : TEXT("off"));
}

bool FWoTTextEntry::IsSteamKeyboardEnabled()
{
#if WITH_AUTOMATION_TESTS
	if (WoTTextEntryGlobals().EnabledOverride.IsSet())
	{
		return WoTTextEntryGlobals().EnabledOverride.GetValue();
	}
#endif
	// The toggle lives in a preferences side file, not GameUserSettings: in
	// guarded player builds the engine patch resets the GameUserSettings ini
	// path, so a [WoT.TextEntry] section there would never reach disk. Missing
	// or invalid means off; it stays off by default until the Task 0 Deck
	// probe passes.
	if (!WoTTextEntry_SideFileAllowed())
	{
		return false;
	}
	FString Path;
	TArray<uint8> Bytes;
	if (!UWoTPreferencesSubsystem::SideFilePath(ToggleSideFileName, Path)
		|| !UWoTPreferencesSubsystem::LoadSideFileBytes(Path, 64, Bytes))
	{
		return false;
	}
	return ParseToggle(Bytes);
}

bool FWoTTextEntry::SetSteamKeyboardEnabled(bool bEnabled)
{
#if WITH_AUTOMATION_TESTS
	if (WoTTextEntryGlobals().EnabledOverride.IsSet())
	{
		WoTTextEntryGlobals().EnabledOverride = bEnabled;
		return true;
	}
#endif
	if (!WoTTextEntry_SideFileAllowed())
	{
		return false;
	}
	FString Path;
	if (!UWoTPreferencesSubsystem::SideFilePath(ToggleSideFileName, Path))
	{
		return false;
	}
	const FString Line = FormatToggle(bEnabled) + TEXT("\n");
	TArray<uint8> Bytes;
	Bytes.Reserve(Line.Len());
	for (const TCHAR Character : Line)
	{
		Bytes.Add(static_cast<uint8>(Character));
	}
	return UWoTPreferencesSubsystem::SaveSideFileBytes(Path, Bytes);
}

EWoTTextEntryRoute FWoTTextEntry::ChooseRoute()
{
	return IsSteamKeyboardAvailable() && IsSteamKeyboardEnabled()
		? EWoTTextEntryRoute::SteamKeyboard : EWoTTextEntryRoute::Panel;
}

FString FWoTTextEntry::GetLastLaunchError()
{
	return WoTTextEntryGlobals().LastLaunchError;
}

#if WITH_AUTOMATION_TESTS
void FWoTTextEntry::SetLauncherForTesting(FLauncher Launcher)
{
	WoTTextEntryGlobals().Launcher = MoveTemp(Launcher);
}

void FWoTTextEntry::SetSteamDeckEnvironmentForTesting(TOptional<bool> bSteamDeck)
{
	WoTTextEntryGlobals().SteamDeckOverride = bSteamDeck;
}

void FWoTTextEntry::SetSteamKeyboardEnabledForTesting(TOptional<bool> bEnabled)
{
	WoTTextEntryGlobals().EnabledOverride = bEnabled;
}

void FWoTTextEntry::ResetForTesting()
{
	WoTTextEntry_End(/*bAbandon*/ true, /*bFocusField*/ false);
	FWoTTextEntryGlobals& Globals = WoTTextEntryGlobals();
	Globals.Launcher = nullptr;
	Globals.SteamDeckOverride.Reset();
	Globals.EnabledOverride.Reset();
	Globals.LastLaunchError.Reset();
}
#endif
