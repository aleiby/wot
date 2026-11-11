#include "UI/SWoTOnScreenKeyboard.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "Input/NavigationReply.h"
#include "UI/SWoTFocusOutline.h"
#include "UI/WoTControllerUIInput.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "WoTOnScreenKeyboard"

namespace
{
// The menus' dark charcoal, ivory and gold skin.
const FLinearColor WoTKeyboard_Ivory(0.88f, 0.84f, 0.73f, 1.0f);
const FLinearColor WoTKeyboard_Gold = FLinearColor::FromSRGBColor(FColor(240, 196, 92));
const FLinearColor WoTKeyboard_Error(1.0f, 0.45f, 0.35f, 1.0f);
// Each key's slot padding on either side, in layout units.
constexpr float WoTKeyboard_KeyGap = 3.0f;

const FSlateBrush* WoTKeyboard_PanelBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.035f, 0.035f, 0.04f, 0.96f), 6.0f,
		FLinearColor(0.48f, 0.34f, 0.13f, 1.0f), 1.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_PreviewBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f), 3.0f,
		FLinearColor(0.3f, 0.28f, 0.25f, 1.0f), 1.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_KeyBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.12f, 0.115f, 0.11f, 1.0f), 4.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_HoverBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.21f, 0.2f, 0.18f, 1.0f), 4.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_ShiftOnBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.3f, 0.23f, 0.11f, 1.0f), 4.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_HighlightBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.48f, 0.34f, 0.13f, 1.0f), 4.0f);
	return &Brush;
}

const FSlateBrush* WoTKeyboard_HighlightOutlinedBrush()
{
	// Controller mode: the project's gold focus outline on the highlighted key.
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.48f, 0.34f, 0.13f, 1.0f), 4.0f,
		WoTKeyboard_Gold, 2.0f);
	return &Brush;
}

FString WoTKeyboard_Label(const SWoTOnScreenKeyboard::FKeyCap& Cap, bool bShifted)
{
	switch (Cap.Kind)
	{
	case SWoTOnScreenKeyboard::EKeyKind::Shift: return TEXT("Shift");
	case SWoTOnScreenKeyboard::EKeyKind::Space: return TEXT("Space");
	case SWoTOnScreenKeyboard::EKeyKind::Backspace: return TEXT("Backspace");
	case SWoTOnScreenKeyboard::EKeyKind::Done: return TEXT("Done");
	case SWoTOnScreenKeyboard::EKeyKind::Character:
	default:
		return FString::Chr(bShifted ? FChar::ToUpper(Cap.Character) : Cap.Character);
	}
}

/** A key's centre across its row, in layout units, with the row centred. */
float WoTKeyboard_CenterX(int32 Row, int32 Column)
{
	const TArray<TArray<SWoTOnScreenKeyboard::FKeyCap>>& Keys = SWoTOnScreenKeyboard::Layout();
	float RowWidth = 0.0f;
	float Before = 0.0f;
	float Own = 0.0f;
	for (int32 Index = 0; Index < Keys[Row].Num(); ++Index)
	{
		const float Width = Keys[Row][Index].Width * SWoTOnScreenKeyboard::KeyUnit
			+ 2.0f * WoTKeyboard_KeyGap;
		if (Index < Column)
		{
			Before += Width;
		}
		else if (Index == Column)
		{
			Own = Width;
		}
		RowWidth += Width;
	}
	return Before + Own * 0.5f - RowWidth * 0.5f;
}
}

// ---------------------------------------------------------------------------
// The panel

const TArray<TArray<SWoTOnScreenKeyboard::FKeyCap>>& SWoTOnScreenKeyboard::Layout()
{
	static const TArray<TArray<FKeyCap>> Rows = []()
	{
		auto Characters = [](const TCHAR* Text)
		{
			TArray<FKeyCap> Row;
			for (const TCHAR* Character = Text; *Character != 0; ++Character)
			{
				FKeyCap Cap;
				Cap.Kind = EKeyKind::Character;
				Cap.Character = *Character;
				Row.Add(Cap);
			}
			return Row;
		};
		auto Special = [](EKeyKind Kind, float Width)
		{
			FKeyCap Cap;
			Cap.Kind = Kind;
			Cap.Width = Width;
			return Cap;
		};
		TArray<TArray<FKeyCap>> Result;
		Result.Add(Characters(TEXT("1234567890")));
		Result.Add(Characters(TEXT("qwertyuiop")));
		Result.Add(Characters(TEXT("asdfghjkl")));
		Result.Add(Characters(TEXT("zxcvbnm.-_:")));
		Result.Add({Special(EKeyKind::Shift, 2.0f), Special(EKeyKind::Space, 5.0f),
			Special(EKeyKind::Backspace, 2.5f), Special(EKeyKind::Done, 2.0f)});
		return Result;
	}();
	return Rows;
}

