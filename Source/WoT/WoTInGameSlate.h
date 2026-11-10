#pragma once

#include "CoreMinimal.h"
#include "Settings/WoTGameSettings.h"
#include "UI/SWoTButtonPrompts.h"
#include "UI/WoTTextEntry.h"
#include "WoTFrontEnd.h"
#include "Styling/SlateBrush.h"
#include "Types/SlateStructs.h"
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
//
// On Deck-class screens (FWoTMenuStyle::IsCompactScreen) the line, its labels,
// its prompts and the keyboard panel over it draw the list face at its crisp
// 2x strike, 28 physical pixels as the front end's text is, and the line's
// strip holds the prompts beside the text. From 1080p up nothing changes.
class SWoTChatInputOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTChatInputOverlay) {}
		SLATE_ARGUMENT(AWoTPlayerController*, PlayerController)
		SLATE_ARGUMENT(FString, InitialText)
		/** The viewport the line opens in, in pixels, so the first frame
		 * already uses its sizes (Tick keeps them current). */
		SLATE_ARGUMENT_DEFAULT(FVector2D, InitialPhysicalSize) = FVector2D::ZeroVector;
		/** The integer bitmap strikes; unset draws the same sizes in the
		 * native face (headless specs). */
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry,
		const double InCurrentTime, const float InDeltaTime) override;
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
	FSlateFontInfo LineFontForTesting() const { return LineFont(); }
	/** The dark strip behind the line. */
	TSharedPtr<SWidget> LineStripForTesting() const { return LineStrip; }
#endif

private:
	/** The line's face: the list face, at its 2x strike on a Deck-class screen. */
	FSlateFontInfo LineFont() const;
	/** The line strip's least content height: room for the prompt strip
	 * beside the text on a Deck-class screen while it shows; else none. */
	FOptionalSize LineStripMinHeight() const;
	/** Room kept clear at the line's right end for the prompt strip. */
	FVector2D PromptRoom() const;
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
	TSharedPtr<SWidget> LineStrip;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	/** The live layout scale and the viewport's UI DPI curve value. */
	float LayoutScale = 1.0f;
	float DpiScale = 1.0f;
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
	TSharedPtr<SWidget> MenuBackgroundForTesting() const { return MenuBackground; }
	TSharedPtr<SWoTTextEntryLayer> TextEntryLayerForTesting() const { return TextEntryLayer; }
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
	/** The main menu's room for its heading and entries, in layout units: the
	 * screen less a margin above and below and the menu's chrome. */
	float MainMenuContentRoom() const;
	/** The menu box's height limit: the room above for the main menu, and the
	 * pages' 620-unit panel within it. Unbounded for the main menu before the
	 * first Tick. */
	FOptionalSize PageMaxHeight() const;
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
	/** The screen's height in layout units (0 before the first Tick). */
	float LiveScreenHeight = 0.0f;
	/** The menu's background: it must enclose every entry, on screen. */
	TSharedPtr<SWidget> MenuBackground;
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

/** One Help page in the window's text clip, in window units: the title on
 * top, the content lines, and the subtitle below them. The retail window's
 * clip is 512x432 (FWoTHelpContent::ClipWidth/ClipHeight); the reflowed
 * full-screen page's is ClipSize. */
struct FWoTHelpPageLayout
{
	FVector2D ClipSize = FVector2D(512.0f, 432.0f);
	FVector2D TitleSize = FVector2D::ZeroVector;
	TArray<FWoTHelpLayoutLine> Lines;
	float SubTitleY = 0.0f;
	FVector2D SubTitleSize = FVector2D::ZeroVector;

	/** The title, every line and the subtitle lie inside ClipSize. */
	bool FitsClip() const;
	/** The lowest edge of the title, the lines and the subtitle. */
	float ContentBottom() const;
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
	 * under FWoTMenuStyle::FloorLegiblePixels: only at 1x, which the window
	 * now reaches only when a page cannot reflow (SWoTHelpOverlay). */
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

