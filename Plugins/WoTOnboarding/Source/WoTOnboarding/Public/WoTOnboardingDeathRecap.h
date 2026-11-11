#pragma once

#include "CoreMinimal.h"
#include "WoTOnboardingPickupCards.h"

struct FWoTAngrealGuideRow;
class UWoTAngrealGuide;
class UWoTOnboardingSettings;

// The death recap (plan Task 7): when the server names the item that killed
// the local player, a card names it and the counter the guide knows for it.
// Plain functions, testable without a viewport; the module wires them to the
// ClientKilledBy seam and shows the card where the first-pickup card goes.

/** Why a reported killing item does or does not show a recap. */
enum class EWoTDeathRecapVerdict : uint8
{
	Show,
	/** The master switch is off. */
	AidsOff,
	/** The death-recap aid is off. */
	RecapsOff,
	/** No guide, or the guide has no row for the item. */
	NotInGuide,
	/** The row has no hint line to teach: no template, or one that needs a
	    counter the guide does not know. */
	NoHint,
	/** The guide or a retail window holds the screen. A pickup card does
	    not: the recap replaces it. */
	ScreenBusy,
};

namespace WoTOnboardingDeathRecap
{
	WOTONBOARDING_API EWoTDeathRecapVerdict Decide(
		const UWoTOnboardingSettings& Settings, const UWoTAngrealGuide* Guide,
		FName ClassName, bool bScreenBusy);

	/** The recap for Row: "KILLED BY" over its title, "COUNTER" over the
	    row's hint line and the counter that hint names, and the guide key
	    line. ResolveTitle supplies display titles (a title resolving empty
	    falls back to the spaced class name); GuideKeyName is the ShowGuide
	    key's display name, or empty. All empty when the row has no hint
	    line. */
	WOTONBOARDING_API FWoTPickupCardText ComposeText(
		const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row,
		TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle,
		const FString& GuideKeyName);
}