FIntPoint SWoTOnScreenKeyboard::FindKey(const FString& Label, bool bShifted)
{
	const TArray<TArray<FKeyCap>>& Keys = Layout();
	for (int32 Row = 0; Row < Keys.Num(); ++Row)
	{
		for (int32 Column = 0; Column < Keys[Row].Num(); ++Column)
		{
			if (WoTKeyboard_Label(Keys[Row][Column], bShifted).Equals(Label, ESearchCase::CaseSensitive))
			{
				return FIntPoint(Column, Row);
			}
		}
	}
	return FIntPoint(INDEX_NONE, INDEX_NONE);
}

void SWoTOnScreenKeyboard::Construct(const FArguments& InArgs)
{
	Title = InArgs._Title;
	MaxLength = FMath::Max(0, InArgs._MaxLength);
	bPassword = InArgs._IsPassword;
	Filter = InArgs._Filter;
	Font = InArgs._Font;
	OnDone = InArgs._OnDone;
	OnCancel = InArgs._OnCancel;
	SetDraft(InArgs._InitialText);
	// The highlighted key is the focus state; no outline around the panel.
	AddMetadata<FWoTSelfDrawnFocusMetaData>(MakeShared<FWoTSelfDrawnFocusMetaData>());
	// The panel opens on q (spec r3 section 1a).
	const FIntPoint Start = FindKey(TEXT("q"));
	HighlightRow = Start.Y;
	HighlightColumn = Start.X;

	const TArray<TArray<FKeyCap>>& Keys = Layout();
	KeyButtons.SetNum(Keys.Num());
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (int32 Row = 0; Row < Keys.Num(); ++Row)
	{
		TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox);
		for (int32 Column = 0; Column < Keys[Row].Num(); ++Column)
		{
			Line->AddSlot()
			.AutoWidth()
			.Padding(WoTKeyboard_KeyGap)
			[
				BuildKey(Row, Column)
			];
		}
		Rows->AddSlot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			Line
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(WoTKeyboard_PanelBrush())
		.Padding(FMargin(16.0f, 12.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(Title)
				.Font(this, &SWoTOnScreenKeyboard::KeyFont)
				.ColorAndOpacity(WoTKeyboard_Gold)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 2.0f)
			[
				SNew(SBorder)
				.BorderImage(WoTKeyboard_PreviewBrush())
				.Padding(FMargin(10.0f, 6.0f))
				[
					SNew(STextBlock)
					.Text(this, &SWoTOnScreenKeyboard::DisplayText)
					.Font(this, &SWoTOnScreenKeyboard::KeyFont)
					.ColorAndOpacity(WoTKeyboard_Ivory)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
				})
				.Text_Lambda([this]() { return Message; })
				.Font(this, &SWoTOnScreenKeyboard::KeyFont)
				.ColorAndOpacity(WoTKeyboard_Error)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				Rows
			]
		]
	];
}

TSharedRef<SWidget> SWoTOnScreenKeyboard::BuildKey(int32 Row, int32 Column)
{
	const FKeyCap& Cap = Layout()[Row][Column];
	TSharedPtr<SButton> Button;
	TSharedRef<SWidget> Key = SNew(SBox)
		.WidthOverride(Cap.Width * KeyUnit)
		.HeightOverride(KeyHeight)
		[
			SAssignNew(Button, SButton)
			.ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
			// The panel holds focus; a key is only ever pressed.
			.IsFocusable(false)
			// A mouse press types at once, hover or not (Steam's touch-as-mouse
			// may click without a move first); a touch types on release.
			.ClickMethod(EButtonClickMethod::MouseDown)
			.ContentPadding(FMargin(0.0f))
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			.OnClicked_Lambda([this, Row, Column]()
			{
				ActivateKey(Row, Column);
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.BorderImage_Lambda([this, Row, Column]() { return KeyBrush(Row, Column); })
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(this, &SWoTOnScreenKeyboard::KeyFont)
					.ColorAndOpacity_Lambda([this, Row, Column]()
					{
						return Row == HighlightRow && Column == HighlightColumn
							? FSlateColor(FLinearColor::White) : FSlateColor(WoTKeyboard_Ivory);
					})
					.Text_Lambda([this, Row, Column]()
					{
						return FText::FromString(KeyLabel(Layout()[Row][Column]));
					})
				]
			]
		];
	KeyButtons[Row].Add(Button);
	return Key;
}