	/** The gap between the reflowed page's two columns, and above its trailing
	 * block, in window units. */
	static constexpr float ReflowColumnGap = 16.0f;
	static constexpr float ReflowBlockGap = 8.0f;
	/** The page reflowed into a ClipSize clip (the Steam Deck legibility
	 * pass): the title; the text before the first %f=2 heading across the
	 * full width; the headed sections in two columns, split where the taller
	 * column is shortest; the absolutely placed %y block (the F-key table)
	 * below the columns at its own %x offsets; the subtitle where retail puts
	 * it (WOTTextWindow.uc:334, against the new clip). The interpreter state
	 * carries through the sections in their retail order. A page with fewer
	 * than two headed sections stays in one column. */
	static FWoTHelpPageLayout LayoutReflow(const FWoTHelpContent& In,
		const TFunction<FVector2D(const FString&, int32)>& Measure,
		const FVector2D& ClipSize);
	/** LayoutReflow measured with MeasureRetail at PixelScale. */
	static FWoTHelpPageLayout LayoutReflow(const FWoTHelpContent& In,
		int32 PixelScale, const FVector2D& ClipSize);
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
		SLATE_ARGUMENT_DEFAULT(FVector2D, InitialPhysicalSize) = FVector2D::ZeroVector;
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
	/** Screen pixels per window unit: PixelScale, or ReflowDrawScale. */
	int32 DrawScaleForTesting() const { return DrawScale(); }
	bool IsReflowedForTesting() const { return bReflowed; }
	float WindowScaleForTesting() const { return WindowScale(); }
	TSharedPtr<SWoTButtonPrompts> ButtonPromptsForTesting() const { return ButtonPrompts; }
	/** The page as the canvas currently places it, measured in its own fonts. */
	const FWoTHelpPageLayout& PageLayoutForTesting() const { return PageLayout; }
	FSlateFontInfo FontForIndexForTesting(int32 FontIndex) const
		{ return FontForIndex(FontIndex); }
	FSlateFontInfo PromptFontForTesting() const { return PromptFont(); }
#endif

