#pragma once

#include "CoreMinimal.h"
#include "WoTHUDDrawCmd.h"

class UCanvas;
class UFont;
class UTexture2D;
enum ESimpleElementBlendMode : int;

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

	/** The one place the HUD submits a texture tile to a canvas: Size in
	 * canvas units, Blend as the HUD styles map it. */
	static void SubmitTile(UCanvas* CanvasObject, UTexture2D* Texture,
		const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color,
		EWoTHUDStyle Style);
	/** The one place the HUD submits a text run to a canvas: a canvas text
	 * item at Position, masked or translucent, Scale times its face's size;
	 * bCentreX centres it by the item's own whole-pixel rule. */
	static void SubmitText(UCanvas* CanvasObject, const FString& Text, const UFont* Font,
		const FVector2D& Position, const FLinearColor& Color, EWoTHUDStyle Style,
		float Scale, bool bCentreX);
	/** The blend SubmitTile and SubmitText give Style now: Normal opaque,
	 * Masked masked, Translucent SE_BLEND_Translucent, or SE_BLEND_AlphaComposite
	 * while an FWoTCanvasAlphaScope is open. */
	static WOT_API ESimpleElementBlendMode BlendModeFor(EWoTHUDStyle Style);

#if WITH_AUTOMATION_TESTS
	/** Test-only: sees every icon the HUD asks DrawIconAt for (drawable or
	 * not) and every text run it submits, one line each, with the canvas
	 * size and transform it lands on. */
	static WOT_API TFunction<void(const FString&)>& SubmissionObserverForTesting();
#endif
};

/** While one is open (game thread only), the HUD's translucent style submits
 * with SE_BLEND_AlphaComposite instead of SE_BLEND_Translucent.
 * SE_BLEND_Translucent blends colour by the source alpha but writes colour
 * only, leaving the target's alpha as it was (SetBlendState in the engine's
 * BatchedElements.cpp). A headset panel is cleared to transparent and its
 * stereo layer is composited as unpremultiplied colour by the texture's own
 * alpha, so it needs straight colour plus that alpha: SE_BLEND_AlphaComposite
 * (colour One/InvSrcAlpha, alpha One/InvSrcAlpha) stores exactly (colour,
 * alpha) over the clear, and the layer then shows colour * alpha over the
 * world as the flat HUD does over the scene. SE_BLEND_AlphaBlend would store
 * colour * alpha, which the layer multiplies by alpha again (fades darker).
 * The HUD's translucent elements do not overlap each other on the panel. The
 * flat HUD never opens one, so its draw stream is unchanged. */
class WOT_API FWoTCanvasAlphaScope
{
public:
	FWoTCanvasAlphaScope();
	~FWoTCanvasAlphaScope();
	FWoTCanvasAlphaScope(const FWoTCanvasAlphaScope&) = delete;
	FWoTCanvasAlphaScope& operator=(const FWoTCanvasAlphaScope&) = delete;
	static bool IsOpen();
};
