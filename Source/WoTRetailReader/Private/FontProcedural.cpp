#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/CacheBuilder.h"

#include "CanonicalObjectBuilder.h"
#include "RemainingPackage.h"
#include "StructSchema.h"
#include "TypedProperties.h"
#include "Containers/BitArray.h"
#include "Containers/StringConv.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr uint32 WoTRemainingMaxFontPages = 256;
constexpr uint32 WoTRemainingMaxGlyphs = 65536;
constexpr uint32 WoTRemainingMaxKerningPairs = 65536;
constexpr uint32 WoTRemainingMaxFontPageDimension = 4096;
constexpr uint32 WoTRemainingMaxFontLineMetric = 4096;
constexpr uint64 WoTRemainingMaxFontAtlasPixels = 16ull * 1024 * 1024;
constexpr uint64 WoTRemainingMaxFontAtlasExpansionBytes = 64ull * 1024 * 1024;
constexpr uint64 WoTRemainingMaxFontGlyphPixels = 16ull * 1024 * 1024;
constexpr uint64 WoTRemainingMaxFontBdfBytes = 64ull * 1024 * 1024;
constexpr uint32 WoTRemainingMaxStates = 65536;
constexpr uint32 WoTRemainingRandomSeed = 1;
constexpr ANSICHAR WoTRemainingObjectMagic[] = "WOTOBJ3";

bool WoTRemainingCheckedAdd(uint64 Left, uint64 Right, uint64& Out)
{
	if (Left > MAX_uint64 - Right) return false;
	Out = Left + Right;
	return true;
}

bool WoTRemainingCheckedMultiply(uint64 Left, uint64 Right, uint64& Out)
{
	if (Left != 0 && Right > MAX_uint64 / Left) return false;
	Out = Left * Right;
	return true;
}

bool WoTRemainingIsUnicodeScalar(uint32 Codepoint)
{
	return Codepoint <= 0x10ffffu
		&& (Codepoint < 0xd800u || Codepoint > 0xdfffu);
}

FWoTBootstrapStatus WoTRemainingComposeFontCodepoint(uint32 PageIndex,
	uint32 CharactersPerPage, uint32 LocalIndex, uint32& OutCodepoint)
{
	uint64 Product = 0;
	uint64 Sum = 0;
	if (CharactersPerPage == 0 || LocalIndex >= CharactersPerPage
		|| !WoTRemainingCheckedMultiply(PageIndex, CharactersPerPage, Product)
		|| !WoTRemainingCheckedAdd(Product, LocalIndex, Sum)
		|| Sum > MAX_uint32)
	{
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::SizeLimit,
			TEXT("font codepoint arithmetic overflowed")));
	}
	OutCodepoint = static_cast<uint32>(Sum);
	if (!WoTRemainingIsUnicodeScalar(OutCodepoint))
	{
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			OutCodepoint >= 0xd800u && OutCodepoint <= 0xdfffu
				? TEXT("font codepoint is a surrogate")
				: TEXT("font codepoint exceeds Unicode")));
	}
	return FWoTBootstrapStatus::Success();
}

struct FWoTRemainingGlyphAliasKey
{
	uint32 Page = 0;
	FIntRect Rect;
	int32 Advance = 0;
	int32 BearingX = 0;
	int32 BearingY = 0;

	bool operator==(const FWoTRemainingGlyphAliasKey& Other) const
	{
		return Page == Other.Page && Rect == Other.Rect
			&& Advance == Other.Advance && BearingX == Other.BearingX
			&& BearingY == Other.BearingY;
	}
	friend uint32 GetTypeHash(const FWoTRemainingGlyphAliasKey& Key)
	{
		uint32 Hash = HashCombine(GetTypeHash(Key.Page), GetTypeHash(Key.Rect.Min));
		Hash = HashCombine(Hash, GetTypeHash(Key.Rect.Max));
		Hash = HashCombine(Hash, GetTypeHash(Key.Advance));
		Hash = HashCombine(Hash, GetTypeHash(Key.BearingX));
		return HashCombine(Hash, GetTypeHash(Key.BearingY));
	}
};

void WoTRemainingAppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void WoTRemainingAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

bool WoTRemainingAppendString(TArray<uint8>& Bytes, FStringView Value)
{
	const FString Owned(Value);
	FTCHARToUTF8 Encoded(*Owned);
	if (Encoded.Length() < 0 || Encoded.Length() > 1024 * 1024) return false;
	WoTRemainingAppendU32(Bytes, static_cast<uint32>(Encoded.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	return true;
}

TWoTBootstrapResult<TArray<uint8>> WoTRemainingFrame(
	const FWoTCanonicalJsonValue& Header, TConstArrayView<uint8> Payload)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> HeaderBytes = WriteCanonicalJson(Header);
	if (HeaderBytes.IsError()) return FResult::Error(MoveTemp(HeaderBytes).GetError());
	if (HeaderBytes.GetValue().IsEmpty()
		|| HeaderBytes.GetValue().Num() > 1024 * 1024
		|| static_cast<uint64>(Payload.Num()) > WoTRemainingMaxSourceBytes)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("the remaining-domain object exceeds its framing bound")));
	}
	TArray<uint8> Object;
	Object.Reserve(12 + HeaderBytes.GetValue().Num() + Payload.Num());
	Object.Append(reinterpret_cast<const uint8*>(WoTRemainingObjectMagic), 8);
	WoTRemainingAppendU32(Object, static_cast<uint32>(HeaderBytes.GetValue().Num()));
	Object.Append(HeaderBytes.GetValue());
	Object.Append(Payload.GetData(), Payload.Num());
	return FResult::Success(MoveTemp(Object));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTRemainingSourceJson(
	const FWoTRequiredFact& Fact, const FWoTRemainingSource& Source)
{
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("export"), FWoTCanonicalJsonValue::MakeString(Fact.SourceExport));
	Builder.Add(TEXT("package"), FWoTCanonicalJsonValue::MakeString(Fact.SourcePath));
	Builder.Add(TEXT("sha256"),
		FWoTCanonicalJsonValue::MakeString(Source.Sha256.ToHex()));
	return MoveTemp(Builder).Build(TEXT("remaining-domain source"));
}

FWoTBootstrapStatus WoTRemainingBindProperties(
	const FWoTRemainingSource& Source, TArray<FWoTTypedProperty>& Properties)
{
	return WoTBindTypedPropertyReferences(
		Properties, [&Source](int32 Reference)
			-> TWoTBootstrapResult<FWoTTypedObjectBinding>
		{
			using FBindingResult = TWoTBootstrapResult<FWoTTypedObjectBinding>;
			TWoTBootstrapResult<FString> Identity =
				WoTRemainingCanonicalIdentity(Source, Reference, true);
			TWoTBootstrapResult<FString> Class =
				WoTRemainingObjectClass(Source, Reference);
			if (Identity.IsError())
				return FBindingResult::Error(MoveTemp(Identity).GetError());
			if (Class.IsError())
				return FBindingResult::Error(MoveTemp(Class).GetError());
			FWoTBootstrapStatus Closed = WoTRemainingRequireSemanticReference(
				Source, Identity.GetValue(), Class.GetValue(), TEXT("typed property"));
			if (Closed.IsError())
				return FBindingResult::Error(MoveTemp(Closed).GetError());
			return FBindingResult::Success(FWoTTypedObjectBinding{
				MoveTemp(Identity).GetValue(), MoveTemp(Class).GetValue()});
		});
}

bool WoTRemainingReadProperties(FWoTRemainingReader& Reader,
	const FWoTRemainingSource& Source, TArray<FWoTTypedProperty>& Out,
	FStringView Context, bool bSavedClassDefaults = false)
{
	(void)Context;
	FWoTTypedPropertyReadOptions Options;
	Options.MaximumProperties = WoTRemainingMaxItems;
	Options.bSavedClassDefaults = bSavedClassDefaults;
	Options.bRejectUnsupportedTypes = true;
	Options.bRejectDuplicateIndexedFields = true;
	Options.bNormalizeNegativeZero = true;
	Options.bSortCanonical = true;
	Options.bDecodeKnownStructs = !bSavedClassDefaults;
	if (!WoTReadTypedProperties(Reader,
		[&Source](FWoTRemainingReader& From, FString& Value)
		{
			return WoTRemainingReadName(From, Source, Value);
		},
		[&Source](int32 Reference)
		{
			return WoTRemainingReferenceValid(Source, Reference);
		}, Out, Options)) return false;
	FWoTBootstrapStatus Bound = WoTRemainingBindProperties(Source, Out);
	return !Bound.IsError();
}

bool WoTRemainingAppendProperties(TArray<uint8>& Payload,
	const TArray<FWoTTypedProperty>& Properties)
{
	return WoTAppendCanonicalTypedProperties(Payload, Properties);
}

TWoTBootstrapResult<const FWoTRemainingExport*> WoTRemainingFindExport(
	const FWoTRemainingSource& Source, FStringView Identity,
	const TCHAR* RequiredClass)
{
	using FResult = TWoTBootstrapResult<const FWoTRemainingExport*>;
	const int32* Reference =
		Source.CanonicalReferenceByIdentity.Find(WoTRemainingFold(Identity));
	if (Reference == nullptr || *Reference <= 0
		|| !Source.Exports.IsValidIndex(*Reference - 1))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("the exact remaining-domain export was not found"), FString(Identity)));
	}
	const FWoTRemainingExport& Export = Source.Exports[*Reference - 1];
	const FString* ClassName = Source.ObjectName(Export.ClassReference);
	if (ClassName == nullptr || !ClassName->Equals(
		RequiredClass, ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("the resolved remaining-domain export has the wrong class"),
			FString(Identity)));
	}
	return FResult::Success(&Export);
}

struct FWoTRemainingResolvedProceduralExport
{
	const FWoTRemainingExport* Export = nullptr;
	FString CanonicalIdentity;
};

TWoTBootstrapResult<FWoTRemainingResolvedProceduralExport>
WoTRemainingFindProceduralExport(
	const FWoTRemainingSource& Source, FStringView Identity)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingResolvedProceduralExport>;
	const int32* Reference =
		Source.CanonicalReferenceByIdentity.Find(WoTRemainingFold(Identity));
	int32 ResolvedReference = Reference == nullptr ? 0 : *Reference;
	if (ResolvedReference == 0)
	{
		// The signed production contract predates collision suffixes and has one
		// raw ParticleSystems path shared by a Palette and a FireTexture. Keep
		// canonical lookup primary, then accept a raw path only when its
		// procedural-class projection is itself injective.
		for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
		{
			const FWoTRemainingExport& Candidate = Source.Exports[ExportIndex];
			const FString* CandidateClass = Source.ObjectName(Candidate.ClassReference);
			if (CandidateClass == nullptr
				|| (*CandidateClass != TEXT("FireTexture")
					&& *CandidateClass != TEXT("WaterTexture")
					&& *CandidateClass != TEXT("WaveTexture")
					&& *CandidateClass != TEXT("WetTexture")
					&& *CandidateClass != TEXT("IceTexture")))
			{
				continue;
			}
			TWoTBootstrapResult<FString> RawIdentity =
				WoTRemainingObjectPath(Source, ExportIndex + 1);
			if (RawIdentity.IsError())
				return FResult::Error(MoveTemp(RawIdentity).GetError());
			if (!FStringView(RawIdentity.GetValue()).Equals(
				Identity, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ResolvedReference != 0)
			{
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::DuplicateField,
					TEXT("the raw procedural source identity is ambiguous"),
					FString(Identity)));
			}
			ResolvedReference = ExportIndex + 1;
		}
	}
	if (ResolvedReference <= 0
		|| !Source.Exports.IsValidIndex(ResolvedReference - 1))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("the exact procedural source export was not found"), FString(Identity)));
	}
	const FWoTRemainingExport& Export = Source.Exports[ResolvedReference - 1];
	const FString* ClassName = Source.ObjectName(Export.ClassReference);
	if (ClassName == nullptr
		|| (*ClassName != TEXT("FireTexture") && *ClassName != TEXT("WaterTexture")
			&& *ClassName != TEXT("WaveTexture") && *ClassName != TEXT("WetTexture")
			&& *ClassName != TEXT("IceTexture")))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("the resolved procedural export has the wrong class"),
			FString(Identity)));
	}
	const FString* CanonicalIdentity =
		Source.CanonicalIdentityByReference.Find(ResolvedReference);
	if (CanonicalIdentity == nullptr)
	{
		return FResult::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the procedural export lacks a canonical registry identity"),
			FString(Identity)));
	}
	return FResult::Success(
		FWoTRemainingResolvedProceduralExport{&Export, *CanonicalIdentity});
}

struct FWoTRemainingFontGlyph
{
	uint32 X = 0;
	uint32 Y = 0;
	uint32 Width = 0;
	uint32 Height = 0;
};

struct FWoTRemainingFontPage
{
	FString TextureIdentity;
	FString TextureLogicalId;
	uint32 Width = 0;
	uint32 Height = 0;
	TArray<FWoTRemainingFontGlyph> Glyphs;
};

struct FWoTRemainingFont
{
	uint32 CharactersPerPage = 0;
	TArray<FWoTRemainingFontPage> Pages;
};