	/** The whole-number scale the 640x480 window draws at on a screen of
	 * PhysicalSize pixels: the largest that fits, clamped to the bitmap
	 * strikes (1x, 2x) so every glyph lands 1:1 on screen pixels. */
	static int32 PixelScaleFor(const FVector2D& PhysicalSize);
	/** Where the 640x480 window would draw at 1x (screens narrower than 1280
	 * or shorter than 960 pixels), Help fills the screen instead and draws at
	 * this scale, with its page reflowed (FWoTHelpContent::LayoutReflow): the
	 * Steam Deck legibility pass. Larger screens keep the retail window. */
	static constexpr int32 ReflowDrawScale = 2;
	/** The reflowed window's frame padding, in window units. */
	static constexpr float ReflowPaddingX = 16.0f;
	static constexpr float ReflowPaddingY = 8.0f;
	/** True where Help tries the full-screen reflow: PixelScaleFor is 1. */
	static bool ReflowsOn(const FVector2D& PhysicalSize);
	/** The full-screen window in its own units: the screen at ReflowDrawScale. */
	static FVector2D ReflowWindowSizeFor(const FVector2D& PhysicalSize);
	/** The reflowed page's text clip: the window less its frame padding. */
	static FVector2D ReflowClipSizeFor(const FVector2D& PhysicalSize);

private:
	FReply Close();
	FSlateFontInfo FontForIndex(int32 FontIndex) const;
	FSlateFontInfo FontForIndexAt(int32 FontIndex, int32 Scale) const;
	/** The Close prompt's label face: where the window would draw at 1x, the
	 * list face at its 2x strike, 1:1 at the live layout scale (28 physical
	 * pixels); elsewhere the window's own list face, as before. */
	FSlateFontInfo PromptFont() const;
	/** Lays the page out in the fonts of its draw scale and places it on the
	 * canvas: reflowed on a screen where the window would draw at 1x and the
	 * reflowed page fits, else the retail 640x480 page. A scale change can
	 * swap a face (FWoTHelpContent::FaceFor) and the reflowed clip follows
	 * the screen, so Tick lays the page out again when either changes. */
	void RebuildPage();
	TSharedPtr<FWoTBackStackEntryHandle> BackStackEntry;
	TSharedPtr<SWoTButtonPrompts> ButtonPrompts;
	TSharedPtr<SConstraintCanvas> Canvas;
	FWoTHelpPageLayout PageLayout;
	/** Screen pixels per window unit. */
	int32 DrawScale() const { return bReflowed ? ReflowDrawScale : PixelScale; }
	float WindowScale() const { return DrawScale() / FMath::Max(DpiScale, 0.01f); }
	FVector2D WindowSize() const;
	FMargin WindowPadding() const;
	int32 PixelScale = 1;
	int32 PageLayoutPixelScale = 0;
	FVector2D LivePhysicalSize = FVector2D::ZeroVector;
	FVector2D PageLayoutPhysicalSize = FVector2D::ZeroVector;
	bool bReflowed = false;
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

// On Deck-class screens (FWoTMenuStyle::IsCompactScreen) the window's text
// draws at the crisp whole-number strikes that reach 16 physical pixels: the
// 14-pixel faces (body, rarity, charges, element names, quote) at 2x, 28
// pixels, and the title in its own 30-pixel face. The window widens so the
// text wraps at about 1000 pixels. From 1080p up nothing changes.
class SWoTItemInfoOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTItemInfoOverlay) {}
		SLATE_ARGUMENT(AWoTPlayerController*, PlayerController)
		SLATE_ARGUMENT(AWoTInventory*, Item)
		/** The viewport the overlay opens in, in pixels, so the first frame
		 * already sizes its text for its DPI scale (Tick keeps it current
		 * across resizes). Zero size: 1080p sizes until the first Tick. */
		SLATE_ARGUMENT_DEFAULT(FVector2D, InitialPhysicalSize) = FVector2D::ZeroVector;
		/** The integer bitmap strikes; unset draws the same sizes in the
		 * native faces (headless specs). */
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthority)
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<const FWoTRequiredMenuFonts>, RequiredFontsForTesting)
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry,
		const double InCurrentTime, const float InDeltaTime) override;
	static EVisibility VisibilityForFonts(bool bFontsValid);

	/** The window's content width and text wrap on a Deck-class screen, in
	 * physical pixels (narrower where the screen is). Elsewhere the retail
	 * 720 and 600 layout units. */
	static constexpr float CompactContentPixels = 1100.0f;
	static constexpr float CompactWrapPixels = 1000.0f;
	static constexpr float RetailContentWidth = 720.0f;
	static constexpr float RetailWrapWidth = 600.0f;

#if WITH_AUTOMATION_TESTS
	float DpiScaleForTesting() const { return DpiScale; }
	FSlateFontInfo BodyFontForTesting() const { return BodyFont(); }
	FSlateFontInfo QuoteFontForTesting() const { return QuoteFont(); }
	FSlateFontInfo TitleFontForTesting() const { return TitleFont(); }
	FSlateFontInfo GemFontForTesting() const { return GemFont(); }
	float WrapWidthForTesting() const { return WrapWidth(); }
	float ContentWidthForTesting() const { return ContentWidth().Get(); }
#endif

private:
	bool IsCompact() const;
	FSlateFontInfo BodyFont() const;
	FSlateFontInfo QuoteFont() const;
	FSlateFontInfo TitleFont() const;
	FSlateFontInfo GemFont() const;
	FOptionalSize ContentWidth() const;
	float WrapWidth() const;

	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TWeakObjectPtr<AWoTInventory> Item;
	FWoTItemInfoContent Content;
	FSlateBrush IconBrush;
	TSharedPtr<const FWoTRequiredMenuFonts> MenuFonts;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	/** The viewport's UI DPI scale; desktop's element gems' small face is
	 * floored against it (FWoTMenuStyle::LegibleSmallFont). */
	float DpiScale = 1.0f;
	/** The live layout scale, and the viewport's width in layout units. */
	float LayoutScale = 1.0f;
	float ViewportWidthUnits = 0.0f;
};
