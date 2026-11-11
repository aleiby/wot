#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "UI/WoTTextEntry.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SButton;
class SScaleBox;

/**
 * The project's QWERTY panel (spec r3 section 3). It edits a draft and never
 * touches the field: Done and Cancel go to the owner (FWoTTextEntry), which
 * validates, sets the text and commits.
 *
 * Rows: 1-0; q-p; a-l; z-m plus . - _ :; Shift, Space, Backspace and Done.
 * The panel holds keyboard focus and draws its own highlighted key (a gold
 * outline in Controller mode). The D-pad, stick and arrows move the highlight
 * with wrap (Up and Down land on the nearest key of the next row).
 *
 * The buttons follow the owner's Steam Deck ruling (2026-11-09), which
 * replaces spec r3's map: A types the highlighted key, B cancels, X is
 * Backspace, Y is Space and Menu is Done. View does nothing, and Shift has no
 * button: it is the on-screen Shift key, pressed with A. Held A and X repeat
 * on the navigation clock (0.35 s, then every 0.08 s); XInput's own repeats
 * are ignored [N5]. Physical typing edits the draft, Backspace deletes, Enter
 * is Done and Escape cancels. Taps and clicks on a key type it.
 *
 * The panel is drawn at PanelScale times its first size (keys, gaps and
 * padding; the owner found it too small on the Deck), and its key labels are
 * at least MinimumKeyLabelSize points. The draft's preview wraps inside the
 * panel's fixed width, so a long draft stays visible in full.
 */
class WOT_API SWoTOnScreenKeyboard : public SCompoundWidget
{
public:
	enum class EKeyKind : uint8
	{
		Character,
		Shift,
		Space,
		Backspace,
		Done
	};

	struct FKeyCap
	{
		EKeyKind Kind = EKeyKind::Character;
		/** The lower-case character a Character key types. */
		TCHAR Character = 0;
		/** Width in key units. */
		float Width = 1.0f;
	};

	SLATE_BEGIN_ARGS(SWoTOnScreenKeyboard)
		: _MaxLength(0)
		, _IsPassword(false)
		, _Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16))
	{}
		SLATE_ATTRIBUTE(FText, Title)
		SLATE_ARGUMENT(FString, InitialText)
		/** 0 is unbounded. */
		SLATE_ARGUMENT(int32, MaxLength)
		SLATE_ARGUMENT(bool, IsPassword)
		SLATE_ARGUMENT(FWoTTextEntryFilter, Filter)
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
		SLATE_EVENT(FSimpleDelegate, OnDone)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The key rows, top to bottom. */
	static const TArray<TArray<FKeyCap>>& Layout();
	static constexpr TCHAR MaskCharacter = TEXT('*');
	/** The panel's size against its first (PR 3) layout: twice as large. The
	 * host layer scales it down only where it would not fit the screen. */
	static constexpr float PanelScale = 2.0f;
	/** One key unit and a key's height, in layout units (the first layout's
	 * 56 x 50, past the 48 px touch target, at PanelScale). */
	static constexpr float KeyUnit = 56.0f * PanelScale;
	static constexpr float KeyHeight = 50.0f * PanelScale;
	/** Each key's slot padding on either side, in layout units. */
	static constexpr float KeyGap = 3.0f * PanelScale;
	/** The key labels' smallest size, in points: an em of 0.36 key heights,
	 * about twice the chat line's old labels. A larger host face is kept. */
	static constexpr float MinimumKeyLabelSize = 0.36f * KeyHeight * 72.0f / 96.0f;
	/** The panel's width, in layout units: its widest key row and padding. */
	static float PanelWidth();

	const FString& GetDraft() const { return Draft; }
	/** Replaces the draft (MaxLength and the filter applied). */
	void SetDraft(const FString& Text);
	/** The preview line's text: the draft, masked for passwords. */
	FString GetPreviewText() const;
	void SetMessage(const FText& InMessage) { Message = InMessage; }
	const FText& GetMessage() const { return Message; }
	bool IsShifted() const { return bShift; }
	bool IsPassword() const { return bPassword; }
	int32 GetMaxLength() const { return MaxLength; }

	FIntPoint GetHighlight() const { return FIntPoint(HighlightColumn, HighlightRow); }
	void SetHighlight(int32 Row, int32 Column);
	/** The highlighted key's label as drawn ("q", "Q", "Space", "Done"). */
	FString GetHighlightedLabel() const;
	/** The key at Row/Column, while built. */
	TSharedPtr<SWidget> GetKeyWidget(int32 Row, int32 Column) const;
	/** The row and column of the first key whose label is Label, or (-1,-1). */
	static FIntPoint FindKey(const FString& Label, bool bShifted = false);

#if WITH_AUTOMATION_TESTS
	/** The face the keys, title and preview draw in. */
	FSlateFontInfo KeyFontForTesting() const { return KeyFont(); }
	/** The draft's preview line. */
	TSharedPtr<SWidget> PreviewForTesting() const { return Preview; }
