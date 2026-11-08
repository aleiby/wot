#pragma once

#include "CoreMinimal.h"

struct FWoTAngrealGuideRow;
class UWoTAngrealGuide;

// The counter-hint resolution shared by the guide browser (Task 5) and the
// death recap (Task 7): plain functions over the guide rows, unit-testable
// without any UI. Display titles come in through a resolver callback so the
// retail-text seam stays the callers' concern.
namespace WoTOnboardingHints
{
	/** The display stand-in for a row whose retail title does not resolve:
	    the shipped class name with its registry affixes dropped and camel-case
	    boundaries spaced ("AngrealInvAirBurst" -> "Air Burst",
	    "AlarmInventory" -> "Alarm"). Derived identity, never invented prose. */
	WOTONBOARDING_API FString SpacedClassName(FName ClassName);

	/** The most-specific counter of Row among Guide.CounteredBy(Row), per the
	    plan's pinned rule: a dedicated counter (a counterer whose Counters
	    list has exactly one entry) first, then the Reflect family, then any
	    element shield. Reflect and the element shields are told apart by the
	    elements of the rows a counterer counters: an element shield's derived
	    edges all carry the shield's own element, so they share one; Reflect's
	    span the projectile roster and share none. Within a tier the first row
	    in Rows order wins (CounteredBy answers in Rows order), so the result
	    is deterministic. Null when nothing counters the row. */
	WOTONBOARDING_API const FWoTAngrealGuideRow* SelectCounter(
		const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row);

	/** Row's hint line with {title} and {counter} resolved, or empty when the
	    row carries no hint or its template wants a {counter} and nothing
	    counters the row (Balefire's template carries no {counter} and always
	    renders). ResolveTitle supplies display titles; a title resolving
	    empty falls back to SpacedClassName — never placeholder text. */
	WOTONBOARDING_API FString ResolveHintLine(const UWoTAngrealGuide& Guide,
		const FWoTAngrealGuideRow& Row,
		TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle);
}