bool WoTRemainingTextureExportDimensions(const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Texture, uint32& OutWidth, uint32& OutHeight)
{
	const FString* ClassName = Source.ObjectName(Texture.ClassReference);
	if (ClassName == nullptr || *ClassName != TEXT("Texture")) return false;
	FWoTRemainingReader Reader(Source.Bytes);
	if (!WoTRemainingOpenExport(Source, Texture, Reader)) return false;
	TArray<FWoTTypedProperty> Properties;
	if (!WoTRemainingReadProperties(Reader, Source, Properties, TEXT("font atlas")))
		return false;
	bool bSawPalette = false;
	for (const FWoTTypedProperty& Property : Properties)
	{
		if (Property.Name == TEXT("bParametric") && Property.Type == 3
			&& Property.Value.Num() == 1 && Property.Value[0] != 0) return false;
		if (Property.Name == TEXT("Format") && Property.Type == 1
			&& Property.Value.Num() == 1 && Property.Value[0] != 0) return false;
		if (Property.Name == TEXT("Palette") && Property.Type == 5
			&& (Property.ObjectClassIdentity == TEXT("Palette")
				|| Property.ObjectClassIdentity.EndsWith(
					TEXT(".Palette"), ESearchCase::CaseSensitive)))
			bSawPalette = true;
	}
	int32 MipCount = 0;
	if (!bSawPalette || !Reader.ReadCompact(MipCount) || MipCount <= 0 || MipCount > 32)
		return false;
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		if (Source.Version > 61 && !Reader.Skip(4)) return false;
		int32 DataCount = 0, Width = 0, Height = 0;
		uint8 UBits = 0, VBits = 0;
		if (!Reader.ReadCompact(DataCount) || DataCount < 0
			|| !Reader.Skip(static_cast<uint64>(DataCount))
			|| !Reader.ReadI32(Width) || !Reader.ReadI32(Height)
			|| !Reader.ReadU8(UBits) || !Reader.ReadU8(VBits)
			|| Width <= 0 || Height <= 0) return false;
		if (MipIndex == 0)
		{
			OutWidth = static_cast<uint32>(Width);
			OutHeight = static_cast<uint32>(Height);
		}
	}
	return Reader.Tell() == Reader.GetEnd();
}

bool WoTRemainingTextureDimensions(const FWoTRemainingSource& Source,
	int32 Reference, uint32& OutWidth, uint32& OutHeight)
{
	if (Reference <= 0 || !Source.Exports.IsValidIndex(Reference - 1)) return false;
	return WoTRemainingTextureExportDimensions(
		Source, Source.Exports[Reference - 1], OutWidth, OutHeight);
}

TWoTBootstrapResult<FWoTRemainingFont> WoTRemainingDecodeFont(
	const FWoTRemainingSource& Source, const FWoTRemainingExport& Export)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingFont>;
	FWoTRemainingReader Reader(Source.Bytes);
	TArray<FWoTTypedProperty> Properties;
	int32 PageCount = 0;
	if (!WoTRemainingOpenExport(Source, Export, Reader)
		|| !WoTRemainingReadProperties(Reader, Source, Properties, TEXT("font"))
		|| !Reader.ReadCompact(PageCount) || PageCount < 1
		|| PageCount > static_cast<int32>(WoTRemainingMaxFontPages))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the font page table is malformed"), Source.Path));
	}
	FWoTRemainingFont Font;
	Font.Pages.Reserve(PageCount);
	uint64 TotalGlyphs = 0;
	uint64 TotalAtlasPixels = 0;
	uint64 TotalGlyphPixels = 0;
	uint64 EstimatedBdfBytes = 4096;
	for (int32 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
	{
		int32 TextureReference = 0, GlyphCount = 0;
		if (!Reader.ReadCompact(TextureReference) || TextureReference <= 0
			|| !Reader.ReadCompact(GlyphCount) || GlyphCount < 0
			|| GlyphCount > static_cast<int32>(WoTRemainingMaxGlyphs)
			|| TotalGlyphs + static_cast<uint64>(GlyphCount) > WoTRemainingMaxGlyphs)
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the font page references or glyph counts are invalid"), Source.Path));
		}
		FWoTRemainingFontPage& Page = Font.Pages.AddDefaulted_GetRef();
		TWoTBootstrapResult<FString> Identity =
			WoTRemainingCanonicalIdentity(Source, TextureReference);
		if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
		Page.TextureIdentity = MoveTemp(Identity).GetValue();
		TWoTBootstrapResult<FString> TextureClass =
			WoTRemainingObjectClass(Source, TextureReference);
		if (TextureClass.IsError())
			return FResult::Error(MoveTemp(TextureClass).GetError());
		FWoTBootstrapStatus Closed = WoTRemainingRequireSemanticReference(
			Source, Page.TextureIdentity, TextureClass.GetValue(), TEXT("font page"));
		if (Closed.IsError()) return FResult::Error(MoveTemp(Closed).GetError());
		if (!WoTRemainingTextureDimensions(Source, TextureReference,
			Page.Width, Page.Height))
		{
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a font page does not bind a complete P8 atlas"), Source.Path));
		}
		uint64 PagePixels = 0;
		uint64 ExpandedBytes = 0;
		if (Page.Width > WoTRemainingMaxFontPageDimension
			|| Page.Height > WoTRemainingMaxFontPageDimension
			|| !WoTRemainingCheckedMultiply(Page.Width, Page.Height, PagePixels)
			|| !WoTRemainingCheckedAdd(
				TotalAtlasPixels, PagePixels, TotalAtlasPixels)
			|| !WoTRemainingCheckedMultiply(
				TotalAtlasPixels, sizeof(FColor), ExpandedBytes)
			|| TotalAtlasPixels > WoTRemainingMaxFontAtlasPixels
			|| ExpandedBytes > WoTRemainingMaxFontAtlasExpansionBytes)
		{
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::SizeLimit,
				TEXT("a font atlas exceeds its materialization budget"), Source.Path));
		}
		TBitArray<> OccupiedPixels;
		OccupiedPixels.Init(false, static_cast<int32>(PagePixels));
		TSet<FWoTRemainingGlyphAliasKey> PageAliases;
		PageAliases.Reserve(GlyphCount);
		Page.Glyphs.Reserve(GlyphCount);
		for (int32 LocalIndex = 0; LocalIndex < GlyphCount; ++LocalIndex)
		{
			int32 X = 0, Y = 0, Width = 0, Height = 0;
			if (!Reader.ReadI32(X) || !Reader.ReadI32(Y)
				|| !Reader.ReadI32(Width) || !Reader.ReadI32(Height)
				|| X < 0 || Y < 0 || Width < 0 || Height < 0
				|| static_cast<uint64>(X) + static_cast<uint64>(Width) > Page.Width
				|| static_cast<uint64>(Y) + static_cast<uint64>(Height) > Page.Height)
			{
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a font glyph extends outside its P8 atlas"), Source.Path));
			}
			uint64 GlyphPixels = 0;
			uint64 GlyphBdfBytes = 0;
			if (!WoTRemainingCheckedMultiply(
					static_cast<uint32>(Width), static_cast<uint32>(Height),
					GlyphPixels)
				|| !WoTRemainingCheckedAdd(
					TotalGlyphPixels, GlyphPixels, TotalGlyphPixels)
				|| !WoTRemainingCheckedMultiply(GlyphPixels, 2, GlyphBdfBytes)
				|| !WoTRemainingCheckedAdd(
					GlyphBdfBytes, static_cast<uint64>(Height) * 4, GlyphBdfBytes)
				|| !WoTRemainingCheckedAdd(GlyphBdfBytes, 512, GlyphBdfBytes)
				|| !WoTRemainingCheckedAdd(
					EstimatedBdfBytes, GlyphBdfBytes, EstimatedBdfBytes)
				|| TotalGlyphPixels > WoTRemainingMaxFontGlyphPixels
				|| EstimatedBdfBytes > WoTRemainingMaxFontBdfBytes)
			{
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::SizeLimit,
					TEXT("font glyph work exceeds its materialization budget"),
					Source.Path));
			}
			const FWoTRemainingGlyphAliasKey AliasKey{0,
				FIntRect(X, Y, X + Width, Y + Height), Width, 0, 0};
			const bool bExactAlias = PageAliases.Contains(AliasKey);
			for (int32 Row = Y; Row < Y + Height; ++Row)
			{
				const int32 RowStart = Row * static_cast<int32>(Page.Width);
				for (int32 Column = X; Column < X + Width; ++Column)
				{
					const int32 Pixel = RowStart + Column;
					if (OccupiedPixels[Pixel] && !bExactAlias)
					{
						return FResult::Error(WoTRemainingError(
							EWoTBootstrapError::CorruptInstallation,
							TEXT("font glyph rectangles overlap"), Source.Path));
					}
					if (!bExactAlias) OccupiedPixels[Pixel] = true;
				}
			}
			PageAliases.Add(AliasKey);
			FWoTRemainingFontGlyph& Glyph = Page.Glyphs.AddDefaulted_GetRef();
			Glyph.X = static_cast<uint32>(X);
			Glyph.Y = static_cast<uint32>(Y);
			Glyph.Width = static_cast<uint32>(Width);
			Glyph.Height = static_cast<uint32>(Height);
		}
		TotalGlyphs += static_cast<uint64>(GlyphCount);
	}
	int32 CharactersPerPage = 0;
	if (!Reader.ReadI32(CharactersPerPage)
		|| Reader.Tell() != Reader.GetEnd() || CharactersPerPage <= 0
		|| (CharactersPerPage & (CharactersPerPage - 1)) != 0
		|| CharactersPerPage > static_cast<int32>(WoTRemainingMaxGlyphs))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the font CharactersPerPage or payload tail is invalid"), Source.Path));
	}
	for (const FWoTRemainingFontPage& Page : Font.Pages)
		if (Page.Glyphs.Num() > CharactersPerPage)
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a font page exceeds CharactersPerPage"), Source.Path));
	Font.CharactersPerPage = static_cast<uint32>(CharactersPerPage);
	return FResult::Success(MoveTemp(Font));
}

