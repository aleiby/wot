#include "SWoTRetailArt.h"

#include "WoTOnboardingChrome.h"
#include "WoTOnboardingSeams.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

namespace
{
/** A texel region of the original art. */
FBox2f WoTRetailArtRegion(float X, float Y, float W, float H)
{
	return FBox2f(FVector2f(X, Y), FVector2f(X + W, Y + H));
}

// WOTWindow's frame (WOTWindow.uc defaultproperties): the corners come from
// InfoBoxC1, the edge tiles from InfoBoxC2. The fill starts LOffset + 8 and
// TOffset + 8 in and stops ROffset and BOffset short of the far edges; the
// side edges run between the corners.
constexpr float WoTRetailArtLOffset = 16.0f;
constexpr float WoTRetailArtTOffset = 4.0f;
constexpr float WoTRetailArtROffset = 31.0f;
constexpr float WoTRetailArtBOffset = 20.0f;
constexpr float WoTRetailArtCorner = 64.0f;
constexpr float WoTRetailArtEdge = 16.0f;
/** InventoryInfoWindow's GemBarIndentX. */
constexpr float WoTRetailArtGemBarIndent = 7.0f;
/** The original InfoBoxC1 and InfoBoxC2 sizes. Every region here is a texel
    rectangle of those textures, so UVs divide by these sizes whatever the
    resolved texture reports: an editor still compiling a texture answers a
    placeholder's size, and a headless one answers none. */
constexpr float WoTRetailArtC1Width = 128.0f;
constexpr float WoTRetailArtC1Height = 128.0f;
constexpr float WoTRetailArtC2Width = 128.0f;
constexpr float WoTRetailArtC2Height = 256.0f;
/** The band under a card: the canvas shade a dimmed screen keeps. */
constexpr float WoTRetailArtBandOpacity = 0.74f;

FLinearColor WoTRetailArtFlatGemColor(FName Element)
{
	// Flat stand-ins only, for a build without the art.
	if (Element == FName(TEXT("Earth"))) return FLinearColor(0.30f, 0.62f, 0.28f);
	if (Element == FName(TEXT("Air"))) return FLinearColor(0.82f, 0.88f, 0.94f);
	if (Element == FName(TEXT("Fire"))) return FLinearColor(0.90f, 0.34f, 0.18f);
	if (Element == FName(TEXT("Water"))) return FLinearColor(0.28f, 0.52f, 0.90f);
	return FLinearColor(0.90f, 0.78f, 0.38f);
}
} // namespace

ESlateDrawEffect FWoTRetailArt::DrawEffectFor(const FSlateBrush& Brush)
{
	const UTexture* Texture = Cast<UTexture>(Brush.GetResourceObject());
	return Texture && !Texture->SRGB ? ESlateDrawEffect::NoGamma : ESlateDrawEffect::None;
}

void SWoTRetailImage::Construct(const FArguments& InArgs)
{
	ImageBrush = InArgs._Image;
	ImageColorAndOpacity = InArgs._ColorAndOpacity;
	SetCanTick(false);
	bCanSupportFocus = false;
}

int32 SWoTRetailImage::OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry,
	const FSlateRect&, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (ImageBrush && ImageBrush->DrawAs != ESlateBrushDrawType::NoDrawType)
	{
		const ESlateDrawEffect DrawEffects = (ShouldBeEnabled(bParentEnabled)
			? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect)
			| FWoTRetailArt::DrawEffectFor(*ImageBrush);
		const FLinearColor FinalTint = InWidgetStyle.GetColorAndOpacityTint()
			* ImageColorAndOpacity.Get().GetColor(InWidgetStyle)
			* ImageBrush->GetTint(InWidgetStyle);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(), ImageBrush, DrawEffects, FinalTint);
	}
	return LayerId;
}

FVector2D SWoTRetailImage::ComputeDesiredSize(float) const
{
	return ImageBrush ? FVector2D(ImageBrush->ImageSize.X, ImageBrush->ImageSize.Y)
		: FVector2D::ZeroVector;
}

