#include "WoTWorldMaterializerTestData.h"
#include "WoTCacheTestSupport.h"

#if WITH_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "../Payload/WoTCacheStore.h"
#include "../World/WoTWorldMaterializer.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/FrontEndWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"
#include "WoTRetailReader/Generated/RequiredWorlds.generated.h"

#include "Containers/StringConv.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

const FString& WoTFrontEndTest_WorldLogicalId(
	const FWoTFrontEndDescriptor& Descriptor);
const FString& WoTFrontEndTest_BSPLogicalId(
	const FWoTFrontEndDescriptor& Descriptor);
const FWoTSha256& WoTFrontEndTest_DescriptorRoot(
	const FWoTFrontEndDescriptor& Descriptor);
TWoTBootstrapResult<TArray<uint8>> WoTFrontEndTest_WriteDescriptorSidecar(
	const FWoTFrontEndDescriptor& Descriptor,
	const FWoTSha256& InstallerSha256, const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256);

namespace
{
void AppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void AppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value >> (Index * 8)));
	}
}

void AppendI32(TArray<uint8>& Bytes, int32 Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	AppendU32(Bytes, Bits);
}

void AppendU64(TArray<uint8>& Bytes, uint64 Value)
{
	AppendU32(Bytes, static_cast<uint32>(Value));
	AppendU32(Bytes, static_cast<uint32>(Value >> 32));
}

void AppendFloat(TArray<uint8>& Bytes, float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	AppendU32(Bytes, Bits);
}

