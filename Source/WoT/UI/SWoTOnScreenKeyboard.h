#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "UI/WoTTextEntry.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SButton;

/**
 * The project's QWERTY panel (spec r3 section 3). It edits a draft and never
 * touches the field: Done and Cancel go to the owner (FWoTTextEntry), which
 * validates, sets the text and commits.
 *
 * Rows: 1-0; q-p; a-l; z-m plus . - _ :; Shift, Space, Backspace and Done.
 * The panel holds keyboard focus and draws its own highlighted key (a gold
 * outline in Controller mode). The D-pad, stick and arrows move the highlight
 * with wrap (Up and Down land on the nearest key of the next row); A types
 * the highlighted key, B is Backspace, X is Space, Y toggles Shift, Menu is
 * Done and View cancels. Held A and B repeat on the navigation clock (0.35 s,
 * then every 0.08 s); XInput's own repeats are ignored [N5]. Physical typing
 * edits the draft, Backspace deletes, Enter is Done and Escape cancels. Taps
 * and clicks on a key type it.
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
	/** One key unit and a key's height, in layout units (48 px touch targets). */
	static constexpr float KeyUnit = 56.0f;
	static constexpr float KeyHeight = 50.0f;

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

	// The panel's actions, as the buttons and keys run them.
	void ActivateHighlighted();
	void ActivateKey(int32 Row, int32 Column);
	void TypeCharacter(TCHAR Character);
	void Backspace();
	void Space() { TypeCharacter(TEXT(' ')); }
	void ToggleShift();
	void RequestDone();
	void RequestCancel();
	/** Ends a held A or B repeat. */
	void StopRepeat();
	bool IsRepeating() const { return RepeatKey.IsValid(); }
	/** Fires a due A/B repeat (Tick runs it). */
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
};

/**
 * Hosts the keyboard panel over a screen (spec r3 section 3): a host wraps
 * its content in one, and FWoTTextEntry::Begin finds the nearest one around
 * the field. While the panel shows, a dim backdrop covers the content: a
 * press on it cancels the entry, so a stray click never reaches the screen
 * behind. The panel docks at the bottom, clear of the prompt strip.
 * Exported for plugin windows.
 */
class WOT_API SWoTTextEntryLayer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTTextEntryLayer)
		: _PanelPadding(FMargin(16.0f, 16.0f, 16.0f, 64.0f))
	{}
		/** The panel's face; the engine default when unbound. */
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
		SLATE_ARGUMENT(FMargin, PanelPadding)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SWoTTextEntryLayer() override;

	void ShowPanel(const TSharedRef<SWidget>& Panel);
	void HidePanel();
	bool IsPanelShown() const { return bPanelShown; }
	FSlateFontInfo GetPanelFont() const;

	/** The nearest layer around Widget (Widget itself included), if any. */
	static TSharedPtr<SWoTTextEntryLayer> FindFor(const TSharedPtr<SWidget>& Widget);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

private:
	TSharedPtr<SBox> PanelHost;
	TAttribute<FSlateFontInfo> Font;
	bool bPanelShown = false;
};
