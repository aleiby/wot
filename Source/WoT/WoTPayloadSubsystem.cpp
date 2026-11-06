#include "WoTPayloadSubsystem.h"

#include "Assets/WoTRuntimeAssets.h"
#include "Assets/WoTRetailIdentity.h"
#include "Mesh/WoTRuntimeVertexMesh.h"
#include "RetailData/WoTRetailFamilies.h"
#include "RetailData/WoTRetailFamilyPins.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "WoTRetailReader/CacheBuilder.h"

#include "Async/Async.h"
#include "WoTBootstrapHeartbeat.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Animation/MorphTarget.h"
#include "Components/ExternalMorphSet.h"
#include "Engine/Texture2D.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/App.h"
#include "PhysicsEngine/BodySetup.h"
#include "RHIShaderPlatform.h"
#include "RHIGlobals.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "WoTGameInstance.h"
#include "WoTMenuStyle.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTBootstrapSubsystem.h"
#include "WoTFrontEndCatalog.h"
#include "WoTSessionSubsystem.h"
#include "WoTAnimData.h"
#include "WoTMeshPayloadLayout.h"
#include "WoTBSPPayload.h"
#include "WoTPayloadSoundWave.h"
#include "WoTSha256.h"
#include "WoTVertexAnimComponent.h"
#include "Payload/WoTPayloadActivation.h"
#include "Payload/WoTRuntimeProceduralResource.h"
#include "Payload/WoTRuntimeRegistry.h"
#include "Payload/WoTFrontEndFontOwner.h"
#include "Payload/WoTVerifiedBlobReader.h"

#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogWoTPayload, Log, All);

namespace
{
	constexpr uint64 WoTPayload_MaxEntries = 10000;
	constexpr uint64 WoTPayload_MaxBlobSize = 256ull * 1024ull * 1024ull;
	constexpr uint64 WoTPayload_MaxTextureDimension = 8192;
	constexpr uint64 WoTPayload_MaxTotalExpandedBytes = 2ull * 1024ull * 1024ull * 1024ull;
	constexpr uint64 WoTPayload_MaxTextureMips = 32;
	constexpr uint64 WoTPayload_MaxBSPVertices = 2000000;
	constexpr uint64 WoTPayload_MaxBSPIndices = 6000000;
	constexpr uint64 WoTPayload_MaxBSPSections = 4096;
	constexpr uint64 WoTPayload_MaxBSPSlotNameBytes = 1024;
	constexpr float WoTPayload_MaxBSPCoordinateAbs = 10000000.0f;
	constexpr float WoTPayload_MeshLatticeStep = 1.0f;
	constexpr float WoTPayload_MeshLatticeTolerance = 1.e-4f;
	constexpr float WoTPayload_MeshLatticeMaxIndex = 1023.0f;
	constexpr int64 WoTPayload_MaxManifestBytes = 64ll * 1024ll * 1024ll;
	constexpr uint64 WoTPayload_MaxExactJsonInteger = (1ull << 53) - 1ull;



	bool WoTPayload_MakeVerifiedDescriptor(
		const FString& CacheKey, const TCHAR* Domain,
		const TCHAR* Schema, uint64 Size, const FString& Sha256,
		FWoTCacheBlobDescriptor& OutDescriptor, FString& OutError)
	{
		TWoTBootstrapResult<FWoTSha256> ParsedSha =
			FWoTSha256::FromHex(Sha256);
		if (ParsedSha.IsError())
		{
			OutError = MoveTemp(ParsedSha).GetError().ToString();
			return false;
		}
		OutDescriptor.LogicalId =
			CacheKey.StartsWith(TEXT("cache/"), ESearchCase::CaseSensitive)
			? CacheKey
			: FString::Printf(TEXT("cache/%s/%s"), Domain, *CacheKey);
		OutDescriptor.Schema = Schema;
		OutDescriptor.Size = Size;
		OutDescriptor.Sha256 = MoveTemp(ParsedSha).GetValue();
		return true;
	}

	bool WoTPayload_IsMeshLatticeComponent(const float Component)
	{
		if (!FMath::IsFinite(Component))
		{
			return false;
		}
		const float LatticeIndex = FMath::RoundToFloat(
			Component / WoTPayload_MeshLatticeStep);
		return LatticeIndex >= 0.0f
			&& LatticeIndex <= WoTPayload_MeshLatticeMaxIndex
			&& FMath::IsNearlyEqual(Component,
				LatticeIndex * WoTPayload_MeshLatticeStep,
				WoTPayload_MeshLatticeTolerance);
	}

	bool WoTPayload_IsMeshLatticePosition(const FVector3f& Position)
	{
		return WoTPayload_IsMeshLatticeComponent(Position.X)
			&& WoTPayload_IsMeshLatticeComponent(Position.Y)
			&& WoTPayload_IsMeshLatticeComponent(Position.Z);
	}

	TAutoConsoleVariable<int32> WoTPayload_InjectionCVar(
		TEXT("wot.PayloadInjection"), 0,
		TEXT("Load player-local WoT payloads (0=off, 1=on)."),
		ECVF_Default);

#if WITH_AUTOMATION_TESTS
	TOptional<bool> WoTPayload_TestInjectionOverride;
#if WITH_AUTOMATION_TESTS
	bool WoTPayload_FailNextActivationForTesting = false;
#endif
#endif

	TWeakObjectPtr<UWoTPayloadSubsystem> WoTPayload_ActiveSubsystem;
	FCriticalSection WoTPayload_ActiveSoundBindingsMutex;
	UWoTPayloadSubsystem* WoTPayload_ActiveSoundBindingOwner = nullptr;
	TMap<FObjectKey, TSharedPtr<const FWoTSoundPayloadData>>
		WoTPayload_ActiveSoundBindings;

	struct FWoTPayloadBlobRecord
	{
		FString Path;
		FString SHA256;
		uint64 Size = 0;
	};

	struct FWoTPayloadEntryBase
	{
		EWoTPayloadType Type = EWoTPayloadType::Texture;
		FString TypeName;
		FString PackageName;
		FString AssetName;
		FWoTPayloadBlobRecord Blob;
	};

	bool WoTPayload_CheckedAdd(uint64 A, uint64 B, uint64& Out)
	{
		if (B > MAX_uint64 - A)
		{
			return false;
		}
		Out = A + B;
		return true;
	}

	bool WoTPayload_CheckedMultiply(uint64 A, uint64 B, uint64& Out)
	{
		if (A != 0 && B > MAX_uint64 / A)
		{
			return false;
		}
		Out = A * B;
		return true;
	}

	bool WoTPayload_ValidateTextureMipLayout(uint64 BaseWidth, uint64 BaseHeight,
		uint32 MipIndex, uint64 Width, uint64 Height, uint64 Stored,
		uint64 Expanded)
	{
		const uint64 ExpectedWidth = FMath::Max<uint64>(1, BaseWidth >> MipIndex);
		const uint64 ExpectedHeight = FMath::Max<uint64>(1, BaseHeight >> MipIndex);
		uint64 ExpectedStored = 0;
		uint64 ExpectedExpanded = 0;
		return Width == ExpectedWidth && Height == ExpectedHeight
			&& WoTPayload_CheckedMultiply(ExpectedWidth, ExpectedHeight, ExpectedStored)
			&& Stored == ExpectedStored
			&& WoTPayload_CheckedMultiply(ExpectedStored, 4, ExpectedExpanded)
			&& Expanded == ExpectedExpanded;
	}

	int32 WoTPayload_FullTextureMipCount(uint32 Width, uint32 Height)
	{
		return FMath::FloorLog2(FMath::Max(Width, Height)) + 1;
	}

#if !WOT_GUARDED_BOOTSTRAP
	bool WoTPayload_ReadFileBounded(const FString& Filename, uint64 MaximumBytes,
		const TOptional<uint64>& ExactBytes, TArray<uint8>& OutBytes,
		FString& OutError)
	{
		OutBytes.Reset();
		TUniquePtr<IFileHandle> File(
			FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Filename));
		if (!File)
		{
			OutError = TEXT("cannot open file");
			return false;
		}

		// Size() and Read() operate on this same open handle. Reading exactly the
		// validated amount and then probing one more byte closes the stat/reopen
		// growth race without ever allocating beyond the compiled cap.
		const int64 OpenSize = File->Size();
		if (OpenSize < 0 || static_cast<uint64>(OpenSize) > MaximumBytes
			|| OpenSize > MAX_int32
			|| (ExactBytes.IsSet()
				&& static_cast<uint64>(OpenSize) != ExactBytes.GetValue()))
		{
			OutError = FString::Printf(TEXT("open-handle size %lld is invalid"), OpenSize);
			return false;
		}

		OutBytes.SetNumUninitialized(static_cast<int32>(OpenSize));
		if ((OpenSize != 0
				&& (!File->Read(OutBytes.GetData(), OpenSize)
					|| File->Tell() != OpenSize)))
		{
			OutBytes.Reset();
			OutError = TEXT("file read was incomplete");
			return false;
		}
		uint8 ExtraByte = 0;
		if (File->Read(&ExtraByte, 1))
		{
			OutBytes.Reset();
			OutError = TEXT("file grew beyond its validated open-handle size");
			return false;
		}
		return true;
	}