void AppendString(TArray<uint8>& Bytes, const FString& Value)
{
	const FTCHARToUTF8 Encoded(*Value);
	AppendU32(Bytes, static_cast<uint32>(Encoded.Length()));
	Bytes.Append(
		reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
}

bool SplitRuntimeObject(TConstArrayView<uint8> Object, FString& Header,
	TArray<uint8>& Payload)
{
	if (Object.Num() < 12
		|| FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
	{
		return false;
	}
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

bool ReplaceRuntimePayloadString(TArray<uint8>& Payload,
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
		{
			Payload[Index - 4 + Byte] =
				static_cast<uint8>(NewLength >> (Byte * 8));
		}
		return true;
	}
	return false;
}

bool ReplaceRuntimePayloadLength(FString& Header, int32 PayloadLength)
{
	const FString Prefix = TEXT("\"payload_byte_length\":");
	const int32 Begin = Header.Find(Prefix, ESearchCase::CaseSensitive);
	if (Begin == INDEX_NONE) return false;
	const int32 ValueBegin = Begin + Prefix.Len();
	int32 ValueEnd = ValueBegin;
	while (ValueEnd < Header.Len() && FChar::IsDigit(Header[ValueEnd]))
	{
		++ValueEnd;
	}
	if (ValueEnd == ValueBegin) return false;
	Header = Header.Left(ValueBegin) + FString::FromInt(PayloadLength)
		+ Header.Mid(ValueEnd);
	return true;
}

TArray<uint8> RuntimeFontAtlasObject(FStringView SourceExport)
{
	TArray<uint8> Payload;
	const uint8 Magic[] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
	Payload.Append(Magic, UE_ARRAY_COUNT(Magic));
	AppendU16(Payload, 1);
	AppendU16(Payload, 0);
	AppendU32(Payload, 256);
	AppendU32(Payload, 1);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const uint8 Channel = Index == 0 ? 0 : Index == 1 ? 64 : 255;
		Payload.Append({Channel, Channel, Channel, 255});
	}
	AppendU32(Payload, 8);
	AppendU32(Payload, 8);
	AppendU32(Payload, 64);
	for (int32 Pixel = 0; Pixel < 64; ++Pixel) Payload.Add(1);
	const FString Header = FString::Printf(
		TEXT("{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},")
		TEXT("\"decoded_layout\":{\"mip_count\":1,\"mips\":[{\"expanded_rgba_byte_length\":256,\"height\":8,\"index_byte_length\":64,\"width\":8}],\"texel_format\":\"p8\"},")
		TEXT("\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.frontend.font-atlas\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":false,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,")
		TEXT("\"source\":{\"export\":\"%s\",\"package\":\"Synthetic/remaining.u\",\"sha256\":\"0511f92abc5f2308a0de6aa429fbfd30e1ff802d40f0cf0cd18f7b128e145c56\"}}"),
		Payload.Num(), *FString(SourceExport));
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> RuntimeFontObject(TConstArrayView<uint8> Fixture,
	FStringView FaceSourceExport, FStringView PageLogicalId,
	FStringView PageSourceExport)
{
	FString Header;
	TArray<uint8> Payload;
	if (!SplitRuntimeObject(Fixture, Header, Payload)
		|| !ReplaceRuntimePayloadString(Payload,
			TEXTVIEW("cache/texture/synthetic-atlas"), PageLogicalId)
		|| !ReplaceRuntimePayloadString(Payload,
			TEXTVIEW("remaining.Atlas"), PageSourceExport)
		|| !ReplaceRuntimePayloadLength(Header, Payload.Num()))
	{
		return {};
	}
	Header.ReplaceInline(TEXT("\"export\":\"remaining.FontA\""),
		*FString::Printf(TEXT("\"export\":\"%s\""),
			*FString(FaceSourceExport)), ESearchCase::CaseSensitive);
	Header.ReplaceInline(TEXT("\"logical_id\":\"cache/texture/synthetic-atlas\""),
		*FString::Printf(TEXT("\"logical_id\":\"%s\""),
			*FString(PageLogicalId)), ESearchCase::CaseSensitive);
	Header.ReplaceInline(TEXT("\"source_identity\":\"remaining.Atlas\""),
		*FString::Printf(TEXT("\"source_identity\":\"%s\""),
			*FString(PageSourceExport)), ESearchCase::CaseSensitive);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

FWoTSha256 SeedDigest(uint8 Seed)
{
	FWoTSha256 Digest;
	for (int32 Index = 0; Index < FWoTSha256::ByteCount; ++Index)
	{
		Digest.Bytes[Index] = static_cast<uint8>(Seed + Index);
	}
	return Digest;
}

void AppendUE1String(TArray<uint8>& Bytes, const FString& Value)
{
	const FTCHARToUTF8 Encoded(*Value);
	uint32 Remaining = static_cast<uint32>(Encoded.Length() + 1);
	uint8 First = static_cast<uint8>(Remaining & 0x3f);
	Remaining >>= 6;
	if (Remaining != 0) First |= 0x40;
	Bytes.Add(First);
	while (Remaining != 0)
	{
		uint8 Next = static_cast<uint8>(Remaining & 0x7f);
		Remaining >>= 7;
		if (Remaining != 0) Next |= 0x80;
		Bytes.Add(Next);
	}
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	Bytes.Add(0);
}

constexpr const TCHAR* ClassDefaultPackages[] = {
	TEXT("Angreal"), TEXT("Core"), TEXT("Editor"), TEXT("Engine"),
	TEXT("Environment"), TEXT("Fire"), TEXT("IpDrv"), TEXT("IpServer"),
	TEXT("Legend"), TEXT("MachinShin"), TEXT("ParticleSystems"),
	TEXT("UBrowser"), TEXT("UWindow"), TEXT("WinDrv"), TEXT("WOT"),
	TEXT("WoTBase"), TEXT("WoTBrowser"), TEXT("WoTDecorations"),
	TEXT("WoTPawns"), TEXT("WoTTraps")};

TArray<uint8> ClassDefaultsObject(const FString& Package)
{
	struct FRecord { FString Identity; FString Super; };
	TArray<FRecord> Records;
	if (Package == TEXT("Engine"))
	{
		Records = {{TEXT("Engine.Actor"), {}},
			{TEXT("Engine.Info"), TEXT("Engine.Actor")},
			{TEXT("Engine.NavigationPoint"), TEXT("Engine.Actor")},
			{TEXT("Engine.PathNode"), TEXT("Engine.NavigationPoint")},
			{TEXT("Engine.Mover"), TEXT("Engine.Actor")},
			{TEXT("Engine.Trigger"), TEXT("Engine.Actor")}};
	}
	else if (Package == TEXT("Angreal"))
	{
		Records = {{TEXT("Angreal.AngrealSpawnPool"), TEXT("Engine.Actor")}};
	}
	else if (Package == TEXT("WOT"))
	{
		Records = {
			{TEXT("WOT.BattleInfo"), TEXT("Engine.Info")},
			{TEXT("WOT.DoorMover"), TEXT("Engine.Mover")},
			{TEXT("WOT.DoorTrigger"), TEXT("Engine.Trigger")}};
	}
	else if (Package == TEXT("WoTPawns"))
	{
		Records = {{TEXT("WoTPawns.MashadarGuide"), TEXT("Engine.Actor")}};
	}
	else
	{
		Records.Add({Package + TEXT(".FixtureRoot"), {}});
	}
	TArray<uint8> Payload = {'W','O','T','D','E','F','3',0};
	AppendU16(Payload,3); AppendU16(Payload,0); AppendU32(Payload,Records.Num());
	for (int32 Index=0; Index<Records.Num(); ++Index)
	{
		AppendString(Payload,Records[Index].Identity);
		AppendU32(Payload,Index+1); AppendString(Payload,Records[Index].Super);
		Payload.Add(0);
		if (Records[Index].Identity == TEXT("Angreal.AngrealSpawnPool"))
		{
			// Canonical case-folded property order: bRespawn, Pool,
			// RespawnTime, Weight. This fixture lets the real listen harness
			// exercise a functional projected pool without authored map data.
			AppendU32(Payload, 4);
			auto Header = [&Payload](const TCHAR* Name, uint8 Type,
				uint32 ArrayIndex = 0)
			{
				AppendString(Payload, Name); Payload.Add(Type);
				AppendU32(Payload, ArrayIndex); AppendString(Payload, FString());
			};
			Header(TEXT("bRespawn"), 3); AppendString(Payload, FString());
			AppendString(Payload, FString()); AppendString(Payload, FString());
			AppendU32(Payload, 1); Payload.Add(1); AppendU32(Payload, 0);
			Header(TEXT("Pool"), 5); AppendString(Payload, FString());
			AppendString(Payload, TEXT("Angreal.Inventory"));
			AppendString(Payload, TEXT("Core.Class")); AppendU32(Payload, 0);
			AppendU32(Payload, 0);
			Header(TEXT("RespawnTime"), 4); AppendString(Payload, FString());
			AppendString(Payload, FString()); AppendString(Payload, FString());
			AppendU32(Payload, 4); AppendFloat(Payload, 80.0f);
			AppendU32(Payload, 0);
			Header(TEXT("Weight"), 1); AppendString(Payload, FString());
			AppendString(Payload, FString()); AppendString(Payload, FString());
			AppendU32(Payload, 1); Payload.Add(1); AppendU32(Payload, 0);
		}
		else AppendU32(Payload,0);
	}
	const FString Header=FString::Printf(
		TEXT("{\"encoding\":\"ue1-class-defaults-v2\",\"kind\":\"class_default\",\"payload_byte_length\":%d}"),
		Payload.Num());
	return WoTPayloadActivationTest_FrameObject(Header,Payload);
}

TArray<uint8> WorldObject(const FString& MapIdentity,
	bool bIncludeSpawnPool = false, bool bMalformedPoolRespawnTime = false)
{
	const FString ActorIdentity = MapIdentity + TEXT(".PathNode0");
	const FString ActorClass = TEXT("Engine.PathNode");
	const FString PoolIdentity = MapIdentity + TEXT(".AngrealSpawnPool0");
	const FString PoolClass = TEXT("Angreal.AngrealSpawnPool");
	const FString TextureIdentity = MapIdentity + TEXT(".Texture0");
	const FString TextureClass = TEXT("Engine.Texture");
	TArray<uint8> Payload = {'W', 'O', 'T', 'W', 'R', 'L', '3', 0};
	AppendU16(Payload, 2);
	AppendU16(Payload, 0);
	for (const uint32 Count : {
			bIncludeSpawnPool ? 2u : 1u, 0u, 3u, 3u, 1u,
			1u, 3u, 1u, bIncludeSpawnPool ? 3u : 2u, 1u})
	{
		AppendU32(Payload, Count);
	}
	for (const FVector3f& Vector : {
			FVector3f(0, 0, 1), FVector3f(1, 0, 0), FVector3f(0, 1, 0)})
	{
		AppendFloat(Payload, Vector.X);
		AppendFloat(Payload, Vector.Y);
		AppendFloat(Payload, Vector.Z);
	}
	for (const FVector3f& Point : {
			FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0)})
	{
		AppendFloat(Payload, Point.X);
		AppendFloat(Payload, Point.Y);
		AppendFloat(Payload, Point.Z);
	}
	for (float Plane : {0.0f, 0.0f, 1.0f, 0.0f})
	{
		AppendFloat(Payload, Plane);
	}
	AppendU64(Payload, 1);
	Payload.Add(0);
	for (int32 Value : {0, 0, static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE),
			static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE)})
	{
		AppendI32(Payload, Value);
	}
	Payload.Add(0);
	Payload.Add(0);
	Payload.Add(3);
	AppendI32(Payload, INDEX_NONE);
	AppendI32(Payload, INDEX_NONE);
	for (int32 Value : {0, 0, 1, 2, static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE)})
	{
		AppendI32(Payload, Value);
	}
	AppendU32(Payload, 0);
	AppendU16(Payload, 0);
	AppendU16(Payload, 0);
	AppendString(Payload, TextureIdentity);
	AppendString(Payload, ActorIdentity);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AppendI32(Payload, Index);
		AppendI32(Payload, static_cast<int32>(INDEX_NONE));
	}
	AppendString(Payload, ActorIdentity);
	AppendU64(Payload, 1);
	AppendU64(Payload, 1);
	AppendString(Payload, ActorIdentity);
	AppendString(Payload, ActorClass);
	Payload.Add(1);
	if (bIncludeSpawnPool)
	{
		AppendString(Payload, PoolIdentity);
		AppendString(Payload, PoolClass);
		Payload.Add(1);
	}
	AppendString(Payload, TextureIdentity);
	AppendString(Payload, TextureClass);
	Payload.Add(0);
	AppendString(Payload, ActorIdentity);
	AppendString(Payload, ActorClass);
	AppendU32(Payload, 1);
	AppendString(Payload, TEXT("Skin"));
	Payload.Add(5);
	AppendU32(Payload, 0);
	AppendString(Payload, FString());
	AppendString(Payload, FString());
	AppendString(Payload, TextureIdentity);
	AppendString(Payload, TextureClass);
	AppendU32(Payload, 0);
	AppendString(Payload, FString());
	if (bIncludeSpawnPool)
	{
		AppendString(Payload, PoolIdentity);
		AppendString(Payload, PoolClass);
		AppendU32(Payload, bMalformedPoolRespawnTime ? 1u : 0u);
		if (bMalformedPoolRespawnTime)
		{
			AppendString(Payload, TEXT("RespawnTime"));
			Payload.Add(4);
			AppendU32(Payload, 0);
			AppendString(Payload, FString());
			AppendString(Payload, FString());
			AppendString(Payload, FString());
			AppendString(Payload, FString());
			AppendU32(Payload, 4);
			AppendU32(Payload, 0x7fc00000u);
		}
		AppendString(Payload, FString());
	}
	for (int32 Value : {100, 20, 40})
	{
		AppendI32(Payload, Value);
	}
	AppendU32(Payload, 1);
	Payload.Add(0);
	AppendString(Payload, ActorIdentity);
	AppendString(Payload, ActorIdentity);
	const FString Header = FString::Printf(
		TEXT("{\"actor_count\":%d,\"attachment_count\":0,\"bsp_node_count\":1,")
		TEXT("\"bsp_surface_count\":1,\"encoding\":\"ue1-world-records-le-v2\",")
		TEXT("\"fact_id\":\"spec.world.%s\",\"importer\":\"world-v1\",")
		TEXT("\"importer_version\":2,\"kind\":\"world\",\"light_count\":0,")
		TEXT("\"map_identity\":\"%s\",\"mover_count\":0,")
		TEXT("\"navigation_seed_count\":1,\"payload_byte_length\":%d,")
		TEXT("\"portal_surface_count\":0,\"reach_spec_count\":1,")
		TEXT("\"semantic_object_count\":%d,\"sound_count\":0,")
		TEXT("\"source\":{\"export\":\"Synthetic.World\",")
		TEXT("\"package\":\"Synthetic/World.u\",\"sha256\":")
		TEXT("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},")
		TEXT("\"zone_count\":1}"), bIncludeSpawnPool ? 2 : 1,
		*MapIdentity, *MapIdentity, Payload.Num(), bIncludeSpawnPool ? 3 : 2);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

