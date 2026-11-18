#include "WoTCanvasScale.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "WoTMenuStyle.h"

namespace
{
	ESimpleElementBlendMode WoTCanvasScale_BlendMode(EWoTHUDStyle Style)
	{
		switch (Style)
		{
		case EWoTHUDStyle::Masked:
			return SE_BLEND_Masked;
		case EWoTHUDStyle::Translucent:
			return SE_BLEND_Translucent;
		case EWoTHUDStyle::Normal:
		default:
			return SE_BLEND_Opaque;
		}
	}
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
	if (!CanvasObject || !Texture || !Texture->GetResource())
	{
		return;
	}

	const float DrawScale = Scale == 0.0f ? 1.0f : Scale;
	FCanvasTileItem Tile(
		FVector2D(X * FactorX, Y * FactorY),
		Texture->GetResource(),
		FVector2D(Texture->GetSizeX() * DrawScale * FactorX,
			Texture->GetSizeY() * DrawScale * FactorY),
		Tint);
	Tile.BlendMode = WoTCanvasScale_BlendMode(Style);
	CanvasObject->DrawItem(Tile);
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

	FCanvasTextItem TextItem(FVector2D(DrawX, DrawY), FText::FromString(Text),
		Font, Tint);
	TextItem.BlendMode = SE_BLEND_Masked;
	TextItem.Scale = FVector2D(Scale, Scale);
	CanvasObject->DrawItem(TextItem);
}
