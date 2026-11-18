#include "WoTRuntimeFontResource.h"

#include "Containers/BitArray.h"
#include "Containers/StringConv.h"

namespace
{
FWoTBootstrapStatus WoTRuntimeFontError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

bool WoTCheckedAdd(uint64 Left, uint64 Right, uint64& Out)
{
	if (Left > MAX_uint64 - Right) return false;
	Out = Left + Right;
	return true;
}

bool WoTCheckedMultiply(uint64 Left, uint64 Right, uint64& Out)
{
	if (Left != 0 && Right > MAX_uint64 / Left) return false;
	Out = Left * Right;
	return true;
}

bool WoTIsUnicodeScalar(uint32 Codepoint)
{
	return Codepoint <= 0x10ffffu
		&& (Codepoint < 0xd800u || Codepoint > 0xdfffu);
}

struct FWoTRuntimeGlyphAliasKey
{
	uint16 Page = 0;
	FIntRect Rect;
	int16 Advance = 0;
	int16 BearingX = 0;
	int16 BearingY = 0;

	bool operator==(const FWoTRuntimeGlyphAliasKey& Other) const
	{
		return Page == Other.Page && Rect == Other.Rect
			&& Advance == Other.Advance && BearingX == Other.BearingX
			&& BearingY == Other.BearingY;
	}
	friend uint32 GetTypeHash(const FWoTRuntimeGlyphAliasKey& Key)
	{
		uint32 Hash = HashCombine(GetTypeHash(Key.Page), GetTypeHash(Key.Rect.Min));
		Hash = HashCombine(Hash, GetTypeHash(Key.Rect.Max));
		Hash = HashCombine(Hash, GetTypeHash(Key.Advance));
		Hash = HashCombine(Hash, GetTypeHash(Key.BearingX));
		return HashCombine(Hash, GetTypeHash(Key.BearingY));
	}
};

class FWoTRuntimeFontCursor final
{
public:
	explicit FWoTRuntimeFontCursor(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes)
	{
	}

