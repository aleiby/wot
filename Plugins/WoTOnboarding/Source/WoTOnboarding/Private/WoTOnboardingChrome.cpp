#include "WoTOnboardingChrome.h"

#include "Layout/Geometry.h"
#include "Styling/CoreStyle.h"

namespace
{
FLinearColor WoTOnboardingFaceGold(WoTOnboardingChrome::EFace Face, int32 Level)
{
	// The colour the original faces' glyphs carry; the published faces
	// deliver coverage only.
	static const FColor Gold[] = {
		FColor(255, 212, 68, 255),
		FColor(255, 207, 55, 255),
		FColor(255, 204, 50, 255),
		FColor(255, 209, 90, 255),
		FColor(255, 218, 130, 255),
	};
	static_assert(static_cast<int32>(UE_ARRAY_COUNT(Gold))
		== static_cast<int32>(WoTOnboardingChrome::EFace::Count),
		"one gold per face");
	const FColor& Glyph = Gold[static_cast<int32>(Face)];
	return FLinearColor(FColor(
		static_cast<uint8>((Glyph.R * Level + 127) / 255),
		static_cast<uint8>((Glyph.G * Level + 127) / 255),
		static_cast<uint8>((Glyph.B * Level + 127) / 255), 255));
}

FName WoTOnboardingFaceName(WoTOnboardingChrome::EFace Face)
{
	switch (Face)
	{
	case WoTOnboardingChrome::EFace::Title:
		return FName(TEXT("F_WOTReg30"));
	case WoTOnboardingChrome::EFace::Italic:
		return FName(TEXT("F_WOTIta14"));
	case WoTOnboardingChrome::EFace::Element:
		return FName(TEXT("F_Element"));
	case WoTOnboardingChrome::EFace::Charge:
		return FName(TEXT("F_Charge"));
	default:
		return FName(TEXT("F_WOTReg14"));
	}
}

/** An engine face standing in for an original one, at about its height in
    window units. */
FSlateFontInfo WoTOnboardingEngineFace(WoTOnboardingChrome::EFace Face)
{
	switch (Face)
	{
	case WoTOnboardingChrome::EFace::Title:
		return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18);
	case WoTOnboardingChrome::EFace::Italic:
		return FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 10);
	case WoTOnboardingChrome::EFace::Element:
		return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
	default:
		return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);
	}
}

/** Face at PixelScale as Font() describes, and whether the chrome
    published it (false for an engine stand-in). */
FSlateFontInfo WoTOnboardingResolveFace(const FWoTOnboardingMenuChrome& Chrome,
	WoTOnboardingChrome::EFace Face, int32 PixelScale, bool& bOutPublished)
{
	FSlateFontInfo Font;
	if (Chrome.PixelFont)
	{
		Font = Chrome.PixelFont(WoTOnboardingFaceName(Face),
			FMath::Clamp(PixelScale, 1, 2));
	}
	bOutPublished = Font.HasValidFont();
	if (!bOutPublished)
	{
		Font = WoTOnboardingEngineFace(Face);
	}
	Font.Size = FMath::Max(Font.Size, WoTOnboardingChrome::DeckMinimumFontSize());
	return Font;
}

float WoTOnboardingFit(const FVector2D& PhysicalSize)
{
	return FMath::Min(PhysicalSize.X / WoTOnboardingChrome::WindowWidth,
		PhysicalSize.Y / WoTOnboardingChrome::WindowHeight);
}
} // namespace

FWoTOnboardingMenuChrome WoTOnboardingChrome::Resolve()
{
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TOptional<FWoTOnboardingMenuChrome> Resolved = Seams.ResolveMenuChrome
		? Seams.ResolveMenuChrome() : TOptional<FWoTOnboardingMenuChrome>();
	return Resolved.IsSet() ? Resolved.GetValue() : FWoTOnboardingMenuChrome();
}

bool WoTOnboardingChrome::IsRetail(const FWoTOnboardingMenuChrome& Chrome)
{
	return static_cast<bool>(Chrome.PixelFont);
}

