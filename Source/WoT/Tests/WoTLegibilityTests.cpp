// Steam Deck text legibility (controller sub-project 3, PRs 5a and 7). Valve's
// checklist, Display > Text Legibility: no on-screen character under 9
// physical pixels at 1280x800. The game's own targets (FWoTMenuStyle) are
// stricter: every text at least 12 physical pixels, and text read during play
// at least 16. These specs measure the game's texts at the Deck's 1280x800,
// its 720p output, 1080p and 1440p through the functions the game draws them
// with, and pin that 1080p and up draw exactly as before.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../UI/SWoTButtonPrompts.h"
#include "../WoTCanvasScale.h"
#include "../WoTCitadelTeamOverview.h"
#include "../WoTHUD.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTRetailText.h"
#include "../WoTScoreboard.h"
#include "WoTTestWorld.h"

#include "Engine/Font.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Layout/ArrangedChildren.h"
#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr EAutomationTestFlags WoTLegibility_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

// Steam Deck, its 720p dock and TV output, and the 1080p and 1440p desktops.
const FVector2D WoTLegibility_Resolutions[] = {
	FVector2D(1280.0, 800.0), FVector2D(1280.0, 720.0), FVector2D(1920.0, 1080.0),
	FVector2D(2560.0, 1440.0)};

const FName WoTLegibility_Reg14(TEXT("F_WOTReg14"));
const FName WoTLegibility_Ita14(TEXT("F_WOTIta14"));
const FName WoTLegibility_Reg30(TEXT("F_WOTReg30"));

FString WoTLegibility_Label(const FVector2D& Resolution)
{
	return FString::Printf(TEXT("%gx%g"), Resolution.X, Resolution.Y);
}

void WoTLegibility_CollectArranged(const TSharedRef<SWidget>& Widget,
	const FGeometry& Geometry, FName Type, TArray<FArrangedWidget>& Out)
{
	if (Widget->GetType() == Type) Out.Emplace(Widget, Geometry);
	FArrangedChildren Children(EVisibility::Visible);
	Widget->ArrangeChildren(Geometry, Children);
	for (const FArrangedWidget& Child : Children.GetInternalArray())
	{
		WoTLegibility_CollectArranged(Child.Widget, Child.Geometry, Type, Out);
	}
}

TSharedRef<FWoTRequiredMenuFonts> WoTLegibility_Fonts(
	const TSharedRef<FWoTRequiredMenuFontGeneration>& Generation)
{
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	return Fonts;
}

// A page that uses every %f= face, small lines included: a localized help
// page may select faces 0 and 1, which the English one never does.
FWoTHelpContent WoTLegibility_EveryFaceContent(bool bAbsoluteColumn)
{
	FWoTHelpContent Content;
	Content.Title = TEXT("Help");
	Content.SubTitle = TEXT("Press F1 to proceed");
	Content.Content = {
		TEXT("%f=3"), TEXT("%s=4"), TEXT("%c=0"),
		TEXT("A body line in the list face."),
		TEXT("%f=0"), TEXT("%c=1"), TEXT("A small line, face zero."),
		TEXT("%f=1"), TEXT("%c=0"),
		TEXT("Another small line in face one that runs long enough to wrap ")
		TEXT("across the 512-unit clip of the help window at least once."),
		TEXT("%f=2"), TEXT("%s=4"), TEXT("%c=1"), TEXT("An italic heading"),
		TEXT("%f=4"), TEXT("Large"),
		TEXT("%f=5"), TEXT("Large too")};
	if (bAbsoluteColumn)
	{
		// The retail F-key column: six small lines placed at y=325.
		Content.Content.Append({TEXT("%f=0"), TEXT("%c=0"), TEXT("%y=325"),
			TEXT("F1:"), TEXT("%y=-1"), TEXT("F2:"), TEXT("F3:"), TEXT("F4:"),
			TEXT("F5:"), TEXT("F6:")});
	}
	return Content;
}

// The retail page with every list-face command switched to face 0: the
// worst case for a localized page that drew its body in the 8-pixel face.
FWoTHelpContent WoTLegibility_SmallFaceVariant(const FWoTHelpContent& Retail)
{
	FWoTHelpContent Variant = Retail;
	for (FString& Command : Variant.Content)
	{
		if (Command == TEXT("%f=3")) Command = TEXT("%f=0");
	}
	return Variant;
}

// Lines, title and subtitle of a page drawn at ScreenPixelsPerUnit are all at
// least Floor and, when the page was measured in glyph widths and line
// heights true to the faces drawn, the page fits its clip.
void WoTLegibility_CheckPage(FAutomationTestBase& Test, const FString& Label,
	const FWoTHelpPageLayout& Page, float ScreenPixelsPerUnit, bool bCheckFit,
	float Floor = FWoTMenuStyle::MinimumLegiblePixels)
{
	float Smallest = Page.TitleSize.Y * ScreenPixelsPerUnit;
	Smallest = FMath::Min(Smallest, Page.SubTitleSize.Y * ScreenPixelsPerUnit);
	for (const FWoTHelpLayoutLine& Line : Page.Lines)
	{
		const float Physical = Line.Height * ScreenPixelsPerUnit;
		Smallest = FMath::Min(Smallest, Physical);
		if (Physical < Floor - UE_KINDA_SMALL_NUMBER)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: help line \"%s\" (face %d) is %g physical pixels"),
				*Label, *Line.Text, Line.FontIndex, Physical));
		}
	}
	Test.TestTrue(FString::Printf(
		TEXT("%s: the smallest help line is at least %g physical pixels (%g)"),
		*Label, Floor, Smallest), Smallest >= Floor - UE_KINDA_SMALL_NUMBER);
	if (bCheckFit)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: the help page fits its %gx%g clip"),
			*Label, Page.ClipSize.X, Page.ClipSize.Y), Page.FitsClip());
	}
}

// The scale Help draws its page at on a screen: the reflow's 2x where the
// 640x480 window would draw at 1x, else the window's own scale.
int32 WoTLegibility_HelpDrawScale(const FVector2D& Resolution)
{
	return SWoTHelpOverlay::ReflowsOn(Resolution) ? SWoTHelpOverlay::ReflowDrawScale
		: SWoTHelpOverlay::PixelScaleFor(Resolution);
}

// A short page with two headed sections, an absolutely placed block and a
// face-0 line: every part of the reflow, small enough to fit in stand-in faces.
FWoTHelpContent WoTLegibility_ReflowContent()
{
	FWoTHelpContent Content;
	Content.Title = TEXT("Help");
	Content.SubTitle = TEXT("Press F1 to proceed");
	Content.Content = {
		TEXT("%f=3"), TEXT("%s=4"), TEXT("%c=0"), TEXT("An introduction line."),
		TEXT("%f=2"), TEXT("%s=4"), TEXT("%c=1"), TEXT("First section"),
		TEXT("%f=3"), TEXT("%c=0"), TEXT("Line one"), TEXT("Line two"),
		TEXT("%f=2"), TEXT("%s=4"), TEXT("%c=1"), TEXT("Second section"),
		TEXT("%f=3"), TEXT("%c=0"), TEXT("Line three"),
		TEXT("%f=0"), TEXT("A small line"),
		TEXT("%f=3"), TEXT("%y=325"), TEXT("F1:"), TEXT("%y=-1"), TEXT("F2:"),
		TEXT("%x=30"), TEXT("%y=325"), TEXT("Help"), TEXT("%y=-1"), TEXT("Info")};
	return Content;
}

// The game's HUD canvas at Resolution: the logical size inside its integer
// upscale (FWoTCanvasScale::ApplyLogicalDimensions) and the text scale.
struct FWoTLegibility_HUDCanvas
{
	int32 Upscale = 1;
	int32 TextScale = 1;
	int32 SizeX = 0;
	int32 SizeY = 0;
};

FWoTLegibility_HUDCanvas WoTLegibility_HUDCanvasFor(const FVector2D& Resolution)
{
	FWoTLegibility_HUDCanvas Canvas;
	const int32 X = FMath::RoundToInt32(Resolution.X);
	const int32 Y = FMath::RoundToInt32(Resolution.Y);
	Canvas.Upscale = FWoTCanvasScale::ComputeUpscale(X, Y, 0);
	Canvas.TextScale = AWoTHUD::ComputeHUDTextScale(X, Y, 0,
		static_cast<float>(FWoTMenuStyle::RetailFaceLineHeight(WoTLegibility_Reg14)));
	Canvas.SizeX = X / Canvas.Upscale;
	Canvas.SizeY = Y / Canvas.Upscale;
	return Canvas;
}

const TCHAR* WoTLegibility_BoardModeName(EWoTScoreboardBoardMode Mode)
{
	switch (Mode)
	{
	case EWoTScoreboardBoardMode::Double: return TEXT("2x");
	case EWoTScoreboardBoardMode::DoubleCompact: return TEXT("2x compact");
	default: return TEXT("1x");
	}
}

