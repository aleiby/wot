#include "WoTFrontEndFontOwner.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ScopeLock.h"
#include "Rendering/Texture2DResource.h"
#include "Rendering/SlateRenderer.h"
#include "WoTCacheLease.h"
#include "WoTPayloadActivation.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTMenuStyle.h"

FWoTFrontEndFontOwner* FWoTFrontEndFontOwner::Active = nullptr;

namespace
{
#if WITH_AUTOMATION_TESTS
uint32 GWoTAtlasMaterializationAttempts = 0;
uint32 GWoTBdfConstructionAttempts = 0;
uint32 GWoTFreeTypeValidationAttempts = 0;
bool GWoTForcePostCacheValidationFailure = false;
bool GWoTForceOptionalVariantFailure = false;
TWeakObjectPtr<UFont> GWoTLastValidatedFont;
#endif

struct FWoTCachedFontFace
{
	const TCHAR* Face;
	const TCHAR* LogicalId;
};

// Complete cache/font production closure. Keeping this list explicit prevents
// an arbitrary runtime name from becoming a cache lookup or diagnostic key.
constexpr FWoTCachedFontFace GWoTCachedFontFaces[] = {
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
	{TEXT("F_WOTReg14"), TEXT("cache/font/WOT__F_WOTReg14")},
	{TEXT("F_WOTReg14_S"), TEXT("cache/font/WOT__F_WOTReg14_S")},
	{TEXT("F_WOTReg30"), TEXT("cache/font/WOT__F_WOTReg30")},
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

const TCHAR* WoTCachedFontLogicalId(FName Face)
{
	for (const FWoTCachedFontFace& Candidate : GWoTCachedFontFaces)
		if (Face == FName(Candidate.Face)) return Candidate.LogicalId;
	return nullptr;
}

FWoTBootstrapStatus WoTFontOwnerError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

bool WoTAppendUtf8(TArray<uint8>& Out, const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() < 0
		|| static_cast<uint64>(Out.Num()) + static_cast<uint64>(Utf8.Length())
			> WoTRuntimeFontLimits::MaxBdfBytes)
	{
		return false;
	}
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return true;
}

TArray<uint8> WoTBuildBdf(const FWoTRuntimeFontResource& Font,
	TConstArrayView<TSharedPtr<const FWoTTexturePayloadData>> Atlases,
	int32 PixelScale = 1, bool bExactPixelStrike = false)
{
#if WITH_AUTOMATION_TESTS
	++GWoTBdfConstructionAttempts;
#endif
	if (PixelScale != 1 && PixelScale != 2) return {};
	int32 MaximumWidth = 1;
	const int64 Descent = static_cast<int64>(Font.LineHeight()) - Font.Baseline();
	if (Font.LineHeight() == 0
		|| Font.LineHeight() > WoTRuntimeFontLimits::MaxLineMetric / PixelScale
		|| Font.Baseline() > Font.LineHeight()
		|| Descent < 0 || Descent > MAX_int32) return {};
	for (const FWoTRuntimeGlyph& Glyph : Font.Glyphs())
	{
		if (Glyph.AtlasRect.Width() > MAX_int16 / PixelScale
			|| Glyph.AtlasRect.Height() > MAX_int16 / PixelScale
			|| Glyph.Advance > MAX_int32 / PixelScale
			|| Glyph.BearingX > MAX_int32 / PixelScale
			|| Glyph.BearingX < MIN_int32 / PixelScale
			|| Glyph.BearingY > MAX_int32 / PixelScale
			|| Glyph.BearingY < MIN_int32 / PixelScale) return {};
		MaximumWidth = FMath::Max(MaximumWidth,
			Glyph.AtlasRect.Width() * PixelScale);
	}
	TArray<uint8> Result;
	const uint64 ReservedBytes = Font.MaterializationPlan().BdfBytes;
	if (ReservedBytes == 0 || ReservedBytes > WoTRuntimeFontLimits::MaxBdfBytes
		|| ReservedBytes > static_cast<uint64>(MAX_int32)) return {};
	Result.Reserve(static_cast<int32>(FMath::Min<uint64>(
		ReservedBytes * PixelScale * PixelScale,
		FMath::Min<uint64>(WoTRuntimeFontLimits::MaxBdfBytes,
			static_cast<uint64>(MAX_int32)))));
	const uint32 ScaledLineHeight = Font.LineHeight() * PixelScale;
	const uint32 ScaledBaseline = Font.Baseline() * PixelScale;
	const int32 ScaledDescent = static_cast<int32>(Descent) * PixelScale;
	const uint32 Resolution = bExactPixelStrike ? 72u : 75u;
	const uint32 XlfdPointSize = bExactPixelStrike ? 720u : 750u;
	const FString PixelSizeProperty = bExactPixelStrike
		? FString::Printf(TEXT("PIXEL_SIZE %u\n"), ScaledLineHeight)
		: FString();
	if (!WoTAppendUtf8(Result, FString::Printf(
		TEXT("STARTFONT 2.2\nCOMMENT Generated from authenticated font-v2 bytes\n")
		TEXT("FONT -wot-retail-medium-r-normal--%u-%u-%u-%u-c-0-iso10646-1\n")
		TEXT("SIZE %u %u %u 8\nFONTBOUNDINGBOX %d %u 0 %d\n")
		TEXT("STARTPROPERTIES %d\nFONT_ASCENT %u\nFONT_DESCENT %u\n")
		TEXT("DEFAULT_CHAR 0\n%sENDPROPERTIES\nCHARS %d\n"),
		ScaledLineHeight, XlfdPointSize, Resolution, Resolution,
		ScaledLineHeight, Resolution, Resolution,
		MaximumWidth, ScaledLineHeight, -ScaledDescent,
		bExactPixelStrike ? 4 : 3, ScaledBaseline,
		static_cast<uint32>(ScaledDescent), *PixelSizeProperty,
		Font.Glyphs().Num()))) return {};
	static const TCHAR Hex[] = TEXT("0123456789ABCDEF");
	for (const FWoTRuntimeGlyph& Glyph : Font.Glyphs())
	{
		if (!Atlases.IsValidIndex(Glyph.Page) || !Atlases[Glyph.Page].IsValid())
		{
			return {};
		}
		const FWoTTexturePayloadData& Atlas = *Atlases[Glyph.Page];
		if (Atlas.Mips.IsEmpty() || Atlas.Palette.Num() != 256)
		{
			return {};
		}
		const FWoTTexturePayloadMip& Mip = Atlas.Mips[0];
		const TConstArrayView<uint8> Bytes = Atlas.PayloadBytes();
		const int32 Width = Glyph.AtlasRect.Width();
		const int32 Height = Glyph.AtlasRect.Height();
		const int64 YOffset64 = static_cast<int64>(Font.Baseline())
			- Height - Glyph.BearingY;
		const int64 ScalableWidth64 = static_cast<int64>(Glyph.Advance) * 1000
			/ static_cast<int64>(Font.LineHeight());
		if (YOffset64 < MIN_int32 / PixelScale
			|| YOffset64 > MAX_int32 / PixelScale
			|| ScalableWidth64 < MIN_int32 || ScalableWidth64 > MAX_int32) return {};
		const int32 YOffset = static_cast<int32>(YOffset64) * PixelScale;
		if (!WoTAppendUtf8(Result, FString::Printf(
			TEXT("STARTCHAR uni%04X\nENCODING %u\nSWIDTH %d 0\n")
			TEXT("DWIDTH %d 0\nBBX %d %d %d %d\nBITMAP\n"),
			Glyph.Codepoint, Glyph.Codepoint,
			static_cast<int32>(ScalableWidth64),
			Glyph.Advance * PixelScale, Width * PixelScale,
			Height * PixelScale, Glyph.BearingX * PixelScale, YOffset))) return {};
		for (int32 Row = 0; Row < Height; ++Row)
		{
			for (int32 RepRow = 0; RepRow < PixelScale; ++RepRow)
			{
				const uint64 RowBytes = static_cast<uint64>(Width)
					* PixelScale * 2 + 4;
				if (RowBytes > WoTRuntimeFontLimits::MaxBdfBytes
					|| static_cast<uint64>(Result.Num())
						> WoTRuntimeFontLimits::MaxBdfBytes - RowBytes) return {};
				for (int32 Column = 0; Column < Width; ++Column)
				{
					const uint64 Pixel = Mip.DataOffset
						+ static_cast<uint64>(Glyph.AtlasRect.Min.Y + Row) * Mip.Width
						+ static_cast<uint64>(Glyph.AtlasRect.Min.X + Column);
					if (Pixel >= static_cast<uint64>(Bytes.Num())) return {};
					const uint8 PaletteIndex = Bytes[static_cast<int32>(Pixel)];
					const FColor Color = Atlas.Palette[PaletteIndex];
					const uint8 Coverage = Atlas.bMasked && PaletteIndex == 0 ? 0
						: FMath::Max3(Color.R, Color.G, Color.B);
					for (int32 RepColumn = 0; RepColumn < PixelScale; ++RepColumn)
					{
						Result.Add(static_cast<uint8>(Hex[Coverage >> 4]));
						Result.Add(static_cast<uint8>(Hex[Coverage & 0x0f]));
					}
				}
				// The pinned Win64 FreeType bitmap parser widens a hex-character
				// read to four bytes. Printable trailing padding keeps that read
				// inside its line buffer even at an allocation boundary. FreeType
				// consumes only the declared bitmap width; coverage is unchanged.
				// Zero-width retail glyphs have no bitmap parser invocation:
				// their empty rows must remain blank, not whitespace tokens.
				if (Width > 0)
				{
					Result.Add(' ');
					Result.Add(' ');
					Result.Add(' ');
				}
				Result.Add('\n');
			}
		}
		if (!WoTAppendUtf8(Result, TEXT("ENDCHAR\n"))) return {};
	}
	if (!WoTAppendUtf8(Result, TEXT("ENDFONT\n"))) return {};
	return Result;
}

UTexture2D* WoTMaterializeAtlas(const FWoTTexturePayloadData& Payload,
	FName Name)
{
#if WITH_AUTOMATION_TESTS
	++GWoTAtlasMaterializationAttempts;
#endif
	if (Payload.Mips.IsEmpty() || Payload.Palette.Num() != 256) return nullptr;
	const FWoTTexturePayloadMip& Mip = Payload.Mips[0];
	const TConstArrayView<uint8> Bytes = Payload.PayloadBytes();
	uint64 PixelCount = 0;
	if (Mip.Width == 0 || Mip.Height == 0 || Mip.Width > MAX_int32
		|| Mip.Height > MAX_int32 || Mip.Width > MAX_uint64 / Mip.Height
		|| (PixelCount = Mip.Width * Mip.Height) > WoTRuntimeFontLimits::MaxAtlasPixels
		|| Mip.StoredByteLength != PixelCount
		|| Mip.DataOffset > static_cast<uint64>(Bytes.Num())
		|| Mip.StoredByteLength > static_cast<uint64>(Bytes.Num()) - Mip.DataOffset)
	{
		return nullptr;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(
		static_cast<int32>(Mip.Width), static_cast<int32>(Mip.Height),
		PF_B8G8R8A8, Name);
	if (Texture == nullptr || Texture->GetPlatformData() == nullptr
		|| Texture->GetPlatformData()->Mips.IsEmpty()) return nullptr;
	FColor* Destination = static_cast<FColor*>(
		Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
	if (Destination == nullptr) return nullptr;
	for (uint64 Pixel = 0; Pixel < Mip.StoredByteLength; ++Pixel)
	{
		const uint8 Index = Bytes[static_cast<int32>(Mip.DataOffset + Pixel)];
		FColor Color = Payload.Palette[Index];
		if (Payload.bMasked && Index == 0) Color.A = 0;
		Destination[Pixel] = Color;
	}
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->SRGB = true;
	Texture->Filter = TF_Nearest;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	return Texture;
}

void WoTFlushSlateFontPair(UFont* Font, UFontFace* Face)
{
	if (!FSlateApplication::IsInitialized()) return;
	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (Renderer == nullptr) return;
	const TSharedRef<FSlateFontCache> FontCache = Renderer->GetFontCache();
	if (Font != nullptr)
	{
		FontCache->FlushCompositeFont(Font->GetInternalCompositeFont());
		FontCache->FlushObject(Font);
	}
	if (Face != nullptr)
		FontCache->FlushObject(Face);
	FontCache->ConditionalFlushCache();
}

void WoTFlushSlateFontResource(FWoTFrontEndFontResource& Resource)
{
	WoTFlushSlateFontPair(Resource.NativeMultiplayerFont(),
		Resource.NativeMultiplayerFaceObject());
	WoTFlushSlateFontPair(Resource.DoubledFont(), Resource.DoubledFaceObject());
	WoTFlushSlateFontPair(Resource.Font(), Resource.SlateFaceObject());
}

}

#if WITH_AUTOMATION_TESTS
void WoTFrontEndFontOwner_SetForceIntegerVariantFailureForTesting(bool bForce)
{
	GWoTForceOptionalVariantFailure = bForce;
}
#endif

FSlateFontInfo FWoTFrontEndFontResource::SlateInfo(float Size) const
{
	FSlateFontInfo Result(SlateFont.Get(), Size, FName(TEXT("Regular")));
	Result.FontFallback = EFontFallback::FF_NoFallback;
	return Result;
}

bool FWoTFrontEndFontResource::ResolveGlyphSource(
	uint32 Codepoint, FWoTFrontEndGlyphSource& Out) const
{
	const FWoTRuntimeCharacterMap* Map = FontRecord.CharacterMap().FindByPredicate(
		[Codepoint](const FWoTRuntimeCharacterMap& Entry)
		{
			return Entry.Codepoint == Codepoint;
		});
	if (Map == nullptr || !FontRecord.Glyphs().IsValidIndex(Map->Glyph)) return false;
	const FWoTRuntimeGlyph& Glyph = FontRecord.Glyphs()[Map->Glyph];
	if (!AtlasTextures.IsValidIndex(Glyph.Page) || !AtlasTextures[Glyph.Page].IsValid())
		return false;
	Out.AtlasTexture = AtlasTextures[Glyph.Page].Get();
	Out.Page = Glyph.Page;
	Out.AtlasRect = Glyph.AtlasRect;
	Out.Advance = Glyph.Advance;
	Out.BearingX = Glyph.BearingX;
	Out.BearingY = Glyph.BearingY;
	Out.Baseline = FontRecord.Baseline();
	Out.LineHeight = FontRecord.LineHeight();
	return true;
}

FWoTFrontEndFontOwner::~FWoTFrontEndFontOwner()
{
	Shutdown();
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::Initialize(
	UWoTPayloadSubsystem& Payload, const FWoTCacheGenerationLease& Lease)
{
	const FString RequiredLogicalIds[] = {
		TEXT("cache/font/WOT__F_WOTReg30"),
		TEXT("cache/font/WOT__F_WOTReg14")
	};
	return InitializeRequired(Payload, Lease, MakeArrayView(RequiredLogicalIds));
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::InitializeRequired(
	UWoTPayloadSubsystem& Payload, const FWoTCacheGenerationLease& Lease,
	TConstArrayView<FString> RequiredLogicalIds)
{
	if (Active != nullptr || !IsInGameThread() || !Lease.IsValid()
		|| !IsWoTPayloadBootstrapActive() || RequiredLogicalIds.Num() != 2)
	{
		return WoTFontOwnerError(TEXT("front-end font owner requires one active payload lease"));
	}
	{
		FScopeLock RetentionAdmission(&Lease.GetVerifiedReadGuard());
		if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
		{
			return WoTFontOwnerError(
				TEXT("front-end font owner retention was denied during revocation"));
		}
		PinnedLease.Reset(new FWoTCacheGenerationLease(Lease));
	}
	static const FName Required[] = {
		FName(TEXT("F_WOTReg30")), FName(TEXT("F_WOTReg14"))
	};
	for (int32 RequiredIndex = 0; RequiredIndex < UE_ARRAY_COUNT(Required);
		++RequiredIndex)
	{
		FWoTBootstrapStatus Loaded = MaterializeFace(Payload,
			Required[RequiredIndex], RequiredLogicalIds[RequiredIndex]);
		if (Loaded.IsError()) return Loaded;
	}
	PayloadOwner = &Payload;
	Active = this;
	Generation = MakeShared<FWoTRequiredMenuFontGeneration>();
	Generation->bValid = true;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::EnsureBrowserFont(UWoTPayloadSubsystem& Payload)
{
	if (!IsInGameThread() || !IsValid() || !PinnedLease->CanPublishPresentation()
		|| !Payload.SharesSnapshotWith(*PinnedLease))
		return WoTFontOwnerError(TEXT("browser font requires the current presentation lease"));
	const FName Face(TEXT("Tahoma20"));
	return EnsureFace(Face);
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::EnsureFace(FName FaceId)
{
	if (!IsInGameThread() || !IsValid() || !PinnedLease.IsValid())
		return WoTFontOwnerError(TEXT("font face requires the current presentation lease"));
	UWoTPayloadSubsystem* Payload = PayloadOwner.Get();
	if (Payload == nullptr || !Payload->SharesSnapshotWith(*PinnedLease))
		return WoTFontOwnerError(TEXT("font face lost its authenticated payload owner"));
	{
		FScopeLock ReadAdmission(&PinnedLease->GetVerifiedReadGuard());
		if (!PinnedLease->MayBeginVerifiedReadUnderVerifiedReadGuard()
			|| !Payload->SharesSnapshotWith(*PinnedLease))
		{
			return WoTFontOwnerError(
				TEXT("font face requires the current presentation lease"));
		}
		if (Resources.Contains(FaceId)) return FWoTBootstrapStatus::Success();
	}
	const TCHAR* LogicalId = WoTCachedFontLogicalId(FaceId);
	if (LogicalId == nullptr)
	{
		if (!bReportedUnsupportedFace)
		{
			bReportedUnsupportedFace = true;
			WoTRecordBootstrapHeartbeat(TEXT("font-face-failure"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				TEXT("unsupported cached font face requested"));
		}
		return WoTFontOwnerError(TEXT("font face is outside the authenticated registry"));
	}
	if (FailedFaces.Contains(FaceId))
		return WoTFontOwnerError(TEXT("font face previously failed authenticated materialization"));
	FWoTBootstrapStatus Loaded = MaterializeFace(*Payload, FaceId, LogicalId);
	if (Loaded.IsError())
	{
		FailedFaces.Add(FaceId);
		WoTRecordBootstrapHeartbeat(TEXT("font-face-failure"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("face=%s reason=%s"), *FaceId.ToString(),
				*Loaded.GetError().ToString()).Left(512));
	}
	return Loaded;
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::MaterializeFace(
	UWoTPayloadSubsystem& Payload, FName FaceId, const FString& LogicalId)
{
		const FWoTNeutralResourceId ResourceId{
			LogicalId, EWoTNeutralResourceKind::Font};
		TUniquePtr<FWoTFrontEndFontResource> Resource =
			MakeUnique<FWoTFrontEndFontResource>();
		FWoTBootstrapStatus Loaded = Resource->FontRecord.LoadFirstUse(ResourceId);
		if (Loaded.IsError()) return Loaded;
		FString Leaf = Resource->FontRecord.SourceExport();
		int32 Dot = INDEX_NONE;
		if (Leaf.FindLastChar(TEXT('.'), Dot)) Leaf.RightChopInline(Dot + 1);
		if (FName(*Leaf) != FaceId)
			return WoTFontOwnerError(
				TEXT("front-end font logical id resolved the wrong required face"));
		if (Resources.Contains(FaceId))
			return WoTFontOwnerError(TEXT("a required front-end font face is duplicated"));
		const FWoTRuntimeFontMaterializationPlan& Plan =
			Resource->FontRecord.MaterializationPlan();
		if (Plan.AtlasPixels == 0
			|| Plan.AtlasPixels > WoTRuntimeFontLimits::MaxAtlasPixels
			|| Plan.AtlasExpansionBytes
				> WoTRuntimeFontLimits::MaxAtlasExpansionBytes
			|| Plan.GlyphPixels > WoTRuntimeFontLimits::MaxGlyphPixels
			|| Plan.BdfBytes == 0
			|| Plan.BdfBytes > WoTRuntimeFontLimits::MaxBdfBytes)
		{
			return WoTFontOwnerError(
				TEXT("front-end font materialization plan is unsafe"));
		}
		Resource->AtlasPayloads.Reserve(Resource->FontRecord.Pages().Num());
		Resource->AtlasTextures.Reserve(Resource->FontRecord.Pages().Num());
		for (const FWoTRuntimeFontPage& Page : Resource->FontRecord.Pages())
		{
			TSharedPtr<const FWoTTexturePayloadData> Atlas =
				Payload.LoadTexturePayload(Page.TextureLogicalId);
			if (!Atlas.IsValid() || Atlas->SourceExport != Page.SourceIdentity
				|| Atlas->SourceSha256 != Page.SourceSha256 || Atlas->Mips.IsEmpty()
				|| Atlas->Mips[0].Width != Page.Width
				|| Atlas->Mips[0].Height != Page.Height)
			{
				return WoTFontOwnerError(TEXT("font atlas texture authority mismatched font-v2"));
			}
			UTexture2D* Texture = WoTMaterializeAtlas(*Atlas, *FString::Printf(
				TEXT("%s_Atlas_%u"), *Leaf, Page.Ordinal));
			if (Texture == nullptr)
				return WoTFontOwnerError(TEXT("authenticated font atlas materialization failed"));
			Resource->AtlasPayloads.Add(MoveTemp(Atlas));
			Resource->AtlasTextures.Emplace(Texture);
		}
		UFont* CanvasFont = NewObject<UFont>(GetTransientPackage(),
			*FString::Printf(TEXT("%s_AuthenticatedCanvasFont"), *Leaf));
		CanvasFont->FontCacheType = EFontCacheType::Offline;
		for (const TStrongObjectPtr<UTexture2D>& Texture : Resource->AtlasTextures)
		{
			CanvasFont->Textures.Add(Texture.Get());
		}
		uint32 MaxCanvasCodepoint = UFont::NULLCHARACTER;
		for (const FWoTRuntimeCharacterMap& Mapping :
			Resource->FontRecord.CharacterMap())
		{
			if (Mapping.Codepoint <= MAX_uint16)
			{
				MaxCanvasCodepoint = FMath::Max(
					MaxCanvasCodepoint, Mapping.Codepoint);
			}
		}
		CanvasFont->Characters.SetNum(static_cast<int32>(MaxCanvasCodepoint) + 1);
		for (const FWoTRuntimeCharacterMap& Mapping :
			Resource->FontRecord.CharacterMap())
		{
			if (Mapping.Codepoint > MAX_uint16
				|| !Resource->FontRecord.Glyphs().IsValidIndex(Mapping.Glyph))
			{
				continue;
			}
			const FWoTRuntimeGlyph& Glyph =
				Resource->FontRecord.Glyphs()[Mapping.Glyph];
			if (!CanvasFont->Textures.IsValidIndex(Glyph.Page))
			{
				return WoTFontOwnerError(
					TEXT("HUD canvas glyph references an unavailable atlas"));
			}
			FFontCharacter& Character =
				CanvasFont->Characters[static_cast<int32>(Mapping.Codepoint)];
			Character.StartU = Glyph.AtlasRect.Min.X;
			Character.StartV = Glyph.AtlasRect.Min.Y;
			Character.USize = Glyph.AtlasRect.Width();
			Character.VSize = Glyph.AtlasRect.Height();
			Character.TextureIndex = static_cast<uint8>(Glyph.Page);
			Character.VerticalOffset = Glyph.BearingY;
		}
		CanvasFont->IsRemapped = 0;
		CanvasFont->Kerning = 0;
		CanvasFont->CharRemap.Reset();
		CanvasFont->CacheCharacterCountAndMaxCharHeight();
		Resource->CanvasFont.Reset(CanvasFont);
		TArray<uint8> Bdf = WoTBuildBdf(Resource->FontRecord, Resource->AtlasPayloads);
		if (Bdf.IsEmpty())
			return WoTFontOwnerError(TEXT("authenticated bitmap face construction failed"));
		UFontFace* FontFace = NewObject<UFontFace>(GetTransientPackage(),
			*FString::Printf(TEXT("%s_AuthenticatedFace"), *Leaf));
		FontFace->SourceFilename = TEXT("authenticated-font-v2.bdf");
		FontFace->Hinting = EFontHinting::None;
		FontFace->LoadingPolicy = EFontLoadingPolicy::Inline;
		FontFace->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(Bdf));
		UFont* Font = NewObject<UFont>(GetTransientPackage(),
			*FString::Printf(TEXT("%s_AuthenticatedFont"), *Leaf));
		Font->FontCacheType = EFontCacheType::Runtime;
		FTypefaceEntry& Entry = Font->GetMutableInternalCompositeFont()
			.DefaultTypeface.Fonts.AddDefaulted_GetRef();
		Entry.Name = FName(TEXT("Regular"));
		Entry.Font = FFontData(FontFace);
		for (const TStrongObjectPtr<UTexture2D>& Texture : Resource->AtlasTextures)
			Font->Textures.Add(Texture.Get());
		Resource->SlateFace.Reset(FontFace);
		Resource->SlateFont.Reset(Font);
		if (!ValidateFreeTypeFace(*Resource))
		{
			// GetCharacterList caches the candidate before it is eligible for
			// publication. Flush it while its UObject graph is still strongly
			// owned so a failed face can never survive outside this owner.
			WoTFlushSlateFontResource(*Resource);
			return WoTFontOwnerError(
				TEXT("authenticated bitmap face failed FreeType validation"));
		}
		if (PinnedLease.IsValid())
		{
			FScopeLock PublicationAdmission(&PinnedLease->GetVerifiedReadGuard());
			if (PinnedLease->MayBeginVerifiedReadUnderVerifiedReadGuard()
				&& Payload.SharesSnapshotWith(*PinnedLease))
			{
				Resources.Add(FaceId, MoveTemp(Resource));
				return FWoTBootstrapStatus::Success();
			}
		}
		WoTFlushSlateFontResource(*Resource);
		return WoTFontOwnerError(
			TEXT("font materialization lost its captured cache generation"));
}

bool FWoTFrontEndFontOwner::ValidateFreeTypeFace(
	FWoTFrontEndFontResource& Resource)
{
#if WITH_AUTOMATION_TESTS
	++GWoTFreeTypeValidationAttempts;
#endif
	if (!FSlateApplication::IsInitialized()) return false;
	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (Renderer == nullptr) return false;
	const FWoTRuntimeGlyph* Representative = Resource.FontRecord.Glyphs()
		.FindByPredicate([](const FWoTRuntimeGlyph& Glyph)
		{
			return Glyph.AtlasRect.Width() > 0 && Glyph.AtlasRect.Height() > 0;
		});
	if (Representative == nullptr || Representative->Codepoint > MAX_uint16)
		return false;
	const FSlateFontInfo Info = Resource.SlateInfo(
		static_cast<float>(Resource.FontRecord.LineHeight()));
	FCharacterList& Characters = Renderer->GetFontCache()
		->GetCharacterList(Info, 1.0f);
	const FCharacterEntry& Character = Characters.GetCharacter(
		static_cast<TCHAR>(Representative->Codepoint), EFontFallback::FF_NoFallback);
	const bool bValid = Character.Valid && Character.GlyphIndex != 0
		&& Character.FallbackLevel == EFontFallback::FF_NoFallback
		&& Character.FontData != nullptr
		&& Character.FontData->GetFontFaceAsset() == Resource.SlateFaceObject();
#if WITH_AUTOMATION_TESTS
	GWoTLastValidatedFont = Resource.Font();
	if (GWoTForcePostCacheValidationFailure) return false;
#endif
	return bValid;
}

#if WITH_AUTOMATION_TESTS
void FWoTFrontEndFontOwner::ResetMaterializationWorkForTesting()
{
	GWoTAtlasMaterializationAttempts = 0;
	GWoTBdfConstructionAttempts = 0;
	GWoTFreeTypeValidationAttempts = 0;
	GWoTForcePostCacheValidationFailure = false;
	GWoTForceOptionalVariantFailure = false;
	GWoTLastValidatedFont.Reset();
}

uint32 FWoTFrontEndFontOwner::GetAtlasMaterializationAttemptsForTesting()
{
	return GWoTAtlasMaterializationAttempts;
}

uint32 FWoTFrontEndFontOwner::GetBdfConstructionAttemptsForTesting()
{
	return GWoTBdfConstructionAttempts;
}

uint32 FWoTFrontEndFontOwner::GetFreeTypeValidationAttemptsForTesting()
{
	return GWoTFreeTypeValidationAttempts;
}

void FWoTFrontEndFontOwner::SetForcePostCacheValidationFailureForTesting(bool bForce)
{
	GWoTForcePostCacheValidationFailure = bForce;
}

TWeakObjectPtr<UFont> FWoTFrontEndFontOwner::GetLastValidatedFontForTesting()
{
	return GWoTLastValidatedFont;
}

bool FWoTFrontEndFontOwner::BeginLeaseRevocationForTesting(
	TArray<TFunction<bool()>>& OutCallbacks) const
{
	if (!PinnedLease.IsValid()) return false;
	FScopeLock VerifiedRead(&PinnedLease->GetVerifiedReadGuard());
	return PinnedLease->BeginRevocationUnderVerifiedReadGuard(OutCallbacks);
}

void FWoTFrontEndFontOwner::FinishLeaseRevocationForTesting(
	TArray<TFunction<bool()>>&& Callbacks) const
{
	if (PinnedLease.IsValid())
		PinnedLease->FinishRevocation(MoveTemp(Callbacks));
}

FWoTBootstrapStatus FWoTFrontEndFontOwner::InitializeForTesting(
	UWoTPayloadSubsystem& Payload,
	const FWoTCacheGenerationLease& UnscopedLease)
{
	if (!UnscopedLease.IsValid())
	{
		return WoTFontOwnerError(
			TEXT("font automation requires the fixture's unscoped lease"));
	}
	FWoTCacheGenerationLease PayloadLease(UnscopedLease);
	const FString RequiredLogicalIds[] = {
		TEXT("cache/font/spec-reg30"), TEXT("cache/font/spec-reg14")
	};
	return InitializeRequired(
		Payload, PayloadLease, MakeArrayView(RequiredLogicalIds));
}
#endif

TWoTBootstrapResult<FSlateFontInfo> FWoTFrontEndFontOwner::Resolve(
	FName Face) const
{
	using FResult = TWoTBootstrapResult<FSlateFontInfo>;
	FWoTBootstrapStatus Available =
		const_cast<FWoTFrontEndFontOwner*>(this)->EnsureFace(Face);
	if (Available.IsError())
	{
		return FResult::Error(MoveTemp(Available).GetError());
	}
	const FWoTFrontEndFontResource* Resource = ResolveResource(Face);
	if (Resource == nullptr)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("an authenticated front-end font face is unavailable"),
			Face.ToString()));
	}
	return FResult::Success(Resource->SlateInfo(
		static_cast<float>(Resource->FontRecord.LineHeight())));
}

TWoTBootstrapResult<FSlateFontInfo> FWoTFrontEndFontOwner::ResolveIntegerScaled(
	FName Face, int32 PixelScale)
{
	using FResult = TWoTBootstrapResult<FSlateFontInfo>;
	if (PixelScale != 1 && PixelScale != 2)
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("multiplayer bitmap scale must be one or two")));
#if WITH_AUTOMATION_TESTS
	if (GWoTForceOptionalVariantFailure)
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("forced optional multiplayer bitmap strike failure")));
#endif
	if (!IsInGameThread() || !IsValid() || !PinnedLease->CanPublishPresentation())
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("multiplayer bitmap face requires an active presentation lease")));
	TUniquePtr<FWoTFrontEndFontResource>* Found = Resources.Find(Face);
	if (Found == nullptr || Found->Get() == nullptr)
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("an authenticated multiplayer font face is unavailable"),
			Face.ToString()));
	FWoTFrontEndFontResource& Resource = **Found;
	const uint32 NativeHeight = Resource.FontRecord.LineHeight();
	if (NativeHeight > static_cast<uint32>(MAX_int16 / PixelScale))
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("multiplayer bitmap strike exceeds Slate metrics")));
	TStrongObjectPtr<UFontFace>& VariantFace = PixelScale == 1
		? Resource.NativeSlateFace : Resource.DoubledSlateFace;
	TStrongObjectPtr<UFont>& VariantFont = PixelScale == 1
		? Resource.NativeSlateFont : Resource.DoubledSlateFont;
	if (VariantFont.Get() != nullptr)
	{
		FSlateFontInfo Cached(
			VariantFont.Get(), NativeHeight * PixelScale,
			FName(TEXT("Regular")));
		Cached.FontFallback = EFontFallback::FF_NoFallback;
		return FResult::Success(MoveTemp(Cached));
	}
	TArray<uint8> Bdf = WoTBuildBdf(Resource.FontRecord,
		Resource.AtlasPayloads, PixelScale, true);
	if (Bdf.IsEmpty())
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::SizeLimit,
			TEXT("authenticated multiplayer bitmap strike exceeds its bounds")));
	UFontFace* FontFace = NewObject<UFontFace>(GetTransientPackage());
	FontFace->SourceFilename = PixelScale == 1
		? TEXT("authenticated-font-v2-native.bdf")
		: TEXT("authenticated-font-v2-x2.bdf");
	FontFace->Hinting = EFontHinting::None;
	FontFace->LoadingPolicy = EFontLoadingPolicy::Inline;
	FontFace->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(Bdf));
	UFont* Font = NewObject<UFont>(GetTransientPackage());
	Font->FontCacheType = EFontCacheType::Runtime;
	FTypefaceEntry& Entry = Font->GetMutableInternalCompositeFont()
		.DefaultTypeface.Fonts.AddDefaulted_GetRef();
	Entry.Name = FName(TEXT("Regular"));
	Entry.Font = FFontData(FontFace);
	for (const TStrongObjectPtr<UTexture2D>& Texture : Resource.AtlasTextures)
		Font->Textures.Add(Texture.Get());
	VariantFace.Reset(FontFace);
	VariantFont.Reset(Font);
	const FWoTRuntimeGlyph* Representative = Resource.FontRecord.Glyphs()
		.FindByPredicate([](const FWoTRuntimeGlyph& Glyph)
		{
			return Glyph.Codepoint <= MAX_uint16
				&& Glyph.AtlasRect.Width() > 0 && Glyph.AtlasRect.Height() > 0;
		});
	bool bValid = false;
	if (Representative != nullptr && FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			FSlateFontInfo NativeInfo(Font,
				static_cast<float>(NativeHeight * PixelScale)
					* 72.0f / FontConstants::RenderDPI,
				FName(TEXT("Regular")));
			NativeInfo.FontFallback = EFontFallback::FF_NoFallback;
			FCharacterList& Characters = Renderer->GetFontCache()
				->GetCharacterList(NativeInfo, 1.0f);
			const FCharacterEntry& Character = Characters.GetCharacter(
				static_cast<TCHAR>(Representative->Codepoint),
				EFontFallback::FF_NoFallback);
			bValid = Character.Valid && Character.GlyphIndex != 0
				&& Character.FontData != nullptr
				&& Character.FontData->GetFontFaceAsset() == FontFace
				&& Character.USize == Representative->AtlasRect.Width()
					* PixelScale + 2
				&& Character.VSize == Representative->AtlasRect.Height()
					* PixelScale + 2
				&& FMath::IsNearlyEqual(Character.BitmapRenderScale, 1.0f,
					0.001f);
		}
	}
	if (!bValid || !IsValid() || !PinnedLease->CanPublishPresentation())
	{
		WoTFlushSlateFontPair(VariantFont.Get(), VariantFace.Get());
		VariantFont.Reset();
		VariantFace.Reset();
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("authenticated multiplayer bitmap strike failed validation")));
	}
	FSlateFontInfo Info(Font, NativeHeight * PixelScale,
		FName(TEXT("Regular")));
	Info.FontFallback = EFontFallback::FF_NoFallback;
	return FResult::Success(MoveTemp(Info));
}

