#pragma once

#include "CoreMinimal.h"
#include "Settings/WoTGameSettings.h"
#include "UI/SWoTButtonPrompts.h"
#include "UI/WoTTextEntry.h"
#include "WoTFrontEnd.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SCompoundWidget.h"

class AWoTPlayerController;
class AWoTInventory;
class FWoTBackStackEntryHandle;
class FWoTCursorLease;
class FWoTFrontEndPresentationAuthority;
class SBox;
class SConstraintCanvas;
class SEditableText;
class SWoTMenuButton;
class SWoTSettingsScreen;
class SWoTTextEntryLayer;
class SWoTTextFieldActivator;
class UTexture2D;
class UWoTPreferencesSubsystem;
struct FWoTRequiredMenuFonts;
enum class EWoTSettingsPage : uint8;

// Engine.Console Typing-state projection: a bottom-screen prompt and focused
// editable line. The controller owns lifetime and command parsing; this widget
// never exposes the UE console.
//
// Talk and TeamTalk's "Say " / "TeamSay " prefill shows as a "Say:" or
// "Team say:" label before the typed text (owner ruling 2026-11-08); the line
// is unchanged: GetText is the prefix plus the typed text, and Backspace on an
// empty line turns the label back into editable text, as retail's Backspace
// over the prefill did. A controller's A on the line opens the keyboard panel
// (spec r3 section 3).
class SWoTChatInputOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTChatInputOverlay) {}
		SLATE_ARGUMENT(AWoTPlayerController*, PlayerController)
		SLATE_ARGUMENT(FString, InitialText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	TSharedPtr<SWidget> TextWidget() const;
	/** The whole line: the label's prefix plus the typed text. */
	FString GetText() const;
	void SetText(const FString& Text);
	/** The visible label: "Say:", "Team say:", or empty for a bare line. */
	FText GetLabel() const;
	/** Opens the keyboard panel (or Steam's keyboard) for the line, as A does. */
	bool BeginTextEntry(int32 UserIndex = 0);
	TSharedPtr<SWoTTextEntryLayer> TextEntryLayer() const { return EntryLayer; }
#if WITH_AUTOMATION_TESTS
	TSharedPtr<SWoTButtonPrompts> ButtonPromptsForTesting() const { return ButtonPrompts; }
#endif

private:
	FReply HandleKey(const FKey& Key);
	/** The typed text after the label's prefix. */
	FString TypedText() const;
	FWoTTextEntryRequest MakeEntryRequest();
	void RegisterControllerBack();
	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TSharedPtr<SEditableText> Input;
	TSharedPtr<SWoTTextFieldActivator> Activator;
	TSharedPtr<SWoTTextEntryLayer> EntryLayer;
	TSharedPtr<SWoTButtonPrompts> ButtonPrompts;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
	/** B/View on the bare chat line close chat (the keyboard closes first). */
	TSharedPtr<FWoTBackStackEntryHandle> BackStackEntry;
	/** "Say ", "TeamSay " or empty: shown as the label, sent with the line. */
	FString Prefix;
};

/**
 * The in-game menu's entries. The menu shows them in this order, with Edit
 * Citadel only during a citadel edit phase; Chat, Team Chat and Taunts sit
 * after Help and before any onboarding seam rows and Quit (spec r3 section 4).
 * Positions come from the runtime list, never from these values.
 */
enum class EWoTInGameMenuEntry : uint8
{
	Resume, Scoreboard, Help, Chat, TeamChat, Taunts, EditCitadel, PlayerConfig,
	Controls, Hardware, Quit
};