#endif

	bool WoTPayload_IsSHA256(const FString& Value)
	{
		if (Value.Len() != 64)
		{
			return false;
		}
		for (TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool WoTPayload_IsSafeComponent(const FString& Value, bool bAllowSeparator)
	{
		if (Value.IsEmpty() || Value == TEXT(".") || Value == TEXT("..")
			|| (!bAllowSeparator && Value.Contains(TEXT("__"))))
		{
			return false;
		}
		for (TCHAR Character : Value)
		{
			if (Character == TEXT('/') || Character == TEXT('\\') || Character == 0
				|| Character == TEXT('\r') || Character == TEXT('\n')
				|| Character < 32 || Character == 127)
			{
				return false;
			}
		}
		return true;
	}

	bool WoTPayload_IsSafeRelativePath(const FString& Value)
	{
		if (Value.IsEmpty() || Value.StartsWith(TEXT("/"))
			|| Value.Contains(TEXT("\\")) || Value.EndsWith(TEXT("/")))
		{
			return false;
		}
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT("/"), /*CullEmpty*/ false);
		if (Parts.IsEmpty())
		{
			return false;
		}
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."))
			{
				return false;
			}
			for (TCHAR Character : Part)
			{
				if (Character < 32 || Character == 127)
				{
					return false;
				}
			}
		}
		return true;
	}

	bool WoTPayload_HasExactFields(
		const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Fields,
		bool bAllowExportOccurrences = false)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		TSet<FString> Expected;
		for (const TCHAR* Field : Fields)
		{
			Expected.Add(Field);
		}
		if (bAllowExportOccurrences && Object->HasField(TEXT("export_occurrences")))
		{
			Expected.Add(TEXT("export_occurrences"));
		}
		if (Object->Values.Num() != Expected.Num())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Expected.Contains(Pair.Key))
			{
				return false;
			}
		}
		return true;
	}

	bool WoTPayload_ReadString(
		const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Object.IsValid()
			? Object->Values.Find(Field) : nullptr;
		return Value && Value->IsValid() && (*Value)->Type == EJson::String
			&& (*Value)->TryGetString(Out);
	}

	bool WoTPayload_ReadBool(
		const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Object.IsValid()
			? Object->Values.Find(Field) : nullptr;
		return Value && Value->IsValid() && (*Value)->Type == EJson::Boolean
			&& (*Value)->TryGetBool(Out);
	}

	bool WoTPayload_ReadUInt(const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field, uint64 Maximum, uint64& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Object.IsValid()
			? Object->Values.Find(Field) : nullptr;
		double Number = 0.0;
		if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Number
			|| !(*Value)->TryGetNumber(Number) || !FMath::IsFinite(Number)
			|| Number < 0.0 || Number > static_cast<double>(Maximum)
			|| FMath::FloorToDouble(Number) != Number)
		{
			return false;
		}
		Out = static_cast<uint64>(Number);
		return static_cast<double>(Out) == Number;
	}

	bool WoTPayload_ReadArray(const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field, const TArray<TSharedPtr<FJsonValue>>*& Out)
	{
		return Object.IsValid() && Object->TryGetArrayField(Field, Out) && Out;
	}

	bool WoTPayload_ReadObject(const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field, const TSharedPtr<FJsonObject>*& Out)
	{
		return Object.IsValid() && Object->TryGetObjectField(Field, Out)
			&& Out && Out->IsValid();
	}

	bool WoTPayload_ParseType(const FString& TypeName, EWoTPayloadType& Out)
	{
		if (TypeName == TEXT("textures"))
		{
			Out = EWoTPayloadType::Texture;
			return true;
		}
		if (TypeName == TEXT("sounds"))
		{
			Out = EWoTPayloadType::Sound;
			return true;
		}
		if (TypeName == TEXT("meshes"))
		{
			Out = EWoTPayloadType::Mesh;
			return true;
		}
		if (TypeName == TEXT("bsps"))
		{
			Out = EWoTPayloadType::BSP;
			return true;
		}
		return false;
	}

	int32 WoTPayload_TypeOrder(const FString& TypeName)
	{
		if (TypeName == TEXT("textures")) return 0;
		if (TypeName == TEXT("sounds")) return 1;
		if (TypeName == TEXT("meshes")) return 2;
		if (TypeName == TEXT("bsps")) return 3;
		return INDEX_NONE;
	}

	const TCHAR* WoTPayload_TypePrefix(EWoTPayloadType Type)
	{
		switch (Type)
		{
		case EWoTPayloadType::Texture: return TEXT("texture");
		case EWoTPayloadType::Sound: return TEXT("sound");
		case EWoTPayloadType::Mesh: return TEXT("mesh");
		case EWoTPayloadType::BSP: return TEXT("bsp");
		default: return TEXT("");
		}
	}

	const TCHAR* WoTPayload_Encoding(EWoTPayloadType Type)
	{
		switch (Type)
		{
		case EWoTPayloadType::Texture:
			return TEXT("ue1-p8-mips-rgba-palette-v1");
		case EWoTPayloadType::Sound:
			return TEXT("wav-v1");
		case EWoTPayloadType::Mesh:
			return TEXT("ue1-packed-verts-v1");
		case EWoTPayloadType::BSP:
			return TEXT("wot-bsp-static-mesh-v1");
		default:
			return TEXT("");
		}
	}

	FString WoTPayload_BlobPath(const FString& SHA256)
	{
		return FString::Printf(TEXT("blobs/%s/%s.blob"), *SHA256.Left(2), *SHA256);
	}

	bool WoTPayload_ParseEntryBase(const FString& Key,
		const TSharedPtr<FJsonObject>& Object, FWoTPayloadEntryBase& Out,
		FString& OutError)
	{
		FString Encoding;
		FString BlobPath;
		if (!WoTPayload_ReadString(Object, TEXT("type"), Out.TypeName)
			|| !WoTPayload_ParseType(Out.TypeName, Out.Type)
			|| !WoTPayload_ReadString(Object, TEXT("package"), Out.PackageName)
			|| !WoTPayload_ReadString(Object, TEXT("name"), Out.AssetName)
			|| !WoTPayload_ReadString(Object, TEXT("encoding"), Encoding)
			|| !WoTPayload_ReadString(Object, TEXT("blob"), BlobPath)
			|| !WoTPayload_ReadString(Object, TEXT("sha256"), Out.Blob.SHA256)
			|| !WoTPayload_ReadUInt(Object, TEXT("size"),
				WoTPayload_MaxBlobSize, Out.Blob.Size)
			|| Out.Blob.Size == 0)
		{
			OutError = TEXT("missing or invalid base metadata");
			return false;
		}
		if (!WoTPayload_IsSHA256(Out.Blob.SHA256)
			|| BlobPath != WoTPayload_BlobPath(Out.Blob.SHA256))
		{
			OutError = TEXT("invalid content-addressed blob path/hash");
			return false;
		}
		Out.Blob.Path = BlobPath;

		if (!WoTPayload_IsSafeComponent(Out.PackageName, false)
			|| !WoTPayload_IsSafeComponent(Out.AssetName, true))
		{
			OutError = TEXT("unsafe package/name grammar");
			return false;
		}
		const FString ExpectedKey = FString::Printf(TEXT("%s:%s__%s"),
			WoTPayload_TypePrefix(Out.Type), *Out.PackageName, *Out.AssetName);
		if (Key != ExpectedKey)
		{
			OutError = TEXT("semantic key disagrees with package/name");
			return false;
		}
		if (Encoding != WoTPayload_Encoding(Out.Type))
		{
			OutError = TEXT("unsupported encoding");
			return false;
		}
		if (Object->HasField(TEXT("export_occurrences")))
		{
			uint64 Occurrences = 0;
			if (!WoTPayload_ReadUInt(Object, TEXT("export_occurrences"),
				WoTPayload_MaxEntries, Occurrences) || Occurrences < 2)
			{
				OutError = TEXT("invalid export_occurrences");
				return false;
			}
		}
		return true;
	}

	bool WoTPayload_ParseTextureEntry(const FString& Key,
		const TSharedPtr<FJsonObject>& Object, const FWoTPayloadEntryBase& Base,
		FWoTTexturePayloadEntry& Out, uint64& OutExpandedBytes, FString& OutError)
	{
		if (!WoTPayload_HasExactFields(Object, {
			TEXT("type"), TEXT("package"), TEXT("name"), TEXT("encoding"),
			TEXT("blob"), TEXT("size"), TEXT("sha256"), TEXT("width"),
			TEXT("height"), TEXT("mip_count"), TEXT("mips"),
			TEXT("palette_colors"), TEXT("masked")}, true))
		{
			OutError = TEXT("missing or unknown texture metadata fields");
			return false;
		}

		uint64 Width = 0;
		uint64 Height = 0;
		uint64 MipCount = 0;
		uint64 PaletteColors = 0;
		bool bMasked = false;
		const TArray<TSharedPtr<FJsonValue>>* Mips = nullptr;
		if (!WoTPayload_ReadUInt(Object, TEXT("width"),
				WoTPayload_MaxTextureDimension, Width) || Width == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("height"),
				WoTPayload_MaxTextureDimension, Height) || Height == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("mip_count"),
				WoTPayload_MaxTextureMips, MipCount) || MipCount == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("palette_colors"), 256, PaletteColors)
			|| PaletteColors != 256
			|| !WoTPayload_ReadBool(Object, TEXT("masked"), bMasked)
			|| !WoTPayload_ReadArray(Object, TEXT("mips"), Mips)
			|| static_cast<uint64>(Mips->Num()) != MipCount)
		{
			OutError = TEXT("invalid texture dimensions/palette/mip count");
			return false;
		}

		TArray<FWoTTexturePayloadMip> ParsedMips;
		ParsedMips.Reserve(static_cast<int32>(MipCount));
		uint64 ExpandedTotal = 0;
		for (int32 MipIndex = 0; MipIndex < Mips->Num(); ++MipIndex)
		{
			const TSharedPtr<FJsonObject> MipObject = (*Mips)[MipIndex].IsValid()
				? (*Mips)[MipIndex]->AsObject() : nullptr;
			if (!WoTPayload_HasExactFields(MipObject, {
				TEXT("width"), TEXT("height"), TEXT("stored_byte_length"),
				TEXT("expanded_byte_length")}))
			{
				OutError = FString::Printf(TEXT("invalid mip %d fields"), MipIndex);
				return false;
			}

			uint64 MipWidth = 0;
			uint64 MipHeight = 0;
			uint64 Stored = 0;
			uint64 Expanded = 0;
			if (!WoTPayload_ReadUInt(MipObject, TEXT("width"),
					WoTPayload_MaxTextureDimension, MipWidth) || MipWidth == 0
				|| !WoTPayload_ReadUInt(MipObject, TEXT("height"),
					WoTPayload_MaxTextureDimension, MipHeight) || MipHeight == 0
				|| !WoTPayload_ReadUInt(MipObject, TEXT("stored_byte_length"),
					WoTPayload_MaxBlobSize, Stored)
				|| !WoTPayload_ReadUInt(MipObject, TEXT("expanded_byte_length"),
					WoTPayload_MaxTotalExpandedBytes, Expanded)
				|| !WoTPayload_ValidateTextureMipLayout(Width, Height,
					static_cast<uint32>(MipIndex), MipWidth, MipHeight,
					Stored, Expanded)
				|| !WoTPayload_CheckedAdd(ExpandedTotal, Expanded, ExpandedTotal)
				|| ExpandedTotal > WoTPayload_MaxTotalExpandedBytes)
			{
				OutError = FString::Printf(
					TEXT("mip %d dimensions or byte lengths break the base-derived chain"),
					MipIndex);
				return false;
			}

			FWoTTexturePayloadMip& Mip = ParsedMips.AddDefaulted_GetRef();
			Mip.Width = static_cast<uint32>(MipWidth);
			Mip.Height = static_cast<uint32>(MipHeight);
			Mip.StoredByteLength = static_cast<uint32>(Stored);
			Mip.ExpandedByteLength = Expanded;
		}
		Out.CacheKey = Key;
		Out.PackageName = Base.PackageName;
		Out.AssetName = Base.AssetName;
		const FString LongPackage = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s"), *Base.PackageName, *Base.AssetName);
		Out.AssetPath = FString::Printf(TEXT("%s.%s"), *LongPackage, *Base.AssetName);
		Out.BlobPath = Base.Blob.Path;
		Out.BlobSHA256 = Base.Blob.SHA256;
		Out.BlobSize = Base.Blob.Size;
		Out.bMasked = bMasked;
		Out.Mips = MoveTemp(ParsedMips);
		OutExpandedBytes = ExpandedTotal;
		return true;
	}

	bool WoTPayload_ParseSoundEntry(const FString& Key,
		const TSharedPtr<FJsonObject>& Object, const FWoTPayloadEntryBase& Base,
		FWoTSoundPayloadEntry& Out, uint64& OutExpandedBytes, FString& OutError)
	{
		if (!WoTPayload_HasExactFields(Object, {
			TEXT("type"), TEXT("package"), TEXT("name"), TEXT("encoding"),
			TEXT("blob"), TEXT("size"), TEXT("sha256"), TEXT("pcm_format_tag"),
			TEXT("channels"), TEXT("sample_rate"), TEXT("bits_per_sample"),
			TEXT("frame_count"), TEXT("data_byte_length"),
			TEXT("duration_seconds")}, true))
		{
			OutError = TEXT("missing or unknown sound metadata fields");
			return false;
		}
		uint64 Format = 0;
		uint64 Channels = 0;
		uint64 Rate = 0;
		uint64 Bits = 0;
		uint64 Frames = 0;
		uint64 DataLength = 0;
		const TSharedPtr<FJsonValue>* DurationValue = Object->Values.Find(
			TEXT("duration_seconds"));
		double Duration = 0.0;
		uint64 Expected = 0;
		uint64 BytesPerFrame = 0;
		if (!WoTPayload_ReadUInt(Object, TEXT("pcm_format_tag"), 1, Format)
			|| Format != 1
			|| !WoTPayload_ReadUInt(Object, TEXT("channels"), 8, Channels)
			|| Channels == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("sample_rate"), 384000, Rate)
			|| Rate == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("bits_per_sample"), 16, Bits)
			|| (Bits != 8 && Bits != 16)
			|| !WoTPayload_ReadUInt(Object, TEXT("frame_count"),
				WoTPayload_MaxExactJsonInteger, Frames)
			|| !WoTPayload_ReadUInt(Object, TEXT("data_byte_length"),
				WoTPayload_MaxBlobSize, DataLength)
			|| !DurationValue || !DurationValue->IsValid()
			|| (*DurationValue)->Type != EJson::Number
			|| !(*DurationValue)->TryGetNumber(Duration) || !FMath::IsFinite(Duration)
			|| !WoTPayload_CheckedMultiply(Channels, Bits / 8, BytesPerFrame)
			|| !WoTPayload_CheckedMultiply(Frames, BytesPerFrame, Expected)
			|| Expected != DataLength
			|| Duration != static_cast<double>(Frames) / static_cast<double>(Rate))
		{
			OutError = TEXT("inconsistent PCM preflight metadata");
			return false;
		}
		Out.CacheKey = Key;
		Out.PackageName = Base.PackageName;
		Out.AssetName = Base.AssetName;
		const FString LongPackage = FString::Printf(
			TEXT("/Game/WoT/Sounds/%s/%s"), *Base.PackageName, *Base.AssetName);
		Out.AssetPath = FString::Printf(TEXT("%s.%s"), *LongPackage, *Base.AssetName);
		Out.BlobPath = Base.Blob.Path;
		Out.BlobSHA256 = Base.Blob.SHA256;
		Out.BlobSize = Base.Blob.Size;
		Out.PCMFormatTag = static_cast<uint16>(Format);
		Out.Channels = static_cast<uint16>(Channels);
		Out.SampleRate = static_cast<uint32>(Rate);
		Out.BitsPerSample = static_cast<uint16>(Bits);
		Out.FrameCount = Frames;
		Out.DataByteLength = DataLength;
		OutExpandedBytes = DataLength;
		return true;
	}

	bool WoTPayload_ParseMeshEntry(const FString& Key,
		const TSharedPtr<FJsonObject>& Object, const FWoTPayloadEntryBase& Base,
		FWoTMeshPayloadEntry& Out, uint64& OutExpandedBytes, FString& OutError)
	{
		if (!WoTPayload_HasExactFields(Object, {
			TEXT("type"), TEXT("package"), TEXT("name"), TEXT("encoding"),
			TEXT("blob"), TEXT("size"), TEXT("sha256"), TEXT("class_name"),
			TEXT("frame_verts"), TEXT("anim_frames"),
			TEXT("packed_vertex_count"), TEXT("frame_byte_length")}, true))
		{
			OutError = TEXT("missing or unknown mesh metadata fields");
			return false;
		}
		FString ClassName;
		uint64 FrameVerts = 0;
		uint64 AnimFrames = 0;
		uint64 PackedCount = 0;
		uint64 FrameBytes = 0;
		uint64 ExpectedCount = 0;
		uint64 ExpectedFrameBytes = 0;
		uint64 ExpectedBlobBytes = 0;
		if (!WoTPayload_ReadString(Object, TEXT("class_name"), ClassName)
			|| (ClassName != TEXT("Mesh") && ClassName != TEXT("LodMesh"))
			|| !WoTPayload_ReadUInt(Object, TEXT("frame_verts"),
				WoTPayload_MaxExactJsonInteger, FrameVerts) || FrameVerts == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("anim_frames"),
				WoTPayload_MaxExactJsonInteger, AnimFrames) || AnimFrames == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("packed_vertex_count"),
				WoTPayload_MaxExactJsonInteger, PackedCount)
			|| !WoTPayload_ReadUInt(Object, TEXT("frame_byte_length"),
				WoTPayload_MaxBlobSize, FrameBytes)
			|| !WoTPayload_CheckedMultiply(FrameVerts, AnimFrames, ExpectedCount)
			|| ExpectedCount != PackedCount
			|| !WoTPayload_CheckedMultiply(FrameVerts, 4, ExpectedFrameBytes)
			|| ExpectedFrameBytes != FrameBytes
			|| !WoTPayload_CheckedMultiply(FrameBytes, AnimFrames, OutExpandedBytes)
			|| OutExpandedBytes > WoTPayload_MaxTotalExpandedBytes
			|| !WoTPayload_CheckedAdd(24, OutExpandedBytes, ExpectedBlobBytes)
			|| ExpectedBlobBytes != Base.Blob.Size
			|| FrameVerts > MAX_uint32 || AnimFrames > MAX_uint32
			|| PackedCount > MAX_uint32 || FrameBytes > MAX_uint32)
		{
			OutError = TEXT("inconsistent mesh counts or blob byte length");
			return false;
		}
		Out.CacheKey = Key;
		Out.PackageName = Base.PackageName;
		Out.AssetName = Base.AssetName;
		Out.BlobPath = Base.Blob.Path;
		Out.BlobSHA256 = Base.Blob.SHA256;
		Out.BlobSize = Base.Blob.Size;
		Out.ClassName = ClassName;
		Out.FrameVertexCount = static_cast<uint32>(FrameVerts);
		Out.AnimationFrameCount = static_cast<uint32>(AnimFrames);
		Out.PackedVertexCount = static_cast<uint32>(PackedCount);
		Out.FrameByteLength = static_cast<uint32>(FrameBytes);
		return true;
	}

	bool WoTPayload_ReadFiniteVector3(const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field, FVector3f& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!WoTPayload_ReadArray(Object, Field, Values) || Values->Num() != 3)
		{
			return false;
		}
		float Components[3] = {};
		for (int32 Index = 0; Index < 3; ++Index)
		{
			double Value = 0.0;
			if (!Values->IsValidIndex(Index) || !(*Values)[Index].IsValid()
				|| (*Values)[Index]->Type != EJson::Number
				|| !(*Values)[Index]->TryGetNumber(Value)
				|| !FMath::IsFinite(Value)
				|| Value < -static_cast<double>(MAX_flt)
				|| Value > static_cast<double>(MAX_flt))
			{
				return false;
			}
			Components[Index] = static_cast<float>(Value);
		}
		Out = FVector3f(Components[0], Components[1], Components[2]);
		return true;
	}

	bool WoTPayload_ValidateBSPProvenanceFile(
		const TSharedPtr<FJsonObject>& Object)
	{
		FString Path;
		FString SHA256;
		uint64 Size = 0;
		return WoTPayload_HasExactFields(Object, {
				TEXT("path"), TEXT("size"), TEXT("sha256")})
			&& WoTPayload_ReadString(Object, TEXT("path"), Path)
			&& WoTPayload_IsSafeRelativePath(Path)
			&& WoTPayload_ReadUInt(Object, TEXT("size"),
				WoTPayload_MaxExactJsonInteger, Size)
			&& WoTPayload_ReadString(Object, TEXT("sha256"), SHA256)
			&& WoTPayload_IsSHA256(SHA256);
	}

	bool WoTPayload_ValidateBSPProvenance(
		const TSharedPtr<FJsonObject>& Object, const FString& PackageName)
	{
		if (!WoTPayload_HasExactFields(Object, {
			TEXT("schema"), TEXT("map"), TEXT("texture_dimensions_sha256"),
			TEXT("lightmap_atlas"), TEXT("sky_split"), TEXT("converter"),
			TEXT("payload_encoder"), TEXT("source_glb")})) return false;
		FString Schema;
		FString DimensionsSHA;
		const TSharedPtr<FJsonObject>* Map = nullptr;
		const TSharedPtr<FJsonObject>* Atlas = nullptr;
		const TSharedPtr<FJsonObject>* Sky = nullptr;
		const TSharedPtr<FJsonObject>* Converter = nullptr;
		const TSharedPtr<FJsonObject>* PayloadEncoder = nullptr;
		const TSharedPtr<FJsonObject>* GLB = nullptr;
		if (!WoTPayload_ReadString(Object, TEXT("schema"), Schema)
			|| Schema != TEXT("bsp-payload-provenance-v1")
			|| !WoTPayload_ReadString(Object, TEXT("texture_dimensions_sha256"),
				DimensionsSHA) || !WoTPayload_IsSHA256(DimensionsSHA)
			|| !WoTPayload_ReadObject(Object, TEXT("map"), Map)
			|| !WoTPayload_ReadObject(Object, TEXT("lightmap_atlas"), Atlas)
			|| !WoTPayload_ReadObject(Object, TEXT("sky_split"), Sky)
			|| !WoTPayload_ReadObject(Object, TEXT("converter"), Converter)
			|| !WoTPayload_ReadObject(Object, TEXT("payload_encoder"), PayloadEncoder)
			|| !WoTPayload_ReadObject(Object, TEXT("source_glb"), GLB)
			|| !WoTPayload_ValidateBSPProvenanceFile(*Map)
			|| !WoTPayload_ValidateBSPProvenanceFile(*Atlas)
			|| !WoTPayload_ValidateBSPProvenanceFile(*Sky)
			|| !WoTPayload_ValidateBSPProvenanceFile(*Converter)
			|| !WoTPayload_ValidateBSPProvenanceFile(*PayloadEncoder)
			|| !WoTPayload_ValidateBSPProvenanceFile(*GLB)) return false;
		FString MapPath;
		return WoTPayload_ReadString(*Map, TEXT("path"), MapPath)
			&& FPaths::GetBaseFilename(MapPath) == PackageName;
	}

	bool WoTPayload_ParseBSPEntry(const FString& Key,
		const TSharedPtr<FJsonObject>& Object, const FWoTPayloadEntryBase& Base,
		FWoTBSPPayloadEntry& Out, uint64& OutExpandedBytes, FString& OutError)
	{
		if (!WoTPayload_HasExactFields(Object, {
			TEXT("type"), TEXT("package"), TEXT("name"), TEXT("encoding"),
			TEXT("blob"), TEXT("size"), TEXT("sha256"), TEXT("role"),
			TEXT("vertex_count"), TEXT("index_count"), TEXT("triangle_count"),
			TEXT("section_count"), TEXT("uv_channels"), TEXT("bounds_min"),
			TEXT("bounds_max"), TEXT("tangent_algorithm"), TEXT("sections"),
			TEXT("provenance")}))
		{
			OutError = TEXT("missing or unknown BSP metadata fields");
			return false;
		}
		FString Role;
		FString TangentAlgorithm;
		uint64 Vertices = 0, Indices = 0, Triangles = 0, SectionCount = 0, UVs = 0;
		FVector3f BoundsMin, BoundsMax;
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		const TSharedPtr<FJsonObject>* Provenance = nullptr;
		if (!WoTPayload_ReadString(Object, TEXT("role"), Role)
			|| (Role != TEXT("main") && Role != TEXT("portal"))
			|| !WoTPayload_ReadUInt(Object, TEXT("vertex_count"),
				WoTPayload_MaxBSPVertices, Vertices) || Vertices == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("index_count"),
				WoTPayload_MaxBSPIndices, Indices) || Indices < 3 || Indices % 3
			|| !WoTPayload_ReadUInt(Object, TEXT("triangle_count"),
				WoTPayload_MaxBSPIndices / 3, Triangles) || Triangles != Indices / 3
			|| !WoTPayload_ReadUInt(Object, TEXT("section_count"),
				WoTPayload_MaxBSPSections, SectionCount) || SectionCount == 0
			|| !WoTPayload_ReadUInt(Object, TEXT("uv_channels"), 2, UVs) || UVs != 2
			|| !WoTPayload_ReadFiniteVector3(Object, TEXT("bounds_min"), BoundsMin)
			|| !WoTPayload_ReadFiniteVector3(Object, TEXT("bounds_max"), BoundsMax)
			|| BoundsMin.X > BoundsMax.X || BoundsMin.Y > BoundsMax.Y
			|| BoundsMin.Z > BoundsMax.Z
			|| BoundsMin.GetAbsMax() > WoTPayload_MaxBSPCoordinateAbs
			|| BoundsMax.GetAbsMax() > WoTPayload_MaxBSPCoordinateAbs
			|| !WoTPayload_ReadString(Object, TEXT("tangent_algorithm"),
				TangentAlgorithm)
			|| TangentAlgorithm != TEXT("ue-mikk-preserve-normals-v1")
			|| !WoTPayload_ReadArray(Object, TEXT("sections"), Sections)
			|| static_cast<uint64>(Sections->Num()) != SectionCount
			|| !WoTPayload_ReadObject(Object, TEXT("provenance"), Provenance)
			|| !WoTPayload_ValidateBSPProvenance(*Provenance, Base.PackageName))
		{
			OutError = TEXT("invalid BSP preflight counts/bounds/provenance");
			return false;
		}
		uint64 ExpectedFirst = 0;
		for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
		{
			if (!Sections->IsValidIndex(SectionIndex))
			{
				OutError = FString::Printf(
					TEXT("BSP section %d is missing from the declared array"), SectionIndex);
				return false;
			}
			const TSharedPtr<FJsonObject> Section = (*Sections)[SectionIndex].IsValid()
				? (*Sections)[SectionIndex]->AsObject() : nullptr;
			FString Slot;
			uint64 First = 0, Count = 0, SectionTriangles = 0, Minimum = 0, Maximum = 0;
			bool bCollision = false;
			if (!WoTPayload_HasExactFields(Section, {
				TEXT("material_slot"), TEXT("first_index"), TEXT("index_count"),
				TEXT("triangle_count"), TEXT("min_vertex"), TEXT("max_vertex"),
				TEXT("collision_enabled")})
				|| !WoTPayload_ReadString(Section, TEXT("material_slot"), Slot)
				|| Slot.IsEmpty() || FTCHARToUTF8(*Slot).Length() > WoTPayload_MaxBSPSlotNameBytes
				|| Out.MaterialSlots.Contains(Slot)
				|| !WoTPayload_ReadUInt(Section, TEXT("first_index"), Indices, First)
				|| First != ExpectedFirst
				|| !WoTPayload_ReadUInt(Section, TEXT("index_count"), Indices, Count)
				|| First > Indices || Count == 0 || Count % 3 || Count > Indices - First
				|| !WoTPayload_ReadUInt(Section, TEXT("triangle_count"),
					Indices / 3, SectionTriangles) || SectionTriangles != Count / 3
				|| !WoTPayload_ReadUInt(Section, TEXT("min_vertex"), Vertices - 1, Minimum)
				|| !WoTPayload_ReadUInt(Section, TEXT("max_vertex"), Vertices - 1, Maximum)
				|| Minimum > Maximum
				|| !WoTPayload_ReadBool(Section, TEXT("collision_enabled"), bCollision)
				|| bCollision != (Role == TEXT("main")))
			{
				OutError = FString::Printf(TEXT("invalid BSP section %d"), SectionIndex);
				return false;
			}
			for (TCHAR Character : Slot)
			{
				if (Character < 32 || Character == 127)
				{
					OutError = TEXT("unsafe BSP material slot");
					return false;
				}
			}
			Out.MaterialSlots.Add(Slot);
			Out.SectionFirstIndices.Add(static_cast<uint32>(First));
			Out.SectionIndexCounts.Add(static_cast<uint32>(Count));
			Out.SectionMinVertices.Add(static_cast<uint32>(Minimum));
			Out.SectionMaxVertices.Add(static_cast<uint32>(Maximum));
			Out.SectionCollisionEnabled.Add(bCollision);
			ExpectedFirst += Count;
		}
		uint64 VertexBytes = 0, IndexBytes = 0;
		if (ExpectedFirst != Indices
			|| !WoTPayload_CheckedMultiply(Vertices, 40, VertexBytes)
			|| !WoTPayload_CheckedMultiply(Indices, 4, IndexBytes)
			|| !WoTPayload_CheckedAdd(VertexBytes, IndexBytes, OutExpandedBytes)
			|| OutExpandedBytes > WoTPayload_MaxTotalExpandedBytes)
		{
			OutError = TEXT("BSP expanded byte count overflowed");
			return false;
		}
		Out.CacheKey = Key;
		Out.PackageName = Base.PackageName;
		Out.AssetName = Base.AssetName;
		Out.Role = Role;
		Out.TangentAlgorithm = TangentAlgorithm;
		Out.BlobPath = Base.Blob.Path;
		Out.BlobSHA256 = Base.Blob.SHA256;
		Out.BlobSize = Base.Blob.Size;
		Out.VertexCount = static_cast<uint32>(Vertices);
		Out.IndexCount = static_cast<uint32>(Indices);
		Out.SectionCount = static_cast<uint32>(SectionCount);
		Out.BoundsMin = BoundsMin;
		Out.BoundsMax = BoundsMax;
		const FString Folder = Role == TEXT("portal")
			? Base.PackageName + TEXT("_portals") : Base.PackageName;
		const FString LongPackage = FString::Printf(
			TEXT("/Game/WoT/Maps/Geo/%s/%s"), *Folder, *Base.AssetName);
		Out.AssetPath = FString::Printf(TEXT("%s.%s"), *LongPackage, *Base.AssetName);
		return true;
	}

	FString WoTPayload_SHA256(const void* Data, uint32 Size)
	{
		return WoTSha256::HashBuffer(Data, Size);
	}

	bool WoTPayload_AppendASCII(const FString& String, TArray<uint8>& Out)
	{
		if (String.Len() > MAX_int32 - Out.Num())
		{
			return false;
		}
		for (TCHAR Character : String)
		{
			if (Character > 127)
			{
				return false;
			}
			Out.Add(static_cast<uint8>(Character));
		}
		return true;
	}

	void WoTPayload_AppendUInt64LE(uint64 Value, TArray<uint8>& Out)
	{
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Out.Add(static_cast<uint8>((Value >> (ByteIndex * 8)) & 0xff));
		}
	}

	bool WoTPayload_CompactCanonicalManifest(const FString& RawManifest,
		const FString& CacheSHA256, FString& OutCore)
	{
		FString Compact;
		Compact.Reserve(RawManifest.Len());
		bool bInString = false;
		bool bEscaped = false;
		for (TCHAR Character : RawManifest)
		{
			if (Character > 127)
			{
				return false;
			}
			if (bInString)
			{
				Compact.AppendChar(Character);
				if (bEscaped)
				{
					bEscaped = false;
				}
				else if (Character == TEXT('\\'))
				{
					bEscaped = true;
				}
				else if (Character == TEXT('"'))
				{
					bInString = false;
				}
			}
			else if (Character == TEXT('"'))
			{
				bInString = true;
				Compact.AppendChar(Character);
			}
			else if (Character != TEXT(' ') && Character != TEXT('\t')
				&& Character != TEXT('\r') && Character != TEXT('\n'))
			{
				Compact.AppendChar(Character);
			}
		}
		if (bInString || bEscaped)
		{
			return false;
		}

		// The builder writes sorted JSON, so cache_sha256 is followed by the
		// next top-level member. Removing this exact compact token reproduces
		// json.dumps(..., sort_keys=True, separators=(",", ":")).
		const FString HashMember = FString::Printf(
			TEXT("\"cache_sha256\":\"%s\","), *CacheSHA256);
		const int32 HashIndex = Compact.Find(HashMember, ESearchCase::CaseSensitive);
		if (HashIndex == INDEX_NONE
			|| Compact.Find(HashMember, ESearchCase::CaseSensitive,
				ESearchDir::FromStart, HashIndex + HashMember.Len()) != INDEX_NONE)
		{
			return false;
		}
		Compact.RemoveAt(HashIndex, HashMember.Len(), EAllowShrinking::No);
		OutCore = MoveTemp(Compact);
		return true;
	}

	bool WoTPayload_ComputeCacheRoot(const FString& RawManifest,
		uint64 CacheVersion, const FString& DeclaredRoot,
		const TArray<FWoTPayloadBlobRecord>& BlobRecords,
		FString& OutRoot)
	{
		FString CanonicalCore;
		if (!WoTPayload_CompactCanonicalManifest(
			RawManifest, DeclaredRoot, CanonicalCore))
		{
			return false;
		}

		TMap<FString, FWoTPayloadBlobRecord> Unique;
		for (const FWoTPayloadBlobRecord& Record : BlobRecords)
		{
			if (const FWoTPayloadBlobRecord* Prior = Unique.Find(Record.Path))
			{
				if (Prior->Size != Record.Size || Prior->SHA256 != Record.SHA256)
				{
					return false;
				}
			}
			else
			{
				Unique.Add(Record.Path, Record);
			}
		}

		TArray<uint8> HashInput;
		HashInput.Reserve(CanonicalCore.Len() + Unique.Num() * 160 + 64);
		const FString HashDomain = CacheVersion == 2
			? TEXT("WOT-PAYLOAD-CACHE-HASH-V2")
			: TEXT("WOT-PAYLOAD-CACHE-HASH-V1");
		if (!WoTPayload_AppendASCII(HashDomain, HashInput))
		{
			return false;
		}
		HashInput.Add(0);
		WoTPayload_AppendUInt64LE(static_cast<uint64>(CanonicalCore.Len()), HashInput);
		if (!WoTPayload_AppendASCII(CanonicalCore, HashInput))
		{
			return false;
		}

		TArray<FString> Paths;
		Unique.GetKeys(Paths);
		Paths.Sort();
		for (const FString& Path : Paths)
		{
			const FWoTPayloadBlobRecord& Record = Unique.FindChecked(Path);
			TArray<uint8> EncodedRecord;
			EncodedRecord.Reserve(160);
			if (!WoTPayload_AppendASCII(Path, EncodedRecord)) return false;
			EncodedRecord.Add(0);
			if (!WoTPayload_AppendASCII(LexToString(Record.Size), EncodedRecord)) return false;
			EncodedRecord.Add(0);
			if (!WoTPayload_AppendASCII(Record.SHA256, EncodedRecord)) return false;
			EncodedRecord.Add(TEXT('\n'));
			WoTPayload_AppendUInt64LE(static_cast<uint64>(EncodedRecord.Num()), HashInput);
			if (EncodedRecord.Num() > MAX_int32 - HashInput.Num()) return false;
			HashInput.Append(EncodedRecord);
		}
		if (static_cast<uint64>(HashInput.Num()) > MAX_uint32)
		{
			return false;
		}
		OutRoot = WoTPayload_SHA256(HashInput.GetData(),
			static_cast<uint32>(HashInput.Num()));
		return !OutRoot.IsEmpty();
	}

	bool WoTPayload_ReadU16(
		TConstArrayView<uint8> Bytes, uint64& Offset, uint16& Out)
	{
		uint64 End = 0;
		if (!WoTPayload_CheckedAdd(Offset, 2, End)
			|| End > static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		Out = static_cast<uint16>(Bytes[static_cast<int32>(Offset)])
			| (static_cast<uint16>(Bytes[static_cast<int32>(Offset + 1)]) << 8);
		Offset = End;
		return true;
	}

	bool WoTPayload_ReadU32(
		TConstArrayView<uint8> Bytes, uint64& Offset, uint32& Out)
	{
		uint64 End = 0;
		if (!WoTPayload_CheckedAdd(Offset, 4, End)
			|| End > static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		Out = static_cast<uint32>(Bytes[static_cast<int32>(Offset)])
			| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 1)]) << 8)
			| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 2)]) << 16)
			| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 3)]) << 24);
		Offset = End;
		return true;
	}

	bool WoTPayload_ExpandMip(const FWoTTexturePayloadData& Payload,
		int32 MipIndex, TArray<uint8>& OutBGRA, FString& OutError)
	{
		if (!Payload.Mips.IsValidIndex(MipIndex))
		{
			OutError = TEXT("source mip index is out of range");
			return false;
		}
		const FWoTTexturePayloadMip& Mip = Payload.Mips[MipIndex];
		const TConstArrayView<uint8> PayloadBytes = Payload.PayloadBytes();
		uint64 DataEnd = 0;
		if (Payload.Mips.IsEmpty()
			|| !WoTPayload_ValidateTextureMipLayout(
				Payload.Mips[0].Width, Payload.Mips[0].Height,
				static_cast<uint32>(MipIndex), Mip.Width, Mip.Height,
				Mip.StoredByteLength, Mip.ExpandedByteLength)
			|| !WoTPayload_CheckedAdd(Mip.DataOffset, Mip.StoredByteLength, DataEnd)
			|| DataEnd > static_cast<uint64>(PayloadBytes.Num())
			|| Mip.ExpandedByteLength > static_cast<uint64>(MAX_int32))
		{
			OutError = TEXT("source mip range/expanded size is invalid");
			return false;
		}

		OutBGRA.SetNumUninitialized(static_cast<int32>(Mip.ExpandedByteLength));
		bool bHasTransparent = false;
		bool bHasOpaque = false;
		for (uint64 PixelIndex = 0; PixelIndex < Mip.StoredByteLength; ++PixelIndex)
		{
			const uint8 PaletteIndex = PayloadBytes[
				static_cast<int32>(Mip.DataOffset + PixelIndex)];
			const FColor& Color = Payload.Palette[PaletteIndex];
			const bool bTransparent = Payload.bMasked && PaletteIndex == 0;
			const int32 OutputOffset = static_cast<int32>(PixelIndex * 4);
			OutBGRA[OutputOffset + 0] = Color.B;
			OutBGRA[OutputOffset + 1] = Color.G;
			OutBGRA[OutputOffset + 2] = Color.R;
			OutBGRA[OutputOffset + 3] = bTransparent ? 0 : 255;
			bHasTransparent |= bTransparent;
			bHasOpaque |= !bTransparent;
		}

		if (!Payload.bMasked || !bHasTransparent || !bHasOpaque)
		{
			return true;
		}

		const int32 Width = static_cast<int32>(Mip.Width);
		const int32 Height = static_cast<int32>(Mip.Height);
		const int32 PixelCount = static_cast<int32>(Mip.StoredByteLength);
		uint64 RGBWorkingBytes = 0;
		if (!WoTPayload_CheckedMultiply(
			Mip.StoredByteLength, sizeof(FVector3f), RGBWorkingBytes)
			|| RGBWorkingBytes > WoTPayload_MaxTotalExpandedBytes)
		{
			OutError = TEXT("masked-RGB dilation working set exceeds the compiled cap");
			return false;
		}
		TArray<FVector3f> RGB;
		RGB.SetNumUninitialized(PixelCount);
		TBitArray<> Filled(false, PixelCount);
		TBitArray<> OriginallyOpaque(false, PixelCount);
		for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
		{
			const int32 OutputOffset = PixelIndex * 4;
			RGB[PixelIndex] = FVector3f(
				OutBGRA[OutputOffset + 2], OutBGRA[OutputOffset + 1],
				OutBGRA[OutputOffset + 0]);
			const bool bOpaque = OutBGRA[OutputOffset + 3] != 0;
			Filled[PixelIndex] = bOpaque;
			OriginallyOpaque[PixelIndex] = bOpaque;
		}

		for (int32 Iteration = 0; Iteration < 16; ++Iteration)
		{
			if (Filled.CountSetBits() == PixelCount)
			{
				break;
			}
			TBitArray<> Grow(false, PixelCount);
			for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				if (Filled[PixelIndex]) continue;
				const int32 X = PixelIndex % Width;
				const int32 Y = PixelIndex / Width;
				const int32 Neighbors[4] = {
					((Y + 1) % Height) * Width + X,
					((Y + Height - 1) % Height) * Width + X,
					Y * Width + ((X + 1) % Width),
					Y * Width + ((X + Width - 1) % Width),
				};
				Grow[PixelIndex] = Filled[Neighbors[0]] || Filled[Neighbors[1]]
					|| Filled[Neighbors[2]] || Filled[Neighbors[3]];
			}
			if (Grow.CountSetBits() == 0)
			{
				break;
			}
			for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				if (!Grow[PixelIndex]) continue;
				const int32 X = PixelIndex % Width;
				const int32 Y = PixelIndex / Width;
				const int32 Neighbors[4] = {
					((Y + 1) % Height) * Width + X,
					((Y + Height - 1) % Height) * Width + X,
					Y * Width + ((X + 1) % Width),
					Y * Width + ((X + Width - 1) % Width),
				};
				FVector3f Sum(0.0f, 0.0f, 0.0f);
				float Count = 0.0f;
				for (int32 Neighbor : Neighbors)
				{
					if (Filled[Neighbor])
					{
						Sum += RGB[Neighbor];
						Count += 1.0f;
					}
				}
				RGB[PixelIndex] = Sum / Count;
			}
			for (TConstSetBitIterator<> It(Grow); It; ++It)
			{
				Filled[It.GetIndex()] = true;
			}
		}

		if (Filled.CountSetBits() != PixelCount)
		{
			FVector3f Mean(0.0f, 0.0f, 0.0f);
			float Count = 0.0f;
			for (TConstSetBitIterator<> It(OriginallyOpaque); It; ++It)
			{
				Mean += RGB[It.GetIndex()];
				Count += 1.0f;
			}
			Mean /= Count;
			for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				if (!Filled[PixelIndex]) RGB[PixelIndex] = Mean;
			}
		}

		for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
		{
			const int32 OutputOffset = PixelIndex * 4;
			const FVector3f Color = RGB[PixelIndex];
			OutBGRA[OutputOffset + 2] = static_cast<uint8>(
				FMath::Clamp(FMath::FloorToInt(Color.X + 0.5f), 0, 255));
			OutBGRA[OutputOffset + 1] = static_cast<uint8>(
				FMath::Clamp(FMath::FloorToInt(Color.Y + 0.5f), 0, 255));
			OutBGRA[OutputOffset + 0] = static_cast<uint8>(
				FMath::Clamp(FMath::FloorToInt(Color.Z + 0.5f), 0, 255));
		}
		return true;
	}

	// Reproduce TMGS_SimpleAverage's cook-time path for an uncompressed BGRA8
	// source. UE linearizes mip 0 to RGBA32F, recursively averages each 2x2
	// parent footprint, and only quantizes the individual output mips. Keeping
	// the float parent here is important: deriving mip N from the already
	// quantized bytes of mip N-1 can differ by one at deeper levels. Normal maps
	// receive the same post-chain renormalization as the current canonical cook.
	FLinearColor WoTPayload_NormalizeTextureNormal(const FLinearColor& Color)
	{
		const FVector NormalIfZero(0.0, 0.0, 1.0);
		FVector Normal(
			Color.R * 2.0f - 1.0f,
			Color.G * 2.0f - 1.0f,
			Color.B * 2.0f - 1.0f);
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, NormalIfZero);
		return FLinearColor(
			static_cast<float>(Normal.X * 0.5 + 0.5),
			static_cast<float>(Normal.Y * 0.5 + 0.5),
			static_cast<float>(Normal.Z * 0.5 + 0.5),
			Color.A);
	}

	void WoTPayload_QuantizeTextureColor(const FLinearColor& Linear,
		bool bSRGB, uint8* OutBGRA)
	{
		const FColor Quantized = bSRGB
			? Linear.ToFColorSRGB() : Linear.QuantizeRound();
		OutBGRA[0] = Quantized.B;
		OutBGRA[1] = Quantized.G;
		OutBGRA[2] = Quantized.R;
		OutBGRA[3] = Quantized.A;
	}

	bool WoTPayload_GenerateCanonicalMipChain(TArray<uint8>&& BaseBGRA,
		uint32 BaseWidth, uint32 BaseHeight, int32 DestinationMipCount,
		bool bSRGB, bool bNormalizeNormals,
		TArray<TArray<uint8>>& OutMips, FString& OutError)
	{
		OutMips.Reset();
		if (DestinationMipCount <= 0
			|| DestinationMipCount > WoTPayload_FullTextureMipCount(
				BaseWidth, BaseHeight))
		{
			OutError = TEXT("destination mip count exceeds the complete base-derived chain");
			return false;
		}

		uint64 ExpandedTotal = 0;
		for (int32 MipIndex = 0; MipIndex < DestinationMipCount; ++MipIndex)
		{
			const uint64 Width = FMath::Max<uint64>(1, BaseWidth >> MipIndex);
			const uint64 Height = FMath::Max<uint64>(1, BaseHeight >> MipIndex);
			uint64 Pixels = 0;
			uint64 Bytes = 0;
			if (!WoTPayload_CheckedMultiply(Width, Height, Pixels)
				|| !WoTPayload_CheckedMultiply(Pixels, 4, Bytes)
				|| !WoTPayload_CheckedAdd(ExpandedTotal, Bytes, ExpandedTotal)
				|| ExpandedTotal > WoTPayload_MaxTotalExpandedBytes)
			{
				OutError = TEXT("generated destination mip chain exceeds the compiled cap");
				return false;
			}
		}

		uint64 BasePixels = 0;
		uint64 BaseWorkingBytes = 0;
		uint64 NextWorkingBytes = 0;
		uint64 PeakBytes = 0;
		const uint64 NextWidth = FMath::Max<uint64>(1, BaseWidth >> 1);
		const uint64 NextHeight = FMath::Max<uint64>(1, BaseHeight >> 1);
		if (!WoTPayload_CheckedMultiply(BaseWidth, BaseHeight, BasePixels)
			|| !WoTPayload_CheckedMultiply(
				BasePixels, sizeof(FLinearColor), BaseWorkingBytes)
			|| !WoTPayload_CheckedMultiply(
				NextWidth * NextHeight, sizeof(FLinearColor), NextWorkingBytes)
			|| !WoTPayload_CheckedAdd(ExpandedTotal, BaseWorkingBytes, PeakBytes)
			|| !WoTPayload_CheckedAdd(PeakBytes, NextWorkingBytes, PeakBytes)
			|| PeakBytes > WoTPayload_MaxTotalExpandedBytes
			|| BaseBGRA.Num() != static_cast<int32>(BasePixels * 4))
		{
			OutError = TEXT("generated destination mip working set is invalid or exceeds the compiled cap");
			return false;
		}

		OutMips.Reserve(DestinationMipCount);
		OutMips.Add(MoveTemp(BaseBGRA));
		if (DestinationMipCount == 1 && !bNormalizeNormals)
		{
			return true;
		}

		TArray<FLinearColor> Parent;
		Parent.SetNumUninitialized(static_cast<int32>(BasePixels));
		const TArray<uint8>& BaseBytes = OutMips[0];
		for (int32 PixelIndex = 0; PixelIndex < Parent.Num(); ++PixelIndex)
		{
			const int32 Offset = PixelIndex * 4;
			const FColor SourceColor(
				BaseBytes[Offset + 2], BaseBytes[Offset + 1],
				BaseBytes[Offset + 0], BaseBytes[Offset + 3]);
			Parent[PixelIndex] = bSRGB
				? FLinearColor::FromSRGBColor(SourceColor)
				: SourceColor.ReinterpretAsLinear();
		}
		if (bNormalizeNormals)
		{
			TArray<uint8>& BaseOutput = OutMips[0];
			for (int32 PixelIndex = 0; PixelIndex < Parent.Num(); ++PixelIndex)
			{
				const FLinearColor Normalized =
					WoTPayload_NormalizeTextureNormal(Parent[PixelIndex]);
				WoTPayload_QuantizeTextureColor(
					Normalized, bSRGB, &BaseOutput[PixelIndex * 4]);
			}
		}
		if (DestinationMipCount == 1)
		{
			return true;
		}

		uint32 ParentWidth = BaseWidth;
		uint32 ParentHeight = BaseHeight;
		for (int32 MipIndex = 1; MipIndex < DestinationMipCount; ++MipIndex)
		{
			const uint32 Width = FMath::Max<uint32>(1, ParentWidth >> 1);
			const uint32 Height = FMath::Max<uint32>(1, ParentHeight >> 1);
			const int32 PixelCount = static_cast<int32>(Width * Height);
			TArray<FLinearColor> Child;
			Child.SetNumUninitialized(PixelCount);
			TArray<uint8>& ChildBGRA = OutMips.AddDefaulted_GetRef();
			ChildBGRA.SetNumUninitialized(PixelCount * 4);

			for (uint32 Y = 0; Y < Height; ++Y)
			{
				const uint32 SourceY0 = Y * 2;
				const uint32 SourceY1 = FMath::Min(SourceY0 + 1, ParentHeight - 1);
				for (uint32 X = 0; X < Width; ++X)
				{
					const uint32 SourceX0 = X * 2;
					const uint32 SourceX1 = FMath::Min(SourceX0 + 1, ParentWidth - 1);
					const FLinearColor& A = Parent[SourceY0 * ParentWidth + SourceX0];
					const FLinearColor& B = Parent[SourceY0 * ParentWidth + SourceX1];
					const FLinearColor& C = Parent[SourceY1 * ParentWidth + SourceX0];
					const FLinearColor& D = Parent[SourceY1 * ParentWidth + SourceX1];
					const int32 PixelIndex = static_cast<int32>(Y * Width + X);
					Child[PixelIndex] = (((A + B) + C) + D) * 0.25f;

					const int32 Offset = PixelIndex * 4;
					const FLinearColor OutputColor = bNormalizeNormals
						? WoTPayload_NormalizeTextureNormal(Child[PixelIndex])
						: Child[PixelIndex];
					WoTPayload_QuantizeTextureColor(
						OutputColor, bSRGB, &ChildBGRA[Offset]);
				}
			}

			Parent = MoveTemp(Child);
			ParentWidth = Width;
			ParentHeight = Height;
		}
		return true;
	}

	int32 WoTPayload_SignExtend(uint32 Value, uint32 Bits)
	{
		const uint32 Sign = 1u << (Bits - 1u);
		const uint32 Mask = (1u << Bits) - 1u;
		Value &= Mask;
		return static_cast<int32>((Value ^ Sign) - Sign);
	}

	FVector3d WoTPayload_TransformMeshVertex(uint32 Packed,
		const UWoTMeshPayloadLayout& Layout)
	{
		uint32 PackedX = Packed & 0x7ffu;
		if (Layout.bUnmirror)
		{
			// Match ue1pkg._negate_packed_x exactly, including the signed
			// 11-bit minimum (-1024), whose negation wraps back to -1024.
			const int32 SignedX = WoTPayload_SignExtend(PackedX, 11);
			PackedX = static_cast<uint32>(-SignedX) & 0x7ffu;
		}
		const double X = static_cast<double>(
			WoTPayload_SignExtend(PackedX, 11));
		const double Y = static_cast<double>(
			WoTPayload_SignExtend(Packed >> 11, 11));
		const double Z = static_cast<double>(
			WoTPayload_SignExtend(Packed >> 22, 10));
		FVector3d Point(
			(X - Layout.Origin.X) * Layout.Scale.X,
			(Y - Layout.Origin.Y) * Layout.Scale.Y,
			(Z - Layout.Origin.Z) * Layout.Scale.Z);

		auto SinCos = [](double Units, double& OutSin, double& OutCos)
		{
			FMath::SinCos(&OutSin, &OutCos,
				Units * 2.0 * UE_DOUBLE_PI / 256.0);
		};
		if (Layout.RotationUnits.Z != 0.0)
		{
			double Sin = 0.0, Cos = 1.0;
			SinCos(Layout.RotationUnits.Z, Sin, Cos);
			const double OldY = Point.Y;
			const double OldZ = Point.Z;
			Point.Y = OldY * Cos + OldZ * Sin;
			Point.Z = -OldY * Sin + OldZ * Cos;
		}
		if (Layout.RotationUnits.X != 0.0)
		{
			double Sin = 0.0, Cos = 1.0;
			SinCos(Layout.RotationUnits.X, Sin, Cos);
			const double OldX = Point.X;
			const double OldZ = Point.Z;
			Point.X = OldX * Cos - OldZ * Sin;
			Point.Z = OldX * Sin + OldZ * Cos;
		}
		if (Layout.RotationUnits.Y != 0.0)
		{
			double Sin = 0.0, Cos = 1.0;
			SinCos(Layout.RotationUnits.Y, Sin, Cos);
			const double OldX = Point.X;
			const double OldY = Point.Y;
			Point.X = OldX * Cos - OldY * Sin;
			Point.Y = OldX * Sin + OldY * Cos;
		}
		return Point;
	}

	bool WoTPayload_ComputeSmoothNormals(const TArray<FVector3d>& Positions,
		const UWoTMeshPayloadLayout& Layout, TArray<FVector3f>& OutNormals,
		FString& OutError)
	{
		TArray<FVector3d> Accumulated;
		Accumulated.SetNumZeroed(Positions.Num());
		for (int32 TriangleOffset = 0;
			TriangleOffset < Layout.SmoothTriangles.Num(); TriangleOffset += 3)
		{
			const int32 IA = Layout.SmoothTriangles[TriangleOffset];
			const int32 IB = Layout.SmoothTriangles[TriangleOffset + 1];
			const int32 IC = Layout.SmoothTriangles[TriangleOffset + 2];
			if (!Positions.IsValidIndex(IA) || !Positions.IsValidIndex(IB)
				|| !Positions.IsValidIndex(IC))
			{
				OutError = TEXT("smooth-normal triangle is outside the decoded frame");
				return false;
			}
			FVector3d Face = FVector3d::CrossProduct(
				Positions[IB] - Positions[IA], Positions[IC] - Positions[IA]);
			Face /= FMath::Sqrt(Face.SquaredLength() + 0.001);
			Accumulated[IA] += Face;
			Accumulated[IB] += Face;
			Accumulated[IC] += Face;
		}
		OutNormals.SetNumUninitialized(Positions.Num());
		for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
		{
			const double Length = Accumulated[Vertex].Length();
			OutNormals[Vertex] = Length > 0.0
				? FVector3f(Accumulated[Vertex] / Length) : FVector3f::ZeroVector;
		}
		return true;
	}
}

void UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
	bool bHasOverride, bool bEnabled)
{
#if WITH_AUTOMATION_TESTS
	WoTPayload_TestInjectionOverride = bHasOverride
		? TOptional<bool>(bEnabled) : TOptional<bool>();
#else
	(void)bHasOverride;
	(void)bEnabled;
#endif
}

#if WITH_AUTOMATION_TESTS
void UWoTPayloadSubsystem::FailNextActivationForTesting(bool bFail)
{
	WoTPayload_FailNextActivationForTesting = bFail;
}
#endif

bool UWoTPayloadSubsystem::TryBindSoundWaveDuringPostLoad(
	UWoTPayloadSoundWave* SoundWave)
{
	if (!IsInGameThread())
	{
		return false;
	}
	if (UWoTPayloadSubsystem* ActiveSubsystem = WoTPayload_ActiveSubsystem.Get())
	{
		ActiveSubsystem->TryBindSoundWave(SoundWave);
		return IsValid(SoundWave) && SoundWave->IsPayloadBound();
	}
	return false;
}

TSharedPtr<const FWoTSoundPayloadData>
UWoTPayloadSubsystem::ResolveSoundPayloadForGenerator(
	const UWoTPayloadSoundWave* SoundWave)
{
	if (!SoundWave)
	{
		return nullptr;
	}
	FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
	if (const TSharedPtr<const FWoTSoundPayloadData>* Payload =
		WoTPayload_ActiveSoundBindings.Find(FObjectKey(SoundWave)))
	{
		return *Payload;
	}
	return nullptr;
}

void UWoTPayloadSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Before verified activation the newest game instance is the developer
	// binding context. Once a generation is published, transient host/client
	// game instances must not replace its lease owner: hidden guarded worlds use
	// that process-wide owner for class defaults and runtime assets.
	const bool bHasAuthenticatedOwner = IsWoTPayloadBootstrapActive()
		&& WoTPayload_ActiveSubsystem.IsValid();
	if (!bHasAuthenticatedOwner) WoTPayload_ActiveSubsystem = this;
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		if (!bHasAuthenticatedOwner || WoTPayload_ActiveSoundBindingOwner == nullptr)
		{
			WoTPayload_ActiveSoundBindingOwner = this;
			WoTPayload_ActiveSoundBindings.Empty();
		}
	}
	bInjectionEnabled = false;
	bManifestReady = false;
	bPayloadActivated = false;
	CacheDirectory.Empty();

#if !WOT_GUARDED_BOOTSTRAP
	bInjectionEnabled = WoTPayload_InjectionCVar.GetValueOnGameThread() != 0
		|| FParse::Param(FCommandLine::Get(), TEXT("WoTPayloadInjection"));
#if WITH_AUTOMATION_TESTS
	if (WoTPayload_TestInjectionOverride.IsSet())
	{
		bInjectionEnabled = WoTPayload_TestInjectionOverride.GetValue();
		// The explicit OFF fixture models the guarded pre-activation state in
		// editor automation.  It must be fully inert, not merely prevented from
		// reading the developer oracle after installing its callbacks.
		if (!bInjectionEnabled)
		{
			return;
		}
	}
#endif
	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddUObject(
		this, &UWoTPayloadSubsystem::HandleWorldTickStart);
	// Register before developer-manifest preflight so proof carriers still fail
	// closed when the oracle fixture is absent or malformed.
	PackageLoadCompletedHandle =
		FCoreUObjectDelegates::OnPackageLoadCompleted.AddUObject(
			this, &UWoTPayloadSubsystem::HandlePackageLoadCompleted);
	if (!bInjectionEnabled)
	{
		BindAlreadyLoadedPayloads();
		return;
	}

	CacheDirectory = DiscoverCacheDirectory();
	if (!LoadManifest())
	{
		UE_LOG(LogWoTPayload, Display,
			TEXT("Developer payload oracle preflight failed at %s; awaiting authenticated activation"),
			*CacheDirectory);
		BindAlreadyLoadedPayloads();
		return;
	}
	PostGarbageCollectHandle =
		FCoreUObjectDelegates::GetPostGarbageCollect().AddUObject(
			this, &UWoTPayloadSubsystem::HandlePostGarbageCollect);
	BindAlreadyLoadedPayloads();
#endif
}

FWoTBootstrapStatus UWoTPayloadSubsystem::ActivateVerifiedCache(
	const FWoTCacheGenerationLease& Snapshot)
{
	using FResult = FWoTBootstrapStatus;
	auto Error = [](const TCHAR* Message)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation, Message));
	};

	if (bPayloadActivated || !Snapshot.IsValid())
	{
		return Error(TEXT("payload activation received a stale or invalid snapshot"));
	}
#if WITH_AUTOMATION_TESTS
	if (WoTPayload_FailNextActivationForTesting)
	{
		WoTPayload_FailNextActivationForTesting = false;
		return Error(TEXT("payload activation failed at the automation boundary"));
	}
#endif
	ActiveCacheLease = MakeUnique<FWoTCacheGenerationLease>(Snapshot);
	if (!ActiveCacheLease.IsValid() || !ActiveCacheLease->IsValid())
	{
		ActiveCacheLease.Reset();
		return Error(TEXT("the verified cache snapshot could not be retained"));
	}
	FWoTBootstrapStatus RuntimeRegistry =
		FWoTRuntimeRegistry::Get().BindVerifiedGeneration(*ActiveCacheLease);
	if (RuntimeRegistry.IsError())
	{
		ActiveCacheLease.Reset();
		return FResult::Error(MoveTemp(RuntimeRegistry).GetError());
	}
	FWoTBootstrapStatus Published =
		FWoTPayloadActivationPublication::
			PublishWoTPayloadBootstrapActivation(
				*ActiveCacheLease);
	if (Published.IsError())
	{
		FWoTRuntimeRegistry::Get().Reset();
		ActiveCacheLease.Reset();
		return FResult::Error(MoveTemp(Published).GetError());
	}
	TConstArrayView<FWoTRetailFamily> RetailFamilies = WoTRetailFamilies();
#if WITH_AUTOMATION_TESTS
	// Retail families are built from the PRODUCTION contract's class defaults.
	// Automation's synthetic fixture generations carry none, so they activate
	// without families unless a spec injects its own. Builds without
	// automation (packaged Test/Shipping) never skip: a non-production
	// generation fails closed on its missing class defaults.
	{
		const FWoTCacheGenerationBuildIdentity& BuildIdentity =
			ActiveCacheLease->GetBuildIdentity();
		const bool bProductionGeneration = BuildIdentity.bHasBuildIdentity
			&& IsCurrentProductionCacheBuildIdentity(BuildIdentity.ContractSha256,
				BuildIdentity.ProductionDescriptorSha256);
		if (!bProductionGeneration && !WoTRetailFamiliesOverriddenForTesting())
		{
			RetailFamilies = TConstArrayView<FWoTRetailFamily>();
		}
	}
#endif
	FString RetailError;
	// Packaged builds compile logging out and the bootstrap surfaces only
	// PAYLOAD-CAPABILITY-ABSENT, so keep the reason durably in the heartbeat.
	const auto RecordRetailFamiliesFailure = [](const FString& Reason)
	{
		WoTRecordBootstrapHeartbeat(TEXT("retail-families-failed"),
			EWoTBootstrapPhase::StartingGame, 0, {}, {}, Reason.Left(1024));
	};
	auto FindTexture = [this](FStringView Identity, FWoTRetailTextureFacts& Out)
	{
		return FindRetailTextureFacts(Identity, Out);
	};
	auto FindMeshSequences = [](FStringView Identity,
		TArray<FWoTVertexMeshSequence>& Out)
	{
		Out.Reset();
		FWoTRetailObjectPath Path;
		if (!WoTParseRetailObjectPath(Identity, Path))
		{
			return false;
		}
		auto Resolve = [&Path](EWoTNeutralResourceKind Kind)
		{
			const FString LogicalBase = WoTRetailResolveLogicalBase(
				Kind, Path, [Kind](const FString& LogicalId)
				{
					return FWoTRuntimeRegistry::Get().FindDescriptor(
						{LogicalId, Kind}).IsSet();
				});
			return WoTRetailLogicalIdPrefix(Kind) + LogicalBase;
		};
		const FString MeshId = Resolve(EWoTNeutralResourceKind::VertexMesh);
		const FString AnimationId = Resolve(EWoTNeutralResourceKind::Animation);
		if (!FWoTRuntimeRegistry::Get().FindDescriptor(
			{MeshId, EWoTNeutralResourceKind::VertexMesh}).IsSet()
			|| !FWoTRuntimeRegistry::Get().FindDescriptor(
				{AnimationId, EWoTNeutralResourceKind::Animation}).IsSet())
		{
			return false;
		}
		const FWoTRuntimeVertexMeshDataPtr Mesh =
			FWoTRuntimeVertexMesh::Load(MeshId, AnimationId);
		if (!Mesh.IsValid())
		{
			return false;
		}
		Out.Reset(Mesh->GetSequences().Num());
		for (const FWoTAnimSequence& Sequence : Mesh->GetSequences())
		{
			FWoTVertexMeshSequence& Target = Out.AddDefaulted_GetRef();
			Target.Name = Sequence.Name.ToString();
			Target.FirstFrame = static_cast<uint32>(Sequence.StartFrame);
			Target.NumFrames = static_cast<uint32>(Sequence.NumFrames);
			Target.Rate = Sequence.Rate;
			Target.Group = Sequence.Group.ToString();
			for (const FWoTAnimNotify& Notify : Sequence.Notifies)
			{
				FWoTVertexMeshNotify& TargetNotify =
					Target.Notifies.AddDefaulted_GetRef();
				TargetNotify.Time = Notify.Fraction;
				TargetNotify.Function = Notify.Function.ToString();
			}
		}
		return true;
	};
	if (RetailFamilies.IsEmpty())
	{
		FWoTRuntimeClassDefaults EmptyDefaults;
		const FWoTRetailBuildContext Context{
			EmptyDefaults, FindTexture, FindMeshSequences};
		if (!WoTActivateRetailFamilies(
			Context, RetailFamilies, WoTRetailFamilyPins(), RetailError))
		{
			RecordRetailFamiliesFailure(RetailError);
			FWoTPayloadActivationPublication::
				RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
			FWoTRuntimeRegistry::Get().Reset();
			LoadedTexturePayloads.Empty();
			FWoTRuntimeVertexMesh::ResetRuntimeCache();
			ActiveCacheLease.Reset();
			return Error(*RetailError);
		}
	}
	else
	{
		TUniquePtr<FWoTRuntimeClassDefaults> Candidate =
			MakeUnique<FWoTRuntimeClassDefaults>();
		const FWoTRetailBuildContext Context{
			*Candidate, FindTexture, FindMeshSequences};
		if (!Candidate->LoadActiveGeneration(RetailError)
			|| !WoTActivateRetailFamilies(
				Context, RetailFamilies, WoTRetailFamilyPins(), RetailError))
		{
			RecordRetailFamiliesFailure(RetailError);
			FWoTPayloadActivationPublication::
				RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
			FWoTRuntimeRegistry::Get().Reset();
			LoadedTexturePayloads.Empty();
			FWoTRuntimeVertexMesh::ResetRuntimeCache();
			ActiveCacheLease.Reset();
			return Error(*RetailError);
		}
		ClassDefaultsOwner = MoveTemp(Candidate);
	}

	bInjectionEnabled = true;
	bManifestReady = true;
	bPayloadActivated = true;
	WoTPayload_ActiveSubsystem = this;
	UWoTRuntimeAssets::RefreshProviderBinding();
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		WoTPayload_ActiveSoundBindingOwner = this;
		WoTPayload_ActiveSoundBindings.Empty();
	}
	if (!IsWoTPayloadBootstrapActive())
	{
		FWoTRuntimeRegistry::Get().Reset();
		ActiveCacheLease.Reset();
		bInjectionEnabled = false;
		bManifestReady = false;
		bPayloadActivated = false;
		return Error(TEXT("payload activation publication was not observable"));
	}
	#if WOT_GUARDED_BOOTSTRAP
	UWoTPreferencesSubsystem* PreferencesSubsystem =
		GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>();
	if (PreferencesSubsystem == nullptr)
	{
		FWoTPayloadActivationPublication::
			RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
		FWoTRuntimeRegistry::Get().Reset();
		ActiveCacheLease.Reset();
		bInjectionEnabled = false;
		bManifestReady = false;
		bPayloadActivated = false;
		return Error(TEXT("verified cache activation has no typed preference owner"));
	}
	FWoTBootstrapStatus FullPreferences =
		PreferencesSubsystem->ApplyFullPreferencesAfterCacheActivation();
	if (FullPreferences.IsError())
	{
		FWoTPayloadActivationPublication::
			RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
		FWoTRuntimeRegistry::Get().Reset();
		ActiveCacheLease.Reset();
		bInjectionEnabled = false;
		bManifestReady = false;
		bPayloadActivated = false;
		return FResult::Error(MoveTemp(FullPreferences).GetError());
	}
	#endif
	if (ActiveCacheLease->HasFrontEndDescriptorAttestation())
	{
		FrontEndFontOwner = MakeUnique<FWoTFrontEndFontOwner>();
		FWoTBootstrapStatus Fonts = FrontEndFontOwner->Initialize(
			*this, *ActiveCacheLease);
		if (Fonts.IsError())
		{
			FrontEndFontOwner.Reset();
			FWoTPayloadActivationPublication::
				RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
			FWoTRuntimeRegistry::Get().Reset();
			ActiveCacheLease.Reset();
			bInjectionEnabled = false;
			bManifestReady = false;
			bPayloadActivated = false;
			return FResult::Error(MoveTemp(Fonts).GetError());
		}
	}

	if (!WorldTickStartHandle.IsValid())
	{
		WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddUObject(
			this, &UWoTPayloadSubsystem::HandleWorldTickStart);
	}
	if (!PackageLoadCompletedHandle.IsValid())
	{
		PackageLoadCompletedHandle =
			FCoreUObjectDelegates::OnPackageLoadCompleted.AddUObject(
				this, &UWoTPayloadSubsystem::HandlePackageLoadCompleted);
	}
	if (!PostGarbageCollectHandle.IsValid())
	{
		PostGarbageCollectHandle =
			FCoreUObjectDelegates::GetPostGarbageCollect().AddUObject(
				this, &UWoTPayloadSubsystem::HandlePostGarbageCollect);
	}
	bRuntimeIntegrityRecoveryHandled = false;
	BindAlreadyLoadedPayloads();
	return FResult::Success();
}

void UWoTPayloadSubsystem::HandleRuntimeIntegrityFailure(
	const FWoTBootstrapError& Failure)
{
	if (bRuntimeIntegrityRecoveryHandled || bShutdownForUninstall)
	{
		return;
	}
	bRuntimeIntegrityRecoveryHandled = true;

	FString DisplayName = Failure.Path;
	FString MapIdentity;
	if (DisplayName.RemoveFromStart(TEXT("cache/world/"),
		ESearchCase::CaseSensitive))
	{
		MapIdentity = DisplayName;
	}
	else if (DisplayName.RemoveFromStart(TEXT("cache/bsp/"),
		ESearchCase::CaseSensitive))
	{
		DisplayName.Split(TEXT("/"), &MapIdentity, nullptr,
			ESearchCase::CaseSensitive, ESearchDir::FromStart);
	}
	if (!MapIdentity.IsEmpty())
	{
		DisplayName = FWoTFrontEndMapCatalog::CachedRetailTitle(MapIdentity);
	}
	const FString Message = FString::Printf(
		TEXT("Some of WoT Reborn's game data is damaged (%s). It will be rebuilt the next time you start the game."),
		*DisplayName);

	UGameInstance* Instance = GetGameInstance();
	UWoTSessionSubsystem* Sessions = Instance
		? Instance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	bool bRecovered = false;
	if (Sessions != nullptr && ActiveCacheLease.IsValid()
		&& ActiveCacheLease->CanRecoverFrontEndAfterRuntimeIntegrityFailure())
	{
		bRecovered = Sessions->RecoverFromRuntimeIntegrityFailure(Message);
	}
	else if (Sessions != nullptr)
	{
		Sessions->ReportRuntimeIntegrityFailure(Message);
	}
	if (UWoTBootstrapSubsystem* Bootstrap = Instance
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->HandleRuntimeCacheIntegrityFailure(Message, bRecovered);
	}
}