UFont* FWoTFrontEndFontOwner::ResolveFontObject(FName Face) const
{
	FWoTBootstrapStatus Available =
		const_cast<FWoTFrontEndFontOwner*>(this)->EnsureFace(Face);
	if (Available.IsError()) return nullptr;
	const FWoTFrontEndFontResource* Resource = ResolveResource(Face);
	return Resource ? Resource->Font() : nullptr;
}

UFont* FWoTFrontEndFontOwner::ResolveCanvasFontObject(FName Face) const
{
	FWoTBootstrapStatus Available =
		const_cast<FWoTFrontEndFontOwner*>(this)->EnsureFace(Face);
	if (Available.IsError()) return nullptr;
	const FWoTFrontEndFontResource* Resource = ResolveResource(Face);
	return Resource ? Resource->CanvasFontObject() : nullptr;
}

const FWoTFrontEndFontResource* FWoTFrontEndFontOwner::ResolveResource(FName Id) const
{
	if (!IsInGameThread() || !IsValid() || !PinnedLease.IsValid()) return nullptr;
	FScopeLock ReadAdmission(&PinnedLease->GetVerifiedReadGuard());
	if (!PinnedLease->MayBeginVerifiedReadUnderVerifiedReadGuard()) return nullptr;
	const TUniquePtr<FWoTFrontEndFontResource>* Found = Resources.Find(Id);
	return Found ? Found->Get() : nullptr;
}