// Controller-owned, code-only gameplay overlay. It never owns session or world
// lifetime; the gameplay controller removes it before EndPlay completes.
class SWoTInGameMenuOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTInGameMenuOverlay)
		: _Preferences(nullptr)
	{}
		SLATE_ARGUMENT(AWoTPlayerController*, PlayerController)
		SLATE_ARGUMENT(UWoTPreferencesSubsystem*, Preferences)
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<const FWoTRequiredMenuFonts>, RequiredFontsForTesting)
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	TSharedPtr<SWidget> InitialFocusWidget() const;
	void FocusResumeButton();
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual void Tick(
		const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;
	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FNavigationReply OnNavigation(
		const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;

#if WITH_AUTOMATION_TESTS
	void OpenSettingsForTesting(EWoTSettingsPage Page);
	bool IsShowingMainMenuForTesting() const;
	TSharedPtr<SWoTSettingsScreen> SettingsScreenForTesting() const
		{ return ActiveSettingsScreen; }
	TArray<FString> MenuLabelsForTesting() const;
	FSlateFontInfo HeadingFontForTesting() const;
	FSlateFontInfo ItemFontForTesting() const;
	float MainMenuDesiredHeightForTesting() const;
	int32 RenderedMainMenuEntryCountForTesting() const
		{ return MainMenuEntryIds.Num(); }
	bool IsUsingFallbackFontsForTesting() const
		{ return bUsingFallbackFonts; }
	FReply ResumeForTesting() { return Resume(); }
	void SetGameSettingsForTesting(const FWoTGameSettings& Settings)
		{ GameSettingsForTesting = Settings; ReturnToMainMenu(); }
	FReply ActivateScoreboardForTesting();
	FReply ActivateHelpForTesting();
	FReply ActivateCitadelForTesting();
	FReply OpenPlayerConfigForTesting() { return OpenPlayerConfig(); }
	void SetPendingPlayerConfigForTesting(const FString& Name,
		EWoTPlayerSkinColor Color)
	{
		PendingPlayerName = Name;
		PendingSkinColor = Color;
	}
	FReply SavePlayerConfigForTesting() { return SavePlayerConfig(); }
	FReply ActivateVisibleMainMenuEntryForTesting(int32 EntryIndex);
	TSharedPtr<SWoTMenuButton> MainMenuButtonForTesting(int32 EntryIndex) const
		{ return MainMenuButtons.IsValidIndex(EntryIndex)
			? MainMenuButtons[EntryIndex] : nullptr; }
	int32 SelectedMainMenuIndexForTesting() const
		{ return SelectedMainMenuIndex; }
	/** The entry's position in the rendered menu, or INDEX_NONE. */
	int32 MainMenuIndexForTesting(EWoTInGameMenuEntry Entry) const
		{ return MainMenuEntryIds.IndexOfByKey(static_cast<uint8>(Entry)); }
	FReply OpenTauntsForTesting() { return OpenTaunts(); }
	bool IsShowingTauntsForTesting() const { return bShowingTaunts; }
	TArray<FString> TauntLabelsForTesting() const { return TauntLabels; }
	TSharedPtr<SWoTMenuButton> TauntButtonForTesting(int32 EntryIndex) const
		{ return TauntButtons.IsValidIndex(EntryIndex)
			? TauntButtons[EntryIndex] : nullptr; }
	int32 SelectedTauntIndexForTesting() const { return SelectedTauntIndex; }
	/** Runs a Taunts page entry's action, as A on it does. */
	FReply ActivateTauntEntryForTesting(int32 EntryIndex);
	TSharedPtr<SWoTButtonPrompts> ButtonPromptsForTesting() const { return ButtonPrompts; }
	TArray<FWoTButtonPrompt> ControllerPromptsForTesting() const { return ControllerPrompts(); }
	TSharedPtr<SWidget> PlayerNameInputForTesting() const;
	const FString& PendingPlayerNameForTesting() const { return PendingPlayerName; }
#endif

private:
	TSharedRef<SWidget> BuildMainMenu();
	TSharedRef<SWidget> BuildSettingsPage(EWoTSettingsPage Page);
	TSharedRef<SWidget> BuildPlayerConfigPage();
	TSharedRef<SWidget> BuildTauntsPage();
	FReply OpenSettings(EWoTSettingsPage Page);
	FReply OpenPlayerConfig();
	/** The Taunts page: Taunt 1..6 and Random (spec r3 section 4). */
	FReply OpenTaunts();
	/** Chat or Team Chat: the menu closes and chat opens (spec r3 section 4). */
	FReply ActivateChat(bool bTeam);
	/** A Taunts page entry: the menu closes and PlayTaunt(TauntIndex) runs. */
	FReply ActivateTaunt(int32 TauntIndex);
	bool MoveTauntSelection(int32 Direction);
	void SelectTauntEntry(int32 EntryIndex);
	FReply SavePlayerConfig();
	void ReturnToMainMenu();
	FReply Resume();
	/** Escape, B and View: leave the page, else resume. Always handled. */
	bool BackOneLevel();
	FReply ActivateScoreboard();
	FReply ActivateHelp();
	FReply ActivateCitadel();
	FReply ActivateMainMenuEntryById(uint8 EntryId);
	bool MoveMainMenuSelection(int32 Direction);
	bool IsShowingMainMenu() const;
	void SelectMainMenuEntry(int32 EntryIndex);
	FReply LeaveToMenu();
	bool CanInteract() const;
	bool IsHelpKey(const FKey& Key) const;
	int32 MainMenuEntryCount() const;
	FMargin EntryPadding(int32 EntryIndex) const;
	FSlateFontInfo HeadingFont() const;
	FSlateFontInfo ItemFont() const;
	FText ResolveRetailText(FName Id) const;
	bool HasUsableFonts() const;
	/** The prompt strip's set for the page showing now (spec r3 section 1a). */
	TArray<FWoTButtonPrompt> ControllerPrompts() const;

	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TWeakObjectPtr<UWoTPreferencesSubsystem> Preferences;
	TSharedPtr<SWoTMenuButton> ResumeButton;
	TArray<TSharedPtr<SWoTMenuButton>> MainMenuButtons;
	TArray<uint8> MainMenuEntryIds;
	TArray<FString> MainMenuLabels;
	TSharedPtr<SBox> ContentHost;
	TSharedPtr<SWoTSettingsScreen> ActiveSettingsScreen;
	TSharedPtr<SEditableText> PlayerNameInput;
	/** Hosts the keyboard panel over every page (spec r3 section 3). */
	TSharedPtr<SWoTTextEntryLayer> TextEntryLayer;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
	/** B/View: as Escape on every page (spec r3 section 1a, in-game menu). */
	TSharedPtr<FWoTBackStackEntryHandle> BackStackEntry;
	TSharedPtr<FWoTCursorLease> CursorLease;
	TSharedPtr<SWoTButtonPrompts> ButtonPrompts;
	bool bUsingFallbackFonts = false;
	float LiveViewportScale = 1.0f;
	float EntryHeight = 0.0f;
	float EntrySpacing = 0.0f;
	float ListHeight = 0.0f;
	int32 SelectedMainMenuIndex = INDEX_NONE;
	FString PendingPlayerName;
	EWoTPlayerSkinColor PendingSkinColor = EWoTPlayerSkinColor::Base;
	bool bShowingPlayerConfig = false;
	/** The Taunts page is showing; its seven entries, one highlighted. */
	bool bShowingTaunts = false;
	TArray<TSharedPtr<SWoTMenuButton>> TauntButtons;
	TArray<FString> TauntLabels;
	int32 SelectedTauntIndex = INDEX_NONE;
#if WITH_AUTOMATION_TESTS
	TOptional<FWoTGameSettings> GameSettingsForTesting;
#endif
};

/** One rendered line after applying WOTTextWindow's inline command stream. */
struct FWoTHelpLayoutLine
{
	FString Text;
	int32 FontIndex = 3;
	bool bCentered = true;
	float X = 0.0f;
	float Y = 0.0f;
	float Width = 0.0f;
	float Height = 0.0f;
};

/** One Help page in the window's 512x432 text clip, in window units: the
 * title on top, the content lines, and the subtitle below them. */
struct FWoTHelpPageLayout
{
	FVector2D TitleSize = FVector2D::ZeroVector;
	TArray<FWoTHelpLayoutLine> Lines;
	float SubTitleY = 0.0f;
	FVector2D SubTitleSize = FVector2D::ZeroVector;

	/** The title, every line and the subtitle lie inside the clip. */
	bool FitsClip() const;
};

/** Runtime-only projection of the purchaser's localized [WOTHelpInfo] data. */
struct FWoTHelpContent
{
	// uiWindow: 640x480 less WindowBorderX (64) and WindowBorderY (24) each side.
	static constexpr float ClipWidth = 512.0f;
	static constexpr float ClipHeight = 432.0f;

	FString Title;
	FString SubTitle;
	TArray<FString> Content;

	static bool ParseRetailInt(TConstArrayView<uint8> Bytes,
		FWoTHelpContent& Out);
	static bool LoadRuntime(FWoTHelpContent& Out);
	/** The retail face a %f= font index draws in (WOTTextWindow.GetStrFont),
	 * with one legibility change: indices 0 and 1, the 8-pixel F_WOTReg08,
	 * draw in F_WOTReg14 whenever the whole-number PixelScale would put them
	 * under FWoTMenuStyle::MinimumLegiblePixels: at 1x, on any screen narrower
	 * than 1280 or shorter than 960 pixels. */
	static FName FaceFor(int32 FontIndex, int32 PixelScale);
	/** The retail bitmap line height of a Help face (WOT.u), in window units. */
	static int32 RetailLineHeight(FName Face);
	/** A line's size in window units from the retail face metrics: the face's
	 * line height and a rough advance per character. */
	static FVector2D MeasureRetail(const FString& Text, int32 FontIndex,
		int32 PixelScale);
	/** Lines laid out with MeasureRetail at PixelScale. */
	static TArray<FWoTHelpLayoutLine> Layout(const FWoTHelpContent& In,
		int32 PixelScale = 1);
	static TArray<FWoTHelpLayoutLine> Layout(const FWoTHelpContent& In,
		const TFunction<FVector2D(const FString&, int32)>& Measure);
	/** The whole page as SWoTHelpOverlay places it. */
	static FWoTHelpPageLayout LayoutPage(const FWoTHelpContent& In,
		const TFunction<FVector2D(const FString&, int32)>& Measure);
	static FWoTHelpPageLayout LayoutPage(const FWoTHelpContent& In,
		int32 PixelScale);
};

// WOTHelpWindow/WOTTextWindow's fixed 640x480 code-only projection. Content
// remains runtime retail .int data; this widget only interprets its commands.
class SWoTHelpOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTHelpOverlay) {}
		SLATE_ARGUMENT(FWoTHelpContent, Content)
		SLATE_EVENT(FSimpleDelegate, OnClose)
		SLATE_ARGUMENT(FKey, PrimaryCloseKey)
		SLATE_ARGUMENT(FKey, SecondaryCloseKey)
		/** The viewport the window opens in, in pixels, and its UI DPI scale, so
		 * the first frame already draws at the whole-number scale (Tick keeps it
		 * current across resizes). Zero size: 1x until the first Tick. */
		SLATE_ARGUMENT(FVector2D, InitialPhysicalSize)
		SLATE_ARGUMENT_DEFAULT(float, InitialDpiScale) = 1.0f;
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<const FWoTRequiredMenuFonts>, RequiredFontsForTesting)
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry,
		const double InCurrentTime, const float InDeltaTime) override;
