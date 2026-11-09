#include "WoTOnboardingDeathRecap.h"

#include "WoTAngrealGuide.h"
#include "WoTOnboardingHints.h"
#include "WoTOnboardingSettings.h"

EWoTDeathRecapVerdict WoTOnboardingDeathRecap::Decide(
	const UWoTOnboardingSettings& Settings, const UWoTAngrealGuide* Guide,
	FName ClassName, bool bScreenBusy)
{
	if (!Settings.bEnableOnboardingAids)
	{
		return EWoTDeathRecapVerdict::AidsOff;
	}
	if (!Settings.bShowDeathCounterHints)
	{
		return EWoTDeathRecapVerdict::RecapsOff;
	}
	const FWoTAngrealGuideRow* Row =
		Guide && !ClassName.IsNone() ? Guide->FindByClassName(ClassName) : nullptr;
	if (!Row)
	{
		return EWoTDeathRecapVerdict::NotInGuide;
	}
	// Whether a line renders does not depend on the titles (an unresolved
	// title falls back to the spaced class name), so none are resolved here.
	if (WoTOnboardingHints::ResolveHintLine(*Guide, *Row,
			[](const FWoTAngrealGuideRow&) { return FString(); }).IsEmpty())
	{
		return EWoTDeathRecapVerdict::NoHint;
	}
	return bScreenBusy ? EWoTDeathRecapVerdict::ScreenBusy
	                   : EWoTDeathRecapVerdict::Show;
}

FWoTPickupCardText WoTOnboardingDeathRecap::ComposeText(
	const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row,
	TFunctionRef<FString(const FWoTAngrealGuideRow&)> ResolveTitle,
	const FString& GuideKeyName)
{
	FWoTPickupCardText Text;
	const FString HintLine =
		WoTOnboardingHints::ResolveHintLine(Guide, Row, ResolveTitle);
	if (HintLine.IsEmpty())
	{
		return Text;
	}
	FString Title = ResolveTitle(Row).TrimStartAndEnd();
	if (Title.IsEmpty())
	{
		Title = WoTOnboardingHints::SpacedClassName(Row.ClassName);
	}
	Text.Title = FString::Printf(TEXT("Killed by %s"), *Title);
	Text.Summary = HintLine;
	if (!GuideKeyName.IsEmpty())
	{
		Text.KeyHint = FString::Printf(
			TEXT("Press %s for the field guide"), *GuideKeyName);
	}
	return Text;
}