void UWoTPayloadSubsystem::Deinitialize()
{
	ShutdownForUninstall();
	Super::Deinitialize();
}

void UWoTPayloadSubsystem::ShutdownForUninstall()
{
	if (bShutdownForUninstall)
	{
		return;
	}
	bShutdownForUninstall = true;
	bRuntimeIntegrityRecoveryHandled = true;
	// Runtime gameplay waves/textures borrow this exact authenticated generation.
	// Tear them down before revocation makes their lease-backed payloads unavailable.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UWoTRuntimeAssets* RuntimeAssets =
			GameInstance->GetSubsystem<UWoTRuntimeAssets>())
		{
			RuntimeAssets->ShutdownForPayloadDeactivation();
		}
	}
	// The decoded WOTDEF3 owner retains generation-backed verified objects.
	ClassDefaultsOwner.Reset();
	// Revoke the shared snapshot before dismantling any dependent projection.
	// This is a synchronous barrier: scene and presentation subscribers finish
	// collapsing widgets, stopping audio, and releasing their exact bindings
	// while this subsystem, its font owner, and its payload tables are intact.
	if (ActiveCacheLease.IsValid())
	{
		ActiveCacheLease->Revoke();
	}
	if (WorldTickStartHandle.IsValid())
	{
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
		WorldTickStartHandle.Reset();
	}
	if (PackageLoadCompletedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnPackageLoadCompleted.Remove(
			PackageLoadCompletedHandle);
		PackageLoadCompletedHandle.Reset();
	}
	if (PostGarbageCollectHandle.IsValid())
	{
		FCoreUObjectDelegates::GetPostGarbageCollect().Remove(
			PostGarbageCollectHandle);
		PostGarbageCollectHandle.Reset();
	}
	PendingCompletedPackages.Empty();
	if (WoTPayload_ActiveSubsystem.Get() == this)
	{
		WoTPayload_ActiveSubsystem.Reset();
		UWoTRuntimeAssets::RefreshProviderBinding();
	}
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		if (WoTPayload_ActiveSoundBindingOwner == this)
		{
			WoTPayload_ActiveSoundBindingOwner = nullptr;
			WoTPayload_ActiveSoundBindings.Empty();
		}
	}
	for (const TWeakObjectPtr<UWoTVertexAnimComponent>& WeakComponent
		: BoundMeshComponents)
	{
		if (UWoTVertexAnimComponent* Component = WeakComponent.Get())
		{
			Component->ClearExternalMorphInjection();
		}
	}
	BoundMeshComponents.Empty();
	if (!PreparedTextureConsumers.IsEmpty())
	{
		FWoTMenuStyle::Get().UnbindTextureConsumers();
	}
	InjectedTextures.Empty();
	for (UTexture2D* Texture : PreparedTextureConsumers)
	{
		if (IsValid(Texture))
		{
			Texture->ReleaseResource();
			Texture->ClearFlags(RF_Public | RF_Standalone);
			Texture->MarkAsGarbage();
		}
	}
	PreparedTextureConsumers.Empty();
#if WITH_AUTOMATION_TESTS
	TextureConsumerInjectionFailureIndexForTests = INDEX_NONE;
#endif
	InjectedSounds.Empty();
	InjectedBSPMeshes.Empty();
	for (UBodySetup* PendingBody : PendingBSPBodySetups)
	{
		if (IsValid(PendingBody))
		{
			PendingBody->AbortPhysicsMeshAsyncCreation();
		}
	}
	PendingBSPBodySetups.Empty();
	ReadyBSPKeys.Empty();
	FailedRequiredBSPKeys.Empty();
	BSPPausedWorlds.Empty();
	bool bReleasedMeshResources = false;
	for (const TPair<FObjectKey, TSharedPtr<FWoTPreparedMeshPayload>>& Pair
		: PreparedMeshPayloads)
	{
		if (Pair.Value.IsValid() && Pair.Value->MorphSet.IsValid()
			&& Pair.Value->MorphSet->MorphBuffers.IsMorphResourcesInitialized())
		{
			BeginReleaseResource(&Pair.Value->MorphSet->MorphBuffers);
			bReleasedMeshResources = true;
		}
	}
	if (bReleasedMeshResources)
	{
		// Keep the shared set memory alive until every queued release has run.
		FlushRenderingCommands();
	}
	PreparedMeshPayloads.Empty();
	RejectedBlobKeys.Empty();
	LoadedTexturePayloads.Empty();
	LoadedSoundPayloads.Empty();
	LoadedMeshPayloads.Empty();
	LoadedBSPPayloads.Empty();
	DirectTextureKeys.Empty();
	DirectSoundKeys.Empty();
	TextureEntries.Empty();
	SoundEntries.Empty();
	MeshEntries.Empty();
	BSPEntries.Empty();
	RequiredBSPKeysByMap.Empty();
	WoTAnimData::ResetRuntimeCache();
	ShutdownFrontEndFontsForScene();
	FWoTFrontEndPresentationAuthority::ShutdownIntegrityRecoverySnapshot();
	if (ActiveCacheLease.IsValid())
	{
		FWoTPayloadActivationPublication::
			RevokeWoTPayloadBootstrapActivation(*ActiveCacheLease);
		// The verified registry is process-global but its lifetime belongs to
		// the subsystem which adopted the generation lease. Transient game
		// instances must not revoke another subsystem's authenticated census.
		FWoTRuntimeRegistry::Get().Reset();
	}
	ActiveCacheLease.Reset();
	bPayloadActivated = false;
	bManifestReady = false;
}

FWoTRuntimeClassDefaults* UWoTPayloadSubsystem::RuntimeClassDefaults(
	FString& OutError)
{
	if (!bPayloadActivated || !ActiveCacheLease.IsValid()
		|| !FWoTRuntimeRegistry::Get().SharesSnapshotWith(*ActiveCacheLease))
	{
		OutError = TEXT("runtime class defaults require the active payload generation");
		return nullptr;
	}
	if (!ClassDefaultsOwner.IsValid())
	{
		TUniquePtr<FWoTRuntimeClassDefaults> Candidate =
			MakeUnique<FWoTRuntimeClassDefaults>();
		if (!Candidate->LoadActiveGeneration(OutError)) return nullptr;
		ClassDefaultsOwner = MoveTemp(Candidate);
	}
	return ClassDefaultsOwner.Get();
}

FWoTRuntimeClassDefaults* UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(
	FString& OutError)
{
	if (UWoTPayloadSubsystem* Active = WoTPayload_ActiveSubsystem.Get())
		return Active->RuntimeClassDefaults(OutError);
	OutError = TEXT("runtime class defaults have no active payload owner");
	return nullptr;
}

UWoTPayloadSubsystem* UWoTPayloadSubsystem::ActivePayloadOwnerForRuntimeAssets()
{
	return IsWoTPayloadBootstrapActive()
		? WoTPayload_ActiveSubsystem.Get() : nullptr;
}

void UWoTPayloadSubsystem::ShutdownFrontEndFontsForScene()
{
	if (FrontEndFontOwner.IsValid())
	{
		FrontEndFontOwner->Shutdown();
		FrontEndFontOwner.Reset();
	}
}

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus UWoTPayloadSubsystem::InitializeFrontEndFontsForTesting()
{
	if (!ActiveCacheLease.IsValid() || FrontEndFontOwner.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("font automation requires one active unmaterialized lease")));
	}
	FrontEndFontOwner = MakeUnique<FWoTFrontEndFontOwner>();
	FWoTBootstrapStatus Result = FrontEndFontOwner->InitializeForTesting(
		*this, *ActiveCacheLease);
	if (Result.IsError()) FrontEndFontOwner.Reset();
	return Result;
}
#endif

void UWoTPayloadSubsystem::RebuildRequiredBSPKeysByMap()
{
	RequiredBSPKeysByMap.Empty();
	static const FString GeoPrefix = TEXT("/Game/WoT/Maps/Geo/");
	static const FString PortalSuffix = TEXT("_portals");
	for (const TPair<FString, FWoTBSPPayloadEntry>& Pair : BSPEntries)
	{
		if (!Pair.Value.AssetPath.StartsWith(GeoPrefix, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const FString Relative = Pair.Value.AssetPath.RightChop(GeoPrefix.Len());
		int32 SlashIndex = INDEX_NONE;
		if (!Relative.FindChar(TEXT('/'), SlashIndex) || SlashIndex <= 0)
		{
			continue;
		}
		FString MapName = Relative.Left(SlashIndex);
		if (MapName.EndsWith(PortalSuffix, ESearchCase::CaseSensitive))
		{
			MapName.LeftChopInline(PortalSuffix.Len(), EAllowShrinking::No);
		}
		if (!MapName.IsEmpty())
		{
			RequiredBSPKeysByMap.FindOrAdd(MapName).AddUnique(Pair.Key);
		}
	}
	for (TPair<FString, TArray<FString>>& Pair : RequiredBSPKeysByMap)
	{
		Pair.Value.Sort();
	}
}

bool UWoTPayloadSubsystem::TryGetBSPMapFromWorldPackage(
	const FString& WorldPackageName, FString& OutMapName)
{
	static const FString MapPrefix = TEXT("/Game/WoT/Maps/");
	OutMapName.Reset();
	if (!WorldPackageName.StartsWith(MapPrefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	OutMapName = WorldPackageName.RightChop(MapPrefix.Len());
	if (OutMapName.IsEmpty() || OutMapName.Contains(TEXT("/")))
	{
		OutMapName.Reset();
		return false;
	}
	return true;
}

bool UWoTPayloadSubsystem::IsBSPBarrierReadyForMap(const FString& MapName) const
{
	const TArray<FString>* Required = RequiredBSPKeysByMap.Find(MapName);
	if (!Required || Required->IsEmpty())
	{
		return true;
	}
	for (const FString& Key : *Required)
	{
		if (FailedRequiredBSPKeys.Contains(Key) || !ReadyBSPKeys.Contains(Key))
		{
			return false;
		}
	}
	return true;
}

FString UWoTPayloadSubsystem::DiscoverCacheDirectory() const
{
	FString Directory;
	if (FParse::Value(FCommandLine::Get(), TEXT("WoTPayloadCache="), Directory))
	{
		if (!FPaths::IsRelative(Directory))
		{
			FPaths::NormalizeDirectoryName(Directory);
			return Directory;
		}
		UE_LOG(LogWoTPayload, Error,
			TEXT("Ignoring relative -WoTPayloadCache override: %s"), *Directory);
	}

	if (const UWoTGameInstance* Instance = Cast<UWoTGameInstance>(GetGameInstance()))
	{
		Directory = Instance->GetPayloadCacheDir();
		if (!Directory.IsEmpty())
		{
			Directory = FPaths::IsRelative(Directory)
				? FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir(), Directory)
				: FPaths::ConvertRelativePathToFull(Directory);
			FPaths::NormalizeDirectoryName(Directory);
			return Directory;
		}
	}

	Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PayloadCache"));
	Directory = FPaths::ConvertRelativePathToFull(Directory);
	FPaths::NormalizeDirectoryName(Directory);
	return Directory;
}

bool UWoTPayloadSubsystem::FindRetailTextureFacts(FStringView Identity,
	FWoTRetailTextureFacts& Out)
{
	Out = FWoTRetailTextureFacts{};
	FWoTRetailObjectPath Path;
	if (!WoTParseRetailObjectPath(Identity, Path)) return false;
	auto Resolve = [&Path](EWoTNeutralResourceKind Kind)
	{
		return WoTRetailResolveLogicalBase(Kind, Path,
			[Kind](const FString& LogicalId)
			{
				return FWoTRuntimeRegistry::Get().FindDescriptor(
					{LogicalId, Kind}).IsSet();
			});
	};
	const EWoTNeutralResourceKind TextureKind = EWoTNeutralResourceKind::Texture;
	const FString TextureId = WoTRetailLogicalIdPrefix(TextureKind)
		+ Resolve(TextureKind);
	if (FWoTRuntimeRegistry::Get().FindDescriptor({TextureId, TextureKind}).IsSet())
	{
		const TSharedPtr<const FWoTTexturePayloadData> Payload =
			LoadTexturePayloadForRetailBuildContext(TextureId);
		if (!Payload.IsValid() || Payload->Mips.IsEmpty()) return false;
		Out.USize = Payload->Mips[0].Width;
		Out.VSize = Payload->Mips[0].Height;
		Out.Format = Payload->Format;
		Out.AnimNext = Payload->AnimNext;
		Out.MinFrameRate = Payload->MinFrameRate;
		Out.MaxFrameRate = Payload->MaxFrameRate;
		Out.PrimeCount = Payload->PrimeCount;
		return true;
	}
	const EWoTNeutralResourceKind ProceduralKind =
		EWoTNeutralResourceKind::Procedural;
	const FString ProceduralId = WoTRetailLogicalIdPrefix(ProceduralKind)
		+ Resolve(ProceduralKind);
	if (!FWoTRuntimeRegistry::Get().FindDescriptor(
		{ProceduralId, ProceduralKind}).IsSet()) return false;
	FWoTRuntimeProceduralResource Procedural;
	if (Procedural.LoadFirstUse({ProceduralId, ProceduralKind}).IsError()
		|| Procedural.Mips().IsEmpty()) return false;
	Out.USize = Procedural.Mips()[0].Width;
	Out.VSize = Procedural.Mips()[0].Height;
	Out.Format = 0;
	Out.bProcedural = true;
	return true;
}

const FWoTTexturePayloadEntry* UWoTPayloadSubsystem::FindTexturePayload(
	const FString& CacheKey) const
{
	return bManifestReady ? TextureEntries.Find(CacheKey) : nullptr;
}

const FWoTSoundPayloadEntry* UWoTPayloadSubsystem::FindSoundPayload(
	const FString& CacheKey) const
{
	return bManifestReady ? SoundEntries.Find(CacheKey) : nullptr;
}

const FWoTMeshPayloadEntry* UWoTPayloadSubsystem::FindMeshPayload(
	const FString& CacheKey) const
{
	return bManifestReady ? MeshEntries.Find(CacheKey) : nullptr;
}

const FWoTBSPPayloadEntry* UWoTPayloadSubsystem::FindBSPPayload(
	const FString& CacheKey) const
{
	return bManifestReady ? BSPEntries.Find(CacheKey) : nullptr;
}

#if !WOT_GUARDED_BOOTSTRAP
bool UWoTPayloadSubsystem::LoadManifest()
{
	bManifestReady = false;
	TextureEntries.Empty();
	DirectTextureKeys.Empty();
	SoundEntries.Empty();
	DirectSoundKeys.Empty();
	MeshEntries.Empty();
	BSPEntries.Empty();
	RequiredBSPKeysByMap.Empty();

	const FString ManifestPath = FPaths::Combine(CacheDirectory, TEXT("manifest.json"));
	TArray<uint8> ManifestBytes;
	FString ManifestReadError;
	if (!WoTPayload_ReadFileBounded(ManifestPath,
			static_cast<uint64>(WoTPayload_MaxManifestBytes), TOptional<uint64>(),
			ManifestBytes, ManifestReadError))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Cannot read payload manifest %s: %s"),
			*ManifestPath, *ManifestReadError);
		return false;
	}
	FString RawManifest;
	const FUTF8ToTCHAR ConvertedManifest(
		reinterpret_cast<const ANSICHAR*>(ManifestBytes.GetData()),
		ManifestBytes.Num());
	RawManifest = FString(ConvertedManifest.Length(), ConvertedManifest.Get());
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RawManifest), Root)
		|| !WoTPayload_HasExactFields(Root, {
			TEXT("format"), TEXT("cache_version"), TEXT("hash_algorithm"),
			TEXT("limits"), TEXT("types"), TEXT("entries"), TEXT("sources"),
			TEXT("skipped"), TEXT("summary"), TEXT("cache_sha256")}))
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest is not an exact supported top-level object"));
		return false;
	}

	FString Format;
	FString HashAlgorithm;
	FString DeclaredRoot;
	uint64 CacheVersion = 0;
	if (!WoTPayload_ReadString(Root, TEXT("format"), Format)
		|| Format != TEXT("wot-payload-cache")
		|| !WoTPayload_ReadUInt(Root, TEXT("cache_version"), 2, CacheVersion)
		|| (CacheVersion != 1 && CacheVersion != 2)
		|| !WoTPayload_ReadString(Root, TEXT("hash_algorithm"), HashAlgorithm)
		|| HashAlgorithm != TEXT("sha256")
		|| !WoTPayload_ReadString(Root, TEXT("cache_sha256"), DeclaredRoot)
		|| !WoTPayload_IsSHA256(DeclaredRoot))
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest format/version/hash gate failed"));
		return false;
	}

	const TSharedPtr<FJsonObject>* Limits = nullptr;
	uint64 MaxBlobSize = 0;
	uint64 MaxEntries = 0;
	uint64 MaxDimension = 0;
	uint64 MaxExpanded = 0;
	uint64 MaxBSPVertices = 0;
	uint64 MaxBSPIndices = 0;
	uint64 MaxBSPSections = 0;
	uint64 MaxBSPSlotNameBytes = 0;
	if (!WoTPayload_ReadObject(Root, TEXT("limits"), Limits))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest limits is not an object"));
		return false;
	}
	const bool bLimitsShapeValid = CacheVersion == 1
		? WoTPayload_HasExactFields(*Limits, {
			TEXT("max_blob_size"), TEXT("max_entries"),
			TEXT("max_texture_dimension"), TEXT("max_total_expanded_bytes")})
		: WoTPayload_HasExactFields(*Limits, {
			TEXT("max_blob_size"), TEXT("max_entries"),
			TEXT("max_texture_dimension"), TEXT("max_total_expanded_bytes"),
			TEXT("max_bsp_vertices"), TEXT("max_bsp_indices"),
			TEXT("max_bsp_sections"), TEXT("max_bsp_slot_name_bytes")});
	if (!bLimitsShapeValid
		|| !WoTPayload_ReadUInt(*Limits, TEXT("max_blob_size"),
			WoTPayload_MaxExactJsonInteger, MaxBlobSize)
		|| !WoTPayload_ReadUInt(*Limits, TEXT("max_entries"),
			WoTPayload_MaxExactJsonInteger, MaxEntries)
		|| !WoTPayload_ReadUInt(*Limits, TEXT("max_texture_dimension"),
			WoTPayload_MaxExactJsonInteger, MaxDimension)
		|| !WoTPayload_ReadUInt(*Limits, TEXT("max_total_expanded_bytes"),
			WoTPayload_MaxExactJsonInteger, MaxExpanded)
		|| MaxBlobSize != WoTPayload_MaxBlobSize
		|| MaxEntries != WoTPayload_MaxEntries
		|| MaxDimension != WoTPayload_MaxTextureDimension
		|| MaxExpanded != WoTPayload_MaxTotalExpandedBytes
		|| (CacheVersion == 2
			&& (!WoTPayload_ReadUInt(*Limits, TEXT("max_bsp_vertices"),
				WoTPayload_MaxExactJsonInteger, MaxBSPVertices)
				|| !WoTPayload_ReadUInt(*Limits, TEXT("max_bsp_indices"),
					WoTPayload_MaxExactJsonInteger, MaxBSPIndices)
				|| !WoTPayload_ReadUInt(*Limits, TEXT("max_bsp_sections"),
					WoTPayload_MaxExactJsonInteger, MaxBSPSections)
				|| !WoTPayload_ReadUInt(*Limits, TEXT("max_bsp_slot_name_bytes"),
					WoTPayload_MaxExactJsonInteger, MaxBSPSlotNameBytes)
				|| MaxBSPVertices != WoTPayload_MaxBSPVertices
				|| MaxBSPIndices != WoTPayload_MaxBSPIndices
				|| MaxBSPSections != WoTPayload_MaxBSPSections
				|| MaxBSPSlotNameBytes != WoTPayload_MaxBSPSlotNameBytes)))
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest does not declare the compiled version-%llu limits"),
			CacheVersion);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
	if (!WoTPayload_ReadArray(Root, TEXT("types"), TypeValues)
		|| TypeValues->Num() > (CacheVersion == 2 ? 4 : 3))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest has an invalid type list"));
		return false;
	}
	TSet<FString> DeclaredTypes;
	int32 PreviousTypeOrder = INDEX_NONE;
	for (const TSharedPtr<FJsonValue>& TypeValue : *TypeValues)
	{
		FString TypeName;
		EWoTPayloadType IgnoredType;
		if (!TypeValue.IsValid() || TypeValue->Type != EJson::String
			|| !TypeValue->TryGetString(TypeName)
			|| !WoTPayload_ParseType(TypeName, IgnoredType)
			|| (CacheVersion == 1 && IgnoredType == EWoTPayloadType::BSP)
			|| DeclaredTypes.Contains(TypeName)
			|| WoTPayload_TypeOrder(TypeName) <= PreviousTypeOrder)
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload manifest type list is unknown, repeated, or unsorted"));
			return false;
		}
		PreviousTypeOrder = WoTPayload_TypeOrder(TypeName);
		DeclaredTypes.Add(TypeName);
	}

	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	if (!WoTPayload_ReadArray(Root, TEXT("sources"), Sources))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest sources is not an array"));
		return false;
	}
	TSet<FString> SourceKeys;
	TMap<FString, TSet<FString>> SourcePackages;
	uint64 SourceCounts[4] = {0, 0, 0, 0};
	int32 PriorSourceType = INDEX_NONE;
	FString PriorSourceFoldedPath;
	FString PriorSourcePath;
	for (const TSharedPtr<FJsonValue>& SourceValue : *Sources)
	{
		const TSharedPtr<FJsonObject> Source = SourceValue.IsValid()
			? SourceValue->AsObject() : nullptr;
		FString TypeName;
		FString Path;
		FString SHA256;
		uint64 Size = 0;
		EWoTPayloadType Type;
		if (!WoTPayload_HasExactFields(Source, {
				TEXT("type"), TEXT("path"), TEXT("size"), TEXT("sha256")})
			|| !WoTPayload_ReadString(Source, TEXT("type"), TypeName)
			|| !WoTPayload_ParseType(TypeName, Type)
			|| !DeclaredTypes.Contains(TypeName)
			|| !WoTPayload_ReadString(Source, TEXT("path"), Path)
			|| !WoTPayload_IsSafeRelativePath(Path)
			|| !WoTPayload_ReadUInt(Source, TEXT("size"),
				WoTPayload_MaxExactJsonInteger, Size)
			|| !WoTPayload_ReadString(Source, TEXT("sha256"), SHA256)
			|| !WoTPayload_IsSHA256(SHA256))
		{
			UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest has an invalid source record"));
			return false;
		}

		const FString SourceKey = TypeName + TEXT(":") + Path;
		const int32 TypeOrder = WoTPayload_TypeOrder(TypeName);
		const FString FoldedPath = Path.ToLower();
		const bool bOutOfOrder = PriorSourceType != INDEX_NONE
			&& (TypeOrder < PriorSourceType
				|| (TypeOrder == PriorSourceType
					&& (FoldedPath < PriorSourceFoldedPath
						|| (FoldedPath == PriorSourceFoldedPath && Path < PriorSourcePath))));
		if (SourceKeys.Contains(SourceKey) || bOutOfOrder)
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload manifest repeats or misorders source %s"), *SourceKey);
			return false;
		}
		SourceKeys.Add(SourceKey);
		SourcePackages.FindOrAdd(TypeName).Add(FPaths::GetBaseFilename(Path));
		++SourceCounts[TypeOrder];
		PriorSourceType = TypeOrder;
		PriorSourceFoldedPath = FoldedPath;
		PriorSourcePath = Path;
	}

	const TSharedPtr<FJsonObject>* EntriesObject = nullptr;
	if (!WoTPayload_ReadObject(Root, TEXT("entries"), EntriesObject)
		|| static_cast<uint64>((*EntriesObject)->Values.Num()) > WoTPayload_MaxEntries)
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest entries is invalid or exceeds the 10,000-entry cap"));
		return false;
	}

	TMap<FString, FWoTTexturePayloadEntry> AcceptedTextures;
	TMap<FString, FWoTSoundPayloadEntry> AcceptedSounds;
	TMap<FString, FWoTMeshPayloadEntry> AcceptedMeshes;
	TMap<FString, FWoTBSPPayloadEntry> AcceptedBSPs;
	TArray<FWoTPayloadBlobRecord> BlobRecords;
	TMap<FString, FWoTPayloadBlobRecord> UniqueBlobs;
	uint64 DeclaredTypeCounts[4] = {0, 0, 0, 0};
	uint64 LogicalPayloadBytes = 0;
	uint64 AcceptedExpandedBytes = 0;
	int32 RejectedEntryCount = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*EntriesObject)->Values)
	{
		const FString& Key = Pair.Key;
		const TSharedPtr<FJsonObject> EntryObject = Pair.Value.IsValid()
			? Pair.Value->AsObject() : nullptr;
		if (!EntryObject.IsValid())
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting payload entry %s: record is not an object"), *Key);
			++RejectedEntryCount;
			continue;
		}

		FWoTPayloadEntryBase Base;
		FString EntryError;
		const bool bBaseValid = WoTPayload_ParseEntryBase(
			Key, EntryObject, Base, EntryError);

		// The Merkle record uses only these authenticated base values. Retain it
		// even when type-specific metadata rejects the semantic entry.
		const bool bBlobRecordUsable = Base.Blob.Size > 0
			&& Base.Blob.Size <= WoTPayload_MaxBlobSize
			&& WoTPayload_IsSHA256(Base.Blob.SHA256)
			&& Base.Blob.Path == WoTPayload_BlobPath(Base.Blob.SHA256);
		if (!bBlobRecordUsable)
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload entry %s lacks a safe Merkle blob record"), *Key);
			return false;
		}
		BlobRecords.Add(Base.Blob);
		if (const FWoTPayloadBlobRecord* Prior = UniqueBlobs.Find(Base.Blob.Path))
		{
			if (Prior->Size != Base.Blob.Size || Prior->SHA256 != Base.Blob.SHA256)
			{
				UE_LOG(LogWoTPayload, Error,
					TEXT("Payload blob %s has conflicting manifest records"), *Base.Blob.Path);
				return false;
			}
		}
		else
		{
			UniqueBlobs.Add(Base.Blob.Path, Base.Blob);
		}
		if (!WoTPayload_CheckedAdd(
			LogicalPayloadBytes, Base.Blob.Size, LogicalPayloadBytes))
		{
			UE_LOG(LogWoTPayload, Error, TEXT("Payload logical byte total overflowed"));
			return false;
		}
		const int32 TypeOrder = WoTPayload_TypeOrder(Base.TypeName);
		if (TypeOrder != INDEX_NONE)
		{
			++DeclaredTypeCounts[TypeOrder];
		}

		if (!bBaseValid)
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting payload entry %s: %s"), *Key, *EntryError);
			++RejectedEntryCount;
			continue;
		}
		const TSet<FString>* PackagesForType = SourcePackages.Find(Base.TypeName);
		if (!DeclaredTypes.Contains(Base.TypeName) || !PackagesForType
			|| !PackagesForType->Contains(Base.PackageName))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting payload entry %s: undeclared type or no matching source package"),
				*Key);
			++RejectedEntryCount;
			continue;
		}

		uint64 EntryExpandedBytes = 0;
		bool bEntryValid = false;
		if (Base.Type == EWoTPayloadType::Texture)
		{
			FWoTTexturePayloadEntry TextureEntry;
			bEntryValid = WoTPayload_ParseTextureEntry(Key, EntryObject, Base,
				TextureEntry, EntryExpandedBytes, EntryError);
			if (bEntryValid)
			{
				AcceptedTextures.Add(Key, MoveTemp(TextureEntry));
			}
		}
		else if (Base.Type == EWoTPayloadType::Sound)
		{
			FWoTSoundPayloadEntry SoundEntry;
			bEntryValid = WoTPayload_ParseSoundEntry(Key, EntryObject, Base,
				SoundEntry, EntryExpandedBytes, EntryError);
			if (bEntryValid)
			{
				AcceptedSounds.Add(Key, MoveTemp(SoundEntry));
			}
		}
		else if (Base.Type == EWoTPayloadType::Mesh)
		{
			FWoTMeshPayloadEntry MeshEntry;
			bEntryValid = WoTPayload_ParseMeshEntry(Key, EntryObject, Base,
				MeshEntry, EntryExpandedBytes, EntryError);
			if (bEntryValid)
			{
				AcceptedMeshes.Add(Key, MoveTemp(MeshEntry));
			}
		}
		else
		{
			FWoTBSPPayloadEntry BSPEntry;
			bEntryValid = WoTPayload_ParseBSPEntry(Key, EntryObject, Base,
				BSPEntry, EntryExpandedBytes, EntryError);
			if (bEntryValid)
			{
				AcceptedBSPs.Add(Key, MoveTemp(BSPEntry));
			}
		}
		if (!bEntryValid)
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting payload entry %s: %s"), *Key, *EntryError);
			++RejectedEntryCount;
			continue;
		}
		if (!WoTPayload_CheckedAdd(
			AcceptedExpandedBytes, EntryExpandedBytes, AcceptedExpandedBytes)
			|| AcceptedExpandedBytes > WoTPayload_MaxTotalExpandedBytes)
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Accepted payload entries exceed the 2 GiB expanded-byte cap"));
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Skipped = nullptr;
	if (!WoTPayload_ReadArray(Root, TEXT("skipped"), Skipped))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest skipped is not an array"));
		return false;
	}
	int32 PriorSkippedType = INDEX_NONE;
	FString PriorSkippedSourceFolded;
	FString PriorSkippedNameFolded;
	FString PriorSkippedReason;
	for (const TSharedPtr<FJsonValue>& SkippedValue : *Skipped)
	{
		const TSharedPtr<FJsonObject> Item = SkippedValue.IsValid()
			? SkippedValue->AsObject() : nullptr;
		FString TypeName;
		FString Source;
		FString Package;
		FString Name;
		FString Reason;
		EWoTPayloadType IgnoredType;
		if (!WoTPayload_HasExactFields(Item, {
				TEXT("type"), TEXT("source"), TEXT("package"),
				TEXT("name"), TEXT("reason")})
			|| !WoTPayload_ReadString(Item, TEXT("type"), TypeName)
			|| !WoTPayload_ParseType(TypeName, IgnoredType)
			|| !DeclaredTypes.Contains(TypeName)
			|| !WoTPayload_ReadString(Item, TEXT("source"), Source)
			|| !WoTPayload_IsSafeRelativePath(Source)
			|| !SourceKeys.Contains(TypeName + TEXT(":") + Source)
			|| !WoTPayload_ReadString(Item, TEXT("package"), Package)
			|| !WoTPayload_IsSafeComponent(Package, false)
			|| Package != FPaths::GetBaseFilename(Source)
			|| !WoTPayload_ReadString(Item, TEXT("name"), Name)
			|| !WoTPayload_IsSafeComponent(Name, true)
			|| !WoTPayload_ReadString(Item, TEXT("reason"), Reason)
			|| Reason.IsEmpty())
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload manifest has an invalid skipped-entry record"));
			return false;
		}
		const int32 TypeOrder = WoTPayload_TypeOrder(TypeName);
		const FString SourceFolded = Source.ToLower();
		const FString NameFolded = Name.ToLower();
		const bool bOutOfOrder = PriorSkippedType != INDEX_NONE
			&& (TypeOrder < PriorSkippedType
				|| (TypeOrder == PriorSkippedType
					&& (SourceFolded < PriorSkippedSourceFolded
						|| (SourceFolded == PriorSkippedSourceFolded
							&& (NameFolded < PriorSkippedNameFolded
								|| (NameFolded == PriorSkippedNameFolded
									&& Reason < PriorSkippedReason))))));
		if (bOutOfOrder)
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload manifest skipped records are not canonical"));
			return false;
		}
		PriorSkippedType = TypeOrder;
		PriorSkippedSourceFolded = SourceFolded;
		PriorSkippedNameFolded = NameFolded;
		PriorSkippedReason = Reason;
	}

	const TSharedPtr<FJsonObject>* Summary = nullptr;
	const TSharedPtr<FJsonObject>* SummaryEntries = nullptr;
	const TSharedPtr<FJsonObject>* SummarySources = nullptr;
	uint64 SummaryLogicalBytes = 0;
	uint64 SummaryExpandedBytes = 0;
	uint64 SummaryUniqueBlobs = 0;
	uint64 SummaryUniqueBytes = 0;
	if (!WoTPayload_ReadObject(Root, TEXT("summary"), Summary)
		|| !WoTPayload_HasExactFields(*Summary, {
			TEXT("entries"), TEXT("source_packages"), TEXT("logical_payload_bytes"),
			TEXT("total_expanded_bytes"), TEXT("unique_blobs"),
			TEXT("unique_blob_bytes")})
		|| !WoTPayload_ReadObject(*Summary, TEXT("entries"), SummaryEntries)
		|| !WoTPayload_ReadObject(*Summary, TEXT("source_packages"), SummarySources)
		|| !WoTPayload_ReadUInt(*Summary, TEXT("logical_payload_bytes"),
			WoTPayload_MaxExactJsonInteger, SummaryLogicalBytes)
		|| !WoTPayload_ReadUInt(*Summary, TEXT("total_expanded_bytes"),
			WoTPayload_MaxTotalExpandedBytes, SummaryExpandedBytes)
		|| !WoTPayload_ReadUInt(*Summary, TEXT("unique_blobs"),
			WoTPayload_MaxEntries, SummaryUniqueBlobs)
		|| !WoTPayload_ReadUInt(*Summary, TEXT("unique_blob_bytes"),
			WoTPayload_MaxExactJsonInteger, SummaryUniqueBytes))
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest summary shape is invalid"));
		return false;
	}
	const bool bSummaryEntryShapeValid = CacheVersion == 1
		? WoTPayload_HasExactFields(*SummaryEntries, {
			TEXT("textures"), TEXT("sounds"), TEXT("meshes")})
		: WoTPayload_HasExactFields(*SummaryEntries, {
			TEXT("textures"), TEXT("sounds"), TEXT("meshes"), TEXT("bsps")});
	const bool bSummarySourceShapeValid = CacheVersion == 1
		? WoTPayload_HasExactFields(*SummarySources, {
			TEXT("textures"), TEXT("sounds"), TEXT("meshes")})
		: WoTPayload_HasExactFields(*SummarySources, {
			TEXT("textures"), TEXT("sounds"), TEXT("meshes"), TEXT("bsps")});
	if (!bSummaryEntryShapeValid || !bSummarySourceShapeValid)
	{
		UE_LOG(LogWoTPayload, Error, TEXT("Payload manifest summary type shape is invalid"));
		return false;
	}

	uint64 ComputedUniqueBytes = 0;
	for (const TPair<FString, FWoTPayloadBlobRecord>& Pair : UniqueBlobs)
	{
		if (!WoTPayload_CheckedAdd(
			ComputedUniqueBytes, Pair.Value.Size, ComputedUniqueBytes))
		{
			UE_LOG(LogWoTPayload, Error, TEXT("Payload unique byte total overflowed"));
			return false;
		}
	}
	const TCHAR* TypeNames[4] = {
		TEXT("textures"), TEXT("sounds"), TEXT("meshes"), TEXT("bsps")};
	const int32 TypeCount = CacheVersion == 2 ? 4 : 3;
	for (int32 TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
	{
		uint64 SummaryEntryCount = 0;
		uint64 SummarySourceCount = 0;
		if (!WoTPayload_ReadUInt(*SummaryEntries, TypeNames[TypeIndex],
				WoTPayload_MaxEntries, SummaryEntryCount)
			|| SummaryEntryCount != DeclaredTypeCounts[TypeIndex]
			|| !WoTPayload_ReadUInt(*SummarySources, TypeNames[TypeIndex],
				WoTPayload_MaxEntries, SummarySourceCount)
			|| SummarySourceCount != SourceCounts[TypeIndex])
		{
			UE_LOG(LogWoTPayload, Error,
				TEXT("Payload manifest summary count mismatch for %s"), TypeNames[TypeIndex]);
			return false;
		}
	}
	if (SummaryLogicalBytes != LogicalPayloadBytes
		|| SummaryUniqueBlobs != static_cast<uint64>(UniqueBlobs.Num())
		|| SummaryUniqueBytes != ComputedUniqueBytes
		|| SummaryExpandedBytes < AcceptedExpandedBytes
		|| (RejectedEntryCount == 0 && SummaryExpandedBytes != AcceptedExpandedBytes))
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest byte/blob summary is inconsistent"));
		return false;
	}

	FString ComputedRoot;
	if (!WoTPayload_ComputeCacheRoot(
		RawManifest, CacheVersion, DeclaredRoot, BlobRecords, ComputedRoot)
		|| ComputedRoot != DeclaredRoot)
	{
		UE_LOG(LogWoTPayload, Error,
			TEXT("Payload manifest whole-cache root mismatch (declared %s, computed %s)"),
			*DeclaredRoot, *ComputedRoot);
		return false;
	}

	TextureEntries = MoveTemp(AcceptedTextures);
	SoundEntries = MoveTemp(AcceptedSounds);
	MeshEntries = MoveTemp(AcceptedMeshes);
	BSPEntries = MoveTemp(AcceptedBSPs);
	RebuildRequiredBSPKeysByMap();
	for (const TPair<FString, FWoTTexturePayloadEntry>& Pair : TextureEntries)
	{
		const FName Identity(*Pair.Value.AssetPath);
		if (const FString* PriorKey = DirectTextureKeys.Find(Identity))
		{
			if (*PriorKey != Pair.Key)
			{
				UE_LOG(LogWoTPayload, Error,
					TEXT("Payload texture keys %s and %s alias the same case-insensitive UE object identity"),
					**PriorKey, *Pair.Key);
				TextureEntries.Empty();
				DirectTextureKeys.Empty();
				return false;
			}
		}
		else
		{
			DirectTextureKeys.Add(Identity, Pair.Key);
		}
	}
	for (const TPair<FString, FWoTSoundPayloadEntry>& Pair : SoundEntries)
	{
		const FName Identity(*Pair.Value.AssetPath);
		if (const FString* PriorKey = DirectSoundKeys.Find(Identity))
		{
			if (*PriorKey != Pair.Key)
			{
				UE_LOG(LogWoTPayload, Error,
					TEXT("Payload sound keys %s and %s alias the same case-insensitive UE object identity"),
					**PriorKey, *Pair.Key);
				TextureEntries.Empty();
				DirectTextureKeys.Empty();
				SoundEntries.Empty();
				DirectSoundKeys.Empty();
				return false;
			}
		}
		else
		{
			DirectSoundKeys.Add(Identity, Pair.Key);
		}
	}
	bManifestReady = true;
	UE_LOG(LogWoTPayload, Display,
		TEXT("Payload cache ready: %d texture, %d sound, %d mesh, and %d BSP entries accepted, %d entries rejected (%s)"),
		TextureEntries.Num(), SoundEntries.Num(), MeshEntries.Num(), BSPEntries.Num(),
		RejectedEntryCount, *DeclaredRoot);
	return true;
}
#endif