#if WITH_AUTOMATION_TESTS
	int32 PixelScaleForTesting() const { return PixelScale; }
	float WindowScaleForTesting() const { return WindowScale(); }
	TSharedPtr<SWoTButtonPrompts> ButtonPromptsForTesting() const { return ButtonPrompts; }
	/** The page as the canvas currently places it, measured in its own fonts. */
	const FWoTHelpPageLayout& PageLayoutForTesting() const { return PageLayout; }
	FSlateFontInfo FontForIndexForTesting(int32 FontIndex) const
		{ return FontForIndex(FontIndex); }
#endif

	/** The whole-number scale the 640x480 window draws at on a screen of
	 * PhysicalSize pixels: the largest that fits, clamped to the bitmap
	 * strikes (1x, 2x) so every glyph lands 1:1 on screen pixels. */
	static int32 PixelScaleFor(const FVector2D& PhysicalSize);

private:
	FReply Close();
	FSlateFontInfo FontForIndex(int32 FontIndex) const;
	/** Lays the page out in the fonts of the current PixelScale and places it
	 * on the canvas. A scale change can swap a face (FWoTHelpContent::FaceFor),
	 * so Tick lays the page out again whenever the scale changes. */
	void RebuildPage();
	TSharedPtr<FWoTBackStackEntryHandle> BackStackEntry;
	TSharedPtr<SWoTButtonPrompts> ButtonPrompts;
	TSharedPtr<SConstraintCanvas> Canvas;
	FWoTHelpPageLayout PageLayout;
	float WindowScale() const { return PixelScale / FMath::Max(DpiScale, 0.01f); }
	int32 PixelScale = 1;
	int32 PageLayoutPixelScale = 0;
	float DpiScale = 1.0f;
	FWoTHelpContent HelpContent;
	FSimpleDelegate OnClose;
	FKey PrimaryCloseKey;
	FKey SecondaryCloseKey;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
};