FWoTBootstrapStatus WoTRemainingValidateFontMaterialization(
	const FWoTFontV2Record& Record, uint32* OutVisitedGlyphs)
{
	if (OutVisitedGlyphs != nullptr) *OutVisitedGlyphs = 0;
	if (Record.CharactersPerPage == 0
		|| Record.CharactersPerPage > WoTRemainingMaxGlyphs
		|| (Record.CharactersPerPage & (Record.CharactersPerPage - 1)) != 0
		|| Record.Pages.IsEmpty()
		|| Record.Pages.Num() > static_cast<int32>(WoTRemainingMaxFontPages)
		|| Record.Glyphs.IsEmpty()
		|| Record.Glyphs.Num() > static_cast<int32>(WoTRemainingMaxGlyphs)
		|| Record.CharacterMap.Num() != Record.Glyphs.Num()
		|| Record.Kerning.Num() > static_cast<int32>(WoTRemainingMaxKerningPairs)
		|| Record.LineHeight == 0
		|| Record.LineHeight > WoTRemainingMaxFontLineMetric
		|| Record.Baseline > Record.LineHeight)
	{
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::SizeLimit,
			TEXT("font materialization counts exceed their safe bounds")));
	}
	uint64 AtlasPixels = 0;
	uint64 ExpansionBytes = 0;
	uint64 DeclaredGlyphs = 0;
	for (int32 PageIndex = 0; PageIndex < Record.Pages.Num(); ++PageIndex)
	{
		const FWoTFontAtlasPageRecord& Page = Record.Pages[PageIndex];
		uint64 PagePixels = 0;
		if (Page.Ordinal != static_cast<uint32>(PageIndex) || Page.Width == 0
			|| Page.Height == 0
			|| Page.Width > WoTRemainingMaxFontPageDimension
			|| Page.Height > WoTRemainingMaxFontPageDimension
			|| Page.CharacterCount > Record.CharactersPerPage
			|| !WoTRemainingCheckedMultiply(Page.Width, Page.Height, PagePixels)
			|| !WoTRemainingCheckedAdd(AtlasPixels, PagePixels, AtlasPixels)
			|| !WoTRemainingCheckedMultiply(
				AtlasPixels, sizeof(FColor), ExpansionBytes)
			|| AtlasPixels > WoTRemainingMaxFontAtlasPixels
			|| ExpansionBytes > WoTRemainingMaxFontAtlasExpansionBytes
			|| !WoTRemainingCheckedAdd(
				DeclaredGlyphs, Page.CharacterCount, DeclaredGlyphs)
			|| DeclaredGlyphs > WoTRemainingMaxGlyphs)
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::SizeLimit,
				TEXT("font atlas expansion exceeds its safe bound")));
		}
	}
	if (DeclaredGlyphs != static_cast<uint64>(Record.Glyphs.Num()))
	{
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("font page glyph counts do not close")));
	}
	TArray<TBitArray<>> OccupiedPixels;
	OccupiedPixels.Reserve(Record.Pages.Num());
	for (const FWoTFontAtlasPageRecord& Page : Record.Pages)
	{
		TBitArray<>& Bits = OccupiedPixels.AddDefaulted_GetRef();
		Bits.Init(false, static_cast<int32>(
			static_cast<uint64>(Page.Width) * Page.Height));
	}
	uint64 GlyphPixels = 0;
	uint64 BdfBytes = 4096;
	TSet<FWoTRemainingGlyphAliasKey> GlyphAliases;
	GlyphAliases.Reserve(Record.Glyphs.Num());
	for (int32 GlyphIndex = 0; GlyphIndex < Record.Glyphs.Num(); ++GlyphIndex)
	{
		if (OutVisitedGlyphs != nullptr) ++*OutVisitedGlyphs;
		const FWoTFontGlyphRecord& Glyph = Record.Glyphs[GlyphIndex];
		if (!Record.CharacterMap.IsValidIndex(GlyphIndex)
			|| Record.CharacterMap[GlyphIndex].Glyph
				!= static_cast<uint32>(GlyphIndex)
			|| Record.CharacterMap[GlyphIndex].Codepoint != Glyph.Codepoint
			|| !WoTRemainingIsUnicodeScalar(Glyph.Codepoint)
			|| !Record.Pages.IsValidIndex(static_cast<int32>(Glyph.Page)))
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("font glyph map is malformed")));
		}
		const FWoTFontAtlasPageRecord& Page = Record.Pages[Glyph.Page];
		const FIntRect& Rect = Glyph.AtlasRect;
		if (Rect.Min.X < 0 || Rect.Min.Y < 0 || Rect.Max.X < Rect.Min.X
			|| Rect.Max.Y < Rect.Min.Y
			|| static_cast<uint32>(Rect.Max.X) > Page.Width
			|| static_cast<uint32>(Rect.Max.Y) > Page.Height)
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("font glyph rectangle is malformed")));
		}
		uint64 PixelCount = 0;
		uint64 GlyphBdfBytes = 0;
		if (!WoTRemainingCheckedMultiply(
				static_cast<uint32>(Rect.Width()),
				static_cast<uint32>(Rect.Height()), PixelCount)
			|| !WoTRemainingCheckedAdd(GlyphPixels, PixelCount, GlyphPixels)
			|| !WoTRemainingCheckedMultiply(PixelCount, 2, GlyphBdfBytes)
			|| !WoTRemainingCheckedAdd(
				GlyphBdfBytes, static_cast<uint64>(Rect.Height()) * 4, GlyphBdfBytes)
			|| !WoTRemainingCheckedAdd(GlyphBdfBytes, 512, GlyphBdfBytes)
			|| !WoTRemainingCheckedAdd(BdfBytes, GlyphBdfBytes, BdfBytes)
			|| GlyphPixels > WoTRemainingMaxFontGlyphPixels
			|| BdfBytes > WoTRemainingMaxFontBdfBytes)
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::SizeLimit,
				TEXT("font glyph work exceeds its safe bound")));
		}
		TBitArray<>& PageBits = OccupiedPixels[Glyph.Page];
		const FWoTRemainingGlyphAliasKey AliasKey{Glyph.Page, Glyph.AtlasRect,
			Glyph.Advance, Glyph.BearingX, Glyph.BearingY};
		const bool bExactAlias = GlyphAliases.Contains(AliasKey);
		for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
		{
			const int32 Row = Y * static_cast<int32>(Page.Width);
			for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
			{
				const int32 Pixel = Row + X;
				if (PageBits[Pixel] && !bExactAlias)
				{
					return FWoTBootstrapStatus::Error(WoTRemainingError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("font glyph rectangles overlap")));
				}
				if (!bExactAlias) PageBits[Pixel] = true;
			}
		}
		GlyphAliases.Add(AliasKey);
	}
	for (const FWoTFontKerningRecord& Pair : Record.Kerning)
	{
		if (!WoTRemainingIsUnicodeScalar(Pair.LeftCodepoint)
			|| !WoTRemainingIsUnicodeScalar(Pair.RightCodepoint))
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("font kerning codepoint is not a Unicode scalar")));
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildFontObject(
	const FWoTRequiredFact& Fact, const FWoTRetailContract& Contract,
	const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTRemainingFont> Decoded =
		WoTRemainingDecodeFont(Source, Export);
	if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
	FWoTRemainingFont& Font = Decoded.GetValue();
	TSet<FString> BoundLogicalIds;
	for (FWoTRemainingFontPage& Page : Font.Pages)
	{
		const FWoTRequiredFact* Match = nullptr;
		for (const FWoTRequiredFact& Candidate : Contract.Facts)
		{
			if (Candidate.Domain != EWoTFactDomain::Texture
				|| Candidate.SourcePath != Fact.SourcePath
				|| Candidate.SourceExport != Page.TextureIdentity)
				continue;
			if (Match != nullptr)
				return FResult::Error(WoTRemainingError(EWoTBootstrapError::DuplicateField,
					TEXT("a font atlas resolves to more than one texture fact"),
					Page.TextureIdentity));
			Match = &Candidate;
		}
		if (Match == nullptr || Match->Importer != TEXT("texture-v2")
			|| Match->Outputs.Num() != 1
			|| Match->Outputs[0].Schema != TEXT("wot-texture-v3")
			|| Match->Outputs[0].LogicalId.IsEmpty())
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
				TEXT("a font atlas lacks one closed authenticated texture binding"),
				Page.TextureIdentity));
		}
		if (BoundLogicalIds.Contains(Match->Outputs[0].LogicalId))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::DuplicateField,
				TEXT("two font atlas pages bind one texture logical id"),
				Match->Outputs[0].LogicalId));
		BoundLogicalIds.Add(Match->Outputs[0].LogicalId);
		Page.TextureLogicalId = Match->Outputs[0].LogicalId;
	}
	FWoTFontV2Record Record;
	Record.SourceExport = Fact.SourceExport;
	Record.CharactersPerPage = Font.CharactersPerPage;
	// UE1's inert stock LargeFont/BigFont placeholders contain only zero-sized
	// glyphs. Publish a one-pixel logical line instead of forbidden zero metrics.
	Record.LineHeight = 1;
	uint32 GlyphIndex = 0;
	for (int32 PageIndex = 0; PageIndex < Font.Pages.Num(); ++PageIndex)
	{
		const FWoTRemainingFontPage& Page = Font.Pages[PageIndex];
		FWoTFontAtlasPageRecord& Atlas = Record.Pages.AddDefaulted_GetRef();
		Atlas.Ordinal = static_cast<uint32>(PageIndex);
		Atlas.TextureLogicalId = Page.TextureLogicalId;
		Atlas.SourceIdentity = Page.TextureIdentity;
		Atlas.Width = Page.Width;
		Atlas.Height = Page.Height;
		Atlas.Format = TEXT("p8");
		Atlas.SourceSha256 = Source.Sha256;
		Atlas.CharacterCount = static_cast<uint32>(Page.Glyphs.Num());
		for (int32 LocalIndex = 0; LocalIndex < Page.Glyphs.Num(); ++LocalIndex)
		{
			const FWoTRemainingFontGlyph& Glyph = Page.Glyphs[LocalIndex];
			uint32 Codepoint = 0;
			FWoTBootstrapStatus Composed = WoTRemainingComposeFontCodepoint(
				static_cast<uint32>(PageIndex), Record.CharactersPerPage,
				static_cast<uint32>(LocalIndex), Codepoint);
			if (Composed.IsError())
				return FResult::Error(MoveTemp(Composed).GetError());
			Record.CharacterMap.Add(FWoTFontCharacterMapRecord{
				Codepoint, GlyphIndex});
			FWoTFontGlyphRecord& Encoded = Record.Glyphs.AddDefaulted_GetRef();
			Encoded.Codepoint = Codepoint;
			Encoded.Page = static_cast<uint32>(PageIndex);
			Encoded.AtlasRect = FIntRect(
				static_cast<int32>(Glyph.X), static_cast<int32>(Glyph.Y),
				static_cast<int32>(Glyph.X + Glyph.Width),
				static_cast<int32>(Glyph.Y + Glyph.Height));
			Encoded.Advance = static_cast<int32>(Glyph.Width);
			Record.LineHeight = FMath::Max(Record.LineHeight, Glyph.Height);
			++GlyphIndex;
		}
	}
	Record.Baseline = Record.LineHeight;
	FWoTBootstrapStatus Bounded = WoTRemainingValidateFontMaterialization(
		Record, nullptr);
	if (Bounded.IsError()) return FResult::Error(MoveTemp(Bounded).GetError());
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>("WOTFNT3"), 8);
	WoTRemainingAppendU16(Payload, 2);
	WoTRemainingAppendU16(Payload, 0);
	WoTRemainingAppendU32(Payload, Record.CharactersPerPage);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Record.Pages.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Record.CharacterMap.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Record.Glyphs.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Record.Kerning.Num()));
	WoTRemainingAppendU32(Payload, Record.Baseline);
	WoTRemainingAppendU32(Payload, Record.LineHeight);
	FWoTCanonicalJsonValue Pages = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTFontAtlasPageRecord& Page : Record.Pages)
	{
		WoTRemainingAppendU32(Payload, Page.Ordinal);
		if (!WoTRemainingAppendString(Payload, Page.TextureLogicalId))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a font atlas logical id exceeds its bound"), Source.Path));
		if (!WoTRemainingAppendString(Payload, Page.SourceIdentity))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a font atlas identity exceeds its bound"), Source.Path));
		WoTRemainingAppendU32(Payload, Page.Width);
		WoTRemainingAppendU32(Payload, Page.Height);
		if (!WoTRemainingAppendString(Payload, Page.Format))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a font atlas format exceeds its bound"), Source.Path));
		Payload.Append(Page.SourceSha256.GetData(), Page.SourceSha256.Num());
		WoTRemainingAppendU32(Payload, Page.CharacterCount);
	}
	for (const FWoTFontCharacterMapRecord& Map : Record.CharacterMap)
	{
		WoTRemainingAppendU32(Payload, Map.Codepoint);
		WoTRemainingAppendU32(Payload, Map.Glyph);
	}
	for (const FWoTFontGlyphRecord& Glyph : Record.Glyphs)
	{
		WoTRemainingAppendU32(Payload, Glyph.Codepoint);
		WoTRemainingAppendU32(Payload, Glyph.Page);
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.AtlasRect.Min.X));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.AtlasRect.Min.Y));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.AtlasRect.Width()));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.AtlasRect.Height()));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.Advance));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.BearingX));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Glyph.BearingY));
	}
	for (const FWoTFontKerningRecord& Pair : Record.Kerning)
	{
		WoTRemainingAppendU32(Payload, Pair.LeftCodepoint);
		WoTRemainingAppendU32(Payload, Pair.RightCodepoint);
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Pair.Adjustment));
	}
	for (const FWoTFontAtlasPageRecord& Page : Record.Pages)
	{
		FWoTCanonicalObjectBuilder PageBuilder;
		PageBuilder.Add(TEXT("character_count"),
			FWoTCanonicalJsonValue::MakeInteger(Page.CharacterCount));
		PageBuilder.Add(TEXT("format"), FWoTCanonicalJsonValue::MakeString(Page.Format));
		PageBuilder.Add(TEXT("height"), FWoTCanonicalJsonValue::MakeInteger(Page.Height));
		PageBuilder.Add(TEXT("logical_id"),
			FWoTCanonicalJsonValue::MakeString(Page.TextureLogicalId));
		PageBuilder.Add(TEXT("ordinal"),
			FWoTCanonicalJsonValue::MakeInteger(Page.Ordinal));
		PageBuilder.Add(TEXT("source_identity"),
			FWoTCanonicalJsonValue::MakeString(Page.SourceIdentity));
		PageBuilder.Add(TEXT("source_sha256"),
			FWoTCanonicalJsonValue::MakeString(Page.SourceSha256.ToHex()));
		PageBuilder.Add(TEXT("width"), FWoTCanonicalJsonValue::MakeInteger(Page.Width));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> PageJson =
			MoveTemp(PageBuilder).Build(TEXT("font page"));
		if (PageJson.IsError()) return FResult::Error(MoveTemp(PageJson).GetError());
		Pages.AddElement(MoveTemp(PageJson).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTRemainingSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("baseline"), FWoTCanonicalJsonValue::MakeInteger(Record.Baseline));
	Builder.Add(TEXT("character_map_count"),
		FWoTCanonicalJsonValue::MakeInteger(Record.CharacterMap.Num()));
	Builder.Add(TEXT("characters_per_page"),
		FWoTCanonicalJsonValue::MakeInteger(Record.CharactersPerPage));
	Builder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-font-pages-v2")));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("glyph_count"),
		FWoTCanonicalJsonValue::MakeInteger(Record.Glyphs.Num()));
	Builder.Add(TEXT("importer"), FWoTCanonicalJsonValue::MakeString(TEXT("font-v2")));
	Builder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(2));
	Builder.Add(TEXT("kerning_count"),
		FWoTCanonicalJsonValue::MakeInteger(Record.Kerning.Num()));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("font")));
	Builder.Add(TEXT("line_height"),
		FWoTCanonicalJsonValue::MakeInteger(Record.LineHeight));
	Builder.Add(TEXT("page_count"),
		FWoTCanonicalJsonValue::MakeInteger(Record.Pages.Num()));
	Builder.Add(TEXT("pages"), MoveTemp(Pages));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("font object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTRemainingFrame(Header.GetValue(), Payload);
}

struct FWoTRemainingProceduralMip
{
	uint32 Width = 0;
	uint32 Height = 0;
	TArray<uint8> Data;
};

struct FWoTRemainingProcedural
{
	FString ClassName;
	TArray<FWoTTypedProperty> Properties;
	TArray<FWoTRemainingProceduralMip> Mips;
	TArray<TArray<uint8>> States;
};

bool WoTRemainingPowerOfTwo(int32 Value)
{
	return Value > 0 && (Value & (Value - 1)) == 0;
}