TUniquePtr<FWoTRetailArt::FBrushEntry> FWoTRetailArt::MakeEntry(UTexture2D* Texture)
{
	if (!Texture)
	{
		return nullptr;
	}
	TUniquePtr<FBrushEntry> Entry = MakeUnique<FBrushEntry>();
	Entry->Texture.Reset(Texture);
	Entry->Brush.SetResourceObject(Texture);
	Entry->Brush.ImageSize = FVector2f(static_cast<float>(Texture->GetSizeX()),
		static_cast<float>(Texture->GetSizeY()));
	Entry->Brush.DrawAs = ESlateBrushDrawType::Image;
	Entry->Brush.Tiling = ESlateBrushTileType::NoTile;
	return Entry;
}

TSharedRef<FWoTRetailArt> FWoTRetailArt::Resolve()
{
	TSharedRef<FWoTRetailArt> Art = MakeShared<FWoTRetailArt>();
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	if (Seams.ResolveRetailTexture)
	{
		// The frame pieces draw at whole-number scales and stay crisp; the
		// selection frame draws at the guide's icon size.
		Art->CornerTexture.Reset(
			Seams.ResolveRetailTexture(FName(TEXT("InfoBoxC1")), false));
		Art->OtherTexture.Reset(
			Seams.ResolveRetailTexture(FName(TEXT("InfoBoxC2")), false));
		Art->Mask = MakeEntry(
			Seams.ResolveRetailTexture(FName(TEXT("IconMask")), false));
		Art->Frame = MakeEntry(
			Seams.ResolveRetailTexture(FName(TEXT("StatusIconFrame")), true));
	}
	return Art;
}

const FSlateBrush* FWoTRetailArt::IconMask() const
{
	return Mask.IsValid() ? &Mask->Brush : nullptr;
}

const FSlateBrush* FWoTRetailArt::SelectionFrame() const
{
	return Frame.IsValid() ? &Frame->Brush : nullptr;
}

const FSlateBrush* FWoTRetailArt::ItemIcon(FName ClassName, bool bSmooth)
{
	if (ClassName.IsNone())
	{
		return nullptr;
	}
	TMap<FName, TUniquePtr<FBrushEntry>>& Cache = Icons[bSmooth ? 1 : 0];
	if (const TUniquePtr<FBrushEntry>* Found = Cache.Find(ClassName))
	{
		return Found->IsValid() ? &(*Found)->Brush : nullptr;
	}
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	TUniquePtr<FBrushEntry> Entry = MakeEntry(Seams.ResolveItemIcon
		? Seams.ResolveItemIcon(ClassName, bSmooth) : nullptr);
	// The entry lives on the heap, so the brush keeps its address however
	// the cache grows.
	const FSlateBrush* Brush = Entry.IsValid() ? &Entry->Brush : nullptr;
	Cache.Add(ClassName, MoveTemp(Entry));
	return Brush;
}

TConstArrayView<FWoTRetailGem> SWoTRetailArt::Gems()
{
	// InventoryInfoWindow draws Earth, Air, Fire, Water, Spirit at offsets
	// -2 to 2 from the centre, each label nudged by its own offset.
	static const FWoTRetailGem GemTable[] = {
		{FName(TEXT("Earth")), TEXT("EarthStr"), -4.0f,
			WoTRetailArtRegion(0.0f, 0.0f, 64.0f, 32.0f),
			WoTRetailArtRegion(64.0f, 0.0f, 64.0f, 32.0f)},
		{FName(TEXT("Air")), TEXT("AirStr"), -1.0f,
			WoTRetailArtRegion(0.0f, 32.0f, 64.0f, 32.0f),
			WoTRetailArtRegion(64.0f, 32.0f, 64.0f, 32.0f)},
		{FName(TEXT("Fire")), TEXT("FireStr"), -2.0f,
			WoTRetailArtRegion(0.0f, 64.0f, 64.0f, 32.0f),
			WoTRetailArtRegion(64.0f, 64.0f, 64.0f, 32.0f)},
		{FName(TEXT("Water")), TEXT("WaterStr"), -1.0f,
			WoTRetailArtRegion(0.0f, 128.0f, 64.0f, 32.0f),
			WoTRetailArtRegion(64.0f, 128.0f, 64.0f, 32.0f)},
		{FName(TEXT("Spirit")), TEXT("SpiritStr"), -1.0f,
			WoTRetailArtRegion(0.0f, 96.0f, 64.0f, 32.0f),
			WoTRetailArtRegion(64.0f, 96.0f, 64.0f, 32.0f)},
	};
	return GemTable;
}