struct FWoTItemInfoElement
{
	FString Label;
	bool bActive = false;
};

// Pure projection of InventoryInfoWindow.uc's retail content. The code-only
// Slate host uses the shared menuWOT chrome and fonts.
struct FWoTItemInfoContent
{
	FString Title;
	TSoftObjectPtr<UTexture2D> Icon;
	FString Rarity;
	FString Charges;
	TArray<FWoTItemInfoElement> Elements;
	FString Description;
	FString Quote;
	bool bAngreal = false;

	static FWoTItemInfoContent FromItem(const AWoTInventory* Item);
};

class SWoTItemInfoOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTItemInfoOverlay) {}
		SLATE_ARGUMENT(AWoTPlayerController*, PlayerController)
		SLATE_ARGUMENT(AWoTInventory*, Item)
		/** The viewport the overlay opens in, in pixels, so the first frame
		 * already sizes the element gems for its DPI scale (Tick keeps it
		 * current across resizes). Zero size: 1080p sizes until the first Tick. */
		SLATE_ARGUMENT(FVector2D, InitialPhysicalSize)
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<const FWoTRequiredMenuFonts>, RequiredFontsForTesting)
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry,
		const double InCurrentTime, const float InDeltaTime) override;
	static EVisibility VisibilityForFonts(bool bFontsValid);
#if WITH_AUTOMATION_TESTS
	float DpiScaleForTesting() const { return DpiScale; }
#endif

private:
	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TWeakObjectPtr<AWoTInventory> Item;
	FWoTItemInfoContent Content;
	FSlateBrush IconBrush;
	/** The viewport's UI DPI scale; the element gems' small face is floored
	 * against it (FWoTMenuStyle::LegibleSmallFont). */
	float DpiScale = 1.0f;
};