	bool Skip(uint32 Count)
	{
		if (static_cast<uint64>(Offset) + Count
			> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool ReadU16(uint16& Out)
	{
		if (Offset + 2 > Bytes.Num())
		{
			return false;
		}
		Out = static_cast<uint16>(Bytes[Offset])
			| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
		Offset += 2;
		return true;
	}

	bool ReadU32(uint32& Out)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}
		Out = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	bool ReadString(FString& Out)
	{
		uint32 Count = 0;
		if (!ReadU32(Count) || Count > 1024u * 1024u
			|| static_cast<uint64>(Offset) + Count
				> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		if (Count == 0)
		{
			Out.Empty();
			return true;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			static_cast<int32>(Count));
		if (Converted.Length() <= 0)
		{
			return false;
		}
		Out = FString(Converted.Length(), Converted.Get());
		FTCHARToUTF8 RoundTrip(*Out);
		if (RoundTrip.Length() != static_cast<int32>(Count)
			|| FMemory::Memcmp(
				RoundTrip.Get(), Bytes.GetData() + Offset, Count) != 0)
		{
			return false;
		}
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool ReadSha256(FWoTSha256& Out)
	{
		if (Offset > Bytes.Num() - FWoTSha256::ByteCount)
		{
			return false;
		}
		FMemory::Memcpy(Out.Bytes, Bytes.GetData() + Offset,
			FWoTSha256::ByteCount);
		Offset += FWoTSha256::ByteCount;
		return true;
	}

	bool AtEnd() const { return Offset == Bytes.Num(); }

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool WoTRuntimeFontReadU32(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Header, Field, TEXT("font object header"), Out).IsSet();
}

bool WoTRuntimeFontReadString(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, FString& Out)
{
	return !ReadStringField(
		Header, Field, TEXT("font object header"), Out).IsSet();
}

bool WoTRuntimeFontValidateSource(
	const FWoTCanonicalJsonValue& Header, FString& OutExport)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* Fields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	FString Package;
	FString Hash;
	return !ReadObjectField(
			Header, TEXT("source"), TEXT("font object header"), Source).IsSet()
		&& Source != nullptr
		&& !RequireExactMembers(
			*Source, MakeArrayView(Fields), TEXT("font source")).IsSet()
		&& WoTRuntimeFontReadString(*Source, TEXT("export"), OutExport)
		&& WoTRuntimeFontReadString(*Source, TEXT("package"), Package)
		&& WoTRuntimeFontReadString(*Source, TEXT("sha256"), Hash)
		&& !Package.IsEmpty() && Hash.Len() == 64;
}
}

FWoTBootstrapStatus FWoTRuntimeFontResource::LoadFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (Object.IsSet() || Resource.Kind != EWoTNeutralResourceKind::Font)
	{
		return WoTRuntimeFontError(
			TEXT("font resource was reused or has the wrong semantic kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
		Resource, 256ull * 1024ull * 1024ull,
			TEXT("ue1-font-pages-v2"), TEXT("font"),
			TEXT("ue1-font-pages-v1"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	FWoTRuntimeVerifiedObject Verified =
		MoveTemp(Read).GetValue();
	const FWoTCanonicalJsonValue& Header = Verified.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("baseline"), TEXT("character_map_count"),
		TEXT("characters_per_page"), TEXT("encoding"), TEXT("fact_id"),
		TEXT("glyph_count"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kerning_count"), TEXT("kind"), TEXT("line_height"),
		TEXT("page_count"), TEXT("pages"),
		TEXT("payload_byte_length"), TEXT("source")
	};
	uint32 CharactersPerPage = 0;
	uint32 CharacterMapCount = 0;
	uint32 GlyphCount = 0;
	uint32 KerningCount = 0;
	uint32 PageCount = 0;
	uint32 Baseline = 0;
	uint32 LineHeight = 0;
	uint32 ImporterVersion = 0;
	FString FactId;
	FString Importer;
	const FWoTCanonicalJsonValue* Pages = Header.FindMember(TEXT("pages"));
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields),
			TEXT("font object header")).IsSet()
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("characters_per_page"), CharactersPerPage)
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("character_map_count"), CharacterMapCount)
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("glyph_count"), GlyphCount)
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("kerning_count"), KerningCount)
		|| !WoTRuntimeFontReadU32(Header, TEXT("baseline"), Baseline)
		|| !WoTRuntimeFontReadU32(Header, TEXT("line_height"), LineHeight)
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("page_count"), PageCount)
		|| !WoTRuntimeFontReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimeFontReadString(Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimeFontReadString(Header, TEXT("importer"), Importer)
		|| CharactersPerPage == 0
		|| (CharactersPerPage & (CharactersPerPage - 1)) != 0
		|| CharactersPerPage > WoTRuntimeFontLimits::MaxGlyphs
		|| PageCount == 0 || PageCount > WoTRuntimeFontLimits::MaxPages
		|| GlyphCount == 0 || GlyphCount > WoTRuntimeFontLimits::MaxGlyphs
		|| CharacterMapCount != GlyphCount
		|| KerningCount > WoTRuntimeFontLimits::MaxKerningPairs
		|| LineHeight == 0 || LineHeight > WoTRuntimeFontLimits::MaxLineMetric
		|| Baseline > LineHeight
		|| Pages == nullptr || !Pages->IsArray()
		|| Pages->Num() != static_cast<int32>(PageCount)
		|| FactId.IsEmpty() || Importer != TEXT("font-v2")
		|| ImporterVersion != 2
		|| !WoTRuntimeFontValidateSource(Header, ParsedSourceExport)
		|| ParsedSourceExport.IsEmpty())
	{
		return WoTRuntimeFontError(TEXT("font object header is malformed"));
	}

	TArray<FWoTRuntimeFontPage> HeaderPages;
	HeaderPages.Reserve(Pages->Num());
	TSet<FString> HeaderLogicalIds;
	FWoTRuntimeFontMaterializationPlan Plan;
	uint64 DeclaredGlyphs = 0;
	FString SourceHash;
	const FWoTCanonicalJsonValue* SourceObject = nullptr;
	if (ReadObjectField(Header, TEXT("source"), TEXT("font object header"),
		SourceObject).IsSet() || SourceObject == nullptr
		|| !WoTRuntimeFontReadString(*SourceObject, TEXT("sha256"), SourceHash))
	{
		return WoTRuntimeFontError(TEXT("font source digest is malformed"));
	}
	for (int32 PageIndex = 0; PageIndex < Pages->Num(); ++PageIndex)
	{
		const FWoTCanonicalJsonValue& Page = Pages->GetElement(PageIndex);
		static const TCHAR* PageFields[] = {
			TEXT("character_count"), TEXT("format"), TEXT("height"),
			TEXT("logical_id"), TEXT("ordinal"), TEXT("source_identity"),
			TEXT("source_sha256"), TEXT("width")
		};
		FWoTRuntimeFontPage& Parsed = HeaderPages.AddDefaulted_GetRef();
		FString HeaderSha;
		if (RequireExactMembers(
				Page, MakeArrayView(PageFields), TEXT("font page")).IsSet()
			|| !WoTRuntimeFontReadU32(
				Page, TEXT("character_count"), Parsed.CharacterCount)
			|| !WoTRuntimeFontReadU32(Page, TEXT("ordinal"), Parsed.Ordinal)
			|| !WoTRuntimeFontReadU32(Page, TEXT("height"), Parsed.Height)
			|| !WoTRuntimeFontReadU32(Page, TEXT("width"), Parsed.Width)
			|| !WoTRuntimeFontReadString(
				Page, TEXT("logical_id"), Parsed.TextureLogicalId)
			|| !WoTRuntimeFontReadString(
				Page, TEXT("source_identity"), Parsed.SourceIdentity)
			|| !WoTRuntimeFontReadString(Page, TEXT("format"), Parsed.Format)
			|| !WoTRuntimeFontReadString(Page, TEXT("source_sha256"), HeaderSha)
			|| Parsed.Ordinal != static_cast<uint32>(PageIndex)
			|| Parsed.CharacterCount > CharactersPerPage
			|| Parsed.Width == 0
			|| Parsed.Width > WoTRuntimeFontLimits::MaxPageDimension
			|| Parsed.Height == 0
			|| Parsed.Height > WoTRuntimeFontLimits::MaxPageDimension
			|| Parsed.TextureLogicalId.IsEmpty() || Parsed.SourceIdentity.IsEmpty()
			|| Parsed.Format != TEXT("p8") || HeaderSha != SourceHash
			|| HeaderLogicalIds.Contains(Parsed.TextureLogicalId))
		{
			return WoTRuntimeFontError(TEXT("font page header is malformed"));
		}
		uint64 PagePixels = 0;
		uint64 AtlasPixels = 0;
		uint64 ExpansionBytes = 0;
		if (!WoTCheckedMultiply(Parsed.Width, Parsed.Height, PagePixels)
			|| !WoTCheckedAdd(Plan.AtlasPixels, PagePixels, AtlasPixels)
			|| !WoTCheckedMultiply(AtlasPixels, sizeof(FColor), ExpansionBytes)
			|| AtlasPixels > WoTRuntimeFontLimits::MaxAtlasPixels
			|| ExpansionBytes > WoTRuntimeFontLimits::MaxAtlasExpansionBytes
			|| !WoTCheckedAdd(
				DeclaredGlyphs, Parsed.CharacterCount, DeclaredGlyphs)
			|| DeclaredGlyphs > WoTRuntimeFontLimits::MaxGlyphs)
		{
			return WoTRuntimeFontError(
				TEXT("font atlas materialization budget is exceeded"));
		}
		Plan.AtlasPixels = AtlasPixels;
		Plan.AtlasExpansionBytes = ExpansionBytes;
		TWoTBootstrapResult<FWoTSha256> ParsedSha = FWoTSha256::FromHex(HeaderSha);
		if (ParsedSha.IsError())
		{
			return WoTRuntimeFontError(TEXT("font page source digest is malformed"));
		}
		Parsed.SourceSha256 = MoveTemp(ParsedSha).GetValue();
		HeaderLogicalIds.Add(Parsed.TextureLogicalId);
	}
	if (DeclaredGlyphs != GlyphCount)
	{
		return WoTRuntimeFontError(
			TEXT("font page glyph counts do not close"));
	}

	const TConstArrayView<uint8> Payload = Verified.Payload();
	FWoTRuntimeFontCursor Cursor(Payload);
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 PayloadCharactersPerPage = 0;
	uint32 PayloadPageCount = 0;
	uint32 PayloadGlyphCount = 0;
	uint32 PayloadCharacterMapCount = 0;
	uint32 PayloadKerningCount = 0;
	uint32 PayloadBaseline = 0;
	uint32 PayloadLineHeight = 0;
	if (Payload.Num() < 40
		|| FMemory::Memcmp(Payload.GetData(), "WOTFNT3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags)
		|| !Cursor.ReadU32(PayloadCharactersPerPage)
		|| !Cursor.ReadU32(PayloadPageCount)
		|| !Cursor.ReadU32(PayloadGlyphCount)
		|| !Cursor.ReadU32(PayloadCharacterMapCount)
		|| !Cursor.ReadU32(PayloadKerningCount)
		|| !Cursor.ReadU32(PayloadBaseline)
		|| !Cursor.ReadU32(PayloadLineHeight)
		|| Version != 2 || Flags != 0
		|| PayloadCharactersPerPage != CharactersPerPage
		|| PayloadPageCount != PageCount || PayloadGlyphCount != GlyphCount
		|| PayloadCharacterMapCount != CharacterMapCount
		|| PayloadKerningCount != KerningCount
		|| PayloadBaseline != Baseline || PayloadLineHeight != LineHeight)
	{
		return WoTRuntimeFontError(TEXT("font payload framing is malformed"));
	}
	TArray<FWoTRuntimeGlyph> Glyphs;
	Glyphs.Reserve(static_cast<int32>(GlyphCount));
	TArray<uint32> GlyphsPerPage;
	GlyphsPerPage.Init(0, static_cast<int32>(PageCount));
	TArray<TBitArray<>> OccupiedPixels;
	OccupiedPixels.Reserve(static_cast<int32>(PageCount));
	for (const FWoTRuntimeFontPage& Page : HeaderPages)
	{
		const uint64 PagePixels = static_cast<uint64>(Page.Width) * Page.Height;
		TBitArray<>& PageBits = OccupiedPixels.AddDefaulted_GetRef();
		PageBits.Init(false, static_cast<int32>(PagePixels));
	}
	TSet<FWoTRuntimeGlyphAliasKey> GlyphAliases;
	GlyphAliases.Reserve(static_cast<int32>(GlyphCount));
	Plan.BdfBytes = 4096;
	uint32 ExpectedGlyph = 0;
	for (uint32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
	{
		uint32 Ordinal = 0;
		FString TextureLogicalId;
		FString SourceIdentity;
		FString Format;
		uint32 Width = 0;
		uint32 Height = 0;
		FWoTSha256 SourceSha;
		uint32 PageGlyphCount = 0;
		const FWoTRuntimeFontPage& Page = HeaderPages[static_cast<int32>(PageIndex)];
		if (!Cursor.ReadU32(Ordinal)
			|| !Cursor.ReadString(TextureLogicalId)
			|| !Cursor.ReadString(SourceIdentity)
			|| !Cursor.ReadU32(Width) || !Cursor.ReadU32(Height)
			|| !Cursor.ReadString(Format) || !Cursor.ReadSha256(SourceSha)
			|| !Cursor.ReadU32(PageGlyphCount)
			|| Ordinal != PageIndex || TextureLogicalId != Page.TextureLogicalId
			|| SourceIdentity != Page.SourceIdentity || Width != Page.Width
			|| Height != Page.Height || Format != Page.Format
			|| SourceSha != Page.SourceSha256 || PageGlyphCount != Page.CharacterCount)
		{
			return WoTRuntimeFontError(
				TEXT("font payload page identity or count mismatched"));
		}
	}
	TArray<FWoTRuntimeCharacterMap> CharacterMap;
	CharacterMap.Reserve(static_cast<int32>(CharacterMapCount));
	for (uint32 MapIndex = 0; MapIndex < CharacterMapCount; ++MapIndex)
	{
		FWoTRuntimeCharacterMap& Entry = CharacterMap.AddDefaulted_GetRef();
		if (!Cursor.ReadU32(Entry.Codepoint) || !Cursor.ReadU32(Entry.Glyph)
			|| Entry.Glyph != MapIndex
			|| !WoTIsUnicodeScalar(Entry.Codepoint)
			|| (MapIndex > 0 && Entry.Codepoint <= CharacterMap[MapIndex - 1].Codepoint))
		{
			return WoTRuntimeFontError(TEXT("font character map is malformed"));
		}
	}
	for (uint32 GlyphIndex = 0; GlyphIndex < GlyphCount; ++GlyphIndex)
	{
		uint32 Codepoint = 0;
		uint32 EncodedPage = 0;
		uint32 X = 0;
		uint32 Y = 0;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Advance = 0;
		uint32 BearingX = 0;
		uint32 BearingY = 0;
		if (!Cursor.ReadU32(Codepoint) || !Cursor.ReadU32(EncodedPage)
			|| !Cursor.ReadU32(X) || !Cursor.ReadU32(Y)
			|| !Cursor.ReadU32(Width) || !Cursor.ReadU32(Height)
			|| !Cursor.ReadU32(Advance) || !Cursor.ReadU32(BearingX)
			|| !Cursor.ReadU32(BearingY) || EncodedPage >= PageCount
			|| CharacterMap[GlyphIndex].Codepoint != Codepoint
			|| CharacterMap[GlyphIndex].Glyph != GlyphIndex
			|| !WoTIsUnicodeScalar(Codepoint)
			|| EncodedPage > MAX_uint16 || Advance > MAX_int16
			|| BearingX > static_cast<uint32>(MAX_int16)
			|| BearingY > static_cast<uint32>(MAX_int16))
		{
			return WoTRuntimeFontError(TEXT("font glyph record is malformed"));
		}
		const FWoTRuntimeFontPage& Page = HeaderPages[static_cast<int32>(EncodedPage)];
		if (X > Page.Width || Y > Page.Height
			|| Width > Page.Width - X || Height > Page.Height - Y)
		{
			return WoTRuntimeFontError(TEXT("font glyph extends outside its atlas"));
		}
		uint64 GlyphPixels = 0;
		uint64 TotalGlyphPixels = 0;
		uint64 GlyphBdfBytes = 0;
		uint64 TotalBdfBytes = 0;
		if (!WoTCheckedMultiply(Width, Height, GlyphPixels)
			|| !WoTCheckedAdd(Plan.GlyphPixels, GlyphPixels, TotalGlyphPixels)
			|| !WoTCheckedMultiply(GlyphPixels, 2, GlyphBdfBytes)
			// Each bitmap row includes three parser-read padding spaces and LF.
			|| !WoTCheckedAdd(GlyphBdfBytes, static_cast<uint64>(Height) * 4, GlyphBdfBytes)
			|| !WoTCheckedAdd(GlyphBdfBytes, 512, GlyphBdfBytes)
			|| !WoTCheckedAdd(Plan.BdfBytes, GlyphBdfBytes, TotalBdfBytes)
			|| TotalGlyphPixels > WoTRuntimeFontLimits::MaxGlyphPixels
			|| TotalBdfBytes > WoTRuntimeFontLimits::MaxBdfBytes)
		{
			return WoTRuntimeFontError(
				TEXT("font glyph materialization budget is exceeded"));
		}
		TBitArray<>& PageBits = OccupiedPixels[static_cast<int32>(EncodedPage)];
		const FIntRect CandidateRect(X, Y, X + Width, Y + Height);
		const FWoTRuntimeGlyphAliasKey AliasKey{
			static_cast<uint16>(EncodedPage), CandidateRect,
			static_cast<int16>(Advance), static_cast<int16>(BearingX),
			static_cast<int16>(BearingY)};
		const bool bExactAlias = GlyphAliases.Contains(AliasKey);
		for (uint32 Row = 0; Row < Height; ++Row)
		{
			const uint64 RowStart = static_cast<uint64>(Y + Row) * Page.Width + X;
			for (uint32 Column = 0; Column < Width; ++Column)
			{
				const int32 Pixel = static_cast<int32>(RowStart + Column);
				if (PageBits[Pixel] && !bExactAlias)
				{
					return WoTRuntimeFontError(
						TEXT("font glyph rectangles overlap"));
				}
				if (!bExactAlias) PageBits[Pixel] = true;
			}
		}
		GlyphAliases.Add(AliasKey);
		Plan.GlyphPixels = TotalGlyphPixels;
		Plan.BdfBytes = TotalBdfBytes;
		FWoTRuntimeGlyph& Glyph = Glyphs.AddDefaulted_GetRef();
		Glyph.Codepoint = Codepoint;
		Glyph.Page = static_cast<uint16>(EncodedPage);
		Glyph.AtlasRect = CandidateRect;
		Glyph.Advance = static_cast<int16>(Advance);
		Glyph.BearingX = static_cast<int16>(BearingX);
		Glyph.BearingY = static_cast<int16>(BearingY);
		++GlyphsPerPage[static_cast<int32>(EncodedPage)];
		++ExpectedGlyph;
	}
	for (uint32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
	{
		if (GlyphsPerPage[static_cast<int32>(PageIndex)]
			!= HeaderPages[static_cast<int32>(PageIndex)].CharacterCount)
		{
			return WoTRuntimeFontError(
				TEXT("font glyph page distribution mismatched"));
		}
	}
	TArray<FWoTRuntimeKerning> Kerning;
	Kerning.Reserve(static_cast<int32>(KerningCount));
	for (uint32 Index = 0; Index < KerningCount; ++Index)
	{
		FWoTRuntimeKerning& Entry = Kerning.AddDefaulted_GetRef();
		uint32 Adjustment = 0;
		if (!Cursor.ReadU32(Entry.LeftCodepoint)
			|| !Cursor.ReadU32(Entry.RightCodepoint)
			|| !Cursor.ReadU32(Adjustment))
		{
			return WoTRuntimeFontError(TEXT("font kerning record is malformed"));
		}
		if (!WoTIsUnicodeScalar(Entry.LeftCodepoint)
			|| !WoTIsUnicodeScalar(Entry.RightCodepoint))
		{
			return WoTRuntimeFontError(
				TEXT("font kerning codepoint is not a Unicode scalar"));
		}
		Entry.Adjustment = static_cast<int32>(Adjustment);
	}
	if (!Cursor.AtEnd() || ExpectedGlyph != GlyphCount)
	{
		return WoTRuntimeFontError(
			TEXT("font glyph count or payload tail mismatched"));
	}
	ParsedCharactersPerPage = CharactersPerPage;
	ParsedBaseline = Baseline;
	ParsedLineHeight = LineHeight;
	ParsedPages = MoveTemp(HeaderPages);
	ParsedCharacterMap = MoveTemp(CharacterMap);
	ParsedGlyphs = MoveTemp(Glyphs);
	ParsedKerning = MoveTemp(Kerning);
	ParsedMaterializationPlan = Plan;
	Object.Emplace(MoveTemp(Verified));
	return FWoTBootstrapStatus::Success();
}
