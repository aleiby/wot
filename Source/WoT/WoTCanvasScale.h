#pragma once

#include "CoreMinimal.h"
#include "WoTHUDDrawCmd.h"

class UCanvas;
class UFont;
class UTexture2D;

struct FWoTCanvasDimensions
{
	int32 SizeX = 0;
	int32 SizeY = 0;
	float ClipX = 0.0f;
	float ClipY = 0.0f;
};

struct FWoTCanvasScale
{
	float FactorX = 1.0f;
	float FactorY = 1.0f;

	static int32 ComputeUpscale(int32 SizeX, int32 SizeY, int32 Override);
	/** The HUD's text scale (the Steam Deck legibility pass): the whole number
	 * the retail canvas text draws at on top of Upscale so a face whose line
	 * is LineHeightPixels tall at 1x reaches FWoTMenuStyle's primary 16
	 * physical pixels. 2 for the 14-pixel faces at Upscale 1 (1280x800,
	 * 1280x720), 1 from Upscale 2 up, so 1080p and larger draw as before. The
	 * canvas atlases are nearest-filtered, so 2x is pixel-exact. */
	static int32 ComputeTextScale(int32 Upscale, float LineHeightPixels);
	static FWoTCanvasDimensions ApplyLogicalDimensions(
		UCanvas* CanvasObject, int32 Upscale);
	static void RestoreDimensions(UCanvas* CanvasObject,
		const FWoTCanvasDimensions& Dimensions);
	void ComputeFactors(int32 SizeX, int32 SizeY);
	float ScaleValX(float Value) const { return Value * FactorX; }
	float ScaleValY(float Value) const { return Value * FactorY; }

	void DrawIconAt(UCanvas* CanvasObject, UTexture2D* Texture, float X, float Y,
		float Scale, EWoTHUDStyle Style,
		const FLinearColor& Tint = FLinearColor::White) const;
	/** TextScale draws the text that many times its face's size from the
	 * same anchor; alignment measures the scaled text. */
	void DrawScaledTextAt(UCanvas* CanvasObject, float X, float Y,
		const FString& Text, UFont* Font, bool bCenterX = false,
		const FLinearColor& Tint = FLinearColor::White,
		EWoTTextAlignment Alignment = EWoTTextAlignment::Left,
		float TextScale = 1.0f) const;
};