TSharedPtr<const FWoTTexturePayloadData> UWoTPayloadSubsystem::LoadTexturePayload(
	const FString& CacheKey)
{
	return LoadTexturePayloadInternal(CacheKey, false);
}

TSharedPtr<const FWoTTexturePayloadData>
UWoTPayloadSubsystem::LoadTexturePayloadForRetailBuildContext(
	const FString& CacheKey)
{
	return LoadTexturePayloadInternal(CacheKey, true);
}

TSharedPtr<const FWoTTexturePayloadData>
UWoTPayloadSubsystem::LoadTexturePayloadInternal(
	const FString& CacheKey, bool bAllowActivatingGeneration)
{
	const bool bActivationRead = bAllowActivatingGeneration
		&& ActiveCacheLease.IsValid() && ActiveCacheLease->IsValid()
		&& IsWoTPayloadBootstrapActive()
		&& FWoTRuntimeRegistry::Get().SharesSnapshotWith(*ActiveCacheLease);
	if (((!bInjectionEnabled || !bManifestReady) && !bActivationRead)
		|| !IsInGameThread())
	{
		return nullptr;
	}
	if (const TSharedPtr<const FWoTTexturePayloadData>* Existing =
		LoadedTexturePayloads.Find(CacheKey))
	{
		return *Existing;
	}
	if (RejectedBlobKeys.Contains(CacheKey))
	{
		return nullptr;
	}
	const FWoTTexturePayloadEntry* Entry = nullptr;
	FWoTTexturePayloadEntry ActivatedEntry;
	FWoTRuntimeVerifiedObject VerifiedObject;
#if !WOT_GUARDED_BOOTSTRAP
	TArray<uint8> DeveloperBlobBytes;
#endif
	TConstArrayView<uint8> BlobBytes;
	FString AuthenticatedSourcePackage;
	FString AuthenticatedSourceExport;
	FWoTSha256 AuthenticatedSourceSha;

	auto Reject = [this, &CacheKey](const FString& Reason)
	{
		RejectedBlobKeys.Add(CacheKey);
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting texture payload %s: %s"), *CacheKey, *Reason);
	};

	if (bPayloadActivated || bActivationRead)
	{
	if (!CacheKey.StartsWith(
			TEXT("cache/texture/"), ESearchCase::CaseSensitive))
	{
		Reject(TEXT("guarded texture carrier does not contain an exact logical id"));
		return nullptr;
	}
	const FWoTNeutralResourceId Resource{
		CacheKey, EWoTNeutralResourceKind::Texture};
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> ObjectRead =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, WoTPayload_MaxBlobSize,
			TEXT("ue1-p8-mips-rgba-palette-v1"), TEXT("texture"));
	if (ObjectRead.IsError())
	{
		Reject(MoveTemp(ObjectRead).GetError().ToString());
		return nullptr;
	}
	VerifiedObject = MoveTemp(ObjectRead).GetValue();
	const FWoTCanonicalJsonValue& ObjectHeader = VerifiedObject.Header();
	static const TCHAR* TextureHeaderFields[] = {
		TEXT("anim_next"), TEXT("color"), TEXT("decoded_layout"), TEXT("encoding"),
		TEXT("fact_id"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kind"), TEXT("masked"), TEXT("max_frame_rate_bits"),
		TEXT("min_frame_rate_bits"), TEXT("payload_byte_length"),
		TEXT("prime_count"), TEXT("source")
	};
	static const TCHAR* ColorFields[] = {
		TEXT("alpha"), TEXT("channel_order"), TEXT("palette_colors"),
		TEXT("stored_palette_alpha")
	};
	static const TCHAR* LayoutFields[] = {
		TEXT("mip_count"), TEXT("mips"), TEXT("texel_format")
	};
	static const TCHAR* MipFields[] = {
		TEXT("expanded_rgba_byte_length"), TEXT("height"),
		TEXT("index_byte_length"), TEXT("width")
	};
	static const TCHAR* SourceFields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	const FWoTCanonicalJsonValue* Color = nullptr;
	const FWoTCanonicalJsonValue* Layout = nullptr;
	const FWoTCanonicalJsonValue* Source = nullptr;
	FString Alpha;
	FString ChannelOrder;
	FString StoredPaletteAlpha;
	FString TexelFormat;
	FString Importer;
	FString FactId;
	FString SourceExport;
	FString SourcePackage;
	FString SourceHash;
	FString AnimNext;
	uint32 PaletteColorCount = 0;
	uint32 MipCountFromHeader = 0;
	uint32 ImporterVersion = 0;
	uint32 MinFrameRateBits = 0;
	uint32 MaxFrameRateBits = 0;
	uint32 PrimeCount = 0;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	bool bMasked = false;
	const FWoTCanonicalJsonValue* HeaderMips = nullptr;
	ActivatedEntry.CacheKey = CacheKey;
	TOptional<FWoTBootstrapError> HeaderError = RequireExactMembers(
		ObjectHeader, MakeArrayView(TextureHeaderFields),
		TEXT("texture object header"));
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			ObjectHeader, TEXT("anim_next"),
			TEXT("texture object header"), AnimNext);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("color"), TEXT("texture object header"), Color);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("decoded_layout"),
			TEXT("texture object header"), Layout);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("source"),
			TEXT("texture object header"), Source);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			ObjectHeader, TEXT("fact_id"),
			TEXT("texture object header"), FactId);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			ObjectHeader, TEXT("importer"),
			TEXT("texture object header"), Importer);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsigned32Field(
			ObjectHeader, TEXT("importer_version"),
			TEXT("texture object header"), ImporterVersion);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadBoolField(
			ObjectHeader, TEXT("masked"),
			TEXT("texture object header"), bMasked);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsigned32Field(ObjectHeader,
			TEXT("min_frame_rate_bits"), TEXT("texture object header"),
			MinFrameRateBits);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsigned32Field(ObjectHeader,
			TEXT("max_frame_rate_bits"), TEXT("texture object header"),
			MaxFrameRateBits);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsigned32Field(ObjectHeader,
			TEXT("prime_count"), TEXT("texture object header"), PrimeCount);
	}
	FMemory::Memcpy(&MinFrameRate, &MinFrameRateBits, sizeof(float));
	FMemory::Memcpy(&MaxFrameRate, &MaxFrameRateBits, sizeof(float));
	if (!HeaderError.IsSet() && Color != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Color, MakeArrayView(ColorFields), TEXT("texture color"));
	}
	if (!HeaderError.IsSet() && Color != nullptr)
	{
		HeaderError = ReadStringField(
			*Color, TEXT("alpha"), TEXT("texture color"), Alpha);
	}
	if (!HeaderError.IsSet() && Color != nullptr)
	{
		HeaderError = ReadStringField(
			*Color, TEXT("channel_order"),
			TEXT("texture color"), ChannelOrder);
	}
	if (!HeaderError.IsSet() && Color != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Color, TEXT("palette_colors"),
			TEXT("texture color"), PaletteColorCount);
	}
	if (!HeaderError.IsSet() && Color != nullptr)
	{
		HeaderError = ReadStringField(
			*Color, TEXT("stored_palette_alpha"),
			TEXT("texture color"), StoredPaletteAlpha);
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Layout, MakeArrayView(LayoutFields), TEXT("texture layout"));
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Layout, TEXT("mip_count"),
			TEXT("texture layout"), MipCountFromHeader);
		HeaderMips = Layout->FindMember(TEXT("mips"));
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = ReadStringField(
			*Layout, TEXT("texel_format"),
			TEXT("texture layout"), TexelFormat);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Source, MakeArrayView(SourceFields), TEXT("texture source"));
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("export"), TEXT("texture source"), SourceExport);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("package"), TEXT("texture source"), SourcePackage);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("sha256"), TEXT("texture source"), SourceHash);
	}
	if (HeaderError.IsSet() || FactId.IsEmpty()
		|| Importer != TEXT("texture-v2") || ImporterVersion != 2
		|| Alpha != TEXT("index-zero-if-masked")
		|| ChannelOrder != TEXT("rgba") || PaletteColorCount != 256
		|| StoredPaletteAlpha != TEXT("preserved-but-non-semantic")
		|| TexelFormat != TEXT("p8") || MipCountFromHeader == 0
		|| MipCountFromHeader > WoTPayload_MaxTextureMips
		|| HeaderMips == nullptr || !HeaderMips->IsArray()
		|| HeaderMips->Num() != static_cast<int32>(MipCountFromHeader)
		|| SourcePackage.IsEmpty() || SourceHash.Len() != 64
		|| !FMath::IsFinite(MinFrameRate) || MinFrameRate < 0.0f
		|| !FMath::IsFinite(MaxFrameRate) || MaxFrameRate < MinFrameRate
		|| PrimeCount > MAX_uint8)
	{
		Reject(TEXT("texture canonical header is malformed"));
		return nullptr;
	}
	FWoTRetailObjectPath AnimNextPath;
	if (!AnimNext.IsEmpty()
		&& !WoTParseRetailObjectPath(AnimNext, AnimNextPath))
	{
		Reject(TEXT("texture AnimNext identity is malformed"));
		return nullptr;
	}
	TWoTBootstrapResult<FWoTSha256> ParsedTextureSourceSha =
		FWoTSha256::FromHex(SourceHash);
	if (ParsedTextureSourceSha.IsError())
	{
		Reject(TEXT("texture source digest is malformed"));
		return nullptr;
	}
	AuthenticatedSourcePackage = SourcePackage;
	AuthenticatedSourceExport = SourceExport;
	AuthenticatedSourceSha = MoveTemp(ParsedTextureSourceSha).GetValue();
	ActivatedEntry.bMasked = bMasked;
	ActivatedEntry.AnimNext = MoveTemp(AnimNext);
	ActivatedEntry.MinFrameRate = MinFrameRate;
	ActivatedEntry.MaxFrameRate = MaxFrameRate;
	ActivatedEntry.PrimeCount = static_cast<uint8>(PrimeCount);
	ActivatedEntry.Mips.Reserve(static_cast<int32>(MipCountFromHeader));
	for (int32 MipIndex = 0; MipIndex < HeaderMips->Num(); ++MipIndex)
	{
		const FWoTCanonicalJsonValue& Mip = HeaderMips->GetElement(MipIndex);
		FWoTTexturePayloadMip& Parsed =
			ActivatedEntry.Mips.AddDefaulted_GetRef();
		uint32 Expanded = 0;
		uint32 Stored = 0;
		if (RequireExactMembers(
				Mip, MakeArrayView(MipFields), TEXT("texture mip")).IsSet()
			|| ReadUnsigned32Field(
				Mip, TEXT("width"), TEXT("texture mip"), Parsed.Width).IsSet()
			|| ReadUnsigned32Field(
				Mip, TEXT("height"), TEXT("texture mip"), Parsed.Height).IsSet()
			|| ReadUnsigned32Field(
				Mip, TEXT("index_byte_length"),
				TEXT("texture mip"), Stored).IsSet()
			|| ReadUnsigned32Field(
				Mip, TEXT("expanded_rgba_byte_length"),
				TEXT("texture mip"), Expanded).IsSet())
		{
			Reject(TEXT("texture mip canonical header is malformed"));
			return nullptr;
		}
		Parsed.StoredByteLength = Stored;
		Parsed.ExpandedByteLength = Expanded;
	}
	Entry = &ActivatedEntry;
	BlobBytes = VerifiedObject.Payload();
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		Entry = TextureEntries.Find(CacheKey);
		if (!Entry)
		{
			return nullptr;
		}
		FString BlobReadError;
		const FString BlobFilename =
			FPaths::Combine(CacheDirectory, Entry->BlobPath);
		if (!WoTPayload_ReadFileBounded(
				BlobFilename, WoTPayload_MaxBlobSize,
				TOptional<uint64>(Entry->BlobSize),
				DeveloperBlobBytes, BlobReadError))
		{
			Reject(BlobReadError);
			return nullptr;
		}
		if (WoTPayload_SHA256(
				DeveloperBlobBytes.GetData(),
				static_cast<uint32>(DeveloperBlobBytes.Num()))
			!= Entry->BlobSHA256)
		{
			Reject(TEXT("blob SHA-256 does not match the developer manifest"));
			return nullptr;
		}
		BlobBytes = DeveloperBlobBytes;
	}