TWoTBootstrapResult<FWoTRemainingProcedural> WoTRemainingDecodeProcedural(
	const FWoTRemainingSource& Source, const FWoTRemainingExport& Export)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingProcedural>;
	const FString* ClassName = Source.ObjectName(Export.ClassReference);
	if (ClassName == nullptr
		|| (*ClassName != TEXT("FireTexture") && *ClassName != TEXT("WaterTexture")
			&& *ClassName != TEXT("WaveTexture") && *ClassName != TEXT("WetTexture")
			&& *ClassName != TEXT("IceTexture")))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the procedural texture class is unsupported"), Source.Path));
	}
	FWoTRemainingReader Reader(Source.Bytes);
	FWoTRemainingProcedural Decoded;
	Decoded.ClassName = *ClassName;
	int32 MipCount = 0;
	if (!WoTRemainingOpenExport(Source, Export, Reader)
		|| !WoTRemainingReadProperties(
			Reader, Source, Decoded.Properties, TEXT("procedural texture"))
		|| !Reader.ReadCompact(MipCount) || MipCount <= 0 || MipCount > 32)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the procedural texture properties or mip table are malformed"), Source.Path));
	}
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		FWoTRemainingReader Candidate = Reader;
		int32 DataCount = 0, Width = 0, Height = 0;
		uint8 UBits = 0, VBits = 0;
		bool bUnprefixed = Candidate.ReadCompact(DataCount) && DataCount >= 0
			&& Candidate.Skip(static_cast<uint64>(DataCount))
			&& Candidate.ReadI32(Width) && Candidate.ReadI32(Height)
			&& Candidate.ReadU8(UBits) && Candidate.ReadU8(VBits)
			&& UBits < 31 && VBits < 31
			&& Width == (1 << UBits) && Height == (1 << VBits)
			&& (DataCount == 0
				|| static_cast<uint64>(DataCount)
					== static_cast<uint64>(Width) * static_cast<uint64>(Height));
		if (!bUnprefixed)
		{
			Candidate = Reader;
			if (!Candidate.Skip(4) || !Candidate.ReadCompact(DataCount) || DataCount < 0
				|| !Candidate.Skip(static_cast<uint64>(DataCount))
				|| !Candidate.ReadI32(Width) || !Candidate.ReadI32(Height)
				|| !Candidate.ReadU8(UBits) || !Candidate.ReadU8(VBits))
			{
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a procedural mip is truncated"), Source.Path));
			}
		}
		const uint64 PixelCount = Width > 0 && Height > 0
			? static_cast<uint64>(Width) * static_cast<uint64>(Height) : 0;
		if (!WoTRemainingPowerOfTwo(Width) || !WoTRemainingPowerOfTwo(Height)
			|| PixelCount > WoTRemainingMaxSourceBytes
			|| (DataCount != 0 && static_cast<uint64>(DataCount) != PixelCount))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a procedural mip has invalid power-of-two dimensions"), Source.Path));
		}
		FWoTRemainingProceduralMip& Mip = Decoded.Mips.AddDefaulted_GetRef();
		Mip.Width = static_cast<uint32>(Width);
		Mip.Height = static_cast<uint32>(Height);
		FWoTRemainingReader DataReader = Reader;
		if (!bUnprefixed && !DataReader.Skip(4)) return FResult::Error(
			WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a procedural mip prefix is truncated"), Source.Path));
		int32 VerifiedCount = 0;
		if (!DataReader.ReadCompact(VerifiedCount) || VerifiedCount != DataCount
			|| !DataReader.ReadBytes(static_cast<uint64>(DataCount), Mip.Data))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a procedural mip byte vector is truncated"), Source.Path));
		}
		if (DataCount == 0) Mip.Data.AddZeroed(static_cast<int32>(PixelCount));
		Reader = Candidate;
	}
	const bool bHasStates = Decoded.ClassName == TEXT("FireTexture")
		|| Decoded.ClassName == TEXT("WaterTexture")
		|| Decoded.ClassName == TEXT("WetTexture")
		|| Decoded.ClassName == TEXT("WaveTexture");
	if (bHasStates && Reader.Remaining() > 0)
	{
		int32 StateCount = 0;
		if (!Reader.ReadCompact(StateCount) || StateCount < 0
			|| StateCount > static_cast<int32>(WoTRemainingMaxStates))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the procedural state count is invalid"), Source.Path));
		}
		for (int32 StateIndex = 0; StateIndex < StateCount; ++StateIndex)
		{
			TArray<uint8>& State = Decoded.States.AddDefaulted_GetRef();
			if (!Reader.ReadBytes(8, State))
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the procedural state vector is truncated"), Source.Path));
		}
	}
	return FResult::Success(MoveTemp(Decoded));
}

// ---------------------------------------------------------------------------
// Procedural v2 (ue1-fractal-inputs-v2): the v1 body (typed properties, mips,
// serialized spark states) is followed by the object's own 256-entry palette
// bytes, the resolved simulation parameters the runtime fractal port consumes
// (Tools/convert/bake_flipbooks.py _runtime_base), the authored ADrop table,
// and the SourceTexture/GlassTexture identities with their mip-0 dimensions.
// Foreign (import) references resolve only through packages the contract
// names. Byte layout is mirrored by Tools/dist/build_payload_cache.py.
// ---------------------------------------------------------------------------

constexpr uint16 WoTRemainingProceduralPayloadVersion = 2;
constexpr uint32 WoTRemainingProceduralMaxDrops = 256;

struct FWoTRemainingResolvedObject
{
	const FWoTRemainingSource* Source = nullptr;
	const FWoTRemainingExport* Export = nullptr;
	FString ClassName;
};

/** Resolves a typed-property object reference (local export or foreign import)
    to exactly one export of an allowed class. Foreign raw paths must be
    injective within the target package for the allowed classes. */
TWoTBootstrapResult<FWoTRemainingResolvedObject> WoTRemainingResolveObjectReference(
	const FWoTRemainingSource& Current, int32 Reference,
	FWoTRemainingPackageResolver& Resolver,
	TConstArrayView<const TCHAR*> AllowedClasses, FStringView Context)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingResolvedObject>;
	auto ClassAllowed = [&AllowedClasses](const FString* ClassName)
	{
		if (ClassName == nullptr) return false;
		for (const TCHAR* Allowed : AllowedClasses)
			if (ClassName->Equals(Allowed, ESearchCase::CaseSensitive)) return true;
		return false;
	};
	if (Reference == 0)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a required procedural object reference is null"), FString(Context)));
	}
	TWoTBootstrapResult<FString> RawPath = WoTRemainingObjectPath(Current, Reference);
	if (RawPath.IsError()) return FResult::Error(MoveTemp(RawPath).GetError());
	FWoTRemainingResolvedObject Resolved;
	if (Reference > 0)
	{
		Resolved.Source = &Current;
		Resolved.Export = &Current.Exports[Reference - 1];
	}
	else
	{
		int32 Dot = INDEX_NONE;
		const FString& Path = RawPath.GetValue();
		if (!Path.FindChar(TEXT('.'), Dot) || Dot <= 0)
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a foreign procedural reference has no package component"),
				FString(Context)));
		}
		TWoTBootstrapResult<const FWoTRemainingSource*> Foreign =
			Resolver.Resolve(Path.Left(Dot), Current);
		if (Foreign.IsError()) return FResult::Error(MoveTemp(Foreign).GetError());
		const FWoTRemainingSource& Target = *Foreign.GetValue();
		int32 Matches = 0;
		for (int32 ExportIndex = 0; ExportIndex < Target.Exports.Num(); ++ExportIndex)
		{
			const FWoTRemainingExport& Candidate = Target.Exports[ExportIndex];
			if (!ClassAllowed(Target.ObjectName(Candidate.ClassReference))) continue;
			TWoTBootstrapResult<FString> CandidatePath =
				WoTRemainingObjectPath(Target, ExportIndex + 1);
			if (CandidatePath.IsError())
				return FResult::Error(MoveTemp(CandidatePath).GetError());
			if (!CandidatePath.GetValue().Equals(Path, ESearchCase::IgnoreCase)) continue;
			++Matches;
			Resolved.Source = &Target;
			Resolved.Export = &Candidate;
		}
		if (Matches != 1)
		{
			return FResult::Error(WoTRemainingError(
				Matches == 0 ? EWoTBootstrapError::UnknownField
					: EWoTBootstrapError::DuplicateField,
				Matches == 0
					? TEXT("a foreign procedural reference resolves to no export of its class")
					: TEXT("a foreign procedural reference is ambiguous in its package"),
				FString(Context)));
		}
	}
	const FString* ClassName = Resolved.Source->ObjectName(Resolved.Export->ClassReference);
	if (!ClassAllowed(ClassName))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("a procedural reference resolves to an unexpected class"),
			FString(Context)));
	}
	Resolved.ClassName = *ClassName;
	return FResult::Success(MoveTemp(Resolved));
}

bool WoTRemainingDecodePalette(const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export, TArray<uint8>& OutRGBA)
{
	FWoTRemainingReader Reader(Source.Bytes);
	TArray<FWoTTypedProperty> Properties;
	int32 Count = 0;
	OutRGBA.Reset();
	return WoTRemainingOpenExport(Source, Export, Reader)
		&& WoTRemainingReadProperties(Reader, Source, Properties, TEXT("palette"))
		&& Reader.ReadCompact(Count) && Count == 256
		&& Reader.ReadBytes(1024, OutRGBA) && OutRGBA.Num() == 1024
		&& Reader.Tell() == Reader.GetEnd();
}

struct FWoTRemainingProceduralParameters
{
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	int32 PrimeCount = 0;
	uint8 WaveAmp = 0;
	uint8 BumpMapLight = 0;
	uint8 BumpMapAngle = 0;
	uint8 PhongRange = 0;
	uint8 PhongSize = 0;
	uint8 RenderHeat = 0;
	bool bRising = false;
	int32 SparksLimit = 0;
	uint8 PanningStyle = 0;
	uint8 TimeMethod = 0;
	uint8 HorizPanSpeed = 0;
	uint8 VertPanSpeed = 0;
	uint8 Frequency = 0;
	uint8 Amplitude = 0;
	bool bMoveIce = false;
	float MasterCount = 0.0f;
	float UDisplace = 0.0f;
	float VDisplace = 0.0f;
	float UPosition = 0.0f;
	float VPosition = 0.0f;
	bool bMasked = false;
	int32 NumDrops = 0;
	TArray<TArray<uint8>> Drops;
};

const FWoTTypedProperty* WoTRemainingFindProperty(
	const TArray<FWoTTypedProperty>& Properties, const TCHAR* Name)
{
	for (const FWoTTypedProperty& Property : Properties)
		if (Property.ArrayIndex == 0
			&& Property.Name.Equals(Name, ESearchCase::IgnoreCase)) return &Property;
	return nullptr;
}

/** Absent properties are engine zero-init (bake_flipbooks props.get(X, 0));
    a present property of the wrong UE1 type fails closed. */
bool WoTRemainingReadByteProperty(const TArray<FWoTTypedProperty>& Properties,
	const TCHAR* Name, uint8& Out)
{
	const FWoTTypedProperty* Property = WoTRemainingFindProperty(Properties, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 1 || Property->Value.Num() != 1) return false;
	Out = Property->Value[0];
	return true;
}

bool WoTRemainingReadBoolProperty(const TArray<FWoTTypedProperty>& Properties,
	const TCHAR* Name, bool& Out)
{
	const FWoTTypedProperty* Property = WoTRemainingFindProperty(Properties, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 3 || Property->Value.Num() != 1) return false;
	Out = Property->Value[0] != 0;
	return true;
}

bool WoTRemainingReadIntProperty(const TArray<FWoTTypedProperty>& Properties,
	const TCHAR* Name, int32& Out)
{
	const FWoTTypedProperty* Property = WoTRemainingFindProperty(Properties, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 2 || Property->Value.Num() != 4) return false;
	Out = static_cast<int32>(static_cast<uint32>(Property->Value[0])
		| (static_cast<uint32>(Property->Value[1]) << 8)
		| (static_cast<uint32>(Property->Value[2]) << 16)
		| (static_cast<uint32>(Property->Value[3]) << 24));
	return true;
}

bool WoTRemainingReadFloatProperty(const TArray<FWoTTypedProperty>& Properties,
	const TCHAR* Name, float& Out)
{
	const FWoTTypedProperty* Property = WoTRemainingFindProperty(Properties, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 4 || Property->Value.Num() != 4) return false;
	const uint32 Bits = static_cast<uint32>(Property->Value[0])
		| (static_cast<uint32>(Property->Value[1]) << 8)
		| (static_cast<uint32>(Property->Value[2]) << 16)
		| (static_cast<uint32>(Property->Value[3]) << 24);
	float Value = 0.0f;
	FMemory::Memcpy(&Value, &Bits, sizeof(float));
	if (!FMath::IsFinite(Value)) return false;
	Out = Value;
	return true;
}

bool WoTRemainingReadProceduralParameters(const FString& ClassName,
	const TArray<FWoTTypedProperty>& Properties,
	FWoTRemainingProceduralParameters& Out)
{
	bool bMasked = false;
	uint8 PrimeCount = 0;
	if (!WoTRemainingReadFloatProperty(Properties, TEXT("MinFrameRate"), Out.MinFrameRate)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("MaxFrameRate"), Out.MaxFrameRate)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("PrimeCount"), PrimeCount)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("WaveAmp"), Out.WaveAmp)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("BumpMapLight"), Out.BumpMapLight)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("BumpMapAngle"), Out.BumpMapAngle)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("PhongRange"), Out.PhongRange)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("PhongSize"), Out.PhongSize)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("RenderHeat"), Out.RenderHeat)
		|| !WoTRemainingReadBoolProperty(Properties, TEXT("bRising"), Out.bRising)
		|| !WoTRemainingReadIntProperty(Properties, TEXT("SparksLimit"), Out.SparksLimit)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("PanningStyle"), Out.PanningStyle)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("TimeMethod"), Out.TimeMethod)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("HorizPanSpeed"), Out.HorizPanSpeed)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("VertPanSpeed"), Out.VertPanSpeed)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("Frequency"), Out.Frequency)
		|| !WoTRemainingReadByteProperty(Properties, TEXT("Amplitude"), Out.Amplitude)
		|| !WoTRemainingReadBoolProperty(Properties, TEXT("MoveIce"), Out.bMoveIce)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("MasterCount"), Out.MasterCount)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("UDisplace"), Out.UDisplace)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("VDisplace"), Out.VDisplace)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("UPosition"), Out.UPosition)
		|| !WoTRemainingReadFloatProperty(Properties, TEXT("VPosition"), Out.VPosition)
		|| !WoTRemainingReadBoolProperty(Properties, TEXT("bMasked"), bMasked)
		|| !WoTRemainingReadIntProperty(Properties, TEXT("NumDrops"), Out.NumDrops))
	{
		return false;
	}
	// UE1 Texture: PrimeCount is a byte; the record widens it to int32.
	Out.PrimeCount = PrimeCount;
	// bake_flipbooks masks only the IceTexture palette slot zero.
	Out.bMasked = bMasked && ClassName == TEXT("IceTexture");
	// Authored ADrop table: the static Drops[256] array serializes only its
	// non-zero elements as indexed struct tags; the first NumDrops in index
	// order are live (_drop_bytes).
	const int32 LiveDrops = FMath::Clamp(Out.NumDrops, 0,
		static_cast<int32>(WoTRemainingProceduralMaxDrops));
	uint32 PreviousIndex = 0;
	bool bFirst = true;
	for (const FWoTTypedProperty& Property : Properties)
	{
		if (!Property.Name.Equals(TEXT("Drops"), ESearchCase::IgnoreCase)) continue;
		if (Property.Type != 10 || Property.Value.Num() != 8
			|| !Property.StructName.Equals(TEXT("ADrop"), ESearchCase::IgnoreCase)
			|| (!bFirst && Property.ArrayIndex <= PreviousIndex)
			|| Property.ArrayIndex >= WoTRemainingProceduralMaxDrops)
		{
			return false;
		}
		bFirst = false;
		PreviousIndex = Property.ArrayIndex;
		if (Out.Drops.Num() < LiveDrops) Out.Drops.Add(Property.Value);
	}
	return true;
}

