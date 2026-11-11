// Steam Deck text legibility (controller sub-project 3, PR 5a). Valve's
// checklist, Display > Text Legibility: no on-screen character under 9
// physical pixels at 1280x800. These specs measure the two game-module texts
// that drew under it, the Help window's 8-pixel lines and the item-info
// element gems, through the functions the game draws them with.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "../Inventory/WoTAngrealInvFireball.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTRetailText.h"
#include "WoTTestWorld.h"

#include "Engine/Font.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Misc/ScopeExit.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr EAutomationTestFlags WoTLegibility_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

// Steam Deck, its 720p dock and TV output, and the 1080p baseline.
const FVector2D WoTLegibility_Resolutions[] = {
	FVector2D(1280.0, 800.0), FVector2D(1280.0, 720.0), FVector2D(1920.0, 1080.0)};

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
// least the floor and, when the page was measured in glyph widths and line
// heights true to the faces drawn, the page fits its clip.
void WoTLegibility_CheckPage(FAutomationTestBase& Test, const FString& Label,
	const FWoTHelpPageLayout& Page, float ScreenPixelsPerUnit, bool bCheckFit)
{
	const float Floor = FWoTMenuStyle::MinimumLegiblePixels;
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
		Test.TestTrue(Label + TEXT(": the help page fits its 512x432 window"),
			Page.FitsClip());
	}
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

	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Res = WoTLegibility_Label(Resolution);
		const int32 PixelScale = SWoTHelpOverlay::PixelScaleFor(Resolution);
		// Retail drew faces 0 and 1 in F_WOTReg08 at every scale.
		const int32 RetailSmallPixels =
			FWoTHelpContent::RetailLineHeight(Reg08) * PixelScale;
		const FName SmallFace = FWoTHelpContent::FaceFor(0, PixelScale);
		const int32 SmallPixels =
			FWoTHelpContent::RetailLineHeight(SmallFace) * PixelScale;
		AddInfo(FString::Printf(
			TEXT("%s: pixel scale %d; small help lines %d px in retail, %d px now (%s)"),
			*Res, PixelScale, RetailSmallPixels, SmallPixels, *SmallFace.ToString()));
		TestEqual(Res + TEXT(": face 1 draws like face 0"),
			FWoTHelpContent::FaceFor(1, PixelScale), SmallFace);
		if (PixelScale == 1)
		{
			TestTrue(Res + TEXT(": the 1x 8-pixel strike is under the floor"),
				RetailSmallPixels < FWoTMenuStyle::MinimumLegiblePixels);
			TestEqual(Res + TEXT(": small lines take the next retail face"),
				SmallFace, Reg14);
		}
		else
		{
			TestEqual(Res + TEXT(": from 1280x960 up the small lines keep their face"),
				SmallFace, Reg08);
		}
		// Only the small face changes; the others keep GetStrFont's choice.
		TestEqual(Res + TEXT(": face 2 is the italic"),
			FWoTHelpContent::FaceFor(2, PixelScale), FName(TEXT("F_WOTIta14")));
		TestEqual(Res + TEXT(": face 3 is the list face"),
			FWoTHelpContent::FaceFor(3, PixelScale), Reg14);
		TestEqual(Res + TEXT(": face 4 is the large face"),
			FWoTHelpContent::FaceFor(4, PixelScale), FName(TEXT("F_WOTReg30")));

		// Line heights here are the retail faces' own; widths are a rough mean
		// advance that over-wraps the real page, so its fit is measured glyph
		// by glyph in HelpRetailGlyphs. The every-face page fits either way.
		for (const TPair<FString, FWoTHelpContent>& Page : Pages)
		{
			WoTLegibility_CheckPage(*this, Res + TEXT(" ") + Page.Key,
				FWoTHelpContent::LayoutPage(Page.Value, PixelScale),
				static_cast<float>(PixelScale),
				Page.Key == TEXT("every-face page"));
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
		const int32 PixelScale = SWoTHelpOverlay::PixelScaleFor(Resolution);
		// Real glyph advances and cell heights of the face each line draws in.
		const auto Measure = [&Faces, &LineHeights, PixelScale](
			const FString& Text, int32 FontIndex)
		{
			const FName Face = FWoTHelpContent::FaceFor(FontIndex, PixelScale);
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
			const FWoTHelpPageLayout Page =
				FWoTHelpContent::LayoutPage(Entry.Value, Measure);
			WoTLegibility_CheckPage(*this, Label, Page, static_cast<float>(PixelScale), true);
			float ContentEnd = 0.0f;
			for (const FWoTHelpLayoutLine& Line : Page.Lines)
			{
				ContentEnd = FMath::Max(ContentEnd, Line.Y + Line.Height);
			}
			AddInfo(FString::Printf(TEXT("%s: %d lines, content ends at %g, subtitle %g-%g of 432"),
				*Label, Page.Lines.Num(), ContentEnd, Page.SubTitleY,
				Page.SubTitleY + Page.SubTitleSize.Y));
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
	// faces here; HelpLineHeights and HelpRetailGlyphs measure the retail ones.
	const FWoTHelpContent Content = WoTLegibility_EveryFaceContent(false);
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
		const FGeometry& Geometry, const FString& Label, int32 ExpectedScale)
	{
		TestEqual(Label + TEXT(": draws at the screen's whole-number scale"),
			Help->PixelScaleForTesting(), ExpectedScale);
		TArray<FArrangedWidget> Canvases;
		WoTLegibility_CollectArranged(Help, Geometry,
			FName(TEXT("SConstraintCanvas")), Canvases);
		if (!TestEqual(Label + TEXT(": one help canvas"), Canvases.Num(), 1)) return;
		const float Scale =
			Canvases[0].Geometry.GetAccumulatedLayoutTransform().GetScale();
		TestTrue(FString::Printf(TEXT("%s: the canvas draws %d screen pixels per unit (%g)"),
			*Label, ExpectedScale, Scale),
			FMath::IsNearlyEqual(Scale, static_cast<float>(ExpectedScale), 0.001f));
		// The widget's own measure, in the stand-in faces: heights only, since
		// the stand-ins are taller than the retail bitmaps the window was laid
		// out for (the subtitle alone overruns at 416 + 22).
		const FWoTHelpPageLayout& Page = Help->PageLayoutForTesting();
		WoTLegibility_CheckPage(*this, Label, Page, Scale, false);
		// The canvas holds exactly this layout: the title, each line, the subtitle.
		TArray<FArrangedWidget> Texts;
		WoTLegibility_CollectArranged(Canvases[0].Widget, Canvases[0].Geometry,
			FName(TEXT("STextBlock")), Texts);
		TestEqual(Label + TEXT(": one text per laid-out line"),
			Texts.Num(), Page.Lines.Num() + 2);
		// Face 0 draws through FaceFor: the list face at 1x, its own 8 at 2x.
		const FSlateFontInfo Small = Help->FontForIndexForTesting(0);
		TestEqual(Label + TEXT(": the small line's font size"), Small.Size,
			ExpectedScale == 1 ? Fonts->List.Size : 8.0f);
	};

	for (const FVector2D& Resolution : WoTLegibility_Resolutions)
	{
		const FString Label = WoTLegibility_Label(Resolution) + TEXT(" help widget");
		const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
			.Content(Content)
			.InitialPhysicalSize(Resolution)
			.InitialDpiScale(FWoTMenuStyle::ViewportDpiScale(Resolution))
			.RequiredFontsForTesting(Fonts);
		const int32 ExpectedScale = SWoTHelpOverlay::PixelScaleFor(Resolution);
		TestEqual(Label + TEXT(": the first frame lays out at its own scale"),
			Help->PixelScaleForTesting(), ExpectedScale);
		Check(Help, Settle(Help, Resolution), Label, ExpectedScale);
	}

	// A resize across 1280x960 swaps the small face, so the page is laid out
	// again: no slot keeps the 2x layout's line heights.
	const TSharedRef<SWoTHelpOverlay> Resized = SNew(SWoTHelpOverlay)
		.Content(Content)
		.InitialPhysicalSize(FVector2D(1920.0, 1080.0))
		.InitialDpiScale(1.0f)
		.RequiredFontsForTesting(Fonts);
	const auto SmallLineHeight = [](const FWoTHelpPageLayout& Page)
	{
		const FWoTHelpLayoutLine* Line = Page.Lines.FindByPredicate(
			[](const FWoTHelpLayoutLine& Candidate) { return Candidate.FontIndex == 0; });
		return Line ? Line->Height : 0.0f;
	};
	Settle(Resized, FVector2D(1920.0, 1080.0));
	const float SmallAt2x = SmallLineHeight(Resized->PageLayoutForTesting());
	Check(Resized, Settle(Resized, FVector2D(1280.0, 800.0)),
		TEXT("1920x1080 resized to 1280x800"), 1);
	const float SmallAt1x = SmallLineHeight(Resized->PageLayoutForTesting());
	TestTrue(FString::Printf(
		TEXT("the resize laid the small line out again in the list face (%g, was %g)"),
		SmallAt1x, SmallAt2x), SmallAt2x > 0.0f && SmallAt1x > SmallAt2x);
	Check(Resized, Settle(Resized, FVector2D(1920.0, 1080.0)),
		TEXT("1280x800 resized back to 1920x1080"), 2);
	TestTrue(TEXT("resizing back restores the small line's 2x layout"), FMath::IsNearlyEqual(
		SmallLineHeight(Resized->PageLayoutForTesting()), SmallAt2x, 0.01f));
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
				TestTrue(FString::Printf(TEXT("%s is at least %g physical pixels (%.2f at size %g)"),
					*Gem, Floor, Physical, Font.Size), Physical >= Floor - UE_KINDA_SMALL_NUMBER);
			}
			else
			{
				TestEqual(Gem + TEXT(" keeps the retail size"), Font.Size, SmallSize);
				TestTrue(Gem + TEXT(" draws exactly as before"), FMath::IsNearlyEqual(
					Physical, FWoTMenuStyle::PhysicalTextPixels(SmallSize, 1.0f), 0.01f));
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

#endif
