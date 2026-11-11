#pragma once

#include "CoreMinimal.h"

class SWidget;

/** One row of navigable controls, left to right. */
using FWoTNavRow = TArray<TSharedPtr<SWidget>>;

/**
 * Explicit menu focus graphs (spec r3 §1, "Wrap and focus graphs"). Every
 * helper takes the menu's runtime entry list, so rows a seam adds at runtime
 * (the onboarding MenuRows and SettingsRows) are part of the graph. Links are
 * custom navigation delegates evaluated when the player navigates: an entry
 * that is collapsed, hidden or disabled at that moment is skipped, so one
 * graph covers rows that come and go (Battle-only host rows, the update
 * banner, the video Keep/Revert strip).
 *
 * Explicit links apply to every input device: the D-pad and stick, and the
 * arrow keys for keyboard players (owner ruling 2026-11-08).
 */

/**
 * True when the widget can take focus now: it is alive and focusable, and it
 * and every ancestor are visible and enabled.
 */
WOT_API bool WoTIsNavigable(const TSharedPtr<SWidget>& Widget);

/**
 * Up/Down through the entries in order, skipping entries that cannot take
 * focus, and wrapping at both ends. Left and Right are left to the widgets.
 */
WOT_API void WoTWrapVerticalNavigation(TConstArrayView<TSharedPtr<SWidget>> Entries);

/**
 * Left/Right through the entries with wrap. With bHoldVertical, Up and Down
 * keep focus where it is, so focus cannot leave a modal button row.
 */
WOT_API void WoTWrapHorizontalNavigation(TConstArrayView<TSharedPtr<SWidget>> Entries,
	bool bHoldVertical);

/**
 * Rows top to bottom. Up/Down move to the next row that has a focusable
 * control (wrapping), entering at the control nearest horizontally;
 * Left/Right move within a row and wrap.
 */
WOT_API void WoTWrapGridNavigation(TConstArrayView<FWoTNavRow> Rows);

/**
 * Two columns of rows above a footer row. Up/Down walk a column and then the
 * footer, wrapping back to the left column's top; Left/Right move within a
 * row, and past either end of a column row switch to the other column at the
 * vertically nearest row. The footer wraps on itself left to right.
 */
WOT_API void WoTTwoColumnNavigation(TConstArrayView<FWoTNavRow> LeftColumn,
	TConstArrayView<FWoTNavRow> RightColumn, const FWoTNavRow& Footer);
