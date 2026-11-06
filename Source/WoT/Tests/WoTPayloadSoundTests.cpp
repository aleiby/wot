// Headless specs for authenticated cache-v3 WAV objects and playback generators.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Assets/WoTRuntimeAssets.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSoundWave.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSha256.h"
#include "WoTPayloadActivationTestSupport.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundGenerator.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	template<typename TWoTPayloadSoundTests_Tag,
		typename TWoTPayloadSoundTests_Tag::Type WoTPayloadSoundTests_Member>
	struct TWoTPayloadSoundTests_PrivateMemberAccessor
	{
		friend typename TWoTPayloadSoundTests_Tag::Type
			WoTPayloadSoundTests_GetPrivateMember(TWoTPayloadSoundTests_Tag)
		{
			return WoTPayloadSoundTests_Member;
		}
	};

	struct FWoTPayloadSoundTests_PackageLoadCompletedTag
	{
		using Type = void (UWoTPayloadSubsystem::*)(UPackage*);
		friend Type WoTPayloadSoundTests_GetPrivateMember(
			FWoTPayloadSoundTests_PackageLoadCompletedTag);
	};

	template struct TWoTPayloadSoundTests_PrivateMemberAccessor<
		FWoTPayloadSoundTests_PackageLoadCompletedTag,
		&UWoTPayloadSubsystem::ProcessCompletedPackage>;

	enum class EWoTPayloadSoundTests_FixtureMode
	{
		ValidPCM16,
		ValidPCM8,
		LyingChannels,
		LyingSampleRate,
		LyingFrameCount,
		LyingDataLength,
	};

	struct FWoTPayloadSoundTests_Fixture
	{
		FString CacheDirectory;
		FString PackageName;
		FString AssetName;
		FString CacheKey;
		FString BlobPath;
		TArray<uint8> WAVBytes;
		TArray<uint8> PCMBytes;
		int32 Channels = 1;
		int32 SampleRate = 8000;
		int32 BitsPerSample = 16;
		int32 FrameCount = 4;
		bool bManifestMatchesWAV = true;
		bool bPublished = false;
		FString PublishError;
	};

	struct FWoTPayloadSoundTests_World
	{
		UWorld* World = nullptr;
		UWoTGameInstance* GameInstance = nullptr;
		bool bActivated = false;
		FString ActivationError;
	};

	void WoTPayloadSoundTests_AppendU16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	void WoTPayloadSoundTests_AppendPCM16(TArray<uint8>& Bytes, int16 Value)
	{
		WoTPayloadSoundTests_AppendU16(Bytes, static_cast<uint16>(Value));
	}

	void WoTPayloadSoundTests_AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadSoundTests_AppendU64(TArray<uint8>& Bytes, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadSoundTests_AppendASCII(
		TArray<uint8>& Bytes, const FString& Value)
	{
		for (TCHAR Character : Value)
		{
			check(Character <= 127);
			Bytes.Add(static_cast<uint8>(Character));
		}
	}

	FString WoTPayloadSoundTests_SHA256(const void* Data, uint32 Size)
	{
		return WoTSha256::HashBuffer(Data, Size);
	}

	FString WoTPayloadSoundTests_CacheRoot(const FString& CanonicalCore,
		const FString& BlobPath, uint64 BlobSize, const FString& BlobSHA256)
	{
		TArray<uint8> Input;
		WoTPayloadSoundTests_AppendASCII(
			Input, TEXT("WOT-PAYLOAD-CACHE-HASH-V1"));
		Input.Add(0);
		WoTPayloadSoundTests_AppendU64(
			Input, static_cast<uint64>(CanonicalCore.Len()));
		WoTPayloadSoundTests_AppendASCII(Input, CanonicalCore);

		TArray<uint8> Record;
		WoTPayloadSoundTests_AppendASCII(Record, BlobPath);
		Record.Add(0);
		WoTPayloadSoundTests_AppendASCII(Record, LexToString(BlobSize));
		Record.Add(0);
		WoTPayloadSoundTests_AppendASCII(Record, BlobSHA256);
		Record.Add('\n');
		WoTPayloadSoundTests_AppendU64(
			Input, static_cast<uint64>(Record.Num()));
		Input.Append(Record);
		return WoTPayloadSoundTests_SHA256(Input.GetData(), Input.Num());
	}

	TArray<uint8> WoTPayloadSoundTests_MakeWAV(
		int32 BitsPerSample, const TArray<uint8>& PCMBytes,
		int32 Channels = 1, int32 SampleRate = 8000)
	{
		TArray<uint8> Body;
		Body.Append({'W', 'A', 'V', 'E'});
		Body.Append({'f', 'm', 't', ' '});
		WoTPayloadSoundTests_AppendU32(Body, 16);
		WoTPayloadSoundTests_AppendU16(Body, 1);
		WoTPayloadSoundTests_AppendU16(
			Body, static_cast<uint16>(Channels));
		WoTPayloadSoundTests_AppendU32(Body, SampleRate);
		const uint16 BlockAlign = static_cast<uint16>(
			Channels * BitsPerSample / 8);
		WoTPayloadSoundTests_AppendU32(Body, SampleRate * BlockAlign);
		WoTPayloadSoundTests_AppendU16(Body, BlockAlign);
		WoTPayloadSoundTests_AppendU16(
			Body, static_cast<uint16>(BitsPerSample));
		// An odd unknown chunk proves the parser honors RIFF padding.
		Body.Append({'J', 'U', 'N', 'K'});
		WoTPayloadSoundTests_AppendU32(Body, 1);
		Body.Add(0x5a);
		Body.Add(0);
		Body.Append({'d', 'a', 't', 'a'});
		WoTPayloadSoundTests_AppendU32(Body, PCMBytes.Num());
		Body.Append(PCMBytes);
		if ((PCMBytes.Num() & 1) != 0)
		{
			Body.Add(0);
		}

		TArray<uint8> WAV;
		WAV.Append({'R', 'I', 'F', 'F'});
		WoTPayloadSoundTests_AppendU32(WAV, Body.Num());
		WAV.Append(Body);
		return WAV;
	}

	FWoTPayloadSoundTests_Fixture WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode Mode, int32 FixtureChannels = 1,
		int32 FixtureFrames = 4,
		const TArray<uint8>& PCMOverride = TArray<uint8>(),
		const FString& PackageOverride = FString(),
		bool bHeaderLoop = false)
	{
		FWoTPayloadSoundTests_Fixture Result;
		Result.PackageName = PackageOverride.IsEmpty()
			? TEXT("WoTPayloadSoundFixture_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
			: PackageOverride;
		Result.AssetName = TEXT("Tone");
		Result.Channels = FixtureChannels;
		Result.FrameCount = FixtureFrames;
		Result.CacheKey = FString::Printf(TEXT("cache/sound/%s__%s"),
			*Result.PackageName, *Result.AssetName);
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadSoundTests_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, /*Tree*/ true);

		if (PCMOverride.Num() > 0)
		{
			Result.PCMBytes = PCMOverride;
		}
		else if (Mode == EWoTPayloadSoundTests_FixtureMode::ValidPCM8)
		{
			Result.BitsPerSample = 8;
			Result.PCMBytes.Append({0, 64, 128, 255});
		}
		else
		{
			Result.PCMBytes.Append({0x00, 0x80, 0x00, 0xc0,
				0x00, 0x00, 0xff, 0x7f});
		}
		Result.WAVBytes = WoTPayloadSoundTests_MakeWAV(Result.BitsPerSample,
			Result.PCMBytes, Result.Channels, Result.SampleRate);
		const FString BlobSHA = WoTPayloadSoundTests_SHA256(
			Result.WAVBytes.GetData(), Result.WAVBytes.Num());
		Result.BlobPath = FString::Printf(TEXT("blobs/%s/%s.blob"),
			*BlobSHA.Left(2), *BlobSHA);

		int32 ManifestChannels = Result.Channels;
		int32 ManifestRate = Result.SampleRate;
		int32 ManifestBits = Result.BitsPerSample;
		int32 ManifestFrames = Result.FrameCount;
		int32 ManifestDataLength = Result.PCMBytes.Num();
		FString ManifestDuration = Result.FrameCount == 3
			? TEXT("0.000375") : TEXT("0.0005");
		switch (Mode)
		{
		case EWoTPayloadSoundTests_FixtureMode::LyingChannels:
			ManifestChannels = 2;
			ManifestFrames = 2;
			ManifestDuration = TEXT("0.00025");
			Result.bManifestMatchesWAV = false;
			break;
		case EWoTPayloadSoundTests_FixtureMode::LyingSampleRate:
			ManifestRate = 16000;
			ManifestDuration = TEXT("0.00025");
			Result.bManifestMatchesWAV = false;
			break;
		case EWoTPayloadSoundTests_FixtureMode::LyingFrameCount:
			// Keep hostile manifest cross-fields internally consistent while
			// declaring eight PCM8 frames for the real four-frame PCM16 blob.
			ManifestBits = 8;
			ManifestFrames = 8;
			ManifestDuration = TEXT("0.001");
			Result.bManifestMatchesWAV = false;
			break;
		case EWoTPayloadSoundTests_FixtureMode::LyingDataLength:
			// Two channels x three PCM16 frames makes the declared 12 bytes
			// internally valid but still disagrees with the eight-byte blob.
			ManifestChannels = 2;
			ManifestFrames = 3;
			ManifestDataLength = 12;
			ManifestDuration = TEXT("0.000375");
			Result.bManifestMatchesWAV = false;
			break;
		default:
			break;
		}

		const FString Entry = FString::Printf(
			TEXT("{\"bits_per_sample\":%d,\"blob\":\"%s\",\"channels\":%d,")
			TEXT("\"data_byte_length\":%d,\"duration_seconds\":%s,")
			TEXT("\"encoding\":\"wav-v1\",\"frame_count\":%d,")
			TEXT("\"name\":\"%s\",\"package\":\"%s\",\"pcm_format_tag\":1,")
			TEXT("\"sample_rate\":%d,\"sha256\":\"%s\",\"size\":%d,")
			TEXT("\"type\":\"sounds\"}"),
			ManifestBits, *Result.BlobPath, ManifestChannels,
			ManifestDataLength, *ManifestDuration, ManifestFrames,
			*Result.AssetName, *Result.PackageName, ManifestRate, *BlobSHA,
			Result.WAVBytes.Num());
		const FString SourcePath = FString::Printf(
			TEXT("Sounds/%s.uax"), *Result.PackageName);
		const FString CanonicalCore = FString::Printf(
			TEXT("{\"cache_version\":1,\"entries\":{\"%s\":%s},")
			TEXT("\"format\":\"wot-payload-cache\",\"hash_algorithm\":\"sha256\",")
			TEXT("\"limits\":{\"max_blob_size\":268435456,\"max_entries\":10000,")
			TEXT("\"max_texture_dimension\":8192,\"max_total_expanded_bytes\":2147483648},")
			TEXT("\"skipped\":[],\"sources\":[{\"path\":\"%s\",")
			TEXT("\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"size\":1,\"type\":\"sounds\"}],")
			TEXT("\"summary\":{\"entries\":{\"meshes\":0,\"sounds\":1,\"textures\":0},")
			TEXT("\"logical_payload_bytes\":%d,")
			TEXT("\"source_packages\":{\"meshes\":0,\"sounds\":1,\"textures\":0},")
			TEXT("\"total_expanded_bytes\":%d,\"unique_blob_bytes\":%d,")
			TEXT("\"unique_blobs\":1},\"types\":[\"sounds\"]}"),
			*Result.CacheKey, *Entry, *SourcePath, Result.WAVBytes.Num(),
			ManifestDataLength, Result.WAVBytes.Num());
		const FString CacheRoot = WoTPayloadSoundTests_CacheRoot(
			CanonicalCore, Result.BlobPath, Result.WAVBytes.Num(), BlobSHA);
		const FString Manifest = FString::Printf(
			TEXT("{\"cache_sha256\":\"%s\",%s"),
			*CacheRoot, *CanonicalCore.RightChop(1));
		const FString HeaderLoops = bHeaderLoop
			? TEXT("[{\"end_frame_exclusive\":3,\"play_count\":0,\"start_frame\":1}]")
			: TEXT("[]");
		const FString Header = FString::Printf(
			TEXT("{\"audio\":{\"bits_per_sample\":%d,\"channels\":%d,\"format_tag\":1,\"sample_rate\":%d},")
			TEXT("\"decoded_layout\":{\"data_byte_length\":%d,\"frame_count\":%d,\"interleaved\":true},")
			TEXT("\"duration\":{\"frames\":%d,\"sample_rate\":%d},\"encoding\":\"wav-v1\",\"fact_id\":\"spec.sound\",\"importer\":\"sound-v1\",\"importer_version\":1,\"kind\":\"sound\",\"loops\":%s,\"payload_byte_length\":%d,\"source\":{\"export\":\"%s\",\"package\":\"Sounds/%s.uax\",\"sha256\":\"%s\"}}"),
			ManifestBits, ManifestChannels, ManifestRate,
			ManifestDataLength, ManifestFrames, ManifestFrames, ManifestRate,
			*HeaderLoops, Result.WAVBytes.Num(), *Result.AssetName, *Result.PackageName,
			TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
		const TArray<uint8> Object = WoTPayloadActivationTest_FrameObject(
			Header, Result.WAVBytes);
		Result.bPublished = WoTPayloadActivationTest_PublishObject(
			Result.CacheDirectory, Result.CacheKey, TEXT("wot-sound-v3"),
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
		FFileHelper::SaveArrayToFile(Result.WAVBytes, *BlobFilename);
		return Result;
	}

	FWoTPayloadSoundTests_World WoTPayloadSoundTests_CreateWorld(
		const FString& CacheDirectory, bool bInjectionEnabled)
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ true, bInjectionEnabled);
		FWoTPayloadSoundTests_World Result;
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

	void WoTPayloadSoundTests_DestroyWorld(FWoTPayloadSoundTests_World& State)
	{
		WoTTest::DestroySpecWorld(State.World);
		State.World = nullptr;
		State.GameInstance = nullptr;
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ false);
	}

	UWoTPayloadSoundWave* WoTPayloadSoundTests_CreateWave(
		const FString& LongPackageName, const FString& AssetName,
		const FWoTPayloadSoundTests_Fixture& Fixture, bool bLooping)
	{
		UPackage* Package = CreatePackage(*LongPackageName);
		UWoTPayloadSoundWave* Wave = NewObject<UWoTPayloadSoundWave>(
			Package, *AssetName, RF_Public | RF_Standalone);
		Wave->PayloadKey = Fixture.CacheKey;
		Wave->PayloadChannels = Fixture.Channels;
		Wave->PayloadSampleRate = Fixture.SampleRate;
		Wave->PayloadBitsPerSample = Fixture.BitsPerSample;
		Wave->PayloadFrameCount = Fixture.FrameCount;
		Wave->bLooping = bLooping;
		return Wave;
	}

	UWoTPayloadSoundWave* WoTPayloadSoundTests_CreateCanonicalWave(
		const FWoTPayloadSoundTests_Fixture& Fixture, bool bLooping = false)
	{
		const FString LongPackageName = FString::Printf(
			TEXT("/Game/WoT/Sounds/%s/%s"),
			*Fixture.PackageName, *Fixture.AssetName);
		return WoTPayloadSoundTests_CreateWave(
			LongPackageName, Fixture.AssetName, Fixture, bLooping);
	}

	void WoTPayloadSoundTests_DestroyWave(UWoTPayloadSoundWave*& Wave)
	{
		if (!Wave)
		{
			return;
		}
		UPackage* Package = Wave->GetOutermost();
		Wave->ClearFlags(RF_Public | RF_Standalone);
		Wave->MarkAsGarbage();
		if (Package)
		{
			ResetLoaders(Package);
			Package->MarkAsGarbage();
		}
		Wave = nullptr;
	}

	void WoTPayloadSoundTests_DeleteFixture(
		const FWoTPayloadSoundTests_Fixture& Fixture)
	{
		IFileManager::Get().DeleteDirectory(
			*Fixture.CacheDirectory, /*RequireExists*/ false, /*Tree*/ true);
	}

	bool WoTPayloadSoundTests_NearlyEqual(float A, float B)
	{
		return FMath::IsNearlyEqual(A, B, 1.0e-6f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundManifestSpec,
	"WoT.Payload.Sound.CacheV3ObjectAndWAVValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundManifestSpec::RunTest(const FString&)
{
	for (const EWoTPayloadSoundTests_FixtureMode Mode : {
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16,
		EWoTPayloadSoundTests_FixtureMode::ValidPCM8,
		EWoTPayloadSoundTests_FixtureMode::LyingChannels,
		EWoTPayloadSoundTests_FixtureMode::LyingSampleRate,
		EWoTPayloadSoundTests_FixtureMode::LyingFrameCount,
		EWoTPayloadSoundTests_FixtureMode::LyingDataLength})
	{
		FWoTPayloadSoundTests_Fixture Fixture =
			WoTPayloadSoundTests_WriteFixture(Mode);
		TestTrue(TEXT("sound cache-v3 fixture publishes"), Fixture.bPublished);
		if (!Fixture.bPublished)
		{
			AddError(Fixture.PublishError);
		}
		FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
			Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
		TestTrue(TEXT("sound generation activates from a verified snapshot"),
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
			const TSharedPtr<const FWoTSoundPayloadData> Payload =
				Payloads->LoadSoundPayload(Fixture.CacheKey);
			TestEqual(TEXT("WAV/cache-v3 header agreement controls payload acceptance"),
				Payload.IsValid(), Fixture.bManifestMatchesWAV);
			if (Payload.IsValid())
			{
				TestEqual(TEXT("decoded channels"),
					static_cast<int32>(Payload->Channels), Fixture.Channels);
				TestEqual(TEXT("decoded sample rate"),
					static_cast<int32>(Payload->SampleRate), Fixture.SampleRate);
				TestEqual(TEXT("decoded bit depth"),
					static_cast<int32>(Payload->BitsPerSample),
					Fixture.BitsPerSample);
				TestEqual(TEXT("decoded frame count"),
					static_cast<int32>(Payload->FrameCount), Fixture.FrameCount);
				TestTrue(TEXT("decoded data bytes are exact"),
					Payload->PCMBytes == Fixture.PCMBytes);
			}
		}
		WoTPayloadSoundTests_DestroyWorld(State);
		WoTPayloadSoundTests_DeleteFixture(Fixture);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundGeneratorSpec,
	"WoT.Payload.Sound.GeneratorAndBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundGeneratorSpec::RunTest(const FString&)
{
	FWoTPayloadSoundTests_Fixture Fixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16);
	UWoTPayloadSoundWave* EarlyWave =
		WoTPayloadSoundTests_CreateCanonicalWave(Fixture);
	FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("generator fixture publishes"), Fixture.bPublished);
	TestTrue(TEXT("generator fixture activates"), State.bActivated);
	UWoTPayloadSoundWave* LateWave = nullptr;
	UWoTPayloadSoundWave* PostLoadWave = nullptr;
	UWoTPayloadSoundWave* LoopWave = nullptr;
	ON_SCOPE_EXIT
	{
		WoTPayloadSoundTests_DestroyWorld(State);
		WoTPayloadSoundTests_DestroyWave(LoopWave);
		WoTPayloadSoundTests_DestroyWave(PostLoadWave);
		WoTPayloadSoundTests_DestroyWave(LateWave);
		WoTPayloadSoundTests_DestroyWave(EarlyWave);
		WoTPayloadSoundTests_DeleteFixture(Fixture);
	};

	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	TestNotNull(TEXT("payload subsystem exists"), Payloads);
	TestTrue(TEXT("initialization walk binds an already-loaded payload wave"),
		EarlyWave->IsPayloadBound());
	TestEqual(TEXT("bound channel metadata is visible on the wave"),
		EarlyWave->NumChannels, 1);
	TestEqual(TEXT("bound sample rate is visible on the wave"),
		static_cast<int32>(EarlyWave->GetSampleRateForCurrentPlatform()), 8000);
	TestEqual(TEXT("bound frame count is visible on the wave"),
		static_cast<int32>(EarlyWave->GetNumFrames()), 4);

#if UE_USOUNDWAVE_HOT_RELOADING_ENABLED
	// Desktop non-Shipping holds separate working/published FSoundWaveData. The
	// procedural superclass returns from PostLoad before proxy initialization,
	// so late mutation is intentionally not treated as a proxy publication seam.
	const TSharedRef<const FSoundWaveData> ProxyData =
		EarlyWave->GetSoundWaveProxy()->GetSoundWaveDataRef();
	TestNotEqual(TEXT("late procedural channel metadata is not a proxy contract"),
		static_cast<int32>(ProxyData->GetNumChannels()), EarlyWave->NumChannels);
#endif

	FSoundGeneratorInitParams InitParams;
	ISoundGeneratorPtr First = EarlyWave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("bound wave creates a generator"), First.IsValid()))
	{
		float Samples[4] = {};
		TestEqual(TEXT("generator fills the complete source"),
			First->OnGenerateAudio(Samples, UE_ARRAY_COUNT(Samples)), 4);
		TestTrue(TEXT("PCM16 -32768 maps exactly to -1"),
			WoTPayloadSoundTests_NearlyEqual(Samples[0], -1.0f));
		TestTrue(TEXT("PCM16 -16384 maps exactly to -0.5"),
			WoTPayloadSoundTests_NearlyEqual(Samples[1], -0.5f));
		TestTrue(TEXT("PCM16 zero maps exactly to zero"),
			WoTPayloadSoundTests_NearlyEqual(Samples[2], 0.0f));
		TestTrue(TEXT("PCM16 positive full scale uses the signed divisor"),
			WoTPayloadSoundTests_NearlyEqual(Samples[3], 32767.0f / 32768.0f));
		TestTrue(TEXT("non-looping generator finishes at the last sample"),
			First->IsFinished());
	}

	ISoundGeneratorPtr GeneratorA = EarlyWave->CreateSoundGenerator(InitParams);
	ISoundGeneratorPtr GeneratorB = EarlyWave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("two concurrent generators are created"),
		GeneratorA.IsValid() && GeneratorB.IsValid()))
	{
		float AFirst[2] = {};
		float BFirst[1] = {};
		float ASecond[2] = {};
		GeneratorA->OnGenerateAudio(AFirst, UE_ARRAY_COUNT(AFirst));
		GeneratorB->OnGenerateAudio(BFirst, UE_ARRAY_COUNT(BFirst));
		GeneratorA->OnGenerateAudio(ASecond, UE_ARRAY_COUNT(ASecond));
		TestTrue(TEXT("second playback starts at its own sample zero"),
			WoTPayloadSoundTests_NearlyEqual(BFirst[0], -1.0f));
		TestTrue(TEXT("first playback cursor independently reaches sample two"),
			WoTPayloadSoundTests_NearlyEqual(ASecond[0], 0.0f));
	}

	ISoundGeneratorPtr Replay = EarlyWave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("replay creates another fresh generator"), Replay.IsValid()))
	{
		float ReplaySample = 0.0f;
		Replay->OnGenerateAudio(&ReplaySample, 1);
		TestTrue(TEXT("replay restarts at sample zero"),
			WoTPayloadSoundTests_NearlyEqual(ReplaySample, -1.0f));
	}

	const FString LatePackageName = TEXT("/Game/__WoTPayloadSoundTest_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/LateTone");
	LateWave = WoTPayloadSoundTests_CreateWave(
		LatePackageName, TEXT("LateTone"), Fixture, /*bLooping*/ false);
	TestFalse(TEXT("late wave begins unbound"), LateWave->IsPayloadBound());
	if (Payloads)
	{
		const FWoTPayloadSoundTests_PackageLoadCompletedTag::Type Handler =
			WoTPayloadSoundTests_GetPrivateMember(
				FWoTPayloadSoundTests_PackageLoadCompletedTag());
		(Payloads->*Handler)(LateWave->GetOutermost());
	}
	TestTrue(TEXT("real package-completion handler binds a late in-memory wave"),
		LateWave->IsPayloadBound());

	const FString PostLoadPackageName = TEXT("/Game/__WoTPayloadSoundTest_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/PostLoadTone");
	PostLoadWave = WoTPayloadSoundTests_CreateWave(
		PostLoadPackageName, TEXT("PostLoadTone"), Fixture, /*bLooping*/ false);
	TestFalse(TEXT("PostLoad fixture begins unbound"),
		PostLoadWave->IsPayloadBound());
	PostLoadWave->PostLoad();
	TestTrue(TEXT("subclass PostLoad binds through the active process cache service"),
		PostLoadWave->IsPayloadBound());

	const FString LoopPackageName = TEXT("/Game/__WoTPayloadSoundTest_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/LoopTone");
	LoopWave = WoTPayloadSoundTests_CreateWave(
		LoopPackageName, TEXT("LoopTone"), Fixture, /*bLooping*/ true);
	if (Payloads)
	{
		TestTrue(TEXT("loop wave binds directly through authenticated subsystem seam"),
			Payloads->InjectSound(LoopWave, Fixture.CacheKey));
	}
	ISoundGeneratorPtr LoopGenerator =
		LoopWave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("looping wave creates a generator"),
		LoopGenerator.IsValid()))
	{
		float LoopSamples[6] = {};
		TestEqual(TEXT("looping generator fills across the wrap"),
			LoopGenerator->OnGenerateAudio(
				LoopSamples, UE_ARRAY_COUNT(LoopSamples)), 6);
		TestTrue(TEXT("loop wrap returns to sample zero"),
			WoTPayloadSoundTests_NearlyEqual(LoopSamples[4], -1.0f));
		TestTrue(TEXT("loop wrap continues with sample one"),
			WoTPayloadSoundTests_NearlyEqual(LoopSamples[5], -0.5f));
		TestFalse(TEXT("looping generator does not finish"), LoopGenerator->IsFinished());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundMultichannelGeneratorSpec,
	"WoT.Payload.Sound.MultichannelFrameAlignment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundMultichannelGeneratorSpec::RunTest(const FString&)
{
	auto RunFixture = [this](int32 Channels, const TCHAR* Label)
	{
		TArray<int16> SourceSamples;
		TArray<uint8> PCMBytes;
		for (int32 Frame = 0; Frame < 3; ++Frame)
		{
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				const int16 Sample = static_cast<int16>(
					1000 + Frame * 5000 + Channel * 250);
				SourceSamples.Add(Sample);
				WoTPayloadSoundTests_AppendPCM16(PCMBytes, Sample);
			}
		}

		FWoTPayloadSoundTests_Fixture Fixture =
			WoTPayloadSoundTests_WriteFixture(
				EWoTPayloadSoundTests_FixtureMode::ValidPCM16,
				Channels, /*FixtureFrames*/ 3, PCMBytes);
		UWoTPayloadSoundWave* Wave =
			WoTPayloadSoundTests_CreateCanonicalWave(Fixture, /*bLooping*/ true);
		FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
			Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
		TestTrue(*FString::Printf(TEXT("%s fixture publishes"), Label),
			Fixture.bPublished);
		TestTrue(*FString::Printf(TEXT("%s fixture activates"), Label),
			State.bActivated);
		ON_SCOPE_EXIT
		{
			WoTPayloadSoundTests_DestroyWorld(State);
			WoTPayloadSoundTests_DestroyWave(Wave);
			WoTPayloadSoundTests_DeleteFixture(Fixture);
		};

		FSoundGeneratorInitParams InitParams;
		ISoundGeneratorPtr Generator = Wave->CreateSoundGenerator(InitParams);
		if (!TestTrue(*FString::Printf(TEXT("%s generator is created"), Label),
			Generator.IsValid()))
		{
			return;
		}

		const int32 CallbackSamples = Channels * 2 + 1;
		TArray<float> First;
		TArray<float> Second;
		First.Init(17.0f, CallbackSamples);
		Second.Init(17.0f, CallbackSamples);
		TestEqual(*FString::Printf(TEXT("%s first callback is fully filled"), Label),
			Generator->OnGenerateAudio(First.GetData(), First.Num()), First.Num());
		TestEqual(*FString::Printf(TEXT("%s second callback is fully filled"), Label),
			Generator->OnGenerateAudio(Second.GetData(), Second.Num()), Second.Num());

		for (int32 SampleIndex = 0; SampleIndex < Channels * 2; ++SampleIndex)
		{
			const float Expected = static_cast<float>(SourceSamples[SampleIndex])
				/ 32768.0f;
			TestTrue(*FString::Printf(
				TEXT("%s callback one preserves interleaved sample %d"),
				Label, SampleIndex),
				WoTPayloadSoundTests_NearlyEqual(First[SampleIndex], Expected));
		}
		TestTrue(*FString::Printf(TEXT("%s callback one zeroes partial frame"), Label),
			WoTPayloadSoundTests_NearlyEqual(First.Last(), 0.0f));

		for (int32 Channel = 0; Channel < Channels; ++Channel)
		{
			const float LastFrame = static_cast<float>(
				SourceSamples[Channels * 2 + Channel]) / 32768.0f;
			const float WrappedFrame = static_cast<float>(SourceSamples[Channel])
				/ 32768.0f;
			TestTrue(*FString::Printf(
				TEXT("%s callback two starts on frame two channel %d"), Label, Channel),
				WoTPayloadSoundTests_NearlyEqual(Second[Channel], LastFrame));
			TestTrue(*FString::Printf(
				TEXT("%s loop wraps at a frame boundary before channel %d"), Label, Channel),
				WoTPayloadSoundTests_NearlyEqual(
					Second[Channels + Channel], WrappedFrame));
		}
		TestTrue(*FString::Printf(TEXT("%s callback two zeroes partial frame"), Label),
			WoTPayloadSoundTests_NearlyEqual(Second.Last(), 0.0f));
	};

	RunFixture(/*Channels*/ 2, TEXT("stereo"));
	RunFixture(/*Channels*/ 6, TEXT("six-channel"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundPCM8GeneratorSpec,
	"WoT.Payload.Sound.PCM8Generator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundPCM8GeneratorSpec::RunTest(const FString&)
{
	FWoTPayloadSoundTests_Fixture Fixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM8);
	UWoTPayloadSoundWave* Wave = WoTPayloadSoundTests_CreateCanonicalWave(Fixture);
	FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("PCM8 fixture publishes"), Fixture.bPublished);
	TestTrue(TEXT("PCM8 fixture activates"), State.bActivated);
	ON_SCOPE_EXIT
	{
		WoTPayloadSoundTests_DestroyWorld(State);
		WoTPayloadSoundTests_DestroyWave(Wave);
		WoTPayloadSoundTests_DeleteFixture(Fixture);
	};

	FSoundGeneratorInitParams InitParams;
	ISoundGeneratorPtr Generator = Wave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("bound PCM8 wave creates a generator"), Generator.IsValid()))
	{
		float Samples[4] = {};
		TestEqual(TEXT("PCM8 generator fills exact fixture length"),
			Generator->OnGenerateAudio(Samples, UE_ARRAY_COUNT(Samples)), 4);
		TestTrue(TEXT("PCM8 zero maps to -1"),
			WoTPayloadSoundTests_NearlyEqual(Samples[0], -1.0f));
		TestTrue(TEXT("PCM8 64 maps to -0.5"),
			WoTPayloadSoundTests_NearlyEqual(Samples[1], -0.5f));
		TestTrue(TEXT("PCM8 128 maps to zero"),
			WoTPayloadSoundTests_NearlyEqual(Samples[2], 0.0f));
		TestTrue(TEXT("PCM8 255 maps to 127/128"),
			WoTPayloadSoundTests_NearlyEqual(Samples[3], 127.0f / 128.0f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundBindingLifetimeSpec,
	"WoT.Payload.Sound.AuthenticatedGenerationBindingLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundBindingLifetimeSpec::RunTest(const FString&)
{
	FWoTPayloadSoundTests_Fixture OldFixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16);
	TArray<uint8> NewPCM;
	for (const int16 Sample : {static_cast<int16>(8192), static_cast<int16>(16384),
		static_cast<int16>(-8192), static_cast<int16>(0)})
	{
		WoTPayloadSoundTests_AppendPCM16(NewPCM, Sample);
	}
	FWoTPayloadSoundTests_Fixture NewFixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16,
		/*FixtureChannels*/ 1, /*FixtureFrames*/ 4, NewPCM,
		OldFixture.PackageName);
	UWoTPayloadSoundWave* Wave =
		WoTPayloadSoundTests_CreateCanonicalWave(OldFixture);
	FWoTPayloadSoundTests_World OldState;
	FWoTPayloadSoundTests_World OffState;
	FWoTPayloadSoundTests_World NewState;
	ON_SCOPE_EXIT
	{
		WoTPayloadSoundTests_DestroyWorld(NewState);
		WoTPayloadSoundTests_DestroyWorld(OffState);
		WoTPayloadSoundTests_DestroyWorld(OldState);
		WoTPayloadSoundTests_DestroyWave(Wave);
		WoTPayloadSoundTests_DeleteFixture(NewFixture);
		WoTPayloadSoundTests_DeleteFixture(OldFixture);
	};

	FSoundGeneratorInitParams InitParams;
	OldState = WoTPayloadSoundTests_CreateWorld(
		OldFixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("old cache-v3 generation activates"), OldState.bActivated);
	TestTrue(TEXT("first ON subsystem publishes the old payload"),
		Wave->IsPayloadBound());
	ISoundGeneratorPtr OldGenerator = Wave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("first ON subsystem creates an old-payload generator"),
		OldGenerator.IsValid()))
	{
		float OldFirst = 0.0f;
		TestEqual(TEXT("old generator supplies its first sample"),
			OldGenerator->OnGenerateAudio(&OldFirst, 1), 1);
		TestTrue(TEXT("old generator reads the old fixture"),
			WoTPayloadSoundTests_NearlyEqual(OldFirst, -1.0f));
	}

	WoTPayloadSoundTests_DestroyWorld(OldState);
	TestFalse(TEXT("tearing down the active subsystem invalidates wave binding"),
		Wave->IsPayloadBound());
	TestFalse(TEXT("a generator created after teardown cannot read stale PCM"),
		Wave->CreateSoundGenerator(InitParams).IsValid());
	if (OldGenerator.IsValid())
	{
		float CapturedSecond = 0.0f;
		TestEqual(TEXT("an existing generator retains its immutable snapshot"),
			OldGenerator->OnGenerateAudio(&CapturedSecond, 1), 1);
		TestTrue(TEXT("the retained snapshot remains the old fixture"),
			WoTPayloadSoundTests_NearlyEqual(CapturedSecond, -0.5f));
	}

	OffState = WoTPayloadSoundTests_CreateWorld(
		OldFixture.CacheDirectory, /*bInjectionEnabled*/ false);
	TestFalse(TEXT("an intervening OFF subsystem remains silent"),
		Wave->CreateSoundGenerator(InitParams).IsValid());
	WoTPayloadSoundTests_DestroyWorld(OffState);

	NewState = WoTPayloadSoundTests_CreateWorld(
		NewFixture.CacheDirectory, /*bInjectionEnabled*/ true);
	TestTrue(TEXT("replacement cache-v3 generation activates"),
		NewState.bActivated);
	TestTrue(TEXT("fresh ON subsystem republishes this wave from its cache"),
		Wave->IsPayloadBound());
	ISoundGeneratorPtr NewGenerator = Wave->CreateSoundGenerator(InitParams);
	if (TestTrue(TEXT("fresh ON subsystem creates a new-payload generator"),
		NewGenerator.IsValid()))
	{
		float NewFirst = 0.0f;
		TestEqual(TEXT("new generator supplies its first sample"),
			NewGenerator->OnGenerateAudio(&NewFirst, 1), 1);
		TestTrue(TEXT("new generator reads only the replacement fixture"),
			WoTPayloadSoundTests_NearlyEqual(NewFirst, 0.25f));
	}

	WoTPayloadSoundTests_DestroyWorld(NewState);
	TestFalse(TEXT("replacement generation teardown revokes its wave binding"),
		Wave->IsPayloadBound());
	TestFalse(TEXT("no generator can reacquire bytes after generation revocation"),
		Wave->CreateSoundGenerator(InitParams).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadSoundToggleOffSpec,
	"WoT.Payload.Sound.ToggleOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadSoundToggleOffSpec::RunTest(const FString&)
{
	FWoTPayloadSoundTests_Fixture Fixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16);
	UWoTPayloadSoundWave* Wave = WoTPayloadSoundTests_CreateCanonicalWave(Fixture);
	Wave->NumChannels = 7;
	Wave->SetSampleRate(12345);
	Wave->SetNumFrames(9);
	Wave->Duration = 0.25f;
	FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ false);
	ON_SCOPE_EXIT
	{
		WoTPayloadSoundTests_DestroyWorld(State);
		WoTPayloadSoundTests_DestroyWave(Wave);
		WoTPayloadSoundTests_DeleteFixture(Fixture);
	};

	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (TestNotNull(TEXT("disabled subsystem still exists"), Payloads))
	{
		TestFalse(TEXT("sound payload injection is off"),
			Payloads->IsInjectionEnabled());
		TestFalse(TEXT("disabled subsystem does not preflight the manifest"),
			Payloads->IsManifestReady());
		TestNull(TEXT("disabled subsystem exposes no sound entry"),
			Payloads->FindSoundPayload(Fixture.CacheKey));
	}
	TestFalse(TEXT("toggle OFF leaves wave unbound"), Wave->IsPayloadBound());
	TestEqual(TEXT("toggle OFF preserves channel metadata"), Wave->NumChannels, 7);
	TestEqual(TEXT("toggle OFF preserves sample-rate metadata"),
		static_cast<int32>(Wave->GetSampleRateForCurrentPlatform()), 12345);
	TestEqual(TEXT("toggle OFF preserves frame metadata"),
		static_cast<int32>(Wave->GetNumFrames()), 9);
	FSoundGeneratorInitParams InitParams;
	TestFalse(TEXT("unbound wave creates no payload generator"),
		Wave->CreateSoundGenerator(InitParams).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeSoundProviderSpec,
	"WoT.Assets.SoundCacheAndLoopPoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeSoundProviderSpec::RunTest(const FString&)
{
	FWoTPayloadSoundTests_Fixture Fixture = WoTPayloadSoundTests_WriteFixture(
		EWoTPayloadSoundTests_FixtureMode::ValidPCM16, 1, 4,
		TArray<uint8>(), FString(), /*bHeaderLoop*/ true);
	FWoTPayloadSoundTests_World State = WoTPayloadSoundTests_CreateWorld(
		Fixture.CacheDirectory, /*bInjectionEnabled*/ true);
	ON_SCOPE_EXIT
	{
		WoTPayloadSoundTests_DestroyWorld(State);
		WoTPayloadSoundTests_DeleteFixture(Fixture);
	};
	TestTrue(TEXT("runtime provider fixture publishes"), Fixture.bPublished);
	TestTrue(TEXT("runtime provider fixture activates"), State.bActivated);
	UWoTRuntimeAssets* Assets = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTRuntimeAssets>() : nullptr;
	if (!TestNotNull(TEXT("runtime asset subsystem exists"), Assets)) return false;
	const FSoftObjectPath Path(FString::Printf(
		TEXT("/Game/WoT/Sounds/%s/%s.%s"), *Fixture.PackageName,
		*Fixture.AssetName, *Fixture.AssetName));
	UWoTPayloadSoundWave* NonUsageLoop = Assets->ResolveSound(Path, false);
	UWoTPayloadSoundWave* UsageLoop = Assets->ResolveSound(Path, true);
	TestNotNull(TEXT("non-usage variant resolves"), NonUsageLoop);
	TestNotNull(TEXT("usage-loop variant resolves"), UsageLoop);
	TestTrue(TEXT("(id, loop) variants are distinct"),
		NonUsageLoop != nullptr && UsageLoop != nullptr
		&& NonUsageLoop != UsageLoop);
	TestEqual(TEXT("same (id, loop) request is cached"),
		Assets->ResolveSound(Path, true), UsageLoop);
	TestEqual(TEXT("two sound variants occupy two cache rows"),
		Assets->SoundCacheSizeForTesting(), 2);
#if !WOT_GUARDED_BOOTSTRAP
	TestNull(TEXT("non-guarded static path keeps cooked behavior during test activation"),
		UWoTRuntimeAssets::ResolveSoundPath(Path, true));
#endif
	if (UsageLoop)
	{
		TestEqual(TEXT("runtime wave keeps retail object name"),
			UsageLoop->GetFName(), FName(*Fixture.AssetName));
		TestTrue(TEXT("header loop makes the wave looping"), UsageLoop->bLooping != 0);
		FSoundGeneratorInitParams InitParams;
		ISoundGeneratorPtr Generator = UsageLoop->CreateSoundGenerator(InitParams);
		if (TestTrue(TEXT("loop-point wave creates a generator"), Generator.IsValid()))
		{
			float Samples[6] = {};
			TestEqual(TEXT("loop-point generator fills request"),
				Generator->OnGenerateAudio(Samples, UE_ARRAY_COUNT(Samples)), 6);
			TestTrue(TEXT("loop retains the pre-roll frame"),
				WoTPayloadSoundTests_NearlyEqual(Samples[0], -1.0f));
			TestTrue(TEXT("loop starts at authored frame one"),
				WoTPayloadSoundTests_NearlyEqual(Samples[3], -0.5f));
			TestTrue(TEXT("loop ends before authored exclusive frame three"),
				WoTPayloadSoundTests_NearlyEqual(Samples[4], 0.0f));
			TestTrue(TEXT("loop wraps back to authored start"),
				WoTPayloadSoundTests_NearlyEqual(Samples[5], -0.5f));
		}
	}
	Assets->ShutdownForPayloadDeactivation();
	TestTrue(TEXT("runtime assets tear down while the authenticated lease is still active"),
		IsWoTPayloadBootstrapActive());
	TestEqual(TEXT("sound cache is empty before payload lease revocation"),
		Assets->SoundCacheSizeForTesting(), 0);
	TestTrue(TEXT("runtime wave binding is removed before payload lease revocation"),
		!UsageLoop || !UsageLoop->IsPayloadBound());
	TestTrue(TEXT("consumer-held runtime wave remains valid after provider release"),
		IsValid(UsageLoop));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