#endif

	// The panel's actions, as the buttons and keys run them.
	void ActivateHighlighted();
	void ActivateKey(int32 Row, int32 Column);
	void TypeCharacter(TCHAR Character);
	void Backspace();
	void Space() { TypeCharacter(TEXT(' ')); }
	void ToggleShift();
	void RequestDone();
	void RequestCancel();
	/** Ends a held A or X repeat. */
	void StopRepeat();
	bool IsRepeating() const { return RepeatKey.IsValid(); }
	/** Fires a due A/X repeat (Tick runs it). */
	void TickRepeat();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry,
		const FCharacterEvent& InCharacterEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry,
		const FNavigationEvent& InNavigationEvent) override;
	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

private:
	FString KeyLabel(const FKeyCap& Cap) const;
	bool IsRepeatable(const FKeyCap& Cap) const;
	void MoveHighlight(int32 ColumnStep, int32 RowStep);
	void ArmRepeat(const FKey& Key);
	FText DisplayText() const;
	FSlateFontInfo KeyFont() const;
	TSharedRef<SWidget> BuildKey(int32 Row, int32 Column);
	const FSlateBrush* KeyBrush(int32 Row, int32 Column) const;

	FString Draft;
	TAttribute<FText> Title;
	FText Message;
	int32 MaxLength = 0;
	bool bPassword = false;
	bool bShift = false;
	FWoTTextEntryFilter Filter;
	TAttribute<FSlateFontInfo> Font;
	FSimpleDelegate OnDone;
	FSimpleDelegate OnCancel;
	int32 HighlightRow = 1;
	int32 HighlightColumn = 0;
	FKey RepeatKey;
	double NextRepeatTime = 0.0;
	TArray<TArray<TSharedPtr<SButton>>> KeyButtons;
	TSharedPtr<SWidget> Preview;
};

/**
 * Hosts the keyboard panel over a screen (spec r3 section 3): a host wraps
 * its content in one, and FWoTTextEntry::Begin finds the nearest one around
 * the field. While the panel shows, a dim backdrop covers the content: a
 * press on it cancels the entry, so a stray click never reaches the screen
 * behind.
 *
 * While the panel shows, a full-width dark strip runs along the bottom of the
 * layer behind the host's prompt strip, so the screen's text never shows
 * through the prompts, and the panel sits directly above it (the owner's
 * Steam Deck session, 2026-11-09). The strip is as tall as the host's prompt
 * strip with its padding (PromptBarMargin). The panel keeps its full size
 * where it fits and is scaled down evenly only where it would not.
 * Exported for plugin windows.
 */
class WOT_API SWoTTextEntryLayer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTTextEntryLayer)
		: _PanelPadding(FMargin(16.0f, 16.0f, 16.0f, 8.0f))
		, _PromptBarMargin(FMargin(0.0f, 10.0f, 0.0f, 10.0f))
	{}
		/** The panel's face; the engine default when unbound. */
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
		/** The host's prompt strip label face; Font when unbound. */
		SLATE_ATTRIBUTE(FSlateFontInfo, PromptFont)
		/** Room around the panel in the area above the dark strip. */
		SLATE_ARGUMENT(FMargin, PanelPadding)
		/** The host's prompt strip's room above (Top) and below (Bottom) the
		 * bar; the dark strip is the bar's height plus both. */
		SLATE_ARGUMENT(FMargin, PromptBarMargin)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SWoTTextEntryLayer() override;

	void ShowPanel(const TSharedRef<SWidget>& Panel);
	void HidePanel();
	bool IsPanelShown() const { return bPanelShown; }
	FSlateFontInfo GetPanelFont() const;
	/** The dark strip's height, in layout units, at the layer's layout scale. */
	float GetBottomStripHeight() const;

	/** The dark strip's opacity: opaque enough that no text shows through. */
	static constexpr float BottomStripOpacity = 0.94f;
	/** The dark strip's colour, BottomStripOpacity included. */
	static FLinearColor BottomStripColor();

	/** The nearest layer around Widget (Widget itself included), if any. */
	static TSharedPtr<SWoTTextEntryLayer> FindFor(const TSharedPtr<SWidget>& Widget);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

#if WITH_AUTOMATION_TESTS
	TSharedPtr<SWidget> BottomStripForTesting() const { return BottomStrip; }
	/** The box that scales the panel down where it would not fit. */
	TSharedPtr<SScaleBox> PanelScaleBoxForTesting() const { return PanelScaleBox; }
#endif

private:
	TSharedPtr<SBox> PanelHost;
	TSharedPtr<SScaleBox> PanelScaleBox;
	TSharedPtr<SWidget> BottomStrip;
	TAttribute<FSlateFontInfo> Font;
	TAttribute<FSlateFontInfo> PromptFont;
	FMargin PromptBarMargin;
	bool bPanelShown = false;
};
