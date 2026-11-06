#include "WoTFrontEndTestData.h"
#include "WoTCacheTestSupport.h"

#if WITH_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"
#include "../Payload/WoTCacheStore.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTPreferencesSubsystem.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTLaunchEngineHooks.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/DevelopmentClassDefaults.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
void AppendU16(TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value));
	Out.Add(static_cast<uint8>(Value >> 8));
}

void AppendU32(TArray<uint8>& Out, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
		Out.Add(static_cast<uint8>(Value >> Shift));
}

void AppendU64(TArray<uint8>& Out, uint64 Value)
{
	for (int32 Shift = 0; Shift < 64; Shift += 8)
		Out.Add(static_cast<uint8>(Value >> Shift));
}

void AppendFloat(TArray<uint8>& Out, float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	AppendU32(Out, Bits);
}

void AppendString(TArray<uint8>& Out, FStringView Value)
{
	const FTCHARToUTF8 Utf8(Value.GetData(), Value.Len());
	AppendU32(Out, static_cast<uint32>(Utf8.Length()));
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

FWoTSha256 SeedDigest(uint8 Seed)
{
	FWoTSha256 Result;
	for (int32 Index = 0; Index < FWoTSha256::ByteCount; ++Index)
		Result.Bytes[Index] = static_cast<uint8>(Seed + Index);
	return Result;
}

bool ReadFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Tools/dist/fixtures"), Name));
}

bool SplitObject(TConstArrayView<uint8> Object, FString& Header,
	TArray<uint8>& Payload)
{
	if (Object.Num() < 12 || FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
		return false;
	const uint32 HeaderSize = static_cast<uint32>(Object[8])
		| static_cast<uint32>(Object[9]) << 8
		| static_cast<uint32>(Object[10]) << 16
		| static_cast<uint32>(Object[11]) << 24;
	if (HeaderSize > static_cast<uint32>(Object.Num() - 12)) return false;
	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Object.GetData() + 12), HeaderSize);
	Header = FString(Converted.Length(), Converted.Get());
	Payload.Append(Object.GetData() + 12 + HeaderSize,
		Object.Num() - 12 - static_cast<int32>(HeaderSize));
	return true;
}

bool ReplaceJsonString(FString& Header, const FString& Prefix,
	FStringView Replacement)
{
	const int32 Begin = Header.Find(Prefix, ESearchCase::CaseSensitive);
	if (Begin == INDEX_NONE) return false;
	const int32 ValueBegin = Begin + Prefix.Len();
	const int32 End = Header.Find(TEXT("\""), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, ValueBegin);
	if (End == INDEX_NONE) return false;
	Header = Header.Left(ValueBegin) + FString(Replacement) + Header.Mid(End);
	return true;
}

bool ReplaceLengthPrefixedString(TArray<uint8>& Payload,
	FStringView OldValue, FStringView NewValue)
{
	const FTCHARToUTF8 OldUtf8(OldValue.GetData(), OldValue.Len());
	const FTCHARToUTF8 NewUtf8(NewValue.GetData(), NewValue.Len());
	for (int32 Index = 4; Index <= Payload.Num() - OldUtf8.Length(); ++Index)
	{
		if (FMemory::Memcmp(Payload.GetData() + Index,
			OldUtf8.Get(), OldUtf8.Length()) != 0) continue;
		const uint32 StoredLength = static_cast<uint32>(Payload[Index - 4])
			| static_cast<uint32>(Payload[Index - 3]) << 8
			| static_cast<uint32>(Payload[Index - 2]) << 16
			| static_cast<uint32>(Payload[Index - 1]) << 24;
		if (StoredLength != static_cast<uint32>(OldUtf8.Length())) continue;
		Payload.RemoveAt(Index, OldUtf8.Length());
		Payload.Insert(reinterpret_cast<const uint8*>(NewUtf8.Get()),
			NewUtf8.Length(), Index);
		const uint32 NewLength = static_cast<uint32>(NewUtf8.Length());
		for (int32 Byte = 0; Byte < 4; ++Byte)
			Payload[Index - 4 + Byte] = static_cast<uint8>(NewLength >> (Byte * 8));
		return true;
	}
	return false;
}

bool ReplacePayloadLength(FString& Header, int32 PayloadLength)
{
	const FString Prefix = TEXT("\"payload_byte_length\":");
	const int32 Begin = Header.Find(Prefix, ESearchCase::CaseSensitive);
	if (Begin == INDEX_NONE) return false;
	const int32 ValueBegin = Begin + Prefix.Len();
	int32 ValueEnd = ValueBegin;
	while (ValueEnd < Header.Len() && FChar::IsDigit(Header[ValueEnd])) ++ValueEnd;
	if (ValueEnd == ValueBegin) return false;
	Header = Header.Left(ValueBegin) + FString::FromInt(PayloadLength)
		+ Header.Mid(ValueEnd);
	return true;
}

bool ReplaceJsonUnsigned(FString& Header, const FString& Prefix, uint32 Value)
{
	const int32 Begin = Header.Find(Prefix, ESearchCase::CaseSensitive);
	if (Begin == INDEX_NONE) return false;
	const int32 ValueBegin = Begin + Prefix.Len();
	int32 ValueEnd = ValueBegin;
	while (ValueEnd < Header.Len() && FChar::IsDigit(Header[ValueEnd])) ++ValueEnd;
	if (ValueEnd == ValueBegin) return false;
	Header = Header.Left(ValueBegin) + FString::FromInt(Value)
		+ Header.Mid(ValueEnd);
	return true;
}