#endif
	if (Entry == nullptr)
	{
		return nullptr;
	}

	constexpr uint8 ExpectedMagic[8] = {
		'W', 'O', 'T', 'P', 'T', 'E', 'X', 0
	};
	if (BlobBytes.Num() < 8
		|| FMemory::Memcmp(BlobBytes.GetData(), ExpectedMagic, 8) != 0)
	{
		Reject(TEXT("truncated or invalid texture magic"));
		return nullptr;
	}
	uint64 Offset = 8;
	uint16 EncodingVersion = 0;
	uint16 Flags = 0;
	uint32 PaletteColors = 0;
	uint32 MipCount = 0;
	if (!WoTPayload_ReadU16(BlobBytes, Offset, EncodingVersion)
		|| !WoTPayload_ReadU16(BlobBytes, Offset, Flags)
		|| !WoTPayload_ReadU32(BlobBytes, Offset, PaletteColors)
		|| !WoTPayload_ReadU32(BlobBytes, Offset, MipCount)
		|| EncodingVersion != 1 || (Flags & ~1u) != 0
		|| PaletteColors != 256 || MipCount == 0
		|| MipCount > WoTPayload_MaxTextureMips
		|| MipCount != static_cast<uint32>(Entry->Mips.Num())
		|| static_cast<bool>(Flags & 1u) != Entry->bMasked)
	{
		Reject(TEXT("texture header disagrees with version-1 manifest metadata"));
		return nullptr;
	}

	uint64 PaletteBytes = 0;
	uint64 PaletteEnd = 0;
	if (!WoTPayload_CheckedMultiply(PaletteColors, 4, PaletteBytes)
		|| !WoTPayload_CheckedAdd(Offset, PaletteBytes, PaletteEnd)
		|| PaletteEnd > static_cast<uint64>(BlobBytes.Num()))
	{
		Reject(TEXT("texture palette is truncated"));
		return nullptr;
	}

	// Validate the complete base-derived mip chain, exact byte sizes, manifest
	// agreement, and blob ranges before sizing palette/mip/output buffers.
	Offset = PaletteEnd;
	for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Stored = 0;
		if (!WoTPayload_ReadU32(BlobBytes, Offset, Width)
			|| !WoTPayload_ReadU32(BlobBytes, Offset, Height)
			|| !WoTPayload_ReadU32(BlobBytes, Offset, Stored))
		{
			Reject(FString::Printf(TEXT("texture mip %u header is truncated"), MipIndex));
			return nullptr;
		}
		const FWoTTexturePayloadMip& ManifestMip = Entry->Mips[MipIndex];
		uint64 DataEnd = 0;
		if (!WoTPayload_ValidateTextureMipLayout(
				Entry->Mips[0].Width, Entry->Mips[0].Height, MipIndex,
				Width, Height, Stored, ManifestMip.ExpandedByteLength)
			|| Width != ManifestMip.Width || Height != ManifestMip.Height
			|| Stored != ManifestMip.StoredByteLength
			|| !WoTPayload_CheckedAdd(Offset, Stored, DataEnd)
			|| DataEnd > static_cast<uint64>(BlobBytes.Num()))
		{
			Reject(FString::Printf(
				TEXT("texture mip %u is invalid, truncated, or disagrees with the manifest"),
				MipIndex));
			return nullptr;
		}
		Offset = DataEnd;
	}
	if (Offset != static_cast<uint64>(BlobBytes.Num()))
	{
		Reject(TEXT("texture blob has trailing structural bytes"));
		return nullptr;
	}

	TSharedRef<FWoTTexturePayloadData> Payload = MakeShared<FWoTTexturePayloadData>();
	Payload->bMasked = Entry->bMasked;
	Payload->Format = 0;
	Payload->AnimNext = Entry->AnimNext;
	Payload->MinFrameRate = Entry->MinFrameRate;
	Payload->MaxFrameRate = Entry->MaxFrameRate;
	Payload->PrimeCount = Entry->PrimeCount;
	Payload->SourcePackage = AuthenticatedSourcePackage;
	Payload->SourceExport = AuthenticatedSourceExport;
	Payload->SourceSha256 = AuthenticatedSourceSha;
	Payload->Palette.Reserve(256);
	Offset = 8 + 2 + 2 + 4 + 4;
	for (uint32 PaletteIndex = 0; PaletteIndex < PaletteColors; ++PaletteIndex)
	{
		const int32 ColorOffset = static_cast<int32>(Offset + PaletteIndex * 4ull);
		Payload->Palette.Emplace(
			BlobBytes[ColorOffset + 0], BlobBytes[ColorOffset + 1],
			BlobBytes[ColorOffset + 2], BlobBytes[ColorOffset + 3]);
	}
	Offset = PaletteEnd;
	Payload->Mips.Reserve(static_cast<int32>(MipCount));
	for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Stored = 0;
		verify(WoTPayload_ReadU32(BlobBytes, Offset, Width)
			&& WoTPayload_ReadU32(BlobBytes, Offset, Height)
			&& WoTPayload_ReadU32(BlobBytes, Offset, Stored));
		FWoTTexturePayloadMip ParsedMip = Entry->Mips[MipIndex];
		ParsedMip.DataOffset = Offset;
		Payload->Mips.Add(ParsedMip);
		Offset += Stored;
	}
	// A retail-family build reads textures before bPayloadActivated is set, and
	// the cached entry is what every later consumer (the front-end scene) gets:
	// it must own its verified bytes, not just the mip layout.
	if (bPayloadActivated || bActivationRead)
	{
		Payload->VerifiedObject.Emplace(MoveTemp(VerifiedObject));
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		Payload->BlobBytes = MoveTemp(DeveloperBlobBytes);
	}
#endif
	TSharedPtr<const FWoTTexturePayloadData> Result = Payload;
	LoadedTexturePayloads.Add(CacheKey, Result);
	return Result;
}

TSharedPtr<const FWoTSoundPayloadData> UWoTPayloadSubsystem::LoadSoundPayload(
	const FString& CacheKey)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread())
	{
		return nullptr;
	}
	if (const TSharedPtr<const FWoTSoundPayloadData>* Existing =
		LoadedSoundPayloads.Find(CacheKey))
	{
		return *Existing;
	}
	if (RejectedBlobKeys.Contains(CacheKey))
	{
		return nullptr;
	}
	const FWoTSoundPayloadEntry* Entry = nullptr;
	FWoTSoundPayloadEntry ActivatedEntry;
	FWoTRuntimeVerifiedObject VerifiedObject;
#if !WOT_GUARDED_BOOTSTRAP
	TArray<uint8> DeveloperBlobBytes;
#endif
	TConstArrayView<uint8> BlobBytes;
	TArray<FWoTSoundPayloadData::FLoop> HeaderLoops;

	auto Reject = [this, &CacheKey](const FString& Reason)
	{
		RejectedBlobKeys.Add(CacheKey);
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting sound payload %s: %s"), *CacheKey, *Reason);
	};

	if (bPayloadActivated)
	{
	if (!CacheKey.StartsWith(
			TEXT("cache/sound/"), ESearchCase::CaseSensitive))
	{
		Reject(TEXT("guarded sound carrier does not contain an exact logical id"));
		return nullptr;
	}
	const FWoTNeutralResourceId Resource{
		CacheKey, EWoTNeutralResourceKind::Sound};
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> ObjectRead =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, WoTPayload_MaxBlobSize, TEXT("wav-v1"), TEXT("sound"));
	if (ObjectRead.IsError())
	{
		Reject(MoveTemp(ObjectRead).GetError().ToString());
		return nullptr;
	}
	VerifiedObject = MoveTemp(ObjectRead).GetValue();
	const FWoTCanonicalJsonValue& ObjectHeader = VerifiedObject.Header();
	static const TCHAR* SoundHeaderFields[] = {
		TEXT("audio"), TEXT("decoded_layout"), TEXT("duration"),
		TEXT("encoding"), TEXT("fact_id"), TEXT("importer"),
		TEXT("importer_version"), TEXT("kind"), TEXT("loops"),
		TEXT("payload_byte_length"), TEXT("source")
	};
	static const TCHAR* AudioFields[] = {
		TEXT("bits_per_sample"), TEXT("channels"),
		TEXT("format_tag"), TEXT("sample_rate")
	};
	static const TCHAR* LayoutFields[] = {
		TEXT("data_byte_length"), TEXT("frame_count"), TEXT("interleaved")
	};
	static const TCHAR* DurationFields[] = {
		TEXT("frames"), TEXT("sample_rate")
	};
	static const TCHAR* LoopFields[] = {
		TEXT("end_frame_exclusive"), TEXT("play_count"),
		TEXT("start_frame")
	};
	static const TCHAR* SourceFields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	const FWoTCanonicalJsonValue* Audio = nullptr;
	const FWoTCanonicalJsonValue* Layout = nullptr;
	const FWoTCanonicalJsonValue* Duration = nullptr;
	const FWoTCanonicalJsonValue* Source = nullptr;
	const FWoTCanonicalJsonValue* Loops =
		ObjectHeader.FindMember(TEXT("loops"));
	FString FactId;
	FString Importer;
	FString SourceExport;
	FString SourcePackage;
	FString SourceHash;
	uint32 ImporterVersion = 0;
	uint32 FormatTagFromHeader = 0;
	uint32 ChannelsFromHeader = 0;
	uint32 SampleRateFromHeader = 0;
	uint32 BitsFromHeader = 0;
	uint64 FramesFromHeader = 0;
	uint64 DataBytesFromHeader = 0;
	uint64 DurationFrames = 0;
	uint32 DurationRate = 0;
	bool bInterleaved = false;
	ActivatedEntry.CacheKey = CacheKey;
	TOptional<FWoTBootstrapError> HeaderError = RequireExactMembers(
		ObjectHeader, MakeArrayView(SoundHeaderFields),
		TEXT("sound object header"));
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("audio"), TEXT("sound object header"), Audio);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("decoded_layout"),
			TEXT("sound object header"), Layout);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("duration"),
			TEXT("sound object header"), Duration);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadObjectField(
			ObjectHeader, TEXT("source"),
			TEXT("sound object header"), Source);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			ObjectHeader, TEXT("fact_id"),
			TEXT("sound object header"), FactId);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadStringField(
			ObjectHeader, TEXT("importer"),
			TEXT("sound object header"), Importer);
	}
	if (!HeaderError.IsSet())
	{
		HeaderError = ReadUnsigned32Field(
			ObjectHeader, TEXT("importer_version"),
			TEXT("sound object header"), ImporterVersion);
	}
	if (!HeaderError.IsSet() && Audio != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Audio, MakeArrayView(AudioFields), TEXT("sound audio"));
	}
	if (!HeaderError.IsSet() && Audio != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Audio, TEXT("bits_per_sample"),
			TEXT("sound audio"), BitsFromHeader);
	}
	if (!HeaderError.IsSet() && Audio != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Audio, TEXT("channels"), TEXT("sound audio"), ChannelsFromHeader);
	}
	if (!HeaderError.IsSet() && Audio != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Audio, TEXT("format_tag"),
			TEXT("sound audio"), FormatTagFromHeader);
	}
	if (!HeaderError.IsSet() && Audio != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Audio, TEXT("sample_rate"),
			TEXT("sound audio"), SampleRateFromHeader);
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Layout, MakeArrayView(LayoutFields), TEXT("sound layout"));
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = ReadUnsignedField(
			*Layout, TEXT("data_byte_length"),
			TEXT("sound layout"), DataBytesFromHeader);
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = ReadUnsignedField(
			*Layout, TEXT("frame_count"),
			TEXT("sound layout"), FramesFromHeader);
	}
	if (!HeaderError.IsSet() && Layout != nullptr)
	{
		HeaderError = ReadBoolField(
			*Layout, TEXT("interleaved"),
			TEXT("sound layout"), bInterleaved);
	}
	if (!HeaderError.IsSet() && Duration != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Duration, MakeArrayView(DurationFields), TEXT("sound duration"));
	}
	if (!HeaderError.IsSet() && Duration != nullptr)
	{
		HeaderError = ReadUnsignedField(
			*Duration, TEXT("frames"),
			TEXT("sound duration"), DurationFrames);
	}
	if (!HeaderError.IsSet() && Duration != nullptr)
	{
		HeaderError = ReadUnsigned32Field(
			*Duration, TEXT("sample_rate"),
			TEXT("sound duration"), DurationRate);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = RequireExactMembers(
			*Source, MakeArrayView(SourceFields), TEXT("sound source"));
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("export"), TEXT("sound source"), SourceExport);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("package"), TEXT("sound source"), SourcePackage);
	}
	if (!HeaderError.IsSet() && Source != nullptr)
	{
		HeaderError = ReadStringField(
			*Source, TEXT("sha256"), TEXT("sound source"), SourceHash);
	}
	if (HeaderError.IsSet() || FactId.IsEmpty()
		|| Importer != TEXT("sound-v1") || ImporterVersion != 1
		|| FormatTagFromHeader != 1
		|| ChannelsFromHeader == 0 || ChannelsFromHeader > 8
		|| SampleRateFromHeader == 0 || SampleRateFromHeader > 384000
		|| (BitsFromHeader != 8 && BitsFromHeader != 16)
		|| !bInterleaved || FramesFromHeader != DurationFrames
		|| SampleRateFromHeader != DurationRate
		|| Loops == nullptr || !Loops->IsArray() || Loops->Num() > 65536
		|| SourcePackage.IsEmpty() || SourceHash.Len() != 64)
	{
		Reject(TEXT("sound canonical header is malformed"));
		return nullptr;
	}
	for (int32 LoopIndex = 0; LoopIndex < Loops->Num(); ++LoopIndex)
	{
		const FWoTCanonicalJsonValue& Loop = Loops->GetElement(LoopIndex);
		uint64 Start = 0;
		uint64 End = 0;
		uint32 PlayCount = 0;
		if (RequireExactMembers(
				Loop, MakeArrayView(LoopFields), TEXT("sound loop")).IsSet()
			|| ReadUnsignedField(
				Loop, TEXT("start_frame"), TEXT("sound loop"), Start).IsSet()
			|| ReadUnsignedField(
				Loop, TEXT("end_frame_exclusive"),
				TEXT("sound loop"), End).IsSet()
			|| ReadUnsigned32Field(
				Loop, TEXT("play_count"),
				TEXT("sound loop"), PlayCount).IsSet()
			|| Start >= End || End > FramesFromHeader)
		{
			Reject(TEXT("sound loop canonical header is malformed"));
			return nullptr;
		}
		FWoTSoundPayloadData::FLoop& Parsed = HeaderLoops.AddDefaulted_GetRef();
		Parsed.StartFrame = Start;
		Parsed.EndFrameExclusive = End;
		Parsed.PlayCount = PlayCount;
	}
	ActivatedEntry.PCMFormatTag = static_cast<uint16>(FormatTagFromHeader);
	ActivatedEntry.Channels = static_cast<uint16>(ChannelsFromHeader);
	ActivatedEntry.SampleRate = SampleRateFromHeader;
	ActivatedEntry.BitsPerSample = static_cast<uint16>(BitsFromHeader);
	ActivatedEntry.FrameCount = FramesFromHeader;
	ActivatedEntry.DataByteLength = DataBytesFromHeader;
	Entry = &ActivatedEntry;
	BlobBytes = VerifiedObject.Payload();
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		Entry = SoundEntries.Find(CacheKey);
		if (!Entry)
		{
			return nullptr;
		}
		FString BlobReadError;
		const FString BlobFilename =
			FPaths::Combine(CacheDirectory, Entry->BlobPath);
		if (!WoTPayload_ReadFileBounded(
				BlobFilename, WoTPayload_MaxBlobSize,
				TOptional<uint64>(Entry->BlobSize),
				DeveloperBlobBytes, BlobReadError))
		{
			Reject(BlobReadError);
			return nullptr;
		}
		if (WoTPayload_SHA256(
				DeveloperBlobBytes.GetData(),
				static_cast<uint32>(DeveloperBlobBytes.Num()))
			!= Entry->BlobSHA256)
		{
			Reject(TEXT("blob SHA-256 does not match the developer manifest"));
			return nullptr;
		}
		BlobBytes = DeveloperBlobBytes;
	}
#endif
	if (Entry == nullptr)
	{
		return nullptr;
	}

	if (BlobBytes.Num() < 12
		|| FMemory::Memcmp(BlobBytes.GetData(), "RIFF", 4) != 0
		|| FMemory::Memcmp(BlobBytes.GetData() + 8, "WAVE", 4) != 0)
	{
		Reject(TEXT("invalid WAV RIFF header"));
		return nullptr;
	}
	uint64 RiffSizeOffset = 4;
	uint32 RiffSize = 0;
	if (!WoTPayload_ReadU32(BlobBytes, RiffSizeOffset, RiffSize)
		|| static_cast<uint64>(RiffSize) + 8 != static_cast<uint64>(BlobBytes.Num()))
	{
		Reject(TEXT("WAV RIFF length disagrees with the authenticated blob size"));
		return nullptr;
	}

	bool bHasFormat = false;
	bool bHasData = false;
	uint16 FormatTag = 0;
	uint16 Channels = 0;
	uint32 SampleRate = 0;
	uint32 ByteRate = 0;
	uint16 BlockAlign = 0;
	uint16 BitsPerSample = 0;
	uint64 DataOffset = 0;
	uint64 DataLength = 0;
	uint64 Offset = 12;
	while (Offset < static_cast<uint64>(BlobBytes.Num()))
	{
		uint64 ChunkHeaderEnd = 0;
		if (!WoTPayload_CheckedAdd(Offset, 8, ChunkHeaderEnd)
			|| ChunkHeaderEnd > static_cast<uint64>(BlobBytes.Num()))
		{
			Reject(TEXT("truncated WAV chunk header"));
			return nullptr;
		}
		const uint8* ChunkID = BlobBytes.GetData() + static_cast<int32>(Offset);
		uint64 ChunkSizeOffset = Offset + 4;
		uint32 ChunkSize = 0;
		uint64 ChunkEnd = 0;
		uint64 PaddedEnd = 0;
		if (!WoTPayload_ReadU32(BlobBytes, ChunkSizeOffset, ChunkSize)
			|| !WoTPayload_CheckedAdd(ChunkHeaderEnd, ChunkSize, ChunkEnd)
			|| !WoTPayload_CheckedAdd(ChunkEnd, ChunkSize & 1u, PaddedEnd)
			|| PaddedEnd > static_cast<uint64>(BlobBytes.Num()))
		{
			Reject(TEXT("truncated WAV chunk payload"));
			return nullptr;
		}

		if (FMemory::Memcmp(ChunkID, "fmt ", 4) == 0)
		{
			if (bHasFormat || ChunkSize < 16)
			{
				Reject(TEXT("WAV has a duplicate or short fmt chunk"));
				return nullptr;
			}
			uint64 FormatOffset = ChunkHeaderEnd;
			if (!WoTPayload_ReadU16(BlobBytes, FormatOffset, FormatTag)
				|| !WoTPayload_ReadU16(BlobBytes, FormatOffset, Channels)
				|| !WoTPayload_ReadU32(BlobBytes, FormatOffset, SampleRate)
				|| !WoTPayload_ReadU32(BlobBytes, FormatOffset, ByteRate)
				|| !WoTPayload_ReadU16(BlobBytes, FormatOffset, BlockAlign)
				|| !WoTPayload_ReadU16(BlobBytes, FormatOffset, BitsPerSample))
			{
				Reject(TEXT("WAV fmt chunk is truncated"));
				return nullptr;
			}
			bHasFormat = true;
		}
		else if (FMemory::Memcmp(ChunkID, "data", 4) == 0)
		{
			if (bHasData)
			{
				Reject(TEXT("WAV has duplicate data chunks"));
				return nullptr;
			}
			bHasData = true;
			DataOffset = ChunkHeaderEnd;
			DataLength = ChunkSize;
		}
		Offset = PaddedEnd;
	}

	uint64 ExpectedBlockAlign = 0;
	uint64 ExpectedByteRate = 0;
	uint64 FrameCount = 0;
	if (!bHasFormat || !bHasData || FormatTag != 1
		|| Channels < 1 || Channels > 8
		|| SampleRate < 1 || SampleRate > 384000
		|| (BitsPerSample != 8 && BitsPerSample != 16)
		|| !WoTPayload_CheckedMultiply(Channels, BitsPerSample / 8,
			ExpectedBlockAlign)
		|| BlockAlign != ExpectedBlockAlign
		|| !WoTPayload_CheckedMultiply(SampleRate, BlockAlign, ExpectedByteRate)
		|| ByteRate != ExpectedByteRate
		|| DataLength % BlockAlign != 0)
	{
		Reject(TEXT("WAV PCM structure is invalid or unsupported"));
		return nullptr;
	}
	FrameCount = DataLength / BlockAlign;
	if (FormatTag != Entry->PCMFormatTag || Channels != Entry->Channels
		|| SampleRate != Entry->SampleRate
		|| BitsPerSample != Entry->BitsPerSample
		|| FrameCount != Entry->FrameCount
		|| DataLength != Entry->DataByteLength)
	{
		Reject(TEXT("parsed WAV PCM metadata disagrees with the manifest"));
		return nullptr;
	}

	TSharedRef<FWoTSoundPayloadData> Payload = MakeShared<FWoTSoundPayloadData>();
	Payload->Channels = Channels;
	Payload->SampleRate = SampleRate;
	Payload->BitsPerSample = BitsPerSample;
	Payload->FrameCount = FrameCount;
	Payload->Loops = MoveTemp(HeaderLoops);
	Payload->PCMBytes.Append(
		BlobBytes.GetData() + static_cast<int32>(DataOffset),
		static_cast<int32>(DataLength));
	TSharedPtr<const FWoTSoundPayloadData> Result = Payload;
	LoadedSoundPayloads.Add(CacheKey, Result);
	return Result;
}

TSharedPtr<const FWoTMeshPayloadData> UWoTPayloadSubsystem::LoadMeshPayload(
	const FString& CacheKey)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread())
	{
		return nullptr;
	}
	if (const TSharedPtr<const FWoTMeshPayloadData>* Existing =
		LoadedMeshPayloads.Find(CacheKey))
	{
		return *Existing;
	}
	if (RejectedBlobKeys.Contains(CacheKey))
	{
		return nullptr;
	}
	const FWoTMeshPayloadEntry* Entry = MeshEntries.Find(CacheKey);
	if (!Entry)
	{
		return nullptr;
	}

	auto Reject = [this, &CacheKey](const FString& Reason)
	{
		RejectedBlobKeys.Add(CacheKey);
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting mesh payload %s: %s"), *CacheKey, *Reason);
	};
	FString ReadError;
	FWoTImmutableVerifiedBlob VerifiedBlob;
#if !WOT_GUARDED_BOOTSTRAP
	TArray<uint8> DeveloperBlobBytes;
#endif
	TConstArrayView<uint8> BlobBytes;
	if (bPayloadActivated)
	{
		FWoTCacheBlobDescriptor Descriptor;
		if (!ActiveCacheLease.IsValid()
			|| !WoTPayload_MakeVerifiedDescriptor(
				CacheKey, TEXT("mesh"), TEXT("wot-mesh-v3"),
				Entry->BlobSize, Entry->BlobSHA256, Descriptor, ReadError))
		{
			Reject(ReadError);
			return nullptr;
		}
		FWoTCacheBlobReadLimits ReadLimits;
		ReadLimits.MaximumBlobBytes = WoTPayload_MaxBlobSize;
		ReadLimits.MaximumAllocationBytes = WoTPayload_MaxBlobSize;
		TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Read =
			ReadVerifiedCacheBlob(*ActiveCacheLease, Descriptor, ReadLimits);
		if (Read.IsError())
		{
			Reject(MoveTemp(Read).GetError().ToString());
			return nullptr;
		}
		VerifiedBlob = MoveTemp(Read).GetValue();
		BlobBytes = VerifiedBlob.Bytes();
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		if (!WoTPayload_ReadFileBounded(
				FPaths::Combine(CacheDirectory, Entry->BlobPath),
				WoTPayload_MaxBlobSize, TOptional<uint64>(Entry->BlobSize),
				DeveloperBlobBytes, ReadError))
		{
			Reject(ReadError);
			return nullptr;
		}
		if (WoTPayload_SHA256(
				DeveloperBlobBytes.GetData(),
				static_cast<uint32>(DeveloperBlobBytes.Num()))
			!= Entry->BlobSHA256)
		{
			Reject(TEXT("blob SHA-256 does not match the developer manifest"));
			return nullptr;
		}
		BlobBytes = DeveloperBlobBytes;
	}
#endif
	const uint8 ExpectedMagic[8] = {'W', 'O', 'T', 'P', 'M', 'E', 'S', 'H'};
	if (BlobBytes.Num() < 24
		|| FMemory::Memcmp(BlobBytes.GetData(), ExpectedMagic, 8) != 0)
	{
		Reject(TEXT("truncated or invalid mesh magic"));
		return nullptr;
	}
	uint64 Offset = 8;
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 FrameVertices = 0;
	uint32 Frames = 0;
	uint32 PackedCount = 0;
	uint64 PackedBytes = 0;
	uint64 ExpectedSize = 0;
	if (!WoTPayload_ReadU16(BlobBytes, Offset, Version)
		|| !WoTPayload_ReadU16(BlobBytes, Offset, Flags)
		|| !WoTPayload_ReadU32(BlobBytes, Offset, FrameVertices)
		|| !WoTPayload_ReadU32(BlobBytes, Offset, Frames)
		|| !WoTPayload_ReadU32(BlobBytes, Offset, PackedCount)
		|| Version != 1 || Flags != 0 || FrameVertices == 0 || Frames == 0
		|| FrameVertices != Entry->FrameVertexCount
		|| Frames != Entry->AnimationFrameCount
		|| PackedCount != Entry->PackedVertexCount
		|| !WoTPayload_CheckedMultiply(PackedCount, 4, PackedBytes)
		|| !WoTPayload_CheckedAdd(24, PackedBytes, ExpectedSize)
		|| ExpectedSize != static_cast<uint64>(BlobBytes.Num())
		|| PackedBytes != static_cast<uint64>(Entry->FrameByteLength) * Frames
		|| PackedCount > static_cast<uint32>(MAX_int32))
	{
		Reject(TEXT("mesh header/counts disagree with authenticated manifest metadata"));
		return nullptr;
	}

	TSharedRef<FWoTMeshPayloadData> Payload = MakeShared<FWoTMeshPayloadData>();
	Payload->FrameVertexCount = FrameVertices;
	Payload->AnimationFrameCount = Frames;
	Payload->PackedVertices.Reserve(static_cast<int32>(PackedCount));
	for (uint32 Index = 0; Index < PackedCount; ++Index)
	{
		uint32 Packed = 0;
		if (!WoTPayload_ReadU32(BlobBytes, Offset, Packed))
		{
			Reject(TEXT("mesh packed-vertex array is truncated"));
			return nullptr;
		}
		Payload->PackedVertices.Add(Packed);
	}
	if (Offset != static_cast<uint64>(BlobBytes.Num()))
	{
		Reject(TEXT("mesh blob has trailing structural bytes"));
		return nullptr;
	}
	TSharedPtr<const FWoTMeshPayloadData> Result = Payload;
	LoadedMeshPayloads.Add(CacheKey, Result);
	return Result;
}

TSharedPtr<const FWoTBSPPayloadData> UWoTPayloadSubsystem::LoadBSPPayload(
	const FString& CacheKey)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread()) return nullptr;
	if (const TSharedPtr<const FWoTBSPPayloadData>* Existing =
		LoadedBSPPayloads.Find(CacheKey)) return *Existing;
	if (RejectedBlobKeys.Contains(CacheKey)) return nullptr;
	const FWoTBSPPayloadEntry* Entry = BSPEntries.Find(CacheKey);
	if (!Entry) return nullptr;
	auto Reject = [this, &CacheKey](const FString& Reason)
	{
		RejectedBlobKeys.Add(CacheKey);
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTPayload, Error,
			TEXT("Rejecting required BSP payload %s: %s"), *CacheKey, *Reason);
	};
	FString Error;
	FWoTImmutableVerifiedBlob VerifiedBlob;
#if !WOT_GUARDED_BOOTSTRAP
	TArray<uint8> DeveloperBytes;
#endif
	TConstArrayView<uint8> Bytes;
	if (bPayloadActivated)
	{
		FWoTCacheBlobDescriptor Descriptor;
		if (!ActiveCacheLease.IsValid()
			|| !WoTPayload_MakeVerifiedDescriptor(
				CacheKey, TEXT("bsp"), TEXT("wot-bsp-v3"),
				Entry->BlobSize, Entry->BlobSHA256, Descriptor, Error))
		{
			Reject(Error);
			return nullptr;
		}
		FWoTCacheBlobReadLimits ReadLimits;
		ReadLimits.MaximumBlobBytes = WoTPayload_MaxBlobSize;
		ReadLimits.MaximumAllocationBytes = WoTPayload_MaxBlobSize;
		TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Read =
			ReadVerifiedCacheBlob(*ActiveCacheLease, Descriptor, ReadLimits);
		if (Read.IsError())
		{
			Reject(MoveTemp(Read).GetError().ToString());
			return nullptr;
		}
		VerifiedBlob = MoveTemp(Read).GetValue();
		Bytes = VerifiedBlob.Bytes();
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		if (!WoTPayload_ReadFileBounded(
				FPaths::Combine(CacheDirectory, Entry->BlobPath),
				WoTPayload_MaxBlobSize, TOptional<uint64>(Entry->BlobSize),
				DeveloperBytes, Error))
		{
			Reject(Error);
			return nullptr;
		}
		if (WoTPayload_SHA256(
				DeveloperBytes.GetData(),
				static_cast<uint32>(DeveloperBytes.Num()))
			!= Entry->BlobSHA256)
		{
			Reject(TEXT("blob SHA-256 does not match the developer manifest"));
			return nullptr;
		}
		Bytes = DeveloperBytes;
	}
