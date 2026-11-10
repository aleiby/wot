#pragma once

#include "CoreMinimal.h"

struct FWoTAngrealGuideRow;
class UWoTAngrealGuide;
class UWoTOnboardingSettings;

// The first-pickup card's decision and text (plan Task 6): plain functions,
// testable without a viewport. The module wires them to the ClientItemAdded
// seam and hosts the card widget.

/** Why a first add of an item class does or does not show its card. */
enum class EWoTPickupCardVerdict : uint8
{
	Show,
	/** The master switch is off. */
	AidsOff,
	/** The pickup-card aid is off. */
	CardsOff,
	/** No guide, or the guide has no row for the class. */
	NotInGuide,
	/** This class's card has shown before (once per profile). */
	AlreadySeen,
	/** A card, the guide, or a retail window holds the screen. The class
	    stays unseen, so a later first add (the next life's inventory, say)
	    shows it instead. */
	ScreenBusy,
};

/** What a card says, and whose icons it shows. Every part is runtime data
    or our own wording. A death recap fills the kickers and the counter; a
    first-pickup card leaves them empty. */
struct FWoTPickupCardText
{
	/** The item the card is about; its icon leads the card. */
	FName ItemClass;
	/** A label over the title ("KILLED BY" on a recap). */
	FString Kicker;
	/** The resolved retail title, or the spaced class name without it. */
	FString Title;
	/** The counter a recap's hint names; its icon leads the hint. None
	    when the hint names no counter, and on a pickup card. */
	FName CounterClass;
	/** A label over the summary ("COUNTER" on a recap). */
	FString SummaryKicker;
	/** A pickup card's first sentence of the resolved retail description,
	    empty when the description does not resolve (never placeholder
	    text); a recap's hint line. */
	FString Summary;
	/** "<key>  Field Guide", from the live ShowGuide binding; empty when
	    the action has no key. */
	FString KeyHint;
};

namespace WoTOnboardingPickupCards
{
	WOTONBOARDING_API EWoTPickupCardVerdict Decide(
		const UWoTOnboardingSettings& Settings, const UWoTAngrealGuide* Guide,
		FName ClassName, bool bScreenBusy);

	/** Text up to and including the first sentence terminator (. ! ?) that
	    ends the text or precedes whitespace, trimmed; the whole trimmed text
	    when there is none. "2.5" and an ellipsis's inner dots do not end a
	    sentence. */
	WOTONBOARDING_API FString FirstSentence(const FString& Text);

	/** The card text for Row. ResolveText answers retail display text by
	    (section, property); GuideKeyName is the ShowGuide key's display
	    name, or empty. */
	WOTONBOARDING_API FWoTPickupCardText ComposeText(const FWoTAngrealGuideRow& Row,
		TFunctionRef<FString(const FString& Section, const FString& Property)>
			ResolveText,
		const FString& GuideKeyName);

	/** The key legend a card ends with: "<key>  Field Guide", or empty
	    for no key. */
	WOTONBOARDING_API FString GuideKeyHint(const FString& GuideKeyName);
}