TArray<uint8> RetargetObject(TConstArrayView<uint8> Object,
	FStringView SourceExport)
{
	FString Header;
	TArray<uint8> Payload;
	if (!SplitObject(Object, Header, Payload)
		|| !ReplaceJsonString(Header, TEXT("\"source\":{\"export\":\""),
			SourceExport)) return {};
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> RetargetMaskedTexture(TConstArrayView<uint8> Object,
	FStringView SourceExport)
{
	FString Header;
	TArray<uint8> Payload;
	if (!SplitObject(Object, Header, Payload) || Payload.Num() < 12
		|| FMemory::Memcmp(Payload.GetData(), "WOTPTEX\0", 8) != 0
		|| Payload[10] != 0 || Payload[11] != 0
		|| !ReplaceJsonString(Header, TEXT("\"source\":{\"export\":\""), SourceExport)
		|| Header.ReplaceInline(TEXT("\"masked\":false"), TEXT("\"masked\":true"),
			ESearchCase::CaseSensitive) != 1)
		return {};
	Payload[10] = 1;
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> RetargetMainMenuLocalization(TConstArrayView<uint8> Object,
	FStringView SourceExport)
{
	FString Header;
	TArray<uint8> IgnoredPayload;
	if (!SplitObject(Object, Header, IgnoredPayload)
		|| !ReplaceJsonString(Header, TEXT("\"source\":{\"export\":\""),
			SourceExport)
		|| !ReplaceJsonUnsigned(Header, TEXT("\"entry_count\":"), 14)
		|| !ReplaceJsonUnsigned(Header, TEXT("\"section_count\":"), 4))
	{
		return {};
	}
	static const ANSICHAR MainMenu[] =
		"[menuMain]\r\n"
		"MenuList[2]=Multiplayer\r\n"
		"MenuList[3]=Controls\r\n"
		"MenuList[4]=Hardware\r\n"
		"MenuList[7]=Quit\r\n"
		"[menuStartArenaServer]\r\n"
		"MenuTitle=ARENA SERVER\r\n"
		"MenuList[1]=Select Map\r\n"
		"MenuList[2]=Number of Kills to Win\r\n"
		"MenuList[3]=Time Limit\r\n"
		"MenuList[5]=Player Count Limit\r\n"
		"MenuList[7]=Server Name\r\n"
		"[menuStartBattleServer]\r\n"
		"MenuTitle=CITADEL SERVER\r\n"
		"MenuList[6]=WWWWWWWWWWWW\r\n"
		"MenuList[7]=BBBBBBBBBBB\r\n"
		"[menuMultiPlayer]\r\nHelpMessage[1]=Join a network game.\r\n";
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>(MainMenu),
		static_cast<int32>(sizeof(MainMenu) - 1));
	if (!ReplacePayloadLength(Header, Payload.Num())) return {};
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> RetargetMesh(TConstArrayView<uint8> Object,
	FStringView SourceExport, FStringView Skin,
	EWoTFrontEndRainSkinFixture RainSkinFixture)
{
	FString Header;
	TArray<uint8> Payload;
	const bool bWrongIdentity = RainSkinFixture
		== EWoTFrontEndRainSkinFixture::ClassDefaultWrongIdentity;
	if (!SplitObject(Object, Header, Payload)
		|| !ReplaceJsonString(Header, TEXT("\"source\":{\"export\":\""),
			bWrongIdentity ? TEXTVIEW("Environment.NotRainCyd") : SourceExport)
		|| Header.ReplaceInline(
			TEXT("\"package\":\"Synthetic/mesh.u\""),
			TEXT("\"package\":\"System/Environment.u\""),
			ESearchCase::CaseSensitive) != 1)
		return {};
	if (RainSkinFixture != EWoTFrontEndRainSkinFixture::MeshLocal)
	{
		const FString OriginalSections =
			TEXT("\"sections\":[{\"first_triangle\":0,\"lod_attachment\":false,\"poly_flags\":0,\"skin_index\":0,\"triangle_count\":1},{\"first_triangle\":1,\"lod_attachment\":false,\"poly_flags\":258,\"skin_index\":1,\"triangle_count\":1},{\"first_triangle\":2,\"lod_attachment\":false,\"poly_flags\":4194320,\"skin_index\":0,\"triangle_count\":1}]");
		const FString OriginalSkins =
			TEXT("\"skins\":[{\"logical_id\":\"mesh__SkinA\",\"name\":\"SkinA\",\"package\":\"mesh\"},{\"logical_id\":\"mesh__SkinB\",\"name\":\"SkinB\",\"package\":\"mesh\"}]");
		if (!Header.Contains(OriginalSections, ESearchCase::CaseSensitive)
			|| !Header.Contains(OriginalSkins, ESearchCase::CaseSensitive)
			|| Payload.Num() < 104
			|| FMemory::Memcmp(Payload.GetData(), "WOTMSH3", 8) != 0)
			return {};
		auto ReadU32 = [&Payload](int32 Offset)
		{
			return static_cast<uint32>(Payload[Offset])
				| static_cast<uint32>(Payload[Offset + 1]) << 8
				| static_cast<uint32>(Payload[Offset + 2]) << 16
				| static_cast<uint32>(Payload[Offset + 3]) << 24;
		};
		auto WriteU32 = [&Payload](int32 Offset, uint32 Value)
		{
			for (int32 Byte = 0; Byte < 4; ++Byte)
				Payload[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
		};
		const uint32 VertexCount = ReadU32(12);
		const uint32 TriangleCount = ReadU32(16);
		const uint32 UVCount = ReadU32(20);
		const uint32 OriginalSectionCount = ReadU32(24);
		const uint64 TriangleBegin = 104ull + VertexCount * 4ull
			+ UVCount * 2ull;
		const uint64 SectionBegin = 104ull + VertexCount * 4ull
			+ UVCount * 2ull + TriangleCount * 56ull;
		const uint64 TailBegin = SectionBegin + OriginalSectionCount * 20ull;
		if (TriangleCount < 2 || OriginalSectionCount != 3
			|| TailBegin
				> static_cast<uint64>(Payload.Num()))
			return {};

		const bool bExtraSection = RainSkinFixture
			== EWoTFrontEndRainSkinFixture::ClassDefaultExtraSection;
		const bool bMultiSlot = RainSkinFixture
			== EWoTFrontEndRainSkinFixture::ClassDefaultMultiSlot;
		const uint32 SectionCount = bExtraSection ? 2u : 1u;
		const uint32 SkinCount = bMultiSlot ? 2u : 1u;
		const uint32 SkinIndex = RainSkinFixture
			== EWoTFrontEndRainSkinFixture::ClassDefaultWrongIndex ? 1u : 0u;
		for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
			WriteU32(static_cast<int32>(TriangleBegin + Triangle * 56ull + 36ull),
				bExtraSection && Triangle > 0 ? 1u : 0u);

		TArray<uint8> Rebuilt;
		Rebuilt.Append(Payload.GetData(), static_cast<int32>(SectionBegin));
		AppendU32(Rebuilt, 0); AppendU32(Rebuilt,
			bExtraSection ? 1u : TriangleCount);
		AppendU32(Rebuilt, 2); AppendU32(Rebuilt, 256); AppendU32(Rebuilt, SkinIndex);
		if (bExtraSection)
		{
			AppendU32(Rebuilt, 1); AppendU32(Rebuilt, TriangleCount - 1);
			AppendU32(Rebuilt, 2); AppendU32(Rebuilt, 256); AppendU32(Rebuilt, 0);
		}
		Rebuilt.Append(Payload.GetData() + TailBegin,
			Payload.Num() - static_cast<int32>(TailBegin));
		Payload = MoveTemp(Rebuilt);
		WriteU32(24, SectionCount);
		WriteU32(28, SkinCount);

		const FString Sections = bExtraSection
			? FString::Printf(TEXT("\"sections\":[{\"first_triangle\":0,\"lod_attachment\":false,\"poly_flags\":256,\"skin_index\":0,\"triangle_count\":1},{\"first_triangle\":1,\"lod_attachment\":false,\"poly_flags\":256,\"skin_index\":0,\"triangle_count\":%u}]"), TriangleCount - 1)
			: FString::Printf(TEXT("\"sections\":[{\"first_triangle\":0,\"lod_attachment\":false,\"poly_flags\":256,\"skin_index\":%u,\"triangle_count\":%u}]"), SkinIndex, TriangleCount);
		const FString Skins = bMultiSlot
			? TEXT("\"skins\":[{\"logical_id\":null,\"name\":null,\"package\":null},{\"logical_id\":null,\"name\":null,\"package\":null}]")
			: TEXT("\"skins\":[{\"logical_id\":null,\"name\":null,\"package\":null}]");
		Header.ReplaceInline(*OriginalSections, *Sections,
			ESearchCase::CaseSensitive);
		Header.ReplaceInline(*OriginalSkins, *Skins,
			ESearchCase::CaseSensitive);
		if (!ReplacePayloadLength(Header, Payload.Num())) return {};
		return WoTPayloadActivationTest_FrameObject(Header, Payload);
	}
	Header.ReplaceInline(TEXT("\"logical_id\":\"mesh__SkinA\""),
		*FString::Printf(TEXT("\"logical_id\":\"%s\""), *FString(Skin)));
	Header.ReplaceInline(TEXT("\"logical_id\":\"mesh__SkinB\""),
		*FString::Printf(TEXT("\"logical_id\":\"%s\""), *FString(Skin)));
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> RetargetFont(TConstArrayView<uint8> Object,
	FStringView FaceSourceExport, FStringView PageLogicalId,
	FStringView PageSourceIdentity, uint32 LineHeight = 0,
	bool bZeroWidthGlyph = false)
{
	FString Header;
	TArray<uint8> Payload;
	if (!SplitObject(Object, Header, Payload)
		|| !ReplaceLengthPrefixedString(Payload,
			TEXTVIEW("cache/texture/synthetic-atlas"), PageLogicalId)
		|| !ReplaceLengthPrefixedString(Payload,
			TEXTVIEW("remaining.Atlas"), PageSourceIdentity)
		|| !ReplacePayloadLength(Header, Payload.Num())) return {};
	if (bZeroWidthGlyph)
	{
		// The fixed fixture ends with sixteen 36-byte glyphs and no kerning.
		// Keep the last glyph's height and advance, but give it no bitmap width,
		// covering the legal empty-glyph case in the font format.
		if (Payload.Num() < 16 * 36) return {};
		FMemory::Memzero(Payload.GetData() + Payload.Num() - 36 + 16, 4);
	}
	if (LineHeight != 0)
	{
		if (Payload.Num() < 40) return {};
		for (int32 Shift = 0; Shift < 32; Shift += 8)
		{
			Payload[32 + Shift / 8] = static_cast<uint8>(LineHeight >> Shift);
			Payload[36 + Shift / 8] = static_cast<uint8>(LineHeight >> Shift);
		}
		Header.ReplaceInline(TEXT("\"baseline\":2"),
			*FString::Printf(TEXT("\"baseline\":%u"), LineHeight));
		Header.ReplaceInline(TEXT("\"line_height\":2"),
			*FString::Printf(TEXT("\"line_height\":%u"), LineHeight));
	}
	Header.ReplaceInline(TEXT("\"export\":\"remaining.FontA\""),
		*FString::Printf(TEXT("\"export\":\"%s\""),
			*FString(FaceSourceExport)));
	Header.ReplaceInline(TEXT("\"logical_id\":\"cache/texture/synthetic-atlas\""),
		*FString::Printf(TEXT("\"logical_id\":\"%s\""),
			*FString(PageLogicalId)));
	Header.ReplaceInline(TEXT("\"source_identity\":\"remaining.Atlas\""),
		*FString::Printf(TEXT("\"source_identity\":\"%s\""),
			*FString(PageSourceIdentity)));
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> FontAtlasObject(bool bPatterned = false)
{
	TArray<uint8> Payload = {'W','O','T','P','T','E','X',0};
	AppendU16(Payload, 1); AppendU16(Payload, 0);
	AppendU32(Payload, 256); AppendU32(Payload, 1);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const uint8 Channel = Index == 0 ? 0
			: bPatterned ? static_cast<uint8>(Index)
			: Index == 1 ? 64 : 255;
		Payload.Append({Channel, Channel, Channel, 255});
	}
	AppendU32(Payload, 8); AppendU32(Payload, 8); AppendU32(Payload, 64);
	for (int32 Index = 0; Index < 64; ++Index)
		Payload.Add(bPatterned ? static_cast<uint8>((Index * 7) % 63 + 1) : 1);
	const FString Header = FString::Printf(TEXT(
		"{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},"
		"\"decoded_layout\":{\"mip_count\":1,\"mips\":[{\"expanded_rgba_byte_length\":256,\"height\":8,\"index_byte_length\":64,\"width\":8}],\"texel_format\":\"p8\"},"
		"\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.frontend.atlas\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":false,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,"
		"\"source\":{\"export\":\"remaining.Atlas\",\"package\":\"Synthetic/remaining.u\",\"sha256\":\"0511f92abc5f2308a0de6aa429fbfd30e1ff802d40f0cf0cd18f7b128e145c56\"}}"), Payload.Num());
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

// Coordinate-coded atlas: crop/stitched brush tests can assert independent
// pixel values instead of accepting any non-null texture resource.
TArray<uint8> BrowserAtlasObject(const TCHAR* SourceExport, uint32 Side, bool bMasked)
{
	TArray<uint8> Payload = {'W','O','T','P','T','E','X',0};
	AppendU16(Payload, 1); AppendU16(Payload, bMasked ? 1 : 0);
	AppendU32(Payload, 256); AppendU32(Payload, 1);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		// BevelUpArea begins at Misc(8,20), whose coordinate-coded index is 69.
		// Give that fixture pixel a conspicuously non-grey tint so the production
		// FaceBackground sample cannot pass by retaining a literal Win95 grey.
		Payload.Append({static_cast<uint8>(Index),
			static_cast<uint8>(Index == 69 ? 137 : Index),
			static_cast<uint8>(Index == 69 ? 211 : Index), 255});
	}
	AppendU32(Payload, Side); AppendU32(Payload, Side); AppendU32(Payload, Side * Side);
	for (uint32 Y = 0; Y < Side; ++Y)
		for (uint32 X = 0; X < Side; ++X)
			Payload.Add(static_cast<uint8>((X + 3 * Y) % 255 + 1));
	const FString Header = FString::Printf(TEXT(
		"{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},"
		"\"decoded_layout\":{\"mip_count\":1,\"mips\":[{\"expanded_rgba_byte_length\":%u,\"height\":%u,\"index_byte_length\":%u,\"width\":%u}],\"texel_format\":\"p8\"},"
		"\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.browser.atlas\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":%s,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,"
		"\"source\":{\"export\":\"%s\",\"package\":\"Synthetic/browser.u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"),
		Side * Side * 4, Side, Side * Side, Side, bMasked ? TEXT("true") : TEXT("false"),
		Payload.Num(), SourceExport);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

// Synthetic indexed bytes identify every authored mip independently. Level zero
// is deliberately different so a runtime that regenerates/discards mips fails.
TArray<uint8> RainTextureObject()
{
	TArray<uint8> Payload = {'W','O','T','P','T','E','X',0};
	AppendU16(Payload, 1); AppendU16(Payload, 1);
	AppendU32(Payload, 256); AppendU32(Payload, 10);
	for (int32 Index = 0; Index < 256; ++Index)
		Payload.Append({static_cast<uint8>(Index), static_cast<uint8>(Index * 2),
			static_cast<uint8>(Index * 3), 255});
	FString Mips;
	for (uint32 Level = 0; Level < 10; ++Level)
	{
		const uint32 Side = 512u >> Level, Pixels = Side * Side;
		AppendU32(Payload, Side); AppendU32(Payload, Side); AppendU32(Payload, Pixels);
		for (uint32 Pixel = 0; Pixel < Pixels; ++Pixel)
			Payload.Add(Pixel == 0 && Side > 1 ? 0 : static_cast<uint8>(Level + 1));
		if (!Mips.IsEmpty()) Mips += TEXT(",");
		Mips += FString::Printf(TEXT("{\"expanded_rgba_byte_length\":%u,\"height\":%u,\"index_byte_length\":%u,\"width\":%u}"), Pixels * 4, Side, Pixels, Side);
	}
	const FString Header = FString::Printf(TEXT(
		"{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},"
		"\"decoded_layout\":{\"mip_count\":10,\"mips\":[%s],\"texel_format\":\"p8\"},"
		"\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.frontend.rain\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":true,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,"
		"\"source\":{\"export\":\"Environment.Skins.RainDropsA\",\"package\":\"System/Environment.u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"), *Mips, Payload.Num());
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

struct FEncodedProperty
{
	FString Name;
	uint8 Type = 0;
	uint32 ArrayIndex = 0;
	FString StructName;
	FString ObjectIdentity;
	FString ObjectClass;
	TArray<uint8> Value;
	FString NameValue;
};

void AppendProperty(TArray<uint8>& Out, const FEncodedProperty& Property)
{
	AppendString(Out, Property.Name); Out.Add(Property.Type);
	AppendU32(Out, Property.ArrayIndex); AppendString(Out, Property.StructName);
	AppendString(Out, Property.NameValue); AppendString(Out, Property.ObjectIdentity);
	AppendString(Out, Property.ObjectClass); AppendU32(Out, Property.Value.Num());
	Out.Append(Property.Value);
}

TArray<uint8> ParticleTemplate()
{
	TArray<uint8> Result;
	for (float Value : {1.0f, 0.0f, 10.0f, 1.0f, 1.0f, 0.5f, 1.0f, 0.5f})
		AppendFloat(Result, Value);
	Result.Add(0); AppendFloat(Result, 0.0f); AppendFloat(Result, 0.0f);
	Result.Add(0); AppendFloat(Result, 0.0f); AppendFloat(Result, 0.0f);
	check(Result.Num() == 50); return Result;
}

struct FActorRecord
{
	FString Identity;
	FString ClassIdentity;
	TArray<FEncodedProperty> Properties;
};

class FWeatherObjectCapture final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (Bytes.IsEmpty() || static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("real weather fixture importer emitted an invalid object")));
		}
		TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
		Copy.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}

	TMap<FString, TArray<uint8>> Objects;
};

class FWeatherImportProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

TArray<uint8> EntryWorldObject(FStringView WorldExport,
	TConstArrayView<FString> AmbientSounds, FStringView SplashTexture,
	FStringView ParticleTexture,
	FStringView BSPMaterial, bool bDuplicateLightningRoute,
	bool bThunderIndexGap, bool bThunderOverrideMismatch,
	bool bThunderOverrideRootMismatch, bool bThunderOverrideCaseMismatch,
	const FWoTFrontEndWeatherFixtureOptions* WeatherOptions)
{
	TArray<FActorRecord> Actors;
	auto Add = [&Actors](const TCHAR* Leaf, const TCHAR* ClassName, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
			Actors.Add({FString::Printf(TEXT("WheelOfTime.%s%d"), Leaf, Index),
				ClassName, {}});
	};
	Add(TEXT("Light"), TEXT("Engine.Light"), 39);
	const TCHAR* LightTags[] = {TEXT("LightningLight_20"),
		TEXT("LightningLight_30"), TEXT("LightningLight_31"),
		TEXT("LightningLight_32"), TEXT("LightningLight_34")};
	for (int32 Index = 0; Index < 5; ++Index)
		Actors[Index].Properties.Add({TEXT("Tag"), 6, 0, {}, {}, {}, {},
			LightTags[Index]});
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FActorRecord& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = FString::Printf(TEXT("WheelOfTime.AmbientSound%d"), Index);
		Actor.ClassIdentity = TEXT("Engine.AmbientSound");
		Actor.Properties.Add({TEXT("AmbientSound"), 5, 0, {},
			AmbientSounds[Index], TEXT("Engine.Sound"), {}});
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FActorRecord& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = FString::Printf(TEXT("WheelOfTime.ParticleSprayer%d"), Index);
		Actor.ClassIdentity = TEXT("WOT.ParticleSprayer");
		Actor.Properties.Add({TEXT("NumTemplates"), 1, 0, {}, {}, {}, {1}});
		Actor.Properties.Add({TEXT("Particles"), 5, 0, {}, FString(ParticleTexture),
			TEXT("Engine.Texture"), {}});
		Actor.Properties.Add({TEXT("Templates"), 10, 0, TEXT("Particle"), {}, {},
			ParticleTemplate()});
	}
	const int32 RainActorBegin = Actors.Num();
	Add(TEXT("RainCyd"), TEXT("WOT.RainCyd"), 6);
	if (WeatherOptions != nullptr)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TArray<uint8> ScaleGlow;
			AppendFloat(ScaleGlow, WeatherOptions->RainScaleGlow[Index]);
			if (!WeatherOptions->bDefaultRainGlow)
				Actors[RainActorBegin + Index].Properties.Add(
					{TEXT("ScaleGlow"), static_cast<uint8>(WeatherOptions->bMalformedRainScaleGlow ? 2 : 4), 0, {}, {}, {}, MoveTemp(ScaleGlow)});
			if (WeatherOptions->bMalformedRainAmbientGlow)
				Actors[RainActorBegin + Index].Properties.Add(
					{TEXT("AmbientGlow"), 4, 0, {}, {}, {}, {0,0,0,0}});
			TArray<uint8> Location;
			AppendFloat(Location, static_cast<float>(Index * 10));
			AppendFloat(Location, 0.0f);
			AppendFloat(Location, 50.0f);
			Actors[RainActorBegin + Index].Properties.Add(
				{TEXT("Location"), 10, 0, TEXT("Vector"), {}, {},
					MoveTemp(Location)});
		}
	}
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FActorRecord& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = FString::Printf(TEXT("WheelOfTime.Lightning%d"), Index);
		Actor.ClassIdentity = TEXT("WOT.Lightning");
		Actor.Properties.Add({TEXT("Tag"), 6, 0, {}, {}, {}, {},
			TEXT("Lightning01")});
		Actor.Properties.Add({TEXT("LightTag"), 6, 0, {}, {}, {}, {},
			bDuplicateLightningRoute && Index == 4 ? LightTags[0] : LightTags[Index]});
		TArray<uint8> Radius; AppendFloat(Radius, Index == 0 ? 3000.0f : 2048.0f);
		Actor.Properties.Add({TEXT("ThunderRadius"), 4, 0, {}, {}, {}, Radius});
		Actor.Properties.Add({TEXT("SoundVolume"), 1, 0, {}, {}, {}, {96}});
		if (WeatherOptions && WeatherOptions->bMalformedLightningLength)
			Actor.Properties.Add({TEXT("LightningLength"), 1, 0, {}, {}, {}, {1}});
	}
	{
		FActorRecord& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = TEXT("WheelOfTime.RandomLightning0");
		Actor.ClassIdentity = TEXT("WOT.RandomLightning");
		Actor.Properties.Add({TEXT("Event"), 6, 0, {}, {}, {}, {},
			TEXT("Lightning01")});
		for (const TPair<const TCHAR*, float>& Timing : {
			TPair<const TCHAR*, float>(TEXT("MinLightningTime"), 10.0f),
			TPair<const TCHAR*, float>(TEXT("MaxLightningTime"), 20.0f),
			TPair<const TCHAR*, float>(TEXT("MinThunderDelay"), 0.0f),
			TPair<const TCHAR*, float>(TEXT("MaxThunderDelay"), 3.0f)})
		{
			TArray<uint8> Bytes; AppendFloat(Bytes, Timing.Value);
			Actor.Properties.Add({Timing.Key, 4, 0, {}, {}, {}, MoveTemp(Bytes)});
		}
		// Genuine WheelOfTime.wot stores only the RandomLightning instance's
		// string overrides at indices 6..9. Indices 0..5 live in the
		// Environment.RandomLightning class defaults and are authenticated by the
		// front-end descriptor, not repeated as object properties in the world.
		const TCHAR* ThunderOverrides[] = {TEXT("Environment.Thunder8"),
			TEXT("Environment.Thunder2"), TEXT("Environment.Thunder3"),
			TEXT("Environment.Thunder10")};
		for (uint32 Index = 0; Index < UE_ARRAY_COUNT(ThunderOverrides); ++Index)
		{
			TArray<uint8> Encoded;
			const TCHAR* Value = bThunderOverrideCaseMismatch && Index == 3
				? TEXT("Environment.thunder10")
				: bThunderOverrideRootMismatch && Index == 3
				? TEXT("BogusRoot.Thunder10")
				: bThunderOverrideMismatch && Index == 3
					? TEXT("Environment.Thunder9") : ThunderOverrides[Index];
			const FTCHARToUTF8 Utf8(Value);
			Encoded.Add(static_cast<uint8>(Utf8.Length() + 1));
			Encoded.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			Encoded.Add(0);
			Actor.Properties.Add({TEXT("ThunderSounds"), 13,
				bThunderIndexGap && Index == 3 ? 10u : Index + 6u, {}, {}, {},
				MoveTemp(Encoded)});
		}
	}
	if (WeatherOptions != nullptr)
	{
		FActorRecord& Level = Actors.AddDefaulted_GetRef();
		Level.Identity = TEXT("WheelOfTime.LevelInfo0");
		Level.ClassIdentity = TEXT("Engine.LevelInfo");
		FActorRecord& WorldZone = Actors.AddDefaulted_GetRef();
		WorldZone.Identity = TEXT("WheelOfTime.WOTZoneInfo0");
		WorldZone.ClassIdentity = TEXT("WOT.WOTZoneInfo");
		FActorRecord& Sky = Actors.AddDefaulted_GetRef();
		Sky.Identity = TEXT("WheelOfTime.SkyZoneInfo0");
		Sky.ClassIdentity = TEXT("Engine.SkyZoneInfo");
		TArray<uint8> Location;
		AppendFloat(Location, WeatherOptions->bSkyStyleCoverage ? 10.f : 0.f);
		AppendFloat(Location, WeatherOptions->bSkyStyleCoverage ? 20.f : 0.f);
		AppendFloat(Location, WeatherOptions->bSkyStyleCoverage ? 30.f : 0.f);
		Sky.Properties.Add({TEXT("Location"), 10, 0, TEXT("Vector"), {}, {},
			MoveTemp(Location)});
		TArray<uint8> Rotation;
		AppendU32(Rotation, 0);
		AppendU32(Rotation, 16384);
		AppendU32(Rotation, 0);
		Sky.Properties.Add({TEXT("Rotation"), 10, 0, TEXT("Rotator"), {}, {},
			MoveTemp(Rotation)});
	}
	for (FActorRecord& Actor : Actors)
		Actor.Properties.Sort([](const FEncodedProperty& Left,
			const FEncodedProperty& Right)
		{
			const int32 NameOrder = Left.Name.Compare(
				Right.Name, ESearchCase::IgnoreCase);
			return NameOrder != 0 ? NameOrder < 0
				: Left.ArrayIndex < Right.ArrayIndex;
		});

	TArray<TPair<FString, FString>> Semantics;
	for (const FActorRecord& Actor : Actors)
		Semantics.Add({Actor.Identity, Actor.ClassIdentity});
	for (const FString& Sound : AmbientSounds)
		Semantics.Add({Sound, TEXT("Engine.Sound")});
	for (const TCHAR* Thunder : {TEXT("Environment.Thunder.Thunder9"),
		TEXT("Environment.Thunder.Thunder3"), TEXT("Environment.Thunder.Thunder10"),
		TEXT("Environment.Thunder.Thunder2"), TEXT("Environment.Thunder.Thunder4"),
		TEXT("Environment.Thunder.Thunder8")})
		Semantics.Add({Thunder, TEXT("Engine.Sound")});
	Semantics.Add({FString(SplashTexture), TEXT("Engine.Texture")});
	if (ParticleTexture != SplashTexture)
		Semantics.Add({FString(ParticleTexture), TEXT("Engine.Texture")});
	Semantics.Add({FString(BSPMaterial), TEXT("Engine.Texture")});
	if (WeatherOptions && !WeatherOptions->WorldTextureOverride.IsEmpty()
		&& WeatherOptions->WorldTextureOverride != BSPMaterial
		&& WeatherOptions->WorldTextureOverride != SplashTexture)
		Semantics.Add({WeatherOptions->WorldTextureOverride, TEXT("Engine.Texture")});

	const bool bWeather = WeatherOptions != nullptr;
	const uint32 SurfaceCount = bWeather
		? (WeatherOptions->bMissingGround ? 3u : 4u) : 1u;
	const uint32 NodeCount = bWeather ? SurfaceCount : 0u;
	const uint32 PointCount = bWeather ? SurfaceCount * 3u : 1u;
	const uint32 VertexCount = bWeather ? SurfaceCount * 3u : 0u;
	const uint32 ZoneCount = bWeather ? 3u : 0u;
	TArray<uint8> Payload = {'W','O','T','W','R','L','3',0};
	AppendU16(Payload, 2); AppendU16(Payload, 0);
	for (uint32 Count : {static_cast<uint32>(Actors.Num()), 0u, 3u, PointCount,
		NodeCount, SurfaceCount, VertexCount, ZoneCount,
		static_cast<uint32>(Semantics.Num()), 0u})
		AppendU32(Payload, Count);
	for (const FVector3f& Vector : {FVector3f::ZAxisVector,
		FVector3f::XAxisVector, FVector3f::YAxisVector})
	{
		AppendFloat(Payload, Vector.X); AppendFloat(Payload, Vector.Y);
		AppendFloat(Payload, Vector.Z);
	}
	if (bWeather)
	{
		const TArray<FVector3f> Points = {
			FVector3f(-5, -5, 10), FVector3f(5, -5, 10), FVector3f(0, 5, 10),
			FVector3f(-100, -100, 100), FVector3f(100, -100, 100),
			FVector3f(0, 100, 100),
			FVector3f(-2, -2, 5), FVector3f(2, -2, 5), FVector3f(0, 2, 5),
			FVector3f(-100, -100, -20), FVector3f(100, -100, -20),
			FVector3f(0, 100, -20)};
		for (uint32 Index = 0; Index < PointCount; ++Index)
		{
			AppendFloat(Payload, Points[Index].X);
			AppendFloat(Payload, Points[Index].Y);
			AppendFloat(Payload, Points[Index].Z);
		}
		for (uint32 Index = 0; Index < NodeCount; ++Index)
		{
			AppendFloat(Payload, 0.0f); AppendFloat(Payload, 0.0f);
			AppendFloat(Payload, 1.0f); AppendFloat(Payload, Index == 3 ? -20.0f : 100.0f);
			AppendU64(Payload, 0);
			Payload.Add(Index == 3 ? 0 : 1); // Only the final node is solid CSG.
			AppendU32(Payload, Index * 3); AppendU32(Payload, Index);
			for (int32 Child = 0; Child < 5; ++Child)
				AppendU32(Payload, Child == 0 && Index + 1 < NodeCount
					? Index + 1 : static_cast<uint32>(INDEX_NONE));
			const uint8 Zone = Index == 1 || (WeatherOptions->bSkyStyleCoverage && Index == 2) ? 2 : 1;
			Payload.Add(Zone); Payload.Add(Zone); Payload.Add(3);
			AppendU32(Payload, static_cast<uint32>(INDEX_NONE));
			AppendU32(Payload, static_cast<uint32>(INDEX_NONE));
		}
		const FString SkyTexture = WeatherOptions->bSharedSkyTexture
			? FString(BSPMaterial) : FString(SplashTexture);
		for (uint32 Index = 0; Index < SurfaceCount; ++Index)
		{
			const int32 SurfaceFields[] = {
				static_cast<int32>(Index * 3), 0, 1, 2,
				INDEX_NONE, INDEX_NONE};
			for (int32 Value : SurfaceFields)
			{
				AppendU32(Payload, static_cast<uint32>(Value));
			}
			uint32 Flags = 0;
			if (Index == 1) Flags = 0x00000200u | 0x00000400u | 0x00000100u;
			else if (Index == 2) Flags = 0x00000080u;
			if (WeatherOptions->bSkyStyleCoverage && (Index == 1 || Index == 2))
				Flags = 0x00000200u | 0x00000400u | 0x00000100u | 0x00000004u | 0x00000040u | 0x00400000u;
			if (WeatherOptions->bWorldSkyBlendCoverage)
				Flags = Index == 1 ? 0x00000108u
					: Index == 2 ? 0x00000100u
					: Index == 3 ? 0x00000040u : Flags;
			AppendU32(Payload, Flags);
			AppendU16(Payload, 0); AppendU16(Payload, 0);
			AppendString(Payload, Index == 1 || (WeatherOptions->bSkyStyleCoverage && Index == 2)
				|| (WeatherOptions->bWorldSkyBlendCoverage && (Index == 2 || Index == 3)) ? FStringView(SkyTexture)
				: Index == 0 && !WeatherOptions->WorldTextureOverride.IsEmpty()
				? FStringView(WeatherOptions->WorldTextureOverride) : FStringView(BSPMaterial));
			AppendString(Payload, FStringView());
		}
		for (uint32 Index = 0; Index < VertexCount; ++Index)
		{
			AppendU32(Payload, Index);
			AppendU32(Payload, static_cast<uint32>(INDEX_NONE));
		}
		AppendString(Payload, FStringView());
		AppendU64(Payload, 1ull); AppendU64(Payload, 1ull);
		AppendString(Payload, TEXT("WheelOfTime.WOTZoneInfo0"));
		AppendU64(Payload, 1ull << 1); AppendU64(Payload, 1ull << 1);
		AppendString(Payload, TEXT("WheelOfTime.SkyZoneInfo0"));
		AppendU64(Payload, 1ull << 2); AppendU64(Payload, 1ull << 2);
	}
	else
	{
		AppendFloat(Payload, 0.0f); AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 0.0f);
		const int32 SurfaceIndices[] = {0, 0, 1, 2, INDEX_NONE, INDEX_NONE};
		for (int32 Value : SurfaceIndices)
			AppendU32(Payload, static_cast<uint32>(Value));
		AppendU32(Payload, 0); AppendU16(Payload, 0); AppendU16(Payload, 0);
		AppendString(Payload, BSPMaterial); AppendString(Payload, FStringView());
	}
	for (int32 Index = 0; Index < Semantics.Num(); ++Index)
	{
		AppendString(Payload, Semantics[Index].Key);
		AppendString(Payload, Semantics[Index].Value);
		Payload.Add(Index < Actors.Num() ? 1 : 0);
	}
	for (const FActorRecord& Actor : Actors)
	{
		AppendString(Payload, Actor.Identity); AppendString(Payload, Actor.ClassIdentity);
		AppendU32(Payload, Actor.Properties.Num());
		for (const FEncodedProperty& Property : Actor.Properties)
			AppendProperty(Payload, Property);
		AppendString(Payload, FStringView());
	}
	const FString Header = FString::Printf(TEXT(
		"{\"actor_count\":%d,\"attachment_count\":0,\"bsp_node_count\":%u,\"bsp_surface_count\":%u,"
		"\"encoding\":\"ue1-world-records-le-v2\",\"fact_id\":\"spec.frontend.world\",\"importer\":\"world-v1\",\"importer_version\":2,\"kind\":\"world\",\"light_count\":39,"
		"\"map_identity\":\"WheelOfTime\",\"mover_count\":0,\"navigation_seed_count\":0,\"payload_byte_length\":%d,\"portal_surface_count\":0,\"reach_spec_count\":0,"
		"\"semantic_object_count\":%d,\"sound_count\":5,\"source\":{\"export\":\"%s\",\"package\":\"Maps/WheelOfTime.wot\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"zone_count\":%u}"),
		Actors.Num(), NodeCount, SurfaceCount, Payload.Num(), Semantics.Num(),
		*FString(WorldExport), ZoneCount);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> EntryBSPObject(FStringView BSPExport, FStringView MaterialSlot,
	bool bIncludeLightmap, FStringView AdditionalMaterialSlot)
{
	TArray<FStringView> MaterialSlots = {MaterialSlot};
	if (!AdditionalMaterialSlot.IsEmpty()) MaterialSlots.Add(AdditionalMaterialSlot);
	TArray<uint8> Payload = {'W','O','T','P','B','S','P',0};
	AppendU16(Payload, bIncludeLightmap ? 2 : 1); AppendU16(Payload, 2);
	AppendU32(Payload, 3); AppendU32(Payload, MaterialSlots.Num() * 3);
	AppendU32(Payload, MaterialSlots.Num());
	AppendU16(Payload, 2); AppendU16(Payload, 1);
	for (float Bound : {0.f,0.f,0.f,100.f,100.f,0.f}) AppendFloat(Payload, Bound);
	for (int32 Index = 0; Index < MaterialSlots.Num(); ++Index)
	{
		const FStringView Material = MaterialSlots[Index];
		const FTCHARToUTF8 Slot(Material.GetData(), Material.Len());
		AppendU16(Payload, Slot.Length()); AppendU16(Payload, 0);
		AppendU32(Payload, Index * 3); AppendU32(Payload, 3);
		AppendU32(Payload, 0); AppendU32(Payload, 2);
		Payload.Add(1); Payload.AddZeroed(3);
		Payload.Append(reinterpret_cast<const uint8*>(Slot.Get()), Slot.Length());
	}
	for (const FVector3f& P : {FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(0,100,0)})
		{ AppendFloat(Payload,P.X); AppendFloat(Payload,P.Y); AppendFloat(Payload,P.Z); }
	for (int32 Index=0; Index<3; ++Index)
		{ AppendFloat(Payload,0); AppendFloat(Payload,0); AppendFloat(Payload,1); }
	for (int32 Channel=0; Channel<2; ++Channel)
		for (const FVector2f& UV : {FVector2f(0,0),FVector2f(1,0),FVector2f(0,1)})
			{ AppendFloat(Payload,UV.X); AppendFloat(Payload,UV.Y); }
	for (int32 Index = 0; Index < MaterialSlots.Num(); ++Index)
	{
		AppendU32(Payload,0); AppendU32(Payload,1); AppendU32(Payload,2);
	}
	if (bIncludeLightmap)
	{
		AppendU32(Payload, 2); AppendU32(Payload, 2); AppendU32(Payload, 16);
		Payload.Append({
			1, 2, 3, 255,
			4, 5, 6, 255,
			7, 8, 9, 255,
			10, 11, 12, 255
		});
	}
	const FString Header = FString::Printf(TEXT(
		"{\"bounds_max_bits\":[\"42c80000\",\"42c80000\",\"00000000\"],\"bounds_min_bits\":[\"00000000\",\"00000000\",\"00000000\"],"
		"\"encoding\":\"wot-bsp-static-mesh-v1\",\"fact_id\":\"spec.frontend.bsp\",\"importer\":\"bsp-v1\",\"importer_version\":2,\"index_count\":%d,\"kind\":\"bsp\","
		"\"payload_byte_length\":%d,\"role\":\"main\",\"section_count\":%d,\"source\":{\"export\":\"%s\",\"package\":\"Maps/WheelOfTime.wot\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
		"\"tangent_algorithm\":\"ue-mikk-preserve-normals-v1\",\"triangle_count\":%d,\"uv_channels\":2,\"vertex_count\":3}"),
		MaterialSlots.Num() * 3, Payload.Num(), MaterialSlots.Num(),
		*FString(BSPExport), MaterialSlots.Num());
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> EntryWeatherBSPObject(FStringView BSPExport,
	FStringView MaterialSlot, const FWoTFrontEndWeatherFixtureOptions& Options,
	FStringView WaterSlot, FStringView MaterialSource, FStringView WaterSource)
{
	const uint32 TriangleCount = Options.bMissingGround ? 3u : 4u;
	const uint32 VertexCount = TriangleCount * 3u;
	const uint32 IndexCount = VertexCount;
	TArray<uint8> Payload = {'W','O','T','P','B','S','P',0};
	AppendU16(Payload, Options.bLegacyV2 ? 2 : Options.bBSPV4 ? 4 : 3);
	AppendU16(Payload, Options.bBSPV4 ? 14 : 2);
	AppendU32(Payload, VertexCount);
	AppendU32(Payload, IndexCount);
	AppendU32(Payload, Options.bAdditionalWaterSection ? 2 : 1);
	AppendU16(Payload, 2);
	AppendU16(Payload, 1);
	for (float Bound : {-100.f, -100.f, Options.bMissingGround ? 5.f : -20.f,
		100.f, 100.f, 100.f})
	{
		AppendFloat(Payload, Bound);
	}
	const FTCHARToUTF8 Slot(MaterialSlot.GetData(), MaterialSlot.Len());
	AppendU16(Payload, static_cast<uint16>(Slot.Length()));
	AppendU16(Payload, 0);
	AppendU32(Payload, 0);
	AppendU32(Payload, Options.bAdditionalWaterSection ? 9 : IndexCount);
	AppendU32(Payload, 0);
	AppendU32(Payload, Options.bAdditionalWaterSection ? 8 : VertexCount - 1);
	Payload.Add(1);
	Payload.AddZeroed(3);
	Payload.Append(reinterpret_cast<const uint8*>(Slot.Get()), Slot.Length());
	auto AppendV4Identity = [&Payload](FStringView Source)
	{
		const FString Identity(Source);
		int32 FirstDot = INDEX_NONE, LastDot = INDEX_NONE;
		check(Identity.FindChar(TEXT('.'), FirstDot)
			&& Identity.FindLastChar(TEXT('.'), LastDot));
		const FString Package = Identity.Left(FirstDot);
		const FString Name = Identity.Mid(LastDot + 1);
		const FTCHARToUTF8 PackageUtf8(*Package), NameUtf8(*Name), SourceUtf8(*Identity);
		AppendU16(Payload, PackageUtf8.Length()); AppendU16(Payload, NameUtf8.Length());
		AppendU16(Payload, SourceUtf8.Length()); AppendU16(Payload, 0);
		AppendU32(Payload, 0);
		Payload.Append(reinterpret_cast<const uint8*>(PackageUtf8.Get()), PackageUtf8.Length());
		Payload.Append(reinterpret_cast<const uint8*>(NameUtf8.Get()), NameUtf8.Length());
		Payload.Append(reinterpret_cast<const uint8*>(SourceUtf8.Get()), SourceUtf8.Length());
	};
	if (Options.bBSPV4) AppendV4Identity(MaterialSource);
	if (Options.bAdditionalWaterSection)
	{
		const FTCHARToUTF8 Water(WaterSlot.GetData(), WaterSlot.Len());
		AppendU16(Payload, Water.Length()); AppendU16(Payload, 0);
		AppendU32(Payload, 9); AppendU32(Payload, 3);
		AppendU32(Payload, 9); AppendU32(Payload, 11);
		Payload.Add(1); Payload.AddZeroed(3);
		Payload.Append(reinterpret_cast<const uint8*>(Water.Get()), Water.Length());
		if (Options.bBSPV4) AppendV4Identity(WaterSource);
	}

	const TArray<FVector3f> Positions = {
		FVector3f(-5, -5, 10), FVector3f(5, -5, 10), FVector3f(0, 5, 10),
		FVector3f(-100, -100, 100), FVector3f(100, -100, 100),
		FVector3f(0, 100, 100),
		FVector3f(-2, -2, 5), FVector3f(2, -2, 5), FVector3f(0, 2, 5),
		FVector3f(-100, -100, -20), FVector3f(100, -100, -20),
		FVector3f(0, 100, -20)};
	for (uint32 Index = 0; Index < VertexCount; ++Index)
	{
		AppendFloat(Payload, Positions[Index].X);
		AppendFloat(Payload, Positions[Index].Y);
		AppendFloat(Payload, Positions[Index].Z);
	}
	for (uint32 Index = 0; Index < VertexCount; ++Index)
	{
		AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 1.0f);
	}
	const TArray<FVector2f> UV0 = {
		{0.125f, 0.25f}, {0.625f, 0.25f}, {0.125f, 0.75f},
		{0.2f, 0.3f}, {0.8f, 0.3f}, {0.5f, 0.9f},
		{0.15f, 0.16f}, {0.25f, 0.26f}, {0.35f, 0.36f},
		{0.4f, 0.41f}, {0.5f, 0.51f}, {0.6f, 0.61f}};
	const TArray<FVector2f> UV1 = {
		{0.01f, 0.02f}, {0.03f, 0.04f}, {0.05f, 0.06f},
		{0.11f, 0.12f}, {0.21f, 0.22f}, {0.31f, 0.32f},
		{0.41f, 0.42f}, {0.51f, 0.52f}, {0.61f, 0.62f},
		{0.71f, 0.72f}, {0.81f, 0.82f}, {0.91f, 0.92f}};
	for (const TArray<FVector2f>* Stream : {&UV0, &UV1})
	{
		for (uint32 Index = 0; Index < VertexCount; ++Index)
		{
			const bool bNeutral = Options.bSkyStyleCoverage && Stream == &UV1 && Index >= 3 && Index < 9;
			AppendFloat(Payload, bNeutral ? .875f : (*Stream)[Index].X);
			AppendFloat(Payload, bNeutral ? .875f : (*Stream)[Index].Y);
		}
	}
	for (uint32 Index = 0; Index < IndexCount; ++Index)
		AppendU32(Payload, Index);
	AppendU32(Payload, 4);
	AppendU32(Payload, 4);
	AppendU32(Payload, 64);
	for (int32 Pixel = 0; Pixel < 16; ++Pixel)
		if (Options.bSkyStyleCoverage && Pixel == 15) Payload.Append({128,128,128,255});
		else Payload.Append({20, 40, 60, 255});
	if (!Options.bLegacyV2)
	{
		const int32 TrailerOffset = Payload.Num();
		AppendU32(Payload, 0);
		AppendU32(Payload, TriangleCount);
		AppendU32(Payload, TriangleCount);
		AppendU32(Payload, 5);
		AppendU32(Payload, 5);
		AppendU32(Payload, 1);
		for (uint32 Surface = 0; Surface < TriangleCount; ++Surface)
			AppendU32(Payload, Surface);
		for (uint32 Surface = 0; Surface < TriangleCount; ++Surface)
		{
			AppendU32(Payload, Surface);
			for (int32 Value = 0; Value < 4; ++Value)
				AppendU32(Payload, Surface == 0 ? 1 : 0);
			AppendFloat(Payload, Surface == 1 ? 0.01f : Options.bSkyStyleCoverage && Surface == 2 ? .03f : 0.0f);
			AppendFloat(Payload, Surface == 1 ? -0.02f : Options.bSkyStyleCoverage && Surface == 2 ? -.04f : 0.0f);
		}
		for (int32 Light = 0; Light < 5; ++Light)
		{
			const FTCHARToUTF8 Identity(*FString::Printf(TEXT("WheelOfTime.Light%d"), Light));
			AppendU16(Payload, Identity.Length()); AppendU16(Payload, 0);
			Payload.Append(reinterpret_cast<const uint8*>(Identity.Get()), Identity.Length());
			Payload.Append({static_cast<uint8>(Options.bMismatchedLightMetadata ? 65 : 64), 0, 255, 0}); AppendFloat(Payload, 1.0f);
		}
		for (uint32 Light = 0; Light < 5; ++Light)
		{
			AppendU32(Payload, Light); AppendU32(Payload, 0); AppendU32(Payload, 1);
			Payload.Add(127);
		}
		const uint32 TrailerBytes = static_cast<uint32>(Payload.Num() - TrailerOffset);
		for (int32 Byte = 0; Byte < 4; ++Byte)
			Payload[TrailerOffset + Byte] = static_cast<uint8>(TrailerBytes >> (Byte * 8));
	}
	const FString Header = FString::Printf(TEXT(
		"{\"bounds_max_bits\":[\"42c80000\",\"42c80000\",\"42c80000\"],"
		"\"bounds_min_bits\":[\"c2c80000\",\"c2c80000\",\"%s\"],"
		"\"encoding\":\"wot-bsp-static-mesh-v1\",\"fact_id\":\"spec.frontend.bsp\","
		"\"importer\":\"bsp-v1\",\"importer_version\":2,\"index_count\":%u,"
		"\"kind\":\"bsp\",\"payload_byte_length\":%d,\"role\":\"main\","
		"\"section_count\":%u,\"source\":{\"export\":\"%s\","
		"\"package\":\"Maps/WheelOfTime.wot\","
		"\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
		"\"tangent_algorithm\":\"ue-mikk-preserve-normals-v1\","
		"\"triangle_count\":%u,\"uv_channels\":2,\"vertex_count\":%u}"),
		Options.bMissingGround ? TEXT("40a00000") : TEXT("c1a00000"),
		IndexCount, Payload.Num(), Options.bAdditionalWaterSection ? 2u : 1u,
		*FString(BSPExport), TriangleCount, VertexCount);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

FString EntryConverterSlotBase(FStringView SourceExport)
{
	const FString Source(SourceExport);
	int32 FirstDot = INDEX_NONE;
	int32 LastDot = INDEX_NONE;
	if (!Source.FindChar(TEXT('.'), FirstDot)
		|| !Source.FindLastChar(TEXT('.'), LastDot)
		|| FirstDot <= 0 || LastDot + 1 >= Source.Len())
	{
		return {};
	}
	return Source.Left(FirstDot) + TEXT("__") + Source.Mid(LastDot + 1);
}

const FWoTRequiredFact* FactFor(const FWoTRetailContract& Contract,
	FStringView LogicalId, const FWoTRequiredFactOutput*& OutOutput)
{
	for (const FWoTRequiredFact& Fact : Contract.Facts)
		for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
			if (Output.LogicalId == LogicalId)
				{ OutOutput = &Output; return &Fact; }
	return nullptr;
}

} // namespace