float SWoTRetailArt::GemX(float Width, int32 Index)
{
	return FMath::FloorToFloat((Width - GemWidth) * 0.5f)
		+ GemWidth * static_cast<float>(Index - 2);
}

void SWoTRetailArt::Construct(const FArguments& InArgs)
{
	Piece = InArgs._Piece;
	Art = InArgs._Art;
	Indent = InArgs._Indent;
	Elements = InArgs._Elements;
	if (Art.IsValid())
	{
		const auto Bind = [](FSlateBrush& Brush, UTexture2D* Texture,
			const FVector2f& TexelSpace)
		{
			if (Texture)
			{
				Brush.SetResourceObject(Texture);
				Brush.ImageSize = TexelSpace;
				Brush.DrawAs = ESlateBrushDrawType::Image;
				Brush.Tiling = ESlateBrushTileType::NoTile;
			}
		};
		Bind(CornerBrush, Art->InfoBoxC1(),
			FVector2f(WoTRetailArtC1Width, WoTRetailArtC1Height));
		Bind(OtherBrush, Art->InfoBoxC2(),
			FVector2f(WoTRetailArtC2Width, WoTRetailArtC2Height));
	}
	SetVisibility(EVisibility::HitTestInvisible);
	SetCanTick(false);
}

bool SWoTRetailArt::DrawsRetailArt() const
{
	if (!Art.IsValid() || !Art->InfoBoxC2())
	{
		return false;
	}
	return Piece != EWoTRetailArtPiece::Window || Art->InfoBoxC1() != nullptr;
}

FVector2D SWoTRetailArt::ComputeDesiredSize(float) const
{
	switch (Piece)
	{
	case EWoTRetailArtPiece::Divider:
		return FVector2D(0.0f, DividerHeight);
	case EWoTRetailArtPiece::GemBar:
		return FVector2D(0.0f, GemBarHeight);
	default:
		return FVector2D::ZeroVector;
	}
}

int32 SWoTRetailArt::OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry,
	const FSlateRect&, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool) const
{
	// The fade of a card, or any other parent tint, reaches the art too.
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	switch (Piece)
	{
	case EWoTRetailArtPiece::Window:
		PaintWindow(OutDrawElements, LayerId, AllottedGeometry, Tint);
		break;
	case EWoTRetailArtPiece::Divider:
		PaintDivider(OutDrawElements, LayerId, AllottedGeometry, Tint, 0.0f, Indent);
		break;
	case EWoTRetailArtPiece::GemBar:
		PaintGemBar(OutDrawElements, LayerId, AllottedGeometry, Tint);
		break;
	case EWoTRetailArtPiece::Band:
		PaintBand(OutDrawElements, LayerId, AllottedGeometry, Tint);
		break;
	}
	// Each stage draws on its own layer, so a later stage never batches under
	// an earlier one that shares its texture.
	return LayerId + 2;
}

