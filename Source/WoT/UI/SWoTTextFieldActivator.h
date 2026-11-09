#pragma once

#include "CoreMinimal.h"
#include "UI/WoTTextEntry.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Wraps one text field instance (spec r3 section 3). Mouse, touch and
 * keyboard entry stay exactly as they are, with two additions:
 *
 * - A controller's A on the focused field starts entry: Steam's keyboard on
 *   the Deck when the player turned it on, else the QWERTY panel. Y opens the
 *   panel instead of Steam's keyboard (the fallback).
 * - With SteamDeck=1, a tap or click on the field starts entry too [I8].
 *
 * While Steam's keyboard types into the field, Enter (Steam's own Enter, or
 * Menu) is Done and Y switches to the panel; B cancels through the back
 * stack. Exported so plugin windows can use it.
 */
class WOT_API SWoTTextFieldActivator : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTTextFieldActivator) {}
		/** Bound to the field's owner (its view model), so a rebuild keeps the
		 * draft. An unset GetField resolves to this activator's content. */
		SLATE_ARGUMENT(FWoTTextEntryRequest, Request)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Starts entry for this field; Route forces a route. */
	bool BeginEntry(int32 UserIndex = 0,
		TOptional<EWoTTextEntryRoute> Route = TOptional<EWoTTextEntryRoute>());
	/** The field this activator wraps (its content). */
	TSharedPtr<SWidget> GetField() const { return Field.Pin(); }
	const FWoTTextEntryRequest& GetRequest() const { return Request; }

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override;

private:
	/** True while the user's keyboard focus is in this field. */
	bool IsFieldFocused(int32 UserIndex) const;

	FWoTTextEntryRequest Request;
	TWeakPtr<SWidget> Field;
};