// Two layouts draw the same: the same texts and textures at the same places.
bool WoTLegibility_SameCommands(const TArray<FWoTHUDDrawCmd>& A,
	const TArray<FWoTHUDDrawCmd>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].Text != B[Index].Text || A[Index].TexId != B[Index].TexId
			|| A[Index].X != B[Index].X || A[Index].Y != B[Index].Y
			|| A[Index].TextAlignment != B[Index].TextAlignment)
		{
			return false;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityHelpLineHeightsSpec,
	"WoT.UI.Legibility.HelpLineHeights", WoTLegibility_Flags)

bool FWoTLegibilityHelpLineHeightsSpec::RunTest(const FString&)
{
	const FName Reg08(TEXT("F_WOTReg08"));
	const FName Reg14(TEXT("F_WOTReg14"));
	TArray<TPair<FString, FWoTHelpContent>> Pages;
	Pages.Emplace(TEXT("every-face page"), WoTLegibility_EveryFaceContent(true));
	FWoTHelpContent Retail;
	if (FWoTRetailText::EnsureLoaded() && FWoTHelpContent::LoadRuntime(Retail))
	{
		Pages.Emplace(TEXT("retail page"), Retail);
		Pages.Emplace(TEXT("retail page in face 0"),
			WoTLegibility_SmallFaceVariant(Retail));
	}
	else
	{
		AddWarning(TEXT("retail localization is unavailable (set WOT_REFERENCE_ROOT); ")
			TEXT("checking the every-face page only"));
	}

	// The 1x fallback (a page that cannot reflow; none shipped) swaps the
	// 8-pixel face for the 14-pixel one, so nothing draws under the floor.
	TestEqual(TEXT("at 1x small lines take the next retail face"),
		FWoTHelpContent::FaceFor(0, 1), Reg14);
	TestTrue(TEXT("the 1x 8-pixel strike is under the 12-pixel floor"),
		FWoTHelpContent::RetailLineHeight(Reg08) < FWoTMenuStyle::FloorLegiblePixels);
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const int32 PixelScale = SWoTHelpOverlay::PixelScaleFor(Resolution);
		const bool bReflow = SWoTHelpOverlay::ReflowsOn(Resolution);
		const int32 DrawScale = WoTLegibility_HelpDrawScale(Resolution);
		TestEqual(Res + TEXT(": Help reflows exactly where the window would draw at 1x"),
			bReflow, PixelScale == 1);
		TestEqual(Res + TEXT(": Help draws at 2x"), DrawScale, 2);
		// Retail drew faces 0 and 1 in F_WOTReg08 at the window's scale.
		const int32 RetailSmallPixels =
			FWoTHelpContent::RetailLineHeight(Reg08) * PixelScale;
		const FName SmallFace = FWoTHelpContent::FaceFor(0, DrawScale);
		const int32 SmallPixels =
			FWoTHelpContent::RetailLineHeight(SmallFace) * DrawScale;
		AddInfo(FString::Printf(
			TEXT("%s: window scale %d, page drawn at %d%s; small help lines %d px in retail, %d px now (%s); ")
			TEXT("body %d px (was %d)"),
			*Res, PixelScale, DrawScale, bReflow ? TEXT(" reflowed full-screen") : TEXT(""),
			RetailSmallPixels, SmallPixels, *SmallFace.ToString(),
			FWoTHelpContent::RetailLineHeight(Reg14) * DrawScale,
			FWoTHelpContent::RetailLineHeight(Reg14) * PixelScale));
		TestEqual(Res + TEXT(": face 1 draws like face 0"),
			FWoTHelpContent::FaceFor(1, DrawScale), SmallFace);
		TestEqual(Res + TEXT(": at 2x the small lines keep their own face"),
			SmallFace, Reg08);
		// Only the small face changes; the others keep GetStrFont's choice.
		TestEqual(Res + TEXT(": face 2 is the italic"),
			FWoTHelpContent::FaceFor(2, DrawScale), FName(TEXT("F_WOTIta14")));
		TestEqual(Res + TEXT(": face 3 is the list face"),
			FWoTHelpContent::FaceFor(3, DrawScale), Reg14);
		TestEqual(Res + TEXT(": face 4 is the large face"),
			FWoTHelpContent::FaceFor(4, DrawScale), FName(TEXT("F_WOTReg30")));

		// Line heights here are the retail faces' own; widths are a rough mean
		// advance that over-wraps the real page, so its fit is measured glyph
		// by glyph in HelpRetailGlyphs. The every-face page fits either way.
		// Every line is play-time text: at least 16 physical pixels.
		for (const TPair<FString, FWoTHelpContent>& Page : Pages)
		{
			WoTLegibility_CheckPage(*this, Res + TEXT(" ") + Page.Key,
				bReflow
					? FWoTHelpContent::LayoutReflow(Page.Value, DrawScale,
						SWoTHelpOverlay::ReflowClipSizeFor(Resolution))
					: FWoTHelpContent::LayoutPage(Page.Value, DrawScale),
				static_cast<float>(DrawScale),
				Page.Key == TEXT("every-face page"),
				FWoTMenuStyle::PrimaryLegiblePixels);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityHelpRetailGlyphsSpec,
	"WoT.UI.Legibility.HelpRetailGlyphs", WoTLegibility_Flags)

bool FWoTLegibilityHelpRetailGlyphsSpec::RunTest(const FString&)
{
	// The converted retail faces carry the shipped glyph cells; the runtime
	// font owner's line heights and advances match them glyph for glyph
	// (WoT.FrontEnd.Font.DeveloperRetailOracle).
	const TCHAR* FaceNames[] = {
		TEXT("F_WOTReg08"), TEXT("F_WOTReg14"), TEXT("F_WOTIta14"), TEXT("F_WOTReg30")};
	TMap<FName, UFont*> Faces;
	TMap<FName, float> LineHeights;
	for (const TCHAR* FaceName : FaceNames)
	{
		UFont* Font = LoadObject<UFont>(nullptr, *FString::Printf(
			TEXT("/Game/WoT/UI/Fonts/%s.%s"), FaceName, FaceName));
		if (Font == nullptr || Font->Characters.IsEmpty())
		{
			AddWarning(FString::Printf(TEXT("converted retail face %s is absent ")
				TEXT("(run the HUD font import); skipping the real-glyph checks"),
				FaceName));
			return true;
		}
		int32 Tallest = 0;
		for (const FFontCharacter& Character : Font->Characters)
		{
			Tallest = FMath::Max(Tallest, Character.VSize);
		}
		TestEqual(FString(FaceName) + TEXT(" line height matches the shipped face"),
			FWoTHelpContent::RetailLineHeight(FName(FaceName)), Tallest);
		Faces.Add(FName(FaceName), Font);
		LineHeights.Add(FName(FaceName), static_cast<float>(Tallest));
	}

	FWoTHelpContent Retail;
	if (!FWoTRetailText::EnsureLoaded() || !FWoTHelpContent::LoadRuntime(Retail))
	{
		AddWarning(TEXT("retail localization is unavailable (set WOT_REFERENCE_ROOT); ")
			TEXT("skipping the real help page"));
		return !HasAnyErrors();
	}
	TArray<TPair<FString, FWoTHelpContent>> Pages;
	Pages.Emplace(TEXT("retail page"), Retail);
	Pages.Emplace(TEXT("retail page in face 0"), WoTLegibility_SmallFaceVariant(Retail));
	Pages.Emplace(TEXT("every-face page"), WoTLegibility_EveryFaceContent(true));
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const bool bReflow = SWoTHelpOverlay::ReflowsOn(Resolution);
		const int32 DrawScale = WoTLegibility_HelpDrawScale(Resolution);
		// Real glyph advances and cell heights of the face each line draws in.
		const auto Measure = [&Faces, &LineHeights, DrawScale](
			const FString& Text, int32 FontIndex)
		{
			const FName Face = FWoTHelpContent::FaceFor(FontIndex, DrawScale);
			const UFont* Font = Faces.FindChecked(Face);
			float Width = 0.0f;
			for (const TCHAR Character : Text)
			{
				const int32 Code = static_cast<int32>(Character);
				if (Font->Characters.IsValidIndex(Code))
				{
					Width += static_cast<float>(Font->Characters[Code].USize);
				}
			}
			return FVector2D(Width, LineHeights.FindChecked(Face));
		};
		for (const TPair<FString, FWoTHelpContent>& Entry : Pages)
		{
			const FString Label = WoTLegibility_Label(Resolution) + TEXT(" ")
				+ Entry.Key;
			// The page Help draws: reflowed across the screen at 2x where the
			// window would draw at 1x (it must fit there: a page that did not
			// would fall back to the 14-pixel window), else the retail window.
			const FWoTHelpPageLayout Page = bReflow
				? FWoTHelpContent::LayoutReflow(Entry.Value, Measure,
					SWoTHelpOverlay::ReflowClipSizeFor(Resolution))
				: FWoTHelpContent::LayoutPage(Entry.Value, Measure);
			WoTLegibility_CheckPage(*this, Label, Page, static_cast<float>(DrawScale), true,
				FWoTMenuStyle::PrimaryLegiblePixels);
			float ContentEnd = 0.0f;
			for (const FWoTHelpLayoutLine& Line : Page.Lines)
			{
				ContentEnd = FMath::Max(ContentEnd, Line.Y + Line.Height);
			}
			AddInfo(FString::Printf(TEXT("%s: %s, %d lines, content ends at %g, subtitle %g-%g ")
				TEXT("of a %gx%g clip; text spans %g of %g screen pixels"),
				*Label, bReflow ? TEXT("reflowed") : TEXT("retail window"), Page.Lines.Num(),
				ContentEnd, Page.SubTitleY, Page.SubTitleY + Page.SubTitleSize.Y,
				Page.ClipSize.X, Page.ClipSize.Y,
				(Page.SubTitleY + Page.SubTitleSize.Y) * DrawScale, Resolution.Y));
			if (bReflow && Entry.Key == TEXT("retail page"))
			{
				// The shipped page's two columns: Basic and Advanced Movement
				// on the left, Special Keys on the right, the F-key table below.
				const FWoTHelpLayoutLine* Special = Page.Lines.FindByPredicate(
					[](const FWoTHelpLayoutLine& Line) { return Line.FontIndex == 2
						&& Line.Text.Contains(TEXT("Special")); });
				const FWoTHelpLayoutLine* Basic = Page.Lines.FindByPredicate(
					[](const FWoTHelpLayoutLine& Line) { return Line.FontIndex == 2
						&& Line.Text.Contains(TEXT("Basic")); });
				const FWoTHelpLayoutLine* F1 = Page.Lines.FindByPredicate(
					[](const FWoTHelpLayoutLine& Line) { return Line.Text == TEXT("F1:"); });
				const float ColumnWidth = (Page.ClipSize.X - FWoTHelpContent::ReflowColumnGap) * 0.5f;
				float ColumnsBottom = 0.0f;
				for (const FWoTHelpLayoutLine& Line : Page.Lines)
				{
					if (F1 && Line.Y < F1->Y) ColumnsBottom = FMath::Max(ColumnsBottom, Line.Y + Line.Height);
				}
				if (TestTrue(Label + TEXT(": the headings and the F-key table are laid out"),
					Special && Basic && F1))
				{
					TestTrue(Label + TEXT(": the first heading heads the left column"),
						Basic->X + Basic->Width <= ColumnWidth + 0.5f);
					TestTrue(Label + TEXT(": Special Keys heads the right column"),
						Special->X >= ColumnWidth + FWoTHelpContent::ReflowColumnGap - 0.5f
						&& FMath::IsNearlyEqual(Special->Y, Basic->Y, 0.5f));
					TestTrue(Label + TEXT(": the F-key table sits below both columns"),
						F1->Y >= ColumnsBottom + FWoTHelpContent::ReflowBlockGap - 0.5f);
				}
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityHelpWidgetSpec,
	"WoT.UI.Legibility.HelpWidget", WoTLegibility_Flags)

bool FWoTLegibilityHelpWidgetSpec::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate is initialized"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	const TSharedRef<FWoTRequiredMenuFonts> Fonts = WoTLegibility_Fonts(Generation);
	// The retail faces need the payload, so the overlay draws its stand-in
	// faces here (taller and wider than the retail bitmaps, so a short page);
	// HelpLineHeights and HelpRetailGlyphs measure the retail ones.
	const FWoTHelpContent Content = WoTLegibility_ReflowContent();
	const auto Settle = [](const TSharedRef<SWoTHelpOverlay>& Help,
		const FVector2D& Resolution)
	{
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const FGeometry Geometry = FGeometry::MakeRoot(
			Resolution / Dpi, FSlateLayoutTransform(Dpi));
		Help->Tick(Geometry, 0.0, 0.0f);
		Help->SlatePrepass(Dpi);
		return Geometry;
	};
	const auto Check = [this, &Fonts](const TSharedRef<SWoTHelpOverlay>& Help,
		const FGeometry& Geometry, const FString& Label, const FVector2D& Resolution)
	{
		const bool bReflow = SWoTHelpOverlay::ReflowsOn(Resolution);
		const int32 ExpectedScale = WoTLegibility_HelpDrawScale(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		TestEqual(Label + TEXT(": the window's whole-number scale"),
			Help->PixelScaleForTesting(), SWoTHelpOverlay::PixelScaleFor(Resolution));
		TestEqual(Label + (bReflow ? TEXT(": the page reflows across the screen")
			: TEXT(": the retail window is kept")), Help->IsReflowedForTesting(), bReflow);
		TestEqual(Label + TEXT(": the page draws at 2x"), Help->DrawScaleForTesting(),
			ExpectedScale);
		TArray<FArrangedWidget> Canvases;
		WoTLegibility_CollectArranged(Help, Geometry,
			FName(TEXT("SConstraintCanvas")), Canvases);
		if (!TestEqual(Label + TEXT(": one help canvas"), Canvases.Num(), 1)) return;
		const float Scale =
			Canvases[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
		TestTrue(FString::Printf(TEXT("%s: the canvas draws %d screen pixels per unit (%g)"),
			*Label, ExpectedScale, Scale),
			FMath::IsNearlyEqual(Scale, static_cast<float>(ExpectedScale), 0.001f));
		// The clip on screen: the full screen less the frame's padding when
		// reflowed, the retail 512x432 clip at 2x (1024x864) otherwise.
		const FSlateRect Clip = Canvases[0].Geometry.GetLayoutBoundingRect();
		const FVector2D Expected = bReflow
			? FVector2D(Resolution.X - 2.0 * SWoTHelpOverlay::ReflowPaddingX * ExpectedScale,
				Resolution.Y - 2.0 * SWoTHelpOverlay::ReflowPaddingY * ExpectedScale)
			: FVector2D(FWoTHelpContent::ClipWidth * ExpectedScale,
				FWoTHelpContent::ClipHeight * ExpectedScale);
		TestTrue(FString::Printf(TEXT("%s: the text clip is %gx%g on screen (%gx%g)"), *Label,
			Expected.X, Expected.Y, Clip.GetSize().X, Clip.GetSize().Y),
			FMath::IsNearlyEqual(Clip.GetSize().X, Expected.X, 2.5)
			&& FMath::IsNearlyEqual(Clip.GetSize().Y, Expected.Y, 2.5));
		// The widget's own measure, in the stand-in faces: every line is at
		// least the primary 16 pixels, and a reflowed page fits its clip.
		const FWoTHelpPageLayout& Page = Help->PageLayoutForTesting();
		WoTLegibility_CheckPage(*this, Label, Page, Scale, bReflow,
			FWoTMenuStyle::PrimaryLegiblePixels);
		// The canvas holds exactly this layout: the title, each line, the subtitle.
		TArray<FArrangedWidget> Texts;
		WoTLegibility_CollectArranged(Canvases[0].Widget, Canvases[0].Geometry,
			FName(TEXT("STextBlock")), Texts);
		TestEqual(Label + TEXT(": one text per laid-out line"),
			Texts.Num(), Page.Lines.Num() + 2);
		// Face 0 draws through FaceFor: at 2x the 8-pixel face's own strike.
		TestEqual(Label + TEXT(": the small line's font size"),
			Help->FontForIndexForTesting(0).Size, 8.0f);
		if (bReflow)
		{
			const auto Find = [&Page](const TCHAR* Text)
			{
				return Page.Lines.FindByPredicate([Text](const FWoTHelpLayoutLine& Line)
				{
					return Line.Text == Text;
				});
			};
			const FWoTHelpLayoutLine* First = Find(TEXT("First section"));
			const FWoTHelpLayoutLine* Second = Find(TEXT("Second section"));
			const FWoTHelpLayoutLine* Block = Find(TEXT("F1:"));
			const float ColumnWidth =
				(Page.ClipSize.X - FWoTHelpContent::ReflowColumnGap) * 0.5f;
			if (TestTrue(Label + TEXT(": both sections and the placed block are laid out"),
				First && Second && Block))
			{
				TestTrue(Label + TEXT(": the sections sit side by side"),
					First->X + First->Width <= ColumnWidth + 0.5f
					&& Second->X >= ColumnWidth + FWoTHelpContent::ReflowColumnGap - 0.5f
					&& FMath::IsNearlyEqual(First->Y, Second->Y, 0.5f));
				TestTrue(Label + TEXT(": the placed block follows the columns"),
					Block->Y > First->Y && Block->Y > Second->Y);
			}
		}
		// The Close prompt: the list face's 2x strike at 28 physical pixels
		// where the window would draw at 1x; elsewhere the window's own face.
		const FSlateFontInfo Prompt = Help->PromptFontForTesting();
		if (bReflow)
		{
			const float Pixels = FWoTMenuStyle::PhysicalTextPixels(Prompt.Size, Dpi);
			TestTrue(FString::Printf(TEXT("%s: the Close prompt is 28 physical pixels (%.2f)"),
				*Label, Pixels), FMath::IsNearlyEqual(Pixels,
					2.0f * Fonts->List.Size, 0.01f));
		}
		else
		{
			TestEqual(Label + TEXT(": the Close prompt keeps the window's face"),
				Prompt.Size, Help->FontForIndexForTesting(3).Size);
		}
	};

	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Label = WoTLegibility_Label(Resolution) + TEXT(" help widget");
		const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
			.Content(Content)
			.InitialPhysicalSize(Resolution)
			.InitialDpiScale(FWoTMenuStyle::ViewportDpiScale(Resolution))
			.RequiredFontsForTesting(Fonts);
		TestEqual(Label + TEXT(": the first frame lays out at its own scale"),
			Help->DrawScaleForTesting(), WoTLegibility_HelpDrawScale(Resolution));
		TestEqual(Label + TEXT(": the first frame already reflows where it will"),
			Help->IsReflowedForTesting(), SWoTHelpOverlay::ReflowsOn(Resolution));
		Check(Help, Settle(Help, Resolution), Label, Resolution);
	}

	// A resize across 1280x960 switches between the retail window and the
	// reflowed page, laying the page out again each way.
	const TSharedRef<SWoTHelpOverlay> Resized = SNew(SWoTHelpOverlay)
		.Content(Content)
		.InitialPhysicalSize(FVector2D(1920.0, 1080.0))
		.InitialDpiScale(1.0f)
		.RequiredFontsForTesting(Fonts);
	Settle(Resized, FVector2D(1920.0, 1080.0));
	const FVector2D RetailClip = Resized->PageLayoutForTesting().ClipSize;
	Check(Resized, Settle(Resized, FVector2D(1280.0, 800.0)),
		TEXT("1920x1080 resized to 1280x800"), FVector2D(1280.0, 800.0));
	TestEqual(TEXT("the resize laid the page out for the screen's clip"),
		Resized->PageLayoutForTesting().ClipSize,
		SWoTHelpOverlay::ReflowClipSizeFor(FVector2D(1280.0, 800.0)));
	Check(Resized, Settle(Resized, FVector2D(1280.0, 720.0)),
		TEXT("1280x800 resized to 1280x720"), FVector2D(1280.0, 720.0));
	TestEqual(TEXT("a reflowed page follows a resize between small screens"),
		Resized->PageLayoutForTesting().ClipSize,
		SWoTHelpOverlay::ReflowClipSizeFor(FVector2D(1280.0, 720.0)));
	Check(Resized, Settle(Resized, FVector2D(1920.0, 1080.0)),
		TEXT("1280x720 resized back to 1920x1080"), FVector2D(1920.0, 1080.0));
	TestEqual(TEXT("resizing back restores the retail clip"),
		Resized->PageLayoutForTesting().ClipSize, RetailClip);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityItemInfoGemsSpec,
	"WoT.UI.Legibility.ItemInfoGems", WoTLegibility_Flags)

bool FWoTLegibilityItemInfoGemsSpec::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate is initialized"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	const float SmallSize = FWoTMenuStyle::Get().SmallFont().Size;
	const float Floor = FWoTMenuStyle::MinimumLegiblePixels;
	// The floor itself: the smallest whole size at or above the base that
	// reaches 9 pixels, and the base wherever the base already does.
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const float Size = FWoTMenuStyle::LegibleFontSize(SmallSize, Dpi);
		const float Before = FWoTMenuStyle::PhysicalTextPixels(SmallSize, Dpi);
		const float After = FWoTMenuStyle::PhysicalTextPixels(Size, Dpi);
		AddInfo(FString::Printf(
			TEXT("%s: UI DPI scale %.4f; gem text size %g (%.2f px) was %g (%.2f px)"),
			*Res, Dpi, Size, After, SmallSize, Before));
		TestTrue(Res + TEXT(": the floored size reaches the floor"),
			After >= Floor - UE_KINDA_SMALL_NUMBER);
		TestTrue(Res + TEXT(": the floored size is the smallest whole size that does"),
			Size == SmallSize
				|| FWoTMenuStyle::PhysicalTextPixels(Size - 1.0f, Dpi) < Floor);
		TestEqual(Res + TEXT(": the floor is a whole point size"),
			Size, FMath::RoundToFloat(Size));
	}
	TestEqual(TEXT("1080p keeps the retail size"), FWoTMenuStyle::LegibleFontSize(
		SmallSize, FWoTMenuStyle::ViewportDpiScale(FVector2D(1920.0, 1080.0))), SmallSize);

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAngrealInvFireball* Fireball = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("an artifact with element gems"), Fireball)) return false;
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	const TSharedRef<FWoTRequiredMenuFonts> Fonts = WoTLegibility_Fonts(Generation);
	// Measure the layout in the small face players get: a guarded build has no
	// editor font registry and selects Roboto at the same size. The editor's
	// own offline F_WOTReg08 asset is not a face Slate can shape.
	FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const FSlateFontInfo EditorSmall = Style.SmallFont();
	Style.SetSmallFontForTesting(FWoTMenuFontSelection::Select(
		nullptr, SmallSize, FName(TEXT("Regular"))));
	ON_SCOPE_EXIT { FWoTMenuStyle::Get().SetSmallFontForTesting(EditorSmall); };

	const auto Settle = [](const TSharedRef<SWoTItemInfoOverlay>& Overlay,
		const FVector2D& Resolution)
	{
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const FGeometry Geometry = FGeometry::MakeRoot(
			Resolution / Dpi, FSlateLayoutTransform(Dpi));
		Overlay->Tick(Geometry, 0.0, 0.0f);
		Overlay->SlatePrepass(Dpi);
		return Geometry;
	};
	const auto CheckGems = [this, SmallSize, Floor](
		const TSharedRef<SWoTItemInfoOverlay>& Overlay, const FGeometry& Geometry,
		const FString& Label, bool bFloored)
	{
		TArray<FArrangedWidget> Texts;
		WoTLegibility_CollectArranged(Overlay, Geometry, FName(TEXT("STextBlock")), Texts);
		// The gem row: five equal FillWidth slots across the 720-unit body.
		TArray<FArrangedWidget> Rows;
		WoTLegibility_CollectArranged(Overlay, Geometry, FName(TEXT("SHorizontalBox")), Rows);
		float GemSlot = 0.0f;
		for (const FArrangedWidget& Row : Rows)
		{
			if (Row.Widget->GetChildren()->Num() == 5)
			{
				GemSlot = Row.Geometry.GetLocalSize().X / 5.0f;
			}
		}
		TestTrue(Label + TEXT(": the gem row is laid out"), GemSlot > 0.0f);
		int32 Gems = 0;
		float WidestGem = 0.0f;
		for (const FArrangedWidget& Arranged : Texts)
		{
			const TSharedRef<STextBlock> Text =
				StaticCastSharedRef<STextBlock>(Arranged.Widget);
			const FString Shown = Text->GetText().ToString();
			if (!Shown.StartsWith(TEXT("[ON] ")) && !Shown.StartsWith(TEXT("[--] ")))
			{
				continue;
			}
			++Gems;
			const FSlateFontInfo Font = Text->GetFont();
			const float Scale =
				Arranged.Geometry.GetAccumulatedLayoutTransform().GetScale();
			const float Physical = FWoTMenuStyle::PhysicalTextPixels(Font.Size, Scale);
			const FString Gem = FString::Printf(TEXT("%s: gem \"%s\""), *Label, *Shown);
			if (bFloored)
			{
				// SP3 PR 7: on Deck-class screens the element names take the
				// body's 2x strike (28 physical pixels; PR 5a floored the small
				// face to 9), well over the floors.
				TestTrue(FString::Printf(TEXT("%s is at least %g physical pixels (%.2f at size %g)"),
					*Gem, FWoTMenuStyle::PrimaryLegiblePixels, Physical, Font.Size),
					Physical >= FWoTMenuStyle::PrimaryLegiblePixels - UE_KINDA_SMALL_NUMBER
					&& Physical >= Floor);
				TestTrue(FString::Printf(TEXT("%s draws the list face's 2x strike (%.2f px)"),
					*Gem, Physical), FMath::IsNearlyEqual(Physical, 28.0f, 0.01f));
			}
			else
			{
				TestEqual(Gem + TEXT(" keeps the retail size"), Font.Size, SmallSize);
				TestTrue(Gem + TEXT(" draws exactly as before"), FMath::IsNearlyEqual(
					Physical, FWoTMenuStyle::PhysicalTextPixels(SmallSize, Scale), 0.01f));
			}
			// Mouse and keyboard layout intact: each gem still fits its fifth
			// of the row, so nothing is clipped or pushed into a neighbour.
			const float Desired = Text->GetDesiredSize().X;
			TestTrue(FString::Printf(TEXT("%s fits its slot (%g of %g)"), *Gem, Desired, GemSlot),
				Desired <= GemSlot + 0.5f
				&& Desired <= Arranged.Geometry.GetLocalSize().X + 0.5f);
			WidestGem = FMath::Max(WidestGem, Desired);
		}
		TestEqual(Label + TEXT(": five element gems"), Gems, 5);
		AddInfo(FString::Printf(TEXT("%s: widest gem %.1f of its %.1f-unit slot"),
			*Label, WidestGem, GemSlot));
		// The whole window stays on screen.
		const FSlateRect Screen = Geometry.GetLayoutBoundingRect();
		TArray<FArrangedWidget> Borders;
		WoTLegibility_CollectArranged(Overlay, Geometry, FName(TEXT("SBorder")), Borders);
		TestTrue(Label + TEXT(": the item-info frame is drawn"), Borders.Num() >= 2);
		for (const FArrangedWidget& Border : Borders)
		{
			const FSlateRect Rect = Border.Geometry.GetLayoutBoundingRect();
			TestTrue(FString::Printf(TEXT("%s: the window stays on screen (%g,%g-%g,%g)"),
				*Label, Rect.Left, Rect.Top, Rect.Right, Rect.Bottom),
				Rect.Left >= Screen.Left - 0.5f && Rect.Top >= Screen.Top - 0.5f
				&& Rect.Right <= Screen.Right + 0.5f && Rect.Bottom <= Screen.Bottom + 0.5f);
		}
	};

	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Label = WoTLegibility_Label(Resolution) + TEXT(" item info");
		const TSharedRef<SWoTItemInfoOverlay> Overlay = SNew(SWoTItemInfoOverlay)
			.PlayerController(nullptr).Item(Fireball)
			.InitialPhysicalSize(Resolution)
			.RequiredFontsForTesting(Fonts);
		TestEqual(Label + TEXT(": the first frame already uses the viewport's DPI scale"),
			Overlay->DpiScaleForTesting(), FWoTMenuStyle::ViewportDpiScale(Resolution));
		CheckGems(Overlay, Settle(Overlay, Resolution), Label, Resolution.Y < 1080.0);
	}

	// The style is built once; the floor follows a live resize both ways.
	const TSharedRef<SWoTItemInfoOverlay> Resized = SNew(SWoTItemInfoOverlay)
		.PlayerController(nullptr).Item(Fireball)
		.InitialPhysicalSize(FVector2D(1920.0, 1080.0))
		.RequiredFontsForTesting(Fonts);
	CheckGems(Resized, Settle(Resized, FVector2D(1280.0, 720.0)),
		TEXT("1920x1080 resized to 1280x720"), true);
	TestEqual(TEXT("the resize took the new viewport's DPI scale"),
		Resized->DpiScaleForTesting(),
		FWoTMenuStyle::ViewportDpiScale(FVector2D(1280.0, 720.0)));
	CheckGems(Resized, Settle(Resized, FVector2D(1920.0, 1080.0)),
		TEXT("1280x720 resized back to 1920x1080"), false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityTargetsSpec,
	"WoT.UI.Legibility.Targets", WoTLegibility_Flags)

