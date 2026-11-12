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

	/** The counter Row's hint names. A template names the counter its
	    sentence was written for, by class ({counter:AngrealInvFireShield}),
	    and the guide must back it: the named row exists and its Counters
	    hold Row. Null when the template names no counter, names one the
	    guide does not carry, or names one that does not counter Row. */
	WOTONBOARDING_API const FWoTAngrealGuideRow* HintCounter(
		const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row);

	/** Row's hint line with {title} and every {counter:<Class>} resolved, or
	    empty when the row carries no hint, or any counter the template names
	    is not backed by the guide (see HintCounter), or the template uses a
	    placeholder this resolver does not know (a bare {counter} included).
	    ResolveTitle supplies display titles; a title resolving empty falls
	    back to SpacedClassName, never placeholder text. */
	WOTONBOARDING_API FString ResolveHintLine(const UWoTAngrealGuide& Guide,
		const FWoTAngrealGuideRow& Row,
		TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle);
}
