#include "WoTOnboardingChrome.h"

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
	return Resolved.IsSet() ? Resolved.GetValue() : WoTOnboardingFallbackChrome();
}

bool WoTOnboardingChrome::IsRetail(const FWoTOnboardingMenuChrome& Chrome)
{
	// Only the published chrome carries the revocation binder.
	return static_cast<bool>(Chrome.BindWidget);
}