bool FWoTLegibilityTargetsSpec::RunTest(const FString&)
{
	// The guard rails every other spec here measures against.
	TestEqual(TEXT("the floor: every text at least 12 physical pixels"),
		FWoTMenuStyle::FloorLegiblePixels, 12.0f);
	TestEqual(TEXT("play-time text at least 16 physical pixels"),
		FWoTMenuStyle::PrimaryLegiblePixels, 16.0f);
	TestTrue(TEXT("Valve's 9-pixel floor is under both"),
		FWoTMenuStyle::MinimumLegiblePixels < FWoTMenuStyle::FloorLegiblePixels
		&& FWoTMenuStyle::FloorLegiblePixels < FWoTMenuStyle::PrimaryLegiblePixels);
	struct FFace
	{
		const TCHAR* Name;
		int32 LineHeight;
		int32 Strike;
	};
	const FFace Faces[] = {
		{TEXT("F_WOTReg08"), 8, 2}, {TEXT("F_WOTReg14"), 14, 2}, {TEXT("F_WOTIta14"), 14, 2},
		{TEXT("F_WOTReg30"), 30, 1}, {TEXT("Tahoma20"), 24, 1}};
	for (const FFace& Face : Faces)
	{
		const FName Id(Face.Name);
		TestEqual(FString(Face.Name) + TEXT(" line height"),
			FWoTMenuStyle::RetailFaceLineHeight(Id), Face.LineHeight);
		TestEqual(FString(Face.Name) + TEXT(" whole-number strike that reaches 16 pixels"),
			FWoTMenuStyle::LegibleStrikeScale(Id), Face.Strike);
		TestTrue(FString(Face.Name) + TEXT(" at that strike is at least 16 pixels"),
			Face.LineHeight * Face.Strike >= FWoTMenuStyle::PrimaryLegiblePixels);
	}
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		TestEqual(Res + TEXT(": Deck-class screens are those under 1080 pixels tall"),
			FWoTMenuStyle::IsCompactScreen(Dpi), Resolution.Y < 1080.0);
		for (const FName Face : {WoTLegibility_Reg14, WoTLegibility_Ita14, WoTLegibility_Reg30})
		{
			const int32 Line = FWoTMenuStyle::RetailFaceLineHeight(Face);
			// The stand-in carries the contract of the runtime font owner's faces:
			// Size is the face's 1x line height in pixels.
			const FSlateFontInfo Fallback =
				FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), Line);
			const FSlateFontInfo Font =
				FWoTMenuStyle::LegibleStrikeFont(nullptr, Face, Fallback, Dpi);
			const float Pixels = FWoTMenuStyle::PhysicalTextPixels(Font.Size, Dpi);
			TestTrue(FString::Printf(TEXT("%s: %s draws its %d-pixel strike 1:1 (%.3f px)"),
				*Res, *Face.ToString(), Line * FWoTMenuStyle::LegibleStrikeScale(Face), Pixels),
				FMath::IsNearlyEqual(Pixels,
					static_cast<float>(Line * FWoTMenuStyle::LegibleStrikeScale(Face)), 0.01f));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityHUDTextSpec,
	"WoT.UI.Legibility.HUDText", WoTLegibility_Flags)

