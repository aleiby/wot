#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "SWoTRetailArt.h"
#include "SWoTGuideBrowser.h"
#include "SWoTPickupCard.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingSeams.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Input/HittestGrid.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Rendering/DrawElements.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/RenderingCommon.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Types/PaintArgs.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWindow.h"

// The original window art the onboarding widgets paint (plan Task 8): which
// pieces draw it and which fall back to flat panels, the icon cache, and the
// gem bar's geometry. The art comes through injected seams over transient
// textures, never the player's files. Helper names carry the
// WoTOnboardingRetailArt prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingRetailArtFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

/** Saves the art seams and puts them back. */
struct FWoTOnboardingRetailArtSeamScope
{
	TFunction<UTexture2D*(FName, bool)> SavedTexture;
	TFunction<UTexture2D*(FName, bool)> SavedIcon;

	FWoTOnboardingRetailArtSeamScope()
	{
		const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
		SavedTexture = Seams.ResolveRetailTexture;
		SavedIcon = Seams.ResolveItemIcon;
	}

	~FWoTOnboardingRetailArtSeamScope()
	{
		FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
		Seams.ResolveRetailTexture = SavedTexture;
		Seams.ResolveItemIcon = SavedIcon;
	}
};

bool WoTOnboardingRetailArtDraws(const TSharedPtr<FWoTRetailArt>& Art,
	EWoTRetailArtPiece Piece)
{
	const TSharedRef<SWoTRetailArt> Widget =
		SNew(SWoTRetailArt).Piece(Piece).Art(Art);
	return Widget->DrawsRetailArt();
}

struct FWoTOnboardingRetailArtPaintedBox
{
	ESlateDrawEffect Effects;
	FBox2f UVRegion;
	FLinearColor Tint;
};

TArray<FWoTOnboardingRetailArtPaintedBox> WoTOnboardingRetailArtPaint(
	const TSharedRef<SWidget>& Widget, const FVector2D& PaintSize,
	bool bParentEnabled = true, const FWidgetStyle& PaintStyle = FWidgetStyle())
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(PaintSize).CreateTitleBar(false)[Widget];
	Widget->SlatePrepass(1.0f);
	const FGeometry Geometry = FGeometry::MakeRoot(PaintSize, FSlateLayoutTransform(1.0f));
	FHittestGrid Grid;
	FSlateWindowElementList DrawElements(Window);
	const FPaintArgs PaintArgs(&Window.Get(), Grid, FVector2f::ZeroVector,
		FApp::GetCurrentTime(), 0.0f);
	Widget->Paint(PaintArgs, Geometry, Geometry.GetLayoutBoundingRect(),
		DrawElements, 0, PaintStyle, bParentEnabled);
	TArray<FWoTOnboardingRetailArtPaintedBox> Boxes;
	for (const FSlateBoxElement& Box : DrawElements.GetUncachedDrawElements()
		.Get<(uint8)EElementType::ET_Box>())
	{
		Boxes.Add({Box.GetDrawEffects(), Box.GetBrushUVRegion(), Box.GetTint()});
	}
	return Boxes;
}

