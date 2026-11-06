#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "../Payload/WoTFrontEndFontOwner.h"
#include "../WoTGameInstance.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTHUDFonts.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"

#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/UObjectGlobals.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/TextureSound.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
bool WoTFontTestRunDeveloperRetailOracle(FAutomationTestBase& Test);
void WoTFontTestAppendU16(TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value));
	Out.Add(static_cast<uint8>(Value >> 8));
}

void WoTFontTestAppendU32(TArray<uint8>& Out, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
		Out.Add(static_cast<uint8>(Value >> Shift));
}

TArray<uint8> WoTFontTestTexturePayload(
	uint32 Width = 8, uint32 Height = 8, uint8 PaletteAlpha = 255)
{
	TArray<uint8> Result;
	const uint8 Magic[] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
	Result.Append(Magic, UE_ARRAY_COUNT(Magic));
	WoTFontTestAppendU16(Result, 1);
	WoTFontTestAppendU16(Result, 0);
	WoTFontTestAppendU32(Result, 256);
	WoTFontTestAppendU32(Result, 1);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		// Keep the representative glyph at partial coverage so the test below
		// distinguishes the authenticated 8-bpp path from 1-bit thresholding.
		const uint8 Channel = Index == 0 ? 0 : Index == 1 ? 64 : 255;
		const uint8 Alpha = Index == 1 ? PaletteAlpha : static_cast<uint8>(255);
		Result.Append({Channel, Channel, Channel, Alpha});
	}
	WoTFontTestAppendU32(Result, Width);
	WoTFontTestAppendU32(Result, Height);
	WoTFontTestAppendU32(Result, Width * Height);
	for (uint32 Pixel = 0; Pixel < Width * Height; ++Pixel) Result.Add(1);
	return Result;
}

FString WoTFontTestTextureHeader(uint32 Width, uint32 Height,
	int32 PayloadBytes, FStringView SourceSha, FStringView Format = TEXT("p8"))
{
	return FString::Printf(
		TEXT("{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},")
		TEXT("\"decoded_layout\":{\"mip_count\":1,\"mips\":[{\"expanded_rgba_byte_length\":%u,\"height\":%u,\"index_byte_length\":%u,\"width\":%u}],\"texel_format\":\"%s\"},")
		TEXT("\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.font.atlas\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":false,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,")
		TEXT("\"source\":{\"export\":\"remaining.Atlas\",\"package\":\"Synthetic/remaining.u\",\"sha256\":\"%s\"}}"),
		Width * Height * 4, Height, Width * Height, Width,
		*FString(Format), PayloadBytes, *FString(SourceSha));
}

bool WoTFontTestLoadFontFixture(TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Tools/dist/fixtures/cache-v3-font-pages.bin")));
}

TArray<uint8> WoTFontTestRetargetFont(
	TConstArrayView<uint8> Fixture, FStringView Face,
	FStringView ReplaceFrom = FStringView(),
	FStringView ReplaceTo = FStringView())
{
	if (Fixture.Num() < 12) return {};
	const uint32 HeaderSize = static_cast<uint32>(Fixture[8])
		| (static_cast<uint32>(Fixture[9]) << 8)
		| (static_cast<uint32>(Fixture[10]) << 16)
		| (static_cast<uint32>(Fixture[11]) << 24);
	if (HeaderSize > static_cast<uint32>(Fixture.Num() - 12)) return {};
	FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Fixture.GetData() + 12), HeaderSize);
	FString Header(Converted.Length(), Converted.Get());
	Header.ReplaceInline(TEXT("\"export\":\"remaining.FontA\""),
		*FString::Printf(TEXT("\"export\":\"remaining.%s\""), *FString(Face)),
		ESearchCase::CaseSensitive);
	if (!ReplaceFrom.IsEmpty())
	{
		Header.ReplaceInline(*FString(ReplaceFrom), *FString(ReplaceTo),
			ESearchCase::CaseSensitive);
	}
	return WoTPayloadActivationTest_FrameObject(Header,
		Fixture.RightChop(12 + static_cast<int32>(HeaderSize)));
}

enum class EWoTFontHostileMutation : uint8
{
	None,
	OversizedPage,
	OverlappingGlyphs,
	TooManyGlyphs,
	SurrogateCodepoint,
	BeyondUnicodeCodepoint,
	ZeroLineMetrics,
	MaximumLineMetrics,
};

void WoTFontTestWriteU32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
{
	check(Bytes.IsValidIndex(Offset + 3));
	for (int32 Shift = 0; Shift < 32; Shift += 8)
		Bytes[Offset + Shift / 8] = static_cast<uint8>(Value >> Shift);
}

uint32 WoTFontTestReadU32(TConstArrayView<uint8> Bytes, int32& Offset)
{
	check(Bytes.IsValidIndex(Offset + 3));
	const uint32 Value = static_cast<uint32>(Bytes[Offset])
		| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
		| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
		| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
	Offset += 4;
	return Value;
}

