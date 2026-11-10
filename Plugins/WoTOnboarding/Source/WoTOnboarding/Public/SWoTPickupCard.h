#pragma once

#include "CoreMinimal.h"
#include "WoTOnboardingPickupCards.h"
#include "Widgets/SCompoundWidget.h"

class FWoTOnboardingFaces;
class FWoTRetailArt;
struct FWoTOnboardingWindowScale;

// The first-pickup card and the death recap (plan Tasks 6 to 8): a small
// passive band at the left edge, mid-height, clear of the crosshair, the
// centred progress lines, the right-hand message column and the hand fan,
// drawn in the original window look at the guide's scale. A pickup card shows
// the item's icon, title, first sentence and the guide key; a recap (text
// with a kicker) shows "KILLED BY" over the killer's icon and title, then
// "COUNTER" over the counter's icon and the hint line, then the guide key. It
// fades in, holds, fades out and reports its expiry. It never takes focus and
// never hit-tests, so play carries on underneath it.
class WOTONBOARDING_API SWoTPickupCard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTPickupCard)
		: _Lifetime(4.0f)
		, _InitialPhysicalSize(FVector2D::ZeroVector)
		, _InitialDpiScale(1.0f)
	{}
		SLATE_ARGUMENT(FWoTPickupCardText, Text)
		/** Seconds on screen, fades included. */
		SLATE_ARGUMENT(float, Lifetime)
		/** The viewport's size in pixels and its UI DPI scale, so the first
		    frame draws at its final scale. */
		SLATE_ARGUMENT(FVector2D, InitialPhysicalSize)
		SLATE_ARGUMENT(float, InitialDpiScale)
		/** Fires once when the lifetime runs out. It runs inside this
		    widget's paint, so the host must remove the card on a later tick. */
		SLATE_EVENT(FSimpleDelegate, OnExpired)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return false; }
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

	const FWoTPickupCardText& GetCardText() const { return CardText; }

	/** True for the recap layout. */
	bool IsRecap() const { return !CardText.Kicker.IsEmpty(); }

	/** The opacity SecondsIn seconds into a card shown for TotalSeconds: a
	    short fade in, a hold, and a fade out ending exactly at TotalSeconds. */
	static float FadeAlpha(float SecondsIn, float TotalSeconds);

private:
	TSharedRef<SWidget> BuildCard();
	TSharedRef<SWidget> BuildRecap();
	/** ClassName's icon at 64 units, or an empty slot. */
	TSharedRef<SWidget> BuildIcon(FName ClassName);
	EActiveTimerReturnType Advance(double InCurrentTime, float InDeltaTime);
	FLinearColor CurrentColor() const;

	FWoTPickupCardText CardText;
	FSimpleDelegate OnExpired;
	TSharedPtr<FWoTOnboardingFaces> Faces;
	TSharedPtr<FWoTRetailArt> Art;
	TSharedPtr<FWoTOnboardingWindowScale> Scale;
	float Lifetime = 4.0f;
	float Elapsed = 0.0f;
	bool bExpired = false;
};
