#include "UI/SWoTFocusOutline.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Clipping.h"
#include "Rendering/DrawElements.h"
#include "UI/WoTControllerUIInput.h"

namespace
{
const FName WoTFocusOutline_LayerType(TEXT("SWoTFocusOutlineLayer"));
const FName WoTFocusOutline_EditableText(TEXT("SEditableText"));
const FName WoTFocusOutline_EditableTextBox(TEXT("SEditableTextBox"));
const FName WoTFocusOutline_MultiLineText(TEXT("SMultiLineEditableText"));
const FName WoTFocusOutline_MultiLineTextBox(TEXT("SMultiLineEditableTextBox"));

/** The text box around an inner editable text, else the widget itself. */
TSharedPtr<SWidget> WoTFocusOutline_VisibleControl(const TSharedPtr<SWidget>& Focused)
{
	const FName Type = Focused->GetType();
	if (Type != WoTFocusOutline_EditableText && Type != WoTFocusOutline_MultiLineText)
	{
		return Focused;
	}
	TSharedPtr<SWidget> Current = Focused->GetParentWidget();
	for (int32 Depth = 0; Depth < 4 && Current.IsValid(); ++Depth)
	{
		const FName Parent = Current->GetType();
		if (Parent == WoTFocusOutline_EditableTextBox
			|| Parent == WoTFocusOutline_MultiLineTextBox)
		{
			return Current;
		}
		Current = Current->GetParentWidget();
	}
	return Focused;
}
}

void SWoTFocusOutlineLayer::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		InArgs._Content.Widget
	];
}

const FSlateBrush* SWoTFocusOutlineLayer::OutlineBrush()
{
	// Bright gold, the menus' accent: visible on the dark panels and frames.
	static const FSlateRoundedBoxBrush Brush(FLinearColor::Transparent, 4.0f,
		FLinearColor::FromSRGBColor(FColor(240, 196, 92)), 2.0f);
	return &Brush;
}

TSharedPtr<SWidget> SWoTFocusOutlineLayer::FindOutlineTarget(
	const TSharedPtr<SWidget>& Focused) const
{
	if (!Focused.IsValid() || Focused.Get() == this
		|| Focused->GetMetaData<FWoTSelfDrawnFocusMetaData>().IsValid())
	{
		return nullptr;
	}
	for (TSharedPtr<SWidget> Current = Focused; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (Current.Get() == this)
		{
			return WoTFocusOutline_VisibleControl(Focused);
		}
		if (!Current->GetVisibility().IsVisible()
			|| Current->GetType() == WoTFocusOutline_LayerType)
		{
			// Hidden, or a nearer layer draws it.
			return nullptr;
		}
	}
	// Not inside this layer.
	return nullptr;
}

TOptional<bool> SWoTFocusOutlineLayer::OnQueryShowFocus(const EFocusCause) const
{
	// Controller mode: the project outline replaces Slate's dashed rectangle.
	// Pointer mode: Slate decides, as it always has.
	return FWoTControllerUIInput::IsControllerMode() ? TOptional<bool>(false)
		: TOptional<bool>();
}

int32 SWoTFocusOutlineLayer::OnPaint(const FPaintArgs& Args,
	const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 ChildLayer = SCompoundWidget::OnPaint(Args, AllottedGeometry,
		MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	LastOutlined.Reset();
	if (!FWoTControllerUIInput::IsControllerMode() || !FSlateApplication::IsInitialized())
	{
		return ChildLayer;
	}
	const TSharedPtr<SWidget> Target =
		FindOutlineTarget(FSlateApplication::Get().GetKeyboardFocusedWidget());
	if (!Target.IsValid())
	{
		return ChildLayer;
	}
	// The children painted first, so the target's paint-space geometry and the
	// clip it was drawn under are this frame's.
	const FGeometry& TargetGeometry = Target->GetPaintSpaceGeometry();
	const FVector2f Size = TargetGeometry.GetLocalSize();
	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return ChildLayer;
	}
	const FSlateRect Culling = Target->GetPersistentState().CullingBounds;
	const FSlateRect Bounds = TargetGeometry.GetLayoutBoundingRect();
	if (!FSlateRect::DoRectanglesIntersect(Culling, Bounds))
	{
		// Scrolled out of view: not drawn this frame.
		return ChildLayer;
	}
	// Clipped like the control (a scrolled list), with room for the outline.
	const float Margin = OutlineInflation
		* TargetGeometry.GetAccumulatedLayoutTransform().GetScale() + 2.0f;
	OutDrawElements.PushClip(FSlateClippingZone(Culling.ExtendBy(FMargin(Margin))));
	const FSlateBrush* Brush = OutlineBrush();
	FSlateDrawElement::MakeBox(OutDrawElements, ChildLayer + 1,
		TargetGeometry.ToInflatedPaintGeometry(FVector2f(OutlineInflation, OutlineInflation)),
		Brush, ESlateDrawEffect::None, Brush->GetTint(InWidgetStyle));
	OutDrawElements.PopClip();
	LastOutlined = Target;
	return ChildLayer + 1;
}