// The packaged listen matrix needs startup actors, not dynamically spawned
// substitutes: InitializeNetworkActors is what assigns the client roles and
// stable NetGUID paths the production materializer relies on. Keep the older
// minimal v2 object above for decoder/error fixtures and give the route matrix
// one small but complete world-v3 door chain.
TArray<uint8> GameplayWorldObject(const FString& MapIdentity)
{
	const FString PathIdentity = MapIdentity + TEXT(".PathNode0");
	const FString PoolIdentity = MapIdentity + TEXT(".AngrealSpawnPool0");
	const FString MoverIdentity = MapIdentity + TEXT(".Mover0");
	const FString TriggerIdentity = MapIdentity + TEXT(".Trigger0");
	const FString BattleIdentity = MapIdentity + TEXT(".BattleInfo0");
	const FString GuideIdentity = MapIdentity + TEXT(".MashadarGuide0");
	const FString LevelIdentity = MapIdentity + TEXT(".LevelInfo0");
	const FString TextureIdentity = MapIdentity + TEXT(".Texture0");
	const FString ModelIdentity = MapIdentity + TEXT(".MoverModel0");
	TArray<uint8> Payload = {'W','O','T','W','R','L','3',0};
	AppendU16(Payload, 3); AppendU16(Payload, 0);
	for (const uint32 Count : {
		7u, 0u, 3u, 3u, 1u, 1u, 3u, 1u, 11u, 1u, 0u, 0u, 1u})
		AppendU32(Payload, Count);
	AppendU32(Payload, 0); // BSP root is inside.
	for (const FVector3f& Vector : {
		FVector3f(0,0,1), FVector3f(1,0,0), FVector3f(0,1,0)})
	{
		AppendFloat(Payload, Vector.X); AppendFloat(Payload, Vector.Y);
		AppendFloat(Payload, Vector.Z);
	}
	for (const FVector3f& Point : {
		FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(0,100,0)})
	{
		AppendFloat(Payload, Point.X); AppendFloat(Payload, Point.Y);
		AppendFloat(Payload, Point.Z);
	}
	for (float Plane : {0.0f, 0.0f, 1.0f, 0.0f}) AppendFloat(Payload, Plane);
	AppendU64(Payload, 1); Payload.Add(0);
	for (int32 Value : {0, 0, static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE)})
		AppendI32(Payload, Value);
	Payload.Add(0); Payload.Add(0); Payload.Add(3);
	AppendI32(Payload, INDEX_NONE); AppendI32(Payload, INDEX_NONE);
	for (int32 Value : {0, 0, 1, 2, static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE)}) AppendI32(Payload, Value);
	AppendU32(Payload, 0); AppendU16(Payload, 0); AppendU16(Payload, 0);
	AppendFloat(Payload, 1.0f);
	AppendString(Payload, TextureIdentity); AppendString(Payload, PathIdentity);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AppendI32(Payload, Index); AppendI32(Payload, INDEX_NONE);
	}
	AppendString(Payload, FString()); AppendU64(Payload, 1); AppendU64(Payload, 1);

	for (const auto& Semantic : {
		TTuple<FString,FString,uint8>(PathIdentity, TEXT("Engine.PathNode"), 1),
		TTuple<FString,FString,uint8>(PoolIdentity, TEXT("Angreal.AngrealSpawnPool"), 1),
		TTuple<FString,FString,uint8>(MoverIdentity, TEXT("WOT.DoorMover"), 1),
		TTuple<FString,FString,uint8>(TriggerIdentity, TEXT("WOT.DoorTrigger"), 1),
		TTuple<FString,FString,uint8>(BattleIdentity, TEXT("WOT.BattleInfo"), 1),
		TTuple<FString,FString,uint8>(GuideIdentity, TEXT("WoTPawns.MashadarGuide"), 1),
		TTuple<FString,FString,uint8>(LevelIdentity, TEXT("Engine.LevelInfo"), 1),
		TTuple<FString,FString,uint8>(TEXT("WOTPawns.AesSedai"), TEXT("Core.Class"), 0),
		TTuple<FString,FString,uint8>(TEXT("WOTPawns.Hound"), TEXT("Core.Class"), 0),
		TTuple<FString,FString,uint8>(TextureIdentity, TEXT("Engine.Texture"), 0),
		TTuple<FString,FString,uint8>(ModelIdentity, TEXT("Engine.Model"), 0)})
	{
		AppendString(Payload, Semantic.Get<0>());
		AppendString(Payload, Semantic.Get<1>());
		Payload.Add(Semantic.Get<2>());
	}

	auto PropertyHeader = [&Payload](const TCHAR* Name, uint8 Type,
		uint32 ArrayIndex, const TCHAR* StructName = TEXT(""),
		const TCHAR* NameValue = TEXT(""), const TCHAR* Object = TEXT(""),
		const TCHAR* ObjectClass = TEXT(""))
	{
		AppendString(Payload, Name); Payload.Add(Type); AppendU32(Payload, ArrayIndex);
		AppendString(Payload, StructName); AppendString(Payload, NameValue);
		AppendString(Payload, Object); AppendString(Payload, ObjectClass);
	};
	auto EndProperty = [&Payload]() { AppendU32(Payload, 0); };

	AppendString(Payload, PathIdentity); AppendString(Payload, TEXT("Engine.PathNode"));
	AppendU32(Payload, 1);
	PropertyHeader(TEXT("Skin"), 5, 0, TEXT(""), TEXT(""),
		*TextureIdentity, TEXT("Engine.Texture"));
	AppendU32(Payload, 0); EndProperty();
	AppendString(Payload, FString());

	AppendString(Payload, PoolIdentity);
	AppendString(Payload, TEXT("Angreal.AngrealSpawnPool"));
	AppendU32(Payload, 0); AppendString(Payload, FString());

	AppendString(Payload, MoverIdentity); AppendString(Payload, TEXT("WOT.DoorMover"));
	AppendU32(Payload, 5);
	PropertyHeader(TEXT("Brush"), 5, 0, TEXT(""), TEXT(""),
		*ModelIdentity, TEXT("Engine.Model"));
	AppendU32(Payload, 0); EndProperty();
	PropertyHeader(TEXT("InitialState"), 6, 0, TEXT(""), TEXT("TriggerDoor"));
	AppendU32(Payload, 0); EndProperty();
	PropertyHeader(TEXT("KeyPos"), 10, 1, TEXT("Vector"));
	AppendU32(Payload, 12); AppendFloat(Payload, 0.0f);
	AppendFloat(Payload, 0.0f); AppendFloat(Payload, 100.0f); EndProperty();
	PropertyHeader(TEXT("MoveTime"), 4, 0);
	AppendU32(Payload, 4); AppendFloat(Payload, 0.25f); EndProperty();
	PropertyHeader(TEXT("Tag"), 6, 0, TEXT(""), TEXT("FixtureDoor"));
	AppendU32(Payload, 0); EndProperty();
	AppendString(Payload, FString());

	AppendString(Payload, TriggerIdentity);
	AppendString(Payload, TEXT("WOT.DoorTrigger"));
	AppendU32(Payload, 5);
	PropertyHeader(TEXT("CollisionHeight"), 4, 0);
	AppendU32(Payload, 4); AppendFloat(Payload, 64.0f); EndProperty();
	PropertyHeader(TEXT("CollisionRadius"), 4, 0);
	AppendU32(Payload, 4); AppendFloat(Payload, 64.0f); EndProperty();
	PropertyHeader(TEXT("Event"), 6, 0, TEXT(""), TEXT("FixtureDoor"));
	AppendU32(Payload, 0); EndProperty();
	PropertyHeader(TEXT("Location"), 10, 0, TEXT("Vector"));
	AppendU32(Payload, 12); AppendFloat(Payload, 200.0f);
	AppendFloat(Payload, 0.0f); AppendFloat(Payload, 0.0f); EndProperty();
	PropertyHeader(TEXT("Tag"), 6, 0, TEXT(""), TEXT("FixtureTrigger"));
	AppendU32(Payload, 0); EndProperty();
	AppendString(Payload, FString());

	AppendString(Payload, BattleIdentity);
	AppendString(Payload, TEXT("WOT.BattleInfo"));
	AppendU32(Payload, 3);
	PropertyHeader(TEXT("NumTeams"), 2, 0);
	AppendU32(Payload, 4); AppendI32(Payload, 2); EndProperty();
	PropertyHeader(TEXT("TeamClass"), 5, 0, TEXT(""), TEXT(""),
		TEXT("WOTPawns.AesSedai"), TEXT("Core.Class"));
	AppendU32(Payload, 0); EndProperty();
	PropertyHeader(TEXT("TeamClass"), 5, 1, TEXT(""), TEXT(""),
		TEXT("WOTPawns.Hound"), TEXT("Core.Class"));
	AppendU32(Payload, 0); EndProperty();
	AppendString(Payload, FString());

	AppendString(Payload, GuideIdentity);
	AppendString(Payload, TEXT("WoTPawns.MashadarGuide"));
	AppendU32(Payload, 2);
	PropertyHeader(TEXT("Location"), 10, 0, TEXT("Vector"));
	AppendU32(Payload, 12); AppendFloat(Payload, 700.0f);
	AppendFloat(Payload, 800.0f); AppendFloat(Payload, 900.0f); EndProperty();
	PropertyHeader(TEXT("Rotation"), 10, 0, TEXT("Rotator"));
	AppendU32(Payload, 12); AppendI32(Payload, 0);
	AppendI32(Payload, 16384); AppendI32(Payload, 0); EndProperty();
	AppendString(Payload, FString());

	AppendString(Payload, LevelIdentity);
	AppendString(Payload, TEXT("Engine.LevelInfo"));
	AppendU32(Payload, 1);
	PropertyHeader(TEXT("Title"), 13, 0);
	TArray<uint8> TitleValue;
	AppendUE1String(TitleValue, TEXT("Materialized Scoreboard Title"));
	AppendU32(Payload, TitleValue.Num()); Payload.Append(TitleValue); EndProperty();
	AppendString(Payload, FString());

	for (int32 Value : {100, 20, 40}) AppendI32(Payload, Value);
	AppendU32(Payload, 1); Payload.Add(0);
	AppendString(Payload, PathIdentity); AppendString(Payload, PathIdentity);

	AppendString(Payload, MoverIdentity); AppendString(Payload, ModelIdentity);
	AppendU32(Payload, 1); AppendU32(Payload, 3); AppendU32(Payload, 3);
	AppendU32(Payload, 0); AppendU32(Payload, 3);
	AppendU32(Payload, 0); AppendU32(Payload, 2);
	Payload.Add(1); Payload.AddZeroed(3); AppendU32(Payload, 0);
	for (float Value : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f})
		AppendFloat(Payload, Value);
	AppendString(Payload, TEXT("FixtureMoverPlane"));
	AppendString(Payload, FString()); AppendString(Payload, TEXT("None"));
	AppendString(Payload, TEXT("None"));
	for (const FVector3f& Position : {
		FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(0,100,0)})
	{
		AppendFloat(Payload, Position.X); AppendFloat(Payload, Position.Y);
		AppendFloat(Payload, Position.Z);
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AppendFloat(Payload, 0.0f); AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 1.0f);
	}
	for (const FVector2f& UV : {
		FVector2f(0,0), FVector2f(1,0), FVector2f(0,1)})
	{
		AppendFloat(Payload, UV.X); AppendFloat(Payload, UV.Y);
	}
	AppendU32(Payload, 0); AppendU32(Payload, 1); AppendU32(Payload, 2);

	const FString Header = FString::Printf(
		TEXT("{\"actor_count\":7,\"attachment_count\":0,\"bsp_node_count\":1,")
		TEXT("\"bsp_surface_count\":1,\"encoding\":\"ue1-world-records-le-v3\",")
		TEXT("\"fact_id\":\"spec.world.%s\",\"importer\":\"world-v1\",")
		TEXT("\"importer_version\":3,\"kind\":\"world\",\"light_count\":0,")
		TEXT("\"map_identity\":\"%s\",\"mover_count\":1,")
		TEXT("\"navigation_seed_count\":1,\"payload_byte_length\":%d,")
		TEXT("\"portal_surface_count\":0,\"reach_spec_count\":1,")
		TEXT("\"semantic_object_count\":11,\"sound_count\":0,")
		TEXT("\"source\":{\"export\":\"Synthetic.World\",")
		TEXT("\"package\":\"Synthetic/World.u\",\"sha256\":")
		TEXT("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},")
		TEXT("\"zone_count\":1}"), *MapIdentity, *MapIdentity, Payload.Num());
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