const FWoTFrontEndDescriptor*
WoTWorldMaterializerTest_CreateFrontEndAuthority(FString& OutError)
{
	auto Production = WoTLoadProductionRetailContract();
	if (Production.IsError())
	{
		OutError = MoveTemp(Production).GetError().ToString();
		return nullptr;
	}
	auto Descriptor = WoTFrontEndTest_MakeDescriptor(Production.GetValue());
	if (Descriptor.IsError())
	{
		OutError = MoveTemp(Descriptor).GetError().ToString();
		return nullptr;
	}
	return new FWoTFrontEndDescriptor(MoveTemp(Descriptor).GetValue());
}

void WoTWorldMaterializerTest_DestroyFrontEndAuthority(
	const FWoTFrontEndDescriptor* Descriptor)
{
	delete Descriptor;
}

const FString& WoTFrontEndTest_WorldLogicalId(
	const FWoTFrontEndDescriptor& Descriptor)
{
	return Descriptor.WorldLogicalId;
}

const FString& WoTFrontEndTest_BSPLogicalId(
	const FWoTFrontEndDescriptor& Descriptor)
{
	return Descriptor.BspLogicalId;
}

const FWoTSha256& WoTFrontEndTest_DescriptorRoot(
	const FWoTFrontEndDescriptor& Descriptor)
{
	return Descriptor.DescriptorRoot;
}

