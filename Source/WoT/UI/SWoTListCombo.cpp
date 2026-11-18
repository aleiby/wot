#include "UI/SWoTListCombo.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "Input/NavigationReply.h"
#include "Rendering/DrawElements.h"
#include "UI/SWoTFocusOutline.h"
#include "UI/WoTBackStack.h"
#include "UI/WoTControllerUIInput.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

namespace
{
/** Every open dropdown, so Escape and the prompt strip can find them. */
TArray<TWeakPtr<SWoTListCombo>>& WoTListCombo_Open()
{
	static TArray<TWeakPtr<SWoTListCombo>> Open;
	return Open;
}

bool WoTListCombo_IsWithin(const TSharedRef<SWoTListCombo>& Combo, const SWidget& Ancestor)
{
	for (TSharedPtr<SWidget> Current = Combo; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (Current.Get() == &Ancestor)
		{
			return true;
		}
	}
	return false;
}

/** Open dropdowns inside Ancestor, most recently opened first. */
TArray<TSharedRef<SWoTListCombo>> WoTListCombo_OpenWithin(const SWidget& Ancestor)
{
	TArray<TSharedRef<SWoTListCombo>> Result;
	TArray<TWeakPtr<SWoTListCombo>>& Open = WoTListCombo_Open();
	Open.RemoveAll([](const TWeakPtr<SWoTListCombo>& Weak) { return !Weak.IsValid(); });
	for (int32 Index = Open.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<SWoTListCombo> Combo = Open[Index].Pin();
		if (Combo.IsValid() && Combo->IsOpen()
			&& WoTListCombo_IsWithin(Combo.ToSharedRef(), Ancestor))
		{
			Result.Add(Combo.ToSharedRef());
		}
	}
	return Result;
}
}

// ---------------------------------------------------------------------------
// The open list's host: it holds focus, takes the keys and navigation, and
// carries a drag once a press moves past the threshold.

class SWoTListComboPopup final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTListComboPopup) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SWoTListCombo>& InCombo)
	{
		Combo = InCombo;
		// The highlighted row is the focus state; no outline around the list.
		AddMetadata<FWoTSelfDrawnFocusMetaData>(MakeShared<FWoTSelfDrawnFocusMetaData>());
		ChildSlot
		[
			InArgs._Content.Widget
		];
	}

	virtual bool SupportsKeyboardFocus() const override { return true; }

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		if (Pinned.IsValid() && Pinned->HandlePopupKey(InKeyEvent))
		{
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry,
		const FNavigationEvent& InNavigationEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandlePopupNavigation(InNavigationEvent)
			: SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
	}

	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& MouseEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleMove(MouseEvent) : FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& MouseEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleRelease(MouseEvent) : FReply::Unhandled();
	}

	virtual FReply OnTouchMoved(const FGeometry&, const FPointerEvent& InTouchEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleMove(InTouchEvent) : FReply::Unhandled();
	}

	virtual FReply OnTouchEnded(const FGeometry&, const FPointerEvent& InTouchEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleRelease(InTouchEvent) : FReply::Unhandled();
	}

	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override
	{
		SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
		if (const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin())
		{
			Pinned->CancelPress();
		}
	}

private:
	TWeakPtr<SWoTListCombo> Combo;
};

// ---------------------------------------------------------------------------
// A project row: it fully replaces the stock row's press handling, mouse and
// touch, so a press never selects or captures anything (spec r3 section 2).

class SWoTListComboRow final : public STableRow<TSharedPtr<int32>>
{
public:
	using FSuper = STableRow<TSharedPtr<int32>>;