TArray<uint8> BSPObject(
	const FString& MapIdentity, FWoTSha256& OutPayloadSha256)
{
	TArray<uint8> Payload = {'W', 'O', 'T', 'P', 'B', 'S', 'P', 0};
	AppendU16(Payload, 1);
	AppendU16(Payload, 2); // main, canonical tangent streams present
	AppendU32(Payload, 3);
	AppendU32(Payload, 3);
	AppendU32(Payload, 1);
	AppendU16(Payload, 2);
	AppendU16(Payload, 1);
	for (float Bound : {0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0.0f})
	{
		AppendFloat(Payload, Bound);
	}
	const FTCHARToUTF8 Slot(TEXT("Default"));
	AppendU16(Payload, static_cast<uint16>(Slot.Length()));
	AppendU16(Payload, 0);
	AppendU32(Payload, 0);
	AppendU32(Payload, 3);
	AppendU32(Payload, 0);
	AppendU32(Payload, 2);
	Payload.Add(1);
	Payload.AddZeroed(3);
	Payload.Append(
		reinterpret_cast<const uint8*>(Slot.Get()), Slot.Length());
	for (const FVector3f& Position : {
			FVector3f(0, 0, 0), FVector3f(100, 0, 0),
			FVector3f(0, 100, 0)})
	{
		AppendFloat(Payload, Position.X);
		AppendFloat(Payload, Position.Y);
		AppendFloat(Payload, Position.Z);
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 0.0f);
		AppendFloat(Payload, 1.0f);
	}
	for (int32 Channel = 0; Channel < 2; ++Channel)
	{
		for (const FVector2f& UV : {
				FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)})
		{
			AppendFloat(Payload, UV.X);
			AppendFloat(Payload, UV.Y);
		}
	}
	AppendU32(Payload, 0);
	AppendU32(Payload, 1);
	AppendU32(Payload, 2);
	OutPayloadSha256 = Sha256(Payload);
	const FString FactId = FString(TEXT("spec.bsp.")) + MapIdentity;
	const FString Header = FString::Printf(
		TEXT("{\"bounds_max_bits\":[\"42c80000\",\"42c80000\",\"00000000\"],")
		TEXT("\"bounds_min_bits\":[\"00000000\",\"00000000\",\"00000000\"],")
		TEXT("\"encoding\":\"wot-bsp-static-mesh-v1\",")
		TEXT("\"fact_id\":\"%s\",\"importer\":\"bsp-v1\",")
		TEXT("\"importer_version\":2,\"index_count\":3,\"kind\":\"bsp\",")
		TEXT("\"payload_byte_length\":%d,\"role\":\"main\",")
		TEXT("\"section_count\":1,\"source\":{\"export\":\"Synthetic.Model\",")
		TEXT("\"package\":\"Synthetic/World.u\",\"sha256\":")
		TEXT("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},")
		TEXT("\"tangent_algorithm\":\"ue-mikk-preserve-normals-v1\",")
		TEXT("\"triangle_count\":1,\"uv_channels\":2,\"vertex_count\":3}"),
		*FactId, Payload.Num());
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}