TArray<uint8> WoTFontTestHostileFont(TConstArrayView<uint8> Fixture,
	FStringView Face, EWoTFontHostileMutation Mutation)
{
	TArray<uint8> Framed = WoTFontTestRetargetFont(Fixture, Face);
	if (Mutation == EWoTFontHostileMutation::None || Framed.Num() < 12) return Framed;
	int32 HeaderOffset = 8;
	const uint32 HeaderSize = WoTFontTestReadU32(Framed, HeaderOffset);
	if (HeaderSize > static_cast<uint32>(Framed.Num() - 12)) return {};
	FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Framed.GetData() + 12), HeaderSize);
	FString Header(Converted.Length(), Converted.Get());
	TArray<uint8> Payload;
	const int32 PayloadOffset = 12 + static_cast<int32>(HeaderSize);
	Payload.Append(Framed.GetData() + PayloadOffset, Framed.Num() - PayloadOffset);
	if (Payload.Num() < 40) return {};
	if (Mutation == EWoTFontHostileMutation::TooManyGlyphs)
	{
		Header.ReplaceInline(TEXT("\"character_map_count\":16"),
			TEXT("\"character_map_count\":65537"), ESearchCase::CaseSensitive);
		Header.ReplaceInline(TEXT("\"glyph_count\":16"),
			TEXT("\"glyph_count\":65537"), ESearchCase::CaseSensitive);
		WoTFontTestWriteU32(Payload, 20, 65537);
		WoTFontTestWriteU32(Payload, 24, 65537);
		return WoTPayloadActivationTest_FrameObject(Header, Payload);
	}
	if (Mutation == EWoTFontHostileMutation::ZeroLineMetrics
		|| Mutation == EWoTFontHostileMutation::MaximumLineMetrics)
	{
		const uint32 Metric = Mutation == EWoTFontHostileMutation::ZeroLineMetrics
			? 0u : MAX_uint32;
		Header.ReplaceInline(TEXT("\"baseline\":2"),
			*FString::Printf(TEXT("\"baseline\":%u"), Metric),
			ESearchCase::CaseSensitive);
		Header.ReplaceInline(TEXT("\"line_height\":2"),
			*FString::Printf(TEXT("\"line_height\":%u"), Metric),
			ESearchCase::CaseSensitive);
		WoTFontTestWriteU32(Payload, 32, Metric);
		WoTFontTestWriteU32(Payload, 36, Metric);
		return WoTPayloadActivationTest_FrameObject(Header, Payload);
	}
	int32 Cursor = 40;
	(void)WoTFontTestReadU32(Payload, Cursor);
	for (int32 StringIndex = 0; StringIndex < 2; ++StringIndex)
	{
		const uint32 Length = WoTFontTestReadU32(Payload, Cursor);
		Cursor += static_cast<int32>(Length);
	}
	const int32 PageWidthOffset = Cursor;
	Cursor += 8;
	const uint32 FormatLength = WoTFontTestReadU32(Payload, Cursor);
	Cursor += static_cast<int32>(FormatLength) + 32;
	(void)WoTFontTestReadU32(Payload, Cursor);
	if (Mutation == EWoTFontHostileMutation::SurrogateCodepoint
		|| Mutation == EWoTFontHostileMutation::BeyondUnicodeCodepoint)
	{
		const uint32 Codepoint = Mutation == EWoTFontHostileMutation::SurrogateCodepoint
			? 0xd800u : 0x110000u;
		WoTFontTestWriteU32(Payload, Cursor, Codepoint);
		WoTFontTestWriteU32(Payload, Cursor + 16 * 8, Codepoint);
		return WoTPayloadActivationTest_FrameObject(Header, Payload);
	}
	if (Mutation == EWoTFontHostileMutation::OversizedPage)
	{
		Header.ReplaceInline(TEXT("\"width\":8"), TEXT("\"width\":16384"),
			ESearchCase::CaseSensitive);
		WoTFontTestWriteU32(Payload, PageWidthOffset, 16384);
	}
	else if (Mutation == EWoTFontHostileMutation::OverlappingGlyphs)
	{
		Cursor += 16 * 8;
		const int32 SecondGlyphX = Cursor + 36 + 8;
		WoTFontTestWriteU32(Payload, SecondGlyphX, 1);
	}
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

struct FWoTFontOracleWrittenObject
{
	FString LogicalId;
	FString Schema;
	TArray<uint8> Bytes;
};

class FWoTFontOracleWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("font oracle writer overflow")));
		}
		FWoTFontOracleWrittenObject& Written = Objects.AddDefaulted_GetRef();
		Written.LogicalId = FString(LogicalId);
		Written.Schema = FString(Schema);
		Written.Bytes.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}

	const FWoTFontOracleWrittenObject* Find(FStringView LogicalId) const
	{
		return Objects.FindByPredicate([LogicalId](const FWoTFontOracleWrittenObject& Item)
		{
			return Item.LogicalId == LogicalId;
		});
	}

	TArray<FWoTFontOracleWrittenObject> Objects;
};

class FWoTFontOracleProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

struct FWoTFontOraclePage
{
	FString LogicalId;
	FString SourceIdentity;
	FString SourceSha;
	uint32 Width = 0;
	uint32 Height = 0;
};

bool WoTFontTestParseOraclePages(TConstArrayView<uint8> Object,
	TArray<FWoTFontOraclePage>& OutPages)
{
	if (Object.Num() < 12) return false;
	int32 Cursor = 8;
	const uint32 HeaderSize = WoTFontTestReadU32(Object, Cursor);
	if (HeaderSize > static_cast<uint32>(Object.Num() - 12)) return false;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(
		TConstArrayView<uint8>(Object.GetData() + 12, HeaderSize));
	if (Parsed.IsError()) return false;
	const FWoTCanonicalJsonValue* Pages =
		Parsed.GetValue().FindMember(TEXT("pages"));
	if (Pages == nullptr || !Pages->IsArray() || Pages->Num() == 0) return false;
	OutPages.Reserve(Pages->Num());
	for (int32 PageIndex = 0; PageIndex < Pages->Num(); ++PageIndex)
	{
		const FWoTCanonicalJsonValue& Value = Pages->GetElement(PageIndex);
		FWoTFontOraclePage& Page = OutPages.AddDefaulted_GetRef();
		uint32 Ordinal = 0;
		if (ReadStringField(Value, TEXT("logical_id"), TEXT("font oracle page"),
				Page.LogicalId).IsSet()
			|| ReadStringField(Value, TEXT("source_identity"),
				TEXT("font oracle page"), Page.SourceIdentity).IsSet()
			|| ReadStringField(Value, TEXT("source_sha256"),
				TEXT("font oracle page"), Page.SourceSha).IsSet()
			|| ReadUnsigned32Field(Value, TEXT("ordinal"),
				TEXT("font oracle page"), Ordinal).IsSet()
			|| ReadUnsigned32Field(Value, TEXT("width"),
				TEXT("font oracle page"), Page.Width).IsSet()
			|| ReadUnsigned32Field(Value, TEXT("height"),
				TEXT("font oracle page"), Page.Height).IsSet()
			|| Ordinal != static_cast<uint32>(PageIndex)) return false;
	}
	return true;
}

bool WoTFontTestReadConvertedOraclePixels(UTexture2D& Texture,
	const FWoTFontOraclePage& Page, TArray<FColor>& OutPixels)
{
	FImage SourceImage;
	if (!Texture.Source.GetMipImage(SourceImage, 0)
		|| SourceImage.SizeX != Page.Width || SourceImage.SizeY != Page.Height)
	{
		return false;
	}
	FImage Bgra;
	Bgra.Init(SourceImage.SizeX, SourceImage.SizeY, ERawImageFormat::BGRA8,
		SourceImage.GammaSpace);
	FImageCore::CopyImage(SourceImage, Bgra);
	const TArrayView64<const FColor> Pixels = Bgra.AsBGRA8();
	if (Pixels.Num() != static_cast<int64>(Page.Width) * Page.Height) return false;
	OutPixels.Append(Pixels.GetData(), static_cast<int32>(Pixels.Num()));

	return true;
}