	SLATE_BEGIN_ARGS(SWoTListComboRow)
		: _Style(&FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row"))
	{}
		SLATE_STYLE_ARGUMENT(FTableRowStyle, Style)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable,
		const TSharedRef<SWoTListCombo>& InCombo, int32 InIndex)
	{
		Combo = InCombo;
		Index = InIndex;
		FSuper::Construct(FSuper::FArguments()
			.Style(InArgs._Style)
			.ShowSelection(true)
			[
				InArgs._Content.Widget
			],
			InOwnerTable);
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			// The right button still drags the list as Slate's lists do.
			return FReply::Unhandled();
		}
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandlePress(Index, MyGeometry, MouseEvent)
			: FReply::Unhandled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		return OnMouseButtonDown(MyGeometry, MouseEvent);
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleRelease(MouseEvent) : FReply::Unhandled();
	}

	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& MouseEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleMove(MouseEvent) : FReply::Unhandled();
	}

	virtual FReply OnTouchStarted(const FGeometry& MyGeometry,
		const FPointerEvent& InTouchEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandlePress(Index, MyGeometry, InTouchEvent)
			: FReply::Unhandled();
	}

	virtual FReply OnTouchMoved(const FGeometry&, const FPointerEvent& InTouchEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleMove(InTouchEvent) : FReply::Unhandled();
	}

	virtual FReply OnTouchEnded(const FGeometry&, const FPointerEvent& InTouchEvent) override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->HandleRelease(InTouchEvent) : FReply::Unhandled();
	}

	virtual FReply OnDragDetected(const FGeometry&, const FPointerEvent&) override
	{
		// Rows are never dragged; a moving press scrolls the list instead.
		return FReply::Unhandled();
	}

	virtual const FSlateBrush* GetBorder() const override
	{
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		return Pinned.IsValid() ? Pinned->RowBrush(Index, IsHovered()) : nullptr;
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const int32 Layer = FSuper::OnPaint(Args, AllottedGeometry, MyCullingRect,
			OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
		const TSharedPtr<SWoTListCombo> Pinned = Combo.Pin();
		if (!Pinned.IsValid() || Pinned->GetHighlightedIndex() != Index
			|| !FWoTControllerUIInput::IsControllerMode())
		{
			return Layer;
		}
		// Controller mode: the highlighted row wears the project focus outline.
		const FSlateBrush* Outline = SWoTFocusOutlineLayer::OutlineBrush();
		FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1,
			AllottedGeometry.ToPaintGeometry(), Outline, ESlateDrawEffect::None,
			Outline->GetTint(InWidgetStyle));
		return Layer + 1;
	}

private:
	TWeakPtr<SWoTListCombo> Combo;
	int32 Index = INDEX_NONE;
};

// ---------------------------------------------------------------------------
// The control

SWoTListCombo::SWoTListCombo() = default;

SWoTListCombo::~SWoTListCombo() = default;