bool WoTMatTestDataError(FWoTBootstrapError&& Failure, FString& OutError)
{
	OutError = Failure.ToString();
	return false;
}
} // namespace

TArray<uint8> WoTWorldMaterializerTest_MalformedPoolValueObject()
{
	return WorldObject(TEXT("MalformedPoolValue"), true, true);
}

FWoTRetailContract WoTWorldMaterializerTest_WorldContract()
{
	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("bootstrap-activation-world-set");
	Contract.KeyId = TEXT("bootstrap-activation-world-key");
	Contract.ReleaseSequence = 7;
	Contract.StartupCriticalLogicalIds = {TEXT("cache/world/Arena_01")};
	TWoTBootstrapResult<FWoTSha256> CriticalDigest = FWoTSha256::FromHex(
		TEXT("96f79ca528ade0bc9a7b80b0db1f1c4705417148fb150532dc4dd090cb37cbb5"));
	if (!CriticalDigest.IsError())
	{
		Contract.StartupCriticalLogicalIdsSha256 = CriticalDigest.GetValue();
	}
	Contract.AuditSet.Add({TEXT("FWoTWorldMaterializer"),
		TEXT("audit.world-payload-v1")});
	Contract.AuditSet.Add({TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.bsp-payload-v1")});
	for (int32 Slot = 1; Slot <= 19; ++Slot)
	{
		const WoT::Retail::Generated::FRequiredWorld& Required =
			WoT::Retail::Generated::RequiredWorlds[Slot - 1];
		const FString Identity(Required.Identity);
		FWoTRequiredFact& World = Contract.Facts.AddDefaulted_GetRef();
		World.Domain = EWoTFactDomain::World;
		World.FactId = FString(TEXT("spec.world.")) + Identity;
		World.SourcePath = Required.SourcePath;
		World.SourceExport = Identity;
		World.Importer = TEXT("world-v1");
		World.RuntimeConsumer = TEXT("FWoTWorldMaterializer");
		World.AuditRule = TEXT("audit.world-payload-v1");
		FWoTRequiredFactOutput& WorldOutput =
			World.Outputs.AddDefaulted_GetRef();
		WorldOutput.LogicalId =
			FString::Printf(TEXT("cache/world/%s"), *Identity);
		WorldOutput.Schema = TEXT("wot-world-v3");
		WorldOutput.MaxDecodedSize = 1024ull * 1024ull * 1024ull;

		FWoTRequiredFact& BSP = Contract.Facts.AddDefaulted_GetRef();
		BSP.Domain = EWoTFactDomain::BSP;
		BSP.FactId = FString(TEXT("spec.bsp.")) + Identity;
		BSP.SourcePath = Required.SourcePath;
		BSP.SourceExport = Identity + TEXT(".Model");
		BSP.Importer = TEXT("bsp-v1");
		BSP.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
		BSP.AuditRule = TEXT("audit.bsp-payload-v1");
		FWoTRequiredFactOutput& BSPOutput =
			BSP.Outputs.AddDefaulted_GetRef();
		BSPOutput.LogicalId =
			FString::Printf(TEXT("cache/bsp/%s/Main"), *Identity);
		BSPOutput.Schema = TEXT("wot-bsp-v3");
		BSPOutput.MaxDecodedSize = 1024ull * 1024ull * 1024ull;
	}
	FWoTRequiredFact& FrontEndWorld = Contract.Facts.AddDefaulted_GetRef();
	FrontEndWorld.Domain = EWoTFactDomain::World;
	FrontEndWorld.FactId = TEXT("spec.frontend.world.WheelOfTime");
	FrontEndWorld.SourcePath = TEXT("Maps/WheelOfTime.wot");
	FrontEndWorld.SourceExport = TEXT("WheelOfTime.Level");
	FrontEndWorld.Importer = TEXT("world-v1");
	FrontEndWorld.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	FrontEndWorld.AuditRule = TEXT("audit.world-payload-v1");
	FrontEndWorld.Outputs.Add({TEXT("cache/world/WheelOfTime"),
		TEXT("wot-world-v3"), 1024ull * 1024ull * 1024ull});
	FWoTRequiredFact& FrontEndBSP = Contract.Facts.AddDefaulted_GetRef();
	FrontEndBSP.Domain = EWoTFactDomain::BSP;
	FrontEndBSP.FactId = TEXT("spec.frontend.bsp.WheelOfTime");
	FrontEndBSP.SourcePath = TEXT("Maps/WheelOfTime.wot");
	FrontEndBSP.SourceExport = TEXT("WheelOfTime.Model");
	FrontEndBSP.Importer = TEXT("bsp-v1");
	FrontEndBSP.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	FrontEndBSP.AuditRule = TEXT("audit.bsp-payload-v1");
	FrontEndBSP.Outputs.Add({TEXT("cache/bsp/WheelOfTime/WheelOfTime"),
		TEXT("wot-bsp-v3"), 1024ull * 1024ull * 1024ull});
	return Contract;
}

TWoTBootstrapResult<FWoTCacheBuildResult>
WoTWorldMaterializerTest_BuildWorldSet(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTCacheBuildResult>;
	TWoTBootstrapResult<FWoTSha256> DescriptorIdentity =
		GetCacheExpectedProductionDescriptorSha256();
	if (DescriptorIdentity.IsError())
	{
		return FResult::Error(MoveTemp(DescriptorIdentity).GetError());
	}
	FWoTCacheBuildResult Build;
	Build.Manifest.bHasBuildIdentity = true;
	Build.Manifest.InstallerSha256 = Package.CachedInputSha256();
	Build.Manifest.ContractSha256 = ComputeRetailContractIdentity(Contract);
	Build.Manifest.ProductionDescriptorSha256 =
		DescriptorIdentity.GetValue();
	constexpr uint64 TotalObjects = 38;
	FWoTBootstrapStatus Reported =
		Progress.ReportProgress(TEXT("required-world-set"), 0, TotalObjects);
	if (Reported.IsError())
	{
		return FResult::Error(MoveTemp(Reported).GetError());
	}
	uint64 Completed = 0;
	for (int32 Slot = 1; Slot <= 19; ++Slot)
	{
		const FString Identity(
			WoT::Retail::Generated::RequiredWorlds[Slot - 1].Identity);
		const FString WorldLogicalId =
			FString::Printf(TEXT("cache/world/%s"), *Identity);
		const TArray<uint8> WorldBytes = WorldObject(Identity);
		FWoTBootstrapStatus Written = Writer.WriteObject(
			WorldLogicalId, TEXT("wot-world-v3"), WorldBytes, Sha256(WorldBytes),
			1024ull * 1024ull * 1024ull);
		if (Written.IsError())
		{
			return FResult::Error(MoveTemp(Written).GetError());
		}
		FWoTCacheBlob& WorldBlob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		WorldBlob.LogicalId = WorldLogicalId;
		WorldBlob.Schema = TEXT("wot-world-v3");
		WorldBlob.Sha256 = Sha256(WorldBytes);
		WorldBlob.Size = static_cast<uint64>(WorldBytes.Num());
		WorldBlob.Satisfies.Add({FString(TEXT("spec.world.")) + Identity});

		FWoTWorldDescriptor& WorldDescriptor =
			Build.Manifest.WorldDescriptors.AddDefaulted_GetRef();
		WorldDescriptor.MapIdentity = Identity;
		WorldDescriptor.NeutralMapSlot = static_cast<uint32>(Slot);
		WorldDescriptor.BSPFactId = FString(TEXT("spec.bsp.")) + Identity;
		WorldDescriptor.BSPImporter = TEXT("bsp-v1");
		WorldDescriptor.WorldLogicalId = WorldLogicalId;
		WorldDescriptor.WorldSchema = TEXT("wot-world-v3");
		WorldDescriptor.WorldFactId =
			FString(TEXT("spec.world.")) + Identity;
		WorldDescriptor.WorldImporter = TEXT("world-v1");
		WorldDescriptor.PackageSha256 = FWoTSha256::FromHex(
			TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))
			.GetValue();
		WorldDescriptor.WorldSha256 = WorldBlob.Sha256;
		WorldDescriptor.SourceRoot =
			SeedDigest(static_cast<uint8>(70 + Slot));
		WorldDescriptor.MapRoot =
			SeedDigest(static_cast<uint8>(100 + Slot));
		WorldDescriptor.ActorCount = 1;
		WorldDescriptor.BSPNodeCount = 1;
		WorldDescriptor.BSPSurfaceCount = 1;
		WorldDescriptor.ZoneCount = 1;
		WorldDescriptor.NavigationSeedCount = 1;
		WorldDescriptor.SemanticObjectCount = 2;
		WorldDescriptor.ReachSpecCount = 1;
		Reported = Progress.ReportProgress(
			TEXT("required-world-set"), ++Completed, TotalObjects);
		if (Reported.IsError())
		{
			return FResult::Error(MoveTemp(Reported).GetError());
		}

		FWoTSha256 BSPPayloadSha256;
		const TArray<uint8> BSPBytes = BSPObject(Identity, BSPPayloadSha256);
		const FString BSPLogicalId =
			FString::Printf(TEXT("cache/bsp/%s/Main"), *Identity);
		Written = Writer.WriteObject(BSPLogicalId, TEXT("wot-bsp-v3"),
			BSPBytes, Sha256(BSPBytes), 1024ull * 1024ull * 1024ull);
		if (Written.IsError())
		{
			return FResult::Error(MoveTemp(Written).GetError());
		}
		FWoTCacheBlob& BSPBlob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		BSPBlob.LogicalId = BSPLogicalId;
		BSPBlob.Schema = TEXT("wot-bsp-v3");
		BSPBlob.Sha256 = Sha256(BSPBytes);
		BSPBlob.Size = static_cast<uint64>(BSPBytes.Num());
		BSPBlob.Satisfies.Add({WorldDescriptor.BSPFactId});
		FWoTBSPPartDescriptor& BSPPart =
			WorldDescriptor.BSPParts.AddDefaulted_GetRef();
		BSPPart.Role = EWoTBSPPartRole::Main;
		BSPPart.PartIdentity = Identity + TEXT("_Main");
		BSPPart.LogicalId = BSPLogicalId;
		BSPPart.Schema = TEXT("wot-bsp-v3");
		BSPPart.FactId = WorldDescriptor.BSPFactId;
		BSPPart.PayloadSha256 = BSPPayloadSha256;
		BSPPart.Sha256 = BSPBlob.Sha256;
		BSPPart.VertexCount = 3;
		BSPPart.IndexCount = 3;
		BSPPart.SectionCount = 1;
		Reported = Progress.ReportProgress(
			TEXT("required-world-set"), ++Completed, TotalObjects);
		if (Reported.IsError())
		{
			return FResult::Error(MoveTemp(Reported).GetError());
		}
	}
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError())
	{
		return FResult::Error(MoveTemp(Manifest).GetError());
	}
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);

	Build.Progress = FWoTCacheProgressSnapshot(
		TEXT("required-world-set"), TotalObjects, TotalObjects);
	return FResult::Success(MoveTemp(Build));
}

