#pragma once

#include "CoreMinimal.h"
#include "WoTOnboardingPickupCards.h"
#include "Widgets/SCompoundWidget.h"

// The first-pickup card (plan Task 6): a small passive panel at the left
// edge, mid-height, clear of the crosshair, the centred progress lines, the
// right-hand message column and the hand fan. It fades in, holds, fades out
// and reports its expiry. It never takes focus and never hit-tests, so play
// carries on underneath it.
class WOTONBOARDING_API SWoTPickupCard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTPickupCard)
		: _Lifetime(4.0f)
	{}
		SLATE_ARGUMENT(FWoTPickupCardText, Text)
		/** Seconds on screen, fades included. */
		SLATE_ARGUMENT(float, Lifetime)
		/** Fires once when the lifetime runs out. It runs inside this
		    widget's paint, so the host must remove the card on a later tick. */
		SLATE_EVENT(FSimpleDelegate, OnExpired)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return false; }

	const FWoTPickupCardText& GetCardText() const { return CardText; }

	/** The opacity SecondsIn seconds into a card shown for TotalSeconds: a
	    short fade in, a hold, and a fade out ending exactly at TotalSeconds. */
	static float FadeAlpha(float SecondsIn, float TotalSeconds);

private:
	EActiveTimerReturnType Advance(double InCurrentTime, float InDeltaTime);
	FLinearColor CurrentColor() const;

	FWoTPickupCardText CardText;
	FSimpleDelegate OnExpired;
	float Lifetime = 4.0f;
	float Elapsed = 0.0f;
	bool bExpired = false;
};
