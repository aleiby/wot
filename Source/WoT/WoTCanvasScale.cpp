#include "WoTCanvasScale.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "WoTMenuStyle.h"

namespace
{
	/** FWoTCanvasAlphaScopes open now (game thread). */
	int32 GWoTCanvasAlphaScopes = 0;

#if WITH_AUTOMATION_TESTS
	TFunction<void(const FString&)> GWoTCanvasSubmissionObserver;

	/** The canvas a submission lands on: its logical size and clip, and the
	 * scale and offset of its current transform. */
	FString WoTCanvasScale_DescribeCanvas(const UCanvas& CanvasObject)
	{
		FMatrix Transform = FMatrix::Identity;
		if (CanvasObject.Canvas)
		{
			Transform = CanvasObject.Canvas->GetTransform();
		}
		return FString::Printf(TEXT("canvas=(%d,%d,%.3f,%.3f) xform=(%.3f,%.3f,%.3f,%.3f)"),
			CanvasObject.SizeX, CanvasObject.SizeY, CanvasObject.ClipX, CanvasObject.ClipY,
			Transform.M[0][0], Transform.M[1][1], Transform.M[3][0], Transform.M[3][1]);
	}
#endif
}

#if WITH_AUTOMATION_TESTS
TFunction<void(const FString&)>& FWoTCanvasScale::SubmissionObserverForTesting()
{
	return GWoTCanvasSubmissionObserver;
}
#endif

FWoTCanvasAlphaScope::FWoTCanvasAlphaScope()
{
	check(IsInGameThread());
	++GWoTCanvasAlphaScopes;
}

FWoTCanvasAlphaScope::~FWoTCanvasAlphaScope()
{
	--GWoTCanvasAlphaScopes;
}

bool FWoTCanvasAlphaScope::IsOpen()
{
	return GWoTCanvasAlphaScopes > 0;
}

ESimpleElementBlendMode FWoTCanvasScale::BlendModeFor(EWoTHUDStyle Style)
{
	switch (Style)
	{
	case EWoTHUDStyle::Masked:
		return SE_BLEND_Masked;
	case EWoTHUDStyle::Translucent:
		// Inside the scope the target keeps straight colour and its own alpha.
		return GWoTCanvasAlphaScopes > 0 ? SE_BLEND_AlphaComposite : SE_BLEND_Translucent;
	case EWoTHUDStyle::Normal:
	default:
		return SE_BLEND_Opaque;
	}
}

void FWoTCanvasScale::SubmitTile(UCanvas* CanvasObject, UTexture2D* Texture,
	const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color,
	EWoTHUDStyle Style)
{
	if (!CanvasObject || !Texture || !Texture->GetResource())
	{
		return;
	}
	FCanvasTileItem Tile(Position, Texture->GetResource(), Size, Color);
	Tile.BlendMode = BlendModeFor(Style);
	CanvasObject->DrawItem(Tile);
}

void FWoTCanvasScale::SubmitText(UCanvas* CanvasObject, const FString& Text, const UFont* Font,
	const FVector2D& Position, const FLinearColor& Color, EWoTHUDStyle Style,
	float Scale, bool bCentreX)
{
	if (!CanvasObject)
	{
		return;
	}
	FCanvasTextItem TextItem(Position, FText::FromString(Text), Font, Color);
	TextItem.BlendMode = BlendModeFor(Style);
	// A runtime-cache font draws through an alpha-only shader that accepts only
	// the translucent modes (the engine checks it in CanvasItem.cpp), so such a
	// font keeps the plain translucent mode inside an alpha scope too. The HUD's
	// own canvas fonts are offline-cache fonts.
	if (TextItem.BlendMode == SE_BLEND_AlphaComposite && Font
		&& Font->FontCacheType == EFontCacheType::Runtime)
	{
		TextItem.BlendMode = SE_BLEND_Translucent;
	}
	TextItem.bCentreX = bCentreX;
	TextItem.Scale = FVector2D(Scale, Scale);
#if WITH_AUTOMATION_TESTS
	if (GWoTCanvasSubmissionObserver)
	{
		GWoTCanvasSubmissionObserver(FString::Printf(
			TEXT("text %s \"%s\" pos=(%.3f,%.3f) color=(%.3f,%.3f,%.3f,%.3f) blend=%d scale=%.3f centre=%d %s"),
			Font ? *Font->GetName() : TEXT("none"), *Text.ReplaceCharWithEscapedChar(),
			Position.X, Position.Y, Color.R, Color.G, Color.B, Color.A,
			static_cast<int32>(TextItem.BlendMode), Scale, bCentreX ? 1 : 0,
			*WoTCanvasScale_DescribeCanvas(*CanvasObject)));
	}
#endif
	CanvasObject->DrawItem(TextItem);
}

int32 FWoTCanvasScale::ComputeUpscale(
	int32 SizeX, int32 SizeY, int32 Override)
{
	const int32 LargestFit = FMath::Max(
		1, FMath::Min(SizeX / 640, SizeY / 480));
	return Override >= 1 ? FMath::Clamp(Override, 1, LargestFit) : LargestFit;
}

