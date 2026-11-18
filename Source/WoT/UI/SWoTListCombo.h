#pragma once

#include "CoreMinimal.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Types/SlateStructs.h"
#include "Widgets/SCompoundWidget.h"

class FWoTBackStackEntryHandle;
class ITableRow;
class SMenuAnchor;
class STableViewBase;
class SWoTListComboPopup;
class SWoTListComboRow;
template <typename ItemType> class SListView;

DECLARE_DELEGATE_RetVal_OneParam(TSharedRef<SWidget>, FOnWoTListComboGenerateItem, int32);
DECLARE_DELEGATE_TwoParams(FOnWoTListComboPicked, int32, ESelectInfo::Type);

/**
 * A touch-friendly dropdown (spec r3 section 2, "SWoTListCombo"): the map list
 * and the settings choices. Items are indices into the caller's own list, so
 * each caller keeps its data model, labels and selection callback.
 *
 * - Closed, it is one focusable control: A, Enter or a click opens it, and the
 *   D-pad and arrows move focus off it without changing the value.
 * - Open, its list pops up in the current window (an SMenuAnchor) with the
 *   selected row highlighted. Rows are at least 48 px tall. The D-pad, stick
 *   and arrows move the highlight with wrap and repeat; moving never commits.
 *   A or Enter picks the highlighted row; B or Escape closes without a change.
 *   While it is open a back-stack entry handles B and View.
 * - Mouse and touch (owner ruling 2026-11-08): a press only arms the row under
 *   it, nothing is selected or captured; a release within 8 px picks it, and
 *   moving past 8 px drags the list instead. The wheel scrolls.
 *
 * Exported so plugin windows can use it.
 */
