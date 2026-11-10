#include "SWoTPickupCard.h"

#include "SWoTRetailArt.h"
#include "WoTOnboardingChrome.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// The band in window units (plan Task 8): 28 in from the screen's left edge,
// the icon 12 in, the text column 90 in.
constexpr float WoTPickupCardInset = 28.0f;
constexpr float WoTPickupCardWidth = 430.0f;
constexpr float WoTPickupCardHeight = 104.0f;
constexpr float WoTPickupCardRecapWidth = 450.0f;
constexpr float WoTPickupCardRecapHeight = 186.0f;
constexpr float WoTPickupCardIcon = 64.0f;
constexpr float WoTPickupCardIconLeft = 12.0f;
constexpr float WoTPickupCardIconGap = 14.0f;
constexpr float WoTPickupCardMargin = 12.0f;
} // namespace

void SWoTPickupCard::Construct(const FArguments& InArgs)
{
	CardText = InArgs._Text;
	OnExpired = InArgs._OnExpired;
	Lifetime = FMath::Max(InArgs._Lifetime, 0.1f);

	Faces = MakeShared<FWoTOnboardingFaces>(WoTOnboardingChrome::Resolve());
	Art = FWoTRetailArt::Resolve();
	Scale = MakeShared<FWoTOnboardingWindowScale>();
	Scale->Initialize(InArgs._InitialPhysicalSize, InArgs._InitialDpiScale);
	Faces->Refresh(Scale->GetPixelScale());

	const TSharedRef<FWoTOnboardingWindowScale> ScaleRef = Scale.ToSharedRef();
	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	[
		SNew(SScaleBox)
		.Stretch(EStretch::UserSpecified)
		.UserSpecifiedScale_Lambda([ScaleRef]() { return ScaleRef->SlateScale(); })
		[
			SNew(SBox)
			.Padding(FMargin(WoTPickupCardInset, 0.0f, 0.0f, 0.0f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SWoTRetailArt)
					.Piece(EWoTRetailArtPiece::Band)
					.Art(Art)
				]
				// The band's dividers sit on the content's top and bottom
				// edges, half above and half below.
				+ SOverlay::Slot()
				.Padding(FMargin(0.0f, SWoTRetailArt::DividerHeight * 0.5f))
				[
					IsRecap() ? BuildRecap() : BuildCard()
				]
			]
		]
	];

	SetVisibility(EVisibility::HitTestInvisible);
	SetColorAndOpacity(TAttribute<FLinearColor>::CreateSP(
		this, &SWoTPickupCard::CurrentColor));
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
		this, &SWoTPickupCard::Advance));
	if (Faces->GetChrome().BindWidget)
	{
		Faces->GetChrome().BindWidget(AsShared());
	}
}

TSharedRef<SWidget> SWoTPickupCard::BuildIcon(FName ClassName)
{
	const FSlateBrush* Icon = Art->ItemIcon(ClassName, false);
	return SNew(SBox)
		.WidthOverride(WoTPickupCardIcon)
		.HeightOverride(WoTPickupCardIcon)
		[
			SNew(SImage)
			.Image(Icon ? Icon : FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.ColorAndOpacity(Icon ? FLinearColor::White
			                      : FLinearColor(0.10f, 0.09f, 0.08f, 1.0f))
		];
}

TSharedRef<SWidget> SWoTPickupCard::BuildCard()
{
	// The icon, then the title, the first sentence and the guide key.
	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.Title,
				WoTOnboardingChrome::EFace::Title, WoTOnboardingChrome::TextColor())
		];
	if (!CardText.Summary.IsEmpty())
	{
		Column->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.Summary,
				WoTOnboardingChrome::EFace::Body, WoTOnboardingChrome::TextColor(),
				ETextJustify::Left, true)
		];
	}
	if (!CardText.KeyHint.IsEmpty())
	{
		Column->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.KeyHint,
				WoTOnboardingChrome::EFace::Body, WoTOnboardingChrome::LabelColor())
		];
	}
	return SNew(SBox)
		.WidthOverride(WoTPickupCardWidth)
		.MinDesiredHeight(WoTPickupCardHeight)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(WoTPickupCardIconLeft, 20.0f, WoTPickupCardIconGap, 0.0f))
			[
				BuildIcon(CardText.ItemClass)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(FMargin(0.0f, WoTPickupCardMargin, WoTPickupCardMargin,
				WoTPickupCardMargin))
			[
				Column
			]
		];
}

TSharedRef<SWidget> SWoTPickupCard::BuildRecap()
{
	// "KILLED BY" over the killer, "COUNTER" over the counter and the hint,
	// then the guide key.
	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	const FLinearColor Label = WoTOnboardingChrome::LabelColor();
	const FLinearColor Body = WoTOnboardingChrome::TextColor();

	TSharedRef<SVerticalBox> CounterColumn = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.SummaryKicker,
				WoTOnboardingChrome::EFace::Body, Label)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.Summary,
				WoTOnboardingChrome::EFace::Body, Body, ETextJustify::Left, true)
		];
	if (!CardText.KeyHint.IsEmpty())
	{
		CounterColumn->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 20.0f, 0.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, CardText.KeyHint,
				WoTOnboardingChrome::EFace::Body, Label)
		];
	}
	// A hint that names no counter keeps its column; the icon slot stays
	// clear.
	const TSharedRef<SWidget> CounterIcon = CardText.CounterClass.IsNone()
		? StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(WoTPickupCardIcon))
		: BuildIcon(CardText.CounterClass);

	return SNew(SBox)
		.WidthOverride(WoTPickupCardRecapWidth)
		.MinDesiredHeight(WoTPickupCardRecapHeight)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 16.0f, 0.0f, 0.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(FMargin(WoTPickupCardIconLeft, 0.0f, WoTPickupCardIconGap, 0.0f))
				[
					BuildIcon(CardText.ItemClass)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 2.0f, WoTPickupCardMargin, 0.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						WoTOnboardingChrome::MakeText(FacesRef, CardText.Kicker,
							WoTOnboardingChrome::EFace::Body, Label)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
					[
						WoTOnboardingChrome::MakeText(FacesRef, CardText.Title,
							WoTOnboardingChrome::EFace::Title, Body)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, WoTPickupCardMargin, 0.0f, WoTPickupCardMargin))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(FMargin(WoTPickupCardIconLeft, 0.0f, WoTPickupCardIconGap, 0.0f))
				[
					CounterIcon
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 4.0f, WoTPickupCardMargin, 0.0f))
				[
					CounterColumn
				]
			]
		];
}

void SWoTPickupCard::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (Scale.IsValid() && Faces.IsValid() && Scale->Update(AllottedGeometry))
	{
		Faces->Refresh(Scale->GetPixelScale());
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