#endif
	if (Bytes.IsEmpty() || !Bytes.GetData())
	{
		Reject(TEXT("BSP blob is empty despite its manifest size"));
		return nullptr;
	}
	const int32 ManifestSectionCount = static_cast<int32>(Entry->SectionCount);
	if (Entry->SectionCount == 0
		|| Entry->SectionCount > WoTPayload_MaxBSPSections
		|| Entry->MaterialSlots.Num() != ManifestSectionCount
		|| Entry->SectionFirstIndices.Num() != ManifestSectionCount
		|| Entry->SectionIndexCounts.Num() != ManifestSectionCount
		|| Entry->SectionMinVertices.Num() != ManifestSectionCount
		|| Entry->SectionMaxVertices.Num() != ManifestSectionCount
		|| Entry->SectionCollisionEnabled.Num() != ManifestSectionCount)
	{
		Reject(TEXT("BSP manifest section arrays are empty or not equally dense"));
		return nullptr;
	}
	TSharedRef<FWoTBSPPayloadData> Payload = MakeShared<FWoTBSPPayloadData>();
	if (!UWoTBSPPayloadLibrary::DecodePayload(Bytes, *Payload, Error)
		|| Payload->bPortal != (Entry->Role == TEXT("portal"))
		|| Payload->Positions.Num() != static_cast<int32>(Entry->VertexCount)
		|| Payload->Indices.Num() != static_cast<int32>(Entry->IndexCount)
		|| Payload->Sections.Num() != static_cast<int32>(Entry->SectionCount)
		|| Payload->BoundsMin != Entry->BoundsMin
		|| Payload->BoundsMax != Entry->BoundsMax)
	{
		Reject(Error.IsEmpty()
			? TEXT("BSP blob disagrees with authenticated manifest metadata") : Error);
		return nullptr;
	}
	for (int32 SectionIndex = 0; SectionIndex < Payload->Sections.Num(); ++SectionIndex)
	{
		if (!Payload->Sections.IsValidIndex(SectionIndex)
			|| !Entry->MaterialSlots.IsValidIndex(SectionIndex)
			|| !Entry->SectionFirstIndices.IsValidIndex(SectionIndex)
			|| !Entry->SectionIndexCounts.IsValidIndex(SectionIndex)
			|| !Entry->SectionMinVertices.IsValidIndex(SectionIndex)
			|| !Entry->SectionMaxVertices.IsValidIndex(SectionIndex)
			|| !Entry->SectionCollisionEnabled.IsValidIndex(SectionIndex))
		{
			Reject(FString::Printf(
				TEXT("BSP section %d is missing parallel manifest metadata"), SectionIndex));
			return nullptr;
		}
		const FWoTBSPPayloadSection& Section = Payload->Sections[SectionIndex];
		if (Section.MaterialSlot != Entry->MaterialSlots[SectionIndex]
			|| Section.FirstIndex != Entry->SectionFirstIndices[SectionIndex]
			|| Section.IndexCount != Entry->SectionIndexCounts[SectionIndex]
			|| Section.MinVertex != Entry->SectionMinVertices[SectionIndex]
			|| Section.MaxVertex != Entry->SectionMaxVertices[SectionIndex]
			|| Section.bCollisionEnabled != Entry->SectionCollisionEnabled[SectionIndex])
		{
			Reject(FString::Printf(
				TEXT("BSP section %d disagrees with manifest"), SectionIndex));
			return nullptr;
		}
	}
	TSharedPtr<const FWoTBSPPayloadData> Result = Payload;
	LoadedBSPPayloads.Add(CacheKey, Result);
	return Result;
}

bool UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
	const FWoTMeshPayloadData& Payload, const UWoTMeshPayloadLayout& Layout,
	USkeletalMesh* Mesh, FWoTMeshPayloadCPUData& OutData, FString& OutError)
{
	OutData = FWoTMeshPayloadCPUData();
	if (!Layout.ValidateDescriptor(OutError)
		|| Payload.FrameVertexCount != static_cast<uint32>(Layout.FrameVertexCount)
		|| Payload.AnimationFrameCount != static_cast<uint32>(Layout.AnimationFrameCount))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("mesh payload counts disagree with the destination layout");
		}
		return false;
	}
	uint64 ExpectedPacked = 0;
	uint64 ExpandedDeltaBytes = 0;
	if (!WoTPayload_CheckedMultiply(Payload.FrameVertexCount,
			Payload.AnimationFrameCount, ExpectedPacked)
		|| ExpectedPacked != static_cast<uint64>(Payload.PackedVertices.Num())
		|| Layout.RenderVertexToUE1Vertex.Num() > MAX_int32
		|| !WoTPayload_CheckedMultiply(Layout.RenderVertexToUE1Vertex.Num(),
			Payload.AnimationFrameCount, ExpandedDeltaBytes)
		|| !WoTPayload_CheckedMultiply(ExpandedDeltaBytes,
			sizeof(FWoTMeshPayloadDelta), ExpandedDeltaBytes)
		|| ExpandedDeltaBytes > WoTPayload_MaxTotalExpandedBytes)
	{
		OutError = TEXT("mesh packed or render-expanded delta counts exceed the trusted bounds");
		return false;
	}
	const FSkeletalMeshRenderData* RenderData = Mesh
		? Mesh->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
	{
		OutError = TEXT("destination skeletal mesh has no LOD-0 render data");
		return false;
	}
	const FPositionVertexBuffer& PositionBuffer =
		RenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer;
	if (PositionBuffer.GetNumVertices()
		!= static_cast<uint32>(Layout.RenderVertexToUE1Vertex.Num()))
	{
		OutError = TEXT("destination LOD-0 base position count disagrees with the verified render mapping");
		return false;
	}
	TArray<FVector3f> RenderBasePositions;
	RenderBasePositions.Reserve(Layout.RenderVertexToUE1Vertex.Num());
	for (uint32 RenderVertex = 0;
		RenderVertex < PositionBuffer.GetNumVertices(); ++RenderVertex)
	{
		RenderBasePositions.Add(PositionBuffer.VertexPosition(RenderVertex));
	}

	OutData.RenderVertexCount = Layout.RenderVertexToUE1Vertex.Num();
	OutData.Frames.Reserve(Layout.AnimationFrameCount);
	TArray<FVector3d> Positions;
	TArray<FVector3f> Normals;
	Positions.SetNumUninitialized(Layout.FrameVertexCount);
	for (int32 Frame = 0; Frame < Layout.AnimationFrameCount; ++Frame)
	{
		const int64 PackedBase = static_cast<int64>(Frame)
			* Layout.FrameVertexCount;
		for (int32 UE1Vertex = 0; UE1Vertex < Layout.FrameVertexCount; ++UE1Vertex)
		{
			Positions[UE1Vertex] = WoTPayload_TransformMeshVertex(
				Payload.PackedVertices[static_cast<int32>(PackedBase + UE1Vertex)],
				Layout);
		}
		if (!WoTPayload_ComputeSmoothNormals(
				Positions, Layout, Normals, OutError))
		{
			OutData = FWoTMeshPayloadCPUData();
			return false;
		}

		FWoTMeshPayloadFrame& OutputFrame = OutData.Frames.AddDefaulted_GetRef();
		OutputFrame.Deltas.SetNumUninitialized(OutData.RenderVertexCount);
		for (int32 RenderVertex = 0;
			RenderVertex < OutData.RenderVertexCount; ++RenderVertex)
		{
			const int32 UE1Vertex = Layout.RenderVertexToUE1Vertex[RenderVertex];
			FWoTMeshPayloadDelta& Delta = OutputFrame.Deltas[RenderVertex];
			Delta.PositionDelta = FVector3f(Positions[UE1Vertex])
				- RenderBasePositions[RenderVertex];
			Delta.TangentZDelta = Normals[UE1Vertex]
				- FVector3f(Layout.PinnedBaseTangentZ[RenderVertex]);
			Delta.SourceIndex = static_cast<uint32>(RenderVertex);
		}
	}
	return true;
}

bool UWoTPayloadSubsystem::ValidateStrippedMeshLayout(USkeletalMesh* Mesh,
	const UWoTMeshPayloadLayout& Layout, FString& OutError)
{
	if (!Mesh || !Layout.ValidateDescriptor(OutError))
	{
		if (OutError.IsEmpty()) OutError = TEXT("missing skeletal mesh");
		return false;
	}
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
	{
		OutError = TEXT("stripped skeletal mesh has no LOD-0 render data");
		return false;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	const FPositionVertexBuffer& Positions =
		LOD.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices =
		LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const FColorVertexBuffer& Colors =
		LOD.StaticVertexBuffers.ColorVertexBuffer;
	if (Positions.GetNumVertices() != static_cast<uint32>(
			Layout.RenderVertexToUE1Vertex.Num())
		|| Vertices.GetNumVertices() != Positions.GetNumVertices()
		|| Mesh->GetHasVertexColors() || Colors.GetNumVertices() != 0
		|| Vertices.GetNumTexCoords() != 1)
	{
		OutError = TEXT("cooked stripped mesh must match the layout count, contain only TEXCOORD_0, and have no vertex colors");
		return false;
	}
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
	{
		if (Section.bRecomputeTangent)
		{
			OutError = TEXT("cooked stripped mesh enables runtime tangent recomputation");
			return false;
		}
	}
	TArray<FVector> ActualTangents;
	ActualTangents.Reserve(Layout.PinnedBaseTangentZ.Num());
	for (uint32 RenderVertex = 0;
		RenderVertex < Positions.GetNumVertices(); ++RenderVertex)
	{
		const FVector3f Position = Positions.VertexPosition(RenderVertex);
		const FVector4f PackedTangent = Vertices.VertexTangentZ(RenderVertex);
		const FVector Tangent(
			PackedTangent.X, PackedTangent.Y, PackedTangent.Z);
		if (!WoTPayload_IsMeshLatticePosition(Position)
			|| !Tangent.Equals(Layout.PinnedBaseTangentZ[RenderVertex], 0.01))
		{
			OutError = FString::Printf(
				TEXT("cooked stripped render vertex %u disagrees with lattice-base/pinned-TangentZ contract"),
				RenderVertex);
			return false;
		}
		ActualTangents.Add(Tangent);
	}
	if (UWoTMeshPayloadLayout::HashTangentBasis(ActualTangents)
		!= Layout.PinnedBaseTangentZSHA256)
	{
		OutError = TEXT("cooked stripped base TangentZ hash disagrees with the descriptor");
		return false;
	}
	return true;
}

TWoTBootstrapResult<TArray<UTexture2D*>>
UWoTPayloadSubsystem::PrepareTextureConsumers(
	TConstArrayView<FWoTTextureConsumerBinding> Requirements)
{
	using FResult = TWoTBootstrapResult<TArray<UTexture2D*>>;
	auto Error = [](FString Message)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation, MoveTemp(Message)));
	};
	if (!bInjectionEnabled || !bManifestReady || !bPayloadActivated
		|| !IsInGameThread() || Requirements.IsEmpty())
	{
		return Error(TEXT("authenticated texture consumers are unavailable"));
	}

	struct FPreparedConsumer
	{
		FWoTTextureConsumerBinding Binding;
		TSharedPtr<const FWoTTexturePayloadData> Payload;
		FString PackageName;
		FString ObjectName;
		UTexture2D* Existing = nullptr;
	};
	TArray<FPreparedConsumer> Prepared;
	Prepared.Reserve(Requirements.Num());
	TSet<FString> AssetPaths;
	TSet<FString> LogicalIds;
	for (const FWoTTextureConsumerBinding& Requirement : Requirements)
	{
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(Requirement.AssetPath);
		const FString ObjectName =
			FPackageName::ObjectPathToObjectName(Requirement.AssetPath);
		if (Requirement.AssetPath.IsEmpty() || Requirement.LogicalId.IsEmpty()
			|| Requirement.SourcePackage.IsEmpty()
			|| Requirement.SourceExport.IsEmpty()
			|| Requirement.Width <= 0 || Requirement.Height <= 0
			|| Requirement.MipCount <= 0
			|| !Requirement.LogicalId.StartsWith(
				TEXT("cache/texture/"), ESearchCase::CaseSensitive)
			|| !FPackageName::IsValidLongPackageName(PackageName)
			|| ObjectName.IsEmpty()
			|| Requirement.AssetPath != PackageName + TEXT(".") + ObjectName
			|| FPackageName::GetLongPackageAssetName(PackageName) != ObjectName
			|| AssetPaths.Contains(Requirement.AssetPath)
			|| LogicalIds.Contains(Requirement.LogicalId))
		{
			return Error(TEXT("texture consumer identity is invalid or duplicated"));
		}
		TWoTBootstrapResult<FWoTSha256> RequiredSourceSha =
			FWoTSha256::FromHex(Requirement.SourceSha256);
		if (RequiredSourceSha.IsError())
		{
			return Error(TEXT("texture consumer source digest is not canonical"));
		}
		TSharedPtr<const FWoTTexturePayloadData> Payload =
			LoadTexturePayload(Requirement.LogicalId);
		if (!Payload.IsValid() || Payload->Mips.IsEmpty()
			|| Payload->SourcePackage != Requirement.SourcePackage
			|| Payload->SourceExport != Requirement.SourceExport
			|| Payload->SourceSha256 != RequiredSourceSha.GetValue()
			|| Payload->Mips[0].Width != static_cast<uint32>(Requirement.Width)
			|| Payload->Mips[0].Height != static_cast<uint32>(Requirement.Height)
			|| Payload->Mips.Num() != Requirement.MipCount
			|| Payload->bMasked != Requirement.bMasked)
		{
			return Error(TEXT("texture consumer disagrees with authenticated source/header authority"));
		}

		UObject* ExistingObject = FindObject<UObject>(
			nullptr, *Requirement.AssetPath, EFindObjectFlags::None);
		UTexture2D* ExistingTexture = Cast<UTexture2D>(ExistingObject);
		const UWoTPayloadAssetUserData* ExistingBinding = ExistingTexture
			? Cast<UWoTPayloadAssetUserData>(ExistingTexture->GetAssetUserDataOfClass(
				UWoTPayloadAssetUserData::StaticClass())) : nullptr;
		if (ExistingObject != nullptr
			&& (!IsValid(ExistingTexture)
				|| !ExistingTexture->HasAllFlags(RF_Public | RF_Transient)
				|| ExistingTexture->HasAnyFlags(RF_ClassDefaultObject)
				|| !PreparedTextureConsumers.Contains(ExistingTexture)
				|| !InjectedTextures.Contains(FObjectKey(ExistingTexture))
				|| ExistingTexture->GetSizeX() != Requirement.Width
				|| ExistingTexture->GetSizeY() != Requirement.Height
				|| ExistingBinding == nullptr
				|| ExistingBinding->PayloadKey != Requirement.LogicalId
				|| ExistingBinding->DestinationMipCount != 1))
		{
			return Error(TEXT("texture consumer destination identity is already occupied"));
		}
		AssetPaths.Add(Requirement.AssetPath);
		LogicalIds.Add(Requirement.LogicalId);
		FPreparedConsumer& Consumer = Prepared.AddDefaulted_GetRef();
		Consumer.Binding = Requirement;
		Consumer.Payload = MoveTemp(Payload);
		Consumer.PackageName = PackageName;
		Consumer.ObjectName = ObjectName;
		Consumer.Existing = ExistingTexture;
	}

	TArray<UTexture2D*> Result;
	Result.Reserve(Prepared.Num());
	TArray<UTexture2D*> Created;
	auto RollbackCreated = [this, &Created]()
	{
		for (UTexture2D* Rollback : Created)
		{
			InjectedTextures.Remove(FObjectKey(Rollback));
			Rollback->ReleaseResource();
			Rollback->ClearFlags(RF_Public | RF_Standalone);
			const FName RollbackName = MakeUniqueObjectName(
				GetTransientPackage(), UTexture2D::StaticClass(),
				TEXT("WoTRolledBackTexture"));
			(void)Rollback->Rename(*RollbackName.ToString(),
				GetTransientPackage(), REN_DontCreateRedirectors
					| REN_NonTransactional | REN_DoNotDirty);
			Rollback->MarkAsGarbage();
		}
	};
	for (FPreparedConsumer& Consumer : Prepared)
	{
		if (Consumer.Existing != nullptr)
		{
			Result.Add(Consumer.Existing);
			continue;
		}
		UPackage* Package = CreatePackage(*Consumer.PackageName);
		if (Package == nullptr)
		{
			RollbackCreated();
			return Error(TEXT("texture consumer package creation failed"));
		}
		UTexture2D* Texture = NewObject<UTexture2D>(Package,
			*Consumer.ObjectName, RF_Public | RF_Transient);
		if (Texture == nullptr)
		{
			RollbackCreated();
			return Error(TEXT("texture consumer creation failed"));
		}
		Texture->SRGB = true;
		Texture->NeverStream = true;
#if WITH_EDITORONLY_DATA
		Texture->MipGenSettings = TMGS_NoMipmaps;
#endif
		FTexturePlatformData* PlatformData = new FTexturePlatformData();
		PlatformData->SizeX = Consumer.Binding.Width;
		PlatformData->SizeY = Consumer.Binding.Height;
		PlatformData->SetNumSlices(1);
		PlatformData->PixelFormat = PF_B8G8R8A8;
		FTexture2DMipMap* Mip = new FTexture2DMipMap(
			Consumer.Binding.Width, Consumer.Binding.Height, 1);
		PlatformData->Mips.Add(Mip);
		const int64 ByteCount = static_cast<int64>(Consumer.Binding.Width)
			* Consumer.Binding.Height * 4;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		uint8* Pixels = static_cast<uint8*>(Mip->BulkData.Realloc(ByteCount));
		for (int64 Offset = 0; Offset < ByteCount; Offset += 4)
		{
			Pixels[Offset + 0] = 255;
			Pixels[Offset + 1] = 0;
			Pixels[Offset + 2] = 255;
			Pixels[Offset + 3] = 255;
		}
		Mip->BulkData.Unlock();
		Texture->SetPlatformData(PlatformData);
		UWoTPayloadAssetUserData* Binding =
			NewObject<UWoTPayloadAssetUserData>(Texture);
		Binding->PayloadKey = Consumer.Binding.LogicalId;
		Binding->DestinationMipCount = 1;
		Texture->AddAssetUserData(Binding);
		Consumer.Existing = Texture;
		Created.Add(Texture);
		Result.Add(Texture);
	}
	for (int32 Index = 0; Index < Created.Num(); ++Index)
	{
		UTexture2D* Texture = Created[Index];
		const UWoTPayloadAssetUserData* Binding = Cast<UWoTPayloadAssetUserData>(
			Texture->GetAssetUserDataOfClass(
				UWoTPayloadAssetUserData::StaticClass()));
		bool bForcedFailure = false;
#if WITH_AUTOMATION_TESTS
		bForcedFailure =
			TextureConsumerInjectionFailureIndexForTests == Index;
		if (bForcedFailure)
		{
			TextureConsumerInjectionFailureIndexForTests = INDEX_NONE;
		}
#endif
		if (bForcedFailure || Binding == nullptr
			|| !InjectTexture(Texture, Binding->PayloadKey, 1))
		{
			RollbackCreated();
			return Error(TEXT("authenticated texture consumer injection failed"));
		}
	}
	for (UTexture2D* Texture : Created)
	{
		PreparedTextureConsumers.Add(Texture);
	}
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<TArray<UTexture2D*>>
UWoTPayloadSubsystem::PrepareFrontEndMenuTextures()
{
	static const FWoTTextureConsumerBinding Requirements[] = {
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
	return PrepareTextureConsumers(MakeArrayView(Requirements));
}

bool UWoTPayloadSubsystem::InjectTexture(UTexture2D* Texture,
	const FString& CacheKey, int32 DestinationMipCount)
{
	// Diagnostic-only: a refused texture injection renders BLACK in a
	// packaged Test build with every Warning compiled out (the
	// front-end menu, 2026-08-06). Record the first few refusals durably;
	// the cap keeps a systemic failure from flooding the heartbeat.
	static int32 RefusalBreadcrumbsLeft = 8;
	const auto RecordRefusal = [&CacheKey](const TCHAR* Reason)
	{
		if (RefusalBreadcrumbsLeft > 0)
		{
			--RefusalBreadcrumbsLeft;
			WoTRecordBootstrapHeartbeat(TEXT("texture-inject-refused"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("%s: %s"), *CacheKey, Reason));
		}
	};
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread()
		|| !IsValid(Texture) || Texture->HasAnyFlags(RF_ClassDefaultObject))
	{
		RecordRefusal(TEXT("injection disabled or manifest not ready"));
		return false;
	}
	if (InjectedTextures.Contains(FObjectKey(Texture)))
	{
		return true;
	}
	const TSharedPtr<const FWoTTexturePayloadData> Payload =
		LoadTexturePayload(CacheKey);
	if (!Payload.IsValid())
	{
		RecordRefusal(TEXT("payload blob load failed"));
		return false;
	}
	const int32 FullMipCount = WoTPayload_FullTextureMipCount(
		Payload->Mips[0].Width, Payload->Mips[0].Height);
	if (DestinationMipCount <= 0 || DestinationMipCount > FullMipCount)
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting texture binding %s -> %s: DestinationMipCount %d is outside the complete base-derived range [1,%d]"),
			*CacheKey, *Texture->GetPathName(), DestinationMipCount,
			FullMipCount);
		RecordRefusal(TEXT("destination mip count out of range"));
		return false;
	}

	const FTexturePlatformData* ExistingData = Texture->GetPlatformData();
	if (!ExistingData || ExistingData->SizeX != static_cast<int32>(Payload->Mips[0].Width)
		|| ExistingData->SizeY != static_cast<int32>(Payload->Mips[0].Height)
		|| ExistingData->Mips.Num() != DestinationMipCount)
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting texture binding %s -> %s: placeholder dimensions/mip count disagree with destination metadata"),
			*CacheKey, *Texture->GetPathName());
		RecordRefusal(TEXT("placeholder dimensions disagree"));
		return false;
	}
#if WITH_EDITORONLY_DATA
	if (Texture->MipGenSettings == TMGS_NoMipmaps && DestinationMipCount != 1)
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting no-mip texture binding %s -> %s with %d destination mips"),
			*CacheKey, *Texture->GetPathName(), DestinationMipCount);
		return false;
	}
#endif

	// Re-check every destination mip against the authenticated base dimensions
	// before allocating any expanded buffers. Cache mips below mip 0 are retail
	// data, while the canonical PNG importer gives UE only mip 0 and asks
	// TMGS_SimpleAverage to generate the destination chain. They are therefore
	// intentionally not spliced into this canonical-generated chain.
	for (int32 MipIndex = 0; MipIndex < DestinationMipCount; ++MipIndex)
	{
		const FTexture2DMipMap& DestinationMip = ExistingData->Mips[MipIndex];
		const uint64 Width = FMath::Max<uint64>(
			1, static_cast<uint64>(ExistingData->SizeX) >> MipIndex);
		const uint64 Height = FMath::Max<uint64>(
			1, static_cast<uint64>(ExistingData->SizeY) >> MipIndex);
		uint64 Stored = 0;
		uint64 Expanded = 0;
		const bool bLengthsValid = WoTPayload_CheckedMultiply(
			Width, Height, Stored)
			&& WoTPayload_CheckedMultiply(Stored, 4, Expanded);
		if (!bLengthsValid
			|| !WoTPayload_ValidateTextureMipLayout(
				static_cast<uint64>(ExistingData->SizeX),
				static_cast<uint64>(ExistingData->SizeY),
				static_cast<uint32>(MipIndex), Width, Height, Stored, Expanded)
			|| DestinationMip.SizeX != static_cast<int32>(Width)
			|| DestinationMip.SizeY != static_cast<int32>(Height))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting texture binding %s -> %s: destination mip %d breaks the base-derived layout"),
				*CacheKey, *Texture->GetPathName(), MipIndex);
			RecordRefusal(TEXT("destination mip layout broken"));
			return false;
		}
	}

	TArray<uint8> ExpandedBaseMip;
	FString ExpansionError;
	if (!WoTPayload_ExpandMip(
			*Payload, 0, ExpandedBaseMip, ExpansionError))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting texture binding %s -> %s: %s"), *CacheKey,
			*Texture->GetPathName(), *ExpansionError);
		RecordRefusal(TEXT("mip expansion failed"));
		return false;
	}
	TArray<TArray<uint8>> ExpandedMips;
	if (!WoTPayload_GenerateCanonicalMipChain(
			MoveTemp(ExpandedBaseMip), Payload->Mips[0].Width,
			Payload->Mips[0].Height, DestinationMipCount,
			Texture->SRGB != 0, Texture->CompressionSettings == TC_Normalmap,
			ExpandedMips, ExpansionError))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting texture binding %s -> %s: %s"), *CacheKey,
			*Texture->GetPathName(), *ExpansionError);
		RecordRefusal(TEXT("mip expansion failed"));
		return false;
	}

	TUniquePtr<FTexturePlatformData> NewData = MakeUnique<FTexturePlatformData>();
	NewData->SizeX = static_cast<int32>(Payload->Mips[0].Width);
	NewData->SizeY = static_cast<int32>(Payload->Mips[0].Height);
	NewData->SetNumSlices(1);
	NewData->PixelFormat = PF_B8G8R8A8;
	for (int32 MipIndex = 0; MipIndex < DestinationMipCount; ++MipIndex)
	{
		const uint32 Width = FMath::Max<uint32>(
			1, Payload->Mips[0].Width >> MipIndex);
		const uint32 Height = FMath::Max<uint32>(
			1, Payload->Mips[0].Height >> MipIndex);
		FTexture2DMipMap* DestinationMip = new FTexture2DMipMap(
			Width, Height, 1);
		NewData->Mips.Add(DestinationMip);
		DestinationMip->BulkData.Lock(LOCK_READ_WRITE);
		void* DestinationBytes = DestinationMip->BulkData.Realloc(
			static_cast<int64>(ExpandedMips[MipIndex].Num()));
		FMemory::Memcpy(DestinationBytes, ExpandedMips[MipIndex].GetData(),
			ExpandedMips[MipIndex].Num());
		DestinationMip->BulkData.Unlock();
	}

	// SetPlatformData releases the old initialized resource before deleting its
	// platform data. UpdateResource publishes the replacement through the normal
	// render-thread command path; direct RHI mutation is intentionally avoided.
	Texture->NeverStream = true;
	Texture->bNotOfflineProcessed = true;
	Texture->SetPlatformData(NewData.Release());
	Texture->UpdateResource();
	InjectedTextures.Add(FObjectKey(Texture));
	UE_LOG(LogWoTPayload, Verbose,
		TEXT("Injected %s into %s (%d destination mips)"),
		*CacheKey, *Texture->GetPathName(), DestinationMipCount);
	return true;
}