bool FWoTLegibilityHUDTextSpec::RunTest(const FString&)
{
	// The canvas HUD's retail text: messages (chat, kills, pickups, center,
	// subtitle, generic), the Select Team prompt, both scoreboards and the
	// citadel overview with their timers, progress lines and player names. All
	// draw F_WOTReg14 at the HUD's integer upscale times its text scale.
	const float Line = static_cast<float>(
		FWoTMenuStyle::RetailFaceLineHeight(WoTLegibility_Reg14));
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const FWoTLegibility_HUDCanvas Canvas = WoTLegibility_HUDCanvasFor(Resolution);
		const float Before = Line * Canvas.Upscale;
		const float After = Before * Canvas.TextScale;
		AddInfo(FString::Printf(TEXT("%s: HUD upscale %d, text scale %d: HUD text %g px ")
			TEXT("(was %g px); icon digits stay %g px (health, charges) and %g px (hand keys)"),
			*Res, Canvas.Upscale, Canvas.TextScale, After, Before, 14.0f * Canvas.Upscale,
			22.0f * Canvas.Upscale));
		TestTrue(FString::Printf(TEXT("%s: HUD text is at least 16 physical pixels (%g)"),
			*Res, After), After >= FWoTMenuStyle::PrimaryLegiblePixels);
		if (Canvas.Upscale >= 2)
		{
			TestEqual(Res + TEXT(": a 2x-and-up HUD draws its text exactly as before"),
				Canvas.TextScale, 1);
		}
		else
		{
			TestEqual(Res + TEXT(": a 1x HUD draws its text at 2x"), Canvas.TextScale, 2);
		}
		// The digits composed onto the 64-pixel 1x icons stay at 1x (a known
		// exception to the 16-pixel target): they still pass the floor.
		TestTrue(Res + TEXT(": the icon digits pass the 12-pixel floor"),
			14.0f * Canvas.Upscale >= FWoTMenuStyle::FloorLegiblePixels);
	}
	// wot.HUDScale forcing a 1x HUD on a desktop screen gets the 2x text.
	TestEqual(TEXT("a forced 1x HUD at 1920x1080 draws its text at 2x"),
		AWoTHUD::ComputeHUDTextScale(1920, 1080, 1, Line), 2);
	TestEqual(TEXT("a forced 2x HUD at 2560x1440 draws its text at 1x"),
		AWoTHUD::ComputeHUDTextScale(2560, 1440, 2, Line), 1);

	// Select Team: centred and three text heights down, both in scaled text,
	// so 1280x800 puts it where 1080p's 2x HUD does (84 physical pixels down).
	const FWoTLegibility_HUDCanvas Deck = WoTLegibility_HUDCanvasFor(FVector2D(1280.0, 800.0));
	const FWoTLegibility_HUDCanvas Desktop = WoTLegibility_HUDCanvasFor(FVector2D(1920.0, 1080.0));
	const FVector2D DeckPrompt = AWoTHUD::TeamSelectPosition(
		static_cast<float>(Deck.SizeX), 200.0f * Deck.TextScale, Line * Deck.TextScale);
	const FVector2D DesktopPrompt = AWoTHUD::TeamSelectPosition(
		static_cast<float>(Desktop.SizeX), 200.0f, Line);
	TestEqual(TEXT("Select Team sits as low on the Deck as at 1080p"),
		DeckPrompt.Y * Deck.Upscale, DesktopPrompt.Y * Desktop.Upscale);
	TestEqual(TEXT("Select Team is centred with its scaled width"),
		DeckPrompt.X, (Deck.SizeX - 400.0) * 0.5);

	// Progress lines: the retail pitch (a line and a pixel) scales with the text.
	TArray<FWoTProgressEntry> Entries;
	Entries.AddDefaulted(3);
	Entries[0].Message = TEXT("Connecting");
	Entries[2].Message = TEXT("example.invalid");
	const TArray<FWoTHUDDrawCmd> Scaled = AWoTHUD::LayoutProgressMessages(
		Entries, Deck.SizeX, Deck.SizeY, Line, Deck.TextScale);
	const TArray<FWoTHUDDrawCmd> Native = AWoTHUD::LayoutProgressMessages(
		Entries, Desktop.SizeX, Desktop.SizeY, Line, Desktop.TextScale);
	if (TestEqual(TEXT("two progress lines on the Deck"), Scaled.Num(), 2)
		&& TestEqual(TEXT("two progress lines at 1080p"), Native.Num(), 2))
	{
		TestEqual(TEXT("Deck progress text draws at 2x"), Scaled[0].Scale, 2.0f);
		TestEqual(TEXT("Deck progress pitch is two scaled lines"),
			Scaled[1].Y - Scaled[0].Y, 2.0f * (Line + 1.0f) * 2.0f);
		TestEqual(TEXT("1080p progress keeps its 1x text"), Native[0].Scale, 1.0f);
		TestEqual(TEXT("1080p progress keeps the retail pitch"),
			Native[1].Y - Native[0].Y, 2.0f * (Line + 1.0f));
	}

	// The developer readout is exempt: it draws the engine's 8-pixel
	// SmallFont at native size outside the HUD scale, only behind a
	// developer console variable that ships off.
	const IConsoleVariable* Debug =
		IConsoleManager::Get().FindConsoleVariable(TEXT("wot.HUDDebug"));
	TestTrue(TEXT("the exempt developer readout is off by default"),
		Debug != nullptr && Debug->GetInt() == 0);
	return !HasAnyErrors();
}