class WOT_API SWoTListCombo : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTListCombo)
		: _ComboBoxStyle(&FCoreStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox"))
		, _ItemStyle(&FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row"))
		, _ScrollBarStyle(&FCoreStyle::Get().GetWidgetStyle<FScrollBarStyle>("ScrollBar"))
		, _MenuBorderBrush(nullptr)
		, _ForegroundColor(FSlateColor::UseForeground())
		, _MinRowHeight(48.0f)
		, _MaxVisibleRows(6.0f)
	{}
		/** How many items the caller's list holds now. */
		SLATE_ATTRIBUTE(int32, NumItems)
		/** The committed item, or INDEX_NONE. */
		SLATE_ATTRIBUTE(int32, SelectedIndex)
		/** One row's label for an item index. */
		SLATE_EVENT(FOnWoTListComboGenerateItem, OnGenerateItem)
		/** A pick that changes the committed item. */
		SLATE_EVENT(FOnWoTListComboPicked, OnPicked)
		SLATE_STYLE_ARGUMENT(FComboBoxStyle, ComboBoxStyle)
		SLATE_STYLE_ARGUMENT(FTableRowStyle, ItemStyle)
		SLATE_STYLE_ARGUMENT(FScrollBarStyle, ScrollBarStyle)
		/** The open list's frame; the combo style's own when null. A caller
		 * whose frame texture is rebound at run time passes the brush it
		 * keeps current (a copy inside a style would go stale). */
		SLATE_ARGUMENT(const FSlateBrush*, MenuBorderBrush)
		SLATE_ATTRIBUTE(FSlateColor, ForegroundColor)
		/** Each row's minimum height in screen pixels. */
		SLATE_ARGUMENT(float, MinRowHeight)
		/** The open list's height, in rows, before it scrolls. */
		SLATE_ARGUMENT(float, MaxVisibleRows)
		/** The closed control's content: the committed item's label. */
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	SWoTListCombo();
	virtual ~SWoTListCombo() override;
	void Construct(const FArguments& InArgs);

	bool IsOpen() const;
	/** Opens the list with the committed item highlighted; the list takes focus. */
	void Open(int32 UserIndex = 0);
	/** Closes the list without a change; focus inside it returns to the control. */
	void Close();
	/** The row the D-pad and keys act on while open. */
	int32 GetHighlightedIndex() const { return HighlightIndex; }
	/** The list's scroll position, in rows. */
	float GetScrollOffset() const;
	/** The open list's focusable host (it draws its own focus state). */
	TSharedPtr<SWidget> GetPopupWidget() const;
	/** The row widget generated for an item, while it is on screen. */
	TSharedPtr<SWidget> GetRowWidget(int32 Index) const;
	/** The background the item's row draws now: the highlight, pointer
	 * hover (never in Controller mode), or none. Null while not on screen. */
	const FSlateBrush* GetRowBrush(int32 Index) const;

	/**
	 * Closes an open dropdown inside Ancestor (Escape on an open dropdown
	 * closes only the dropdown: spec r3 [N1]). True when one was closed.
	 */
	static bool CloseOpenWithin(const TSharedRef<const SWidget>& Ancestor);
	/** True while a dropdown inside Ancestor is open. */
	static bool IsAnyOpenWithin(const TSharedRef<const SWidget>& Ancestor);

	/** A release within this many screen pixels of the press picks; past it drags. */
	static constexpr float DragThreshold = 8.0f;

	virtual bool SupportsKeyboardFocus() const override { return true; }
	/** The popup draws in the current window, unthrottled: a throttled popup
	 * puts Slate in responsive mode, which stops PIE worlds ticking. */
	virtual FPopupMethodReply OnQueryPopupMethod() const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override;

private:
	friend class SWoTListComboPopup;
	friend class SWoTListComboRow;
	using FItem = TSharedPtr<int32>;

	struct FPress
	{
		bool bArmed = false;
		bool bDragging = false;
		int32 Index = INDEX_NONE;
		uint32 PointerIndex = 0;
		FVector2D Start = FVector2D::ZeroVector;
		float StartOffset = 0.0f;
		float RowScreenHeight = 1.0f;
	};

	FReply OnHeaderClicked();
	void OnMenuOpenChanged(bool bOpen);
	void RefreshItems();
	TSharedRef<ITableRow> GenerateRow(FItem Item, const TSharedRef<STableViewBase>& OwnerTable);
	/** Closes the list; with bRestoreFocus, focus in the list returns here. */
	void CloseInternal(bool bRestoreFocus);
	/** The list closed by any route: drop the back-stack entry and any press. */
	void HandleClosed();
	/** Closes the list, then reports the item when it changes. */
	void Pick(int32 Index, ESelectInfo::Type SelectInfo);
	void MoveHighlight(int32 Step);
	void SyncHighlight();
	float ScreenScale() const;
	FOptionalSize RowMinHeight() const;
	FOptionalSize PopupMinWidth() const;
	FOptionalSize PopupMaxHeight() const;

	// The popup's keys and navigation.
	bool HandlePopupKey(const FKeyEvent& InKeyEvent);
	FNavigationReply HandlePopupNavigation(const FNavigationEvent& InNavigationEvent);

	// Shared by every row and the popup host: one press at a time.
	FReply HandlePress(int32 Index, const FGeometry& RowGeometry, const FPointerEvent& Event);
	FReply HandleMove(const FPointerEvent& Event);
	FReply HandleRelease(const FPointerEvent& Event);
	void CancelPress();
	void DragTo(double DeltaY);

	/** The row's look: highlight, pointer hover, or nothing. */
	const FSlateBrush* RowBrush(int32 Index, bool bHovered) const;

	TAttribute<int32> NumItems;
	TAttribute<int32> SelectedIndex;
	FOnWoTListComboGenerateItem OnGenerateItem;
	FOnWoTListComboPicked OnPicked;
	const FComboBoxStyle* ComboStyle = nullptr;
	const FTableRowStyle* ItemStyle = nullptr;
	float MinRowHeight = 48.0f;
	float MaxVisibleRows = 6.0f;

	TSharedPtr<SMenuAnchor> MenuAnchor;
	TSharedPtr<SWoTListComboPopup> Popup;
	TSharedPtr<SListView<FItem>> ListView;
	TArray<FItem> Items;
	int32 HighlightIndex = INDEX_NONE;
	FPress Press;
	TOptional<bool> bOpenAtPointerPress;
	int32 OpenUserIndex = 0;
	TSharedPtr<FWoTBackStackEntryHandle> BackStackEntry;
};