int32 FWoTCanvasScale::ComputeTextScale(int32 Upscale, float LineHeightPixels)
{
	const float Physical = FMath::Max(LineHeightPixels, 1.0f)
		* static_cast<float>(FMath::Max(Upscale, 1));
	return FMath::Clamp(FMath::CeilToInt(
		FWoTMenuStyle::PrimaryLegiblePixels / Physical), 1, 2);
}

FWoTCanvasDimensions FWoTCanvasScale::ApplyLogicalDimensions(
	UCanvas* CanvasObject, int32 Upscale)
{
	FWoTCanvasDimensions Original;
	if (!CanvasObject)
	{
		return Original;
	}
	Original.SizeX = CanvasObject->SizeX;
	Original.SizeY = CanvasObject->SizeY;
	Original.ClipX = CanvasObject->ClipX;
	Original.ClipY = CanvasObject->ClipY;
	const int32 SafeUpscale = FMath::Max(1, Upscale);
	CanvasObject->SizeX /= SafeUpscale;
	CanvasObject->SizeY /= SafeUpscale;
	CanvasObject->ClipX /= static_cast<float>(SafeUpscale);
	CanvasObject->ClipY /= static_cast<float>(SafeUpscale);
	return Original;
}

void FWoTCanvasScale::RestoreDimensions(UCanvas* CanvasObject,
	const FWoTCanvasDimensions& Dimensions)
{
	if (!CanvasObject)
	{
		return;
	}
	CanvasObject->SizeX = Dimensions.SizeX;
	CanvasObject->SizeY = Dimensions.SizeY;
	CanvasObject->ClipX = Dimensions.ClipX;
	CanvasObject->ClipY = Dimensions.ClipY;
}

void FWoTCanvasScale::ComputeFactors(int32 SizeX, int32 SizeY)
{
	FactorX = (SizeX >= 1 && SizeX < 640) ? static_cast<float>(SizeX) / 640.0f : 1.0f;
	FactorY = (SizeY >= 1 && SizeY < 480) ? static_cast<float>(SizeY) / 480.0f : 1.0f;
}

void FWoTCanvasScale::DrawIconAt(UCanvas* CanvasObject, UTexture2D* Texture,
	float X, float Y, float Scale, EWoTHUDStyle Style,
	const FLinearColor& Tint) const
{
#if WITH_AUTOMATION_TESTS
	// Every icon the HUD asks for, drawable or not: a headless canvas has
	// no texture resources to draw with.
	if (GWoTCanvasSubmissionObserver && CanvasObject)
	{
		const float SizeScale = Scale == 0.0f ? 1.0f : Scale;
		GWoTCanvasSubmissionObserver(FString::Printf(
			TEXT("icon %s pos=(%.3f,%.3f) size=(%.3f,%.3f) color=(%.3f,%.3f,%.3f,%.3f) style=%d %s"),
			Texture ? *Texture->GetName() : TEXT("none"), X * FactorX, Y * FactorY,
			Texture ? Texture->GetSizeX() * SizeScale * FactorX : 0.0f,
			Texture ? Texture->GetSizeY() * SizeScale * FactorY : 0.0f,
			Tint.R, Tint.G, Tint.B, Tint.A, static_cast<int32>(Style),
			*WoTCanvasScale_DescribeCanvas(*CanvasObject)));
	}
#endif
	if (!CanvasObject || !Texture || !Texture->GetResource())
	{
		return;
	}

	const float DrawScale = Scale == 0.0f ? 1.0f : Scale;
	SubmitTile(CanvasObject, Texture,
		FVector2D(X * FactorX, Y * FactorY),
		FVector2D(Texture->GetSizeX() * DrawScale * FactorX,
			Texture->GetSizeY() * DrawScale * FactorY),
		Tint, Style);
}

void FWoTCanvasScale::DrawScaledTextAt(UCanvas* CanvasObject, float X, float Y,
	const FString& Text, UFont* Font, bool bCenterX, const FLinearColor& Tint,
	EWoTTextAlignment Alignment, float TextScale) const
{
	if (!CanvasObject || !Font)
	{
		return;
	}

	const float Scale = TextScale > 0.0f ? TextScale : 1.0f;
	float DrawX = X * FactorX;
	const float DrawY = Y * FactorY;
	const EWoTTextAlignment EffectiveAlignment = bCenterX
		? EWoTTextAlignment::Center : Alignment;
	if (EffectiveAlignment != EWoTTextAlignment::Left)
	{
		float TextSizeX = 0.0f;
		float TextSizeY = 0.0f;
		CanvasObject->StrLen(Font, Text, TextSizeX, TextSizeY);
		TextSizeX *= Scale;
		DrawX -= EffectiveAlignment == EWoTTextAlignment::Center
			? TextSizeX * 0.5f : TextSizeX;
	}

	SubmitText(CanvasObject, Text, Font, FVector2D(DrawX, DrawY), Tint,
		EWoTHUDStyle::Masked, Scale, /*bCentreX*/ false);
}