/** One SourceTexture/GlassTexture input: identity, class, and mip-0 size. */
struct FWoTRemainingProceduralInput
{
	uint8 Kind = 0; // 0 none, 1 plain texture, 2 procedural texture
	FString Identity;
	FString ClassName;
	uint32 Width = 0;
	uint32 Height = 0;
};

TWoTBootstrapResult<FWoTRemainingProceduralInput> WoTRemainingResolveProceduralInput(
	const FWoTRemainingSource& Source, const TArray<FWoTTypedProperty>& Properties,
	const TCHAR* PropertyName, FWoTRemainingPackageResolver& Resolver,
	FStringView Context)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingProceduralInput>;
	FWoTRemainingProceduralInput Input;
	const FWoTTypedProperty* Property = WoTRemainingFindProperty(Properties, PropertyName);
	if (Property == nullptr) return FResult::Success(MoveTemp(Input));
	if (Property->Type != 5)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a procedural input property is not an object reference"), FString(Context)));
	}
	if (!Property->ObjectReference.IsSet() || Property->ObjectReference.GetValue() == 0)
		return FResult::Success(MoveTemp(Input));
	static const TCHAR* Allowed[] = {TEXT("Texture"), TEXT("FireTexture"),
		TEXT("WaterTexture"), TEXT("WaveTexture"), TEXT("WetTexture"),
		TEXT("IceTexture")};
	TWoTBootstrapResult<FWoTRemainingResolvedObject> Resolved =
		WoTRemainingResolveObjectReference(Source, Property->ObjectReference.GetValue(),
			Resolver, MakeArrayView(Allowed), Context);
	if (Resolved.IsError()) return FResult::Error(MoveTemp(Resolved).GetError());
	Input.Identity = Property->ObjectIdentity;
	Input.ClassName = Resolved.GetValue().ClassName;
	if (Input.ClassName == TEXT("Texture"))
	{
		Input.Kind = 1;
		if (!WoTRemainingTextureExportDimensions(*Resolved.GetValue().Source,
			*Resolved.GetValue().Export, Input.Width, Input.Height))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a procedural source texture is malformed"), FString(Context)));
		}
	}
	else
	{
		Input.Kind = 2;
		TWoTBootstrapResult<FWoTRemainingProcedural> Decoded = WoTRemainingDecodeProcedural(
			*Resolved.GetValue().Source, *Resolved.GetValue().Export);
		if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
		if (Decoded.GetValue().Mips.IsEmpty())
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a procedural source fractal has no mip"), FString(Context)));
		}
		Input.Width = Decoded.GetValue().Mips[0].Width;
		Input.Height = Decoded.GetValue().Mips[0].Height;
	}
	if (Input.Identity.IsEmpty())
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a procedural input lacks its canonical identity"), FString(Context)));
	}
	return FResult::Success(MoveTemp(Input));
}

void WoTRemainingAppendFloat(TArray<uint8>& Bytes, float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(float));
	WoTRemainingAppendU32(Bytes, Bits);
}

