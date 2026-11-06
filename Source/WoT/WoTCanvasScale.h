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
	void DrawScaledTextAt(UCanvas* CanvasObject, float X, float Y,
		const FString& Text, UFont* Font, bool bCenterX = false,
		const FLinearColor& Tint = FLinearColor::White,
		EWoTTextAlignment Alignment = EWoTTextAlignment::Left) const;
};