const FSlateBrush* SWoTOnScreenKeyboard::KeyBrush(int32 Row, int32 Column) const
{
	if (Row == HighlightRow && Column == HighlightColumn)
	{
		return FWoTControllerUIInput::IsControllerMode()
			? WoTKeyboard_HighlightOutlinedBrush() : WoTKeyboard_HighlightBrush();
	}
	if (Layout()[Row][Column].Kind == EKeyKind::Shift && bShift)
	{
		return WoTKeyboard_ShiftOnBrush();
	}
	// Pointer hover only; a controller never draws hover (spec r3 [C1]).
	const TSharedPtr<SButton> Button = KeyButtons.IsValidIndex(Row)
		&& KeyButtons[Row].IsValidIndex(Column) ? KeyButtons[Row][Column] : nullptr;
	if (Button.IsValid() && Button->IsHovered() && !FWoTControllerUIInput::IsControllerMode())
	{
		return WoTKeyboard_HoverBrush();
	}
	return WoTKeyboard_KeyBrush();
}

FSlateFontInfo SWoTOnScreenKeyboard::KeyFont() const
{
	const FSlateFontInfo Bound = Font.Get(FSlateFontInfo());
	// An empty face would fall back to the last-resort font.
	return Bound.HasValidFont() ? Bound : FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16);
}

FString SWoTOnScreenKeyboard::KeyLabel(const FKeyCap& Cap) const
{
	return WoTKeyboard_Label(Cap, bShift);
}

bool SWoTOnScreenKeyboard::IsRepeatable(const FKeyCap& Cap) const
{
	return Cap.Kind == EKeyKind::Character || Cap.Kind == EKeyKind::Space
		|| Cap.Kind == EKeyKind::Backspace;
}

FText SWoTOnScreenKeyboard::DisplayText() const
{
	// The caret sits at the end: the panel only appends and deletes.
	return FText::FromString(GetPreviewText() + TEXT("_"));
}

FString SWoTOnScreenKeyboard::GetPreviewText() const
{
	return bPassword ? FString::ChrN(Draft.Len(), MaskCharacter) : Draft;
}

void SWoTOnScreenKeyboard::SetDraft(const FString& Text)
{
	FString Candidate = Filter ? Filter(Text) : Text;
	if (MaxLength > 0)
	{
		Candidate.LeftInline(MaxLength);
	}
	Draft = MoveTemp(Candidate);
}

void SWoTOnScreenKeyboard::SetHighlight(int32 Row, int32 Column)
{
	const TArray<TArray<FKeyCap>>& Keys = Layout();
	HighlightRow = FMath::Clamp(Row, 0, Keys.Num() - 1);
	HighlightColumn = FMath::Clamp(Column, 0, Keys[HighlightRow].Num() - 1);
}

FString SWoTOnScreenKeyboard::GetHighlightedLabel() const
{
	return KeyLabel(Layout()[HighlightRow][HighlightColumn]);
}

TSharedPtr<SWidget> SWoTOnScreenKeyboard::GetKeyWidget(int32 Row, int32 Column) const
{
	return KeyButtons.IsValidIndex(Row) && KeyButtons[Row].IsValidIndex(Column)
		? StaticCastSharedPtr<SWidget>(KeyButtons[Row][Column]) : nullptr;
}

void SWoTOnScreenKeyboard::ActivateKey(int32 Row, int32 Column)
{
	SetHighlight(Row, Column);
	ActivateHighlighted();
}

void SWoTOnScreenKeyboard::ActivateHighlighted()
{
	const FKeyCap& Cap = Layout()[HighlightRow][HighlightColumn];
	switch (Cap.Kind)
	{
	case EKeyKind::Shift:
		ToggleShift();
		break;
	case EKeyKind::Space:
		Space();
		break;
	case EKeyKind::Backspace:
		Backspace();
		break;
	case EKeyKind::Done:
		RequestDone();
		break;
	case EKeyKind::Character:
	default:
		TypeCharacter(bShift ? FChar::ToUpper(Cap.Character) : Cap.Character);
		break;
	}
}

void SWoTOnScreenKeyboard::TypeCharacter(TCHAR Character)
{
	if (MaxLength > 0 && Draft.Len() >= MaxLength)
	{
		return;
	}
	FString Candidate = Draft;
	Candidate.AppendChar(Character);
	SetDraft(Candidate);
	Message = FText::GetEmpty();
}