bool WoTRemainingAppendProceduralInput(TArray<uint8>& Payload,
	const FWoTRemainingProceduralInput& Input)
{
	Payload.Add(Input.Kind);
	if (!WoTRemainingAppendString(Payload, Input.Identity)) return false;
	WoTRemainingAppendU32(Payload, Input.Width);
	WoTRemainingAppendU32(Payload, Input.Height);
	return true;
}

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildProceduralObject(
	const FWoTRequiredFact& Fact, const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export, FWoTRemainingPackageResolver& Resolver)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTRemainingProcedural> DecodedResult =
		WoTRemainingDecodeProcedural(Source, Export);
	if (DecodedResult.IsError())
		return FResult::Error(MoveTemp(DecodedResult).GetError());
	const FWoTRemainingProcedural& Decoded = DecodedResult.GetValue();
	const FString& Context = Fact.SourceExport;

	// The object's own Palette (bake_flipbooks: every shipped Wet/Ice palette
	// is the same object as its source's; Fire/Wave carry their own).
	const FWoTTypedProperty* PaletteProperty =
		WoTRemainingFindProperty(Decoded.Properties, TEXT("Palette"));
	if (PaletteProperty == nullptr || PaletteProperty->Type != 5
		|| !PaletteProperty->ObjectReference.IsSet()
		|| PaletteProperty->ObjectReference.GetValue() == 0
		|| PaletteProperty->ObjectIdentity.IsEmpty())
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a procedural texture has no palette reference"), Context));
	}
	static const TCHAR* PaletteClasses[] = {TEXT("Palette")};
	TWoTBootstrapResult<FWoTRemainingResolvedObject> Palette =
		WoTRemainingResolveObjectReference(Source,
			PaletteProperty->ObjectReference.GetValue(), Resolver,
			MakeArrayView(PaletteClasses), Context);
	if (Palette.IsError()) return FResult::Error(MoveTemp(Palette).GetError());
	TArray<uint8> PaletteRGBA;
	if (!WoTRemainingDecodePalette(*Palette.GetValue().Source,
		*Palette.GetValue().Export, PaletteRGBA))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a procedural palette is malformed"), Context));
	}
	FWoTRemainingProceduralParameters Parameters;
	if (!WoTRemainingReadProceduralParameters(
		Decoded.ClassName, Decoded.Properties, Parameters))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a procedural simulation parameter is malformed"), Context));
	}
	TWoTBootstrapResult<FWoTRemainingProceduralInput> SourceInput =
		WoTRemainingResolveProceduralInput(Source, Decoded.Properties,
			TEXT("SourceTexture"), Resolver, Context);
	if (SourceInput.IsError()) return FResult::Error(MoveTemp(SourceInput).GetError());
	TWoTBootstrapResult<FWoTRemainingProceduralInput> GlassInput =
		WoTRemainingResolveProceduralInput(Source, Decoded.Properties,
			TEXT("GlassTexture"), Resolver, Context);
	if (GlassInput.IsError()) return FResult::Error(MoveTemp(GlassInput).GetError());

	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>("WOTPRC3"), 8);
	WoTRemainingAppendU16(Payload, WoTRemainingProceduralPayloadVersion);
	WoTRemainingAppendU16(Payload, 0);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Decoded.Mips.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Decoded.States.Num()));
	WoTRemainingAppendU32(Payload, WoTRemainingRandomSeed);
	if (!WoTRemainingAppendProperties(Payload, Decoded.Properties))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("a procedural property exceeds its string bound"), Source.Path));
	FWoTCanonicalJsonValue Dimensions = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTRemainingProceduralMip& Mip : Decoded.Mips)
	{
		WoTRemainingAppendU32(Payload, Mip.Width);
		WoTRemainingAppendU32(Payload, Mip.Height);
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Mip.Data.Num()));
		Payload.Append(Mip.Data);
		FWoTCanonicalObjectBuilder DimensionBuilder;
		DimensionBuilder.Add(TEXT("height"),
			FWoTCanonicalJsonValue::MakeInteger(Mip.Height));
		DimensionBuilder.Add(TEXT("width"),
			FWoTCanonicalJsonValue::MakeInteger(Mip.Width));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Dimension =
			MoveTemp(DimensionBuilder).Build(TEXT("procedural mip dimension"));
		if (Dimension.IsError()) return FResult::Error(MoveTemp(Dimension).GetError());
		Dimensions.AddElement(MoveTemp(Dimension).GetValue());
	}
	for (const TArray<uint8>& State : Decoded.States) Payload.Append(State);
	// ---- v2 tail ----
	Payload.Append(PaletteRGBA);
	Payload.Add(static_cast<uint8>(Parameters.bMasked ? 1 : 0));
	Payload.Add(static_cast<uint8>(Parameters.bRising ? 1 : 0));
	Payload.Add(static_cast<uint8>(Parameters.bMoveIce ? 1 : 0));
	Payload.Add(static_cast<uint8>(0));
	WoTRemainingAppendFloat(Payload, Parameters.MinFrameRate);
	WoTRemainingAppendFloat(Payload, Parameters.MaxFrameRate);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Parameters.PrimeCount));
	Payload.Add(Parameters.WaveAmp);
	Payload.Add(Parameters.BumpMapLight);
	Payload.Add(Parameters.BumpMapAngle);
	Payload.Add(Parameters.PhongRange);
	Payload.Add(Parameters.PhongSize);
	Payload.Add(Parameters.RenderHeat);
	Payload.Add(Parameters.PanningStyle);
	Payload.Add(Parameters.TimeMethod);
	Payload.Add(Parameters.HorizPanSpeed);
	Payload.Add(Parameters.VertPanSpeed);
	Payload.Add(Parameters.Frequency);
	Payload.Add(Parameters.Amplitude);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Parameters.SparksLimit));
	WoTRemainingAppendFloat(Payload, Parameters.MasterCount);
	WoTRemainingAppendFloat(Payload, Parameters.UDisplace);
	WoTRemainingAppendFloat(Payload, Parameters.VDisplace);
	WoTRemainingAppendFloat(Payload, Parameters.UPosition);
	WoTRemainingAppendFloat(Payload, Parameters.VPosition);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Parameters.Drops.Num()));
	for (const TArray<uint8>& Drop : Parameters.Drops) Payload.Append(Drop);
	if (!WoTRemainingAppendProceduralInput(Payload, SourceInput.GetValue())
		|| !WoTRemainingAppendProceduralInput(Payload, GlassInput.GetValue()))
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("a procedural input identity exceeds its string bound"), Source.Path));
	}

	TArray<FString> References;
	for (const FWoTTypedProperty& Property : Decoded.Properties)
		if (!Property.ObjectIdentity.IsEmpty()) References.AddUnique(Property.ObjectIdentity);
	References.Sort([](const FString& Left, const FString& Right)
	{
		const FString LF = WoTRemainingFold(Left), RF = WoTRemainingFold(Right);
		return LF == RF ? Left < Right : LF < RF;
	});
	FWoTCanonicalJsonValue ReferenceJson = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& Reference : References)
		ReferenceJson.AddElement(FWoTCanonicalJsonValue::MakeString(Reference));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTRemainingSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FString StateKind = TEXT("none");
	if (Decoded.ClassName == TEXT("FireTexture")) StateKind = TEXT("sparks");
	else if (Decoded.ClassName == TEXT("WaterTexture")
		|| Decoded.ClassName == TEXT("WetTexture")
		|| Decoded.ClassName == TEXT("WaveTexture")) StateKind = TEXT("drops");
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("class_name"),
		FWoTCanonicalJsonValue::MakeString(Decoded.ClassName));
	Builder.Add(TEXT("dimensions"), MoveTemp(Dimensions));
	Builder.Add(TEXT("drop_count"),
		FWoTCanonicalJsonValue::MakeInteger(Parameters.Drops.Num()));
	Builder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-fractal-inputs-v2")));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("glass_class"),
		FWoTCanonicalJsonValue::MakeString(GlassInput.GetValue().ClassName));
	Builder.Add(TEXT("glass_texture"),
		FWoTCanonicalJsonValue::MakeString(GlassInput.GetValue().Identity));
	Builder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("procedural-v1")));
	Builder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(2));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("procedural")));
	Builder.Add(TEXT("masked"),
		FWoTCanonicalJsonValue::MakeInteger(Parameters.bMasked ? 1 : 0));
	Builder.Add(TEXT("mip_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Mips.Num()));
	Builder.Add(TEXT("palette_identity"),
		FWoTCanonicalJsonValue::MakeString(PaletteProperty->ObjectIdentity));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	Builder.Add(TEXT("property_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Properties.Num()));
	Builder.Add(TEXT("random_scope"), FWoTCanonicalJsonValue::MakeString(
		TEXT("process-global-canonical-order")));
	Builder.Add(TEXT("random_seed"),
		FWoTCanonicalJsonValue::MakeInteger(WoTRemainingRandomSeed));
	Builder.Add(TEXT("references"), MoveTemp(ReferenceJson));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	Builder.Add(TEXT("source_class"),
		FWoTCanonicalJsonValue::MakeString(SourceInput.GetValue().ClassName));
	Builder.Add(TEXT("source_texture"),
		FWoTCanonicalJsonValue::MakeString(SourceInput.GetValue().Identity));
	Builder.Add(TEXT("state_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.States.Num()));
	Builder.Add(TEXT("state_kind"), FWoTCanonicalJsonValue::MakeString(StateKind));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("procedural object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTRemainingFrame(Header.GetValue(), Payload);
}

struct FWoTRemainingClassRecord
{
	int32 ExportIndex = 0;
	const FWoTRemainingExport* Export = nullptr;
	FString Identity;
	bool bPlaceholder = false;
};

TWoTBootstrapResult<TArray<FWoTRemainingClassRecord>> WoTRemainingClasses(
	const FWoTRemainingSource& Source)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTRemainingClassRecord>>;
	TArray<FWoTRemainingClassRecord> Classes;
	TSet<FString> Seen;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const FWoTRemainingExport& Export = Source.Exports[ExportIndex];
		if (Export.ClassReference != 0) continue;
		FWoTRemainingClassRecord Record;
		Record.ExportIndex = ExportIndex + 1;
		Record.Export = &Export;
		Record.bPlaceholder = Export.Size == 0 && Export.Name == TEXT("None");
		TWoTBootstrapResult<FString> Identity =
			WoTRemainingCanonicalIdentity(Source, Record.ExportIndex);
		if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
		Record.Identity = MoveTemp(Identity).GetValue();
		FWoTBootstrapStatus Closed = WoTRemainingRequireSemanticReference(
			Source, Record.Identity, TEXT("Core.Class"), TEXT("class row"));
		if (Closed.IsError()) return FResult::Error(MoveTemp(Closed).GetError());
		const FString Folded = WoTRemainingFold(Record.Identity);
		if (Seen.Contains(Folded))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::DuplicateField,
				TEXT("duplicate class identifier after case folding"), Record.Identity));
		}
		Seen.Add(Folded);
		Classes.Add(MoveTemp(Record));
	}
	Classes.Sort([](const FWoTRemainingClassRecord& Left,
		const FWoTRemainingClassRecord& Right)
	{
		const FString LF = WoTRemainingFold(Left.Identity);
		const FString RF = WoTRemainingFold(Right.Identity);
		return LF == RF ? Left.Identity < Right.Identity : LF < RF;
	});
	return FResult::Success(MoveTemp(Classes));
}

bool WoTRemainingSkipScript(FWoTRemainingReader& Reader, int32 MemorySize)
{
	if (MemorySize < 0) return false;
	int64 MemoryOffset = 0;
	uint32 RecursionDepth = 0;
	auto Raw = [&Reader, &MemoryOffset](uint32 Size)
	{
		if (!Reader.Skip(Size)) return false;
		MemoryOffset += Size;
		return true;
	};
	auto Reference = [&Reader, &MemoryOffset](int32* Out)
	{
		int32 Value = 0;
		if (!Reader.ReadCompact(Value)) return false;
		MemoryOffset += 4;
		if (Out != nullptr) *Out = Value;
		return true;
	};
	TFunction<bool(uint8&)> Expression;
	TFunction<bool()> Parameters;
	Parameters = [&Expression]()
	{
		for (uint32 Index = 0; Index < WoTRemainingMaxItems; ++Index)
		{
			uint8 Token = 0;
			if (!Expression(Token)) return false;
			if (Token == 0x16) return true;
		}
		return false;
	};
	Expression = [&Reader, &MemoryOffset, &RecursionDepth, &Raw,
		&Reference, &Expression, &Parameters](uint8& OutToken)
	{
		if (++RecursionDepth > 256) return false;
		uint8 Token = 0;
		if (!Reader.ReadU8(Token)) { --RecursionDepth; return false; }
		++MemoryOffset;
		bool bOk = true;
		auto OneExpression = [&Expression]()
		{
			uint8 Ignored = 0;
			return Expression(Ignored);
		};
		const bool bNoOperands = Token == 0x08 || Token == 0x0b || Token == 0x16
			|| Token == 0x17 || Token == 0x25 || Token == 0x26 || Token == 0x27
			|| Token == 0x28 || Token == 0x2a || Token == 0x2d || Token == 0x30
			|| Token == 0x31;
		const bool bObjectReference = Token == 0x00 || Token == 0x01
			|| Token == 0x02 || Token == 0x20 || Token == 0x29;
		if (Token >= 0x39 && Token < 0x60) bOk = OneExpression();
		else if (Token >= 0x70) bOk = Parameters();
		else if (Token >= 0x60) bOk = Raw(1) && Parameters();
		else if (Token == 0x0f || Token == 0x10 || Token == 0x14 || Token == 0x1a)
			bOk = OneExpression() && OneExpression();
		else if (Token == 0x06) bOk = Raw(2);
		else if (bObjectReference) bOk = Reference(nullptr);
		else if (bNoOperands) bOk = true;
		else if (Token == 0x04 || Token == 0x0d || Token == 0x0e)
			bOk = OneExpression();
		else if (Token == 0x1c) bOk = Reference(nullptr) && Parameters();
		else if (Token == 0x1b || Token == 0x38)
			bOk = Reference(nullptr) && Parameters();
		else if (Token == 0x12 || Token == 0x19)
			bOk = OneExpression() && Raw(3) && OneExpression();
		else if (Token == 0x11)
		{
			for (int32 Index = 0; bOk && Index < 4; ++Index) bOk = OneExpression();
		}
		else if (Token == 0x1d || Token == 0x1e) bOk = Raw(4);
		else if (Token == 0x1f)
		{
			bOk = false;
			for (uint32 Index = 0; Index < WoTRemainingMaxSourceBytes; ++Index)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				++MemoryOffset;
				if (Byte == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x34)
		{
			bOk = false;
			for (uint32 Index = 0; Index < WoTRemainingMaxSourceBytes / 2; ++Index)
			{
				uint16 Value = 0;
				if (!Reader.ReadU16(Value)) break;
				MemoryOffset += 2;
				if (Value == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x21) bOk = Reference(nullptr);
		else if (Token == 0x22 || Token == 0x23) bOk = Raw(12);
		else if (Token == 0x24 || Token == 0x2c) bOk = Raw(1);
		else if (Token == 0x13 || Token == 0x2e)
			bOk = Reference(nullptr) && OneExpression();
		else if (Token == 0x07) bOk = Raw(2) && OneExpression();
		else if (Token == 0x2f) bOk = OneExpression() && Raw(2);
		else if (Token == 0x05) bOk = Raw(1) && OneExpression();
		else if (Token == 0x09) bOk = Raw(2) && OneExpression();
		else if (Token == 0x0a)
		{
			uint16 Destination = 0;
			bOk = Reader.ReadU16(Destination);
			if (bOk) MemoryOffset += 2;
			if (bOk && Destination != 0xffff) bOk = OneExpression();
		}
		else if (Token == 0x0c)
		{
			bOk = (MemoryOffset & 3) == 0;
			for (uint32 Index = 0; bOk && Index < WoTRemainingMaxItems; ++Index)
			{
				int32 Label = -1;
				bOk = Reference(&Label) && Raw(4);
				if (bOk && Label == 0) break;
				if (Index + 1 == WoTRemainingMaxItems) bOk = false;
			}
		}
		else if (Token == 0x18) bOk = Raw(2) && OneExpression();
		else if (Token == 0x32 || Token == 0x33)
			bOk = Reference(nullptr) && OneExpression() && OneExpression();
		else if (Token == 0x36) bOk = Reference(nullptr) && OneExpression();
		else bOk = false;
		OutToken = Token;
		--RecursionDepth;
		return bOk;
	};
	for (uint32 Index = 0; MemoryOffset < MemorySize && Index < WoTRemainingMaxItems;
		++Index)
	{
		uint8 Token = 0;
		if (!Expression(Token)) return false;
	}
	return MemoryOffset == MemorySize;
}

FWoTBootstrapStatus WoTRemainingReadClassDefaults(const FWoTRemainingSource& Source,
	const FWoTRemainingClassRecord& Record, TArray<FWoTTypedProperty>& Out,
	FWoTStructSchemaResolver& SchemaResolver)
{
	if (Record.bPlaceholder) return FWoTBootstrapStatus::Success();
	FWoTRemainingReader Reader(Source.Bytes);
	if (!WoTRemainingOpenExport(Source, *Record.Export, Reader))
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has malformed defaults"), Record.Identity));
	int32 SuperField = 0, NextField = 0, ScriptText = 0, Children = 0;
	int32 FriendlyIndex = 0, Line = 0, TextPosition = 0, MemorySize = 0;
	if (!Reader.ReadCompact(SuperField) || !Reader.ReadCompact(NextField)
		|| !Reader.ReadCompact(ScriptText) || !Reader.ReadCompact(Children)
		|| !Reader.ReadCompact(FriendlyIndex) || Source.NameAt(FriendlyIndex) == nullptr
		|| !Reader.ReadI32(Line) || !Reader.ReadI32(TextPosition)
		|| !Reader.ReadI32(MemorySize) || !WoTRemainingSkipScript(Reader, MemorySize)
		|| !Reader.Skip(42))
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has a malformed UStruct header"), Record.Identity));
	int32 DependencyCount = 0;
	if (!Reader.ReadCompact(DependencyCount) || DependencyCount < 0
		|| DependencyCount > static_cast<int32>(WoTRemainingMaxItems))
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has malformed dependencies"), Record.Identity));
	for (int32 Index = 0; Index < DependencyCount; ++Index)
	{
		int32 Dependency = 0;
		uint32 ScriptCrc = 0;
		if (!Reader.ReadCompact(Dependency) || !Reader.Skip(4)
			|| !Reader.ReadU32(ScriptCrc))
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a Class export has malformed dependencies"), Record.Identity));
	}
	int32 ImportCount = 0;
	if (!Reader.ReadCompact(ImportCount) || ImportCount < 0
		|| ImportCount > static_cast<int32>(WoTRemainingMaxItems))
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has malformed imports"), Record.Identity));
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		FString ImportedName;
		if (!WoTRemainingReadName(Reader, Source, ImportedName))
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a Class export has malformed imports"), Record.Identity));
	}
	int32 Within = 0;
	FString ConfigName;
	if (!Reader.ReadCompact(Within) || !WoTRemainingReadName(Reader, Source, ConfigName)
		|| !WoTRemainingReadProperties(
			Reader, Source, Out, TEXT("class defaults"), true))
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has malformed tagged defaults"), Record.Identity));
	if (Reader.Tell() != Reader.GetEnd())
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a Class export has trailing default bytes"), Record.Identity));
	// UWindowLookAndFeel.uc:23 declares HeadingActiveTitleColor without a
	// defaultproperties assignment. UE1 therefore omits its all-zero Color from
	// the tagged CDO stream. Preserve that authenticated implicit default in the
	// typed graph so consumers do not replace retail data with a C++ fallback.
	if (Record.Identity.Equals(TEXT("UWindow.UWindowLookAndFeel"),
		ESearchCase::CaseSensitive)
		&& !Out.ContainsByPredicate([](const FWoTTypedProperty& Property)
		{
			return Property.ArrayIndex == 0 && Property.Name.Equals(
				TEXT("HeadingActiveTitleColor"), ESearchCase::IgnoreCase);
		}))
	{
		FWoTTypedProperty ImplicitDefault;
		FString Detail;
		if (!SchemaResolver.MaterializeImplicitZeroClassDefaultStruct(
			Source, Record.ExportIndex, TEXT("HeadingActiveTitleColor"),
			ImplicitDefault, Detail))
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the implicit-zero look-and-feel default cannot be decoded"),
				Record.Identity + TEXT(".HeadingActiveTitleColor: ") + Detail));
		}
		Out.Add(MoveTemp(ImplicitDefault));
		Out.Sort([](const FWoTTypedProperty& Left,
			const FWoTTypedProperty& Right)
		{
			const FString LF = WoTTypedPropertyFold(Left.Name);
			const FString RF = WoTTypedPropertyFold(Right.Name);
			if (LF != RF) return LF < RF;
			if (Left.ArrayIndex != Right.ArrayIndex)
				return Left.ArrayIndex < Right.ArrayIndex;
			return Left.Name < Right.Name;
		});
	}
	for (FWoTTypedProperty& Property : Out)
	{
		if (Property.Type != 10) continue;
		FString Detail;
		if (!SchemaResolver.DecodeClassDefaultStruct(Source, Record.ExportIndex,
			Record.Identity, Property, Detail))
		{
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a class-default struct cannot be decoded by reflection"),
				Record.Identity + TEXT(".") + Property.Name + TEXT(": ") + Detail));
		}
	}
	FWoTBootstrapStatus Bound = WoTRemainingBindProperties(Source, Out);
	if (Bound.IsError()) return Bound;
	return FWoTBootstrapStatus::Success();
}

struct FWoTRemainingRegistryImport
{
	FString Identity;
	FString ClassIdentity;
};

struct FWoTRemainingRegistryObject
{
	FString Identity;
	FString ClassIdentity;
	uint32 ExportIndex = 0;
	bool bPlaceholder = false;
};

struct FWoTRemainingDirective
{
	FString Owner;
	uint32 LineIndex = 0;
	TArray<uint8> Line;
};

bool WoTRemainingDecodeTextBuffer(const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export, TArray<uint8>& Out)
{
	FWoTRemainingReader Reader(Source.Bytes);
	TArray<FWoTTypedProperty> Properties;
	int32 Cursor = 0, Top = 0, TextSize = 0;
	if (!WoTRemainingOpenExport(Source, Export, Reader)
		|| !WoTRemainingReadProperties(Reader, Source, Properties, TEXT("TextBuffer"))
		|| !Reader.ReadI32(Cursor) || !Reader.ReadI32(Top)
		|| !Reader.ReadCompact(TextSize) || TextSize <= 0
		|| static_cast<uint64>(TextSize) != Reader.Remaining()
		|| !Reader.ReadBytes(static_cast<uint64>(TextSize), Out)
		|| Out.Last() != 0) return false;
	for (int32 Index = 0; Index + 1 < Out.Num(); ++Index)
		if (Out[Index] == 0) return false;
	Out.Pop(EAllowShrinking::No);
	return true;
}

