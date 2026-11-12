#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Rendering/RenderingCommon.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SLeafWidget.h"

class UTexture2D;

// The original UI art the onboarding windows draw (plan Task 8): the
// item-info window's frame, dividers and gem bar (WOTWindow.uc:107-152,
// InventoryInfoWindow.uc:59-88 and :140-150), its icon bevel, the HUD's gold
// selected-slot frame, and each item class's inventory icon. All of it comes
// from the player's own files through the onboarding seams; a piece that does
// not resolve draws as a flat panel, and an icon that does not resolve leaves
// its slot empty.

/** The art one window draws with, resolved when it opens and held while it
    lives. */
class FWoTRetailArt
{
public:
	/** The window art through the onboarding seams. Icons resolve on first
	    use. */
	static TSharedRef<FWoTRetailArt> Resolve();

	// Textures flagged linear hold the original display colours. Slate would
	// apply its display gamma to them a second time.
	static ESlateDrawEffect DrawEffectFor(const FSlateBrush& Brush);

	/** The frame and fill texture (WOTWindow's CornerTexture), or null. */
	UTexture2D* InfoBoxC1() const { return CornerTexture.Get(); }
	/** The edge, divider and gem texture (WOTWindow's OtherTexture), or null. */
	UTexture2D* InfoBoxC2() const { return OtherTexture.Get(); }

	/** The item-info window's icon bevel, or null. */
	const FSlateBrush* IconMask() const;
	/** The HUD's gold selected-slot frame, filtered smoothly (the guide draws
	    it at its icon size), or null. */
	const FSlateBrush* SelectionFrame() const;
	/** ClassName's inventory icon, or null without one. bSmooth for an icon
	    drawn at a size that is not a whole multiple of its own. */
	const FSlateBrush* ItemIcon(FName ClassName, bool bSmooth);

private:
	struct FBrushEntry
	{
		TStrongObjectPtr<UTexture2D> Texture;
		FSlateBrush Brush;
	};
	static TUniquePtr<FBrushEntry> MakeEntry(UTexture2D* Texture);

	TStrongObjectPtr<UTexture2D> CornerTexture;
	TStrongObjectPtr<UTexture2D> OtherTexture;
	TUniquePtr<FBrushEntry> Mask;
	TUniquePtr<FBrushEntry> Frame;
	/** Crisp and smooth icons by class; a null entry records a miss. */
	TMap<FName, TUniquePtr<FBrushEntry>> Icons[2];
};

/** A retail image with the same tint and sizing as an SImage. */
class SWoTRetailImage : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTRetailImage)
		: _Image(nullptr)
		, _ColorAndOpacity(FLinearColor::White)
	{}
		SLATE_ARGUMENT(const FSlateBrush*, Image)
		SLATE_ATTRIBUTE(FSlateColor, ColorAndOpacity)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	const FSlateBrush* ImageBrush = nullptr;
	TAttribute<FSlateColor> ImageColorAndOpacity;
};

/** One gem of the original gem bar (InventoryInfoWindow.uc:146-150 and its
    default regions). */
struct FWoTRetailGem
{
	FName Element;
	/** The gem's label, an InventoryInfoWindow display string. */
	const TCHAR* LabelKey;
	/** The label's nudge from centred over its gem. */
	float LabelOffsetX;
	/** Unlit and lit texels in InfoBoxC2. */
	FBox2f Off;
	FBox2f On;
};

/** Which piece of the original art an SWoTRetailArt paints. */
enum class EWoTRetailArtPiece : uint8
{
	/** WOTWindow's frame filling the widget: corners, solid black fill and
	    edge tiles. */
	Window,
	/** The thin divider (16 units tall), inset Indent from each end. */
	Divider,
	/** The gem bar (32 units tall) and its five element gems, lit for
	    Elements. */
	GemBar,
	/** The card's backing: a dark band whose top and bottom edges carry a
	    divider each. The dividers take the widget's top and bottom 16 units
	    and the band runs between their centres. */
	Band,
};

/** Paints one piece of the original window art over its whole geometry, in
    window units. Every piece ignores input and takes no focus. */
class SWoTRetailArt : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTRetailArt)
		: _Piece(EWoTRetailArtPiece::Window)
		, _Indent(32.0f)
	{}
		SLATE_ARGUMENT(EWoTRetailArtPiece, Piece)
		SLATE_ARGUMENT(TSharedPtr<FWoTRetailArt>, Art)
		/** Divider: the inset from each end. */
		SLATE_ARGUMENT(float, Indent)
		/** GemBar: the lit elements (Earth, Air, Fire, Water, Spirit). */
		SLATE_ARGUMENT(TArray<FName>, Elements)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The gems left to right (Earth, Air, Fire, Water, Spirit), as
	    InventoryInfoWindow draws them. */
	static TConstArrayView<FWoTRetailGem> Gems();

	/** The left edge of gem Index in a gem bar Width units wide: the gems
	    sit 64 apart, the middle one centred. */
	static float GemX(float Width, int32 Index);

	/** Gem bar height in units, and the gem's width. */
	static constexpr float GemBarHeight = 32.0f;
	static constexpr float GemWidth = 64.0f;
	/** Divider height in units. */
	static constexpr float DividerHeight = 16.0f;

	/** True when this piece draws the original art rather than a flat
	    panel. */
	bool DrawsRetailArt() const;

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	void PaintWindow(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, const FLinearColor& Tint) const;
	void PaintDivider(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, const FLinearColor& Tint, float Y,
		float DividerIndent) const;
	void PaintGemBar(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, const FLinearColor& Tint) const;
	void PaintBand(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, const FLinearColor& Tint) const;

	/** Draws texel Region of Brush's texture at Position, Size units. */
	void DrawRegion(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, FSlateBrush& Brush, const FBox2f& Region,
		const FVector2f& Position, const FVector2f& Size,
		const FLinearColor& Tint) const;
	/** Tiles Region over Size from Position, cropping the last tile on each
	    axis, as the original's TileRegion does. */
	void TileRegion(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, FSlateBrush& Brush, const FBox2f& Region,
		const FVector2f& Position, const FVector2f& Size,
		const FLinearColor& Tint) const;
	/** A solid box, for the fill and every flat stand-in. */
	void DrawFlat(FSlateWindowElementList& Out, int32 Layer,
		const FGeometry& Geometry, const FVector2f& Position,
		const FVector2f& Size, const FLinearColor& Color) const;

	EWoTRetailArtPiece Piece = EWoTRetailArtPiece::Window;
	TSharedPtr<FWoTRetailArt> Art;
	float Indent = 32.0f;
	TArray<FName> Elements;
	/** One brush per texture; only the UV region changes between boxes, and
	    each box copies it when it is made. */
	mutable FSlateBrush CornerBrush;
	mutable FSlateBrush OtherBrush;
};
