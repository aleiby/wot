#pragma once

#include "CoreMinimal.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Marks a focusable widget that draws its own focus state (an open
 * dropdown's list highlights its row), or a container that only holds focus
 * while it waits (a settings page capturing a key), so SWoTFocusOutlineLayer
 * leaves it alone.
 */
class WOT_API FWoTSelfDrawnFocusMetaData final : public ISlateMetaData
{
public:
	SLATE_METADATA_TYPE(FWoTSelfDrawnFocusMetaData, ISlateMetaData)
};

/**
 * The project focus outline (spec r3 section 1, "Focus outlines"). One layer
 * around a modal surface draws the outline around whichever control inside it
 * has keyboard focus: menu buttons, buttons, check boxes, text boxes, sliders,
 * dropdowns and lists alike. It draws only while the last input came from a
 * controller; in Controller mode it also turns off Slate's own dashed focus
 * rectangle, so there is one outline. In Pointer mode nothing changes: the
 * keyboard's existing cues stay exactly as they are (spec r3 [N3]).
 *
 * Layers may nest: the layer nearest the focused control draws. Exported so
 * plugin windows can use it.
 */
class WOT_API SWoTFocusOutlineLayer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTFocusOutlineLayer) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual TOptional<bool> OnQueryShowFocus(const EFocusCause InFocusCause) const override;

	/**
	 * The widget an outline surrounds while Focused has focus, or null: a
	 * focused control inside this layer (and no nearer layer), shown, and not
	 * drawing its own focus state. A text field's box is outlined rather than
	 * its inner editable text. Independent of the input mode.
	 */
	TSharedPtr<SWidget> FindOutlineTarget(const TSharedPtr<SWidget>& Focused) const;

	/** The widget the last paint outlined, or null when it drew none. */
	TSharedPtr<SWidget> GetOutlinedWidget() const { return LastOutlined.Pin(); }

	/** The outline brush, shared by the dropdown's highlighted row. */
	static const FSlateBrush* OutlineBrush();
	/** How far the outline sits outside the control, in its local units. */
	static constexpr float OutlineInflation = 3.0f;

private:
	mutable TWeakPtr<SWidget> LastOutlined;
};