void SWoTListCombo::Construct(const FArguments& InArgs)
{
	NumItems = InArgs._NumItems;
	SelectedIndex = InArgs._SelectedIndex;
	OnGenerateItem = InArgs._OnGenerateItem;
	OnPicked = InArgs._OnPicked;
	ComboStyle = InArgs._ComboBoxStyle;
	ItemStyle = InArgs._ItemStyle;
	MinRowHeight = FMath::Max(InArgs._MinRowHeight, 1.0f);
	MaxVisibleRows = FMath::Max(InArgs._MaxVisibleRows, 1.0f);
	check(ComboStyle != nullptr && ItemStyle != nullptr);
	const FComboButtonStyle& ButtonStyle = ComboStyle->ComboButtonStyle;
	RefreshItems();

	const TSharedRef<SWoTListCombo> Self = StaticCastSharedRef<SWoTListCombo>(AsShared());
	SAssignNew(Popup, SWoTListComboPopup, Self)
	[
		SNew(SBorder)
		.BorderImage(InArgs._MenuBorderBrush != nullptr
			? InArgs._MenuBorderBrush : &ButtonStyle.MenuBorderBrush)
		.Padding(ButtonStyle.MenuBorderPadding)
		[
			SNew(SBox)
			.MinDesiredWidth(this, &SWoTListCombo::PopupMinWidth)
			.MaxDesiredHeight(this, &SWoTListCombo::PopupMaxHeight)
			[
				SAssignNew(ListView, SListView<FItem>)
				.ListItemsSource(&Items)
				.OnGenerateRow(this, &SWoTListCombo::GenerateRow)
				.SelectionMode(ESelectionMode::Single)
				.ClearSelectionOnClick(false)
				// Focus stays on the popup host, which moves the highlight.
				.IsFocusable(false)
				.HandleGamepadEvents(false)
				.HandleDirectionalNavigation(false)
				.EnableAnimatedScrolling(false)
				.ConsumeMouseWheel(EConsumeMouseWheel::Always)
				.ScrollBarStyle(InArgs._ScrollBarStyle)
			]
		]
	];

	ChildSlot
	[
		SAssignNew(MenuAnchor, SMenuAnchor)
		.Placement(MenuPlacement_ComboBox)
		// The popup method comes from OnQueryPopupMethod: the current window,
		// unthrottled (spec r3 section 2).
		.OnMenuOpenChanged(this, &SWoTListCombo::OnMenuOpenChanged)
		.MenuContent(Popup)
		[
			SNew(SButton)
			.ButtonStyle(&ButtonStyle.ButtonStyle)
			// Opens on press like Slate's own combos; the control itself, not
			// this button, holds keyboard focus.
			.ClickMethod(EButtonClickMethod::MouseDown)
			.IsFocusable(false)
			.ContentPadding(ComboStyle->ContentPadding)
			.ForegroundColor(InArgs._ForegroundColor)
			.OnClicked(this, &SWoTListCombo::OnHeaderClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					InArgs._Content.Widget
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				.Padding(ButtonStyle.DownArrowPadding)
				[
					SNew(SImage)
					.Image(&ButtonStyle.DownArrowImage)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]
	];
}

bool SWoTListCombo::IsOpen() const
{
	return MenuAnchor.IsValid() && MenuAnchor->IsOpen();
}

float SWoTListCombo::GetScrollOffset() const
{
	return ListView.IsValid() ? ListView->GetScrollOffset() : 0.0f;
}

TSharedPtr<SWidget> SWoTListCombo::GetPopupWidget() const
{
	return Popup;
}

TSharedPtr<SWidget> SWoTListCombo::GetRowWidget(int32 Index) const
{
	if (!ListView.IsValid() || !Items.IsValidIndex(Index))
	{
		return nullptr;
	}
	const TSharedPtr<ITableRow> Row = ListView->WidgetFromItem(Items[Index]);
	return Row.IsValid() ? Row->AsWidget() : TSharedPtr<SWidget>();
}

const FSlateBrush* SWoTListCombo::GetRowBrush(int32 Index) const
{
	const TSharedPtr<SWidget> Row = GetRowWidget(Index);
	return Row.IsValid() ? RowBrush(Index, Row->IsHovered()) : nullptr;
}

bool SWoTListCombo::CloseOpenWithin(const TSharedRef<const SWidget>& Ancestor)
{
	const TArray<TSharedRef<SWoTListCombo>> Open = WoTListCombo_OpenWithin(Ancestor.Get());
	if (Open.IsEmpty())
	{
		return false;
	}
	Open[0]->CloseInternal(/*bRestoreFocus*/ true);
	return true;
}

bool SWoTListCombo::IsAnyOpenWithin(const TSharedRef<const SWidget>& Ancestor)
{
	return !WoTListCombo_OpenWithin(Ancestor.Get()).IsEmpty();
}

void SWoTListCombo::RefreshItems()
{
	const int32 Count = FMath::Max(0, NumItems.Get(0));
	if (Count == Items.Num())
	{
		return;
	}
	Items.Reset(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Items.Add(MakeShared<int32>(Index));
	}
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SWoTListCombo::GenerateRow(FItem Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const int32 Index = Item.IsValid() ? *Item : INDEX_NONE;
	const TSharedRef<SWidget> Label = OnGenerateItem.IsBound() && Index != INDEX_NONE
		? OnGenerateItem.Execute(Index) : SNullWidget::NullWidget;
	const FMargin RowPadding = ComboStyle->MenuRowPadding;
	return SNew(SWoTListComboRow, OwnerTable,
			StaticCastSharedRef<SWoTListCombo>(AsShared()), Index)
		.Style(ItemStyle)
		[
			// At least 48 px tall on screen whatever the DPI scale (spec r3 [M4]).
			SNew(SBox)
			.MinDesiredHeight(this, &SWoTListCombo::RowMinHeight)
			.VAlign(VAlign_Center)
			.Padding(FMargin(RowPadding.Left + 6.0f, RowPadding.Top,
				RowPadding.Right + 6.0f, RowPadding.Bottom))
			[
				Label
			]
		];
}

float SWoTListCombo::ScreenScale() const
{
	return FMath::Max(GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale(), 0.01f);
}

FOptionalSize SWoTListCombo::RowMinHeight() const
{
	return FOptionalSize(MinRowHeight / ScreenScale());
}

FOptionalSize SWoTListCombo::PopupMinWidth() const
{
	const FMargin Border = ComboStyle->ComboButtonStyle.MenuBorderPadding;
	const float Width = GetCachedGeometry().GetLocalSize().X - Border.Left - Border.Right;
	return Width > 0.0f ? FOptionalSize(Width) : FOptionalSize();
}

FOptionalSize SWoTListCombo::PopupMaxHeight() const
{
	return FOptionalSize(MaxVisibleRows * MinRowHeight / ScreenScale());
}

void SWoTListCombo::Open(int32 UserIndex)
{
	if (IsOpen() || !MenuAnchor.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	RefreshItems();
	if (Items.IsEmpty())
	{
		return;
	}
	const int32 Selected = SelectedIndex.Get(INDEX_NONE);
	HighlightIndex = Items.IsValidIndex(Selected) ? Selected : 0;
	Press = FPress();
	OpenUserIndex = UserIndex;
	MenuAnchor->SetIsOpen(true, /*bFocusMenu*/ true, UserIndex);
	if (!MenuAnchor->IsOpen())
	{
		return;
	}
	const TSharedRef<SWoTListCombo> Self = StaticCastSharedRef<SWoTListCombo>(AsShared());
	WoTListCombo_Open().AddUnique(Self);
	// B and View close the list first (consumed; spec r3 section 1a).
	const TWeakPtr<SWoTListCombo> Weak = Self;
	BackStackEntry = FWoTBackStack::Get().Register(Self,
		[Weak]()
		{
			const TSharedPtr<SWoTListCombo> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->IsOpen();
		},
		[Weak]()
		{
			if (const TSharedPtr<SWoTListCombo> Pinned = Weak.Pin())
			{
				Pinned->CloseInternal(/*bRestoreFocus*/ true);
			}
			return true;
		});
	SyncHighlight();
}

void SWoTListCombo::Close()
{
	CloseInternal(/*bRestoreFocus*/ true);
}

void SWoTListCombo::CloseInternal(bool bRestoreFocus)
{
	if (!IsOpen())
	{
		HandleClosed();
		return;
	}
	const bool bFocusInPopup = Popup.IsValid() && Popup->HasAnyUserFocusOrFocusedDescendants();
	MenuAnchor->SetIsOpen(false);
	HandleClosed();
	if (bRestoreFocus && bFocusInPopup && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetUserFocus(OpenUserIndex, AsShared(),
			EFocusCause::SetDirectly);
	}
}

void SWoTListCombo::OnMenuOpenChanged(bool bOpen)
{
	if (!bOpen)
	{
		// Dismissed by any route: a pick, B, Escape, a click elsewhere, or
		// focus leaving the list.
		HandleClosed();
	}
}

void SWoTListCombo::HandleClosed()
{
	BackStackEntry.Reset();
	if (Press.bDragging && Popup.IsValid() && Popup->HasMouseCapture()
		&& FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().ReleaseAllPointerCapture(OpenUserIndex);
	}
	Press = FPress();
	WoTListCombo_Open().RemoveAll([this](const TWeakPtr<SWoTListCombo>& Weak)
	{
		const TSharedPtr<SWoTListCombo> Pinned = Weak.Pin();
		return !Pinned.IsValid() || Pinned.Get() == this;
	});
}

FReply SWoTListCombo::OnHeaderClicked()
{
	// A press on the header while the list is open closes it. The press may
	// already have dismissed the list by taking focus (touch clicks on
	// release), so the state at the press decides.
	const bool bWasOpen = bOpenAtPointerPress.Get(false) || IsOpen();
	bOpenAtPointerPress.Reset();
	if (bWasOpen)
	{
		CloseInternal(/*bRestoreFocus*/ true);
	}
	else
	{
		Open(OpenUserIndex);
		// Hand focus to the open list in the reply. Without a native key window
		// (a backgrounded game, a headless Mac editor) Slate otherwise focuses the
		// widget under the pointer after this handler returns, and the list's
		// lost-focus dismissal closes it again on the same click.
		if (IsOpen() && Popup.IsValid())
		{
			return FReply::Handled().SetUserFocus(Popup.ToSharedRef(), EFocusCause::Mouse);
		}
	}
	return FReply::Handled();
}

FReply SWoTListCombo::OnPreviewMouseButtonDown(const FGeometry& MyGeometry,
	const FPointerEvent& MouseEvent)
{
	bOpenAtPointerPress = IsOpen();
	OpenUserIndex = MouseEvent.GetUserIndex();
	return SCompoundWidget::OnPreviewMouseButtonDown(MyGeometry, MouseEvent);
}

FReply SWoTListCombo::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FSlateApplication::Get().GetNavigationActionFromKey(InKeyEvent)
		== EUINavigationAction::Accept)
	{
		// A held A's native repeats never reopen the list.
		if (!InKeyEvent.IsRepeat() && !IsOpen())
		{
			Open(InKeyEvent.GetUserIndex());
		}
		return FReply::Handled();
	}
	// The D-pad and arrows move focus off a closed list; they never change
	// its value (spec r3 section 2).
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

bool SWoTListCombo::HandlePopupKey(const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const EUINavigationAction Action =
		FSlateApplication::Get().GetNavigationActionFromKey(InKeyEvent);
	if (Action == EUINavigationAction::Accept)
	{
		if (!InKeyEvent.IsRepeat())
		{
			Pick(HighlightIndex, ESelectInfo::OnKeyPress);
		}
		return true;
	}
	if (Action == EUINavigationAction::Back || Key == EKeys::Escape
		|| FWoTBackStack::IsBackKey(Key))
	{
		// Closes the list only, without a change (spec r3 [N1]).
		if (!InKeyEvent.IsRepeat())
		{
			CloseInternal(/*bRestoreFocus*/ true);
		}
		return true;
	}
	return false;
}

FNavigationReply SWoTListCombo::HandlePopupNavigation(const FNavigationEvent& InNavigationEvent)
{
	switch (InNavigationEvent.GetNavigationType())
	{
	case EUINavigation::Up:
		MoveHighlight(-1);
		break;
	case EUINavigation::Down:
		MoveHighlight(1);
		break;
	default:
		break;
	}
	// Focus stays in the open list whichever way the player pushes.
	return FNavigationReply::Stop();
}

void SWoTListCombo::MoveHighlight(int32 Step)
{
	const int32 Count = Items.Num();
	if (Count == 0)
	{
		return;
	}
	HighlightIndex = Items.IsValidIndex(HighlightIndex)
		? ((HighlightIndex + Step) % Count + Count) % Count
		: (Step > 0 ? 0 : Count - 1);
	SyncHighlight();
}

void SWoTListCombo::SyncHighlight()
{
	if (!ListView.IsValid() || !Items.IsValidIndex(HighlightIndex))
	{
		return;
	}
	// The list's selection mirrors the highlight; it never commits.
	ListView->SetSelection(Items[HighlightIndex], ESelectInfo::OnNavigation);
	ListView->RequestScrollIntoView(Items[HighlightIndex], OpenUserIndex);
}

void SWoTListCombo::Pick(int32 Index, ESelectInfo::Type SelectInfo)
{
	const TSharedRef<SWidget> KeepAlive = AsShared();
	const int32 Committed = SelectedIndex.Get(INDEX_NONE);
	const bool bChanges = Items.IsValidIndex(Index) && Index != Committed;
	const FOnWoTListComboPicked Picked = OnPicked;
	CloseInternal(/*bRestoreFocus*/ true);
	if (bChanges)
	{
		// Last: the callback may rebuild the screen that owns this control.
		Picked.ExecuteIfBound(Index, SelectInfo);
	}
}

FReply SWoTListCombo::HandlePress(int32 Index, const FGeometry& RowGeometry,
	const FPointerEvent& Event)
{
	if (!IsOpen())
	{
		return FReply::Unhandled();
	}
	Press = FPress();
	Press.bArmed = true;
	Press.Index = Index;
	Press.PointerIndex = Event.GetPointerIndex();
	Press.Start = FVector2D(Event.GetScreenSpacePosition());
	Press.StartOffset = GetScrollOffset();
	Press.RowScreenHeight = FMath::Max(RowGeometry.GetAbsoluteSize().Y, 1.0f);
	// Armed only: nothing is selected or captured on press.
	return FReply::Handled();
}

FPopupMethodReply SWoTListCombo::OnQueryPopupMethod() const
{
	return FPopupMethodReply::UseMethod(EPopupMethod::UseCurrentWindow)
		.SetShouldThrottle(EShouldThrottle::No);
}

FReply SWoTListCombo::HandleMove(const FPointerEvent& Event)
{
	if (!Press.bArmed || Event.GetPointerIndex() != Press.PointerIndex)
	{
		return FReply::Unhandled();
	}
	if (!Event.IsTouchEvent() && !Event.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		// The release happened outside the list: never start a drag with no
		// button held.
		CancelPress();
		return FReply::Unhandled();
	}
	const double DeltaY = Event.GetScreenSpacePosition().Y - Press.Start.Y;
	if (!Press.bDragging)
	{
		if (FVector2D::Distance(FVector2D(Event.GetScreenSpacePosition()), Press.Start)
			<= DragThreshold)
		{
			return FReply::Handled();
		}
		// Past the threshold: the press drags the list and can no longer pick.
		Press.bDragging = true;
		DragTo(DeltaY);
		return Popup.IsValid()
			? FReply::Handled().CaptureMouse(Popup.ToSharedRef()) : FReply::Handled();
	}
	DragTo(DeltaY);
	return FReply::Handled();
}

FReply SWoTListCombo::HandleRelease(const FPointerEvent& Event)
{
	if (!Press.bArmed || Event.GetPointerIndex() != Press.PointerIndex)
	{
		return FReply::Unhandled();
	}
	const FPress Ended = Press;
	Press = FPress();
	if (Ended.bDragging)
	{
		return FReply::Handled().ReleaseMouseCapture();
	}
	if (FVector2D::Distance(FVector2D(Event.GetScreenSpacePosition()), Ended.Start)
		<= DragThreshold)
	{
		Pick(Ended.Index, ESelectInfo::OnMouseClick);
	}
	return FReply::Handled();
}

void SWoTListCombo::CancelPress()
{
	Press = FPress();
}

void SWoTListCombo::DragTo(double DeltaY)
{
	if (!ListView.IsValid())
	{
		return;
	}
	// The list follows the pointer: one row per row height moved.
	const double RowHeight = FMath::Max(static_cast<double>(Press.RowScreenHeight), 1.0);
	const double ListHeight = ListView->GetCachedGeometry().GetAbsoluteSize().Y;
	const double VisibleRows = ListHeight > 0.0 ? ListHeight / RowHeight : 0.0;
	const double MaxOffset = FMath::Max(0.0, Items.Num() - VisibleRows);
	const double Offset = FMath::Clamp(Press.StartOffset - DeltaY / RowHeight, 0.0, MaxOffset);
	ListView->SetScrollOffset(static_cast<float>(Offset));
}

const FSlateBrush* SWoTListCombo::RowBrush(int32 Index, bool bHovered) const
{
	if (Index == HighlightIndex)
	{
		return &ItemStyle->ActiveHoveredBrush;
	}
	// Pointer hover only; a controller never draws hover (spec r3 [C1]).
	if (bHovered && !FWoTControllerUIInput::IsControllerMode())
	{
		return &ItemStyle->EvenRowBackgroundHoveredBrush;
	}
	return &ItemStyle->EvenRowBackgroundBrush;
}