void WoTRemainingCollectDirectives(const FString& Owner,
	TConstArrayView<uint8> Text, TArray<FWoTRemainingDirective>& Out)
{
	uint32 LineIndex = 0;
	int32 Start = 0;
	for (int32 Offset = 0; Offset <= Text.Num(); ++Offset)
	{
		const bool bEnd = Offset == Text.Num();
		const bool bBreak = !bEnd && (Text[Offset] == '\r' || Text[Offset] == '\n'
			|| Text[Offset] == 0x0b || Text[Offset] == 0x0c
			|| Text[Offset] == 0x1c || Text[Offset] == 0x1d
			|| Text[Offset] == 0x1e || Text[Offset] == 0x85);
		if (!bEnd && !bBreak) continue;
		int32 First = Start;
		while (First < Offset && (Text[First] == ' ' || Text[First] == '\t')) ++First;
		if (Offset - First >= 5 && Text[First] == '#'
			&& (Text[First + 1] == 'e' || Text[First + 1] == 'E')
			&& (Text[First + 2] == 'x' || Text[First + 2] == 'X')
			&& (Text[First + 3] == 'e' || Text[First + 3] == 'E')
			&& (Text[First + 4] == 'c' || Text[First + 4] == 'C'))
		{
			FWoTRemainingDirective& Directive = Out.AddDefaulted_GetRef();
			Directive.Owner = Owner;
			Directive.LineIndex = LineIndex;
			Directive.Line.Append(Text.GetData() + Start, Offset - Start);
		}
		if (!bEnd && Text[Offset] == '\r' && Offset + 1 < Text.Num()
			&& Text[Offset + 1] == '\n') ++Offset;
		Start = Offset + 1;
		++LineIndex;
	}
}

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildRegistryObject(
	const FWoTRequiredFact& Fact, const FWoTRemainingSource& Source)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TMap<FString, FString> UniqueNames;
	for (const FString& Name : Source.Names)
	{
		const FString Folded = WoTRemainingFold(Name);
		if (const FString* Existing = UniqueNames.Find(Folded))
		{
			if (*Existing != Name)
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::DuplicateField,
					TEXT("duplicate package name after case folding"), Name));
		}
		else UniqueNames.Add(Folded, Name);
	}
	TArray<FString> Names;
	UniqueNames.GenerateValueArray(Names);
	Names.Sort([](const FString& Left, const FString& Right)
	{
		const FString LF = WoTRemainingFold(Left), RF = WoTRemainingFold(Right);
		return LF == RF ? Left < Right : LF < RF;
	});
	TArray<FWoTRemainingRegistryImport> Imports;
	for (int32 ImportIndex = 0; ImportIndex < Source.Imports.Num(); ++ImportIndex)
	{
		FWoTRemainingRegistryImport& Imported = Imports.AddDefaulted_GetRef();
		TWoTBootstrapResult<FString> Identity =
			WoTRemainingCanonicalIdentity(Source, -ImportIndex - 1);
		if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
		Imported.Identity = MoveTemp(Identity).GetValue();
		Imported.ClassIdentity = Source.Imports[ImportIndex].ClassPackage
			+ TEXT(".") + Source.Imports[ImportIndex].ClassName;
	}
	Imports.Sort([](const FWoTRemainingRegistryImport& Left,
		const FWoTRemainingRegistryImport& Right)
	{
		const FString LI = WoTRemainingFold(Left.Identity);
		const FString RI = WoTRemainingFold(Right.Identity);
		if (LI != RI) return LI < RI;
		if (Left.Identity != Right.Identity) return Left.Identity < Right.Identity;
		const FString LC = WoTRemainingFold(Left.ClassIdentity);
		const FString RC = WoTRemainingFold(Right.ClassIdentity);
		return LC == RC ? Left.ClassIdentity < Right.ClassIdentity : LC < RC;
	});
	TWoTBootstrapResult<TArray<FWoTRemainingClassRecord>> ClassResult =
		WoTRemainingClasses(Source);
	if (ClassResult.IsError()) return FResult::Error(MoveTemp(ClassResult).GetError());
	const TArray<FWoTRemainingClassRecord>& Classes = ClassResult.GetValue();
	TMap<int32, const FWoTRemainingClassRecord*> ClassByIndex;
	for (const FWoTRemainingClassRecord& Class : Classes)
		ClassByIndex.Add(Class.ExportIndex, &Class);
	TArray<FString> RawIdentities;
	TArray<bool> Placeholders;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		FString Identity;
		bool bPlaceholder = false;
		if (const FWoTRemainingClassRecord* const* Class =
			ClassByIndex.Find(ExportIndex + 1))
		{
			Identity = (*Class)->Identity;
			bPlaceholder = (*Class)->bPlaceholder;
		}
		else
		{
			TWoTBootstrapResult<FString> Path =
				WoTRemainingCanonicalIdentity(Source, ExportIndex + 1);
			if (Path.IsError()) return FResult::Error(MoveTemp(Path).GetError());
			Identity = MoveTemp(Path).GetValue();
		}
		RawIdentities.Add(Identity);
		Placeholders.Add(bPlaceholder);
	}
	TArray<FWoTRemainingRegistryObject> Objects;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		FWoTRemainingRegistryObject& Object = Objects.AddDefaulted_GetRef();
		Object.Identity = RawIdentities[ExportIndex];
		Object.ExportIndex = static_cast<uint32>(ExportIndex + 1);
		Object.bPlaceholder = Placeholders[ExportIndex];
		const FString* ClassIdentity =
			Source.CanonicalClassByReference.Find(ExportIndex + 1);
		if (ClassIdentity == nullptr)
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a registry export lacks its canonical class"), Source.Path));
		Object.ClassIdentity = *ClassIdentity;
	}
	Objects.Sort([](const FWoTRemainingRegistryObject& Left,
		const FWoTRemainingRegistryObject& Right)
	{
		const FString LF = WoTRemainingFold(Left.Identity);
		const FString RF = WoTRemainingFold(Right.Identity);
		return LF == RF ? Left.Identity < Right.Identity : LF < RF;
	});
	TArray<FWoTRemainingDirective> Directives;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const FWoTRemainingExport& Export = Source.Exports[ExportIndex];
		const FString* ClassName = Source.ObjectName(Export.ClassReference);
		if (ClassName == nullptr || *ClassName != TEXT("TextBuffer") || Export.Size == 0)
			continue;
		TArray<uint8> Text;
		TWoTBootstrapResult<FString> Owner =
			WoTRemainingCanonicalIdentity(Source, ExportIndex + 1);
		if (Owner.IsError()) return FResult::Error(MoveTemp(Owner).GetError());
		const FString* OwnerClass =
			Source.CanonicalClassByReference.Find(ExportIndex + 1);
		if (OwnerClass == nullptr)
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a directive owner lacks its canonical class"), Source.Path));
		FWoTBootstrapStatus Closed = WoTRemainingRequireSemanticReference(
			Source, Owner.GetValue(), *OwnerClass, TEXT("directive owner"));
		if (Closed.IsError()) return FResult::Error(MoveTemp(Closed).GetError());
		if (!WoTRemainingDecodeTextBuffer(Source, Export, Text))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a registry TextBuffer is malformed"), Owner.GetValue()));
		WoTRemainingCollectDirectives(Owner.GetValue(), Text, Directives);
	}
	Directives.Sort([](const FWoTRemainingDirective& Left,
		const FWoTRemainingDirective& Right)
	{
		const FString LF = WoTRemainingFold(Left.Owner);
		const FString RF = WoTRemainingFold(Right.Owner);
		if (LF != RF) return LF < RF;
		if (Left.Owner != Right.Owner) return Left.Owner < Right.Owner;
		return Left.LineIndex < Right.LineIndex;
	});
	if (Names.Num() > static_cast<int32>(WoTRemainingMaxItems)
		|| Imports.Num() > static_cast<int32>(WoTRemainingMaxItems)
		|| Objects.Num() > static_cast<int32>(WoTRemainingMaxItems)
		|| Classes.Num() > static_cast<int32>(WoTRemainingMaxItems)
		|| Directives.Num() > static_cast<int32>(WoTRemainingMaxItems))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("the package registry exceeds its item bound"), Source.Path));
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>("WOTREG3"), 8);
	WoTRemainingAppendU16(Payload, 1);
	WoTRemainingAppendU16(Payload, 0);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Names.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Imports.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Objects.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Classes.Num()));
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Directives.Num()));
	for (const FString& Name : Names)
		if (!WoTRemainingAppendString(Payload, Name)) return FResult::Error(
			WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a registry name exceeds its bound"), Source.Path));
	for (const FWoTRemainingRegistryImport& Imported : Imports)
	{
		if (!WoTRemainingAppendString(Payload, Imported.Identity)
			|| !WoTRemainingAppendString(Payload, Imported.ClassIdentity))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a registry import exceeds its bound"), Source.Path));
	}
	for (const FWoTRemainingRegistryObject& Object : Objects)
	{
		if (!WoTRemainingAppendString(Payload, Object.Identity)
			|| !WoTRemainingAppendString(Payload, Object.ClassIdentity))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a registry export exceeds its bound"), Source.Path));
		WoTRemainingAppendU32(Payload, Object.ExportIndex);
		Payload.Add(Object.bPlaceholder ? 1 : 0);
	}
	for (const FWoTRemainingClassRecord& Class : Classes)
	{
		if (!WoTRemainingAppendString(Payload, Class.Identity))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a registry class exceeds its bound"), Source.Path));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Class.ExportIndex));
		Payload.Add(Class.bPlaceholder ? 1 : 0);
	}
	for (const FWoTRemainingDirective& Directive : Directives)
	{
		if (!WoTRemainingAppendString(Payload, Directive.Owner))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a registry directive owner exceeds its bound"), Source.Path));
		WoTRemainingAppendU32(Payload, Directive.LineIndex);
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Directive.Line.Num()));
		Payload.Append(Directive.Line);
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTRemainingSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("class_count"), FWoTCanonicalJsonValue::MakeInteger(Classes.Num()));
	Builder.Add(TEXT("directive_count"),
		FWoTCanonicalJsonValue::MakeInteger(Directives.Num()));
	Builder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-package-registry-v1")));
	Builder.Add(TEXT("export_count"), FWoTCanonicalJsonValue::MakeInteger(Objects.Num()));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("import_count"), FWoTCanonicalJsonValue::MakeInteger(Imports.Num()));
	Builder.Add(TEXT("importer"), FWoTCanonicalJsonValue::MakeString(TEXT("registry-v1")));
	Builder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(1));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("registry")));
	Builder.Add(TEXT("name_count"), FWoTCanonicalJsonValue::MakeInteger(Names.Num()));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("registry object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTRemainingFrame(Header.GetValue(), Payload);
}

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildClassDefaultsObject(
	const FWoTRequiredFact& Fact, const FWoTRemainingSource& Source,
	FWoTRemainingPackageResolver& Resolver)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<FWoTRemainingClassRecord>> ClassResult =
		WoTRemainingClasses(Source);
	if (ClassResult.IsError()) return FResult::Error(MoveTemp(ClassResult).GetError());
	const TArray<FWoTRemainingClassRecord>& Classes = ClassResult.GetValue();
	FWoTStructSchemaResolver SchemaResolver(Resolver);
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>("WOTDEF3"), 8);
	WoTRemainingAppendU16(Payload, 3);
	WoTRemainingAppendU16(Payload, 0);
	WoTRemainingAppendU32(Payload, static_cast<uint32>(Classes.Num()));
	uint32 DefaultCount = 0, ReferenceCount = 0;
	for (const FWoTRemainingClassRecord& Class : Classes)
	{
		TArray<FWoTTypedProperty> Properties;
		FWoTBootstrapStatus ReadDefaults = WoTRemainingReadClassDefaults(
			Source, Class, Properties, SchemaResolver);
		if (ReadDefaults.IsError())
			return FResult::Error(MoveTemp(ReadDefaults).GetError());
		if (!WoTRemainingAppendString(Payload, Class.Identity))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a class-default identity exceeds its bound"), Class.Identity));
		WoTRemainingAppendU32(Payload, static_cast<uint32>(Class.ExportIndex));
		TWoTBootstrapResult<FString> Superclass = WoTRemainingCanonicalIdentity(
			Source, Class.Export->SuperReference, true);
		if (Superclass.IsError()) return FResult::Error(MoveTemp(Superclass).GetError());
		if (!Superclass.GetValue().IsEmpty())
		{
			FWoTBootstrapStatus Closed = WoTRemainingRequireSemanticReference(
				Source, Superclass.GetValue(), TEXT("Core.Class"),
				TEXT("class superclass"));
			if (Closed.IsError()) return FResult::Error(MoveTemp(Closed).GetError());
		}
		if (!WoTRemainingAppendString(Payload, Superclass.GetValue()))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("class defaults exceed their string bound"), Class.Identity));
		Payload.Add(Class.bPlaceholder ? 1 : 0);
		if (!WoTAppendCanonicalTypedProperties(Payload, Properties, true))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("class defaults exceed their string bound"), Class.Identity));
		DefaultCount += static_cast<uint32>(Properties.Num());
		TArray<const FWoTTypedProperty*> PendingProperties;
		for (const FWoTTypedProperty& Property : Properties)
			PendingProperties.Add(&Property);
		while (!PendingProperties.IsEmpty())
		{
			const FWoTTypedProperty* Property = PendingProperties.Pop();
			if (!Property->ObjectIdentity.IsEmpty()) ++ReferenceCount;
			for (const FWoTTypedProperty& Member : Property->StructMembers)
				PendingProperties.Add(&Member);
		}
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTRemainingSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("class_count"), FWoTCanonicalJsonValue::MakeInteger(Classes.Num()));
	Builder.Add(TEXT("default_count"), FWoTCanonicalJsonValue::MakeInteger(DefaultCount));
	Builder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-class-defaults-v2")));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("class-default-v2")));
	Builder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(2));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("class_default")));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	Builder.Add(TEXT("reference_count"),
		FWoTCanonicalJsonValue::MakeInteger(ReferenceCount));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("class-default object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTRemainingFrame(Header.GetValue(), Payload);
}

struct FWoTRemainingFontFactType
{
	const TCHAR* Importer = nullptr;
	const TCHAR* Schema = nullptr;
};

bool WoTRemainingFontSelectFact(const FWoTRequiredFact& Fact,
	FWoTRemainingFontFactType& Out)
{
	if (Fact.Domain == EWoTFactDomain::Font)
	{
		Out = {TEXT("font-v2"), TEXT("wot-font-v3")};
		return true;
	}
	if (Fact.Domain == EWoTFactDomain::Procedural)
	{
		Out = {TEXT("procedural-v1"), TEXT("wot-procedural-v3")};
		return true;
	}
	return false;
}

class FWoTRemainingCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTRemainingCollectSink(uint64 InLimit) : Limit(InLimit) {}
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		const uint64 Count = static_cast<uint64>(Chunk.Num());
		if (Count > Limit - Written
			|| Count > static_cast<uint64>(MAX_int32 - Bytes.Num()))
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::SizeLimit,
				TEXT("a remaining-domain source exceeds its quarantine bound")));
		Bytes.Append(Chunk.GetData(), Chunk.Num());
		Written += Count;
		return FWoTBootstrapStatus::Success();
	}
	TArray<uint8> Bytes;
private:
	uint64 Limit = 0;
	uint64 Written = 0;
};

using FWoTRemainingLoader =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;
using FWoTRemainingPublicationVerifier = TFunction<FWoTBootstrapStatus()>;

TWoTBootstrapResult<TArray<FWoTCacheBlob>> WoTRemainingImportFontCore(
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress,
	FWoTRemainingLoader&& Loader,
	FWoTRemainingPublicationVerifier&& ReverifyBeforePublication)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
	uint64 Total = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTRemainingFontFactType Type;
		if (WoTRemainingFontSelectFact(Fact, Type))
		{
			++Total;
		}
	}
	FWoTBootstrapStatus Initial = Progress.ReportProgress(TEXT("font-procedural"), 0, Total);
	if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
	if (Progress.IsCancellationRequested())
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::ServiceUnavailable,
			TEXT("font/procedural import was cancelled"), TEXT("font-procedural")));
	TArray<FWoTCacheBlob> Blobs;
	TSet<FString> LogicalIds;
	TOptional<FWoTRemainingSource> CachedSource;
	FString CachedSourcePath;
	uint64 Completed = 0;
	// Foreign palette/source references (v2 procedural objects) resolve only
	// through packages the contract names; parsed once per import run.
	FWoTRemainingPackageResolver Resolver(Contract, Loader);
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTRemainingFontFactType Type;
		if (!WoTRemainingFontSelectFact(Fact, Type)) continue;
		if (Fact.FactId.IsEmpty() || Fact.SourcePath.IsEmpty()
			|| Fact.SourceExport.IsEmpty() || Fact.Importer != Type.Importer
			|| Fact.Outputs.Num() != 1 || Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].Schema != Type.Schema
			|| Fact.Outputs[0].MaxDecodedSize == 0)
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
				TEXT("the font/procedural fact has a closed-schema mismatch"), Fact.FactId));
		}
		const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
		if (LogicalIds.Contains(Output.LogicalId))
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::DuplicateField,
					TEXT("two remaining-domain facts emit one logical object"), Output.LogicalId));
		LogicalIds.Add(Output.LogicalId);
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::ServiceUnavailable,
				TEXT("font/procedural import was cancelled"), Fact.FactId));
		if (!CachedSource.IsSet() || CachedSourcePath != Fact.SourcePath)
		{
			TWoTBootstrapResult<TArray<uint8>> Loaded = Loader(Fact.SourcePath);
			if (Loaded.IsError())
				return FResult::Error(MoveTemp(Loaded).GetError());
			TWoTBootstrapResult<FWoTRemainingSource> Parsed =
				WoTRemainingParsePackage(Loaded.GetValue(), Fact.SourcePath);
			if (Parsed.IsError())
				return FResult::Error(MoveTemp(Parsed).GetError());
			CachedSource.Emplace(MoveTemp(Parsed).GetValue());
			CachedSourcePath = Fact.SourcePath;
		}
		const FWoTRemainingSource& Source = CachedSource.GetValue();
		TWoTBootstrapResult<TArray<uint8>> Object = [&]()
			-> TWoTBootstrapResult<TArray<uint8>>
		{
			if (Fact.Domain == EWoTFactDomain::Font)
			{
				TWoTBootstrapResult<const FWoTRemainingExport*> Export =
					WoTRemainingFindExport(Source, Fact.SourceExport, TEXT("Font"));
				if (Export.IsError())
					return TWoTBootstrapResult<TArray<uint8>>::Error(
						MoveTemp(Export).GetError());
				return WoTRemainingBuildFontObject(
					Fact, Contract, Source, *Export.GetValue());
			}
			TWoTBootstrapResult<FWoTRemainingResolvedProceduralExport> Resolved =
				WoTRemainingFindProceduralExport(Source, Fact.SourceExport);
			if (Resolved.IsError())
				return TWoTBootstrapResult<TArray<uint8>>::Error(
					MoveTemp(Resolved).GetError());
			FWoTRequiredFact CanonicalFact = Fact;
			CanonicalFact.SourceExport = Resolved.GetValue().CanonicalIdentity;
			return WoTRemainingBuildProceduralObject(
				CanonicalFact, Source, *Resolved.GetValue().Export, Resolver);
		}();
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		if (static_cast<uint64>(Object.GetValue().Num()) > Output.MaxDecodedSize)
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("the font/procedural object exceeds its contract maximum"),
				Output.LogicalId));
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::ServiceUnavailable,
				TEXT("font/procedural import was cancelled"), Fact.FactId));
		FWoTBootstrapStatus Reverified = ReverifyBeforePublication();
		if (Reverified.IsError())
			return FResult::Error(MoveTemp(Reverified).GetError());
		const FWoTSha256 ObjectSha256 = Sha256(Object.GetValue());
		FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
			Output.Schema, Object.GetValue(), ObjectSha256, Output.MaxDecodedSize);
		if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
		FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = Output.LogicalId;
		Blob.Schema = Output.Schema;
		Blob.Sha256 = ObjectSha256;
		Blob.Size = static_cast<uint64>(Object.GetValue().Num());
		Blob.Satisfies.AddDefaulted_GetRef().Value = Fact.FactId;
		FWoTBootstrapStatus Reported = Progress.ReportProgress(
			TEXT("font-procedural"), ++Completed, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::ServiceUnavailable,
				TEXT("font/procedural import was cancelled"), Fact.FactId));
	}
	return FResult::Success(MoveTemp(Blobs));
}
} // namespace

#if !WOT_GUARDED_BOOTSTRAP
struct FWoTDevelopmentProceduralPackage::FImpl
{
	FWoTRemainingSource Source;
};

FWoTDevelopmentProceduralPackage::FWoTDevelopmentProceduralPackage(
	TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FWoTDevelopmentProceduralPackage::~FWoTDevelopmentProceduralPackage() = default;

TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentProceduralPackage>>
FWoTDevelopmentProceduralPackage::Parse(
	TArray<uint8> SourceBytes, FStringView SourcePath)
{
	using FResult =
		TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentProceduralPackage>>;
	TWoTBootstrapResult<FWoTRemainingSource> Parsed =
		WoTRemainingParsePackage(SourceBytes, SourcePath);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	TUniquePtr<FImpl> State = MakeUnique<FImpl>();
	State->Source = MoveTemp(Parsed).GetValue();
	return FResult::Success(
		TUniquePtr<FWoTDevelopmentProceduralPackage>(
			new FWoTDevelopmentProceduralPackage(MoveTemp(State))));
}

bool FWoTDevelopmentProceduralPackage::FindTexture(
	FStringView SourceExport, uint32& OutUSize,
	uint32& OutVSize, uint8& OutFormat) const
{
	OutUSize = 0;
	OutVSize = 0;
	OutFormat = 0;
	if (!Impl.IsValid())
	{
		return false;
	}
	TWoTBootstrapResult<FWoTRemainingResolvedProceduralExport> Resolved =
		WoTRemainingFindProceduralExport(Impl->Source, SourceExport);
	if (Resolved.IsError())
	{
		return false;
	}
	TWoTBootstrapResult<FWoTRemainingProcedural> Decoded =
		WoTRemainingDecodeProcedural(
			Impl->Source, *Resolved.GetValue().Export);
	if (Decoded.IsError() || Decoded.GetValue().Mips.IsEmpty())
	{
		return false;
	}
	OutUSize = Decoded.GetValue().Mips[0].Width;
	OutVSize = Decoded.GetValue().Mips[0].Height;
	return OutUSize > 0 && OutVSize > 0;
}
#endif

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildPackageAggregateObject(
	TConstArrayView<uint8> SourceBytes, FStringView SourcePath,
	const FWoTRequiredFact& Fact, FWoTRemainingPackageResolver& Resolver)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTRemainingSource> Parsed =
		WoTRemainingParsePackage(SourceBytes, SourcePath);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	if (Fact.Domain == EWoTFactDomain::Registry)
		return WoTRemainingBuildRegistryObject(Fact, Parsed.GetValue());
	if (Fact.Domain == EWoTFactDomain::ClassDefault)
		return WoTRemainingBuildClassDefaultsObject(Fact, Parsed.GetValue(), Resolver);
	return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
		TEXT("the aggregate package builder received the wrong fact domain"), Fact.FactId));
}

TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportFontProcedural(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	FWoTRemainingLoader Loader = [&Package](FStringView SourcePath)
		-> TWoTBootstrapResult<TArray<uint8>>
	{
		using FBytesResult = TWoTBootstrapResult<TArray<uint8>>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FBytesResult::Error(MoveTemp(Entry).GetError());
		if (Entry.GetValue().Size == 0
			|| Entry.GetValue().Size > WoTRemainingMaxSourceBytes)
			return FBytesResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
				TEXT("a font/procedural source exceeds its declared bound"), FString(SourcePath)));
		FWoTRemainingCollectSink Sink(WoTRemainingMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError()) return FBytesResult::Error(MoveTemp(Streamed).GetError());
		return FBytesResult::Success(MoveTemp(Sink.Bytes));
	};
	FWoTRemainingPublicationVerifier ReverifyBeforePublication = [&Package]()
	{
		return Package.ReverifyHeldSnapshotAfterExtraction();
	};
	return WoTRemainingImportFontCore(
		Contract, Writer, Progress, MoveTemp(Loader),
		MoveTemp(ReverifyBeforePublication));
}

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus ComposeFontCodepointForTesting(uint32 PageIndex,
	uint32 CharactersPerPage, uint32 LocalIndex, uint32& OutCodepoint)
{
	return WoTRemainingComposeFontCodepoint(
		PageIndex, CharactersPerPage, LocalIndex, OutCodepoint);
}

FWoTBootstrapStatus ValidateFontV2MaterializationBoundsForTesting(
	const FWoTFontV2Record& Record, uint32& OutVisitedGlyphs)
{
	return WoTRemainingValidateFontMaterialization(Record, &OutVisitedGlyphs);
}

TWoTBootstrapResult<FWoTRemainingPackageInspectionForTesting>
InspectRemainingPackageForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath)
{
	using FResult =
		TWoTBootstrapResult<FWoTRemainingPackageInspectionForTesting>;
	TWoTBootstrapResult<FWoTRemainingSource> Parsed =
		WoTRemainingParsePackage(SourceBytes, SourcePath);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTRemainingSource& Source = Parsed.GetValue();
	FWoTRemainingPackageInspectionForTesting Result;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const FWoTRemainingExport& Export = Source.Exports[ExportIndex];
		const FString* ClassName = Source.ObjectName(Export.ClassReference);
		if (Export.ClassReference == 0) ++Result.ClassCount;
		if (ClassName == nullptr) continue;
		TArray<FWoTRemainingExportForTesting>* Domain = nullptr;
		if (*ClassName == TEXT("Font")) Domain = &Result.Fonts;
		else if (*ClassName == TEXT("Texture")) Domain = &Result.Textures;
		else if (*ClassName == TEXT("FireTexture")
			|| *ClassName == TEXT("WaterTexture")
			|| *ClassName == TEXT("WaveTexture")
			|| *ClassName == TEXT("WetTexture")
			|| *ClassName == TEXT("IceTexture")) Domain = &Result.Procedurals;
		if (Domain == nullptr) continue;
		TWoTBootstrapResult<FString> FactIdentity =
			WoTRemainingObjectPath(Source, ExportIndex + 1);
		TWoTBootstrapResult<FString> CanonicalIdentity =
			WoTRemainingCanonicalIdentity(Source, ExportIndex + 1);
		if (FactIdentity.IsError())
			return FResult::Error(MoveTemp(FactIdentity).GetError());
		if (CanonicalIdentity.IsError())
			return FResult::Error(MoveTemp(CanonicalIdentity).GetError());
		FWoTRemainingExportForTesting& Item = Domain->AddDefaulted_GetRef();
		Item.FactIdentity = MoveTemp(FactIdentity).GetValue();
		Item.CanonicalIdentity = MoveTemp(CanonicalIdentity).GetValue();
	}
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportFontProceduralPackageBytesForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress,
	FWoTRemainingSiblingLoaderForTesting SiblingLoader)
{
	TArray<uint8> Owned;
	Owned.Append(SourceBytes.GetData(), SourceBytes.Num());
	const FString ExactPath(SourcePath);
	FWoTRemainingLoader Loader = [Owned = MoveTemp(Owned), ExactPath,
		SiblingLoader = MoveTemp(SiblingLoader)](FStringView Wanted)
		-> TWoTBootstrapResult<TArray<uint8>>
	{
		using FBytesResult = TWoTBootstrapResult<TArray<uint8>>;
		if (FString(Wanted) != ExactPath)
		{
			if (SiblingLoader) return SiblingLoader(Wanted);
			return FBytesResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
				TEXT("the requested test source is absent"), FString(Wanted)));
		}
		return FBytesResult::Success(Owned);
	};
	FWoTRemainingPublicationVerifier ReverifyBeforePublication = []()
	{
		return FWoTBootstrapStatus::Success();
	};
	return WoTRemainingImportFontCore(
		Contract, Writer, Progress, MoveTemp(Loader),
		MoveTemp(ReverifyBeforePublication));
}
#endif