void SWoTRetailArt::PaintWindow(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, const FLinearColor& Tint) const
{
	const FVector2f Size = FVector2f(Geometry.GetLocalSize());
	const float W = Size.X;
	const float H = Size.Y;
	const FVector2f FillAt(WoTRetailArtLOffset + 8.0f, WoTRetailArtTOffset + 8.0f);
	const FVector2f FillSize(W - WoTRetailArtLOffset - WoTRetailArtROffset,
		H - WoTRetailArtTOffset - WoTRetailArtBOffset);
	if (!DrawsRetailArt())
	{
		// A flat panel in the frame's footprint: the fill and a thin rule
		// where the edges run.
		DrawFlat(Out, Layer, Geometry, FillAt - FVector2f(8.0f, 8.0f),
			FillSize + FVector2f(16.0f, 16.0f),
			FLinearColor(0.32f, 0.29f, 0.24f) * Tint);
		DrawFlat(Out, Layer + 1, Geometry, FillAt - FVector2f(6.0f, 6.0f),
			FillSize + FVector2f(12.0f, 12.0f), FLinearColor::Black * Tint);
		return;
	}
	// FillRegion's Background region is solid black.
	DrawFlat(Out, Layer, Geometry, FillAt, FillSize, FLinearColor::Black * Tint);

	const float SideLength = H - 2.0f * WoTRetailArtCorner;
	const float TopLength = W - 2.0f * WoTRetailArtCorner;
	TileRegion(Out, Layer + 1, Geometry, OtherBrush,
		WoTRetailArtRegion(64.0f, 240.0f, 16.0f, 16.0f),
		FVector2f(WoTRetailArtLOffset, WoTRetailArtCorner),
		FVector2f(WoTRetailArtEdge, SideLength), Tint);
	TileRegion(Out, Layer + 1, Geometry, OtherBrush,
		WoTRetailArtRegion(80.0f, 240.0f, 16.0f, 16.0f),
		FVector2f(W - WoTRetailArtROffset, WoTRetailArtCorner),
		FVector2f(WoTRetailArtEdge, SideLength), Tint);
	TileRegion(Out, Layer + 1, Geometry, OtherBrush,
		WoTRetailArtRegion(96.0f, 240.0f, 16.0f, 16.0f),
		FVector2f(WoTRetailArtCorner, WoTRetailArtTOffset),
		FVector2f(TopLength, WoTRetailArtEdge), Tint);
	TileRegion(Out, Layer + 1, Geometry, OtherBrush,
		WoTRetailArtRegion(112.0f, 240.0f, 16.0f, 16.0f),
		FVector2f(WoTRetailArtCorner, H - WoTRetailArtBOffset),
		FVector2f(TopLength, WoTRetailArtEdge), Tint);

	const FVector2f CornerSize(WoTRetailArtCorner, WoTRetailArtCorner);
	DrawRegion(Out, Layer + 2, Geometry, CornerBrush,
		WoTRetailArtRegion(0.0f, 0.0f, 64.0f, 64.0f), FVector2f(0.0f, 0.0f),
		CornerSize, Tint);
	DrawRegion(Out, Layer + 2, Geometry, CornerBrush,
		WoTRetailArtRegion(64.0f, 0.0f, 64.0f, 64.0f),
		FVector2f(W - WoTRetailArtCorner, 0.0f), CornerSize, Tint);
	DrawRegion(Out, Layer + 2, Geometry, CornerBrush,
		WoTRetailArtRegion(0.0f, 64.0f, 64.0f, 64.0f),
		FVector2f(0.0f, H - WoTRetailArtCorner), CornerSize, Tint);
	DrawRegion(Out, Layer + 2, Geometry, CornerBrush,
		WoTRetailArtRegion(64.0f, 64.0f, 64.0f, 64.0f),
		FVector2f(W - WoTRetailArtCorner, H - WoTRetailArtCorner), CornerSize, Tint);
}

void SWoTRetailArt::PaintDivider(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, const FLinearColor& Tint, float Y,
	float DividerIndent) const
{
	const float W = static_cast<float>(Geometry.GetLocalSize().X);
	if (!DrawsRetailArt())
	{
		DrawFlat(Out, Layer, Geometry, FVector2f(DividerIndent, Y + 7.0f),
			FVector2f(W - 2.0f * DividerIndent, 2.0f),
			WoTOnboardingChrome::LabelColor() * Tint);
		return;
	}
	// WOTWindow.DrawSeparator: the left cap at the indent, the middle tiled,
	// the right cap ending the indent short of the far edge.
	const float Cap = 16.0f;
	DrawRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(16.0f, 224.0f, Cap, 16.0f), FVector2f(DividerIndent, Y),
		FVector2f(Cap, DividerHeight), Tint);
	TileRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(32.0f, 224.0f, 32.0f, 16.0f),
		FVector2f(DividerIndent + Cap, Y),
		FVector2f(W - 2.0f * (DividerIndent + Cap), DividerHeight), Tint);
	DrawRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(64.0f, 224.0f, Cap, 16.0f),
		FVector2f(W - DividerIndent - Cap, Y), FVector2f(Cap, DividerHeight), Tint);
}

