#include "UI/SWoTTextFieldActivator.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "UI/SWoTOnScreenKeyboard.h"

void SWoTTextFieldActivator::Construct(const FArguments& InArgs)
{
	Request = InArgs._Request;
	const TSharedRef<SWidget> Content = InArgs._Content.Widget;
	Field = Content;
	if (!Request.GetField)
	{
		const TWeakPtr<SWidget> WeakField = Content;
		Request.GetField = [WeakField]() { return WeakField.Pin(); };
	}
	ChildSlot
	[
		Content
	];
}

bool SWoTTextFieldActivator::BeginEntry(int32 UserIndex, TOptional<EWoTTextEntryRoute> Route)
{
	return FWoTTextEntry::Begin(Request, AsShared(), UserIndex, Route);
}

bool SWoTTextFieldActivator::IsFieldFocused(int32 UserIndex) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWidget> Content = Field.Pin();
	for (TSharedPtr<SWidget> Current =
			FSlateApplication::Get().GetUserFocusedWidget(UserIndex);
		Current.IsValid() && Content.IsValid(); Current = Current->GetParentWidget())
	{
		if (Current == Content)
		{
			return true;
		}
	}
	return false;
}

FReply SWoTTextFieldActivator::OnPreviewKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const int32 UserIndex = InKeyEvent.GetUserIndex();
	if (!IsFieldFocused(UserIndex))
	{
		return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}
	if (FWoTTextEntry::IsActiveForField(Field.Pin()))
	{
		// Steam's keyboard is typing into this field.
		if (Key == EKeys::Enter || Key == EKeys::Gamepad_Special_Right)
		{
			if (!InKeyEvent.IsRepeat())
			{
				FWoTTextEntry::Done();
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_FaceButton_Top)
		{
			if (!InKeyEvent.IsRepeat())
			{
				FWoTTextEntry::SwitchToPanel();
			}
			return FReply::Handled();
		}
		if (Key == EKeys::Escape)
		{
			FWoTTextEntry::Cancel();
			return FReply::Handled();
		}
		if (Key == EKeys::Gamepad_FaceButton_Bottom)
		{
			// Already typing; A never restarts the entry.
			return FReply::Handled();
		}
		return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}
	if (Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		// A on the focused field starts entry (spec r3 section 3); a held A's
		// own repeats never start a second one.
		if (!InKeyEvent.IsRepeat())
		{
			BeginEntry(UserIndex);
		}
		return FReply::Handled();
	}
	if (Key == EKeys::Gamepad_FaceButton_Top
		&& FWoTTextEntry::ChooseRoute() == EWoTTextEntryRoute::SteamKeyboard)
	{
		// Y: the panel instead of Steam's keyboard.
		if (!InKeyEvent.IsRepeat())
		{
			BeginEntry(UserIndex, EWoTTextEntryRoute::Panel);
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTTextFieldActivator::OnPreviewMouseButtonDown(const FGeometry& MyGeometry,
	const FPointerEvent& MouseEvent)
{
	// On the Deck a tap on a field starts entry [I8]; elsewhere a click keeps
	// placing the caret as it always has.
	const bool bPrimary = MouseEvent.IsTouchEvent()
		|| MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton;
	if (bPrimary && FWoTTextEntry::IsSteamDeck()
		&& !FWoTTextEntry::IsActiveForField(Field.Pin())
		&& BeginEntry(MouseEvent.GetUserIndex()))
	{
		return FReply::Handled();
	}
	return SCompoundWidget::OnPreviewMouseButtonDown(MyGeometry, MouseEvent);
}
