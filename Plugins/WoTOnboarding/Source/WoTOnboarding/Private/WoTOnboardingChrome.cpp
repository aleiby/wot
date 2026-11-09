#include "WoTOnboardingChrome.h"

#include "Engine/UserInterfaceSettings.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/CoreStyle.h"

namespace
{
FWoTOnboardingMenuChrome WoTOnboardingFallbackChrome()
{
	FWoTOnboardingMenuChrome Chrome;
	Chrome.TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 26);
	Chrome.BodyFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Chrome.ItalicFont = FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 14);
	Chrome.SmallFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);
	const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	Chrome.Background = White;
	Chrome.Frame = White;
	Chrome.Blackout = White;
	Chrome.Separator = White;
	Chrome.Foreground = FSlateColor(FLinearColor(0.92f, 0.92f, 0.88f));
	Chrome.Accent = FSlateColor(FLinearColor(1.0f, 0.85f, 0.45f));
	return Chrome;
}
} // namespace

FWoTOnboardingMenuChrome WoTOnboardingChrome::Resolve()
{
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TOptional<FWoTOnboardingMenuChrome> Resolved = Seams.ResolveMenuChrome
		? Seams.ResolveMenuChrome() : TOptional<FWoTOnboardingMenuChrome>();
	FWoTOnboardingMenuChrome Chrome = Resolved.IsSet()
		? Resolved.GetValue() : WoTOnboardingFallbackChrome();
	const float MinimumSize = DeckMinimumFontSize();
	for (FSlateFontInfo* Font : {&Chrome.TitleFont, &Chrome.BodyFont,
		&Chrome.ItalicFont, &Chrome.SmallFont})
	{
		Font->Size = FMath::Max(Font->Size, MinimumSize);
	}
	return Chrome;
}

bool WoTOnboardingChrome::IsRetail(const FWoTOnboardingMenuChrome& Chrome)
{
	// Only the published chrome carries the revocation binder.
	return static_cast<bool>(Chrome.BindWidget);
}

float WoTOnboardingChrome::LayoutScaleAt(int32 Width, int32 Height)
{
	const UUserInterfaceSettings* Settings = GetDefault<UUserInterfaceSettings>();
	return Settings
		? Settings->GetDPIScaleBasedOnSize(FIntPoint(Width, Height)) : 1.0f;
}

float WoTOnboardingChrome::PhysicalPixels(const FSlateFontInfo& Font,
	float LayoutScale)
{
	return Font.Size * static_cast<float>(FontConstants::RenderDPI) / 72.0f
		* LayoutScale;
}

float WoTOnboardingChrome::DeckMinimumFontSize()
{
	const float DeckScale = FMath::Max(LayoutScaleAt(1280, 800), 0.01f);
	return DeckMinimumTextPixels * 72.0f
		/ (static_cast<float>(FontConstants::RenderDPI) * DeckScale);
}
