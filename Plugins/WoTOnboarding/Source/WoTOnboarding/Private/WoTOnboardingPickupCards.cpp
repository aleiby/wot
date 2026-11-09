#include "WoTOnboardingPickupCards.h"

#include "WoTAngrealGuide.h"
#include "WoTOnboardingHints.h"
#include "WoTOnboardingSettings.h"

EWoTPickupCardVerdict WoTOnboardingPickupCards::Decide(
	const UWoTOnboardingSettings& Settings, const UWoTAngrealGuide* Guide,
	FName ClassName, bool bScreenBusy)
{
	if (!Settings.bEnableOnboardingAids)
	{
		return EWoTPickupCardVerdict::AidsOff;
	}
	if (!Settings.bShowFirstPickupCards)
	{
		return EWoTPickupCardVerdict::CardsOff;
	}
	if (ClassName.IsNone() || !Guide || !Guide->FindByClassName(ClassName))
	{
		return EWoTPickupCardVerdict::NotInGuide;
	}
	if (Settings.HasSeenPickupCard(ClassName))
	{
		return EWoTPickupCardVerdict::AlreadySeen;
	}
	return bScreenBusy ? EWoTPickupCardVerdict::ScreenBusy
	                   : EWoTPickupCardVerdict::Show;
}

FString WoTOnboardingPickupCards::FirstSentence(const FString& Text)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
	{
		const TCHAR Char = Trimmed[Index];
		if (Char != TEXT('.') && Char != TEXT('!') && Char != TEXT('?'))
		{
			continue;
		}
		if (Index + 1 == Trimmed.Len() || FChar::IsWhitespace(Trimmed[Index + 1]))
		{
			return Trimmed.Left(Index + 1);
		}
	}
	return Trimmed;
}

FWoTPickupCardText WoTOnboardingPickupCards::ComposeText(
	const FWoTAngrealGuideRow& Row,
	TFunctionRef<FString(const FString& Section, const FString& Property)>
		ResolveText,
	const FString& GuideKeyName)
{
	// The retail localization address: section = the shipped class name.
	const FString Section = Row.ClassName.ToString();
	FWoTPickupCardText Text;
	Text.Title = ResolveText(Section, TEXT("Title")).TrimStartAndEnd();
	if (Text.Title.IsEmpty())
	{
		Text.Title = WoTOnboardingHints::SpacedClassName(Row.ClassName);
	}
	Text.Summary = FirstSentence(ResolveText(Section, TEXT("Description")));
	if (!GuideKeyName.IsEmpty())
	{
		Text.KeyHint = FString::Printf(
			TEXT("Press %s for the field guide"), *GuideKeyName);
	}
	return Text;
}