TWeakPtr<FWoTRequiredMenuFontGeneration>
FWoTFrontEndFontOwner::FontGeneration() const
{
	return IsValid() ? Generation : TWeakPtr<FWoTRequiredMenuFontGeneration>();
}

bool FWoTFrontEndFontOwner::IsValid() const
{
	return Active == this && PinnedLease.IsValid() && PinnedLease->IsValid()
		&& IsWoTPayloadBootstrapActive();
}

bool FWoTFrontEndFontOwner::SharesSnapshotWith(
	const FWoTCacheGenerationLease& Lease) const
{
	return IsValid() && PinnedLease.IsValid()
		&& PinnedLease->SharesSnapshotWith(Lease);
}

TUniquePtr<FWoTCacheGenerationLease>
FWoTFrontEndFontOwner::RetainPresentationLease() const
{
	if (!IsValid() || !PinnedLease.IsValid()) return nullptr;
	FScopeLock RetentionAdmission(&PinnedLease->GetVerifiedReadGuard());
	if (!PinnedLease->MayBeginVerifiedReadUnderVerifiedReadGuard()) return nullptr;
	return MakeUnique<FWoTCacheGenerationLease>(*PinnedLease);
}

void FWoTFrontEndFontOwner::Shutdown()
{
	if (Active == this)
	{
		if (Generation.IsValid()) Generation->Revoke();
		Active = nullptr;
	}
	for (const TPair<FName, TUniquePtr<FWoTFrontEndFontResource>>& Pair : Resources)
		WoTFlushSlateFontResource(*Pair.Value);
	Resources.Empty();
	FailedFaces.Empty();
	bReportedUnsupportedFace = false;
	PayloadOwner.Reset();
	Generation.Reset();
	PinnedLease.Reset();
}

const FWoTFrontEndFontOwner* FWoTFrontEndFontOwner::GetActive()
{
	return Active != nullptr && Active->IsValid() ? Active : nullptr;
}
