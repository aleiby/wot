#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "UI/SWoTButtonPrompts.h"

class SWidget;
class SWoTOnScreenKeyboard;
class SWoTTextEntryLayer;

/** Done's answer for a request (spec r3 section 3 [M6]). */
struct WOT_API FWoTTextValidation
{
	bool bAccepted = true;
	/** Why a Reject was refused; shown on the panel. */
	FText Message;

	static FWoTTextValidation Accept() { return FWoTTextValidation(); }
	static FWoTTextValidation Reject(const FText& InMessage)
	{
		FWoTTextValidation Result;
		Result.bAccepted = false;
		Result.Message = InMessage;
		return Result;
	}
};

/** A live per-edit filter (the player name's allowed characters). */
using FWoTTextEntryFilter = TFunction<FString(const FString&)>;

/**
 * One text field's entry by controller or touch (spec r3 section 3). The
 * request is delegate-based and bound to the field's owner (its view model),
 * never to the field widget, so a screen rebuild during entry keeps the
 * draft: GetField re-resolves the live widget each time it is asked.
 */
struct WOT_API FWoTTextEntryRequest
{
	/** The panel's heading ("Server Name", "Say"). */
	TAttribute<FText> Title;
	/** Characters the entry accepts; 0 is unbounded. */
	int32 MaxLength = 0;
	/** The panel's preview is masked. */
	bool bPassword = false;

	/** The field's widget now. Unset: the activator's own field. */
	TFunction<TSharedPtr<SWidget>()> GetField;
	/** Gives the field keyboard focus. Unset: focus GetField(). */
	TFunction<void()> FocusField;
	TFunction<FString()> GetText;
	TFunction<void(const FString&)> SetText;
	/** Optional live filter run on the draft after every edit. */
	FWoTTextEntryFilter Filter;
	/** Done runs it: Reject keeps the entry open with the message. Unset accepts. */
	TFunction<FWoTTextValidation(const FString&)> Validate;
	/** After an accepted Done has set the text. */
	TFunction<void()> Commit;
	/** After a cancelled entry restored the snapshot. */
	TFunction<void()> Cancel;
	/** The panel opened over the field (chat suspends its input router). */
	TFunction<void(const TSharedRef<SWidget>& Panel)> OnPanelOpened;
	/** The panel closed by any route except abandonment. */
	TFunction<void()> OnPanelClosed;
};

/** How a session takes the text. */
enum class EWoTTextEntryRoute : uint8
{
	/** The project's QWERTY panel (everywhere; the Deck's fallback). */
	Panel,
	/** Steam's own keyboard, typing into the focused field (the Deck). */
	SteamKeyboard
};

/**
 * Text entry sessions (spec r3 section 3). One session runs at a time.
 *
 * Begin snapshots the field's text and picks a route: Steam's keyboard when
 * the game runs under Wine with SteamDeck=1 and the player's toggle is on,
 * else the QWERTY panel. The panel edits a draft; Done validates it, sets the
 * text and commits; Cancel restores the snapshot (which also undoes Steam's
 * typing into the live field). A session also ends when focus moves to
 * somewhere other than the field or its panel [N2]. A focus that simply goes
 * away (the game window losing OS focus to Steam's keyboard) is not a move.
 *
 * While a session runs, keyboard keys never flip the last input to Pointer
 * (FWoTControllerUIInput::SetTextEntryActive), and a back-stack entry sits on
 * top: on the panel B cancels and View does nothing (the owner's Steam Deck
 * ruling, 2026-11-09); on Steam's route B cancels the entry.
 *
 * The panel is shown by the nearest SWoTTextEntryLayer around the field, so a
 * host screen wraps its content in one. Exported for plugin windows.
 */
class WOT_API FWoTTextEntry
{
public:
	static constexpr const TCHAR* SteamKeyboardUrl = TEXT("steam://open/keyboard");
	/** Side file (UWoTPreferencesSubsystem::SideFilePath) holding the toggle. */
	static constexpr const TCHAR* ToggleSideFileName = TEXT("TextEntry");