bool UWoTPayloadSubsystem::InjectSound(UWoTPayloadSoundWave* SoundWave,
	const FString& CacheKey)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread()
		|| !IsValid(SoundWave) || SoundWave->HasAnyFlags(RF_ClassDefaultObject))
	{
		return false;
	}
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		if (WoTPayload_ActiveSoundBindingOwner != this)
		{
			return false;
		}
	}
	if (InjectedSounds.Contains(FObjectKey(SoundWave)))
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		return WoTPayload_ActiveSoundBindingOwner == this
			&& WoTPayload_ActiveSoundBindings.Contains(FObjectKey(SoundWave));
	}
	const TSharedPtr<const FWoTSoundPayloadData> Payload =
		LoadSoundPayload(CacheKey);
	if (!Payload.IsValid() || !SoundWave->PreparePayloadSnapshot(*Payload))
	{
		return false;
	}
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		if (WoTPayload_ActiveSoundBindingOwner != this)
		{
			return false;
		}
		WoTPayload_ActiveSoundBindings.Add(FObjectKey(SoundWave), Payload);
	}
	InjectedSounds.Add(FObjectKey(SoundWave));
	UE_LOG(LogWoTPayload, Verbose, TEXT("Injected %s into %s"),
		*CacheKey, *SoundWave->GetPathName());
	return true;
}

bool UWoTPayloadSubsystem::RemoveInjectedSound(
	UWoTPayloadSoundWave* SoundWave)
{
	if (!IsInGameThread() || !IsValid(SoundWave)) return false;
	const FObjectKey Key(SoundWave);
	bool bRemovedBinding = false;
	{
		FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
		if (WoTPayload_ActiveSoundBindingOwner != this) return false;
		bRemovedBinding = WoTPayload_ActiveSoundBindings.Remove(Key) > 0;
	}
	const bool bRemovedInjection = InjectedSounds.Remove(Key) > 0;
	return bRemovedBinding || bRemovedInjection;
}

bool UWoTPayloadSubsystem::IsInjectedSoundBound(
	const UWoTPayloadSoundWave* SoundWave,
	const TSharedPtr<const FWoTSoundPayloadData>& ExpectedPayload) const
{
	if (!IsValid(SoundWave) || !ExpectedPayload.IsValid()
		|| !InjectedSounds.Contains(FObjectKey(SoundWave))) return false;
	FScopeLock Lock(&WoTPayload_ActiveSoundBindingsMutex);
	const TSharedPtr<const FWoTSoundPayloadData>* Bound =
		WoTPayload_ActiveSoundBindings.Find(FObjectKey(SoundWave));
	return WoTPayload_ActiveSoundBindingOwner == this && Bound != nullptr
		&& *Bound == ExpectedPayload;
}

bool UWoTPayloadSubsystem::SharesSnapshotWith(
	const FWoTCacheGenerationLease& Lease) const
{
	return bPayloadActivated && ActiveCacheLease.IsValid()
		&& ActiveCacheLease->IsValid()
		&& ActiveCacheLease->SharesSnapshotWith(Lease);
}

TSharedPtr<FWoTPreparedMeshPayload> UWoTPayloadSubsystem::PrepareMeshPayload(
	USkeletalMesh* Mesh, const UWoTMeshPayloadLayout& Layout,
	const bool bRetainCPUData)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread()
		|| !IsValid(Mesh) || Mesh->HasAnyFlags(RF_ClassDefaultObject))
	{
		return nullptr;
	}
	PrunePreparedMeshPayloads();
	const FObjectKey MeshKey(Mesh);
	if (const TSharedPtr<FWoTPreparedMeshPayload>* Existing =
		PreparedMeshPayloads.Find(MeshKey))
	{
		if (bRetainCPUData && (*Existing).IsValid()
			&& (*Existing)->CPUData.Frames.IsEmpty())
		{
			const TSharedPtr<const FWoTMeshPayloadData> Payload =
				LoadMeshPayload(Layout.PayloadKey);
			FString Error;
			FWoTMeshPayloadCPUData CPUData;
			if (Payload.IsValid()
				&& ComputeMeshPayloadDeltas(
					*Payload, Layout, Mesh, CPUData, Error))
			{
				(*Existing)->CPUData = MoveTemp(CPUData);
			}
			else
			{
				UE_LOG(LogWoTPayload, Warning,
					TEXT("Could not retain CPU pose %s on %s: %s"),
					*Layout.PayloadKey, *Mesh->GetPathName(), *Error);
			}
		}
		return *Existing;
	}
	FString Error;
	if (!MeshEntries.Contains(Layout.PayloadKey)
		|| !Layout.ValidateDescriptor(Error))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting mesh layout %s on %s: %s"),
			*Layout.PayloadKey, *Mesh->GetPathName(),
			Error.IsEmpty() ? TEXT("cache key is absent or invalid") : *Error);
		return nullptr;
	}
	const TSharedPtr<const FWoTMeshPayloadData> Payload =
		LoadMeshPayload(Layout.PayloadKey);
	FWoTMeshPayloadCPUData CPUData;
	if (!Payload.IsValid()
		|| !ComputeMeshPayloadDeltas(*Payload, Layout, Mesh, CPUData, Error))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting mesh payload build %s on %s: %s"),
			*Layout.PayloadKey, *Mesh->GetPathName(),
			Error.IsEmpty() ? TEXT("authenticated blob load failed") : *Error);
		return nullptr;
	}
	// The authenticated CPU payload may be decoded headlessly, but no set can be
	// published until the actual cooked stripped base/basis matches the pin.
	if (!ValidateStrippedMeshLayout(Mesh, Layout, Error))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("Rejecting cooked mesh basis %s on %s: %s"),
			*Layout.PayloadKey, *Mesh->GetPathName(), *Error);
		return nullptr;
	}

	// -nullrhi can and does validate every CPU delta above. GPU compression and
	// resource publication are deliberately an owed packaged/PIE gate. Retain
	// the decoded CPU pose even there: bParticles-style consumers are CPU-only
	// and must remain specable without an RHI.
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		TSharedRef<FWoTPreparedMeshPayload> Prepared =
			MakeShared<FWoTPreparedMeshPayload>();
		Prepared->Mesh = Mesh;
		if (bRetainCPUData)
		{
			Prepared->CPUData = MoveTemp(CPUData);
		}
		Prepared->FrameCount = Layout.AnimationFrameCount;
		PreparedMeshPayloads.Add(FObjectKey(Mesh), Prepared);
		return Prepared;
	}
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0))
	{
		return nullptr;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	TArray<FMorphTargetLODModel> MorphModels;
	MorphModels.SetNum(CPUData.Frames.Num());
	TArray<const FMorphTargetLODModel*> MorphModelPointers;
	MorphModelPointers.Reserve(MorphModels.Num());
	for (int32 Frame = 0; Frame < CPUData.Frames.Num(); ++Frame)
	{
		FMorphTargetLODModel& Model = MorphModels[Frame];
		const FWoTMeshPayloadFrame& Source = CPUData.Frames[Frame];
		Model.Vertices.SetNumUninitialized(Source.Deltas.Num());
		for (int32 DeltaIndex = 0; DeltaIndex < Source.Deltas.Num(); ++DeltaIndex)
		{
			const FWoTMeshPayloadDelta& SourceDelta = Source.Deltas[DeltaIndex];
			FMorphTargetDelta& DestinationDelta = Model.Vertices[DeltaIndex];
			DestinationDelta.PositionDelta = SourceDelta.PositionDelta;
			DestinationDelta.TangentZDelta = SourceDelta.TangentZDelta;
			DestinationDelta.SourceIdx = SourceDelta.SourceIndex;
		}
		Model.NumVertices = Model.Vertices.Num();
		Model.NumBaseMeshVerts = CPUData.RenderVertexCount;
		for (int32 SectionIndex = 0;
			SectionIndex < LOD.RenderSections.Num(); ++SectionIndex)
		{
			Model.SectionIndices.Add(SectionIndex);
		}
		MorphModelPointers.Add(&Model);
	}

	TSharedRef<FExternalMorphSet> ExternalSet = MakeShared<FExternalMorphSet>();
	ExternalSet->Name = FName(*FString::Printf(
		TEXT("WoTPayload:%s"), *Layout.PayloadKey));
	const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(0);
	const float ErrorTolerance = LODInfo
		? LODInfo->MorphTargetPositionErrorTolerance : 20.0f;
	ExternalSet->MorphBuffers.InitMorphResourcesStreaming(
		GMaxRHIShaderPlatform, LOD.RenderSections, MorphModelPointers,
		CPUData.RenderVertexCount, ErrorTolerance);
	if (!ExternalSet->MorphBuffers.IsMorphCPUDataValid()
		|| ExternalSet->MorphBuffers.GetNumMorphs()
			!= static_cast<uint32>(Layout.AnimationFrameCount))
	{
		UE_LOG(LogWoTPayload, Warning,
			TEXT("External morph compression rejected %s"), *Layout.PayloadKey);
		return nullptr;
	}
#if WITH_EDITOR
	ExternalSet->MorphBuffers.SetEmptyMorphCPUDataOnInitRHI(false);
#else
	ExternalSet->MorphBuffers.SetEmptyMorphCPUDataOnInitRHI(true);
#endif
	BeginInitResource(&ExternalSet->MorphBuffers);

	TSharedRef<FWoTPreparedMeshPayload> Prepared =
		MakeShared<FWoTPreparedMeshPayload>();
	Prepared->Mesh = Mesh;
	Prepared->MorphSet = ExternalSet;
	if (bRetainCPUData)
	{
		Prepared->CPUData = MoveTemp(CPUData);
	}
	Prepared->FrameCount = Layout.AnimationFrameCount;
	PreparedMeshPayloads.Add(MeshKey, Prepared);
	return Prepared;
}

void UWoTPayloadSubsystem::PrunePreparedMeshPayloads()
{
	check(IsInGameThread());
	// Keep each shared set alive until its queued render-resource release has
	// completed. Otherwise removing the map entry could destroy MorphBuffers
	// while the render thread still holds the release command's raw pointer.
	TArray<TSharedPtr<FWoTPreparedMeshPayload>> Releasing;
	for (auto It = PreparedMeshPayloads.CreateIterator(); It; ++It)
	{
		const TSharedPtr<FWoTPreparedMeshPayload>& Prepared = It.Value();
		if (Prepared.IsValid() && Prepared->Mesh.IsValid())
		{
			continue;
		}
		if (Prepared.IsValid() && Prepared->MorphSet.IsValid()
			&& Prepared->MorphSet->MorphBuffers.IsMorphResourcesInitialized())
		{
			BeginReleaseResource(&Prepared->MorphSet->MorphBuffers);
			Releasing.Add(Prepared);
		}
		It.RemoveCurrent();
	}
	if (!Releasing.IsEmpty())
	{
		FlushRenderingCommands();
		UE_LOG(LogWoTPayload, Verbose,
			TEXT("Released %d collected skeletal-mesh payload GPU set(s)"),
			Releasing.Num());
	}
	for (auto It = BoundMeshComponents.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void UWoTPayloadSubsystem::HandlePostGarbageCollect()
{
	if (IsInGameThread())
	{
		PrunePreparedMeshPayloads();
	}
}

void UWoTPayloadSubsystem::TryPrepareSkeletalMesh(USkeletalMesh* Mesh)
{
	if (!bInjectionEnabled || !bManifestReady || !IsValid(Mesh)
		|| Mesh->HasAnyFlags(RF_ClassDefaultObject)
		|| PreparedMeshPayloads.Contains(FObjectKey(Mesh)))
	{
		return;
	}
	const UWoTMeshPayloadLayout* Layout =
		Cast<UWoTMeshPayloadLayout>(Mesh->GetAssetUserDataOfClass(
			UWoTMeshPayloadLayout::StaticClass()));
	if (!Layout || !Layout->bStrippedBasePositions)
	{
		return;
	}
	PrepareMeshPayload(Mesh, *Layout);
}

bool UWoTPayloadSubsystem::BindMeshComponent(
	UWoTVertexAnimComponent* Component)
{
	if (!bInjectionEnabled || !bManifestReady || !IsInGameThread()
		|| !IsValid(Component) || Component->HasAnyFlags(RF_ClassDefaultObject)
		|| !Component->GetWorld()
		|| Component->GetWorld()->GetGameInstance() != GetGameInstance())
	{
		return false;
	}
	USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
	const UWoTMeshPayloadLayout* Layout = Mesh
		? Cast<UWoTMeshPayloadLayout>(Mesh->GetAssetUserDataOfClass(
			UWoTMeshPayloadLayout::StaticClass())) : nullptr;
	if (!Mesh || !Layout || !Layout->bStrippedBasePositions)
	{
		return false;
	}
	const TSharedPtr<FWoTPreparedMeshPayload> Prepared =
		PrepareMeshPayload(
			Mesh, *Layout, Component->bCurrentPoseCPUAccessRequested);
	const bool bPoseInstalled = Prepared.IsValid()
		&& Component->InstallPayloadPoseCache(Prepared->CPUData);
	const bool bInstalled = Component->IsExternalMorphInjectionActive()
		|| (Prepared.IsValid() && Prepared->MorphSet.IsValid()
			&& Prepared->MorphSet->MorphBuffers.GetNumMorphs()
				== static_cast<uint32>(Prepared->FrameCount)
			&& Component->InstallExternalMorphSet(
				Prepared->MorphSet, Prepared->FrameCount));
	if (bInstalled)
	{
		BoundMeshComponents.Add(Component);
	}
	return bInstalled || bPoseInstalled;
}

void UWoTPayloadSubsystem::TryBindMeshComponent(
	UWoTVertexAnimComponent* Component)
{
	if (!IsValid(Component))
	{
		return;
	}
	if (bPayloadActivated)
	{
		USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
		const UWoTMeshPayloadLayout* Layout = Mesh
			? Cast<UWoTMeshPayloadLayout>(Mesh->GetAssetUserDataOfClass(
				UWoTMeshPayloadLayout::StaticClass())) : nullptr;
		if (Layout && Layout->AnimationLogicalId.StartsWith(
				TEXT("cache/animation/"), ESearchCase::CaseSensitive))
		{
			if (Component->AnimationLogicalId != Layout->AnimationLogicalId)
			{
				Component->AnimationLogicalId = Layout->AnimationLogicalId;
				Component->Sequences.Reset();
			}
			// A component registered before activation saw no verified row. Rebind
			// its sequence carrier after the generation becomes authoritative.
			WoTAnimData::PopulateSequences(
				Component, Mesh->GetFName(), Component->AnimationLogicalId);
		}
	}
	if (!Component->IsExternalMorphInjectionActive())
	{
		BindMeshComponent(Component);
	}
}

void UWoTPayloadSubsystem::HandlePackageLoadCompleted(UPackage* Package)
{
	if (!Package)
	{
		return;
	}
	// The engine queues this broadcast per completed package and drains the
	// queue at later safe points; garbage collection can run in between (PIE
	// start does, -game start does not), so the raw pointer can already be
	// dead here — dead enough that even TWeakObjectPtr construction asserts
	// (AllocateSerialNumber Index >= 0, PIE possession, 2026-07-24) and that
	// iterating it faults on a null ClassPrivate (the prior 0x124 SIGSEGV).
	// IsValidLowLevel only reads (index bounds + slot identity), and a GC'd
	// package has nothing left to scan, so dropping it is correct.
	if (!Package->IsValidLowLevel())
	{
		return;
	}
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UWoTPayloadSubsystem> WeakThis(this);
		TWeakObjectPtr<UPackage> WeakPackage(Package);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakPackage]()
		{
			if (UWoTPayloadSubsystem* StrongThis = WeakThis.Get())
			{
				StrongThis->HandlePackageLoadCompleted(WeakPackage.Get());
			}
		});
		return;
	}
	// This delegate can fire in the middle of FlushAsyncLoading
	// (LoadPackageInternal -> ConditionalProcessCallbacks) while sibling
	// packages' exports are still under construction; iterating the UObject
	// hash at that point faults on a half-constructed inner (PIE
	// first-possession SIGSEGV at null ClassPrivate + 0x124, 2026-07-23).
	// Queue the package and scan it at the next world tick start, which runs
	// ahead of the BSP barrier evaluation in the same tick handler.
	PendingCompletedPackages.Add(Package);
}

void UWoTPayloadSubsystem::DrainCompletedPackages()
{
	if (PendingCompletedPackages.IsEmpty())
	{
		return;
	}
	TArray<TWeakObjectPtr<UPackage>> Drained = MoveTemp(PendingCompletedPackages);
	PendingCompletedPackages.Reset();
	for (const TWeakObjectPtr<UPackage>& WeakPackage : Drained)
	{
		if (UPackage* Package = WeakPackage.Get())
		{
			ProcessCompletedPackage(Package);
		}
	}
}

void UWoTPayloadSubsystem::ProcessCompletedPackage(UPackage* Package)
{
	if (!IsValid(Package) || !IsInGameThread())
	{
		return;
	}
	if (!bManifestReady)
	{
		// Collect first: BSP binding creates UObjects, which is fatal inside
		// a UObject-hash iteration (packaged first-launch crash).
		TArray<UStaticMesh*> EarlyMeshes;
		ForEachObjectWithOuter(Package, [&EarlyMeshes](UObject* Object)
		{
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
			{
				EarlyMeshes.Add(StaticMesh);
			}
		}, EGetObjectsFlags::IncludeNestedObjects);
		for (UStaticMesh* StaticMesh : EarlyMeshes)
		{
			TryBindBSPStaticMesh(StaticMesh);
		}
		return;
	}
	PrunePreparedMeshPayloads();
	// BSP static-mesh binds are deferred past the iteration: injection
	// creates UObjects, which is fatal while the UObject hash is iterated.
	TArray<UStaticMesh*> PackageMeshes;
	ForEachObjectWithOuter(Package, [this, &PackageMeshes](UObject* Object)
	{
		if (UTexture2D* Texture = Cast<UTexture2D>(Object))
		{
			TryBindTexture(Texture);
		}
		if (UWoTPayloadSoundWave* SoundWave = Cast<UWoTPayloadSoundWave>(Object))
		{
			TryBindSoundWave(SoundWave);
		}
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object))
		{
			TryPrepareSkeletalMesh(Mesh);
		}
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			PackageMeshes.Add(StaticMesh);
		}
		if (UWoTVertexAnimComponent* Component =
			Cast<UWoTVertexAnimComponent>(Object))
		{
			TryBindMeshComponent(Component);
		}
	}, EGetObjectsFlags::IncludeNestedObjects);
	for (UStaticMesh* StaticMesh : PackageMeshes)
	{
		TryBindBSPStaticMesh(StaticMesh);
	}
}

void UWoTPayloadSubsystem::BindAlreadyLoadedPayloads()
{
	check(IsInGameThread());
	for (TObjectIterator<UTexture2D> It; It; ++It)
	{
		TryBindTexture(*It);
	}
	for (TObjectIterator<UWoTPayloadSoundWave> It; It; ++It)
	{
		TryBindSoundWave(*It);
	}
	for (TObjectIterator<USkeletalMesh> It; It; ++It)
	{
		TryPrepareSkeletalMesh(*It);
	}
	// BSP injection creates UObjects (UBodySetup, rebuilt render data), which
	// is fatal INSIDE a UObject-hash iteration ("modify UObject map while
	// iterated" — first live packaged run). Collect, then bind.
	TArray<UStaticMesh*> StaticMeshes;
	for (TObjectIterator<UStaticMesh> It; It; ++It)
	{
		StaticMeshes.Add(*It);
	}
	for (UStaticMesh* StaticMesh : StaticMeshes)
	{
		TryBindBSPStaticMesh(StaticMesh);
	}
	for (TObjectIterator<UWoTVertexAnimComponent> It; It; ++It)
	{
		TryBindMeshComponent(*It);
	}
}

bool UWoTPayloadSubsystem::RebindLoadedPayloads()
{
	if (!IsInGameThread() || !bInjectionEnabled || !bManifestReady
		|| !bPayloadActivated)
	{
		return false;
	}
	BindAlreadyLoadedPayloads();
	return true;
}

bool UWoTPayloadSubsystem::RebindLoadedPayloads(
	TConstArrayView<UTexture2D*> RequiredFrontEndTextures)
{
	if (RequiredFrontEndTextures.IsEmpty() || !RebindLoadedPayloads())
	{
		return false;
	}
	for (UTexture2D* Texture : RequiredFrontEndTextures)
	{
		if (!IsValid(Texture) || Texture->HasAnyFlags(RF_ClassDefaultObject)
			|| Cast<UWoTPayloadAssetUserData>(
				Texture->GetAssetUserDataOfClass(
					UWoTPayloadAssetUserData::StaticClass())) == nullptr
			|| !InjectedTextures.Contains(FObjectKey(Texture)))
		{
			return false;
		}
	}
	return true;
}

void UWoTPayloadSubsystem::TryBindTexture(UTexture2D* Texture)
{
	if (!bInjectionEnabled || !bManifestReady || !IsValid(Texture)
		|| Texture->HasAnyFlags(RF_ClassDefaultObject)
		|| InjectedTextures.Contains(FObjectKey(Texture)))
	{
		// Diagnostic-only: the last silent path in the black-menu hunt. Only
		// interesting when the gate itself refuses a live WoT texture.
		if (!bInjectionEnabled || !bManifestReady)
		{
			static int32 GateBreadcrumbsLeft = 4;
			if (GateBreadcrumbsLeft > 0 && IsValid(Texture)
				&& Texture->GetPathName().StartsWith(TEXT("/Game/WoT/")))
			{
				--GateBreadcrumbsLeft;
				WoTRecordBootstrapHeartbeat(TEXT("texture-gate-bail"),
					EWoTBootstrapPhase::Ready, 0, {}, {},
					FString::Printf(TEXT("%s enabled=%d manifest=%d"),
						*Texture->GetPathName(), bInjectionEnabled ? 1 : 0,
						bManifestReady ? 1 : 0));
			}
		}
		return;
	}
	const UWoTPayloadAssetUserData* Binding =
		Cast<UWoTPayloadAssetUserData>(Texture->GetAssetUserDataOfClass(
			UWoTPayloadAssetUserData::StaticClass()));
	if (!Binding)
	{
		// Diagnostic-only: a WoT-owned texture with no payload binding in a
		// packaged build renders black with no trace (the front-end menu,
		// 2026-08-06). Only project textures are interesting - engine and
		// plugin textures legitimately have no binding.
		static int32 UnboundBreadcrumbsLeft = 8;
		if (UnboundBreadcrumbsLeft > 0)
		{
			const FString Path = Texture->GetPathName();
			if (Path.StartsWith(TEXT("/Game/WoT/")))
			{
				--UnboundBreadcrumbsLeft;
				WoTRecordBootstrapHeartbeat(TEXT("texture-unbound"),
					EWoTBootstrapPhase::Ready, 0, {}, {}, Path);
			}
		}
		return;
	}
	FString CacheKey = Binding->PayloadKey;
#if !WOT_GUARDED_BOOTSTRAP
	if (!bPayloadActivated && CacheKey.StartsWith(
		TEXT("cache/texture/"), ESearchCase::CaseSensitive))
	{
		CacheKey = TEXT("texture:") + CacheKey.RightChop(14);
	}
#endif

	if (bPayloadActivated)
	{
		if (!Binding->PayloadKey.StartsWith(
				TEXT("cache/texture/"), ESearchCase::CaseSensitive))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting texture binding on %s: neutral logical id %s is outside the texture domain"),
				*Texture->GetPathName(), *Binding->PayloadKey);
			return;
		}
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		const FString* DirectKey =
			DirectTextureKeys.Find(FName(*Texture->GetPathName()));
		if (DirectKey && *DirectKey != CacheKey)
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting texture binding on %s: metadata key %s disagrees with identity key %s"),
				*Texture->GetPathName(), *CacheKey, **DirectKey);
			return;
		}
		if (!TextureEntries.Contains(CacheKey))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting texture binding on %s: cache key %s is absent or invalid"),
				*Texture->GetPathName(), *CacheKey);
			return;
		}
	}
#endif
	const bool bInjected =
		InjectTexture(Texture, CacheKey, Binding->DestinationMipCount);
	// Diagnostic-only success record for the menu texture set: with every
	// failure path already breadcrumbed, this decides injected-but-black
	// (render path) vs never-injected (subsystem state).
	static int32 SuccessBreadcrumbsLeft = 32;
	if (bInjected && SuccessBreadcrumbsLeft > 0)
	{
		const FString Path = Texture->GetPathName();
		if (Path.StartsWith(TEXT("/Game/WoT/Textures/WOT/"))
			|| Path.StartsWith(TEXT("/Game/WoT/UI/")))
		{
			--SuccessBreadcrumbsLeft;
			WoTRecordBootstrapHeartbeat(TEXT("texture-injected"),
				EWoTBootstrapPhase::Ready, 0, {}, {}, Path);
		}
	}
}

void UWoTPayloadSubsystem::TryBindSoundWave(UWoTPayloadSoundWave* SoundWave)
{
	if (!bInjectionEnabled || !bManifestReady || !IsValid(SoundWave)
		|| SoundWave->HasAnyFlags(RF_ClassDefaultObject)
		|| InjectedSounds.Contains(FObjectKey(SoundWave)))
	{
		return;
	}
	if (SoundWave->PayloadKey.IsEmpty())
	{
		return;
	}
	FString CacheKey = SoundWave->PayloadKey;
#if !WOT_GUARDED_BOOTSTRAP
	if (!bPayloadActivated && CacheKey.StartsWith(
		TEXT("cache/sound/"), ESearchCase::CaseSensitive))
	{
		CacheKey = TEXT("sound:") + CacheKey.RightChop(12);
	}
#endif

	if (bPayloadActivated)
	{
		if (!SoundWave->PayloadKey.StartsWith(
				TEXT("cache/sound/"), ESearchCase::CaseSensitive))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting sound binding on %s: neutral logical id %s is outside the sound domain"),
				*SoundWave->GetPathName(), *SoundWave->PayloadKey);
			return;
		}
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		const FString* DirectKey = DirectSoundKeys.Find(
			FName(*SoundWave->GetPathName()));
		if (DirectKey && *DirectKey != CacheKey)
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting sound binding on %s: metadata key %s disagrees with identity key %s"),
				*SoundWave->GetPathName(), *CacheKey, **DirectKey);
			return;
		}
		if (!SoundEntries.Contains(CacheKey))
		{
			UE_LOG(LogWoTPayload, Warning,
				TEXT("Rejecting sound binding on %s: cache key %s is absent or invalid"),
				*SoundWave->GetPathName(), *CacheKey);
			return;
		}
	}
#endif
	InjectSound(SoundWave, CacheKey);
}