int32 WoTOnboardingChrome::PixelScaleFor(const FVector2D& PhysicalSize)
{
	const float Fit = WoTOnboardingFit(PhysicalSize);
	return Fit > 0.0f ? FMath::Clamp(FMath::FloorToInt(Fit), 1, 2) : 1;
}

float WoTOnboardingChrome::WindowScaleFor(const FVector2D& PhysicalSize,
	EWindowScaleRule Rule)
{
	const float Fit = WoTOnboardingFit(PhysicalSize);
	if (!(Fit > 0.0f))
	{
		return 1.0f;
	}
	if (Rule == EWindowScaleRule::GuideFill)
	{
		return Fit * GuideScreenFill;
	}
	return Fit < 1.0f ? Fit : static_cast<float>(PixelScaleFor(PhysicalSize));
}

float WoTOnboardingChrome::WindowScaleAt(int32 Width, int32 Height)
{
	return WindowScaleFor(
		FVector2D(static_cast<double>(Width), static_cast<double>(Height)));
}

float WoTOnboardingChrome::PhysicalPixels(const FSlateFontInfo& Font,
	float WindowScale)
{
	return Font.Size * static_cast<float>(FontConstants::RenderDPI) / 72.0f
		* WindowScale;
}

float WoTOnboardingChrome::DeckMinimumFontSize()
{
	return DeckMinimumTextPixels * 72.0f
		/ (static_cast<float>(FontConstants::RenderDPI) * WindowScaleAt(1280, 800));
}

FSlateFontInfo WoTOnboardingChrome::Font(const FWoTOnboardingMenuChrome& Chrome,
	EFace Face, int32 PixelScale)
{
	bool bPublished = false;
	return WoTOnboardingResolveFace(Chrome, Face, PixelScale, bPublished);
}

FLinearColor WoTOnboardingChrome::TextColor(EFace Face)
{
	return WoTOnboardingFaceGold(Face, 255);
}

FLinearColor WoTOnboardingChrome::LabelColor(EFace Face)
{
	return WoTOnboardingFaceGold(Face, 127);
}

TSharedRef<STextBlock> WoTOnboardingChrome::MakeText(
	const TSharedRef<FWoTOnboardingFaces>& Faces, const FString& Text, EFace Face,
	const FLinearColor& Color, ETextJustify::Type Justification, bool bWrap)
{
	return SNew(STextBlock)
		.Text(FText::FromString(Text))
		.Font_Lambda([Faces, Face]() { return Faces->Get(Face); })
		.ColorAndOpacity(Color)
		.Justification(Justification)
		.AutoWrapText(bWrap);
}

void FWoTOnboardingWindowScale::Initialize(const FVector2D& PhysicalSize,
	float InDpiScale)
{
	if (PhysicalSize.X <= 0.0f || PhysicalSize.Y <= 0.0f)
	{
		return;
	}
	DpiScale = FMath::Max(InDpiScale, 0.01f);
	PixelScale = WoTOnboardingChrome::PixelScaleFor(PhysicalSize);
	WindowScale = WoTOnboardingChrome::WindowScaleFor(PhysicalSize, Rule);
}

bool FWoTOnboardingWindowScale::Update(const FGeometry& AllottedGeometry)
{
	DpiScale = FMath::Max(
		AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	const FVector2D PhysicalSize =
		FVector2D(AllottedGeometry.GetLocalSize()) * DpiScale;
	const int32 PreviousPixelScale = PixelScale;
	PixelScale = WoTOnboardingChrome::PixelScaleFor(PhysicalSize);
	WindowScale = WoTOnboardingChrome::WindowScaleFor(PhysicalSize, Rule);
	return PixelScale != PreviousPixelScale;
}

FWoTOnboardingFaces::FWoTOnboardingFaces(const FWoTOnboardingMenuChrome& InChrome)
	: Chrome(InChrome)
{
	Refresh(1);
}

void FWoTOnboardingFaces::Refresh(int32 PixelScale)
{
	for (int32 Index = 0;
		Index < static_cast<int32>(WoTOnboardingChrome::EFace::Count); ++Index)
	{
		Fonts[Index] = WoTOnboardingResolveFace(Chrome,
			static_cast<WoTOnboardingChrome::EFace>(Index), PixelScale,
			bPublished[Index]);
	}
}
