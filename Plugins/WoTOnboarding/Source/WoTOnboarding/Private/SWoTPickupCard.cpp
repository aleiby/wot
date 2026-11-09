#include "SWoTPickupCard.h"

#include "WoTOnboardingChrome.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SWoTPickupCard::Construct(const FArguments& InArgs)
{
	CardText = InArgs._Text;
	OnExpired = InArgs._OnExpired;
	Lifetime = FMath::Max(InArgs._Lifetime, 0.1f);

	const FWoTOnboardingMenuChrome Chrome = WoTOnboardingChrome::Resolve();
	const bool bRetailChrome = WoTOnboardingChrome::IsRetail(Chrome);

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(CardText.Title))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Accent)
		];
	if (!CardText.Summary.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(CardText.Summary))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Foreground)
			.AutoWrapText(true)
		];
	}
	if (!CardText.KeyHint.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(CardText.KeyHint))
			.Font(Chrome.SmallFont)
			.ColorAndOpacity(Chrome.Foreground)
		];
	}

	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	.Padding(FMargin(28.0f, 0.0f, 0.0f, 0.0f))
	[
		SNew(SBox)
		.WidthOverride(400.0f)
		[
			SNew(SBorder)
			.BorderImage(Chrome.Background)
			.BorderBackgroundColor(bRetailChrome
				? FLinearColor(1.0f, 1.0f, 1.0f, 0.92f)
				: FLinearColor(0.06f, 0.05f, 0.04f, 0.88f))
			.Padding(FMargin(16.0f, 12.0f))
			[
				Body
			]
		]
	];

	SetVisibility(EVisibility::HitTestInvisible);
	SetColorAndOpacity(TAttribute<FLinearColor>::CreateSP(
		this, &SWoTPickupCard::CurrentColor));
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
		this, &SWoTPickupCard::Advance));
	if (Chrome.BindWidget)
	{
		Chrome.BindWidget(AsShared());
	}
}

float SWoTPickupCard::FadeAlpha(float SecondsIn, float TotalSeconds)
{
	constexpr float FadeInSeconds = 0.2f;
	constexpr float FadeOutSeconds = 0.6f;
	if (SecondsIn <= 0.0f || SecondsIn >= TotalSeconds)
	{
		return 0.0f;
	}
	const float FadeIn = FMath::Clamp(SecondsIn / FadeInSeconds, 0.0f, 1.0f);
	const float FadeOut =
		FMath::Clamp((TotalSeconds - SecondsIn) / FadeOutSeconds, 0.0f, 1.0f);
	return FMath::Min(FadeIn, FadeOut);
}

EActiveTimerReturnType SWoTPickupCard::Advance(double, float InDeltaTime)
{
	Elapsed += InDeltaTime;
	if (Elapsed < Lifetime)
	{
		return EActiveTimerReturnType::Continue;
	}
	if (!bExpired)
	{
		bExpired = true;
		OnExpired.ExecuteIfBound();
	}
	return EActiveTimerReturnType::Stop;
}

FLinearColor SWoTPickupCard::CurrentColor() const
{
	return FLinearColor(1.0f, 1.0f, 1.0f, FadeAlpha(Elapsed, Lifetime));
}