TWoTBootstrapResult<TArray<uint8>> WoTFrontEndTest_WriteDescriptorSidecar(
	const FWoTFrontEndDescriptor& Descriptor,
	const FWoTSha256& InstallerSha256, const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256)
{
	return WriteSealedFrontEndDescriptorSidecar(Descriptor, InstallerSha256,
		ContractSha256, ProductionDescriptorSha256);
}

FWoTFrontEndStageProductionFixture::FWoTFrontEndStageProductionFixture() = default;
FWoTFrontEndStageProductionFixture::~FWoTFrontEndStageProductionFixture()
{
	WoTFrontEndTest_DestroyProductionStageFixture(*this);
}

static bool BuildProductionStageFixtureImpl(
	FWoTFrontEndStageProductionFixture& Out, bool bCorruptBSP, FString& OutError,
	bool bDuplicateLightningRoute, bool bThunderIndexGap,
	bool bThunderOverrideMismatch, bool bThunderOverrideRootMismatch,
	bool bThunderOverrideCaseMismatch, FStringView BSPMaterialSlotOverride,
	EWoTFrontEndRainSkinFixture RainSkinFixture, bool bMissingLightmap,
	bool bIncludeDefaultWaterSection,
	const FWoTFrontEndWeatherFixtureOptions* WeatherOptions,
	const TMap<FString, TArray<uint8>>* ObjectOverrides)
{
	auto Fail = [&OutError](FWoTBootstrapError&& Error)
		{ OutError=Error.ToString(); return false; };
	auto ContractResult=WoTLoadProductionRetailContract();
	if (ContractResult.IsError()) return Fail(MoveTemp(ContractResult).GetError());
	FWoTRetailContract Contract=MoveTemp(ContractResult).GetValue();
	auto DescriptorResult=WoTFrontEndTest_MakeDescriptor(Contract);
	if (DescriptorResult.IsError()) return Fail(MoveTemp(DescriptorResult).GetError());
	Out.Descriptor=MoveTemp(DescriptorResult).GetValue();
	FWoTFrontEndWeatherFixtureOptions EffectiveWeather = WeatherOptions
		? *WeatherOptions : FWoTFrontEndWeatherFixtureOptions{};
	EffectiveWeather.bAdditionalWaterSection = bIncludeDefaultWaterSection;
	if (bIncludeDefaultWaterSection)
		for (const auto& Resource : Out.Descriptor.RequiredResources)
			if (Resource.Domain == EWoTFactDomain::Texture
				&& FString(BSPMaterialSlotOverride).StartsWith(EntryConverterSlotBase(Resource.SourceExport)))
				EffectiveWeather.WorldTextureOverride = Resource.SourceExport;
	WeatherOptions = &EffectiveWeather;
	Out.CacheRoot=FPaths::Combine(FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir()), TEXT("WoTFrontEndStage_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT("ContentCache"));

	TArray<uint8> Texture, Sound, Mesh, Procedural, Font, Animation, Localization;
	if (!ReadFixture(TEXT("cache-v3-texture-opaque.bin"),Texture)
		|| !ReadFixture(TEXT("cache-v3-sound-pcm8-stereo.bin"),Sound)
		|| !ReadFixture(TEXT("cache-v3-mesh-plain.bin"),Mesh)
		|| !ReadFixture(TEXT("cache-v3-procedural-fire.bin"),Procedural)
		|| !ReadFixture(TEXT("cache-v3-font-pages.bin"),Font)
		|| !ReadFixture(TEXT("cache-v3-animation-plain.bin"),Animation)
		|| !ReadFixture(TEXT("cache-v3-localization-table.bin"),Localization))
		{ OutError=TEXT("canonical front-end object fixture is absent"); return false; }
	const auto FirstOf=[&](EWoTFactDomain Domain)->const FWoTFrontEndResourceRequirement*
		{ return Out.Descriptor.RequiredResources.FindByPredicate(
			[Domain](const auto& R){return R.Domain==Domain;}); };
	const auto* TextureRequirement=FirstOf(EWoTFactDomain::Texture);
	const auto* ProceduralRequirement=FirstOf(EWoTFactDomain::Procedural);
	if (!TextureRequirement || !ProceduralRequirement)
		{ OutError=TEXT("front-end fixture descriptor lacks texture/procedural data"); return false; }
	const FWoTFrontEndResourceRequirement* SurfaceTextureRequirement = TextureRequirement;
	if (WeatherOptions->bMaskedSurfaceTexture || WeatherOptions->bWorldSkyBlendCoverage)
	{
		SurfaceTextureRequirement = Out.Descriptor.RequiredResources.FindByPredicate(
			[](const FWoTFrontEndResourceRequirement& Resource)
			{
				return Resource.Domain == EWoTFactDomain::Texture
					&& Resource.SourceExport == TEXT("ScottT.mountain2");
			});
		if (SurfaceTextureRequirement == nullptr)
		{
			OutError = TEXT("front-end masked surface fixture lacks its exact texture closure");
			return false;
		}
	}
	const FWoTFrontEndResourceRequirement* ParticleTextureRequirement =
		SurfaceTextureRequirement;
	if (WeatherOptions->bUseSplash02ParticleTexture)
	{
		ParticleTextureRequirement = Out.Descriptor.RequiredResources.FindByPredicate(
			[](const FWoTFrontEndResourceRequirement& Resource)
			{
				return Resource.Domain == EWoTFactDomain::Texture
					&& Resource.SourceExport == TEXT("ParticleSystems.Water.Splash02");
			});
		if (ParticleTextureRequirement == nullptr)
		{
			OutError = TEXT("front-end particle fixture lacks its exact texture closure");
			return false;
		}
	}
	const FString DefaultBSPMaterialSlot =
		EntryConverterSlotBase(ProceduralRequirement->SourceExport);
	if (DefaultBSPMaterialSlot.IsEmpty())
		{ OutError=TEXT("front-end fixture procedural identity is malformed"); return false; }
	TArray<FString> Ambient;
	for (const auto& R:Out.Descriptor.RequiredResources)
		if (R.Domain==EWoTFactDomain::Sound
			&& !R.SourceExport.Contains(TEXT("thunder"),ESearchCase::IgnoreCase)
			&& Ambient.Num()<5) Ambient.Add(R.SourceExport);
	if (Ambient.Num()!=5)
		{ OutError=TEXT("front-end fixture could not select five ambience records"); return false; }

	struct FObject { FString Id, Schema, Fact; TArray<uint8> Bytes; };
	TArray<FObject> Objects;
	for (const auto& R:Out.Descriptor.RequiredResources)
	{
		const FWoTRequiredFactOutput* Output=nullptr;
		const FWoTRequiredFact* Fact=FactFor(Contract,R.LogicalId,Output);
		if (!Fact || !Output) { OutError=TEXT("descriptor object is not contract-bound"); return false; }
		TArray<uint8> Bytes;
		if (ObjectOverrides != nullptr)
		{
			if (const TArray<uint8>* Override = ObjectOverrides->Find(R.LogicalId))
				Bytes = *Override;
		}
		if (Bytes.IsEmpty()) switch(R.Domain)
		{
		case EWoTFactDomain::Texture:
			Bytes=WeatherOptions->bMaskedSurfaceTexture
				&& R.SourceExport == SurfaceTextureRequirement->SourceExport
				? RetargetMaskedTexture(Texture, R.SourceExport)
				: RetargetObject(
				R.SourceExport == TEXT("Environment.Skins.RainDropsA")
					? RainTextureObject() : R.SourceExport == TEXT("WOT.F_WOTReg30.Texture10")
					|| R.SourceExport == TEXT("WOT.F_WOTReg14.Texture9")
					? FontAtlasObject(Out.bPatternedFontGlyphs) : Texture,
				R.SourceExport);
			break;
		case EWoTFactDomain::Sound: Bytes=RetargetObject(Sound,R.SourceExport); break;
		case EWoTFactDomain::Mesh: Bytes=RetargetMesh(Mesh,R.SourceExport,
			R.SourceExport == TEXT("Environment.RainCyd")
				? FStringView(TEXT("Environment.Skins.RainDropsA")) : FStringView(TextureRequirement->SourceExport),
			R.SourceExport == TEXT("Environment.RainCyd")
				? RainSkinFixture
				: EWoTFrontEndRainSkinFixture::MeshLocal); break;
		case EWoTFactDomain::Procedural: Bytes=RetargetObject(Procedural,R.SourceExport); break;
		case EWoTFactDomain::Font:
		{
			const FString PageExport = R.SourceExport.Contains(TEXT("Reg30"))
				? TEXT("WOT.F_WOTReg30.Texture10")
				: TEXT("WOT.F_WOTReg14.Texture9");
			const FWoTFrontEndResourceRequirement* Page =
				Out.Descriptor.RequiredResources.FindByPredicate(
					[&PageExport](const auto& Candidate)
					{ return Candidate.Domain == EWoTFactDomain::Texture
						&& Candidate.SourceExport == PageExport; });
			if (Page == nullptr)
			{
				OutError=TEXT("front-end font fixture has no exact atlas closure");
				return false;
			}
			Bytes=RetargetFont(Font, R.SourceExport,
				Page->LogicalId, Page->SourceExport,
				Out.bPatternedFontGlyphs
					? (R.SourceExport.Contains(TEXT("Reg30")) ? 30u : 14u)
					: 0u, Out.bZeroWidthFontGlyph);
			break;
		}
		case EWoTFactDomain::Animation: Bytes=RetargetObject(Animation,R.SourceExport); break;
		case EWoTFactDomain::Localization:
			Bytes=RetargetMainMenuLocalization(Localization,R.SourceExport); break;
		case EWoTFactDomain::World: Bytes=EntryWorldObject(R.SourceExport,Ambient,
			SurfaceTextureRequirement->SourceExport,
			ParticleTextureRequirement->SourceExport,
			ProceduralRequirement->SourceExport, bDuplicateLightningRoute,
			bThunderIndexGap, bThunderOverrideMismatch,
			bThunderOverrideRootMismatch, bThunderOverrideCaseMismatch,
			WeatherOptions); break;
		case EWoTFactDomain::BSP: Bytes=bCorruptBSP?TArray<uint8>{0xde,0xad,0xbe,0xef}:
			(WeatherOptions != nullptr && !bMissingLightmap
				? EntryWeatherBSPObject(R.SourceExport,
					BSPMaterialSlotOverride.IsEmpty()
						? FStringView(DefaultBSPMaterialSlot)
						: BSPMaterialSlotOverride, *WeatherOptions, DefaultBSPMaterialSlot,
					BSPMaterialSlotOverride.IsEmpty()
						? FStringView(ProceduralRequirement->SourceExport)
						: FStringView(SurfaceTextureRequirement->SourceExport),
					ProceduralRequirement->SourceExport)
				: EntryBSPObject(R.SourceExport, BSPMaterialSlotOverride.IsEmpty()
					? FStringView(DefaultBSPMaterialSlot)
					: BSPMaterialSlotOverride, !bMissingLightmap,
					bIncludeDefaultWaterSection
						? FStringView(DefaultBSPMaterialSlot) : FStringView())); break;
		default: break;
		}
		if (Bytes.IsEmpty()) { OutError=TEXT("front-end fixture object encoding failed"); return false; }
		Objects.Add({R.LogicalId,Output->Schema,Fact->FactId,MoveTemp(Bytes)});
	}
	if (Out.bIncludeBrowserResources)
	{
		#if !WOT_GUARDED_BOOTSTRAP
		auto ClassDefaultsResult = WoTBuildClassDefaultObjectsForDevelopment(
			Contract, WoTReferencePath(TEXT("original/System")),
			FPaths::Combine(Out.CacheRoot, TEXT("class-default-build-cache")));
		if (ClassDefaultsResult.IsError())
		{
			return Fail(MoveTemp(ClassDefaultsResult).GetError());
		}
		for (FWoTDevelopmentClassDefaultObject& ClassDefault
			: ClassDefaultsResult.GetValue())
		{
			if (Out.bMalformedBrowserLookAndFeel
				&& ClassDefault.LogicalId == TEXT("cache/class_default/UWindow"))
			{
				// Production-path malformed-look fixture: the authenticated font and
				// atlases remain valid, but the look-and-feel class cannot decode.
				continue;
			}
			const FWoTRequiredFactOutput* Output = nullptr;
			const FWoTRequiredFact* Fact = FactFor(
				Contract, ClassDefault.LogicalId, Output);
			if (Fact == nullptr || Output == nullptr
				|| Fact->Domain != EWoTFactDomain::ClassDefault)
			{
				OutError = TEXT("browser fixture class default is not contract-bound");
				return false;
			}
			Objects.Add({ClassDefault.LogicalId, Output->Schema, Fact->FactId,
				MoveTemp(ClassDefault.ObjectBytes)});
		}
		#endif // !WOT_GUARDED_BOOTSTRAP
		Objects.Add({TEXT("cache/font/UWindowFonts__Tahoma20"), TEXT("wot-font-v3"),
			TEXT("retail.font.uwindowfonts.tahoma20"), RetargetFont(Font,
				TEXT("UWindowFonts.Tahoma20"), TEXT("cache/texture/spec-browser-font"),
				TEXT("UWindowFonts.Tahoma20.Texture4"),
				Out.bPatternedFontGlyphs ? 24u : 0u, Out.bZeroWidthFontGlyph)});
		Objects.Add({TEXT("cache/texture/spec-browser-font"), TEXT("wot-texture-v3"),
			TEXT("spec.browser.font.atlas"), RetargetObject(
				FontAtlasObject(Out.bPatternedFontGlyphs),
				TEXT("UWindowFonts.Tahoma20.Texture4"))});
		Objects.Add({TEXT("cache/texture/UWindow__Misc"), TEXT("wot-texture-v3"),
			TEXT("retail.texture.uwindow.misc"), BrowserAtlasObject(TEXT("UWindow.Icons.Misc"), 128, false)});
		Objects.Add({TEXT("cache/texture/UWindow__ActiveFrameS"), TEXT("wot-texture-v3"),
			TEXT("retail.texture.uwindow.activeframes"), BrowserAtlasObject(TEXT("UWindow.Icons.ActiveFrameS"), 128, true)});
		Objects.Add({TEXT("cache/texture/UBrowser__Highlight"), TEXT("wot-texture-v3"),
			TEXT("retail.texture.ubrowser.highlight"), BrowserAtlasObject(TEXT("UBrowser.Icons.Highlight"), 16, true)});
	}
	auto StoreResult=FWoTCacheStore::ForTesting(Out.CacheRoot);
	if (StoreResult.IsError()) return Fail(MoveTemp(StoreResult).GetError());
	FWoTCacheStore Store=MoveTemp(StoreResult).GetValue();
	auto SessionResult = AcquireWoTProfileSessionShared(
		FPaths::GetPath(Out.CacheRoot));
	if (SessionResult.IsError()) return Fail(MoveTemp(SessionResult).GetError());
	FWoTProfileSession Session = MoveTemp(SessionResult).GetValue();
	auto Exclusive = Session.TryAcquireExclusive();
	if (Exclusive.IsError()) return Fail(MoveTemp(Exclusive).GetError());
	if (!Exclusive.GetValue())
	{
		OutError = TEXT("front-end fixture profile session is contended");
		return false;
	}
	auto Permit=Store.BeginBuild(Session);
	if (Permit.IsError()) return Fail(MoveTemp(Permit).GetError());
	FWoTCacheBuildResult Build;
	Build.Manifest.bHasBuildIdentity=true;
	Build.Manifest.InstallerSha256=SeedDigest(80);
	Build.Manifest.ContractSha256=SeedDigest(100);
	Build.Manifest.ProductionDescriptorSha256=SeedDigest(120);
	for (FObject& Object:Objects)
	{
		FWoTBootstrapStatus Written=Store.WriteObject(Object.Id,Object.Schema,
			Object.Bytes,Sha256(Object.Bytes),1024ull*1024ull*1024ull);
		if (Written.IsError()) return Fail(MoveTemp(Written).GetError());
		FWoTCacheBlob& Blob=Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.LogicalId=Object.Id; Blob.Schema=Object.Schema; Blob.Sha256=Sha256(Object.Bytes);
		Blob.Size=Object.Bytes.Num(); Blob.Satisfies.Add({Object.Fact});
	}
	auto Sidecar=WriteSealedFrontEndDescriptorSidecar(Out.Descriptor,
		Build.Manifest.InstallerSha256,Build.Manifest.ContractSha256,
		Build.Manifest.ProductionDescriptorSha256);
	if (Sidecar.IsError()) return Fail(MoveTemp(Sidecar).GetError());
	constexpr TCHAR DescriptorId[]=TEXT("cache/front_end_world/WheelOfTime");
	FWoTBootstrapStatus SidecarWritten=Store.WriteObject(DescriptorId,
		TEXT("wot-front-end-world-v3"),Sidecar.GetValue(),Sha256(Sidecar.GetValue()),4194304);
	if (SidecarWritten.IsError()) return Fail(MoveTemp(SidecarWritten).GetError());
	FWoTCacheBlob& SidecarBlob=Build.Manifest.Blobs.AddDefaulted_GetRef();
	SidecarBlob.LogicalId=DescriptorId; SidecarBlob.Schema=TEXT("wot-front-end-world-v3");
	SidecarBlob.Sha256=Sha256(Sidecar.GetValue()); SidecarBlob.Size=Sidecar.GetValue().Num();
	SidecarBlob.Satisfies.Add({TEXT("retail.front_end_world.WheelOfTime")});

	Build.Manifest.FrontEndDescriptorRoot=Out.Descriptor.DescriptorRoot;

	auto Manifest=WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError()) return Fail(MoveTemp(Manifest).GetError());
	Build.CanonicalManifest=MoveTemp(Manifest).GetValue(); Build.ManifestSha256=Sha256(Build.CanonicalManifest);
	auto Published=Store.Publish(Build);
	if (Published.IsError()) return Fail(MoveTemp(Published).GetError());
	auto Restored = Session.RestoreShared();
	if (Restored.IsError()) return Fail(MoveTemp(Restored).GetError());

	auto Snapshot=WoTAcquireCurrentForTesting(Store);
	if (Snapshot.IsError()) return Fail(MoveTemp(Snapshot).GetError());
	auto Verified=Snapshot.GetValue().VerifyCriticalClosureForTesting(
		Out.Descriptor.RequiredLogicalIds,nullptr);
	if (Verified.IsError()) return Fail(MoveTemp(Verified).GetError());

	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true,true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	FWoTEarlyUIAuthority& EarlyUIAuthority = GetEarlyUIAuthority();
	EarlyUIAuthority.ResetForTesting();
	static constexpr ANSICHAR EarlyUIText[] =
		"Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"
		"ReducedMotion=0\nHighContrast=0\nScreenReader=0\n"
		"BootstrapVolume=1.0\nInputSensitivity=1.0\n";
	TArray<uint8> EarlyUIBytes;
	EarlyUIBytes.Append(reinterpret_cast<const uint8*>(EarlyUIText),
		UE_ARRAY_COUNT(EarlyUIText) - 1);
	auto AuthenticatedEarlyUI = ParseEarlyUIV1(EarlyUIBytes,
		FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (AuthenticatedEarlyUI.IsError()
		|| EarlyUIAuthority.PublishOnce(
			MoveTemp(AuthenticatedEarlyUI).GetValue()).IsError())
	{
		OutError=TEXT("front-end fixture authenticated preferences publication failed");
		return false;
	}
	// Named, not an inline temporary: the sink holds a TFunctionRef, which
	// would dangle past this statement (Clang -Wdangling; MSVC is silent).
	const auto ApplyFrontEndProjection =
		[](uint16, uint16, EWoTWindowMode) -> FWoTBootstrapStatus
		{
			return FWoTBootstrapStatus::Success();
		};
	const FWoTEarlyUINativeProjectionSink ProjectionSink{ApplyFrontEndProjection};
	FString ProjectionError;
	if (!WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
		ProjectionSink, &ProjectionError))
	{
		OutError=FString::Printf(
			TEXT("front-end fixture native preference projection failed: %s"),
			*ProjectionError);
		return false;
	}
	WoTTest::FSpecWorldOptions Options; Options.GameInstanceClass=UWoTGameInstance::StaticClass();
	Options.bActivatePayload=false;
	Options.BeforeInitializeStandalone=[Root=Out.CacheRoot](UGameInstance* GI)
		{ CastChecked<UWoTGameInstance>(GI)->SetPayloadCacheDir(Root,false); };
	Out.World=WoTTest::CreateSpecWorld(Options);
	Out.Payload=Out.World?Out.World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>():nullptr;
	if (!Out.Payload) { OutError=TEXT("front-end fixture payload subsystem absent"); return false; }
	UWoTPreferencesSubsystem* Preferences = Out.World->GetGameInstance()
		->GetSubsystem<UWoTPreferencesSubsystem>();
	if (!Preferences || !Preferences->HasAdoptedAuthenticatedEarlyUI())
	{
		OutError=TEXT("front-end fixture authenticated preferences absent");
		return false;
	}
	// Previously loaded UI carriers lie outside this deliberately entry-only
	// synthetic closure. Expect only the exact known rejection for each live
	// carrier; other warnings and all production failures remain visible.
	if (FAutomationTestBase* Test = FAutomationTestFramework::Get().GetCurrentTest())
	{
		const TPair<const TCHAR*, const TCHAR*> Carriers[] = {
			{TEXT("/Game/WoT/Textures/WOT/Slide1.Slide1"), TEXT("WOT__Slide1")},
			{TEXT("/Game/WoT/Textures/WOT/Slide2.Slide2"), TEXT("WOT__Slide2")},
			{TEXT("/Game/WoT/Textures/WOT/Slide3.Slide3"), TEXT("WOT__Slide3")},
			{TEXT("/Game/WoT/Textures/WOT/Slide4.Slide4"), TEXT("WOT__Slide4")},
			{TEXT("/Game/WoT/Textures/WOT/WOTConsole.WOTConsole"), TEXT("WOT__WOTConsole")},
			{TEXT("/Game/WoT/Textures/WOT/WOTBorder.WOTBorder"), TEXT("WOT__WOTBorder")},
			{TEXT("/Game/WoT/UI/Fonts/T_F_WOTReg08_0.T_F_WOTReg08_0"), TEXT("WOT__Texture7")},
			{TEXT("/Game/WoT/UI/Fonts/T_F_WOTIta14_0.T_F_WOTIta14_0"), TEXT("WOT__Texture8")}};
		for (const auto& Carrier : Carriers)
			if (FindObject<UTexture2D>(nullptr, Carrier.Key))
				Test->AddExpectedMessagePlain(FString::Printf(TEXT("Rejecting texture binding on %s: neutral logical id texture:%s is outside the texture domain"), Carrier.Key, Carrier.Value), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Exact, 1);
	}
	auto Activated=Out.Payload->ActivateVerifiedCache(Snapshot.GetValue());
	if (Activated.IsError()) return Fail(MoveTemp(Activated).GetError());
	Out.WorldLease=MakeUnique<FWoTCacheGenerationLease>(Snapshot.GetValue());
	return true;
}