void WoTOnboardingRetailArtCheckImages(FAutomationTestBase& Spec,
	const TSharedRef<SWidget>& Widget, TArray<int32>& Counts)
{
	const FVector2D FixtureSizes[] = {FVector2D(61.0f, 59.0f),
		FVector2D(63.0f, 57.0f), FVector2D(67.0f, 53.0f)};
	for (int32 FixtureIndex = 0; FixtureIndex < UE_ARRAY_COUNT(FixtureSizes); ++FixtureIndex)
	{
		if (Widget->GetDesiredSize() == FixtureSizes[FixtureIndex])
		{
			const bool bRetailImage = Widget->GetType() == FName(TEXT("SWoTRetailImage"));
			Spec.TestTrue(FString::Printf(TEXT("%s with fixture %d's size is a retail image"),
				*Widget->GetTypeAsString(), FixtureIndex), bRetailImage);
			if (bRetailImage)
			{
				++Counts[FixtureIndex];
			}
		}
	}
	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			WoTOnboardingRetailArtCheckImages(Spec, Children->GetChildAt(ChildIndex), Counts);
		}
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtPiecesSpec,
	"WoT.Onboarding.RetailArtPieces", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtPiecesSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const EWoTRetailArtPiece Pieces[] = {EWoTRetailArtPiece::Window,
		EWoTRetailArtPiece::Divider, EWoTRetailArtPiece::GemBar,
		EWoTRetailArtPiece::Band};

	// No art seam (a build without the game module's art): flat panels.
	Seams.ResolveRetailTexture = nullptr;
	Seams.ResolveItemIcon = nullptr;
	const TSharedRef<FWoTRetailArt> None = FWoTRetailArt::Resolve();
	for (const EWoTRetailArtPiece Piece : Pieces)
	{
		TestFalse(TEXT("without the art every piece draws flat"),
			WoTOnboardingRetailArtDraws(None, Piece));
		TestFalse(TEXT("a null art draws flat too"),
			WoTOnboardingRetailArtDraws(nullptr, Piece));
	}
	TestNull(TEXT("no bevel without the art"), None->IconMask());
	TestNull(TEXT("no selection frame without the art"), None->SelectionFrame());
	TestNull(TEXT("no icon without the seam"),
		None->ItemIcon(FName(TEXT("EmberLob")), false));

	// The art resolves: every piece draws it.
	const TStrongObjectPtr<UTexture2D> Corner(UTexture2D::CreateTransient(128, 128));
	const TStrongObjectPtr<UTexture2D> Other(UTexture2D::CreateTransient(128, 256));
	const TStrongObjectPtr<UTexture2D> Small(UTexture2D::CreateTransient(64, 64));
	Corner->SRGB = false;
	Other->SRGB = false;
	Small->SRGB = false;
	TArray<TPair<FName, bool>> Asked;
	Seams.ResolveRetailTexture = [&](FName Name, bool bSmooth) -> UTexture2D*
	{
		Asked.Emplace(Name, bSmooth);
		if (Name == FName(TEXT("InfoBoxC1"))) return Corner.Get();
		if (Name == FName(TEXT("InfoBoxC2"))) return Other.Get();
		return Small.Get();
	};
	const TSharedRef<FWoTRetailArt> Full = FWoTRetailArt::Resolve();
	for (const EWoTRetailArtPiece Piece : Pieces)
	{
		TestTrue(TEXT("with the art every piece draws it"),
			WoTOnboardingRetailArtDraws(Full, Piece));
	}
	TestNotNull(TEXT("the bevel resolves"), Full->IconMask());
	TestNotNull(TEXT("the selection frame resolves"), Full->SelectionFrame());
	TestTrue(TEXT("the frame pieces resolve crisp"),
		Asked.Contains(TPair<FName, bool>(FName(TEXT("InfoBoxC1")), false))
		&& Asked.Contains(TPair<FName, bool>(FName(TEXT("InfoBoxC2")), false))
		&& Asked.Contains(TPair<FName, bool>(FName(TEXT("IconMask")), false)));
	TestTrue(TEXT("the selection frame resolves smooth"),
		Asked.Contains(TPair<FName, bool>(FName(TEXT("StatusIconFrame")), true)));

	// Only the corner texture missing: the window falls back alone.
	Seams.ResolveRetailTexture = [&](FName Name, bool) -> UTexture2D*
	{
		return Name == FName(TEXT("InfoBoxC1")) ? nullptr : Other.Get();
	};
	const TSharedRef<FWoTRetailArt> NoCorners = FWoTRetailArt::Resolve();
	TestFalse(TEXT("the window needs its corners"),
		WoTOnboardingRetailArtDraws(NoCorners, EWoTRetailArtPiece::Window));
	TestTrue(TEXT("the divider does not"),
		WoTOnboardingRetailArtDraws(NoCorners, EWoTRetailArtPiece::Divider));

	// The fixed heights the layouts reserve.
	const TSharedRef<SWoTRetailArt> Divider =
		SNew(SWoTRetailArt).Piece(EWoTRetailArtPiece::Divider).Art(Full);
	Divider->SlatePrepass(1.0f);
	TestEqual(TEXT("a divider is 16 units tall"),
		static_cast<float>(Divider->GetDesiredSize().Y), SWoTRetailArt::DividerHeight);
	const TSharedRef<SWoTRetailArt> GemBar =
		SNew(SWoTRetailArt).Piece(EWoTRetailArtPiece::GemBar).Art(Full);
	GemBar->SlatePrepass(1.0f);
	TestEqual(TEXT("a gem bar is 32 units tall"),
		static_cast<float>(GemBar->GetDesiredSize().Y), SWoTRetailArt::GemBarHeight);
	TestFalse(TEXT("the art never hit-tests"),
		GemBar->GetVisibility().IsHitTestVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtIconsSpec,
	"WoT.Onboarding.RetailArtIcons", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtIconsSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UTexture2D> Icon(UTexture2D::CreateTransient(64, 64));
	Icon->SRGB = false;
	int32 Calls = 0;
	Seams.ResolveRetailTexture = nullptr;
	Seams.ResolveItemIcon = [&](FName ClassName, bool) -> UTexture2D*
	{
		++Calls;
		return ClassName == FName(TEXT("EmberLob")) ? Icon.Get() : nullptr;
	};
	const TSharedRef<FWoTRetailArt> Art = FWoTRetailArt::Resolve();
	const FSlateBrush* Crisp = Art->ItemIcon(FName(TEXT("EmberLob")), false);
	if (!TestNotNull(TEXT("a known class has an icon"), Crisp))
	{
		return false;
	}
	const FVector2f IconSize = FVector2f(Crisp->GetImageSize());
	TestTrue(TEXT("the icon keeps its size"),
		IconSize.X == 64.0f && IconSize.Y == 64.0f);
	TestTrue(TEXT("a repeat lookup is cached"),
		Art->ItemIcon(FName(TEXT("EmberLob")), false) == Crisp && Calls == 1);
	TestNotNull(TEXT("the smooth icon resolves on its own"),
		Art->ItemIcon(FName(TEXT("EmberLob")), true));
	TestEqual(TEXT("smooth and crisp resolve once each"), Calls, 2);
	TestNull(TEXT("an unknown class has none"),
		Art->ItemIcon(FName(TEXT("NotAnItem")), false));
	TestNull(TEXT("a miss is cached as a miss"),
		Art->ItemIcon(FName(TEXT("NotAnItem")), false));
	TestEqual(TEXT("the miss resolved once"), Calls, 3);
	TestNull(TEXT("None has no icon and asks nothing"), Art->ItemIcon(NAME_None, false));
	TestEqual(TEXT("None asked nothing"), Calls, 3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtGemsSpec,
	"WoT.Onboarding.RetailArtGems", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtGemsSpec::RunTest(const FString&)
{
	const TConstArrayView<FWoTRetailGem> Gems = SWoTRetailArt::Gems();
	if (!TestEqual(TEXT("five gems"), Gems.Num(), 5))
	{
		return false;
	}
	const TCHAR* const Order[] = {TEXT("Earth"), TEXT("Air"), TEXT("Fire"),
		TEXT("Water"), TEXT("Spirit")};
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestEqual(TEXT("the gems run in the item-info window's order"),
			Gems[Index].Element, FName(Order[Index]));
		TestEqual(TEXT("each label is that element's window string"),
			FString(Gems[Index].LabelKey), FString(Order[Index]) + TEXT("Str"));
		TestEqual(TEXT("each lit gem sits beside its unlit one"),
			Gems[Index].On.Min.X - Gems[Index].Off.Min.X, 64.0f);
	}
	// The middle gem centred, the rest 64 apart.
	TestEqual(TEXT("the middle gem is centred"), SWoTRetailArt::GemX(422.0f, 2), 179.0f);
	TestEqual(TEXT("the first gem"), SWoTRetailArt::GemX(422.0f, 0), 51.0f);
	TestEqual(TEXT("the last gem"), SWoTRetailArt::GemX(422.0f, 4), 307.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailArtLinearGammaSpec,
	"WoT.Onboarding.RetailArtLinearGamma", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailArtLinearGammaSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UTexture2D> Corner(UTexture2D::CreateTransient(128, 128));
	const TStrongObjectPtr<UTexture2D> Other(UTexture2D::CreateTransient(128, 256));
	if (!TestNotNull(TEXT("corner fixture"), Corner.Get())
		|| !TestNotNull(TEXT("edge fixture"), Other.Get()))
	{
		return false;
	}
	Corner->SRGB = false;
	Other->SRGB = false;
	Seams.ResolveRetailTexture = [&](FName TextureName, bool) -> UTexture2D*
	{
		if (TextureName == FName(TEXT("InfoBoxC1"))) return Corner.Get();
		if (TextureName == FName(TEXT("InfoBoxC2"))) return Other.Get();
		return nullptr;
	};
	Seams.ResolveItemIcon = nullptr;
	const EWoTRetailArtPiece Pieces[] = {EWoTRetailArtPiece::Window,
		EWoTRetailArtPiece::Divider, EWoTRetailArtPiece::GemBar, EWoTRetailArtPiece::Band};
	const FVector2D Sizes[] = {FVector2D(422.0f, 286.0f), FVector2D(422.0f, 16.0f),
		FVector2D(422.0f, 32.0f), FVector2D(430.0f, 104.0f)};
	const TArray<FName> LitElements = {FName(TEXT("Fire")), FName(TEXT("Water"))};
	TArray<FWoTOnboardingRetailArtPaintedBox> LinearBoxes;
	for (const bool bSrgb : {false, true})
	{
		Corner->SRGB = bSrgb;
		Other->SRGB = bSrgb;
		const TSharedRef<FWoTRetailArt> Art = FWoTRetailArt::Resolve();
		TArray<FWoTOnboardingRetailArtPaintedBox> RunBoxes;
		for (int32 PieceIndex = 0; PieceIndex < UE_ARRAY_COUNT(Pieces); ++PieceIndex)
		{
			const TSharedRef<SWoTRetailArt> Widget = SNew(SWoTRetailArt)
				.Piece(Pieces[PieceIndex]).Art(Art).Elements(LitElements);
			const TArray<FWoTOnboardingRetailArtPaintedBox> Boxes =
				WoTOnboardingRetailArtPaint(Widget, Sizes[PieceIndex]);
			int32 ArtCount = 0;
			int32 FlatCount = 0;
			for (const FWoTOnboardingRetailArtPaintedBox& Box : Boxes)
			{
				const FBox2f& UV = Box.UVRegion;
				if (UV.bIsValid)
				{
					++ArtCount;
					TestTrue(TEXT("art UVs are sub-rectangles of the unit square"),
						UV.Min.X >= 0.0f && UV.Min.Y >= 0.0f
						&& UV.Max.X <= 1.0f && UV.Max.Y <= 1.0f
						&& UV.Min.X < UV.Max.X && UV.Min.Y < UV.Max.Y
						&& UV.GetArea() < 1.0f);
					if (bSrgb)
					{
						TestFalse(TEXT("sRGB art keeps display gamma"),
							EnumHasAnyFlags(Box.Effects, ESlateDrawEffect::NoGamma));
					}
					else
					{
						TestTrue(TEXT("linear art bypasses display gamma"),
							EnumHasAnyFlags(Box.Effects, ESlateDrawEffect::NoGamma));
					}
				}
				else
				{
					++FlatCount;
					TestFalse(TEXT("flat fills keep display gamma"),
						EnumHasAnyFlags(Box.Effects, ESlateDrawEffect::NoGamma));
				}
			}
			TestTrue(TEXT("each piece emits retail art"), ArtCount > 0);
			if (Pieces[PieceIndex] == EWoTRetailArtPiece::Window
				|| Pieces[PieceIndex] == EWoTRetailArtPiece::Band)
			{
				TestTrue(TEXT("window and band emit flat fills"), FlatCount > 0);
			}
			RunBoxes.Append(Boxes);

			const TSharedRef<SWoTRetailArt> Fallback = SNew(SWoTRetailArt)
				.Piece(Pieces[PieceIndex]).Elements(LitElements);
			const TArray<FWoTOnboardingRetailArtPaintedBox> FlatBoxes =
				WoTOnboardingRetailArtPaint(Fallback, Sizes[PieceIndex]);
			TestTrue(TEXT("each missing-art piece emits stand-ins"), FlatBoxes.Num() > 0);
			for (const FWoTOnboardingRetailArtPaintedBox& FlatBox : FlatBoxes)
			{
				TestFalse(TEXT("stand-ins have no art UV region"), FlatBox.UVRegion.bIsValid);
				TestFalse(TEXT("stand-ins keep display gamma"),
					EnumHasAnyFlags(FlatBox.Effects, ESlateDrawEffect::NoGamma));
			}
			RunBoxes.Append(FlatBoxes);
		}
		if (!bSrgb)
		{
			LinearBoxes = MoveTemp(RunBoxes);
		}
		else if (TestEqual(TEXT("gamma does not change box count"),
			RunBoxes.Num(), LinearBoxes.Num()))
		{
			for (int32 BoxIndex = 0; BoxIndex < RunBoxes.Num(); ++BoxIndex)
			{
				TestTrue(TEXT("gamma does not change UV regions"),
					RunBoxes[BoxIndex].UVRegion == LinearBoxes[BoxIndex].UVRegion
					&& RunBoxes[BoxIndex].UVRegion.bIsValid == LinearBoxes[BoxIndex].UVRegion.bIsValid);
				TestTrue(TEXT("gamma does not change tint"),
					RunBoxes[BoxIndex].Tint == LinearBoxes[BoxIndex].Tint);
				TestTrue(TEXT("only NoGamma differs"), RunBoxes[BoxIndex].Effects
					== (LinearBoxes[BoxIndex].Effects & ~ESlateDrawEffect::NoGamma));
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingRetailImageGammaSpec,
	"WoT.Onboarding.RetailImageGamma", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingRetailImageGammaSpec::RunTest(const FString&)
{
	const TStrongObjectPtr<UTexture2D> Texture(UTexture2D::CreateTransient(61, 59));
	if (!TestNotNull(TEXT("image fixture"), Texture.Get())) return false;
	Texture->SRGB = false;
	FSlateBrush Brush;
	Brush.SetResourceObject(Texture.Get());
	Brush.ImageSize = FVector2f(61.0f, 59.0f);
	Brush.DrawAs = ESlateBrushDrawType::Image;
	const FVector2D PaintSize(122.0f, 118.0f);
	const FLinearColor ImageTint(0.25f, 0.5f, 0.75f, 0.8f);
	const TSharedRef<SWoTRetailImage> Image =
		SNew(SWoTRetailImage).Image(&Brush).ColorAndOpacity(ImageTint);
	const TArray<FWoTOnboardingRetailArtPaintedBox> LinearBoxes =
		WoTOnboardingRetailArtPaint(Image, PaintSize);
	if (TestEqual(TEXT("linear image emits one box"), LinearBoxes.Num(), 1))
	{
		TestTrue(TEXT("linear image bypasses gamma"),
			EnumHasAnyFlags(LinearBoxes[0].Effects, ESlateDrawEffect::NoGamma));
		TestTrue(TEXT("image keeps the supplied tint"), LinearBoxes[0].Tint == ImageTint);
	}
	TestTrue(TEXT("image keeps the brush's desired size"),
		Image->GetDesiredSize() == FVector2D(61.0f, 59.0f));
	TestTrue(TEXT("image keeps default visible hit testing"),
		Image->GetVisibility() == EVisibility::Visible);
	const TArray<FWoTOnboardingRetailArtPaintedBox> DisabledBoxes =
		WoTOnboardingRetailArtPaint(Image, PaintSize, false);
	if (TestEqual(TEXT("disabled image emits one box"), DisabledBoxes.Num(), 1))
	{
		TestTrue(TEXT("disabled linear image keeps both effects"), EnumHasAllFlags(
			DisabledBoxes[0].Effects, ESlateDrawEffect::DisabledEffect | ESlateDrawEffect::NoGamma));
	}
	Brush.TintColor = FLinearColor(0.5f, 0.5f, 1.0f, 0.5f);
	FWidgetStyle TintedStyle;
	TintedStyle.BlendColorAndOpacityTint(FLinearColor(0.5f, 1.0f, 0.5f, 0.5f));
	const TArray<FWoTOnboardingRetailArtPaintedBox> TintedBoxes =
		WoTOnboardingRetailArtPaint(Image, PaintSize, true, TintedStyle);
	if (TestEqual(TEXT("tinted image emits one box"), TintedBoxes.Num(), 1))
	{
		TestTrue(TEXT("style, attribute and brush tints multiply"),
			TintedBoxes[0].Tint == FLinearColor(0.0625f, 0.25f, 0.375f, 0.2f));
	}
	Brush.TintColor = FLinearColor::White;
	Texture->SRGB = true;
	const TArray<FWoTOnboardingRetailArtPaintedBox> SrgbBoxes =
		WoTOnboardingRetailArtPaint(Image, PaintSize);
	if (TestEqual(TEXT("sRGB image emits one box"), SrgbBoxes.Num(), 1))
	{
		TestFalse(TEXT("sRGB image keeps display gamma"),
			EnumHasAnyFlags(SrgbBoxes[0].Effects, ESlateDrawEffect::NoGamma));
	}
	const TArray<FWoTOnboardingRetailArtPaintedBox> WhiteBoxes =
		WoTOnboardingRetailArtPaint(SNew(SWoTRetailImage)
			.Image(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"))), PaintSize);
	if (TestEqual(TEXT("WhiteBrush emits one box"), WhiteBoxes.Num(), 1))
	{
		TestFalse(TEXT("WhiteBrush keeps display gamma"),
			EnumHasAnyFlags(WhiteBoxes[0].Effects, ESlateDrawEffect::NoGamma));
	}
	TestEqual(TEXT("null brush emits nothing"),
		WoTOnboardingRetailArtPaint(SNew(SWoTRetailImage).Image(nullptr), PaintSize).Num(), 0);
	Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
	TestEqual(TEXT("NoDrawType emits nothing"),
		WoTOnboardingRetailArtPaint(Image, PaintSize).Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingBrowserArtWidgetsSpec,
	"WoT.Onboarding.BrowserArtWidgets", WoTOnboardingRetailArtFlags)

bool FWoTOnboardingBrowserArtWidgetsSpec::RunTest(const FString&)
{
	const FWoTOnboardingRetailArtSeamScope Scope;
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const TStrongObjectPtr<UTexture2D> Icon(UTexture2D::CreateTransient(61, 59));
	const TStrongObjectPtr<UTexture2D> Mask(UTexture2D::CreateTransient(63, 57));
	const TStrongObjectPtr<UTexture2D> Frame(UTexture2D::CreateTransient(67, 53));
	if (!TestNotNull(TEXT("icon fixture"), Icon.Get())
		|| !TestNotNull(TEXT("mask fixture"), Mask.Get())
		|| !TestNotNull(TEXT("frame fixture"), Frame.Get()))
	{
		return false;
	}
	Icon->SRGB = false;
	Mask->SRGB = false;
	Frame->SRGB = false;
	Seams.ResolveRetailTexture = [&](FName TextureName, bool) -> UTexture2D*
	{
		if (TextureName == FName(TEXT("IconMask"))) return Mask.Get();
		if (TextureName == FName(TEXT("StatusIconFrame"))) return Frame.Get();
		return nullptr;
	};
	Seams.ResolveItemIcon = [&](FName, bool) { return Icon.Get(); };
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(
		NewObject<UWoTAngrealGuide>(GetTransientPackage()));
	FWoTAngrealGuideRow Ember;
	Ember.ClassName = FName(TEXT("EmberLob"));
	Ember.Rarity = FName(TEXT("Common"));
	Ember.Elements = {FName(TEXT("Fire"))};
	Guide->Rows.Add(Ember);
	FWoTAngrealGuideRow Ward;
	Ward.ClassName = FName(TEXT("GustWard"));
	Ward.Rarity = FName(TEXT("Rare"));
	Ward.Counters = {Ember.ClassName};
	Guide->Rows.Add(Ward);
	const TSharedRef<SWoTGuideBrowser> Browser = SNew(SWoTGuideBrowser)
		.Guide(Guide.Get()).InitialRow(Ember.ClassName)
		.TextResolver([](const FString&, const FString&) { return FString(); });
	Browser->SlatePrepass(1.0f);
	TArray<int32> BrowserCounts = {0, 0, 0};
	WoTOnboardingRetailArtCheckImages(*this, Browser, BrowserCounts);
	TestTrue(TEXT("browser has grid, header and counter retail icons"), BrowserCounts[0] >= 4);
	TestTrue(TEXT("browser has a retail mask"), BrowserCounts[1] > 0);
	TestTrue(TEXT("browser has retail selection frames"), BrowserCounts[2] > 0);

	FWoTPickupCardText CardText;
	CardText.ItemClass = Ember.ClassName;
	CardText.Title = TEXT("Ember Lob");
	CardText.Summary = TEXT("Lobs an invented ember.");
	const TSharedRef<SWoTPickupCard> Card = SNew(SWoTPickupCard).Text(CardText);
	Card->SlatePrepass(1.0f);
	TArray<int32> CardCounts = {0, 0, 0};
	WoTOnboardingRetailArtCheckImages(*this, Card, CardCounts);
	TestTrue(TEXT("pickup card has a retail icon"), CardCounts[0] > 0);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