void SWoTOnScreenKeyboard::Backspace()
{
	if (!Draft.IsEmpty())
	{
		Draft.LeftChopInline(1);
	}
	Message = FText::GetEmpty();
}

void SWoTOnScreenKeyboard::ToggleShift()
{
	bShift = !bShift;
}

void SWoTOnScreenKeyboard::RequestDone()
{
	StopRepeat();
	// The owner may close this panel: keep it and the delegate alive.
	const TSharedRef<SWidget> KeepAlive = AsShared();
	const FSimpleDelegate Done = OnDone;
	Done.ExecuteIfBound();
}

void SWoTOnScreenKeyboard::RequestCancel()
{
	StopRepeat();
	const TSharedRef<SWidget> KeepAlive = AsShared();
	const FSimpleDelegate Cancel = OnCancel;
	Cancel.ExecuteIfBound();
}

void SWoTOnScreenKeyboard::ArmRepeat(const FKey& Key)
{
	RepeatKey = Key;
	NextRepeatTime = FWoTControllerUIInput::CurrentTime() + FWoTControllerUIInput::FirstRepeatDelay;
}

void SWoTOnScreenKeyboard::StopRepeat()
{
	RepeatKey = FKey();
	NextRepeatTime = 0.0;
}

void SWoTOnScreenKeyboard::TickRepeat()
{
	if (!RepeatKey.IsValid())
	{
		return;
	}
	const double Now = FWoTControllerUIInput::CurrentTime();
	if (Now < NextRepeatTime)
	{
		return;
	}
	NextRepeatTime += FWoTControllerUIInput::RepeatInterval;
	if (NextRepeatTime <= Now)
	{
		NextRepeatTime = Now + FWoTControllerUIInput::RepeatInterval;
	}
	if (RepeatKey == EKeys::Gamepad_FaceButton_Bottom)
	{
		// A held A types whatever is highlighted, as a held key would; it never
		// repeats Shift or Done.
		if (IsRepeatable(Layout()[HighlightRow][HighlightColumn]))
		{
			ActivateHighlighted();
		}
	}
	else if (RepeatKey == EKeys::Gamepad_FaceButton_Right)
	{
		Backspace();
	}
}

void SWoTOnScreenKeyboard::MoveHighlight(int32 ColumnStep, int32 RowStep)
{
	const TArray<TArray<FKeyCap>>& Keys = Layout();
	if (RowStep != 0)
	{
		// Up and Down land on the key nearest across, wrapping top to bottom.
		const float From = WoTKeyboard_CenterX(HighlightRow, HighlightColumn);
		const int32 Row = (HighlightRow + RowStep % Keys.Num() + Keys.Num()) % Keys.Num();
		int32 Best = 0;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 Column = 0; Column < Keys[Row].Num(); ++Column)
		{
			const float Distance = FMath::Abs(WoTKeyboard_CenterX(Row, Column) - From);
			if (Distance < BestDistance - KINDA_SMALL_NUMBER)
			{
				Best = Column;
				BestDistance = Distance;
			}
		}
		HighlightRow = Row;
		HighlightColumn = Best;
	}
	else if (ColumnStep != 0)
	{
		const int32 Count = Keys[HighlightRow].Num();
		HighlightColumn = (HighlightColumn + ColumnStep % Count + Count) % Count;
	}
}