void SWoTRetailArt::PaintGemBar(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, const FLinearColor& Tint) const
{
	const float W = static_cast<float>(Geometry.GetLocalSize().X);
	const TConstArrayView<FWoTRetailGem> GemList = Gems();
	if (!DrawsRetailArt())
	{
		DrawFlat(Out, Layer, Geometry, FVector2f(WoTRetailArtGemBarIndent, 4.0f),
			FVector2f(W - 2.0f * WoTRetailArtGemBarIndent, GemBarHeight - 8.0f),
			FLinearColor(0.10f, 0.09f, 0.08f) * Tint);
		for (int32 Index = 0; Index < GemList.Num(); ++Index)
		{
			const bool bLit = Elements.Contains(GemList[Index].Element);
			DrawFlat(Out, Layer + 1, Geometry,
				FVector2f(GemX(W, Index) + 14.0f, 9.0f), FVector2f(36.0f, 14.0f),
				(bLit ? WoTRetailArtFlatGemColor(GemList[Index].Element)
				      : FLinearColor(0.22f, 0.21f, 0.20f)) * Tint);
		}
		return;
	}
	// The bar is a separator of the gem caps and middle, GemBarIndentX in.
	const float Cap = GemWidth;
	DrawRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(0.0f, 192.0f, Cap, 32.0f),
		FVector2f(WoTRetailArtGemBarIndent, 0.0f), FVector2f(Cap, GemBarHeight), Tint);
	TileRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(0.0f, 224.0f, 16.0f, 32.0f),
		FVector2f(WoTRetailArtGemBarIndent + Cap, 0.0f),
		FVector2f(W - 2.0f * (WoTRetailArtGemBarIndent + Cap), GemBarHeight), Tint);
	DrawRegion(Out, Layer, Geometry, OtherBrush,
		WoTRetailArtRegion(64.0f, 192.0f, Cap, 32.0f),
		FVector2f(W - WoTRetailArtGemBarIndent - Cap, 0.0f),
		FVector2f(Cap, GemBarHeight), Tint);
	for (int32 Index = 0; Index < GemList.Num(); ++Index)
	{
		const FWoTRetailGem& Gem = GemList[Index];
		DrawRegion(Out, Layer + 1, Geometry, OtherBrush,
			Elements.Contains(Gem.Element) ? Gem.On : Gem.Off,
			FVector2f(GemX(W, Index), 0.0f), FVector2f(GemWidth, GemBarHeight), Tint);
	}
}

void SWoTRetailArt::PaintBand(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, const FLinearColor& Tint) const
{
	const FVector2f Size = FVector2f(Geometry.GetLocalSize());
	const float Half = DividerHeight * 0.5f;
	DrawFlat(Out, Layer, Geometry, FVector2f(0.0f, Half),
		FVector2f(Size.X, Size.Y - DividerHeight),
		FLinearColor(0.0f, 0.0f, 0.0f, WoTRetailArtBandOpacity) * Tint);
	PaintDivider(Out, Layer + 1, Geometry, Tint, 0.0f, 0.0f);
	PaintDivider(Out, Layer + 1, Geometry, Tint, Size.Y - DividerHeight, 0.0f);
}

void SWoTRetailArt::DrawRegion(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, FSlateBrush& Brush, const FBox2f& Region,
	const FVector2f& Position, const FVector2f& Size, const FLinearColor& Tint) const
{
	const FVector2f TextureSize = FVector2f(Brush.GetImageSize());
	if (TextureSize.X <= 0.0f || TextureSize.Y <= 0.0f || Size.X <= 0.0f
		|| Size.Y <= 0.0f)
	{
		return;
	}
	Brush.SetUVRegion(FBox2f(Region.Min / TextureSize, Region.Max / TextureSize));
	FSlateDrawElement::MakeBox(Out, Layer,
		Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)), &Brush,
		FWoTRetailArt::DrawEffectFor(Brush), Tint);
}

void SWoTRetailArt::TileRegion(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, FSlateBrush& Brush, const FBox2f& Region,
	const FVector2f& Position, const FVector2f& Size, const FLinearColor& Tint) const
{
	const FVector2f Tile = Region.GetSize();
	if (Tile.X <= 0.0f || Tile.Y <= 0.0f)
	{
		return;
	}
	for (float Y = 0.0f; Y < Size.Y; Y += Tile.Y)
	{
		for (float X = 0.0f; X < Size.X; X += Tile.X)
		{
			const FVector2f Crop(FMath::Min(Tile.X, Size.X - X),
				FMath::Min(Tile.Y, Size.Y - Y));
			DrawRegion(Out, Layer, Geometry, Brush,
				FBox2f(Region.Min, Region.Min + Crop), Position + FVector2f(X, Y),
				Crop, Tint);
		}
	}
}

void SWoTRetailArt::DrawFlat(FSlateWindowElementList& Out, int32 Layer,
	const FGeometry& Geometry, const FVector2f& Position, const FVector2f& Size,
	const FLinearColor& Color) const
{
	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return;
	}
	FSlateDrawElement::MakeBox(Out, Layer,
		Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)),
		FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, Color);
}