namespace
{
FWoTScoreboardSnapshot WoTLegibility_ArenaSnapshot(int32 Players, bool bVictory)
{
	FWoTScoreboardSnapshot Snapshot;
	Snapshot.MapName = TEXT("Arena_01");
	Snapshot.bDisplayRemaining = true;
	Snapshot.RemainingTime = 600;
	if (bVictory)
	{
		Snapshot.VictoryMessage = TEXT("[[ The match is won ]]");
	}
	for (int32 Index = 0; Index < Players; ++Index)
	{
		FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.Name = FString::Printf(TEXT("Player %d"), Index + 1);
		Player.Score = Players - Index;
		Player.bHighlight = Index == 0;
	}
	return Snapshot;
}

FWoTScoreboardSnapshot WoTLegibility_CitadelSnapshot(int32 Teams, int32 PerTeam,
	bool bVictory)
{
	FWoTScoreboardSnapshot Snapshot;
	Snapshot.MapName = TEXT("Battle2_03");
	Snapshot.bDisplayRemaining = true;
	Snapshot.RemainingTime = 600;
	Snapshot.SealGoal = 4;
	if (bVictory)
	{
		Snapshot.VictoryMessage = TEXT("[[ Victory goes to Team #1 ]]");
	}
	for (int32 Team = 0; Team < Teams; ++Team)
	{
		for (int32 Index = 0; Index < PerTeam; ++Index)
		{
			FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
			Player.Name = FString::Printf(TEXT("Player %d-%d"), Team + 1, Index + 1);
			Player.Team = static_cast<uint8>(Team);
			Player.TeamSealCount = Teams - Team;
			Player.TeamScore = (Teams - Team) * 10;
			Player.Score = PerTeam - Index;
		}
	}
	return Snapshot;
}

TArray<FString> WoTLegibility_BoardHeaders(const FWoTScoreboardSnapshot& Snapshot,
	bool bCitadel)
{
	TArray<FString> Headers = {Snapshot.MapName,
		FString(TEXT("Remaining Time: ")) + FWoTArenaScoreboard::FormatTime(Snapshot.RemainingTime)};
	if (bCitadel)
	{
		Headers.Add(FString::Printf(TEXT("Number of seals to win: %d"), Snapshot.SealGoal));
	}
	if (!Snapshot.VictoryMessage.IsEmpty())
	{
		Headers.Add(Snapshot.VictoryMessage);
	}
	return Headers;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityScoreboardsSpec,
	"WoT.UI.Legibility.Scoreboards", WoTLegibility_Flags)

bool FWoTLegibilityScoreboardsSpec::RunTest(const FString&)
{
	// The scoreboards' measure-then-fallback order below a 2x HUD: the board
	// at 2x on half the canvas, then its compact form at 2x, then the retail
	// board at 1x (FWoTScoreboardBoardChoice). The full matrix is logged.
	using EMode = EWoTScoreboardBoardMode;
	struct FCase
	{
		const TCHAR* Label;
		bool bCitadel;
		int32 Teams;
		int32 Players;
		bool bVictory;
		EMode At800;
		EMode At720;
	};
	const FCase Cases[] = {
		{TEXT("Arena, 1 player"), false, 0, 1, false, EMode::Double, EMode::Double},
		{TEXT("Arena, 8 players"), false, 0, 8, false, EMode::Double, EMode::Double},
		{TEXT("Arena, 12 players"), false, 0, 12, false, EMode::Double, EMode::Double},
		{TEXT("Arena, 14 players"), false, 0, 14, false, EMode::Double, EMode::DoubleCompact},
		{TEXT("Arena, 15 players"), false, 0, 15, false, EMode::DoubleCompact, EMode::DoubleCompact},
		{TEXT("Arena, 16 players"), false, 0, 16, false, EMode::DoubleCompact, EMode::DoubleCompact},
		{TEXT("Arena, 16 players at match end"), false, 0, 16, true, EMode::DoubleCompact, EMode::Native},
		{TEXT("Citadel, 2 teams of 3"), true, 2, 3, false, EMode::Double, EMode::Double},
		{TEXT("Citadel, 2 teams of 3 at match end"), true, 2, 3, true, EMode::Double, EMode::Double},
		{TEXT("Citadel, 2 teams of 7"), true, 2, 7, false, EMode::Double, EMode::DoubleCompact},
		{TEXT("Citadel, 2 teams of 8"), true, 2, 8, false, EMode::DoubleCompact, EMode::Native},
		{TEXT("Citadel, 3 teams of 5"), true, 3, 5, false, EMode::DoubleCompact, EMode::Native},
		{TEXT("Citadel, 4 teams of 4"), true, 4, 4, false, EMode::Native, EMode::Native},
	};
	const float Line = static_cast<float>(
		FWoTMenuStyle::RetailFaceLineHeight(WoTLegibility_Reg14));
	const FWoTArenaScoreboard ArenaBoard;
	const FWoTCitadelScoreboard CitadelBoard;
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const FWoTLegibility_HUDCanvas Canvas = WoTLegibility_HUDCanvasFor(Resolution);
		for (const FCase& Case : Cases)
		{
			const FString Label = Res + TEXT(" ") + Case.Label;
			const FWoTScoreboardSnapshot Snapshot = Case.bCitadel
				? WoTLegibility_CitadelSnapshot(Case.Teams, Case.Players, Case.bVictory)
				: WoTLegibility_ArenaSnapshot(Case.Players, Case.bVictory);
			const FWoTArenaScoreboard& Board = Case.bCitadel
				? static_cast<const FWoTArenaScoreboard&>(CitadelBoard) : ArenaBoard;
			const FWoTScoreboardBoard Drawn = Board.LayoutBoard(Snapshot,
				Canvas.SizeX, Canvas.SizeY, Line, Canvas.TextScale, false, true);
			const float TextPixels = Line * Drawn.BoardScale * Canvas.Upscale;
			AddInfo(FString::Printf(TEXT("%s: %s board %dx%d, text %g px"), *Label,
				WoTLegibility_BoardModeName(Drawn.Mode), Drawn.LogicalWidth,
				Drawn.LogicalHeight, TextPixels));
			TestTrue(FString::Printf(TEXT("%s: every board passes the 12-pixel floor (%g)"),
				*Label, TextPixels), TextPixels >= FWoTMenuStyle::FloorLegiblePixels);
			if (Canvas.TextScale == 1)
			{
				// 1080p and up: the retail board, exactly as before.
				TestTrue(Label + TEXT(": the retail board at the face's size"),
					Drawn.Mode == EMode::Native && Drawn.BoardScale == 1);
				TestTrue(Label + TEXT(": drawn exactly as the retail layout"),
					WoTLegibility_SameCommands(Drawn.Commands, Board.Layout(Snapshot,
						FMath::Max(640, Canvas.SizeX), FMath::Max(480, Canvas.SizeY),
						Line, false, true)));
				continue;
			}
			const EMode Expected = Resolution.Y == 800.0 ? Case.At800 : Case.At720;
			TestTrue(FString::Printf(TEXT("%s: the %s board (got %s)"), *Label,
				WoTLegibility_BoardModeName(Expected), WoTLegibility_BoardModeName(Drawn.Mode)),
				Drawn.Mode == Expected);
			if (Drawn.Mode == EMode::Native)
			{
				// The explicit fallback: the retail board at 1x, as before.
				TestTrue(Label + TEXT(": the 1x fallback is the retail layout"),
					Drawn.BoardScale == 1 && WoTLegibility_SameCommands(Drawn.Commands,
						Board.Layout(Snapshot, FMath::Max(640, Canvas.SizeX),
							FMath::Max(480, Canvas.SizeY), Line, false, true)));
				continue;
			}
			TestTrue(Label + TEXT(": the 2x board is half the canvas"),
				Drawn.BoardScale == 2 && Drawn.LogicalWidth == Canvas.SizeX / 2
				&& Drawn.LogicalHeight == Canvas.SizeY / 2);
			TestTrue(FString::Printf(TEXT("%s: the 2x text is at least 16 pixels (%g)"),
				*Label, TextPixels), TextPixels >= FWoTMenuStyle::PrimaryLegiblePixels);
			const float Bottom = FWoTScoreboardBoardChoice::Bottom(Drawn.Commands, Line,
				static_cast<float>(FWoTArenaScoreboard::IconSize));
			TestTrue(FString::Printf(TEXT("%s: everything is on screen (ends at %g of %d)"),
				*Label, Bottom, Drawn.LogicalHeight), Bottom <= Drawn.LogicalHeight + 0.01f);
			for (const FString& Header : WoTLegibility_BoardHeaders(Snapshot, Case.bCitadel))
			{
				TestTrue(FString::Printf(TEXT("%s: the header \"%s\" shows"), *Label, *Header),
					Drawn.Commands.ContainsByPredicate([&Header](const FWoTHUDDrawCmd& Command)
					{
						return Command.Text == Header;
					}));
			}
			const int32 Rows = Drawn.Commands.FilterByPredicate(
				[](const FWoTHUDDrawCmd& Command) { return Command.Text.StartsWith(TEXT("Player ")); })
				.Num();
			TestEqual(Label + TEXT(": every player's row is drawn"), Rows,
				Case.bCitadel ? Case.Teams * Case.Players : Case.Players);
		}

		// The citadel team overview (at most four teams) always fits 2x.
		FWoTCitadelTeamOverviewSnapshot Overview;
		Overview.MapName = TEXT("Battle2_03");
		Overview.TeamLegend = TEXT("Team");
		Overview.PlayersLegend = TEXT("Players");
		Overview.StatusLegend = TEXT("Status");
		Overview.BeginBattleText = TEXT("Begin");
		Overview.bAllTeamsReady = true;
		for (int32 Team = 0; Team < FWoTCitadelTeamOverview::MaxTeams; ++Team)
		{
			FWoTCitadelTeamOverviewRow& Row = Overview.Teams.AddDefaulted_GetRef();
			Row.NumPlayers = 4;
			Row.MaxTeamSize = 4;
			Row.Status = TEXT("Ready");
		}
		const FWoTCitadelTeamOverview OverviewBoard;
		const FWoTScoreboardBoard Drawn = OverviewBoard.LayoutBoard(Overview,
			Canvas.SizeX, Canvas.SizeY, Line, Canvas.TextScale);
		AddInfo(FString::Printf(TEXT("%s citadel overview: %s board, text %g px"), *Res,
			WoTLegibility_BoardModeName(Drawn.Mode), Line * Drawn.BoardScale * Canvas.Upscale));
		if (Canvas.TextScale == 1)
		{
			TestTrue(Res + TEXT(" citadel overview: the retail board, exactly as before"),
				Drawn.Mode == EMode::Native && WoTLegibility_SameCommands(Drawn.Commands,
					OverviewBoard.Layout(Overview, FMath::Max(640, Canvas.SizeX),
						FMath::Max(480, Canvas.SizeY), Line)));
		}
		else
		{
			TestTrue(Res + TEXT(" citadel overview: four teams fit the 2x board"),
				Drawn.Mode == EMode::Double && FWoTScoreboardBoardChoice::Bottom(
					Drawn.Commands, Line, 0.0f) <= Drawn.LogicalHeight + 0.01f);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityPromptGlyphLetterSpec,
	"WoT.UI.Legibility.PromptGlyphLetter", WoTLegibility_Flags)

bool FWoTLegibilityPromptGlyphLetterSpec::RunTest(const FString&)
{
	// A face button's letter is half its glyph and never under the 12-pixel
	// floor at the live layout scale; the glyph grows to hold it. Labels at
	// their 16-pixel floor already give a larger letter, so every existing
	// host draws as before.
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const int32 FloorPoints = SWoTButtonPrompts::LetterFloorPoints(Dpi);
		TestTrue(FString::Printf(TEXT("%s: the letter floor is the smallest whole size at 12 pixels (%d)"),
			*Res, FloorPoints),
			FWoTMenuStyle::PhysicalTextPixels(FloorPoints, Dpi)
				>= FWoTMenuStyle::FloorLegiblePixels - 0.01f
			&& FWoTMenuStyle::PhysicalTextPixels(FloorPoints - 1, Dpi)
				< FWoTMenuStyle::FloorLegiblePixels);
		// Labels the hosts bind: Help's 1x window face (10.5), the native list
		// face (14) and the 2x strike's size here (front end, chat on the Deck).
		const float BoundSizes[] = {8.0f, 10.5f, 14.0f, 28.0f * 0.75f / Dpi};
		for (const float Bound : BoundSizes)
		{
			const FSlateFontInfo Label = SWoTButtonPrompts::LegibleLabelFont(
				FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), Bound), Dpi);
			const float Side = SWoTButtonPrompts::GlyphSizeFor(Label, Dpi);
			const FSlateFontInfo Letter = SWoTButtonPrompts::LetterFontFor(Side, Dpi);
			const float LetterPixels = FWoTMenuStyle::PhysicalTextPixels(Letter.Size, Dpi);
			const FString Case = FString::Printf(TEXT("%s: label %g pt (%.1f px)"), *Res,
				Label.Size, FWoTMenuStyle::PhysicalTextPixels(Label.Size, Dpi));
			TestTrue(FString::Printf(TEXT("%s: its glyph letter is at least 12 pixels (%.1f)"),
				*Case, LetterPixels), LetterPixels >= FWoTMenuStyle::FloorLegiblePixels - 0.01f);
			TestTrue(FString::Printf(TEXT("%s: the letter fits its %.1f-unit glyph"), *Case, Side),
				Letter.Size * FontConstants::RenderDPI / 72.0f <= Side + 0.5f);
			// The PR 6 sizes, unchanged wherever labels meet their floor.
			TestEqual(Case + TEXT(": the glyph keeps its size"),
				Side, FMath::Max(14.0f, Label.Size * SWoTButtonPrompts::GlyphSizePerPoint));
			TestEqual(Case + TEXT(": the letter keeps its size"), Letter.Size,
				static_cast<float>(FMath::Max(6, FMath::RoundToInt(Side * 0.5f))));
		}
		// A glyph smaller than the floor allows (a host without the label
		// floor) still gets a 12-pixel letter.
		const FSlateFontInfo Tiny = SWoTButtonPrompts::LetterFontFor(14.0f, Dpi);
		TestTrue(Res + TEXT(": a 14-unit glyph's letter is raised to the floor"),
			FWoTMenuStyle::PhysicalTextPixels(Tiny.Size, Dpi)
				>= FWoTMenuStyle::FloorLegiblePixels - 0.01f);
	}
	return !HasAnyErrors();
}