FReply SWoTOnScreenKeyboard::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const bool bRepeat = InKeyEvent.IsRepeat();
	if (Key.IsGamepadKey())
	{
		// XInput repeats held buttons itself; the panel's own clock replaces it.
		if (Key == EKeys::Gamepad_FaceButton_Bottom)
		{
			if (!bRepeat)
			{
				const bool bRepeatable = IsRepeatable(Layout()[HighlightRow][HighlightColumn]);
				ActivateHighlighted();
				if (bRepeatable)
				{
					ArmRepeat(Key);
				}
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_FaceButton_Right)
		{
			if (!bRepeat)
			{
				Backspace();
				ArmRepeat(Key);
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_FaceButton_Left)
		{
			if (!bRepeat)
			{
				Space();
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_FaceButton_Top)
		{
			if (!bRepeat)
			{
				ToggleShift();
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_Special_Right)
		{
			if (!bRepeat)
			{
				RequestDone();
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_Special_Left)
		{
			if (!bRepeat)
			{
				RequestCancel();
			}
			return FReply::Handled();
		}
		// The D-pad and the rest become navigation.
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	// Physical typing: characters arrive through OnKeyChar.
	if (Key == EKeys::Enter)
	{
		if (!bRepeat)
		{
			RequestDone();
		}
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		RequestCancel();
		return FReply::Handled();
	}
	if (Key == EKeys::BackSpace)
	{
		// A physical key's own repeat deletes as a text box would.
		Backspace();
		return FReply::Handled();
	}
	if (FSlateApplication::IsInitialized()
		&& FSlateApplication::Get().GetNavigationDirectionFromKey(InKeyEvent) != EUINavigation::Invalid)
	{
		// The arrows move the highlight.
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	// Every other key belongs to the panel while it is open.
	return FReply::Handled();
}

FReply SWoTOnScreenKeyboard::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (RepeatKey.IsValid() && InKeyEvent.GetKey() == RepeatKey)
	{
		StopRepeat();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyUp(MyGeometry, InKeyEvent);
}

FReply SWoTOnScreenKeyboard::OnKeyChar(const FGeometry&, const FCharacterEvent& InCharacterEvent)
{
	const TCHAR Character = InCharacterEvent.GetCharacter();
	// Control characters (Backspace, Enter, Escape, Tab) are keys, not text.
	if (Character >= 0x20 && Character != 0x7f)
	{
		TypeCharacter(Character);
	}
	return FReply::Handled();
}

FNavigationReply SWoTOnScreenKeyboard::OnNavigation(const FGeometry&,
	const FNavigationEvent& InNavigationEvent)
{
	switch (InNavigationEvent.GetNavigationType())
	{
	case EUINavigation::Up:
		MoveHighlight(0, -1);
		break;
	case EUINavigation::Down:
		MoveHighlight(0, 1);
		break;
	case EUINavigation::Left:
		MoveHighlight(-1, 0);
		break;
	case EUINavigation::Right:
		MoveHighlight(1, 0);
		break;
	default:
		break;
	}
	// Focus stays on the panel whichever way the player pushes.
	return FNavigationReply::Stop();
}

void SWoTOnScreenKeyboard::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	SCompoundWidget::OnFocusLost(InFocusEvent);
	StopRepeat();
}

void SWoTOnScreenKeyboard::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	TickRepeat();
}

// ---------------------------------------------------------------------------
// The host layer

void SWoTTextEntryLayer::Construct(const FArguments& InArgs)
{
	Font = InArgs._Font;
	// Hit testing passes through to the content, as if the layer were not there.
	SetVisibility(EVisibility::SelfHitTestInvisible);
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			InArgs._Content.Widget
		]
		+ SOverlay::Slot()
		[
			// While the panel shows: dims the screen and takes every press, so a
			// stray click never reaches the controls behind; a press cancels.
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f))
			.Visibility_Lambda([this]()
			{
				return bPanelShown ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent&)
			{
				FWoTTextEntry::CancelWithin(AsShared());
				return FReply::Handled();
			})
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(InArgs._PanelPadding)
		[
			SAssignNew(PanelHost, SBox)
			.Visibility_Lambda([this]()
			{
				return bPanelShown ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
			})
		]
	];
}

SWoTTextEntryLayer::~SWoTTextEntryLayer()
{
	FWoTTextEntry::NotifyLayerDestroyed(this);
}

void SWoTTextEntryLayer::ShowPanel(const TSharedRef<SWidget>& Panel)
{
	if (PanelHost.IsValid())
	{
		PanelHost->SetContent(Panel);
		bPanelShown = true;
	}
}

void SWoTTextEntryLayer::HidePanel()
{
	if (PanelHost.IsValid())
	{
		PanelHost->SetContent(SNullWidget::NullWidget);
	}
	bPanelShown = false;
}

FSlateFontInfo SWoTTextEntryLayer::GetPanelFont() const
{
	const FSlateFontInfo Bound = Font.Get(FSlateFontInfo());
	return Bound.HasValidFont() ? Bound : FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16);
}

TSharedPtr<SWoTTextEntryLayer> SWoTTextEntryLayer::FindFor(const TSharedPtr<SWidget>& Widget)
{
	static const FName LayerType(TEXT("SWoTTextEntryLayer"));
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (Current->GetType() == LayerType)
		{
			return StaticCastSharedPtr<SWoTTextEntryLayer>(Current);
		}
	}
	return nullptr;
}

void SWoTTextEntryLayer::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	FWoTTextEntry::TickLayer(*this);
}

#undef LOCTEXT_NAMESPACE
