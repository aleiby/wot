#pragma once

#include "CoreMinimal.h"
#include "Settings/WoTGameSettings.h"
#include "WoTFrontEnd.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SCompoundWidget.h"

class AWoTPlayerController;
class AWoTInventory;
class FWoTFrontEndPresentationAuthority;
class SBox;
class SEditableText;
class SWoTMenuButton;
class SWoTSettingsScreen;
class UTexture2D;
class UWoTPreferencesSubsystem;
struct FWoTRequiredMenuFonts;
enum class EWoTSettingsPage : uint8;

// Engine.Console Typing-state projection: a bottom-screen prompt and focused
// editable line. The controller owns lifetime and command parsing; this widget
// never exposes the UE console.
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
	FString GetText() const;
	void SetText(const FString& Text);

private:
	FReply HandleKey(const FKey& Key);
	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TSharedPtr<SEditableText> Input;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
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
#endif

private:
	TSharedRef<SWidget> BuildMainMenu();
	TSharedRef<SWidget> BuildSettingsPage(EWoTSettingsPage Page);
	TSharedRef<SWidget> BuildPlayerConfigPage();
	FReply OpenSettings(EWoTSettingsPage Page);
	FReply OpenPlayerConfig();
	FReply SavePlayerConfig();
	void ReturnToMainMenu();
	FReply Resume();
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

	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TWeakObjectPtr<UWoTPreferencesSubsystem> Preferences;
	TSharedPtr<SWoTMenuButton> ResumeButton;
	TArray<TSharedPtr<SWoTMenuButton>> MainMenuButtons;
	TArray<uint8> MainMenuEntryIds;
	TArray<FString> MainMenuLabels;
	TSharedPtr<SBox> ContentHost;
	TSharedPtr<SWoTSettingsScreen> ActiveSettingsScreen;
	TSharedPtr<SEditableText> PlayerNameInput;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	TSharedPtr<const FWoTRequiredMenuFonts> RequiredFonts;
	bool bUsingFallbackFonts = false;
	float LiveViewportScale = 1.0f;
	float EntryHeight = 0.0f;
	float EntrySpacing = 0.0f;
	float ListHeight = 0.0f;
	int32 SelectedMainMenuIndex = INDEX_NONE;
	FString PendingPlayerName;
	EWoTPlayerSkinColor PendingSkinColor = EWoTPlayerSkinColor::Base;
	bool bShowingPlayerConfig = false;
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

/** Runtime-only projection of the purchaser's localized [WOTHelpInfo] data. */
struct FWoTHelpContent
{
	FString Title;
	FString SubTitle;
	TArray<FString> Content;

	static bool ParseRetailInt(TConstArrayView<uint8> Bytes,
		FWoTHelpContent& Out);
	static bool LoadRuntime(FWoTHelpContent& Out);
	static TArray<FWoTHelpLayoutLine> Layout(const FWoTHelpContent& In);
	static TArray<FWoTHelpLayoutLine> Layout(const FWoTHelpContent& In,
		const TFunction<FVector2D(const FString&, int32)>& Measure);
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
#endif

	/** The whole-number scale the 640x480 window draws at on a screen of
	 * PhysicalSize pixels: the largest that fits, clamped to the bitmap
	 * strikes (1x, 2x) so every glyph lands 1:1 on screen pixels. */
	static int32 PixelScaleFor(const FVector2D& PhysicalSize);

private:
	FReply Close();
	FSlateFontInfo FontForIndex(int32 FontIndex) const;
	float WindowScale() const { return PixelScale / FMath::Max(DpiScale, 0.01f); }
	int32 PixelScale = 1;
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
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<const FWoTRequiredMenuFonts>, RequiredFontsForTesting)
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	static EVisibility VisibilityForFonts(bool bFontsValid);

private:
	TWeakObjectPtr<AWoTPlayerController> PlayerController;
	TWeakObjectPtr<AWoTInventory> Item;
	FWoTItemInfoContent Content;
	FSlateBrush IconBrush;
};
