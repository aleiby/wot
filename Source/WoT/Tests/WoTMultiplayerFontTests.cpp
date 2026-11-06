#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTFrontEndTestData.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_AUTOMATION_TESTS
void WoTFrontEndFontOwner_SetForceIntegerVariantFailureForTesting(bool bForce);
#endif

namespace
{
void GatherMultiplayerText(const TSharedRef<SWidget>& Widget,
	TArray<TSharedRef<STextBlock>>& Texts)
{
	if (Widget->GetType() == FName(TEXT("STextBlock")))
		Texts.Add(StaticCastSharedRef<STextBlock>(Widget));
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		GatherMultiplayerText(Children->GetChildAt(Index), Texts);
}

const STextBlock* FindMultiplayerText(
	const TArray<TSharedRef<STextBlock>>& Texts, const FString& Label)
{
	for (const TSharedRef<STextBlock>& Text : Texts)
		if (Text->GetText().ToString() == Label) return &Text.Get();
	return nullptr;
}

bool CheckNativeBitmapGlyph(FAutomationTestBase& Test,
	const TCHAR* Role, const STextBlock& Text, float GeometryScale,
	int32 ExpectedIntegerScale, const FWoTFrontEndFontResource& Resource)
{
	FWoTFrontEndGlyphSource Source;
	// The production-stage fixture is deliberately tiny: it has codepoints
	// 0..15, while the retained retail cache covers ordinary menu text.
	constexpr TCHAR Representative = 1;
	if (!Test.TestTrue(FString(Role) + TEXT(" has authenticated fixture glyph"),
		Resource.ResolveGlyphSource(Representative, Source))) return false;
	const FSlateFontInfo Info = Text.GetFont();
	FCharacterList& Characters = FSlateApplication::Get().GetRenderer()
		->GetFontCache()->GetCharacterList(Info, GeometryScale);
	const FCharacterEntry& Glyph = Characters.GetCharacter(
		Representative, EFontFallback::FF_NoFallback);
	if (!Test.TestTrue(FString(Role) + TEXT(" rasterizes the owned face"),
		Glyph.Valid && Glyph.GlyphIndex != 0 && Glyph.FontData != nullptr
		&& Glyph.FontData->GetFontFaceAsset() != nullptr
		&& Glyph.FontData->GetFontFaceAsset()->GetOutermost()
			== GetTransientPackage()
		&& Glyph.FontData->GetFontFaceAsset() != Resource.SlateFaceObject()))
		return false;
	bool bOk = true;
	bOk &= Test.TestEqual(FString(Role) + TEXT(" has exact integer atlas width"),
		static_cast<int32>(Glyph.USize),
		Source.AtlasRect.Width() * ExpectedIntegerScale + 2);
	bOk &= Test.TestEqual(FString(Role) + TEXT(" has exact integer atlas height"),
		static_cast<int32>(Glyph.VSize),
		Source.AtlasRect.Height() * ExpectedIntegerScale + 2);
	bOk &= Test.TestTrue(FString(Role) + TEXT(" draws preexpanded pixels at 1x"),
		FMath::IsNearlyEqual(Glyph.BitmapRenderScale, 1.0f, 0.001f));
	ISlateFontTexture* Texture = FSlateApplication::Get().GetRenderer()
		->GetFontCache()->GetFontTexture(Glyph.TextureIndex);
	if (!Test.TestNotNull(FString(Role) + TEXT(" has alpha atlas"), Texture))
		return false;
	bOk &= Test.TestEqual(FString(Role) + TEXT(" uses alpha atlas"),
		Texture->GetContentType(), ESlateFontAtlasContentType::Alpha);
	TArray<uint8> Atlas;
	Texture->GetAtlasDataCopy(Atlas);
	const uint32 AtlasWidth = static_cast<const FSlateFontAtlas*>(Texture)->GetWidth();
	UTexture2D* SourceTexture = Source.AtlasTexture;
	if (!Test.TestNotNull(FString(Role) + TEXT(" has source page"), SourceTexture))
		return false;
	const int32 SourceWidth = SourceTexture->GetPlatformData()->SizeX;
	FTexture2DMipMap& Mip = SourceTexture->GetPlatformData()->Mips[0];
	const FColor* SourcePixels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
	if (!Test.TestNotNull(FString(Role) + TEXT(" has source pixels"), SourcePixels))
	{
		Mip.BulkData.Unlock();
		return false;
	}
	TSet<uint8> ObservedCoverage;
	for (int32 Y = 0; Y < Source.AtlasRect.Height() * ExpectedIntegerScale
		&& bOk; ++Y)
	{
		for (int32 X = 0; X < Source.AtlasRect.Width() * ExpectedIntegerScale;
			++X)
		{
			const FColor Color = SourcePixels[
				(Source.AtlasRect.Min.Y + Y / ExpectedIntegerScale) * SourceWidth
				+ Source.AtlasRect.Min.X + X / ExpectedIntegerScale];
			const uint8 ExpectedAlpha = Color.A == 0 ? 0
				: FMath::Max3(Color.R, Color.G, Color.B);
			ObservedCoverage.Add(ExpectedAlpha);
			const uint64 Pixel = (static_cast<uint64>(Glyph.StartV) + 1 + Y)
				* AtlasWidth + Glyph.StartU + 1 + X;
			if (!Test.TestTrue(FString(Role) + TEXT(" atlas coordinate is valid"),
				Pixel < static_cast<uint64>(Atlas.Num()))) { bOk = false; break; }
			if (!Test.TestEqual(FString(Role) + TEXT(" preserves source alpha"),
				Atlas[static_cast<int32>(Pixel)], ExpectedAlpha))
			{ bOk = false; break; }
		}
	}
	Mip.BulkData.Unlock();
	bOk &= Test.TestTrue(FString(Role) + TEXT(" fixture has distinct source texels"),
		ObservedCoverage.Num() >= 2);
	return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMultiplayerNativeBitmapSpec,
	"WoT.Menu.Multiplayer.NativeBitmapFonts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMultiplayerNativeBitmapSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	Fixture.bIncludeBrowserResources = true;
	Fixture.bPatternedFontGlyphs = true;
	FString Error;
	if (!TestTrue(TEXT("production presentation fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTFrontEndFontOwner* Fonts = Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("authenticated font owner exists"), Fonts)) return false;
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fonts, {}, []() { return false; });
	if (!TestFalse(TEXT("scene stages"), Staged.IsError())) return false;
	if (!TestFalse(TEXT("scene commits"),
		MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	auto* Controller = Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	auto Authority = FWoTFrontEndPresentationAuthority::Create(Controller);
	if (!TestTrue(TEXT("presentation captures lease"),
		Authority.IsValid() && Authority->IsReady())) return false;
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller).PresentationAuthorityForTesting(Authority);
	Root->ActivateMainMenuEntryForTesting(0);
	if (!TestTrue(TEXT("real multiplayer screen opens"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play)) return false;
	const FWoTFrontEndFontResource* Body = Fonts->ResolveResource(FName(TEXT("F_WOTReg14")));
	const FWoTFrontEndFontResource* Heading = Fonts->ResolveResource(FName(TEXT("F_WOTReg30")));
	const FWoTFrontEndFontResource* Browser = Fonts->ResolveResource(FName(TEXT("Tahoma20")));
	if (!TestNotNull(TEXT("body face exists"), Body)
		|| !TestNotNull(TEXT("heading face exists"), Heading)
		|| !TestNotNull(TEXT("browser face exists"), Browser)) return false;
	FWoTFrontEndGlyphSource BodySource, HeadingSource, BrowserSource;
	if (!TestTrue(TEXT("body source resolves"), Body->ResolveGlyphSource(1, BodySource))
		|| !TestTrue(TEXT("heading source resolves"), Heading->ResolveGlyphSource(1, HeadingSource))
		|| !TestTrue(TEXT("browser source resolves"), Browser->ResolveGlyphSource(1, BrowserSource)))
		return false;
	TestEqual(TEXT("retail-sized body strike"), BodySource.LineHeight, 14u);
	TestEqual(TEXT("retail-sized heading strike"), HeadingSource.LineHeight, 30u);
	TestEqual(TEXT("retail-sized browser strike"), BrowserSource.LineHeight, 24u);
	auto NativeBody = Fonts->ResolveIntegerScaled(FName(TEXT("F_WOTReg14")), 1);
	if (!TestFalse(TEXT("native multiplayer strike resolves"),
		NativeBody.IsError())) return false;
	FSlateFontInfo NativeInfo = NativeBody.GetValue();
	TestEqual(TEXT("native face reports native strike height"),
		NativeInfo.Size, static_cast<float>(BodySource.LineHeight));
	TestTrue(TEXT("native multiplayer face does not alter the opening face"),
		NativeInfo.FontObject.Get() != Body->Font());
	NativeInfo.Size *= 72.0f / FontConstants::RenderDPI;
	TSharedRef<STextBlock> NativeProbe = SNew(STextBlock).Font(NativeInfo);
	CheckNativeBitmapGlyph(*this, TEXT("native 1x direct probe"),
		*NativeProbe, 1.0f, 1, *Body);
	auto DoubledBody = Fonts->ResolveIntegerScaled(FName(TEXT("F_WOTReg14")), 2);
	if (!TestFalse(TEXT("integer scale two has an authenticated face"),
		DoubledBody.IsError())) return false;
	FWoTFrontEndGlyphSource NativeBodyGlyph;
	if (!TestTrue(TEXT("body native metric resolves"),
		Body->ResolveGlyphSource(1, NativeBodyGlyph))) return false;
	TestEqual(TEXT("doubled face reports doubled native strike height"),
		DoubledBody.GetValue().Size,
		static_cast<float>(NativeBodyGlyph.LineHeight * 2));
	TestTrue(TEXT("doubled face has separate transient font object"),
		DoubledBody.GetValue().FontObject.Get() != Body->Font());
	TestTrue(TEXT("unsupported pixel scale is rejected"),
		Fonts->ResolveIntegerScaled(FName(TEXT("F_WOTReg14")), 3).IsError());
	struct FScaleCase { float Scale; int32 IntegerScale; };
	const FScaleCase Scales[] = {{2.0f / 3.0f, 1}, {1.0f, 1},
		{1.25f, 1}, {2.0f, 2}};
	for (const FScaleCase& Case : Scales)
	{
		// Tick supplies the same accumulated layout scale used by production
		// font attributes. Prepass evaluates those attributes before GetFont().
		Root->Tick(FGeometry::MakeRoot(FVector2D(1920.0f, 1080.0f),
			FSlateLayoutTransform(Case.Scale)), 0.0, 0.0f);
		Root->SlatePrepass(Case.Scale);
		TArray<TSharedRef<STextBlock>> Texts;
		GatherMultiplayerText(Root, Texts);
		const FString HostTitle = Authority->Text(
			FName(TEXT("menuStartArenaServer.MenuTitle"))).ToString();
		const STextBlock* BodyText = FindMultiplayerText(Texts, TEXT("Server Name"));
		const STextBlock* HeadingText = FindMultiplayerText(Texts, HostTitle);
		const STextBlock* RefreshText = FindMultiplayerText(Texts, TEXT("Refresh Sessions"));
		const STextBlock* BrowserText = FindMultiplayerText(Texts, TEXT("Server"));
		const STextBlock* BackText = FindMultiplayerText(Texts, TEXT("Back"));
		const FString ScaleLabel = FString::Printf(TEXT("scale %.3f "), Case.Scale);
		if (TestNotNull(ScaleLabel + TEXT("body widget"), BodyText))
			CheckNativeBitmapGlyph(*this, *(ScaleLabel + TEXT("body")),
				*BodyText, Case.Scale, 2, *Body);
		if (TestNotNull(ScaleLabel + TEXT("refresh widget"), RefreshText))
			CheckNativeBitmapGlyph(*this, *(ScaleLabel + TEXT("refresh")),
				*RefreshText, Case.Scale, 2, *Body);
		if (TestNotNull(ScaleLabel + TEXT("back widget"), BackText))
			CheckNativeBitmapGlyph(*this, *(ScaleLabel + TEXT("back")),
				*BackText, Case.Scale, 2, *Body);
		if (TestNotNull(ScaleLabel + TEXT("heading widget"), HeadingText))
			CheckNativeBitmapGlyph(*this, *(ScaleLabel + TEXT("heading")),
				*HeadingText, Case.Scale, Case.IntegerScale, *Heading);
		if (TestNotNull(ScaleLabel + TEXT("browser widget"), BrowserText))
			CheckNativeBitmapGlyph(*this, *(ScaleLabel + TEXT("browser")),
				*BrowserText, Case.Scale, Case.IntegerScale, *Browser);
	}
	WoTFrontEndFontOwner_SetForceIntegerVariantFailureForTesting(true);
	const FSlateFontInfo FallbackBody = Authority->MultiplayerFont(
		FName(TEXT("F_WOTReg14")), 2.0f);
	const FSlateFontInfo FallbackBrowser = Authority->MultiplayerFont(
		FName(TEXT("Tahoma20")), 2.0f);
	WoTFrontEndFontOwner_SetForceIntegerVariantFailureForTesting(false);
	TestTrue(TEXT("variant failure retains authenticated body instead of Slate fallback"),
		FallbackBody.FontObject.Get() == Body->Font()
		&& FallbackBody.FontFallback == EFontFallback::FF_NoFallback);
	TestTrue(TEXT("variant failure retains authenticated browser instead of Slate fallback"),
		FallbackBrowser.FontObject.Get() == Browser->Font()
		&& FallbackBrowser.FontFallback == EFontFallback::FF_NoFallback);
	Scene.Shutdown();
	return !HasAnyErrors();
}

#endif