namespace
{
// Greedy word wrap at WrapPixels with the converted retail face's own glyph
// advances at Scale, as Slate wraps at word boundaries.
int32 WoTLegibility_RetailLines(const FString& Text, const UFont* Font, float Scale,
	float WrapPixels)
{
	const auto Advance = [Font, Scale](TCHAR Character)
	{
		const int32 Code = static_cast<int32>(Character);
		return Font->Characters.IsValidIndex(Code)
			? static_cast<float>(Font->Characters[Code].USize) * Scale : 0.0f;
	};
	TArray<FString> Words;
	Text.ParseIntoArray(Words, TEXT(" "));
	int32 Lines = Words.IsEmpty() ? 0 : 1;
	float LineWidth = 0.0f;
	for (const FString& Word : Words)
	{
		float Width = 0.0f;
		for (const TCHAR Character : Word)
		{
			Width += Advance(Character);
		}
		if (LineWidth > 0.0f && LineWidth + Advance(TEXT(' ')) + Width > WrapPixels)
		{
			++Lines;
			LineWidth = Width;
		}
		else
		{
			LineWidth += (LineWidth > 0.0f ? Advance(TEXT(' ')) : 0.0f) + Width;
		}
	}
	return Lines;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLegibilityItemInfoSpec,
	"WoT.UI.Legibility.ItemInfo", WoTLegibility_Flags)

bool FWoTLegibilityItemInfoSpec::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate is initialized"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable (set WOT_REFERENCE_ROOT); ")
			TEXT("the worst-case texts are placeholders"));
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// The audit's worst cases: the longest description and the longest quote.
	AWoTInventory* Items[] = {
		World->SpawnActor<AWoTAngrealInvLightning>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn),
		World->SpawnActor<AWoTAngrealInvFireShield>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn)};
	for (AWoTInventory* Item : Items)
	{
		if (!TestNotNull(TEXT("a worst-case artifact"), Item)) return false;
		AddInfo(FString::Printf(TEXT("%s: description %d characters, quote %d"),
			*Item->GetClass()->GetName(), Item->Description.Len(), Item->Quote.Len()));
	}

	// The editor's own offline small and italic assets are not faces Slate can
	// shape; swap in what a guarded build resolves, as ItemInfoGems does.
	FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const FSlateFontInfo EditorSmall = Style.SmallFont();
	const FSlateFontInfo EditorItalic = Style.ItalicFont();
	ON_SCOPE_EXIT
	{
		FWoTMenuStyle::Get().SetSmallFontForTesting(EditorSmall);
		FWoTMenuStyle::Get().SetItalicFontForTesting(EditorItalic);
	};
	Style.SetSmallFontForTesting(FWoTMenuFontSelection::Select(
		nullptr, EditorSmall.Size, FName(TEXT("Regular"))));
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	const auto Settle = [](const TSharedRef<SWoTItemInfoOverlay>& Overlay,
		const FVector2D& Resolution)
	{
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const FGeometry Geometry = FGeometry::MakeRoot(
			Resolution / Dpi, FSlateLayoutTransform(Dpi));
		Overlay->Tick(Geometry, 0.0, 0.0f);
		Overlay->SlatePrepass(Dpi);
		return Geometry;
	};

	// Sizes: the stand-ins carry the runtime faces' contract (Size is the 1x
	// line height), so physical pixels are what the retail strikes draw.
	const TSharedRef<FWoTRequiredMenuFonts> Fonts = WoTLegibility_Fonts(Generation);
	Style.SetItalicFontForTesting(FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 14));
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const float Dpi = FWoTMenuStyle::ViewportDpiScale(Resolution);
		const TSharedRef<SWoTItemInfoOverlay> Overlay = SNew(SWoTItemInfoOverlay)
			.PlayerController(nullptr).Item(Items[0])
			.InitialPhysicalSize(Resolution)
			.RequiredFontsForTesting(Fonts);
		Settle(Overlay, Resolution);
		const auto Pixels = [Dpi](const FSlateFontInfo& Font)
		{
			return FWoTMenuStyle::PhysicalTextPixels(Font.Size, Dpi);
		};
		AddInfo(FString::Printf(TEXT("%s: body %.1f px, quote %.1f px, element names %.1f px, ")
			TEXT("title %.1f px; content %.0f px wide, wrapping at %.0f px"), *Res,
			Pixels(Overlay->BodyFontForTesting()), Pixels(Overlay->QuoteFontForTesting()),
			Pixels(Overlay->GemFontForTesting()), Pixels(Overlay->TitleFontForTesting()),
			Overlay->ContentWidthForTesting() * Dpi, Overlay->WrapWidthForTesting() * Dpi));
		if (Resolution.Y < 1080.0)
		{
			TestTrue(Res + TEXT(": body, rarity and charges draw the 2x strike (28 px)"),
				FMath::IsNearlyEqual(Pixels(Overlay->BodyFontForTesting()), 28.0f, 0.01f));
			TestTrue(Res + TEXT(": the quote draws the italic's 2x strike (28 px)"),
				FMath::IsNearlyEqual(Pixels(Overlay->QuoteFontForTesting()), 28.0f, 0.01f));
			TestTrue(Res + TEXT(": the element names draw the 2x strike (28 px)"),
				FMath::IsNearlyEqual(Pixels(Overlay->GemFontForTesting()), 28.0f, 0.01f));
			TestTrue(Res + TEXT(": the title draws its own face 1:1 (30 px, not fractional)"),
				FMath::IsNearlyEqual(Pixels(Overlay->TitleFontForTesting()), 30.0f, 0.01f));
			TestTrue(Res + TEXT(": the text wraps at about 1000 physical pixels"),
				FMath::IsNearlyEqual(Overlay->WrapWidthForTesting() * Dpi,
					SWoTItemInfoOverlay::CompactWrapPixels, 1.0f));
		}
		else
		{
			// 1080p and up: exactly as before.
			TestEqual(Res + TEXT(": the body keeps the list face"),
				Overlay->BodyFontForTesting().Size, Fonts->List.Size);
			TestEqual(Res + TEXT(": the quote keeps the italic face"),
				Overlay->QuoteFontForTesting().Size, Style.ItalicFont().Size);
			TestEqual(Res + TEXT(": the title keeps the large face"),
				Overlay->TitleFontForTesting().Size, Fonts->Title.Size);
			TestEqual(Res + TEXT(": the element names keep the retail size 8"),
				Overlay->GemFontForTesting().Size, Style.SmallFont().Size);
			TestEqual(Res + TEXT(": the window keeps its 720-unit content"),
				Overlay->ContentWidthForTesting(), SWoTItemInfoOverlay::RetailContentWidth);
			TestEqual(Res + TEXT(": and its 600-unit wrap"),
				Overlay->WrapWidthForTesting(), SWoTItemInfoOverlay::RetailWrapWidth);
		}
	}

	// Fit: stand-ins with the retail strikes' metrics (a Roboto em of 24
	// pixels draws lines of about 28 pixels and letters about 12 wide, the
	// 2x list face's 28 and 11.6) lay out the worst cases in the real
	// widget, which must stay on screen.
	const TSharedRef<FWoTRequiredMenuFonts> Metric = MakeShared<FWoTRequiredMenuFonts>();
	Metric->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 26);
	Metric->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
	Metric->Generation = Generation;
	Style.SetItalicFontForTesting(FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 12));
	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		for (AWoTInventory* Item : Items)
		{
			const FString Label = WoTLegibility_Label(Resolution) + TEXT(" ")
				+ Item->GetClass()->GetName();
			const TSharedRef<SWoTItemInfoOverlay> Overlay = SNew(SWoTItemInfoOverlay)
				.PlayerController(nullptr).Item(Item)
				.InitialPhysicalSize(Resolution)
				.RequiredFontsForTesting(Metric);
			const FGeometry Geometry = Settle(Overlay, Resolution);
			const FSlateRect Screen = Geometry.GetLayoutBoundingRect();
			TArray<FArrangedWidget> Borders;
			WoTLegibility_CollectArranged(Overlay, Geometry, FName(TEXT("SBorder")), Borders);
			if (!TestTrue(Label + TEXT(": the window is drawn"), Borders.Num() >= 2)) continue;
			FSlateRect Window = Borders[0].Geometry.GetLayoutBoundingRect();
			for (const FArrangedWidget& Border : Borders)
			{
				const FSlateRect Rect = Border.Geometry.GetLayoutBoundingRect();
				Window = FSlateRect(FMath::Min(Window.Left, Rect.Left),
					FMath::Min(Window.Top, Rect.Top), FMath::Max(Window.Right, Rect.Right),
					FMath::Max(Window.Bottom, Rect.Bottom));
			}
			AddInfo(FString::Printf(TEXT("%s: window %.0f x %.0f px of %.0f x %.0f"), *Label,
				Window.GetSize().X, Window.GetSize().Y, Resolution.X, Resolution.Y));
			TestTrue(FString::Printf(TEXT("%s: the window stays on screen (%g,%g-%g,%g)"),
				*Label, Window.Left, Window.Top, Window.Right, Window.Bottom),
				Window.Left >= Screen.Left - 0.5f && Window.Top >= Screen.Top - 0.5f
				&& Window.Right <= Screen.Right + 0.5f && Window.Bottom <= Screen.Bottom + 0.5f);
		}
	}

	// The same texts wrapped with the retail faces' own advances, at the 2x
	// strike and the window's wrap on the Deck (the converted faces carry the
	// shipped glyph cells; WoT.UI.Legibility.HelpRetailGlyphs).
	UFont* Body = LoadObject<UFont>(nullptr, TEXT("/Game/WoT/UI/Fonts/F_WOTReg14.F_WOTReg14"));
	UFont* Italic = LoadObject<UFont>(nullptr, TEXT("/Game/WoT/UI/Fonts/F_WOTIta14.F_WOTIta14"));
	if (Body == nullptr || Italic == nullptr || Body->Characters.IsEmpty()
		|| Italic->Characters.IsEmpty())
	{
		AddWarning(TEXT("converted retail faces are absent; skipping the retail line counts"));
		return !HasAnyErrors();
	}
	for (const FVector2D& Resolution : {FVector2D(1280.0, 800.0), FVector2D(1280.0, 720.0)})
	{
		const float Wrap = SWoTItemInfoOverlay::CompactWrapPixels;
		const int32 DescriptionLines =
			WoTLegibility_RetailLines(Items[0]->Description, Body, 2.0f, Wrap);
		const int32 QuoteLines = WoTLegibility_RetailLines(Items[1]->Quote, Italic, 2.0f, Wrap);
		// Title 30, rarity and element rows 28 each, the two texts at 28 a line.
		const float TextPixels = 30.0f + 2.0f * 28.0f
			+ 28.0f * static_cast<float>(DescriptionLines + QuoteLines);
		AddInfo(FString::Printf(TEXT("%s: retail faces wrap the longest description in %d lines ")
			TEXT("and the longest quote in %d; with the title and rows that is %.0f px of text"),
			*WoTLegibility_Label(Resolution), DescriptionLines, QuoteLines, TextPixels));
		TestTrue(WoTLegibility_Label(Resolution)
			+ TEXT(": the worst-case text leaves room for the window's chrome"),
			TextPixels <= 0.75f * Resolution.Y);
	}
	return !HasAnyErrors();
}

#endif