bool WoTFrontEndTest_BuildProductionStageFixture(
	FWoTFrontEndStageProductionFixture& Out, bool bCorruptBSP, FString& OutError,
	bool bDuplicateLightningRoute, bool bThunderIndexGap,
	bool bThunderOverrideMismatch, bool bThunderOverrideRootMismatch,
	bool bThunderOverrideCaseMismatch, FStringView BSPMaterialSlotOverride,
	EWoTFrontEndRainSkinFixture RainSkinFixture, bool bMissingLightmap,
	bool bIncludeDefaultWaterSection)
{
	return BuildProductionStageFixtureImpl(Out, bCorruptBSP, OutError,
		bDuplicateLightningRoute, bThunderIndexGap, bThunderOverrideMismatch,
		bThunderOverrideRootMismatch, bThunderOverrideCaseMismatch,
		BSPMaterialSlotOverride, RainSkinFixture, bMissingLightmap,
		bIncludeDefaultWaterSection, nullptr, nullptr);
}

bool WoTFrontEndTest_BuildWeatherStageFixture(
	FWoTFrontEndStageProductionFixture& Out,
	const FWoTFrontEndWeatherFixtureOptions& Options, FString& Error)
{
	if (Options.RainScaleGlow.Num() != 6
		|| Options.RainScaleGlow.ContainsByPredicate(
			[](float Value) { return !FMath::IsFinite(Value); }))
	{
		Error = TEXT("weather fixture requires six finite rain glow values");
		return false;
	}
	return BuildProductionStageFixtureImpl(Out, false, Error, false, false,
		false, false, false, {}, EWoTFrontEndRainSkinFixture::ClassDefaultOneSection,
		false, false, &Options, nullptr);
}