	/**
	 * Starts entry for a field. From is the field (or its activator): its
	 * nearest SWoTTextEntryLayer hosts the panel. A running session is
	 * cancelled first. Route forces a route (Y's panel fallback); unset picks
	 * one (ChooseRoute). False when no layer hosts the field.
	 */
	static bool Begin(const FWoTTextEntryRequest& Request, const TSharedRef<SWidget>& From,
		int32 UserIndex = 0, TOptional<EWoTTextEntryRoute> Route = TOptional<EWoTTextEntryRoute>());

	static bool IsActive();
	/** The running session's route, when one runs. */
	static TOptional<EWoTTextEntryRoute> GetActiveRoute();
	/** True while a session runs in a layer inside Ancestor (or Ancestor itself). */
	static bool IsActiveWithin(const TSharedRef<const SWidget>& Ancestor);
	/** True while a session runs for this field widget. */
	static bool IsActiveForField(const TSharedPtr<const SWidget>& Field);
	/** The open panel, if the session uses it. */
	static TSharedPtr<SWoTOnScreenKeyboard> GetPanel();
	/** The text the session would commit now: the panel's draft, or the field's. */
	static FString GetPendingText();

	/** Validates and commits. False when there is no session or Validate rejected. */
	static bool Done();
	/** Restores the snapshot and ends the session. */
	static void Cancel();
	/** Escape on a host: cancels a session inside Ancestor. True when it did. */
	static bool CancelWithin(const TSharedRef<const SWidget>& Ancestor);
	/** Ends a session inside Ancestor without calling into the request: its
	 * host is going away (chat closing). */
	static void AbandonWithin(const TSharedRef<const SWidget>& Ancestor);
	/** Y during Steam's route: our panel takes over with the text so far. */
	static bool SwitchToPanel(const FText& Message = FText::GetEmpty());
	/** Puts focus back where the session types: the panel, or on Steam's
	 * route the field (re-resolved, so a rebuilt field gets it). */
	static void RestoreFocus();

	/** The prompt strip's set while a session runs; empty otherwise. */
	static TArray<FWoTButtonPrompt> ControllerPrompts();

	// --- The Steam route -----------------------------------------------------

	/** The SteamDeck=1 environment variable (through the environment seam). */
	static bool IsSteamDeck();
	/** Steam's keyboard can be opened: under Wine (Proton) with SteamDeck=1. */
	static bool IsSteamKeyboardAvailable();
	/** The toggle for a player who never set it: on (the owner's Steam Deck
	 * session, 2026-11-09). */
	static constexpr bool SteamKeyboardDefault = true;
	/** The player's toggle: their stored choice, else SteamKeyboardDefault. */
	static bool IsSteamKeyboardEnabled();
	/** Stores the toggle in its side file. False when it could not be saved. */
	static bool SetSteamKeyboardEnabled(bool bEnabled);
	/** Steam's keyboard when available and enabled, else the panel. */
	static EWoTTextEntryRoute ChooseRoute();
	/** The side file's one ASCII line: the stored choice, or unset when the
	 * file is empty or invalid (no explicit choice, so the default applies). */
	static TOptional<bool> ParseToggle(TConstArrayView<uint8> Bytes);
	static FString FormatToggle(bool bEnabled);
	/** The last error the launcher reported (empty when it opened). */
	static FString GetLastLaunchError();

	/** Ends a session hosted by a layer being destroyed (no request calls). */
	static void NotifyLayerDestroyed(const SWoTTextEntryLayer* Layer);
	/** Per-frame upkeep from the hosting layer: the focus-loss rule and the
	 * field going away. */
	static void TickLayer(const SWoTTextEntryLayer& Layer);

#if WITH_AUTOMATION_TESTS
	/** Opens a URL and reports any error. Specs never launch a URL. */
	using FLauncher = TFunction<bool(const FString& Url, FString& OutError)>;
	static void SetLauncherForTesting(FLauncher Launcher);
	static void SetSteamDeckEnvironmentForTesting(TOptional<bool> bSteamDeck);
	/** Bypasses the side file; unset reads it again. */
	static void SetSteamKeyboardEnabledForTesting(TOptional<bool> bEnabled);
	/** Ends any session without request calls and clears every seam. */
	static void ResetForTesting();
#endif
};