bool WoTFontTestRunDeveloperRetailOracle(FAutomationTestBase& Test)
{
	TArray<uint8> RetailBytes;
	if (!FFileHelper::LoadFileToArray(RetailBytes,
		*WoTReferencePath(TEXT("original/System/WOT.u")))) return false;
	TWoTBootstrapResult<FWoTRetailContract> Production =
		WoTLoadProductionRetailContract();
	if (Production.IsError())
	{
		Test.AddError(Production.GetError().ToString());
		return false;
	}
	static const TCHAR* Faces[] = {TEXT("F_WOTReg30"), TEXT("F_WOTReg14")};
	FWoTRetailContract FontContract;
	for (const FWoTRequiredFact& Fact : Production.GetValue().Facts)
	{
		const bool bRequiredFace = Fact.Domain == EWoTFactDomain::Font
			&& (Fact.SourceExport == TEXT("WOT.F_WOTReg30")
				|| Fact.SourceExport == TEXT("WOT.F_WOTReg14"));
		if (Fact.SourcePath == TEXT("System/WOT.u")
			&& (Fact.Domain == EWoTFactDomain::Texture || bRequiredFace))
		{
			FontContract.Facts.Add(Fact);
		}
	}
	FWoTFontOracleWriter Writer;
	FWoTFontOracleProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedTextures =
		ImportTexturePackageBytesForTesting(RetailBytes,
			TEXT("System/WOT.u"), FontContract, Writer, Progress);
	if (ImportedTextures.IsError())
	{
		Test.AddError(ImportedTextures.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportFontProceduralPackageBytesForTesting(RetailBytes,
			TEXT("System/WOT.u"), FontContract, Writer, Progress);
	if (Imported.IsError())
	{
		Test.AddError(Imported.GetError().ToString());
		return false;
	}
	const FString CacheRoot = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
		TEXT("WoTFrontEndFontRetailOracle_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
		TEXT("ContentCache"));
	IFileManager::Get().MakeDirectory(*CacheRoot, true);
	TArray<FWoTPayloadActivationTestObject> Objects;
	TMap<FName, UFont*> OracleFonts;
	TMap<FName, TArray<TArray<FColor>>> OraclePixels;
	for (const TCHAR* FaceText : Faces)
	{
		const FName Face(FaceText);
		const FString SourceExport = TEXT("WOT.") + FString(FaceText);
		const FWoTRequiredFact* Fact = FontContract.Facts.FindByPredicate(
			[&SourceExport](const FWoTRequiredFact& Item)
			{
				return Item.Domain == EWoTFactDomain::Font
					&& Item.SourceExport == SourceExport;
			});
		if (Fact == nullptr || Fact->Outputs.Num() != 1) return false;
		const FWoTFontOracleWrittenObject* FontObject =
			Writer.Find(Fact->Outputs[0].LogicalId);
		if (FontObject == nullptr) return false;
		UFont* Oracle = LoadObject<UFont>(nullptr, *FString::Printf(
			TEXT("/Game/WoT/UI/Fonts/%s.%s"), FaceText, FaceText));
		if (Oracle == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("converted oracle %s is absent"), FaceText));
			return false;
		}
		TArray<FWoTFontOraclePage> Pages;
		if (!WoTFontTestParseOraclePages(FontObject->Bytes, Pages)
			|| Pages.Num() != Oracle->Textures.Num()) return false;
		TArray<TArray<FColor>>& FacePixels = OraclePixels.Add(Face);
		FacePixels.Reserve(Pages.Num());
		for (int32 PageIndex = 0; PageIndex < Pages.Num(); ++PageIndex)
		{
			UTexture2D* Texture = Cast<UTexture2D>(Oracle->Textures[PageIndex]);
			TArray<FColor>& PagePixels = FacePixels.AddDefaulted_GetRef();
			if (Texture == nullptr || !WoTFontTestReadConvertedOraclePixels(
				*Texture, Pages[PageIndex], PagePixels)) return false;
			const FWoTFontOracleWrittenObject* ActualTexture =
				Writer.Find(Pages[PageIndex].LogicalId);
			if (!Test.TestNotNull(TEXT("producer emitted actual atlas object"),
				ActualTexture)
				|| !Test.TestEqual(TEXT("actual atlas uses runtime texture schema"),
					ActualTexture->Schema, FString(TEXT("wot-texture-v3")))
				|| !Test.TestTrue(TEXT("actual atlas output is nonempty"),
					!ActualTexture->Bytes.IsEmpty())) return false;
			const bool bAlreadyAdded = Objects.ContainsByPredicate(
				[&Pages, PageIndex](const FWoTPayloadActivationTestObject& Item)
				{
					return Item.LogicalId == Pages[PageIndex].LogicalId;
				});
			if (!bAlreadyAdded)
				Objects.Add({Pages[PageIndex].LogicalId,
					ActualTexture->Schema, ActualTexture->Bytes});
		}
		Objects.Add({Face == FName(TEXT("F_WOTReg30"))
			? TEXT("cache/font/spec-reg30") : TEXT("cache/font/spec-reg14"),
			TEXT("wot-font-v3"), FontObject->Bytes});
		OracleFonts.Add(Face, Oracle);
	}
	FString Error;
	if (!WoTPayloadActivationTest_PublishObjects(CacheRoot, Objects, Error))
	{
		Test.AddError(Error);
		return false;
	}
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	Test.AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bActivatePayload = false;
	Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* GameInstance)
	{
		CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(CacheRoot, false);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* Payload = World ? World->GetGameInstance()
		->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	bool bOk = Test.TestNotNull(TEXT("retail oracle payload"), Payload)
		&& Test.TestTrue(TEXT("retail oracle generation activates"),
			WoTPayloadActivationTest_ActivatePublished(Payload, CacheRoot, Error));
	if (!bOk && !Error.IsEmpty()) Test.AddError(Error);
	if (bOk)
	{
		FWoTBootstrapStatus Initialized = Payload->InitializeFrontEndFontsForTesting();
		bOk = Test.TestFalse(TEXT("real retail faces materialize"), Initialized.IsError());
		if (!bOk) Test.AddError(MoveTemp(Initialized).GetError().ToString());
	}
	const FWoTFrontEndFontOwner* Owner = FWoTFrontEndFontOwner::GetActive();
	for (const TCHAR* FaceText : Faces)
	{
		if (!bOk || Owner == nullptr) break;
		const FName Face(FaceText);
		UFont* Oracle = OracleFonts.FindRef(Face);
		const FWoTFrontEndFontResource* Resource = Owner->ResolveResource(Face);
		const uint32 Codepoint = TEXT('A');
		if (!Test.TestNotNull(FString(FaceText) + TEXT(" runtime face"), Resource)
			|| !Oracle->Characters.IsValidIndex(Codepoint)) { bOk = false; break; }
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" exact runtime glyph census"),
			Resource->GlyphCount(), Oracle->Characters.Num());
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" exact runtime page census"),
			Resource->PageCount(), Oracle->Textures.Num());
		const FFontCharacter& OracleGlyph = Oracle->Characters[Codepoint];
		FWoTFrontEndGlyphSource Source;
		bOk &= Test.TestTrue(FString(FaceText) + TEXT(" representative source"),
			Resource->ResolveGlyphSource(Codepoint, Source));
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" page width"),
			Source.AtlasTexture->GetPlatformData()->SizeX,
			Oracle->Textures[OracleGlyph.TextureIndex]->Source.GetSizeX());
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" page height"),
			Source.AtlasTexture->GetPlatformData()->SizeY,
			Oracle->Textures[OracleGlyph.TextureIndex]->Source.GetSizeY());
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" x"),
			Source.AtlasRect.Min.X, static_cast<int32>(OracleGlyph.StartU));
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" y"),
			Source.AtlasRect.Min.Y, static_cast<int32>(OracleGlyph.StartV));
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" width"),
			Source.AtlasRect.Width(), static_cast<int32>(OracleGlyph.USize));
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" height"),
			Source.AtlasRect.Height(), static_cast<int32>(OracleGlyph.VSize));
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" advance"),
			Source.Advance, static_cast<int32>(OracleGlyph.USize));
		const FSlateFontInfo FullFaceInfo = Resource->SlateInfo(Source.LineHeight);
		FCharacterList& FullCharacters = FSlateApplication::Get().GetRenderer()
			->GetFontCache()->GetCharacterList(FullFaceInfo, 1.0f);
		struct FSlateAtlasSnapshot
		{
			TArray<uint8> Bytes;
			uint32 Width = 0;
		};
		TMap<uint32, FSlateAtlasSnapshot> SlateAtlases;
		uint32 ExpectedLineHeight = 0;
		TSet<UTexture2D*> UsedRuntimePages;
		for (int32 CharacterIndex = 0;
			CharacterIndex < Oracle->Characters.Num(); ++CharacterIndex)
		{
			const FFontCharacter& Expected = Oracle->Characters[CharacterIndex];
			FWoTFrontEndGlyphSource Actual;
			if (!Test.TestTrue(FString::Printf(TEXT("%s glyph %d resolves"),
				FaceText, CharacterIndex),
				Resource->ResolveGlyphSource(
					static_cast<uint32>(CharacterIndex), Actual)))
			{
				bOk = false;
				break;
			}
			ExpectedLineHeight = FMath::Max(ExpectedLineHeight,
				static_cast<uint32>(Expected.VSize));
			bOk &= Test.TestEqual(TEXT("all glyph page ordinals match"),
				static_cast<int32>(Actual.Page),
				static_cast<int32>(Expected.TextureIndex));
			bOk &= Test.TestEqual(TEXT("all glyph x metrics match"),
				Actual.AtlasRect.Min.X, Expected.StartU);
			bOk &= Test.TestEqual(TEXT("all glyph y metrics match"),
				Actual.AtlasRect.Min.Y, Expected.StartV);
			bOk &= Test.TestEqual(TEXT("all glyph widths match"),
				Actual.AtlasRect.Width(), Expected.USize);
			bOk &= Test.TestEqual(TEXT("all glyph heights match"),
				Actual.AtlasRect.Height(), Expected.VSize);
			bOk &= Test.TestEqual(TEXT("all glyph advances match"),
				Actual.Advance, Expected.USize);
			bOk &= Test.TestEqual(TEXT("all glyph horizontal bearings match"),
				Actual.BearingX, 0);
			bOk &= Test.TestEqual(TEXT("all glyph vertical bearings match"),
				Actual.BearingY, Expected.VerticalOffset);
			UsedRuntimePages.Add(Actual.AtlasTexture);
			if (CharacterIndex > 0 && Expected.USize > 0 && Expected.VSize > 0)
			{
				const FCharacterEntry& ActualRaster = FullCharacters.GetCharacter(
					static_cast<TCHAR>(CharacterIndex), EFontFallback::FF_NoFallback);
				bOk &= Test.TestTrue(TEXT("every drawable glyph rasterizes from owned face"),
					ActualRaster.Valid && ActualRaster.FontData != nullptr
						&& ActualRaster.FontData->GetFontFaceAsset()
							== Resource->SlateFaceObject());
				ISlateFontTexture* Texture = FSlateApplication::Get().GetRenderer()
					->GetFontCache()->GetFontTexture(ActualRaster.TextureIndex);
				if (!Test.TestNotNull(TEXT("full oracle glyph has a Slate atlas"), Texture))
				{
					bOk = false;
					break;
				}
				FSlateAtlasSnapshot& Snapshot =
					SlateAtlases.FindOrAdd(ActualRaster.TextureIndex);
				Texture->GetAtlasDataCopy(Snapshot.Bytes);
				Snapshot.Width = static_cast<const FSlateFontAtlas*>(Texture)->GetWidth();
				const TArray<FColor>& ExpectedPixels =
					OraclePixels.FindChecked(Face)[Expected.TextureIndex];
				const int32 ExpectedWidth =
					static_cast<int32>(Oracle->Textures[Expected.TextureIndex]
						->Source.GetSizeX());
				for (int32 Row = 0; Row < Expected.VSize; ++Row)
				{
					for (int32 Column = 0; Column < Expected.USize; ++Column)
					{
						const int32 ExpectedPixel = (Expected.StartV + Row)
							* ExpectedWidth + Expected.StartU + Column;
						const uint64 ActualPixel =
							(static_cast<uint64>(ActualRaster.StartV) + 1 + Row)
								* Snapshot.Width + ActualRaster.StartU + 1 + Column;
						if (!ExpectedPixels.IsValidIndex(ExpectedPixel)
							|| ActualPixel >= static_cast<uint64>(Snapshot.Bytes.Num()))
						{
							bOk = false;
							break;
						}
						const FColor Color = ExpectedPixels[ExpectedPixel];
						const uint8 ExpectedCoverage = Color.A == 0 ? 0
							: FMath::Max3(Color.R, Color.G, Color.B);
						if (!Test.TestEqual(TEXT("full Slate coverage matches converted oracle"),
							static_cast<int32>(Snapshot.Bytes[
								static_cast<int32>(ActualPixel)]),
							static_cast<int32>(ExpectedCoverage)))
						{
							bOk = false;
							break;
						}
					}
					if (!bOk) break;
				}
			}
		}
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" all pages are used"),
			UsedRuntimePages.Num(), Oracle->Textures.Num());
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" line height"),
			Source.LineHeight, ExpectedLineHeight);
		bOk &= Test.TestEqual(FString(FaceText) + TEXT(" baseline"),
			Source.Baseline, ExpectedLineHeight);
		const FSlateFontInfo Info = Resource->SlateInfo(Source.LineHeight);
		FCharacterList& Characters = FSlateApplication::Get().GetRenderer()
			->GetFontCache()->GetCharacterList(Info, 1.0f);
		const FCharacterEntry& Raster = Characters.GetCharacter(
			static_cast<TCHAR>(Codepoint), EFontFallback::FF_NoFallback);
		bOk &= Test.TestTrue(FString(FaceText) + TEXT(" FreeType raster"), Raster.Valid);
		ISlateFontTexture* RasterTexture = FSlateApplication::Get().GetRenderer()
			->GetFontCache()->GetFontTexture(Raster.TextureIndex);
		TArray<uint8> RasterAtlas;
		if (RasterTexture == nullptr) { bOk = false; break; }
		RasterTexture->GetAtlasDataCopy(RasterAtlas);
		const FSlateFontAtlas* RasterPage = static_cast<const FSlateFontAtlas*>(RasterTexture);
		const uint32 AtlasWidth = RasterPage->GetWidth();
		const TArray<FColor>& Pixels = OraclePixels.FindChecked(Face)[OracleGlyph.TextureIndex];
		const int32 OracleWidth =
			Oracle->Textures[OracleGlyph.TextureIndex]->Source.GetSizeX();
		for (int32 Row = 0; Row < Source.AtlasRect.Height(); ++Row)
		{
			for (int32 Column = 0; Column < Source.AtlasRect.Width(); ++Column)
			{
				const int32 OraclePixel = (Source.AtlasRect.Min.Y + Row) * OracleWidth
					+ Source.AtlasRect.Min.X + Column;
				const uint64 SlatePixel = (static_cast<uint64>(Raster.StartV) + 1 + Row)
					* AtlasWidth + Raster.StartU + 1 + Column;
				if (!Pixels.IsValidIndex(OraclePixel)
					|| SlatePixel >= static_cast<uint64>(RasterAtlas.Num())) { bOk = false; break; }
				const FColor Color = Pixels[OraclePixel];
				// Converted masked P8 assets retain the palette RGB for index zero
				// while making its final alpha zero. Nonzero stored palette alpha is
				// deliberately not a coverage multiplier (covered by the synthetic
				// varying-alpha regression).
				const uint8 ExpectedCoverage = Color.A == 0 ? 0
					: FMath::Max3(Color.R, Color.G, Color.B);
				bOk &= Test.TestEqual(FString(FaceText) + TEXT(" raster coverage"),
					static_cast<int32>(RasterAtlas[static_cast<int32>(SlatePixel)]),
					static_cast<int32>(ExpectedCoverage));
			}
			if (!bOk) break;
		}
	}
	WoTTest::DestroySpecWorld(World);
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	IFileManager::Get().DeleteDirectory(*CacheRoot, false, true);
	return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndFontMaterializationSpec,
	"WoT.FrontEnd.Font.AuthenticatedMaterialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndFontMaterializationSpec::RunTest(const FString&)
{
	TArray<uint8> FontFixture;
	if (!TestTrue(TEXT("font-v2 parity fixture loads"),
		WoTFontTestLoadFontFixture(FontFixture))) return false;
	const FString CacheRoot = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
		TEXT("WoTFrontEndFontTests_") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
		TEXT("ContentCache"));
	IFileManager::Get().MakeDirectory(*CacheRoot, true);
	const TArray<uint8> TexturePayload = WoTFontTestTexturePayload(8, 8, 7);
	const FString SourceSha =
		TEXT("0511f92abc5f2308a0de6aa429fbfd30e1ff802d40f0cf0cd18f7b128e145c56");
	const FString TextureHeader = WoTFontTestTextureHeader(
		8, 8, TexturePayload.Num(), SourceSha);
	TArray<FWoTPayloadActivationTestObject> Objects;
	Objects.Add({TEXT("cache/texture/synthetic-atlas"), TEXT("wot-texture-v3"),
		WoTPayloadActivationTest_FrameObject(TextureHeader, TexturePayload)});
	Objects.Add({TEXT("cache/font/spec-reg30"), TEXT("wot-font-v3"),
		WoTFontTestRetargetFont(FontFixture, TEXT("F_WOTReg30"))});
	Objects.Add({TEXT("cache/font/spec-reg14"), TEXT("wot-font-v3"),
		WoTFontTestRetargetFont(FontFixture, TEXT("F_WOTReg14"))});
	// Every face named by native HUD/scoreboard code, plus the Engine and
	// UWindow registries those paths may resolve. Required menu faces above are
	// already resident; every other authenticated face must remain first-use.
	struct FOptionalFace { const TCHAR* Id; const TCHAR* LogicalId; };
	static const FOptionalFace OptionalFaces[] = {
		{TEXT("BigFont"), TEXT("cache/font/Engine__BigFont")},
		{TEXT("LargeFont"), TEXT("cache/font/Engine__LargeFont")},
		{TEXT("MedFont"), TEXT("cache/font/Engine__MedFont")},
		{TEXT("SmallFont"), TEXT("cache/font/Engine__SmallFont")},
		{TEXT("F_Charge"), TEXT("cache/font/WOT__F_Charge")},
		{TEXT("F_Charge_S"), TEXT("cache/font/WOT__F_Charge_S")},
		{TEXT("F_Element"), TEXT("cache/font/WOT__F_Element")},
		{TEXT("F_Key"), TEXT("cache/font/WOT__F_Key")},
		{TEXT("F_Key_S"), TEXT("cache/font/WOT__F_Key_S")},
		{TEXT("F_KeySelected"), TEXT("cache/font/WOT__F_KeySelected")},
		{TEXT("F_KeySelected_S"), TEXT("cache/font/WOT__F_KeySelected_S")},
		{TEXT("F_WOTIta14"), TEXT("cache/font/WOT__F_WOTIta14")},
		{TEXT("F_WOTReg08"), TEXT("cache/font/WOT__F_WOTReg08")},
		{TEXT("F_WOTReg14_S"), TEXT("cache/font/WOT__F_WOTReg14_S")},
		{TEXT("F_WOTReg30_S"), TEXT("cache/font/WOT__F_WOTReg30_S")},
		{TEXT("Tahoma10"), TEXT("cache/font/UWindowFonts__Tahoma10")},
		{TEXT("Tahoma20"), TEXT("cache/font/UWindowFonts__Tahoma20")},
		{TEXT("Tahoma30"), TEXT("cache/font/UWindowFonts__Tahoma30")},
		{TEXT("TahomaB10"), TEXT("cache/font/UWindowFonts__TahomaB10")},
		{TEXT("TahomaB20"), TEXT("cache/font/UWindowFonts__TahomaB20")},
		{TEXT("TahomaB30"), TEXT("cache/font/UWindowFonts__TahomaB30")},
		{TEXT("UTFont12"), TEXT("cache/font/UWindowFonts__UTFont12")},
		{TEXT("UTFont24"), TEXT("cache/font/UWindowFonts__UTFont24")},
		{TEXT("UTFont40"), TEXT("cache/font/UWindowFonts__UTFont40")},
	};
	for (const FOptionalFace& Face : OptionalFaces)
	{
		Objects.Add({Face.LogicalId, TEXT("wot-font-v3"),
			WoTFontTestRetargetFont(FontFixture, Face.Id)});
	}
	// This object is authenticated by the generation but is not in the exact
	// front-end font closure. Materialization must not read or parse it.
	Objects.Add({TEXT("cache/font/unrelated-corrupt"), TEXT("wot-font-v3"),
		TArray<uint8>{0xde, 0xad, 0xbe, 0xef}});
	FString Error;
	if (!TestTrue(TEXT("font/atlas generation publishes"),
		WoTPayloadActivationTest_PublishObjects(CacheRoot, Objects, Error)))
	{
		AddError(Error);
		return false;
	}

	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bActivatePayload = false;
	Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* GameInstance)
	{
		CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(
			CacheRoot, false);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* Payload = World ? World->GetGameInstance()
		->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("payload subsystem fixture"), Payload)
		|| !TestTrue(TEXT("verified generation activates"),
			WoTPayloadActivationTest_ActivatePublished(Payload, CacheRoot, Error)))
	{
		AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTFrontEndFontOwner::SetForcePostCacheValidationFailureForTesting(true);
	FWoTBootstrapStatus Rejected = Payload->InitializeFrontEndFontsForTesting();
	const TWeakObjectPtr<UFont> RejectedFont =
		FWoTFrontEndFontOwner::GetLastValidatedFontForTesting();
	FWoTFrontEndFontOwner::SetForcePostCacheValidationFailureForTesting(false);
	TestTrue(TEXT("forced post-cache FreeType validation fails closed"),
		Rejected.IsError());
	TestTrue(TEXT("failed validation reached a transient font candidate"),
		RejectedFont.IsValid());
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("failed prepublication candidate is flushed from Slate before release"),
		RejectedFont.IsValid());
	FWoTFrontEndFontOwner::ResetMaterializationWorkForTesting();
	FWoTBootstrapStatus Initialized = Payload->InitializeFrontEndFontsForTesting();
	if (!TestFalse(TEXT("font owner materializes both required faces"),
		Initialized.IsError()))
	{
		AddError(MoveTemp(Initialized).GetError().ToString());
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("both faces pass FreeType before owner publication"),
		FWoTFrontEndFontOwner::GetFreeTypeValidationAttemptsForTesting(), 2u);
	const FWoTFrontEndFontOwner* Owner = FWoTFrontEndFontOwner::GetActive();
	if (TestNotNull(TEXT("active font owner resolves"), Owner))
	{
		TWoTBootstrapResult<FSlateFontInfo> Resolved = Owner->Resolve(
			FName(TEXT("F_WOTReg14")));
		TestFalse(TEXT("owner returns an authenticated Slate font result"),
			Resolved.IsError());
		FWoTFrontEndFontOwner::ResetMaterializationWorkForTesting();
		for (const FOptionalFace& Face : OptionalFaces)
		{
			TestNotNull(FString(TEXT("lazy face resolves: ")) + Face.Id,
				UWoTHUDFonts::ResolveRuntime(FName(Face.Id)));
		}
		TestEqual(TEXT("each lazy face materializes one atlas exactly once"),
			FWoTFrontEndFontOwner::GetAtlasMaterializationAttemptsForTesting(),
			static_cast<uint32>(UE_ARRAY_COUNT(OptionalFaces)));
		TestEqual(TEXT("each lazy face builds and validates one bitmap face"),
			FWoTFrontEndFontOwner::GetFreeTypeValidationAttemptsForTesting(),
			static_cast<uint32>(UE_ARRAY_COUNT(OptionalFaces)));
		for (const FOptionalFace& Face : OptionalFaces)
			TestNotNull(FString(TEXT("cached face resolves: ")) + Face.Id,
				UWoTHUDFonts::ResolveRuntime(FName(Face.Id)));
		TestEqual(TEXT("cached resolution performs no duplicate atlas work"),
			FWoTFrontEndFontOwner::GetAtlasMaterializationAttemptsForTesting(),
			static_cast<uint32>(UE_ARRAY_COUNT(OptionalFaces)));
		TestNull(TEXT("an unenumerated face remains fail-closed"),
			UWoTHUDFonts::ResolveRuntime(FName(TEXT("NotARetailFace"))));
		UFont* CanvasFont =
			UWoTHUDFonts::ResolveCanvasRuntime(FName(TEXT("F_WOTReg14")));
		if (TestNotNull(TEXT("HUD resolves a dedicated canvas font"), CanvasFont))
		{
			TestEqual(TEXT("HUD canvas font uses the offline point-sampled path"),
				CanvasFont->FontCacheType, EFontCacheType::Offline);
			for (UTexture2D* Atlas : CanvasFont->Textures)
			{
				TestTrue(TEXT("every HUD canvas atlas retains nearest filtering"),
					Atlas != nullptr && Atlas->Filter == TF_Nearest);
			}
		}
	}
	const FWoTFrontEndFontResource* Resource = Owner
		? Owner->ResolveResource(FName(TEXT("F_WOTReg14"))) : nullptr;
	TWeakObjectPtr<UFont> LifetimeFont;
	if (TestNotNull(TEXT("lease-pinned face resolves"), Resource))
	{
		LifetimeFont = Resource->Font();
		const FSlateFontInfo Info = Resource->SlateInfo(8.0f);
		TestTrue(TEXT("Slate uses the generated composite, not implicit last resort"),
			Info.GetCompositeFont() == Resource->Font()->GetCompositeFont());
		TestEqual(TEXT("fallback is disabled at the shaping call"),
			Info.FontFallback, EFontFallback::FF_NoFallback);
		FWoTFrontEndGlyphSource Source;
		if (TestTrue(TEXT("representative glyph traces to authenticated atlas"),
			Resource->ResolveGlyphSource(1, Source)))
		{
			TestNotNull(TEXT("glyph retains transient authenticated texture"),
				Source.AtlasTexture);
			TestEqual(TEXT("glyph advance is font-v2 authority"),
				Source.Advance, Source.AtlasRect.Width());
		}
		if (FSlateApplication::IsInitialized())
		{
			FCharacterList& Characters = FSlateApplication::Get().GetRenderer()
				->GetFontCache()->GetCharacterList(Info, 1.0f);
			const FCharacterEntry& Character = Characters.GetCharacter(
				static_cast<TCHAR>(1), EFontFallback::FF_NoFallback);
			TestTrue(TEXT("FreeType rasterized the generated authenticated bitmap face"),
				Character.Valid && Character.GlyphIndex != 0);
			TestEqual(TEXT("raster lookup never consulted fallback"),
				Character.FallbackLevel, EFontFallback::FF_NoFallback);
			TestTrue(TEXT("raster source is the owner's transient UFontFace"),
				Character.FontData != nullptr
				&& Character.FontData->GetFontFaceAsset() == Resource->SlateFaceObject());
			// Slate adds one atlas padding texel on each side of the unscaled
			// fixed-size bitmap and applies BitmapRenderScale only at draw/layout.
			TestEqual(TEXT("Slate raster width is the authenticated glyph plus padding"),
				Character.USize,
				static_cast<uint16>(Source.AtlasRect.Width() + 2));
			TestEqual(TEXT("Slate raster height is the authenticated glyph plus padding"),
				Character.VSize,
				static_cast<uint16>(Source.AtlasRect.Height() + 2));
			TestEqual(TEXT("Slate layout advance is font-v2 at the fixed-face scale"),
				Character.XAdvance, static_cast<int16>(FMath::RoundToInt(
					Source.Advance * Character.BitmapRenderScale)));
			ISlateFontTexture* RasterTexture = FSlateApplication::Get()
				.GetRenderer()->GetFontCache()->GetFontTexture(Character.TextureIndex);
			TArray<uint8> RasterAtlas;
			if (TestNotNull(TEXT("representative glyph has a Slate atlas texture"),
				RasterTexture))
			{
				RasterTexture->GetAtlasDataCopy(RasterAtlas);
				TestEqual(TEXT("representative glyph uses the alpha atlas"),
					RasterTexture->GetContentType(), ESlateFontAtlasContentType::Alpha);
				// The representative 2x2 bitmap is far below the non-atlased
				// threshold, so this texture is the CPU-backed Slate font atlas.
				const FSlateFontAtlas* RasterPage =
					static_cast<const FSlateFontAtlas*>(RasterTexture);
				const uint32 AtlasWidth = RasterPage->GetWidth();
				if (TestEqual(TEXT("alpha atlas byte count is exact"),
					static_cast<uint64>(RasterAtlas.Num()),
					static_cast<uint64>(AtlasWidth) * RasterPage->GetHeight()))
				{
					for (uint32 Row = 0; Row < 2; ++Row)
					{
						for (uint32 Column = 0; Column < 2; ++Column)
						{
							const uint64 Pixel =
								(static_cast<uint64>(Character.StartV) + 1 + Row)
									* AtlasWidth + Character.StartU + 1 + Column;
							if (TestTrue(TEXT("representative raster pixel is in bounds"),
								Pixel < static_cast<uint64>(RasterAtlas.Num())))
							{
								TestEqual(TEXT("Slate raster coverage matches the frozen atlas oracle"),
									static_cast<int32>(RasterAtlas[static_cast<int32>(Pixel)]), 64);
							}
						}
					}
				}
			}
		}
	}
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const TOptional<FWoTRequiredMenuFonts> RequiredFonts =
		Style.ResolveRequiredFonts();
	TestTrue(TEXT("guarded style exposes required faces only as a checked pair"),
		RequiredFonts.IsSet());
	if (RequiredFonts.IsSet())
	{
		TestTrue(TEXT("guarded style borrows the owner's required title face"),
			RequiredFonts->Title.FontObject.Get() == (Owner
				? Owner->ResolveFontObject(FName(TEXT("F_WOTReg30"))) : nullptr));
	}
	const TSharedRef<SWoTMenuText> ActiveRequiredText = SNew(SWoTMenuText)
		.Text(FText::FromString(TEXT("lease-bound")))
		.Large(true);
	TestEqual(TEXT("required text is visible while its owner is active"),
		ActiveRequiredText->GetVisibility(), EVisibility::Visible);
	const TSharedRef<SWoTItemInfoOverlay> ActiveItemOverlay =
		SNew(SWoTItemInfoOverlay).PlayerController(nullptr).Item(nullptr);
	TestEqual(TEXT("item overlay captures the active font generation without intercepting input"),
		ActiveItemOverlay->GetVisibility(), EVisibility::HitTestInvisible);
	if (FSlateApplication::IsInitialized())
	{
		FSlateFontInfo EmptyRequiredFace;
		TestNotNull(TEXT("pinned Slate maps an empty font info to last resort"),
			EmptyRequiredFace.GetCompositeFont());
		const FCharacterEntry& LastResort = FSlateApplication::Get().GetRenderer()
			->GetFontCache()->GetCharacterList(EmptyRequiredFace, 1.0f)
			.GetCharacter(TEXT('A'), EFontFallback::FF_NoFallback);
		TestTrue(TEXT("pinned Slate can rasterize last resort from an empty info"),
			LastResort.Valid);
	}
	TArray<TFunction<bool()>> RevocationCallbacks;
	if (TestTrue(TEXT("font owner begins revocation before callbacks run"),
		Owner != nullptr
			&& Owner->BeginLeaseRevocationForTesting(RevocationCallbacks)))
	{
		TestNull(TEXT("cached font is unavailable during revocation"),
			UWoTHUDFonts::ResolveRuntime(FName(TEXT("F_WOTReg14"))));
		TestNull(TEXT("canvas font is unavailable during revocation"),
			UWoTHUDFonts::ResolveCanvasRuntime(FName(TEXT("F_WOTReg14"))));
		Owner->FinishLeaseRevocationForTesting(MoveTemp(RevocationCallbacks));
	}
	WoTTest::DestroySpecWorld(World);
	TestNull(TEXT("owner is unavailable after lease revocation/teardown"),
		FWoTFrontEndFontOwner::GetActive());
	TestFalse(TEXT("guarded style exposes no shapeable face after teardown"),
		FWoTMenuStyle::Get().ResolveRequiredFonts().IsSet());
	TestEqual(TEXT("existing required text hides after revocation without prepass"),
		ActiveRequiredText->GetVisibility(), EVisibility::Collapsed);
	TestEqual(TEXT("existing item overlay hides after revocation without prepass"),
		ActiveItemOverlay->GetVisibility(), EVisibility::Collapsed);
	const TSharedRef<SWoTMenuText> HiddenRequiredText = SNew(SWoTMenuText)
		.Text(FText::FromString(TEXT("must not shape")))
		.Large(true);
	TestEqual(TEXT("dependent required-face text hides after revocation"),
		HiddenRequiredText->GetVisibility(), EVisibility::Collapsed);
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("transient font dies after Slate/style release and owner teardown"),
		LifetimeFont.IsValid());

	// Activate a new owner without ticking or prepassing the old widgets.  Their
	// captured A generation must stay revoked instead of following global B.
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	UWorld* SecondWorld = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* SecondPayload = SecondWorld ? SecondWorld->GetGameInstance()
		->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (TestNotNull(TEXT("second generation payload fixture"), SecondPayload)
		&& TestTrue(TEXT("second verified generation activates"),
			WoTPayloadActivationTest_ActivatePublished(
				SecondPayload, CacheRoot, Error)))
	{
		FWoTBootstrapStatus SecondInitialized =
			SecondPayload->InitializeFrontEndFontsForTesting();
		if (!TestFalse(TEXT("second font owner materializes"),
			SecondInitialized.IsError()))
		{
			AddError(MoveTemp(SecondInitialized).GetError().ToString());
		}
	}
	TestEqual(TEXT("generation B cannot revive generation-A menu text"),
		ActiveRequiredText->GetVisibility(), EVisibility::Collapsed);
	TestEqual(TEXT("generation B cannot revive generation-A item overlay"),
		ActiveItemOverlay->GetVisibility(), EVisibility::Collapsed);
	const TSharedRef<SWoTMenuText> SecondRequiredText = SNew(SWoTMenuText)
		.Text(FText::FromString(TEXT("generation B"))).Large(true);
	const TSharedRef<SWoTItemInfoOverlay> SecondItemOverlay =
		SNew(SWoTItemInfoOverlay).PlayerController(nullptr).Item(nullptr);
	TestEqual(TEXT("new menu text captures generation B"),
		SecondRequiredText->GetVisibility(), EVisibility::Visible);
	TestEqual(TEXT("new item overlay captures generation B without intercepting input"),
		SecondItemOverlay->GetVisibility(), EVisibility::HitTestInvisible);
	WoTTest::DestroySpecWorld(SecondWorld);
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	IFileManager::Get().DeleteDirectory(*CacheRoot, false, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndFontMismatchSpec,
	"WoT.FrontEnd.Font.AtlasMismatchRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndFontMismatchSpec::RunTest(const FString&)
{
	TArray<uint8> FontFixture;
	if (!TestTrue(TEXT("font-v2 mismatch fixture loads"),
		WoTFontTestLoadFontFixture(FontFixture))) return false;
	struct FCase
	{
		const TCHAR* Name;
		const TCHAR* From;
		const TCHAR* To;
		uint32 TextureWidth;
		const TCHAR* TextureSha;
		const TCHAR* TextureFormat;
		bool bExpectRebuildDiagnostic;
		EWoTFontHostileMutation HostileMutation;
	};
	const FString MatchingSha =
		TEXT("0511f92abc5f2308a0de6aa429fbfd30e1ff802d40f0cf0cd18f7b128e145c56");
	const FCase Cases[] = {
		{TEXT("unresolved logical id"),
			TEXT("\"logical_id\":\"cache/texture/synthetic-atlas\""),
			TEXT("\"logical_id\":\"cache/texture/missing-atlas\""),
			8, *MatchingSha, TEXT("p8"), false, EWoTFontHostileMutation::None},
		{TEXT("source digest mismatch"), TEXT(""), TEXT(""), 8,
			TEXT("0e4cab6fa3648ccde4bd033b7a23d032be33435581586c8b52662a89a0c7019b"),
			TEXT("p8"), false, EWoTFontHostileMutation::None},
		{TEXT("dimension mismatch"), TEXT(""), TEXT(""), 9,
			*MatchingSha, TEXT("p8"), false, EWoTFontHostileMutation::None},
		{TEXT("format mismatch"), TEXT("\"format\":\"p8\""),
			TEXT("\"format\":\"x8\""), 8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::None},
		{TEXT("out-of-order page"), TEXT("\"ordinal\":0"),
			TEXT("\"ordinal\":1"), 8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::None},
		{TEXT("legacy font-v1 encoding"),
			TEXT("\"encoding\":\"ue1-font-pages-v2\""),
			TEXT("\"encoding\":\"ue1-font-pages-v1\""),
			8, *MatchingSha, TEXT("p8"), true, EWoTFontHostileMutation::None},
		{TEXT("oversized atlas expansion"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::OversizedPage},
		{TEXT("overlapping glyph rectangles"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::OverlappingGlyphs},
		{TEXT("excessive glyph count"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::TooManyGlyphs},
		{TEXT("surrogate codepoint"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::SurrogateCodepoint},
		{TEXT("codepoint beyond Unicode"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::BeyondUnicodeCodepoint},
		{TEXT("zero line metrics"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::ZeroLineMetrics},
		{TEXT("maximum uint32 line metrics"), TEXT(""), TEXT(""),
			8, *MatchingSha, TEXT("p8"), false,
			EWoTFontHostileMutation::MaximumLineMetrics},
	};
	for (const FCase& Case : Cases)
	{
		const FString CacheRoot = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTFrontEndFontMismatch_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*CacheRoot, true);
		const TArray<uint8> TexturePayload =
			WoTFontTestTexturePayload(Case.TextureWidth, 8);
		const FString TextureHeader = WoTFontTestTextureHeader(
			Case.TextureWidth, 8, TexturePayload.Num(),
			Case.TextureSha, Case.TextureFormat);
		TArray<FWoTPayloadActivationTestObject> Objects;
		Objects.Add({TEXT("cache/texture/synthetic-atlas"), TEXT("wot-texture-v3"),
			WoTPayloadActivationTest_FrameObject(TextureHeader, TexturePayload)});
		const auto BuildFont = [&](FStringView Face)
		{
			return Case.HostileMutation == EWoTFontHostileMutation::None
				? WoTFontTestRetargetFont(FontFixture, Face, Case.From, Case.To)
				: WoTFontTestHostileFont(FontFixture, Face, Case.HostileMutation);
		};
		Objects.Add({TEXT("cache/font/spec-reg30"), TEXT("wot-font-v3"),
			BuildFont(TEXT("F_WOTReg30"))});
		Objects.Add({TEXT("cache/font/spec-reg14"), TEXT("wot-font-v3"),
			BuildFont(TEXT("F_WOTReg14"))});
		FString Error;
		if (!TestTrue(FString(Case.Name) + TEXT(" fixture publishes"),
			WoTPayloadActivationTest_PublishObjects(CacheRoot, Objects, Error)))
		{
			AddError(Error);
			IFileManager::Get().DeleteDirectory(*CacheRoot, false, true);
			continue;
		}

		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
		AddExpectedError(TEXT("Cannot read payload manifest"),
			EAutomationExpectedErrorFlags::Contains, 1);
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bActivatePayload = false;
		Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* GameInstance)
		{
			CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(
				CacheRoot, false);
		};
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		UWoTPayloadSubsystem* Payload = World ? World->GetGameInstance()
			->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		if (TestNotNull(FString(Case.Name) + TEXT(" payload fixture"), Payload)
			&& TestTrue(FString(Case.Name) + TEXT(" generation activates"),
				WoTPayloadActivationTest_ActivatePublished(
					Payload, CacheRoot, Error)))
		{
			FWoTFrontEndFontOwner::ResetMaterializationWorkForTesting();
			FWoTBootstrapStatus Initialized =
				Payload->InitializeFrontEndFontsForTesting();
			TestTrue(FString(Case.Name) + TEXT(" fails closed"),
				Initialized.IsError());
			if (Case.bExpectRebuildDiagnostic && Initialized.IsError())
			{
				TestTrue(TEXT("font-v1 is diagnostic-only and requests reconstruction"),
					MoveTemp(Initialized).GetError().ToString().Contains(TEXT("rebuilt")));
			}
			TestNull(FString(Case.Name) + TEXT(" never publishes an owner"),
				FWoTFrontEndFontOwner::GetActive());
			if (Case.HostileMutation != EWoTFontHostileMutation::None)
			{
				TestEqual(FString(Case.Name) + TEXT(" rejects before atlas work"),
					FWoTFrontEndFontOwner::GetAtlasMaterializationAttemptsForTesting(), 0u);
				TestEqual(FString(Case.Name) + TEXT(" rejects before BDF work"),
					FWoTFrontEndFontOwner::GetBdfConstructionAttemptsForTesting(), 0u);
			}
		}
		else if (!Error.IsEmpty())
		{
			AddError(Error);
		}
		WoTTest::DestroySpecWorld(World);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		IFileManager::Get().DeleteDirectory(*CacheRoot, false, true);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndFontDeveloperRetailOracleSpec,
	"WoT.FrontEnd.Font.DeveloperRetailOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndFontDeveloperRetailOracleSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString RetailPackage = WoTReferencePath(TEXT("original/System/WOT.u"));
	const FString OracleRegistry = FPaths::Combine(FPaths::ProjectContentDir(),
		TEXT("WoT/UI/Fonts/DA_WoTHUDFonts.uasset"));
	if (!FPaths::FileExists(RetailPackage) || !FPaths::FileExists(OracleRegistry))
	{
		AddError(TEXT("required retail font oracle inputs are missing"));
		return false;
	}
	return WoTFontTestRunDeveloperRetailOracle(*this);
}

#endif
