// Headless specs for authenticated cache-v3 texture objects and their runtime seam.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFractalData.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTGameInstance.h"
#include "../WoTMenuStyle.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSha256.h"
#include "WoTPayloadActivationTestSupport.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// ProcessCompletedPackage is deliberately private (the delegate handler
	// HandlePackageLoadCompleted only queues packages for the next world tick;
	// the processor holds the actual binding logic these specs exercise). This
	// standard explicit-instantiation accessor lets the spec invoke that exact
	// production entry point without widening the production API for tests.
	template<typename TWoTPayloadTests_Tag,
		typename TWoTPayloadTests_Tag::Type WoTPayloadTests_Member>
	struct TWoTPayloadTests_PrivateMemberAccessor
	{
		friend typename TWoTPayloadTests_Tag::Type WoTPayloadTests_GetPrivateMember(
			TWoTPayloadTests_Tag)
		{
			return WoTPayloadTests_Member;
		}
	};

	struct FWoTPayloadTests_PackageLoadCompletedTag
	{
		using Type = void (UWoTPayloadSubsystem::*)(UPackage*);
		friend Type WoTPayloadTests_GetPrivateMember(
			FWoTPayloadTests_PackageLoadCompletedTag);
	};

	template struct TWoTPayloadTests_PrivateMemberAccessor<
		FWoTPayloadTests_PackageLoadCompletedTag,
		&UWoTPayloadSubsystem::ProcessCompletedPackage>;

	enum class EWoTPayloadTests_FixtureMode
	{
		Valid,
		OversizedDimensions,
		LyingLowerMipDimensions,
		LyingBlobLowerMipDimensions,
		BadKeyGrammar,
		TruncatedBlob,
		OldImporterVersion,
	};

	struct FWoTPayloadTests_Fixture
	{
		FString CacheDirectory;
		FString CacheKey;
		FString BlobPath;
		FString BlobSHA256;
		TArray<uint8> CompleteBlob;
		bool bPublished = false;
		FString PublishError;
	};

	struct FWoTPayloadTests_World
	{
		UWorld* World = nullptr;
		UWoTGameInstance* GameInstance = nullptr;
		bool bActivated = false;
		FString ActivationError;
	};

	struct FWoTPayloadTests_MenuFixture
	{
		FString CacheDirectory;
		bool bPublished = false;
		FString PublishError;
	};

	void WoTPayloadTests_AppendU16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	void WoTPayloadTests_AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadTests_AppendU64(TArray<uint8>& Bytes, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadTests_AppendASCII(TArray<uint8>& Bytes, const FString& Value)
	{
		for (TCHAR Character : Value)
		{
			check(Character <= 127);
			Bytes.Add(static_cast<uint8>(Character));
		}
	}

	FString WoTPayloadTests_SHA256(const void* Data, uint32 Size)
	{
		return WoTSha256::HashBuffer(Data, Size);
	}

	FString WoTPayloadTests_CacheRoot(const FString& CanonicalCore,
		const FString& BlobPath, uint64 BlobSize, const FString& BlobSHA256)
	{
		TArray<uint8> Input;
		WoTPayloadTests_AppendASCII(Input, TEXT("WOT-PAYLOAD-CACHE-HASH-V1"));
		Input.Add(0);
		WoTPayloadTests_AppendU64(Input, static_cast<uint64>(CanonicalCore.Len()));
		WoTPayloadTests_AppendASCII(Input, CanonicalCore);

		TArray<uint8> Record;
		WoTPayloadTests_AppendASCII(Record, BlobPath);
		Record.Add(0);
		WoTPayloadTests_AppendASCII(Record, LexToString(BlobSize));
		Record.Add(0);
		WoTPayloadTests_AppendASCII(Record, BlobSHA256);
		Record.Add('\n');
		WoTPayloadTests_AppendU64(Input, static_cast<uint64>(Record.Num()));
		Input.Append(Record);
		return WoTPayloadTests_SHA256(Input.GetData(), Input.Num());
	}

	TArray<uint8> WoTPayloadTests_TextureBlob()
	{
		TArray<uint8> Blob;
		const uint8 Magic[8] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
		Blob.Append(Magic, UE_ARRAY_COUNT(Magic));
		WoTPayloadTests_AppendU16(Blob, 1); // encoding version
		WoTPayloadTests_AppendU16(Blob, 1); // masked
		WoTPayloadTests_AppendU32(Blob, 256);
		WoTPayloadTests_AppendU32(Blob, 2);
		for (int32 Index = 0; Index < 256; ++Index)
		{
			if (Index == 0)
			{
				Blob.Append({255, 0, 255, 255});
			}
			else if (Index == 1)
			{
				Blob.Append({10, 20, 30, 255});
			}
			else if (Index == 2)
			{
				Blob.Append({40, 50, 60, 255});
			}
			else
			{
				const uint8 Channel = static_cast<uint8>(Index);
				Blob.Append({Channel, Channel, Channel, 255});
			}
		}
		WoTPayloadTests_AppendU32(Blob, 2);
		WoTPayloadTests_AppendU32(Blob, 2);
		WoTPayloadTests_AppendU32(Blob, 4);
		Blob.Append({0, 1, 0, 2});
		WoTPayloadTests_AppendU32(Blob, 1);
		WoTPayloadTests_AppendU32(Blob, 1);
		WoTPayloadTests_AppendU32(Blob, 1);
		Blob.Add(2);
		return Blob;
	}

	TArray<uint8> WoTPayloadTests_MenuTextureBlob(
		uint32 Width, uint32 Height, int32 MipCount, bool bMasked)
	{
		TArray<uint8> Blob;
		const uint8 Magic[8] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
		Blob.Append(Magic, UE_ARRAY_COUNT(Magic));
		WoTPayloadTests_AppendU16(Blob, 1);
		WoTPayloadTests_AppendU16(Blob, bMasked ? 1 : 0);
		WoTPayloadTests_AppendU32(Blob, 256);
		WoTPayloadTests_AppendU32(Blob, MipCount);
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const uint8 Channel = static_cast<uint8>(Index);
			Blob.Append({Channel, Channel, Channel, 255});
		}
		for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
		{
			const uint32 MipWidth = FMath::Max(1u, Width >> MipIndex);
			const uint32 MipHeight = FMath::Max(1u, Height >> MipIndex);
			const uint32 StoredBytes = MipWidth * MipHeight;
			WoTPayloadTests_AppendU32(Blob, MipWidth);
			WoTPayloadTests_AppendU32(Blob, MipHeight);
			WoTPayloadTests_AppendU32(Blob, StoredBytes);
			for (uint32 Pixel = 0; Pixel < StoredBytes; ++Pixel)
			{
				Blob.Add(bMasked && Pixel == 0
					? 0 : static_cast<uint8>((Pixel % 254) + 1));
			}
		}
		return Blob;
	}

	TArray<uint8> WoTPayloadTests_Mip0OnlyTextureBlob()
	{
		TArray<uint8> Blob;
		const uint8 Magic[8] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
		Blob.Append(Magic, UE_ARRAY_COUNT(Magic));
		WoTPayloadTests_AppendU16(Blob, 1); // encoding version
		WoTPayloadTests_AppendU16(Blob, 0); // unmasked
		WoTPayloadTests_AppendU32(Blob, 256);
		WoTPayloadTests_AppendU32(Blob, 1); // cache deliberately has only mip 0
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const uint8 Channel = static_cast<uint8>(Index);
			Blob.Append({Channel, Channel, Channel, 255});
		}
		WoTPayloadTests_AppendU32(Blob, 4);
		WoTPayloadTests_AppendU32(Blob, 4);
		WoTPayloadTests_AppendU32(Blob, 16);
		// The four 2x2 block sums are 1,1,1,5. Mip 1 quantizes to
		// [0,0;0,1], while the unquantized float chain gives mip 2 = 1.
		// A resampler which feeds quantized mip-1 bytes into mip 2 yields 0.
		Blob.Append({
			1, 0, 1, 0,
			0, 0, 0, 0,
			1, 0, 5, 0,
			0, 0, 0, 0,
		});
		return Blob;
	}

	FWoTPayloadTests_Fixture WoTPayloadTests_WriteFixture(
		const FString& PackageName, const FString& AssetName,
		EWoTPayloadTests_FixtureMode Mode)
	{
		FWoTPayloadTests_Fixture Result;
		// Full path required: the subsystem interprets RELATIVE cache dirs as
		// Saved-relative (the config convention), and ProjectIntermediateDir()
		// is engine-binary-relative in installed-engine runs.
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadTests_") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, /*Tree*/ true);

		Result.CompleteBlob = WoTPayloadTests_TextureBlob();
		if (Mode == EWoTPayloadTests_FixtureMode::LyingBlobLowerMipDimensions)
		{
			// Lower-mip header begins after the 20-byte header, 1024-byte
			// palette, and mip-0's 12-byte header + four indices.
			constexpr int32 LowerMipHeader = 1060;
			Result.CompleteBlob[LowerMipHeader + 0] = 0x00;
			Result.CompleteBlob[LowerMipHeader + 1] = 0x20; // width = 8192
			Result.CompleteBlob[LowerMipHeader + 4] = 0x00;
			Result.CompleteBlob[LowerMipHeader + 5] = 0x20; // height = 8192
		}
		Result.BlobSHA256 = WoTPayloadTests_SHA256(
			Result.CompleteBlob.GetData(), Result.CompleteBlob.Num());
		Result.BlobPath = FString::Printf(TEXT("blobs/%s/%s.blob"),
			*Result.BlobSHA256.Left(2), *Result.BlobSHA256);
		const FString EntryPackage = Mode == EWoTPayloadTests_FixtureMode::BadKeyGrammar
			? TEXT("../escape") : PackageName;
		Result.CacheKey = Mode == EWoTPayloadTests_FixtureMode::BadKeyGrammar
			? FString::Printf(TEXT("cache/not-texture/%s__%s"),
				*PackageName, *AssetName)
			: FString::Printf(TEXT("cache/texture/%s__%s"),
				*PackageName, *AssetName);
		const uint32 Width = Mode == EWoTPayloadTests_FixtureMode::OversizedDimensions
			? 8193u : 2u;
		const bool bLyingLowerMip =
			Mode == EWoTPayloadTests_FixtureMode::LyingLowerMipDimensions;
		const uint32 LowerWidth = bLyingLowerMip ? 8192u : 1u;
		const uint32 LowerHeight = bLyingLowerMip ? 8192u : 1u;
		const uint32 LowerStored = bLyingLowerMip ? 67108864u : 1u;
		const uint32 LowerExpanded = bLyingLowerMip ? 268435456u : 4u;
		const uint32 TotalExpanded = bLyingLowerMip ? 268435472u : 20u;

		const FString Entry = FString::Printf(
			TEXT("{\"blob\":\"%s\",\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",")
			TEXT("\"height\":2,\"masked\":true,\"mip_count\":2,")
			TEXT("\"mips\":[{\"expanded_byte_length\":16,\"height\":2,")
			TEXT("\"stored_byte_length\":4,\"width\":%u},")
			TEXT("{\"expanded_byte_length\":%u,\"height\":%u,")
			TEXT("\"stored_byte_length\":%u,\"width\":%u}],")
			TEXT("\"name\":\"%s\",\"package\":\"%s\",\"palette_colors\":256,")
			TEXT("\"sha256\":\"%s\",\"size\":%d,\"type\":\"textures\",\"width\":%u}"),
			*Result.BlobPath, Width, LowerExpanded, LowerHeight, LowerStored,
			LowerWidth, *AssetName, *EntryPackage,
			*Result.BlobSHA256, Result.CompleteBlob.Num(), Width);
		const FString SourcePath = FString::Printf(TEXT("Textures/%s.utx"), *PackageName);
		const FString CanonicalCore = FString::Printf(
			TEXT("{\"cache_version\":1,\"entries\":{\"%s\":%s},")
			TEXT("\"format\":\"wot-payload-cache\",\"hash_algorithm\":\"sha256\",")
			TEXT("\"limits\":{\"max_blob_size\":268435456,\"max_entries\":10000,")
			TEXT("\"max_texture_dimension\":8192,\"max_total_expanded_bytes\":2147483648},")
			TEXT("\"skipped\":[],\"sources\":[{\"path\":\"%s\",")
			TEXT("\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"size\":1,\"type\":\"textures\"}],")
			TEXT("\"summary\":{\"entries\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"logical_payload_bytes\":%d,")
			TEXT("\"source_packages\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"total_expanded_bytes\":%u,\"unique_blob_bytes\":%d,")
			TEXT("\"unique_blobs\":1},\"types\":[\"textures\"]}"),
			*Result.CacheKey, *Entry, *SourcePath, Result.CompleteBlob.Num(),
			TotalExpanded,
			Result.CompleteBlob.Num());
		const FString CacheRoot = WoTPayloadTests_CacheRoot(
			CanonicalCore, Result.BlobPath, Result.CompleteBlob.Num(), Result.BlobSHA256);
		const FString Manifest = FString::Printf(TEXT("{\"cache_sha256\":\"%s\",%s"),
			*CacheRoot, *CanonicalCore.RightChop(1));

		TArray<uint8> WrittenBlob = Result.CompleteBlob;
		if (Mode == EWoTPayloadTests_FixtureMode::TruncatedBlob)
		{
			WrittenBlob.SetNum(WrittenBlob.Num() - 1);
		}
		const bool bOldImporter = Mode == EWoTPayloadTests_FixtureMode::OldImporterVersion;
		const FString Header = FString::Printf(
			TEXT("{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},")
			TEXT("\"decoded_layout\":{\"mip_count\":2,\"mips\":[{\"expanded_rgba_byte_length\":16,\"height\":2,\"index_byte_length\":4,\"width\":%u},{\"expanded_rgba_byte_length\":%u,\"height\":%u,\"index_byte_length\":%u,\"width\":%u}],\"texel_format\":\"p8\"},")
			TEXT("\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.texture\",\"importer\":\"%s\",\"importer_version\":%u,\"kind\":\"texture\",\"masked\":true,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,\"source\":{\"export\":\"%s\",\"package\":\"Textures/%s.utx\",\"sha256\":\"%s\"}}"),
			Width, LowerExpanded, LowerHeight, LowerStored, LowerWidth,
			bOldImporter ? TEXT("texture-v1") : TEXT("texture-v2"),
			bOldImporter ? 1u : 2u,
			WrittenBlob.Num(), *AssetName, *PackageName,
			TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
		const TArray<uint8> Object = WoTPayloadActivationTest_FrameObject(
			Header, WrittenBlob);
		Result.bPublished = WoTPayloadActivationTest_PublishObject(
			Result.CacheDirectory, Result.CacheKey, TEXT("wot-texture-v3"),
			Object, Result.PublishError);
		// BeginBuild discards the cache root. Add the developer-oracle files
		// after v3 publication so initialization can still preflight them.
		FFileHelper::SaveStringToFile(Manifest,
			*FPaths::Combine(Result.CacheDirectory, TEXT("manifest.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString BlobFilename = FPaths::Combine(Result.CacheDirectory, Result.BlobPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(BlobFilename), /*Tree*/ true);
		FFileHelper::SaveArrayToFile(WrittenBlob, *BlobFilename);
		return Result;
	}

	FWoTPayloadTests_Fixture WoTPayloadTests_WriteMip0OnlyFixture(
		const FString& PackageName, const FString& AssetName)
	{
		FWoTPayloadTests_Fixture Result;
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadTests_") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, /*Tree*/ true);

		Result.CompleteBlob = WoTPayloadTests_Mip0OnlyTextureBlob();
		Result.BlobSHA256 = WoTPayloadTests_SHA256(
			Result.CompleteBlob.GetData(), Result.CompleteBlob.Num());
		Result.BlobPath = FString::Printf(TEXT("blobs/%s/%s.blob"),
			*Result.BlobSHA256.Left(2), *Result.BlobSHA256);
		Result.CacheKey = FString::Printf(TEXT("cache/texture/%s__%s"),
			*PackageName, *AssetName);

		const FString Entry = FString::Printf(
			TEXT("{\"blob\":\"%s\",\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",")
			TEXT("\"height\":4,\"masked\":false,\"mip_count\":1,")
			TEXT("\"mips\":[{\"expanded_byte_length\":64,\"height\":4,")
			TEXT("\"stored_byte_length\":16,\"width\":4}],")
			TEXT("\"name\":\"%s\",\"package\":\"%s\",\"palette_colors\":256,")
			TEXT("\"sha256\":\"%s\",\"size\":%d,\"type\":\"textures\",\"width\":4}"),
			*Result.BlobPath, *AssetName, *PackageName,
			*Result.BlobSHA256, Result.CompleteBlob.Num());
		const FString SourcePath = FString::Printf(
			TEXT("Textures/%s.utx"), *PackageName);
		const FString CanonicalCore = FString::Printf(
			TEXT("{\"cache_version\":1,\"entries\":{\"%s\":%s},")
			TEXT("\"format\":\"wot-payload-cache\",\"hash_algorithm\":\"sha256\",")
			TEXT("\"limits\":{\"max_blob_size\":268435456,\"max_entries\":10000,")
			TEXT("\"max_texture_dimension\":8192,\"max_total_expanded_bytes\":2147483648},")
			TEXT("\"skipped\":[],\"sources\":[{\"path\":\"%s\",")
			TEXT("\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"size\":1,\"type\":\"textures\"}],")
			TEXT("\"summary\":{\"entries\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"logical_payload_bytes\":%d,")
			TEXT("\"source_packages\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"total_expanded_bytes\":64,\"unique_blob_bytes\":%d,")
			TEXT("\"unique_blobs\":1},\"types\":[\"textures\"]}"),
			*Result.CacheKey, *Entry, *SourcePath, Result.CompleteBlob.Num(),
			Result.CompleteBlob.Num());
		const FString CacheRoot = WoTPayloadTests_CacheRoot(
			CanonicalCore, Result.BlobPath, Result.CompleteBlob.Num(), Result.BlobSHA256);
		const FString Manifest = FString::Printf(TEXT("{\"cache_sha256\":\"%s\",%s"),
			*CacheRoot, *CanonicalCore.RightChop(1));

		const FString Header = FString::Printf(
			TEXT("{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},")
			TEXT("\"decoded_layout\":{\"mip_count\":1,\"mips\":[{\"expanded_rgba_byte_length\":64,\"height\":4,\"index_byte_length\":16,\"width\":4}],\"texel_format\":\"p8\"},")
			TEXT("\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.texture.mip\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":false,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,\"source\":{\"export\":\"%s\",\"package\":\"Textures/%s.utx\",\"sha256\":\"%s\"}}"),
			Result.CompleteBlob.Num(), *AssetName, *PackageName,
			TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
		const TArray<uint8> Object = WoTPayloadActivationTest_FrameObject(
			Header, Result.CompleteBlob);
		Result.bPublished = WoTPayloadActivationTest_PublishObject(
			Result.CacheDirectory, Result.CacheKey, TEXT("wot-texture-v3"),
			Object, Result.PublishError);
		// BeginBuild discards the cache root. Add the developer-oracle files
		// after v3 publication so initialization can still preflight them.
		FFileHelper::SaveStringToFile(Manifest,
			*FPaths::Combine(Result.CacheDirectory, TEXT("manifest.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString BlobFilename = FPaths::Combine(
			Result.CacheDirectory, Result.BlobPath);
		IFileManager::Get().MakeDirectory(
			*FPaths::GetPath(BlobFilename), /*Tree*/ true);
		FFileHelper::SaveArrayToFile(Result.CompleteBlob, *BlobFilename);
		return Result;
	}

	FWoTPayloadTests_MenuFixture WoTPayloadTests_WriteMenuFixture(
		int32 OmittedIndex = INDEX_NONE, int32 MismatchedSourceIndex = INDEX_NONE)
	{
		FWoTPayloadTests_MenuFixture Result;
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadMenuTests_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, /*Tree*/ true);

		static const TCHAR* AssetNames[] = {
			TEXT("WOTConsole"), TEXT("WOTBorder"), TEXT("Slide1"),
			TEXT("Slide2"), TEXT("Slide3"), TEXT("Slide4")};
		static const uint32 Widths[] = {64, 2, 8, 8, 8, 8};
		static const uint32 Heights[] = {64, 2, 8, 8, 8, 8};
		static const int32 MipCounts[] = {7, 2, 1, 1, 1, 1};
		static const bool Masked[] = {false, false, true, true, true, true};
		TArray<FWoTPayloadActivationTestObject> Objects;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AssetNames); ++Index)
		{
			if (Index == OmittedIndex) continue;
			const FString AssetName(AssetNames[Index]);
			const FString SourceExport = Index == MismatchedSourceIndex
				? TEXT("WOT.Wrong") : TEXT("WOT.") + AssetName;
			const TArray<uint8> Payload = WoTPayloadTests_MenuTextureBlob(
				Widths[Index], Heights[Index], MipCounts[Index], Masked[Index]);
			TArray<FString> MipLayouts;
			for (int32 MipIndex = 0; MipIndex < MipCounts[Index]; ++MipIndex)
			{
				const uint32 MipWidth = FMath::Max(1u, Widths[Index] >> MipIndex);
				const uint32 MipHeight = FMath::Max(1u, Heights[Index] >> MipIndex);
				MipLayouts.Add(FString::Printf(
					TEXT("{\"expanded_rgba_byte_length\":%u,\"height\":%u,\"index_byte_length\":%u,\"width\":%u}"),
					MipWidth * MipHeight * 4, MipHeight,
					MipWidth * MipHeight, MipWidth));
			}
			const FString Header = FString::Printf(
				TEXT("{\"anim_next\":\"\",\"color\":{\"alpha\":\"index-zero-if-masked\",\"channel_order\":\"rgba\",\"palette_colors\":256,\"stored_palette_alpha\":\"preserved-but-non-semantic\"},")
				TEXT("\"decoded_layout\":{\"mip_count\":%d,\"mips\":[%s],\"texel_format\":\"p8\"},")
				TEXT("\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",\"fact_id\":\"spec.menu.%s\",\"importer\":\"texture-v2\",\"importer_version\":2,\"kind\":\"texture\",\"masked\":%s,\"max_frame_rate_bits\":0,\"min_frame_rate_bits\":0,\"payload_byte_length\":%d,\"prime_count\":0,\"source\":{\"export\":\"%s\",\"package\":\"System/WOT.u\",\"sha256\":\"%s\"}}"),
				MipCounts[Index], *FString::Join(MipLayouts, TEXT(",")),
				*AssetName.ToLower(), Masked[Index] ? TEXT("true") : TEXT("false"),
				Payload.Num(), *SourceExport,
				TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"));
			FWoTPayloadActivationTestObject& Object = Objects.AddDefaulted_GetRef();
			Object.LogicalId = TEXT("cache/texture/WOT__") + AssetName;
			Object.Schema = TEXT("wot-texture-v3");
			Object.Bytes = WoTPayloadActivationTest_FrameObject(Header, Payload);
		}
		Result.bPublished = WoTPayloadActivationTest_PublishObjects(
			Result.CacheDirectory, Objects, Result.PublishError);
		return Result;
	}

	FWoTPayloadTests_Fixture WoTPayloadTests_WriteFractalSeedFixture(
		const FString& PackageName, const FString& AssetName)
	{
		// 8x8 single-mip unmasked texture: the smallest seed every fractal sim
		// accepts (IsPowerOfTwoAtLeast8). Same manifest/blob grammar as the
		// fixtures above.
		FWoTPayloadTests_Fixture Result;
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadTests_") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, /*Tree*/ true);

		TArray<uint8>& Blob = Result.CompleteBlob;
		const uint8 Magic[8] = {'W', 'O', 'T', 'P', 'T', 'E', 'X', 0};
		Blob.Append(Magic, UE_ARRAY_COUNT(Magic));
		WoTPayloadTests_AppendU16(Blob, 1); // encoding version
		WoTPayloadTests_AppendU16(Blob, 0); // unmasked
		WoTPayloadTests_AppendU32(Blob, 256);
		WoTPayloadTests_AppendU32(Blob, 1); // single mip
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const uint8 Channel = static_cast<uint8>(Index);
			Blob.Append({Channel, Channel, Channel, 255});
		}
		WoTPayloadTests_AppendU32(Blob, 8);
		WoTPayloadTests_AppendU32(Blob, 8);
		WoTPayloadTests_AppendU32(Blob, 64);
		for (int32 Pixel = 0; Pixel < 64; ++Pixel)
		{
			Blob.Add(static_cast<uint8>(Pixel % 7));
		}

		Result.BlobSHA256 = WoTPayloadTests_SHA256(Blob.GetData(), Blob.Num());
		Result.BlobPath = FString::Printf(TEXT("blobs/%s/%s.blob"),
			*Result.BlobSHA256.Left(2), *Result.BlobSHA256);
		Result.CacheKey = FString::Printf(TEXT("texture:%s__%s"),
			*PackageName, *AssetName);

		const FString Entry = FString::Printf(
			TEXT("{\"blob\":\"%s\",\"encoding\":\"ue1-p8-mips-rgba-palette-v1\",")
			TEXT("\"height\":8,\"masked\":false,\"mip_count\":1,")
			TEXT("\"mips\":[{\"expanded_byte_length\":256,\"height\":8,")
			TEXT("\"stored_byte_length\":64,\"width\":8}],")
			TEXT("\"name\":\"%s\",\"package\":\"%s\",\"palette_colors\":256,")
			TEXT("\"sha256\":\"%s\",\"size\":%d,\"type\":\"textures\",\"width\":8}"),
			*Result.BlobPath, *AssetName, *PackageName,
			*Result.BlobSHA256, Blob.Num());
		const FString SourcePath = FString::Printf(
			TEXT("Textures/%s.utx"), *PackageName);
		const FString CanonicalCore = FString::Printf(
			TEXT("{\"cache_version\":1,\"entries\":{\"%s\":%s},")
			TEXT("\"format\":\"wot-payload-cache\",\"hash_algorithm\":\"sha256\",")
			TEXT("\"limits\":{\"max_blob_size\":268435456,\"max_entries\":10000,")
			TEXT("\"max_texture_dimension\":8192,\"max_total_expanded_bytes\":2147483648},")
			TEXT("\"skipped\":[],\"sources\":[{\"path\":\"%s\",")
			TEXT("\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"size\":1,\"type\":\"textures\"}],")
			TEXT("\"summary\":{\"entries\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"logical_payload_bytes\":%d,")
			TEXT("\"source_packages\":{\"meshes\":0,\"sounds\":0,\"textures\":1},")
			TEXT("\"total_expanded_bytes\":256,\"unique_blob_bytes\":%d,")
			TEXT("\"unique_blobs\":1},\"types\":[\"textures\"]}"),
			*Result.CacheKey, *Entry, *SourcePath, Blob.Num(), Blob.Num());
		const FString CacheRoot = WoTPayloadTests_CacheRoot(
			CanonicalCore, Result.BlobPath, Blob.Num(), Result.BlobSHA256);
		const FString Manifest = FString::Printf(TEXT("{\"cache_sha256\":\"%s\",%s"),
			*CacheRoot, *CanonicalCore.RightChop(1));
		FFileHelper::SaveStringToFile(Manifest,
			*FPaths::Combine(Result.CacheDirectory, TEXT("manifest.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		const FString BlobFilename = FPaths::Combine(
			Result.CacheDirectory, Result.BlobPath);
		IFileManager::Get().MakeDirectory(
			*FPaths::GetPath(BlobFilename), /*Tree*/ true);
		FFileHelper::SaveArrayToFile(Blob, *BlobFilename);
		return Result;
	}

	FWoTPayloadTests_World WoTPayloadTests_CreateWorld(
		const FString& CacheDirectory, bool bInjectionEnabled)
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ true, bInjectionEnabled);
		FWoTPayloadTests_World Result;
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bActivatePayload = false;   // this fixture drives the REAL publication path
		Options.BeforeInitializeStandalone = [&CacheDirectory](UGameInstance* GameInstance)
		{
			CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(
				CacheDirectory, /*bSave*/ false);
		};
		Result.World = WoTTest::CreateSpecWorld(Options);
		Result.GameInstance = Result.World
			? Cast<UWoTGameInstance>(Result.World->GetGameInstance()) : nullptr;
		if (bInjectionEnabled && Result.GameInstance)
		{
			UWoTPayloadSubsystem* Payloads =
				Result.GameInstance->GetSubsystem<UWoTPayloadSubsystem>();
			Result.bActivated = WoTPayloadActivationTest_ActivatePublished(
				Payloads, CacheDirectory, Result.ActivationError);
		}
		return Result;
	}

	void WoTPayloadTests_DestroyWorld(FWoTPayloadTests_World& State)
	{
		WoTTest::DestroySpecWorld(State.World);
		State.World = nullptr;
		State.GameInstance = nullptr;
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ false);
	}

	void WoTPayloadTests_DeleteFixture(const FWoTPayloadTests_Fixture& Fixture)
	{
		IFileManager::Get().DeleteDirectory(
			*Fixture.CacheDirectory, /*RequireExists*/ false, /*Tree*/ true);
	}

	UTexture2D* WoTPayloadTests_CreateDestinationWithMipsInPackage(
		const FString& LongPackageName, const FString& AssetName,
		const FString& CacheKey, const FColor& Placeholder, bool bNeverStream,
		int32 Width, int32 Height, int32 MipCount, bool bNoMipmaps)
	{
		UPackage* Package = CreatePackage(*LongPackageName);
		UTexture2D* Texture = NewObject<UTexture2D>(
			Package, *AssetName, RF_Public | RF_Standalone);
		Texture->CompressionSettings = TC_EditorIcon;
		Texture->SRGB = false;
		TArray<uint8> SourcePixels;
		SourcePixels.SetNumUninitialized(Width * Height * 4);
		for (int32 PixelOffset = 0; PixelOffset < SourcePixels.Num(); PixelOffset += 4)
		{
			SourcePixels[PixelOffset + 0] = Placeholder.B;
			SourcePixels[PixelOffset + 1] = Placeholder.G;
			SourcePixels[PixelOffset + 2] = Placeholder.R;
			SourcePixels[PixelOffset + 3] = Placeholder.A;
		}
	#if WITH_EDITORONLY_DATA
		Texture->MipGenSettings = bNoMipmaps
			? TMGS_NoMipmaps : TMGS_FromTextureGroup;
		Texture->Source.Init(Width, Height, 1, 1,
			ETextureSourceFormat::TSF_BGRA8, SourcePixels.GetData());
#endif
		Texture->NeverStream = bNeverStream;
		FTexturePlatformData* PlatformData = new FTexturePlatformData();
		PlatformData->SizeX = Width;
		PlatformData->SizeY = Height;
		PlatformData->SetNumSlices(1);
		PlatformData->PixelFormat = PF_B8G8R8A8;
		for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
		{
			const int32 MipWidth = FMath::Max(1, Width >> MipIndex);
			const int32 MipHeight = FMath::Max(1, Height >> MipIndex);
			const int32 MipBytes = MipWidth * MipHeight * 4;
			FTexture2DMipMap* Mip = new FTexture2DMipMap(
				MipWidth, MipHeight, 1);
			PlatformData->Mips.Add(Mip);
			Mip->BulkData.Lock(LOCK_READ_WRITE);
			uint8* Pixels = static_cast<uint8*>(Mip->BulkData.Realloc(MipBytes));
			for (int32 PixelOffset = 0; PixelOffset < MipBytes; PixelOffset += 4)
			{
				Pixels[PixelOffset + 0] = Placeholder.B;
				Pixels[PixelOffset + 1] = Placeholder.G;
				Pixels[PixelOffset + 2] = Placeholder.R;
				Pixels[PixelOffset + 3] = Placeholder.A;
			}
			Mip->BulkData.Unlock();
		}
		Texture->SetPlatformData(PlatformData);

		UWoTPayloadAssetUserData* Binding =
			NewObject<UWoTPayloadAssetUserData>(Texture);
		Binding->PayloadKey = CacheKey;
		Binding->DestinationMipCount = MipCount;
		Texture->AddAssetUserData(Binding);
		Texture->UpdateResource();
		return Texture;
	}

	UTexture2D* WoTPayloadTests_CreateDestinationInPackage(
		const FString& LongPackageName, const FString& AssetName,
		const FString& CacheKey, const FColor& Placeholder, bool bNeverStream)
	{
		return WoTPayloadTests_CreateDestinationWithMipsInPackage(
			LongPackageName, AssetName, CacheKey, Placeholder, bNeverStream,
			/*Width*/ 2, /*Height*/ 2, /*MipCount*/ 1, /*bNoMipmaps*/ true);
	}

	UTexture2D* WoTPayloadTests_CreateDestination(const FString& PackageName,
		const FString& AssetName, const FString& CacheKey, uint8 Placeholder)
	{
		const FString LongPackageName = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s"), *PackageName, *AssetName);
		return WoTPayloadTests_CreateDestinationInPackage(
			LongPackageName, AssetName, CacheKey,
			FColor(Placeholder, Placeholder, Placeholder, Placeholder),
			/*bNeverStream*/ false);
	}

	UTexture2D* WoTPayloadTests_CreateStrippedDestination(
		const FString& PackageName, const FString& AssetName,
		const FString& CacheKey)
	{
		const FString LongPackageName = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s"), *PackageName, *AssetName);
		// Exact Tools/import/stripped_texture_import.py placeholder: opaque
		// magenta, whose RGBA and BGRA byte sequences are both ff 00 ff ff.
		return WoTPayloadTests_CreateDestinationInPackage(
			LongPackageName, AssetName, CacheKey, FColor(255, 0, 255, 255),
			/*bNeverStream*/ true);
	}

	UTexture2D* WoTPayloadTests_CreateMipmappedDestination(
		const FString& PackageName, const FString& AssetName,
		const FString& CacheKey)
	{
		const FString LongPackageName = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s"), *PackageName, *AssetName);
		return WoTPayloadTests_CreateDestinationWithMipsInPackage(
			LongPackageName, AssetName, CacheKey,
			FColor(255, 0, 255, 255), /*bNeverStream*/ false,
			/*Width*/ 4, /*Height*/ 4, /*MipCount*/ 3, /*bNoMipmaps*/ false);
	}

	TArray<uint8> WoTPayloadTests_TextureMipBytes(
		UTexture2D* Texture, int32 MipIndex)
	{
		TArray<uint8> Result;
		const FTexturePlatformData* PlatformData = Texture
			? Texture->GetPlatformData() : nullptr;
		if (!PlatformData || !PlatformData->Mips.IsValidIndex(MipIndex))
		{
			return Result;
		}
		FTexture2DMipMap& Mip = const_cast<FTexture2DMipMap&>(
			PlatformData->Mips[MipIndex]);
		const int64 Size = Mip.BulkData.GetBulkDataSize();
		const uint8* Bytes = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
		if (Bytes && Size >= 0 && Size <= MAX_int32)
		{
			Result.Append(Bytes, static_cast<int32>(Size));
		}
		Mip.BulkData.Unlock();
		return Result;
	}

	FString WoTPayloadTests_TextureMipHash(UTexture2D* Texture)
	{
		const FTexturePlatformData* PlatformData = Texture
			? Texture->GetPlatformData() : nullptr;
		if (!PlatformData || PlatformData->Mips.Num() != 1)
		{
			return FString();
		}
		FTexture2DMipMap& Mip = const_cast<FTexture2DMipMap&>(PlatformData->Mips[0]);
		const int64 Size = Mip.BulkData.GetBulkDataSize();
		const void* Bytes = Mip.BulkData.LockReadOnly();
		const FString Hash = Bytes && Size >= 0 && Size <= MAX_uint32
			? WoTPayloadTests_SHA256(Bytes, static_cast<uint32>(Size)) : FString();
		Mip.BulkData.Unlock();
		return Hash;
	}

	bool WoTPayloadTests_TextureMipIsOpaqueMagenta(UTexture2D* Texture)
	{
		const FTexturePlatformData* PlatformData = Texture
			? Texture->GetPlatformData() : nullptr;
		if (!PlatformData || PlatformData->SizeX != 2 || PlatformData->SizeY != 2
			|| PlatformData->Mips.Num() != 1)
		{
			return false;
		}
		FTexture2DMipMap& Mip = const_cast<FTexture2DMipMap&>(PlatformData->Mips[0]);
		const int64 Size = Mip.BulkData.GetBulkDataSize();
		const uint8* Bytes = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
		bool bMatches = Bytes != nullptr && Size == 16;
		for (int64 Offset = 0; bMatches && Offset < Size; Offset += 4)
		{
			bMatches = Bytes[Offset + 0] == 255 && Bytes[Offset + 1] == 0
				&& Bytes[Offset + 2] == 255 && Bytes[Offset + 3] == 255;
		}
		Mip.BulkData.Unlock();
		return bMatches;
	}

	void WoTPayloadTests_DestroyDestination(UTexture2D*& Texture)
	{
		if (!Texture) return;
		UPackage* Package = Texture->GetOutermost();
		Texture->ReleaseResource();
		FlushRenderingCommands();
		Texture->ClearFlags(RF_Public | RF_Standalone);
		Texture->MarkAsGarbage();
		if (Package)
		{
			ResetLoaders(Package);
			Package->MarkAsGarbage();
		}
		Texture = nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSHA256KnownAnswerSpec,
	"WoT.Payload.SHA256KnownAnswers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSHA256KnownAnswerSpec::RunTest(const FString&)
{
	const ANSICHAR* Empty = "";
	const ANSICHAR* ABC = "abc";
	const ANSICHAR* MultiBlock =
		"12345678901234567890123456789012345678901234567890123456789012345678901234567890";
	TestEqual(TEXT("SHA-256 empty-string known answer"),
		WoTSha256::HashBuffer(Empty, FCStringAnsi::Strlen(Empty)),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("SHA-256 abc known answer"),
		WoTSha256::HashBuffer(ABC, FCStringAnsi::Strlen(ABC)),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("SHA-256 80-byte multi-block known answer"),
		WoTSha256::HashBuffer(MultiBlock, FCStringAnsi::Strlen(MultiBlock)),
		FString(TEXT("f371bc4a311f2b009eef952dd83ca80e2b60026c8e935592d0f9c308453c813e")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadManifestSpec,
	"WoT.Payload.Texture.CacheV3ObjectValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadManifestSpec::RunTest(const FString&)
{
	const FString PackageName = TEXT("WoTPayloadManifestFixture");
	const FString AssetName = TEXT("Stone");
	for (const EWoTPayloadTests_FixtureMode Mode : {
		EWoTPayloadTests_FixtureMode::Valid,
		EWoTPayloadTests_FixtureMode::OversizedDimensions,
		EWoTPayloadTests_FixtureMode::LyingLowerMipDimensions,
		EWoTPayloadTests_FixtureMode::LyingBlobLowerMipDimensions,
		EWoTPayloadTests_FixtureMode::BadKeyGrammar,
		EWoTPayloadTests_FixtureMode::TruncatedBlob,
		EWoTPayloadTests_FixtureMode::OldImporterVersion})
	{
		FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteFixture(
			PackageName, AssetName, Mode);
		TestTrue(TEXT("cache-v3 fixture publishes through the generation store"),
			Fixture.bPublished);
		if (!Fixture.bPublished)
		{
			AddError(Fixture.PublishError);
		}
		FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
			Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
		TestTrue(TEXT("verified snapshot activates the fixture generation"),
			State.bActivated);
		if (!State.bActivated)
		{
			AddError(State.ActivationError);
		}
		UWoTPayloadSubsystem* Payloads = State.GameInstance
			? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		if (TestNotNull(TEXT("payload subsystem exists"), Payloads))
		{
			TestTrue(TEXT("typed-object rejection leaves the activated generation ready"),
				Payloads->IsManifestReady());
			TestFalse(TEXT("the subsystem is no longer dormant"),
				Payloads->IsDormant());
			if (Mode == EWoTPayloadTests_FixtureMode::Valid)
			{
				TestTrue(TEXT("valid cache-v3 texture object decodes"),
					Payloads->LoadTexturePayload(Fixture.CacheKey).IsValid());
			}
			else
			{
				TestFalse(TEXT("hostile cache-v3 header or payload is rejected before publication side effects"),
					Payloads->LoadTexturePayload(Fixture.CacheKey).IsValid());
			}
		}
		WoTPayloadTests_DestroyWorld(State);
		WoTPayloadTests_DeleteFixture(Fixture);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadTextureInjectionSpec,
	"WoT.Payload.TextureInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadTextureInjectionSpec::RunTest(const FString&)
{
	const FString PackageName = TEXT("WoTPayloadTextureFixture");
	const FString AssetName = TEXT("Stone");
	FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteFixture(
		PackageName, AssetName, EWoTPayloadTests_FixtureMode::Valid);
	TestTrue(TEXT("texture cache-v3 fixture publishes"), Fixture.bPublished);
	UTexture2D* Texture = WoTPayloadTests_CreateDestination(
		PackageName, AssetName, Fixture.CacheKey, 0x7f);
	FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("texture generation activates from a verified snapshot"),
		State.bActivated);
	UTexture2D* LateTexture = nullptr;
	ON_SCOPE_EXIT
	{
		WoTPayloadTests_DestroyWorld(State);
		WoTPayloadTests_DestroyDestination(LateTexture);
		WoTPayloadTests_DestroyDestination(Texture);
		WoTPayloadTests_DeleteFixture(Fixture);
	};

	const FString WoTPayloadTests_ExpectedTextureHash =
		TEXT("ee8a624d11a4db65ef76d0f2fcd7ef1f79b4552e4ffaa8cc001df0d5cb463037");
	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	TestNotNull(TEXT("payload subsystem exists"), Payloads);
	TestEqual(TEXT("already-loaded object walk injects the destination"),
		WoTPayloadTests_TextureMipHash(Texture),
		WoTPayloadTests_ExpectedTextureHash);
	TestTrue(TEXT("injected texture disables stock streaming"), Texture->NeverStream != 0);
	TestTrue(TEXT("raw replacement mips are marked as not offline processed"),
		Texture->bNotOfflineProcessed != 0);
	TestEqual(TEXT("destination compression setting is preserved"),
		static_cast<int32>(Texture->CompressionSettings),
		static_cast<int32>(TC_EditorIcon));
#if WITH_EDITORONLY_DATA
	TestEqual(TEXT("destination no-mip policy is preserved"),
		static_cast<int32>(Texture->MipGenSettings),
		static_cast<int32>(TMGS_NoMipmaps));
#endif
	const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	if (TestNotNull(TEXT("injected texture has platform data"), PlatformData))
	{
		TestEqual(TEXT("DestinationMipCount selects only source mip 0"),
			PlatformData->Mips.Num(), 1);
		TestEqual(TEXT("runtime format is BGRA8"),
			static_cast<int32>(PlatformData->PixelFormat),
			static_cast<int32>(PF_B8G8R8A8));
	}
	TestEqual(TEXT("expanded/dilated mip pixel SHA-256"),
		WoTPayloadTests_TextureMipHash(Texture),
		WoTPayloadTests_ExpectedTextureHash);

	// Create the bound texture only after the subsystem's initialization walk.
	// Its valid in-memory platform data isolates the real completion-handler seam
	// from the editor texture-build pipeline, which does not finish under -nullrhi.
	const FString WoTPayloadTests_LateLongPackageName =
		TEXT("/Game/__WoTPayloadTest_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ TEXT("/LateStone");
	LateTexture = WoTPayloadTests_CreateDestinationInPackage(
		WoTPayloadTests_LateLongPackageName, TEXT("LateStone"),
		Fixture.CacheKey, FColor(0x33, 0x33, 0x33, 0x33),
		/*bNeverStream*/ false);
	if (TestNotNull(TEXT("late in-memory destination exists"), LateTexture))
	{
		const FTexturePlatformData* WoTPayloadTests_LatePlatformData =
			LateTexture->GetPlatformData();
		if (TestNotNull(TEXT("late placeholder has valid platform data"),
			WoTPayloadTests_LatePlatformData))
		{
			TestEqual(TEXT("late placeholder width is valid"),
				WoTPayloadTests_LatePlatformData->SizeX, 2);
			TestEqual(TEXT("late placeholder height is valid"),
				WoTPayloadTests_LatePlatformData->SizeY, 2);
			TestEqual(TEXT("late placeholder has one mip"),
				WoTPayloadTests_LatePlatformData->Mips.Num(), 1);
		}
		const UWoTPayloadAssetUserData* WoTPayloadTests_LateBinding = Cast<
			UWoTPayloadAssetUserData>(LateTexture->GetAssetUserDataOfClass(
				UWoTPayloadAssetUserData::StaticClass()));
		if (TestNotNull(TEXT("late placeholder has payload binding"),
			WoTPayloadTests_LateBinding))
		{
			TestEqual(TEXT("late binding requests mip 0 only"),
				WoTPayloadTests_LateBinding->DestinationMipCount, 1);
		}
		TestNotEqual(TEXT("late placeholder is untouched before completion"),
			WoTPayloadTests_TextureMipHash(LateTexture),
			WoTPayloadTests_ExpectedTextureHash);
		TestFalse(TEXT("late placeholder begins streamable"),
			LateTexture->NeverStream != 0);

		UPackage* WoTPayloadTests_LatePackage = LateTexture->GetOutermost();
		if (Payloads && TestNotNull(TEXT("late in-memory package exists"),
			WoTPayloadTests_LatePackage))
		{
			const FWoTPayloadTests_PackageLoadCompletedTag::Type
				WoTPayloadTests_PackageLoadCompleted =
				WoTPayloadTests_GetPrivateMember(
					FWoTPayloadTests_PackageLoadCompletedTag());
			(Payloads->*WoTPayloadTests_PackageLoadCompleted)(
				WoTPayloadTests_LatePackage);
		}

		TestEqual(TEXT("package completion handler injects a later destination"),
			WoTPayloadTests_TextureMipHash(LateTexture),
			WoTPayloadTests_ExpectedTextureHash);
		TestTrue(TEXT("late injected texture disables stock streaming"),
			LateTexture->NeverStream != 0);
		TestTrue(TEXT("late raw replacement is marked as not offline processed"),
			LateTexture->bNotOfflineProcessed != 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadFrontEndMenuConsumersSpec,
	"WoT.Payload.FrontEndMenuTransientConsumers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadFrontEndMenuConsumersSpec::RunTest(const FString&)
{
	// Eight hostile fixture worlds, the forced rollback world, and two exact-six
	// lifecycle worlds initialize before their authenticated generation lease
	// is adopted. The developer
	// manifest probe therefore emits this expected pre-activation error once
	// per world; runtime logging remains unsuppressed.
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 11);
	static const FWoTTextureConsumerBinding Expected[] = {
		{TEXT("/Game/WoT/Textures/WOT/WOTConsole.WOTConsole"),
			TEXT("cache/texture/WOT__WOTConsole"), TEXT("System/WOT.u"),
			TEXT("WOT.WOTConsole"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			64, 64, 7, false},
		{TEXT("/Game/WoT/Textures/WOT/WOTBorder.WOTBorder"),
			TEXT("cache/texture/WOT__WOTBorder"), TEXT("System/WOT.u"),
			TEXT("WOT.WOTBorder"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			2, 2, 2, false},
		{TEXT("/Game/WoT/Textures/WOT/Slide1.Slide1"),
			TEXT("cache/texture/WOT__Slide1"), TEXT("System/WOT.u"),
			TEXT("WOT.Slide1"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			8, 8, 1, true},
		{TEXT("/Game/WoT/Textures/WOT/Slide2.Slide2"),
			TEXT("cache/texture/WOT__Slide2"), TEXT("System/WOT.u"),
			TEXT("WOT.Slide2"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			8, 8, 1, true},
		{TEXT("/Game/WoT/Textures/WOT/Slide3.Slide3"),
			TEXT("cache/texture/WOT__Slide3"), TEXT("System/WOT.u"),
			TEXT("WOT.Slide3"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			8, 8, 1, true},
		{TEXT("/Game/WoT/Textures/WOT/Slide4.Slide4"),
			TEXT("cache/texture/WOT__Slide4"), TEXT("System/WOT.u"),
			TEXT("WOT.Slide4"),
			TEXT("8e670c5b58110a31367029a9801ad38c89a587aa82d1786dec4451815b7746fb"),
			8, 8, 1, true},
	};
	auto NoConsumerWasPublished = [this](const TCHAR* Label)
	{
		for (const FWoTTextureConsumerBinding& Requirement : Expected)
		{
			TestNull(FString::Printf(TEXT("%s leaves %s unpublished"), Label,
				*Requirement.AssetPath), FindObject<UTexture2D>(
					nullptr, *Requirement.AssetPath,
					EFindObjectFlags::ExactClass));
		}
	};
	auto RunHostile = [this, &NoConsumerWasPublished](
		const TCHAR* Label, int32 OmittedIndex, int32 MismatchedSourceIndex,
		TFunction<void(TArray<FWoTTextureConsumerBinding>&)> Mutate)
	{
		const FWoTPayloadTests_MenuFixture Fixture =
			WoTPayloadTests_WriteMenuFixture(OmittedIndex, MismatchedSourceIndex);
		TestTrue(FString::Printf(TEXT("%s fixture publishes"), Label),
			Fixture.bPublished);
		FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
			Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
		TestTrue(FString::Printf(TEXT("%s fixture activates"), Label),
			State.bActivated);
		UWoTPayloadSubsystem* Payloads = State.GameInstance
			? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		if (TestNotNull(FString::Printf(TEXT("%s payload subsystem"), Label),
			Payloads))
		{
			TArray<FWoTTextureConsumerBinding> Requirements;
			Requirements.Append(Expected, UE_ARRAY_COUNT(Expected));
			if (Mutate) Mutate(Requirements);
			TestTrue(FString::Printf(TEXT("%s fails closed"), Label),
				Payloads->PrepareTextureConsumers(Requirements).IsError());
			NoConsumerWasPublished(Label);
		}
		WoTPayloadTests_DestroyWorld(State);
		IFileManager::Get().DeleteDirectory(
			*Fixture.CacheDirectory, /*RequireExists*/ false, /*Tree*/ true);
	};

	// Initialize the process-global style before any consumer exists, matching
	// the packaged null/stale singleton ordering. The test-only clear removes
	// editor content that is intentionally absent from the guarded package.
	FWoTMenuStyle& MenuStyle = FWoTMenuStyle::Get();
	MenuStyle.ClearTextureConsumersForTesting();
	CollectGarbage(RF_NoFlags);
	NoConsumerWasPublished(TEXT("fresh invocation"));

	RunHostile(TEXT("missing logical object"), 5, INDEX_NONE, {});
	RunHostile(TEXT("mismatched authenticated source"), INDEX_NONE, 4, {});
	RunHostile(TEXT("unknown logical id"), INDEX_NONE, INDEX_NONE,
		[](TArray<FWoTTextureConsumerBinding>& Requirements)
		{
			Requirements[3].LogicalId = TEXT("cache/texture/WOT__Unknown");
		});
	RunHostile(TEXT("duplicate consumer identity"), INDEX_NONE, INDEX_NONE,
		[](TArray<FWoTTextureConsumerBinding>& Requirements)
		{
			Requirements[5].AssetPath = Requirements[4].AssetPath;
		});
	RunHostile(TEXT("duplicate logical identity"), INDEX_NONE, INDEX_NONE,
		[](TArray<FWoTTextureConsumerBinding>& Requirements)
		{
			Requirements[5].LogicalId = Requirements[4].LogicalId;
		});
	RunHostile(TEXT("wrong authenticated source hash"), INDEX_NONE, INDEX_NONE,
		[](TArray<FWoTTextureConsumerBinding>& Requirements)
		{
			Requirements[0].SourceSha256 =
				TEXT("0000000000000000000000000000000000000000000000000000000000000000");
		});
	RunHostile(TEXT("wrong authenticated header shape"), INDEX_NONE, INDEX_NONE,
		[](TArray<FWoTTextureConsumerBinding>& Requirements)
		{
			Requirements[2].Width = 16;
		});

	const FWoTPayloadTests_MenuFixture ForeignFixture =
		WoTPayloadTests_WriteMenuFixture();
	TestTrue(TEXT("foreign occupant fixture publishes"),
		ForeignFixture.bPublished);
	FWoTPayloadTests_World ForeignState = WoTPayloadTests_CreateWorld(
		ForeignFixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("foreign occupant fixture activates"), ForeignState.bActivated);
	UWoTPayloadSubsystem* ForeignPayloads = ForeignState.GameInstance
		? ForeignState.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	UTexture2D* ForeignTexture = WoTPayloadTests_CreateDestinationWithMipsInPackage(
		TEXT("/Game/WoT/Textures/WOT/WOTConsole"), TEXT("WOTConsole"),
		Expected[0].LogicalId, FColor::Magenta, /*bNeverStream*/ true,
		Expected[0].Width, Expected[0].Height, /*MipCount*/ 1,
		/*bNoMipmaps*/ true);
	if (TestNotNull(TEXT("foreign public transient occupant"), ForeignTexture))
	{
		ForeignTexture->ClearFlags(RF_Standalone);
		ForeignTexture->SetFlags(RF_Transient);
	}
	if (TestNotNull(TEXT("foreign occupant payload subsystem"), ForeignPayloads))
	{
		TestTrue(TEXT("foreign exact-path occupant fails closed"),
			ForeignPayloads->PrepareFrontEndMenuTextures().IsError());
		for (int32 Index = 1; Index < UE_ARRAY_COUNT(Expected); ++Index)
		{
			TestNull(FString::Printf(
				TEXT("foreign occupant rejection leaves consumer %d unpublished"),
				Index), FindObject<UTexture2D>(nullptr,
					*Expected[Index].AssetPath, EFindObjectFlags::ExactClass));
		}
	}
	if (ForeignTexture)
	{
		ForeignTexture->ReleaseResource();
		ForeignTexture->ClearFlags(RF_Public | RF_Standalone);
		const FName RetiredName = MakeUniqueObjectName(GetTransientPackage(),
			UTexture2D::StaticClass(), TEXT("WoTForeignTexture"));
		(void)ForeignTexture->Rename(*RetiredName.ToString(),
			GetTransientPackage(), REN_DontCreateRedirectors
				| REN_NonTransactional | REN_DoNotDirty);
		ForeignTexture->MarkAsGarbage();
	}
	WoTPayloadTests_DestroyWorld(ForeignState);
	CollectGarbage(RF_NoFlags);
	NoConsumerWasPublished(TEXT("foreign occupant teardown"));
	IFileManager::Get().DeleteDirectory(*ForeignFixture.CacheDirectory,
		/*RequireExists*/ false, /*Tree*/ true);

	const FWoTPayloadTests_MenuFixture RollbackFixture =
		WoTPayloadTests_WriteMenuFixture();
	TestTrue(TEXT("rollback fixture publishes"), RollbackFixture.bPublished);
	FWoTPayloadTests_World RollbackState = WoTPayloadTests_CreateWorld(
		RollbackFixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("rollback fixture activates"), RollbackState.bActivated);
	UWoTPayloadSubsystem* RollbackPayloads = RollbackState.GameInstance
		? RollbackState.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (TestNotNull(TEXT("rollback payload subsystem"), RollbackPayloads))
	{
		RollbackPayloads->FailTextureConsumerInjectionAtForTesting(2);
		TestTrue(TEXT("forced mid-batch injection fails closed"),
			RollbackPayloads->PrepareFrontEndMenuTextures().IsError());
		NoConsumerWasPublished(TEXT("forced injection rollback"));
		TWoTBootstrapResult<TArray<UTexture2D*>> Retry =
			RollbackPayloads->PrepareFrontEndMenuTextures();
		TestFalse(TEXT("rollback leaves subsystem reusable"), Retry.IsError());
	}
	WoTPayloadTests_DestroyWorld(RollbackState);
	CollectGarbage(RF_NoFlags);
	NoConsumerWasPublished(TEXT("rollback world teardown"));
	IFileManager::Get().DeleteDirectory(*RollbackFixture.CacheDirectory,
		/*RequireExists*/ false, /*Tree*/ true);

	const FWoTPayloadTests_MenuFixture Fixture = WoTPayloadTests_WriteMenuFixture();
	TestTrue(TEXT("exact six-object fixture publishes"), Fixture.bPublished);
	FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("exact six-object fixture activates"), State.bActivated);
	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	TArray<TWeakObjectPtr<UTexture2D>> LifecycleATextures;
	if (TestNotNull(TEXT("exact-six payload subsystem"), Payloads))
	{
		TWoTBootstrapResult<TArray<UTexture2D*>> Prepared =
			Payloads->PrepareFrontEndMenuTextures();
		TestFalse(TEXT("exact six consumers prepare"), Prepared.IsError());
		if (!Prepared.IsError())
		{
			TArray<UTexture2D*> Textures = MoveTemp(Prepared).GetValue();
			LifecycleATextures.Reserve(Textures.Num());
			for (UTexture2D* Texture : Textures)
			{
				LifecycleATextures.Add(Texture);
			}
			TestEqual(TEXT("exactly six texture consumers publish"),
				Textures.Num(), static_cast<int32>(UE_ARRAY_COUNT(Expected)));
			for (int32 Index = 0; Index < Textures.Num(); ++Index)
			{
				UTexture2D* Texture = Textures[Index];
				if (!TestNotNull(FString::Printf(TEXT("consumer %d exists"), Index),
					Texture)) continue;
				TestEqual(FString::Printf(TEXT("consumer %d exact object path"), Index),
					Texture->GetPathName(), Expected[Index].AssetPath);
				TestEqual(FString::Printf(TEXT("consumer %d width"), Index),
					Texture->GetSizeX(), Expected[Index].Width);
				TestEqual(FString::Printf(TEXT("consumer %d height"), Index),
					Texture->GetSizeY(), Expected[Index].Height);
				const UWoTPayloadAssetUserData* Binding = Cast<
					UWoTPayloadAssetUserData>(Texture->GetAssetUserDataOfClass(
						UWoTPayloadAssetUserData::StaticClass()));
				if (TestNotNull(FString::Printf(TEXT("consumer %d is bound"), Index),
					Binding))
				{
					TestEqual(FString::Printf(TEXT("consumer %d exact logical id"), Index),
						Binding->PayloadKey, Expected[Index].LogicalId);
				}
			}
			TestTrue(TEXT("all six consumers are in the injected set"),
				Payloads->RebindLoadedPayloads(Textures));
			for (int32 Index = 0; Index < Textures.Num(); ++Index)
			{
				TestTrue(FString::Printf(
					TEXT("consumer %d is discoverable by exact LoadObject"), Index),
					LoadObject<UTexture2D>(nullptr, *Expected[Index].AssetPath)
						== Textures[Index]);
			}
			TestTrue(TEXT("real menu style binds lifecycle A consumers"),
				MenuStyle.BindTextureConsumers(Textures));
			const TArray<UTexture2D*> Styled =
				MenuStyle.RequiredTexturePayloads();
			TestEqual(TEXT("seven real menu brush records are rebound"),
				Styled.Num(), 7);
			const int32 ExpectedIndices[] = {0, 1, 1, 2, 3, 4, 5};
			for (int32 BrushIndex = 0; BrushIndex < Styled.Num(); ++BrushIndex)
			{
				TestTrue(FString::Printf(
					TEXT("brush %d binds its exact prepared texture"), BrushIndex),
					Styled[BrushIndex] == Textures[ExpectedIndices[BrushIndex]]);
			}
			TestTrue(TEXT("real style payload set rebinds"),
				Payloads->RebindLoadedPayloads(Styled));
			TestTrue(TEXT("real style brushes refresh"),
				MenuStyle.RefreshTextureBrushes());
			const TArray<uint8> ConsolePixels =
				WoTPayloadTests_TextureMipBytes(Textures[0], 0);
			TestTrue(TEXT("console carries a real injected opaque pixel"),
				ConsolePixels.Num() >= 4 && ConsolePixels[0] == 1
					&& ConsolePixels[1] == 1 && ConsolePixels[2] == 1
					&& ConsolePixels[3] == 255);
			const TArray<uint8> SlidePixels =
				WoTPayloadTests_TextureMipBytes(Textures[2], 0);
			// Masked RGB dilation intentionally fills the transparent texel's
			// color from an opaque neighbor, but its authenticated alpha remains
			// zero and the following source texel remains opaque.
			TestTrue(TEXT("masked slide carries authenticated alpha semantics"),
				SlidePixels.Num() >= 8 && SlidePixels[3] == 0
					&& SlidePixels[7] == 255);
			TWoTBootstrapResult<TArray<UTexture2D*>> Repeated =
				Payloads->PrepareFrontEndMenuTextures();
			TestFalse(TEXT("exact-six preparation is idempotent"),
				Repeated.IsError());
			if (!Repeated.IsError())
			{
				TestEqual(TEXT("repeat preserves exact six"),
					Repeated.GetValue().Num(), Textures.Num());
				for (int32 Index = 0; Index < Textures.Num(); ++Index)
				{
					TestTrue(FString::Printf(
						TEXT("repeat consumer %d preserves identity"), Index),
						Repeated.GetValue()[Index] == Textures[Index]);
				}
			}
		}
	}
	WoTPayloadTests_DestroyWorld(State);
	CollectGarbage(RF_NoFlags);
	TestEqual(TEXT("lifecycle A clears global style payloads"),
		MenuStyle.RequiredTexturePayloads().Num(), 0);
	NoConsumerWasPublished(TEXT("lifecycle A teardown"));
	IFileManager::Get().DeleteDirectory(
		*Fixture.CacheDirectory, /*RequireExists*/ false, /*Tree*/ true);

	// A fresh subsystem must create a fresh exact generation, inject from its
	// authenticated lease, and refresh the same process-global style.
	const FWoTPayloadTests_MenuFixture FixtureB =
		WoTPayloadTests_WriteMenuFixture();
	TestTrue(TEXT("lifecycle B fixture publishes"), FixtureB.bPublished);
	FWoTPayloadTests_World StateB = WoTPayloadTests_CreateWorld(
		FixtureB.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("lifecycle B fixture activates"), StateB.bActivated);
	UWoTPayloadSubsystem* PayloadsB = StateB.GameInstance
		? StateB.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (TestNotNull(TEXT("lifecycle B payload subsystem"), PayloadsB))
	{
		TWoTBootstrapResult<TArray<UTexture2D*>> PreparedB =
			PayloadsB->PrepareFrontEndMenuTextures();
		TestFalse(TEXT("lifecycle B creates exact six"), PreparedB.IsError());
		if (!PreparedB.IsError())
		{
			TArray<UTexture2D*> TexturesB = MoveTemp(PreparedB).GetValue();
			TestEqual(TEXT("lifecycle B retains six consumers"),
				TexturesB.Num(), 6);
			for (int32 Index = 0; Index < TexturesB.Num(); ++Index)
			{
				TestTrue(FString::Printf(
					TEXT("lifecycle B creates fresh consumer identity %d"), Index),
					LifecycleATextures.IsValidIndex(Index)
						&& TWeakObjectPtr<UTexture2D>(TexturesB[Index])
							!= LifecycleATextures[Index]);
			}
			TestTrue(TEXT("real menu style binds lifecycle B consumers"),
				MenuStyle.BindTextureConsumers(TexturesB));
			const TArray<UTexture2D*> StyledB =
				MenuStyle.RequiredTexturePayloads();
			TestEqual(TEXT("lifecycle B rewires all seven brushes"),
				StyledB.Num(), 7);
			TestTrue(TEXT("lifecycle B real style payload set rebinds"),
				PayloadsB->RebindLoadedPayloads(StyledB));
			TestTrue(TEXT("lifecycle B real style brushes refresh"),
				MenuStyle.RefreshTextureBrushes());
		}
	}
	WoTPayloadTests_DestroyWorld(StateB);
	IFileManager::Get().DeleteDirectory(
		*FixtureB.CacheDirectory, /*RequireExists*/ false, /*Tree*/ true);
	CollectGarbage(RF_NoFlags);
	TestEqual(TEXT("lifecycle B clears global style payloads"),
		MenuStyle.RequiredTexturePayloads().Num(), 0);
	NoConsumerWasPublished(TEXT("two-lifecycle teardown"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadTextureMipCanonicalizationSpec,
	"WoT.Payload.TextureMipCanonicalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadTextureMipCanonicalizationSpec::RunTest(const FString&)
{
	const FString PackageName = TEXT("WoTPayloadMipCanonicalizationFixture");
	const FString AssetName = TEXT("Stone");
	FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteMip0OnlyFixture(
		PackageName, AssetName);
	TestTrue(TEXT("mip canonicalization cache-v3 fixture publishes"),
		Fixture.bPublished);
	UTexture2D* Texture = WoTPayloadTests_CreateMipmappedDestination(
		PackageName, AssetName, Fixture.CacheKey);
	TestFalse(TEXT("multi-mip placeholder begins streamable"),
		Texture && Texture->NeverStream != 0);
	FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("mip canonicalization generation activates"),
		State.bActivated);
	ON_SCOPE_EXIT
	{
		WoTPayloadTests_DestroyWorld(State);
		WoTPayloadTests_DestroyDestination(Texture);
		WoTPayloadTests_DeleteFixture(Fixture);
	};

	const FTexturePlatformData* PlatformData = Texture
		? Texture->GetPlatformData() : nullptr;
	if (TestNotNull(TEXT("short-cache destination has injected platform data"),
		PlatformData))
	{
		TestEqual(TEXT("one cache mip fills the complete destination chain"),
			PlatformData->Mips.Num(), 3);
		if (PlatformData->Mips.Num() == 3)
		{
			TestEqual(TEXT("generated mip 1 width"),
				PlatformData->Mips[1].SizeX, 2);
			TestEqual(TEXT("generated mip 1 height"),
				PlatformData->Mips[1].SizeY, 2);
			TestEqual(TEXT("generated mip 2 width"),
				PlatformData->Mips[2].SizeX, 1);
			TestEqual(TEXT("generated mip 2 height"),
				PlatformData->Mips[2].SizeY, 1);
		}
	}

	auto GrayBGRA = [](std::initializer_list<uint8> Values)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(static_cast<int32>(Values.size()) * 4);
		for (const uint8 Value : Values)
		{
			Bytes.Append({Value, Value, Value, 255});
		}
		return Bytes;
	};
	const TArray<uint8> ExpectedMip0 = GrayBGRA({
		1, 0, 1, 0,
		0, 0, 0, 0,
		1, 0, 5, 0,
		0, 0, 0, 0,
	});
	const TArray<uint8> ExpectedMip1 = GrayBGRA({0, 0, 0, 1});
	const TArray<uint8> ExpectedMip2 = GrayBGRA({1});
	TestTrue(TEXT("cache mip 0 is installed without alteration"),
		WoTPayloadTests_TextureMipBytes(Texture, 0) == ExpectedMip0);
	TestTrue(TEXT("mip 1 matches the reference 2x2 box downsample"),
		WoTPayloadTests_TextureMipBytes(Texture, 1) == ExpectedMip1);
	TestTrue(TEXT("mip 2 retains UE's unquantized recursive float parent"),
		WoTPayloadTests_TextureMipBytes(Texture, 2) == ExpectedMip2);
	TestTrue(TEXT("generated-chain injection disables stock streaming"),
		Texture && Texture->NeverStream != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadStrippedTextureRehydrationSpec,
	"WoT.Payload.StrippedTextureRehydration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadStrippedTextureRehydrationSpec::RunTest(const FString&)
{
	const FString PackageName = TEXT("WoTPayloadStrippedTextureFixture");
	const FString AssetName = TEXT("Stone");
	FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteFixture(
		PackageName, AssetName, EWoTPayloadTests_FixtureMode::Valid);
	TestTrue(TEXT("stripped rehydration cache-v3 fixture publishes"),
		Fixture.bPublished);
	UTexture2D* Texture = WoTPayloadTests_CreateStrippedDestination(
		PackageName, AssetName, Fixture.CacheKey);
	FWoTPayloadTests_World State;
	ON_SCOPE_EXIT
	{
		// Production lifetime ordering: EndPlay, then game-instance Shutdown,
		// before the bound texture/package fixtures are released.
		WoTPayloadTests_DestroyWorld(State);
		WoTPayloadTests_DestroyDestination(Texture);
		WoTPayloadTests_DeleteFixture(Fixture);
	};

	if (TestNotNull(TEXT("stripped placeholder texture exists"), Texture))
	{
		TestTrue(TEXT("stripped source policy produces opaque-magenta BGRA8 pixels"),
			WoTPayloadTests_TextureMipIsOpaqueMagenta(Texture));
		TestTrue(TEXT("stripped placeholder disables stock streaming"),
			Texture->NeverStream != 0);
		const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		if (TestNotNull(TEXT("stripped placeholder has platform data"), PlatformData))
		{
			TestEqual(TEXT("stripped placeholder preserves source width"),
				PlatformData->SizeX, 2);
			TestEqual(TEXT("stripped placeholder preserves source height"),
				PlatformData->SizeY, 2);
			TestEqual(TEXT("no-mip stripped destination has exactly one mip"),
				PlatformData->Mips.Num(), 1);
		}
		const UWoTPayloadAssetUserData* Binding = Cast<UWoTPayloadAssetUserData>(
			Texture->GetAssetUserDataOfClass(
				UWoTPayloadAssetUserData::StaticClass()));
		if (TestNotNull(TEXT("stripped placeholder has payload binding"), Binding))
		{
			TestEqual(TEXT("stripped binding retains the canonical payload key"),
				Binding->PayloadKey, Fixture.CacheKey);
			TestEqual(TEXT("stripped binding retains DestinationMipCount"),
				Binding->DestinationMipCount, 1);
		}
	}

	State = WoTPayloadTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("stripped rehydration generation activates"),
		State.bActivated);
	const FString WoTPayloadTests_ExpectedTextureHash =
		TEXT("ee8a624d11a4db65ef76d0f2fcd7ef1f79b4552e4ffaa8cc001df0d5cb463037");
	TestEqual(TEXT("existing injector rehydrates the stripped placeholder"),
		WoTPayloadTests_TextureMipHash(Texture),
		WoTPayloadTests_ExpectedTextureHash);
	TestFalse(TEXT("real cached pixels replaced the magenta placeholder"),
		WoTPayloadTests_TextureMipIsOpaqueMagenta(Texture));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadToggleOffSpec,
	"WoT.Payload.ToggleOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadToggleOffSpec::RunTest(const FString&)
{
	const FString PackageName = TEXT("WoTPayloadToggleFixture");
	const FString AssetName = TEXT("Stone");
	FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteFixture(
		PackageName, AssetName, EWoTPayloadTests_FixtureMode::Valid);
	UTexture2D* Texture = WoTPayloadTests_CreateDestination(
		PackageName, AssetName, Fixture.CacheKey, 0x7f);
	const FString BeforeHash = WoTPayloadTests_TextureMipHash(Texture);
	FWoTPayloadTests_World State = WoTPayloadTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ false);
	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (TestNotNull(TEXT("disabled payload subsystem still exists"), Payloads))
	{
		TestFalse(TEXT("payload injection is off"), Payloads->IsInjectionEnabled());
		TestFalse(TEXT("disabled subsystem does not read the manifest"),
			Payloads->IsManifestReady());
		TestNull(TEXT("disabled subsystem exposes no cache entries"),
			Payloads->FindTexturePayload(Fixture.CacheKey));
	}
	TestEqual(TEXT("toggle OFF leaves placeholder bytes untouched"),
		WoTPayloadTests_TextureMipHash(Texture), BeforeHash);
	TestFalse(TEXT("toggle OFF leaves streaming policy untouched"), Texture->NeverStream != 0);

	WoTPayloadTests_DestroyWorld(State);
	WoTPayloadTests_DestroyDestination(Texture);
	WoTPayloadTests_DeleteFixture(Fixture);
	return true;
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadFractalSeedRehydrationSpec,
	"WoT.Payload.FractalSeedRehydration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadFractalSeedRehydrationSpec::RunTest(const FString& Parameters)
{
	// Stripped catalogs omit the retail p8 seed (the last Audit-1 leak); the
	// fractal subsystem rehydrates PaletteRGBA/InitialBitmap from the payload
	// cache at bind time using the "texture:<Pkg>__<Tex>" key derived from the
	// record's own object path. A Fire record needs exactly those two arrays,
	// so it exercises the whole mapping (source/glass refs ride the same key
	// derivation + mip-0 extraction).
	const FWoTPayloadTests_Fixture Fixture = WoTPayloadTests_WriteFractalSeedFixture(
		TEXT("SpecFrPkg"), TEXT("SpecFractal"));
	FWoTPayloadTests_World State =
		WoTPayloadTests_CreateWorld(Fixture.CacheDirectory, /*bInjection*/ true);
	if (!TestNotNull(TEXT("world"), State.World))
	{
		WoTPayloadTests_DeleteFixture(Fixture);
		return false;
	}
	UWoTFractalSubsystem* Fractals =
		State.World->GetSubsystem<UWoTFractalSubsystem>();
	if (!TestNotNull(TEXT("fractal subsystem"), Fractals))
	{
		WoTPayloadTests_DestroyWorld(State);
		WoTPayloadTests_DeleteFixture(Fixture);
		return false;
	}

	// A seedless record matching the fixture (the cache blob is 2x2, 2 mips).
	FWoTFractalTextureRecord Record;
	Record.PackageName = TEXT("SpecFrPkg");
	Record.TextureName = TEXT("SpecFractal");
	Record.ObjectPath = TEXT("SpecFrPkg.SpecFractal");
	Record.FractalClass = EWoTFractalClass::Fire;
	Record.Width = 8;
	Record.Height = 8;
	Fractals->RegisterRecordForTesting(Record);

	AActor* Actor = State.World->SpawnActor<AActor>();
	UStaticMeshComponent* Component = nullptr;
	UMaterialInterface* GridMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshFBAddGlow.M_WoT_MeshFBAddGlow"));
	if (TestNotNull(TEXT("actor"), Actor) &&
		TestNotNull(TEXT("grid master for the bindable slot"), GridMaster))
	{
		Component = NewObject<UStaticMeshComponent>(Actor);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		Component->SetMaterial(0,
			UMaterialInstanceDynamic::Create(GridMaster, Actor));
	}
	// Activation runs the palette gate + the sim's size validation — it can
	// only succeed if both arrays rehydrated from the cache.
	TestTrue(TEXT("seedless record activates by rehydrating from the payload cache"),
		Component && Fractals->BindMaterialSlotForTesting(Component, 0, Record));
	TestNotNull(TEXT("live texture exists for the rehydrated record"),
		Fractals->FindLiveTexture(TEXT("SpecFrPkg.SpecFractal")));

	// Retail fractal refs are usually GROUP-qualified ("Pkg.Group.Tex" —
	// ScottT.Effects.S_rainTex01, NatureT.Water.WatrPurp_458) while manifest
	// keys are always "texture:<Pkg>__<Tex>" (build_payload_cache drops the
	// group). The key derivation must drop it too; a naive dot replacement
	// produced "texture:Pkg__Group__Tex" and every grouped wet/ice seed failed
	// to rehydrate in the stripped client (Battle2_02 rain, 2026-07-18).
	FWoTFractalTextureRecord Grouped = Record;
	Grouped.ObjectPath = TEXT("SpecFrPkg.Effects.SpecFractal");
	UStaticMeshComponent* GroupedComponent = nullptr;
	if (Actor && GridMaster)
	{
		GroupedComponent = NewObject<UStaticMeshComponent>(Actor);
		Actor->AddInstanceComponent(GroupedComponent);
		GroupedComponent->RegisterComponent();
		GroupedComponent->SetMaterial(0,
			UMaterialInstanceDynamic::Create(GridMaster, Actor));
	}
	TestTrue(TEXT("group-qualified record rehydrates via the group-less manifest key"),
		GroupedComponent
			&& Fractals->BindMaterialSlotForTesting(GroupedComponent, 0, Grouped));
	TestNotNull(TEXT("live texture exists for the group-qualified record"),
		Fractals->FindLiveTexture(TEXT("SpecFrPkg.Effects.SpecFractal")));

	WoTPayloadTests_DestroyWorld(State);
	WoTPayloadTests_DeleteFixture(Fixture);

	// Without a cache the same seedless record must fail CLOSED (no crash, no
	// activation) — the stripped pak's fallback look, never garbage seeds. The
	// manifest loader logs a real read error; the preflight wrapper itself is
	// a normal awaiting-activation state and logs at Display only.
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 0);
	FWoTPayloadTests_World NoCache = WoTPayloadTests_CreateWorld(
		TEXT("Nonexistent_WoTFractalSeedDir"), /*bInjection*/ true);
	if (TestNotNull(TEXT("no-cache world"), NoCache.World))
	{
		UWoTFractalSubsystem* NoCacheFractals =
			NoCache.World->GetSubsystem<UWoTFractalSubsystem>();
		AActor* NoCacheActor = NoCache.World->SpawnActor<AActor>();
		UStaticMeshComponent* NoCacheComponent = NewObject<UStaticMeshComponent>(NoCacheActor);
		NoCacheActor->AddInstanceComponent(NoCacheComponent);
		NoCacheComponent->RegisterComponent();
		FWoTFractalTextureRecord Seedless = Record;
		NoCacheFractals->RegisterRecordForTesting(Seedless);
		TestFalse(TEXT("seedless record without a cache fails closed"),
			NoCacheFractals->BindMaterialSlotForTesting(NoCacheComponent, 0, Seedless));
	}
	WoTPayloadTests_DestroyWorld(NoCache);
	return true;
}
#endif // !WOT_GUARDED_BOOTSTRAP

#endif // WITH_AUTOMATION_TESTS