bool WoTFrontEndTest_BuildRealWeatherStageFixture(
	FWoTFrontEndStageProductionFixture& Out, FString& Error)
{
	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes,
		*WoTReferencePath(TEXT("original/Maps/WheelOfTime.wot"))))
	{
		Error = WoTReferenceMissingWarning(TEXT("original/Maps/WheelOfTime.wot"));
		return false;
	}
	auto ContractResult = WoTLoadProductionRetailContract();
	if (ContractResult.IsError())
	{
		Error = MoveTemp(ContractResult).GetError().ToString();
		return false;
	}
	FWoTRetailContract Contract = MoveTemp(ContractResult).GetValue();
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	Sources.Add({TEXT("Maps/WheelOfTime.wot"), MoveTemp(SourceBytes)});
	FWeatherObjectCapture Writer;
	FWeatherImportProgress Progress;
	auto Imported = ImportFrontEndWorldPackageBytesForTesting(
		Sources, Contract, Writer, Progress);
	if (Imported.IsError())
	{
		Error = MoveTemp(Imported).GetError().ToString();
		return false;
	}
	if (!Writer.Objects.Contains(TEXT("cache/world/WheelOfTime"))
		|| !Writer.Objects.Contains(TEXT("cache/bsp/WheelOfTime/WheelOfTime")))
	{
		Error = TEXT("real weather fixture import omitted its world or BSP object");
		return false;
	}
	return BuildProductionStageFixtureImpl(Out, false, Error, false, false,
		false, false, false, {}, EWoTFrontEndRainSkinFixture::MeshLocal,
		false, false, nullptr, &Writer.Objects);
}