bool WoTWorldMaterializerTest_PublishWorldSet(
	const FString& Root, TArray<FWoTWorldDescriptor>& OutDescriptors,
	FString& OutError, const FWoTFrontEndDescriptor* FrontEndDescriptor,
	uint8 FrontEndAttack, bool bIncludeLazyRuntimeAnimation)
{
	TWoTBootstrapResult<FWoTCacheStore> Created =
		FWoTCacheStore::ForTesting(Root);
	if (Created.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Created).GetError(), OutError);
	}
	FWoTCacheStore Store = MoveTemp(Created).GetValue();
	TWoTBootstrapResult<FWoTProfileSession> SessionResult =
		AcquireWoTProfileSessionShared(FPaths::GetPath(Root));
	if (SessionResult.IsError())
	{
		return WoTMatTestDataError(
			MoveTemp(SessionResult).GetError(), OutError);
	}
	FWoTProfileSession Session = MoveTemp(SessionResult).GetValue();
	TWoTBootstrapResult<bool> Exclusive = Session.TryAcquireExclusive();
	if (Exclusive.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Exclusive).GetError(), OutError);
	}
	if (!Exclusive.GetValue())
	{
		OutError = TEXT("the world fixture profile session is contended");
		return false;
	}
	FWoTBootstrapStatus Mutation =
		Store.BeginBuild(Session);
	if (Mutation.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Mutation).GetError(), OutError);
	}

	FWoTCacheBuildResult Build;
	Build.Manifest.bHasBuildIdentity = true;
	Build.Manifest.InstallerSha256 = SeedDigest(11);
	Build.Manifest.ContractSha256 = SeedDigest(22);
	Build.Manifest.ProductionDescriptorSha256 = SeedDigest(33);
	for (int32 Slot = 1; Slot <= 19; ++Slot)
	{
		const FString Identity(
			WoT::Retail::Generated::RequiredWorlds[Slot - 1].Identity);
		const FString LogicalId = FString::Printf(TEXT("cache/world/%s"), *Identity);
		const TArray<uint8> Object = GameplayWorldObject(Identity);
		FWoTBootstrapStatus Written = Store.WriteObject(
			LogicalId, TEXT("wot-world-v3"), Object, Sha256(Object),
			1024ull * 1024ull * 1024ull);
		if (Written.IsError())
		{
			return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
		}
		FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = LogicalId;
		Blob.Schema = TEXT("wot-world-v3");
		Blob.Sha256 = Sha256(Object);
		Blob.Size = static_cast<uint64>(Object.Num());
		Blob.Satisfies.Add(FWoTRequiredFactId{
			FString(TEXT("spec.world.")) + Identity});

		FWoTWorldDescriptor& Descriptor = OutDescriptors.AddDefaulted_GetRef();
		Descriptor.MapIdentity = Identity;
		Descriptor.NeutralMapSlot = static_cast<uint32>(Slot);
		Descriptor.BSPFactId = FString(TEXT("spec.bsp.")) + Identity;
		Descriptor.BSPImporter = TEXT("bsp-v1");
		Descriptor.WorldLogicalId = LogicalId;
		Descriptor.WorldSchema = TEXT("wot-world-v3");
		Descriptor.WorldFactId = FString(TEXT("spec.world.")) + Identity;
		Descriptor.WorldImporter = TEXT("world-v1");
		Descriptor.PackageSha256 = FWoTSha256::FromHex(
			TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))
			.GetValue();
		Descriptor.WorldSha256 = Blob.Sha256;
		Descriptor.SourceRoot = SeedDigest(static_cast<uint8>(70 + Slot));
		Descriptor.MapRoot = SeedDigest(static_cast<uint8>(100 + Slot));
		Descriptor.ActorCount = 7;
		Descriptor.BSPNodeCount = 1;
		Descriptor.BSPSurfaceCount = 1;
		Descriptor.ZoneCount = 1;
		Descriptor.NavigationSeedCount = 1;
		Descriptor.SemanticObjectCount = 11;
		Descriptor.MoverCount = 1;
		Descriptor.ReachSpecCount = 1;

		FWoTSha256 BSPPayloadSha256;
		const TArray<uint8> BSPObjectBytes =
			BSPObject(Identity, BSPPayloadSha256);
		const FString BSPLogicalId = FString::Printf(
			TEXT("cache/bsp/%s/Main"), *Identity);
		FWoTBootstrapStatus BSPWritten = Store.WriteObject(
			BSPLogicalId, TEXT("wot-bsp-v3"), BSPObjectBytes,
			Sha256(BSPObjectBytes),
			1024ull * 1024ull * 1024ull);
		if (BSPWritten.IsError())
		{
			return WoTMatTestDataError(MoveTemp(BSPWritten).GetError(), OutError);
		}
		FWoTCacheBlob& BSPBlob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		BSPBlob.LogicalId = BSPLogicalId;
		BSPBlob.Schema = TEXT("wot-bsp-v3");
		BSPBlob.Sha256 = Sha256(BSPObjectBytes);
		BSPBlob.Size = static_cast<uint64>(BSPObjectBytes.Num());
		BSPBlob.Satisfies.Add(FWoTRequiredFactId{Descriptor.BSPFactId});
		FWoTBSPPartDescriptor& BSPPart =
			Descriptor.BSPParts.AddDefaulted_GetRef();
		BSPPart.Role = EWoTBSPPartRole::Main;
		BSPPart.PartIdentity = Identity + TEXT("_Main");
		BSPPart.LogicalId = BSPLogicalId;
		BSPPart.Schema = TEXT("wot-bsp-v3");
		BSPPart.FactId = Descriptor.BSPFactId;
		BSPPart.PayloadSha256 = BSPPayloadSha256;
		BSPPart.Sha256 = BSPBlob.Sha256;
		BSPPart.VertexCount = 3;
		BSPPart.IndexCount = 3;
		BSPPart.SectionCount = 1;
	}
	if (FrontEndDescriptor != nullptr)
	{
		const FString& FrontEndWorldLogicalId =
			WoTFrontEndTest_WorldLogicalId(*FrontEndDescriptor);
		const FString& FrontEndBSPLogicalId =
			WoTFrontEndTest_BSPLogicalId(*FrontEndDescriptor);
		const TArray<uint8> FrontEndWorldBytes = WorldObject(TEXT("WheelOfTime"));
		FWoTBootstrapStatus Written = FWoTBootstrapStatus::Success();
		if (FrontEndAttack != WoTFrontEndWorldManifestAttackMissingWorld)
		{
			Written = Store.WriteObject(
				FrontEndWorldLogicalId, TEXT("wot-world-v3"),
				FrontEndWorldBytes, Sha256(FrontEndWorldBytes),
				1024ull * 1024ull * 1024ull);
			if (Written.IsError())
			{
				return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
			}
			FWoTCacheBlob& FrontEndWorldBlob =
				Build.Manifest.Blobs.AddDefaulted_GetRef();
			FrontEndWorldBlob.LogicalId = FrontEndWorldLogicalId;
			FrontEndWorldBlob.Schema = TEXT("wot-world-v3");
			FrontEndWorldBlob.Sha256 = Sha256(FrontEndWorldBytes);
			FrontEndWorldBlob.Size = static_cast<uint64>(FrontEndWorldBytes.Num());
			FrontEndWorldBlob.Satisfies.Add({FrontEndAttack
				== WoTFrontEndWorldManifestAttackWrongWorldFact
				? TEXT("spec.frontend.world.Wrong")
				: TEXT("spec.frontend.world.WheelOfTime")});
		}

		FWoTSha256 FrontEndBSPPayloadSha256;
		const TArray<uint8> FrontEndBSPBytes =
			BSPObject(TEXT("WheelOfTime"), FrontEndBSPPayloadSha256);
		if (FrontEndAttack != WoTFrontEndWorldManifestAttackMissingBSP)
		{
			const TCHAR* FrontEndBSPSchema = FrontEndAttack
				== WoTFrontEndWorldManifestAttackWrongBSPSchema
				? TEXT("wot-bsp-v4") : TEXT("wot-bsp-v3");
			Written = Store.WriteObject(
				FrontEndBSPLogicalId, FrontEndBSPSchema,
				FrontEndBSPBytes, Sha256(FrontEndBSPBytes),
				1024ull * 1024ull * 1024ull);
			if (Written.IsError())
			{
				return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
			}
			FWoTCacheBlob& FrontEndBSPBlob =
				Build.Manifest.Blobs.AddDefaulted_GetRef();
			FrontEndBSPBlob.LogicalId = FrontEndBSPLogicalId;
			FrontEndBSPBlob.Schema = FrontEndBSPSchema;
			FrontEndBSPBlob.Sha256 = Sha256(FrontEndBSPBytes);
			FrontEndBSPBlob.Size = static_cast<uint64>(FrontEndBSPBytes.Num());
			FrontEndBSPBlob.Satisfies.Add(
				{TEXT("spec.frontend.bsp.WheelOfTime")});
		}

		if (FrontEndAttack == WoTFrontEndWorldManifestAttackExtraWorld)
		{
			const FString ExtraLogicalId = TEXT("cache/world/WheelOfTime/Extra");
			Written = Store.WriteObject(ExtraLogicalId, TEXT("wot-world-v3"),
				FrontEndWorldBytes, Sha256(FrontEndWorldBytes),
				1024ull * 1024ull * 1024ull);
			if (Written.IsError())
			{
				return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
			}
			FWoTCacheBlob& Extra = Build.Manifest.Blobs.AddDefaulted_GetRef();
			Extra.LogicalId = ExtraLogicalId;
			Extra.Schema = TEXT("wot-world-v3");
			Extra.Sha256 = Sha256(FrontEndWorldBytes);
			Extra.Size = static_cast<uint64>(FrontEndWorldBytes.Num());
			Extra.Satisfies.Add({TEXT("spec.frontend.world.WheelOfTime")});
		}
		else if (FrontEndAttack == WoTFrontEndWorldManifestAttackExtraBSP)
		{
			const FString ExtraLogicalId = TEXT("cache/bsp/WheelOfTime/Extra");
			Written = Store.WriteObject(ExtraLogicalId, TEXT("wot-bsp-v3"),
				FrontEndBSPBytes, Sha256(FrontEndBSPBytes),
				1024ull * 1024ull * 1024ull);
			if (Written.IsError())
			{
				return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
			}
			FWoTCacheBlob& Extra = Build.Manifest.Blobs.AddDefaulted_GetRef();
			Extra.LogicalId = ExtraLogicalId;
			Extra.Schema = TEXT("wot-bsp-v3");
			Extra.Sha256 = Sha256(FrontEndBSPBytes);
			Extra.Size = static_cast<uint64>(FrontEndBSPBytes.Num());
			Extra.Satisfies.Add({TEXT("spec.frontend.bsp.WheelOfTime")});
		}

		// Retain the complete descriptor closure for use-time recovery specs.
		// The required fonts and their atlases are canonical because production
		// activation materializes them eagerly; other resources remain opaque
		// critical-closure records until their respective loaders consume them.
		TArray<uint8> FontFixture;
		if (!FFileHelper::LoadFileToArray(FontFixture,
			*WoTRepositoryOrReferencePath(
				TEXT("Tools/dist/fixtures/cache-v3-font-pages.bin"))))
		{
			OutError = TEXT("the canonical front-end font fixture is unavailable");
			return false;
		}
		for (int32 ResourceIndex = 0;
			ResourceIndex < FrontEndDescriptor->RequiredResources.Num();
			++ResourceIndex)
		{
			const FWoTFrontEndResourceRequirement& Requirement =
				FrontEndDescriptor->RequiredResources[ResourceIndex];
			if (Build.Manifest.Blobs.ContainsByPredicate(
				[&Requirement](const FWoTCacheBlob& Existing)
				{
					return Existing.LogicalId == Requirement.LogicalId;
				})) continue;
			const TCHAR* Schema = TEXT("wot-front-end-retained-test-v1");
			switch (Requirement.Domain)
			{
			case EWoTFactDomain::Texture: Schema = TEXT("wot-texture-v3"); break;
			case EWoTFactDomain::Sound: Schema = TEXT("wot-sound-v3"); break;
			case EWoTFactDomain::Mesh: Schema = TEXT("wot-mesh-v3"); break;
			case EWoTFactDomain::Animation: Schema = TEXT("wot-animation-v3"); break;
			case EWoTFactDomain::Font: Schema = TEXT("wot-font-v3"); break;
			case EWoTFactDomain::Procedural: Schema = TEXT("wot-procedural-v3"); break;
			case EWoTFactDomain::Localization: Schema = TEXT("wot-localization-v3"); break;
			case EWoTFactDomain::ClassDefault: Schema = TEXT("wot-class-default-v3"); break;
			default: break;
			}
			TArray<uint8> RetainedBytes;
			if (Requirement.Domain == EWoTFactDomain::Font)
			{
				const FString PageSourceExport =
					Requirement.SourceExport.Contains(TEXT("Reg30"))
					? TEXT("WOT.F_WOTReg30.Texture10")
					: TEXT("WOT.F_WOTReg14.Texture9");
				const FWoTFrontEndResourceRequirement* Page =
					FrontEndDescriptor->RequiredResources.FindByPredicate(
						[&PageSourceExport](const auto& Candidate)
						{
							return Candidate.Domain == EWoTFactDomain::Texture
								&& Candidate.SourceExport == PageSourceExport;
						});
				if (Page == nullptr)
				{
					OutError = TEXT("the front-end font fixture has no atlas closure");
					return false;
				}
				RetainedBytes = RuntimeFontObject(FontFixture,
					Requirement.SourceExport, Page->LogicalId,
					Page->SourceExport);
			}
			else if (Requirement.Domain == EWoTFactDomain::Texture
				&& (Requirement.SourceExport == TEXT("WOT.F_WOTReg30.Texture10")
					|| Requirement.SourceExport == TEXT("WOT.F_WOTReg14.Texture9")))
			{
				RetainedBytes = RuntimeFontAtlasObject(Requirement.SourceExport);
			}
			else
			{
				const FTCHARToUTF8 Utf8(*Requirement.LogicalId);
				RetainedBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()),
					Utf8.Length());
			}
			if (RetainedBytes.IsEmpty())
			{
				OutError = TEXT("the front-end retained object fixture could not be encoded");
				return false;
			}
			Written = Store.WriteObject(Requirement.LogicalId, Schema,
				RetainedBytes, Sha256(RetainedBytes), 1024ull * 1024ull);
			if (Written.IsError())
				return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
			FWoTCacheBlob& Retained = Build.Manifest.Blobs.AddDefaulted_GetRef();
			Retained.LogicalId = Requirement.LogicalId;
			Retained.Schema = Schema;
			Retained.Size = RetainedBytes.Num();
			Retained.Sha256 = Sha256(RetainedBytes);
			Retained.Satisfies.Add({FString::Printf(
				TEXT("spec.frontend.retained.%04d"), ResourceIndex)});
		}

		TWoTBootstrapResult<TArray<uint8>> FrontEndSidecar =
			WoTFrontEndTest_WriteDescriptorSidecar(*FrontEndDescriptor,
				Build.Manifest.InstallerSha256, Build.Manifest.ContractSha256,
				Build.Manifest.ProductionDescriptorSha256);
		if (FrontEndSidecar.IsError())
		{
			return WoTMatTestDataError(
				MoveTemp(FrontEndSidecar).GetError(), OutError);
		}
		Written = Store.WriteObject(
			TEXT("cache/front_end_world/WheelOfTime"),
			TEXT("wot-front-end-world-v3"), FrontEndSidecar.GetValue(),
			Sha256(FrontEndSidecar.GetValue()), 4ull * 1024ull * 1024ull);
		if (Written.IsError())
		{
			return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
		}
		FWoTCacheBlob& FrontEndSidecarBlob =
			Build.Manifest.Blobs.AddDefaulted_GetRef();
		FrontEndSidecarBlob.LogicalId =
			TEXT("cache/front_end_world/WheelOfTime");
		FrontEndSidecarBlob.Schema = TEXT("wot-front-end-world-v3");
		FrontEndSidecarBlob.Sha256 = Sha256(FrontEndSidecar.GetValue());
		FrontEndSidecarBlob.Size =
			static_cast<uint64>(FrontEndSidecar.GetValue().Num());
		FrontEndSidecarBlob.Satisfies.Add(
			{TEXT("spec.frontend.descriptor.WheelOfTime")});

		Build.Manifest.FrontEndDescriptorRoot =
			WoTFrontEndTest_DescriptorRoot(*FrontEndDescriptor);
	}

	if (bIncludeLazyRuntimeAnimation)
	{
		const FString MeshLogicalId(TEXT("cache/mesh/RuntimeIntegritySpec"));
		TArray<uint8> MeshBytes;
		if (!FFileHelper::LoadFileToArray(MeshBytes,
			*WoTRepositoryOrReferencePath(
				TEXT("Tools/dist/fixtures/cache-v3-mesh-plain.bin"))))
		{
			OutError = TEXT("the lazy runtime mesh fixture is unavailable");
			return false;
		}
		FWoTBootstrapStatus MeshWritten = Store.WriteObject(MeshLogicalId,
			TEXT("wot-mesh-v3"), MeshBytes, Sha256(MeshBytes),
			1024ull * 1024ull);
		if (MeshWritten.IsError())
		{
			return WoTMatTestDataError(MoveTemp(MeshWritten).GetError(), OutError);
		}
		FWoTCacheBlob& MeshBlob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		MeshBlob.LogicalId = MeshLogicalId;
		MeshBlob.Schema = TEXT("wot-mesh-v3");
		MeshBlob.Sha256 = Sha256(MeshBytes);
		MeshBlob.Size = MeshBytes.Num();
		MeshBlob.Satisfies.Add({TEXT("spec.runtime.mesh")});

		const FString LogicalId(TEXT("cache/animation/RuntimeIntegritySpec"));
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes,
			*WoTRepositoryOrReferencePath(
				TEXT("Tools/dist/fixtures/cache-v3-animation-plain.bin"))))
		{
			OutError = TEXT("the lazy runtime animation fixture is unavailable");
			return false;
		}
		FWoTBootstrapStatus Written = Store.WriteObject(LogicalId,
			TEXT("wot-animation-v3"), Bytes, Sha256(Bytes), 1024ull * 1024ull);
		if (Written.IsError())
		{
			return WoTMatTestDataError(MoveTemp(Written).GetError(), OutError);
		}
		FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = LogicalId;
		Blob.Schema = TEXT("wot-animation-v3");
		Blob.Sha256 = Sha256(Bytes);
		Blob.Size = Bytes.Num();
		Blob.Satisfies.Add({TEXT("spec.runtime.animation")});
	}

	for (const TCHAR* PackageName : ClassDefaultPackages)
	{
		const FString LogicalId=FString(TEXT("cache/class_default/"))+PackageName;
		const TArray<uint8> Bytes=ClassDefaultsObject(PackageName);
		FWoTBootstrapStatus Written=Store.WriteObject(LogicalId,
			TEXT("wot-class-default-v3"),Bytes,Sha256(Bytes),256ull*1024ull*1024ull);
		if(Written.IsError())return WoTMatTestDataError(MoveTemp(Written).GetError(),OutError);
		FWoTCacheBlob& Blob=Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.LogicalId=LogicalId; Blob.Schema=TEXT("wot-class-default-v3");
		Blob.Sha256=Sha256(Bytes); Blob.Size=Bytes.Num();
		Blob.Satisfies.Add({FString(TEXT("retail.class_default."))+FString(PackageName).ToLower()});
	}
	Build.Manifest.WorldDescriptors = OutDescriptors;
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Manifest).GetError(), OutError);
	}
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);

	FWoTBootstrapStatus Published =
		Store.Publish(Build);
	if (Published.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Published).GetError(), OutError);
	}
	FWoTBootstrapStatus Restored = Session.RestoreShared();
	if (Restored.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Restored).GetError(), OutError);
	}
	return true;
}

