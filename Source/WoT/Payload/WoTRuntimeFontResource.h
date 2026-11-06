// Runtime font bytes and semantic metrics loaded from cache-v3 at first use.
#pragma once

#include "CoreMinimal.h"
#include "WoTNeutralResources.h"
#include "WoTRuntimeRegistry.h"

struct FWoTRuntimeGlyph
{
	uint32 Codepoint = 0;
	uint16 Page = 0;
	FIntRect AtlasRect;
	int16 BearingX = 0;
	int16 BearingY = 0;
	int16 Advance = 0;
};

struct FWoTRuntimeFontPage
{
	uint32 Ordinal = 0;
	FString TextureLogicalId;
	FString SourceIdentity;
	uint32 Width = 0;
	uint32 Height = 0;
	FString Format;
	FWoTSha256 SourceSha256;
	uint32 CharacterCount = 0;
};

struct FWoTRuntimeCharacterMap
{
	uint32 Codepoint = 0;
	uint32 Glyph = 0;
};

struct FWoTRuntimeKerning
{
	uint32 LeftCodepoint = 0;
	uint32 RightCodepoint = 0;
	int32 Adjustment = 0;
};

namespace WoTRuntimeFontLimits
{
constexpr uint32 MaxPages = 256;
constexpr uint32 MaxGlyphs = 65536;
constexpr uint32 MaxKerningPairs = 65536;
constexpr uint32 MaxPageDimension = 4096;
constexpr uint32 MaxLineMetric = 4096;
constexpr uint64 MaxAtlasPixels = 16ull * 1024ull * 1024ull;
constexpr uint64 MaxAtlasExpansionBytes = 64ull * 1024ull * 1024ull;
constexpr uint64 MaxGlyphPixels = 16ull * 1024ull * 1024ull;
constexpr uint64 MaxBdfBytes = 64ull * 1024ull * 1024ull;
}

struct FWoTRuntimeFontMaterializationPlan
{
	uint64 AtlasPixels = 0;
	uint64 AtlasExpansionBytes = 0;
	uint64 GlyphPixels = 0;
	uint64 BdfBytes = 0;
};

class WOT_API FWoTRuntimeFontResource final
{
public:
	FWoTRuntimeFontResource() = default;
	FWoTRuntimeFontResource(FWoTRuntimeFontResource&&) = default;
	FWoTRuntimeFontResource& operator=(FWoTRuntimeFontResource&&) = default;
	FWoTRuntimeFontResource(const FWoTRuntimeFontResource&) = delete;
	FWoTRuntimeFontResource& operator=(const FWoTRuntimeFontResource&) = delete;

	FWoTBootstrapStatus LoadFirstUse(const FWoTNeutralResourceId& Resource);
	bool IsLoaded() const { return Object.IsSet(); }
	TConstArrayView<uint8> VerifiedBytes() const
	{
		return Object.IsSet() ? Object->Payload() : TConstArrayView<uint8>();
	}
	const TArray<FWoTRuntimeFontPage>& Pages() const { return ParsedPages; }
	const TArray<FWoTRuntimeCharacterMap>& CharacterMap() const { return ParsedCharacterMap; }
	const TArray<FWoTRuntimeGlyph>& Glyphs() const { return ParsedGlyphs; }
	const TArray<FWoTRuntimeKerning>& Kerning() const { return ParsedKerning; }
	uint32 CharactersPerPage() const { return ParsedCharactersPerPage; }
	uint32 Baseline() const { return ParsedBaseline; }
	uint32 LineHeight() const { return ParsedLineHeight; }
	const FString& SourceExport() const { return ParsedSourceExport; }
	const FWoTRuntimeFontMaterializationPlan& MaterializationPlan() const
	{
		return ParsedMaterializationPlan;
	}

private:
	TOptional<FWoTRuntimeVerifiedObject> Object;
	TArray<FWoTRuntimeFontPage> ParsedPages;
	TArray<FWoTRuntimeCharacterMap> ParsedCharacterMap;
	TArray<FWoTRuntimeGlyph> ParsedGlyphs;
	TArray<FWoTRuntimeKerning> ParsedKerning;
	uint32 ParsedCharactersPerPage = 0;
	uint32 ParsedBaseline = 0;
	uint32 ParsedLineHeight = 0;
	FString ParsedSourceExport;
	FWoTRuntimeFontMaterializationPlan ParsedMaterializationPlan;
};