void WoTFrontEndTest_DestroyProductionStageFixture(
	FWoTFrontEndStageProductionFixture& Fixture)
{
	Fixture.WorldLease.Reset();
	if (Fixture.World) WoTTest::DestroySpecWorld(Fixture.World);
	Fixture.World=nullptr; Fixture.Payload=nullptr;
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
	GetEarlyUIAuthority().ResetForTesting();
	if (!Fixture.CacheRoot.IsEmpty())
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Fixture.CacheRoot),false,true);
	Fixture.CacheRoot.Empty();
}

TArray<uint8> WoTFrontEndTest_PlaceholderMenuLocalization()
{
	static const TCHAR* Sections[] = {
		TEXT("menuMain"), TEXT("menuMultiPlayer"), TEXT("menuStartArenaServer"),
		TEXT("menuStartBattleServer"), TEXT("menuPlayer"), TEXT("menuOptions"),
		TEXT("menuKeyboard"), TEXT("menuConfiguration")};
	FString Text;
	for (int32 Section = 0; Section < UE_ARRAY_COUNT(Sections); ++Section)
	{
		Text += FString::Printf(TEXT("[%s]\r\nMenuTitle=Title %d\r\n"),
			Sections[Section], Section);
		for (int32 Index = 1; Index <= 20; ++Index)
		{
			if (Section == 6 && Index == 12)
			{
				Text += FString::Printf(TEXT("MenuList[%d]=\"Label %d %d\"\r\n"),
					Index, Section, Index);
			}
			else
			{
				Text += FString::Printf(TEXT("MenuList[%d]=Label %d %d\r\n"),
					Index, Section, Index);
			}
			Text += FString::Printf(TEXT("HelpMessage[%d]=Help %d %d\r\n"),
				Index, Section, Index);
		}
	}
	const FTCHARToUTF8 Utf8(*Text);
	return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

#endif