bool WoTWorldMaterializerTest_ActivateAndAuthorize(
	UWoTPayloadSubsystem* Subsystem, const FString& Root,
	TArray<FWoTWorldDescriptor>&& Descriptors, FString& OutError)
{
	TWoTBootstrapResult<FWoTCacheStore> Opened = FWoTCacheStore::ForTesting(Root);
	if (Opened.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Opened).GetError(), OutError);
	}
	FWoTCacheStore Store = MoveTemp(Opened).GetValue();
	TWoTBootstrapResult<FWoTCacheGenerationLease> ExactLease =
		WoTAcquireCurrentForTesting(Store);
	if (ExactLease.IsError())
	{
		return WoTMatTestDataError(MoveTemp(ExactLease).GetError(), OutError);
	}
	FWoTBootstrapStatus Activated =
		WoTPayloadActivationTest_ActivateLeaseForWorldAuthorization(
			Subsystem, ExactLease.GetValue());
	if (Activated.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Activated).GetError(), OutError);
	}
	FWoTBootstrapStatus Authorized = WoTAuthorizeNormalWorldMaterialization(
		ExactLease.GetValue(), MoveTemp(Descriptors));
	if (Authorized.IsError())
	{
		return WoTMatTestDataError(MoveTemp(Authorized).GetError(), OutError);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
