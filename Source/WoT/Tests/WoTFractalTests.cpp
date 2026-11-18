#include "WoTCacheTestSupport.h"
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"
#include "WoTPayloadActivationTestSupport.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "../Payload/WoTCacheStore.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../WoTFractalSim.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Modules/ModuleManager.h"

namespace
{
uint32 StandardCrc32(const TArray<uint8>& Bytes)
{
	uint32 Crc = 0xffffffffu;
	for (const uint8 Byte : Bytes)
	{
		Crc ^= Byte;
		for (int32 Bit = 0; Bit < 8; ++Bit)
		{
			Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
		}
	}
	return Crc ^ 0xffffffffu;
}

FWoTFractalTextureRecord MakeFlatWetRecord()
{
	FWoTFractalTextureRecord Record;
	Record.PackageName = TEXT("Fixture");
	Record.TextureName = TEXT("FlatWet");
	Record.FractalClass = EWoTFractalClass::Wet;
	Record.Width = 8;
	Record.Height = 8;
	Record.WaveAmp = 128;
	Record.InitialBitmap.SetNumZeroed(64);
	Record.SourceWidth = 8;
	Record.SourceHeight = 8;
	Record.SourceIndices.SetNumZeroed(64);
	Record.PaletteRGBA.SetNumZeroed(256 * 4);
	return Record;
}

FWoTFractalTextureRecord MakeSLWater02Record()
{
	// original/Textures/JHPFireEng.utx, WetTexture JHPFireEng.SLWater02.
	// Eight-byte FDrop array decoded by ue1pkg.decode_fractal; behavior is
	// the original Fire package's native WaterRedrawDrops/CalculateWater.
	static const TCHAR* DropsBase64 =
		TEXT("AUgpMwAAAAIBSCkzAAAAAgFIOk4AAAACAUg6TgAAAAIBSBtOAAAAAgFIG04AAAAC")
		TEXT("AUg/RgAAAAIBSD9GAAAAAgFIL1wAAAACAUgvXAAAAAICSGQrAAAAAgJIZCsAAAAC")
		TEXT("AkhLKQAAAAICSEspAAAAAgJITjMAAAACAkhPMgAAAAICSFAyAAAAAgJIUTIAAAAC")
		TEXT("AkhSMgAAAAICSFMyAAAAAgJIVDIAAAACAkhWMwAAAAICSFc0AAAAAgJIWDUAAAAC")
		TEXT("AkhZNwAAAAICSFo6AAAAAgJIWkEAAAACAkhaRgAAAAICSFpLAAAAAgJIWk4AAAAC")
		TEXT("AkhZTwAAAAICSFdPAAAAAgJIVFAAAAACAkhTUAAAAAICSE9QAAAAAgJITFAAAAAC")
		TEXT("AkhJTwAAAAICSEZPAAAAAgJIQ04AAAACAkhATQAAAAICSD5KAAAAAgJIPUcAAAAC")
		TEXT("Akg9RgAAAAICSDxFAAAAAgJIPEUAAAACAkhMRgAAAAICSE1GAAAAAgJITkUAAAAC")
		TEXT("AkhPRAAAAAICSFBDAAAAAgJIUUMAAAACAkhRQgAAAAICSFJCAAAAAgJIUkEAAAAC")
		TEXT("AkhTQQAAAAICSFRBAAAAAgJIVUEAAAACAkhVQgAAAAICSFZDAAAAAgJIV0QAAAAC")
		TEXT("AkhYRAAAAAICSFhFAAAAAgJIWEYAAAACAkhYSAAAAAICSFhJAAAAAgJIV0kAAAAC")
		TEXT("AkhXSgAAAAICSFZKAAAAAgJIVkoAAAACAkg2JgAAAAICSDUlAAAAAgJINCQAAAAC")
		TEXT("AkgyJAAAAAICSC8jAAAAAgJILCIAAAACAkgpIgAAAAICSCghAAAAAgJIJyEAAAAC")
		TEXT("AkgnIAAAAAICSCggAAAAAgJIKSAAAAACAkgqIAAAAAICSCsgAAAAAgJILCAAAAAC")
		TEXT("AkgtIAAAAAICSC4gAAAAAgJILyEAAAACAkgvIgAAAAICSC8kAAAAAgJILyUAAAAC")
		TEXT("AkgtJgAAAAICSCwmAAAAAgJIKyYAAAACAkgrJQAAAAICSCskAAAAAgJIKyIAAAAC")
		TEXT("AkgsIQAAAAICSC0hAAAAAgJILiEAAAACAkgvIgAAAAICSDAiAAAAAgJIMSQAAAAC")
		TEXT("AkgxJQAAAAICSDEmAAAAAgJIMScAAAACAkgwKAAAAAICSC8oAAAAAgJILigAAAAC")
		TEXT("AkgtKAAAAAICSC0oAAAAAgJILGYAAAACAkgtZgAAAAICSC9nAAAAAgJIMmgAAAAC")
		TEXT("Akg1aAAAAAICSDhqAAAAAgJIO2sAAAACAkg8awAAAAICSD1rAAAAAgJIPmsAAAAC")
		TEXT("Akg/awAAAAICSEBrAAAAAgJIQWoAAAACAkhCaQAAAAICSEJnAAAAAgJIQmQAAAAC")
		TEXT("AkhCYwAAAAICSEFhAAAAAgJIQGAAAAACAkg/XwAAAAICSD1eAAAAAgJIPF0AAAAC")
		TEXT("Akg7XAAAAAICSDlbAAAAAgJIOFsAAAACAkg2WgAAAAICSDVZAAAAAgJIM1kAAAAC")
		TEXT("AkgyWQAAAAICSDFZAAAAAgJIMFkAAAACAkgvWgAAAAICSC5bAAAAAgJILV8AAAAC")
		TEXT("AkgsYAAAAAICSCxiAAAAAgJIK2MAAAACAkgrZAAAAAICSCplAAAAAgJIKWYAAAAC")
		TEXT("AkgoZgAAAAICSCdnAAAAAgJIJmcAAAACAkgkZwAAAAICSCNnAAAAAgJIImcAAAAC")
		TEXT("AkghZgAAAAICSCBlAAAAAgJIIGQAAAACAkgfYwAAAAICSB9hAAAAAgJIH2AAAAAC")
		TEXT("AkgfXgAAAAICSCBdAAAAAgJIIlwAAAACAkgjWwAAAAICSCNaAAAAAgJIJFoAAAAC")
		TEXT("AkglWgAAAAICSCZaAAAAAgJIJloAAAACEDMqKQAAAB4QMyopAAAAHgHIbEgAAAAG")
		TEXT("AchsSAAAAAYByFllAAAABgHIWWUAAAAGAchNWwAAAAYByE5bAAAABgHITlsAAAAG")
		TEXT("AchucAAAAAYByG9wAAAABgHIb3AAAAAGAch1YQAAAAYByHVhAAAABgHIcSQAAAAG")
		TEXT("AchxJAAAAAYByGkUAAAABgHIaRQAAAAGAchKFgAAAAYByEoWAAAABgHILwkAAAAG")
		TEXT("AcgvCQAAAAYByDIbAAAABgHIMRsAAAAGAcgwGwAAAAYByC4bAAAABgHILhsAAAAG")
		TEXT("AcgXFAAAAAYByBcUAAAABgHIDQkAAAAGAcgNCgAAAAYByA0KAAAABgHICisAAAAG")
		TEXT("AcgKLAAAAAYByAosAAAABgHIGzEAAAAGAcgbMQAAAAYByCEoAAAABgHIISgAAAAG")
		TEXT("AchKPgAAAAYByEo+AAAABgHILUAAAAAGAcgtQQAAAAYByC1BAAAABgHIIk0AAAAG")
		TEXT("AcgiTgAAAAYByCJOAAAABgHIOmwAAAAGAcg6bAAAAAYByCVqAAAABgHIJWoAAAAG")
		TEXT("AcgcVgAAAAYByBxWAAAABgHIMTgAAAAGAcgyOAAAAAYByDI4AAAABgHIWj4AAAAG")
		TEXT("AchaPgAAAAYByHVRAAAABgHIdVEAAAAGAchoXgAAAAYByGheAAAABgHITzwAAAAG")
		TEXT("AchPPAAAAAYByFNOAAAABgHIU04AAAAGAchyFQAAAAYByHIVAAAABgHIYggAAAAG")
		TEXT("AchiCAAAAAYByFkIAAAABgHIWQkAAAAGAchYCQAAAAYByFgJAAAABgHIVxMAAAAG")
		TEXT("AchXFAAAAAYByFcUAAAABgHINlIAAAAGAcg2UwAAAAYByDZVAAAABgHINlUAAAAG")
		TEXT("Acg9ZAAAAAYByD5lAAAABgHIPmUAAAAGAcgYXwAAAAY=");

	TArray<uint8> Bytes;
	FBase64::Decode(FString(DropsBase64), Bytes);

	FWoTFractalTextureRecord Record = MakeFlatWetRecord();
	Record.PackageName = TEXT("JHPFireEng");
	Record.TextureName = TEXT("SLWater02");
	Record.Width = 256;
	Record.Height = 256;
	Record.InitialBitmap.SetNumZeroed(256 * 256);
	Record.SourceWidth = 256;
	Record.SourceHeight = 256;
	Record.SourceIndices.SetNumUninitialized(256 * 256);
	for (int32 Index = 0; Index < Record.SourceIndices.Num(); ++Index)
	{
		Record.SourceIndices[Index] = static_cast<uint8>(Index & 255);
	}
	Record.Drops.Reserve(Bytes.Num() / 8);
	for (int32 Offset = 0; Offset + 7 < Bytes.Num(); Offset += 8)
	{
		FWoTFractalDrop& Drop = Record.Drops.AddDefaulted_GetRef();
		Drop.Type = Bytes[Offset + 0];
		Drop.Depth = Bytes[Offset + 1];
		Drop.X = Bytes[Offset + 2];
		Drop.Y = Bytes[Offset + 3];
		Drop.ByteA = Bytes[Offset + 4];
		Drop.ByteB = Bytes[Offset + 5];
		Drop.ByteC = Bytes[Offset + 6];
		Drop.ByteD = Bytes[Offset + 7];
	}
	return Record;
}

FWoTFractalTextureRecord MakeFlatFireRecord()
{
	FWoTFractalTextureRecord Record;
	Record.PackageName = TEXT("Fixture");
	Record.TextureName = TEXT("FlatFire");
	Record.FractalClass = EWoTFractalClass::Fire;
	Record.Width = 8;
	Record.Height = 8;
	Record.RenderHeat = 142;
	Record.bRising = true;
	Record.SparksLimit = 1024;
	Record.InitialBitmap.SetNumZeroed(64);
	Record.PaletteRGBA.SetNumZeroed(256 * 4);
	return Record;
}

FWoTFractalTextureRecord MakeFireWallSpriteBRecord()
{
	// original/System/WoTTraps.u, FireTexture
	// WoTTraps.FireWall.FireWallSpriteB. ue1pkg.decode_fractal proves the
	// serialized 64x128 mip0 is 8192 zero bytes; the saved 13-FSpark payload
	// below covers Blaze, OzHasSpoken, RampLightning and emitted transients.
	static const TCHAR* SparksBase64 =
		TEXT("Bf8fXD2BMgIE/xhYB7IyAgT/HFpuKTICBP8kW8epMgIE/ydcv/8yAgT/")
		TEXT("K1qKUDICBP8UXOSBMgIYACpYGTQyEBj/IlkADwUQGP8ZXQQBAxAY/yRX")
		TEXT("AgUAEBj/GlcABQMQGP8bWgALAhA=");
	TArray<uint8> Bytes;
	FBase64::Decode(FString(SparksBase64), Bytes);

	FWoTFractalTextureRecord Record = MakeFlatFireRecord();
	Record.PackageName = TEXT("WoTTraps");
	Record.TextureName = TEXT("FireWallSpriteB");
	Record.ObjectPath = TEXT("WoTTraps.FireWall.FireWallSpriteB");
	Record.Width = 64;
	Record.Height = 128;
	Record.MaxFrameRate = 90.0f;
	Record.PrimeCount = 20;
	Record.InitialBitmap.SetNumZeroed(64 * 128);
	for (int32 Offset = 0; Offset + 7 < Bytes.Num(); Offset += 8)
	{
		FWoTFractalSpark& Spark = Record.Sparks.AddDefaulted_GetRef();
		Spark.Type = Bytes[Offset + 0];
		Spark.Heat = Bytes[Offset + 1];
		Spark.X = Bytes[Offset + 2];
		Spark.Y = Bytes[Offset + 3];
		Spark.ByteA = Bytes[Offset + 4];
		Spark.ByteB = Bytes[Offset + 5];
		Spark.ByteC = Bytes[Offset + 6];
		Spark.ByteD = Bytes[Offset + 7];
	}
	return Record;
}

FWoTFractalTextureRecord MakeSLightning01Record()
{
	// original/System/Angreal.u, FireTexture
	// Angreal.Seeker.SLightning01. RenderHeat is absent from the raw property
	// tags, so FireTexture's empty defaultproperties supplies zero. The saved
	// 13-FSpark payload and oracle constants are shared with the Python fixture.
	static const TCHAR* SparksBase64 =
		TEXT("Gf9+fE1FYGAZ/318TiBgYBn/fX0AAGBgGf98fQAAYGAZ/3x+AABgYBn/")
		TEXT("fH8AAGBgGf99fwAAYGAZ/35/AABgYBn/f38AAGBgGf9/fgAAYGAZ/399")
		TEXT("AABgYBn/gH0AAGBgGf+AfgAAYGA=");
	TArray<uint8> Bytes;
	FBase64::Decode(FString(SparksBase64), Bytes);

	FWoTFractalTextureRecord Record;
	Record.PackageName = TEXT("Angreal");
	Record.TextureName = TEXT("SLightning01");
	Record.ObjectPath = TEXT("Angreal.Seeker.SLightning01");
	Record.FractalClass = EWoTFractalClass::Fire;
	Record.Width = 256;
	Record.Height = 256;
	Record.PrimeCount = 32;
	Record.SparksLimit = 1024;
	Record.InitialBitmap.SetNumZeroed(256 * 256);
	Record.PaletteRGBA.SetNumZeroed(256 * 4);
	for (int32 Offset = 0; Offset + 7 < Bytes.Num(); Offset += 8)
	{
		FWoTFractalSpark& Spark = Record.Sparks.AddDefaulted_GetRef();
		Spark.Type = Bytes[Offset + 0];
		Spark.Heat = Bytes[Offset + 1];
		Spark.X = Bytes[Offset + 2];
		Spark.Y = Bytes[Offset + 3];
		Spark.ByteA = Bytes[Offset + 4];
		Spark.ByteB = Bytes[Offset + 5];
		Spark.ByteC = Bytes[Offset + 6];
		Spark.ByteD = Bytes[Offset + 7];
	}
	return Record;
}

FWoTFractalTextureRecord MakeIceCrossPortRecord()
{
	// Movement state is shipped Angreal.AMAPearl.AMApearl01; compact source
	// fields pin MoveIcePosition + BlitTexIce without embedding two 256px mips.
	FWoTFractalTextureRecord Record;
	Record.PackageName = TEXT("Fixture");
	Record.TextureName = TEXT("PearlIce");
	Record.FractalClass = EWoTFractalClass::Ice;
	Record.Width = Record.Height = 8;
	Record.SourceWidth = Record.SourceHeight = 8;
	Record.GlassWidth = Record.GlassHeight = 8;
	Record.InitialBitmap.SetNumZeroed(64);
	Record.SourceIndices.SetNumUninitialized(64);
	Record.GlassIndices.SetNumUninitialized(64);
	Record.PaletteRGBA.SetNumZeroed(256 * 4);
	for (int32 Index = 0; Index < 64; ++Index)
	{
		Record.SourceIndices[Index] = static_cast<uint8>(Index);
		Record.GlassIndices[Index] = static_cast<uint8>(Index * 13 + 7);
	}
	Record.PanningStyle = 4;
	Record.TimeMethod = 1;
	Record.HorizPanSpeed = 150;
	Record.VertPanSpeed = 128;
	Record.Frequency = 11;
	Record.Amplitude = 30;
	Record.bMoveIce = true;
	Record.MasterCount = 252058.109375f;
	Record.UDisplace = -42432.67578125f;
	Record.VDisplace = 1536.0506591796875f;
	Record.UPosition = -42432.67578125f;
	Record.VPosition = 1529.0506591796875f;
	return Record;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalSerializedDefaultsSpec,
	"WoT.Fractal.SerializedDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalSerializedDefaultsSpec::RunTest(const FString& Parameters)
{
	const FWoTFractalTextureRecord Record;
	TestEqual(TEXT("PrimeCount class default"), Record.PrimeCount, 0);
	TestEqual(TEXT("MinFrameRate class default"), Record.MinFrameRate, 0.0f);
	TestEqual(TEXT("MaxFrameRate class default"), Record.MaxFrameRate, 0.0f);
	TestEqual(TEXT("WaveAmp class default"), Record.WaveAmp, static_cast<uint8>(0));
	TestEqual(TEXT("BumpMapLight class default"), Record.BumpMapLight, static_cast<uint8>(0));
	TestEqual(TEXT("BumpMapAngle class default"), Record.BumpMapAngle, static_cast<uint8>(0));
	TestEqual(TEXT("PhongRange class default"), Record.PhongRange, static_cast<uint8>(0));
	TestEqual(TEXT("PhongSize class default"), Record.PhongSize, static_cast<uint8>(0));
	TestEqual(TEXT("RenderHeat class default"), Record.RenderHeat, static_cast<uint8>(0));
	TestFalse(TEXT("bRising class default"), Record.bRising);
	TestEqual(TEXT("SparksLimit class default"), Record.SparksLimit, 0);
	TestEqual(TEXT("PanningStyle class default"), Record.PanningStyle, static_cast<uint8>(0));
	TestEqual(TEXT("TimeMethod class default"), Record.TimeMethod, static_cast<uint8>(0));
	TestEqual(TEXT("HorizPanSpeed class default"), Record.HorizPanSpeed, static_cast<uint8>(0));
	TestEqual(TEXT("VertPanSpeed class default"), Record.VertPanSpeed, static_cast<uint8>(0));
	TestEqual(TEXT("Frequency class default"), Record.Frequency, static_cast<uint8>(0));
	TestEqual(TEXT("Amplitude class default"), Record.Amplitude, static_cast<uint8>(0));
	TestFalse(TEXT("MoveIce class default"), Record.bMoveIce);
	TestEqual(TEXT("MasterCount class default"), Record.MasterCount, 0.0f);
	TestEqual(TEXT("UDisplace class default"), Record.UDisplace, 0.0f);
	TestEqual(TEXT("VDisplace class default"), Record.VDisplace, 0.0f);
	TestEqual(TEXT("UPosition class default"), Record.UPosition, 0.0f);
	TestEqual(TEXT("VPosition class default"), Record.VPosition, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalSLWater02ExactSpec,
	"WoT.Fractal.SLWater02Exact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalSLWater02ExactSpec::RunTest(const FString& Parameters)
{
	FWoTWaterFractalSim Sim;
	TestTrue(TEXT("fixture initializes"), Sim.Initialize(MakeSLWater02Record()));
	const int32 SampleOffsets[] = {3760, 22034, 37760, 57570, 58580, 59357};
	const uint32 ExpectedCrc[] = {0xab1b6528, 0x7a87976f, 0x8b5f4ed5};
	const uint8 ExpectedSamples[][6] = {
		{162, 244, 113, 241, 212, 221},
		{176, 20, 122, 217, 211, 214},
		{156, 250, 134, 0, 213, 220},
	};
	const int32 CheckSteps[] = {1, 8, 32};
	int32 CheckIndex = 0;
	for (int32 Step = 1; Step <= 32; ++Step)
	{
		Sim.ConstantTimeTick();
		if (Step == CheckSteps[CheckIndex])
		{
			const TArray<uint8>& Output = Sim.GetOutputIndices();
			TestEqual(FString::Printf(TEXT("step %d CRC32"), Step),
				StandardCrc32(Output), ExpectedCrc[CheckIndex]);
			for (int32 Sample = 0; Sample < UE_ARRAY_COUNT(SampleOffsets); ++Sample)
			{
				TestEqual(FString::Printf(TEXT("step %d sample %d"), Step, Sample),
					Output[SampleOffsets[Sample]], ExpectedSamples[CheckIndex][Sample]);
			}
			++CheckIndex;
			if (CheckIndex == UE_ARRAY_COUNT(CheckSteps))
			{
				break;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalFireWallSpriteBExactSpec,
	"WoT.Fractal.FireWallSpriteBExact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalFireWallSpriteBExactSpec::RunTest(const FString& Parameters)
{
	// Constants come from fractalfire_exact.py and are shared verbatim with
	// test_fractalfire_exact.py (FireWallSpriteBFixtureTests).
	FWoTFractalRandomStream Random;
	Random.Initialize(1);
	FWoTFireFractalSim Sim;
	Sim.SetRandomStream(&Random);
	TestTrue(TEXT("shipped fire fixture initializes"),
		Sim.Initialize(MakeFireWallSpriteBRecord()));
	const int32 SampleOffsets[] = {4641, 4963, 5217, 5406, 5660, 5912};
	const int32 CheckSteps[] = {1, 8, 32};
	const uint32 ExpectedCrc[] = {0xf458369a, 0x1aa3db6d, 0x38bee518};
	const uint8 ExpectedSamples[][6] = {
		{5, 23, 35, 46, 56, 46},
		{26, 61, 75, 67, 162, 7},
		{68, 103, 134, 135, 107, 27},
	};
	int32 CheckIndex = 0;
	for (int32 Step = 1; Step <= 32; ++Step)
	{
		Sim.ConstantTimeTick();
		if (Step != CheckSteps[CheckIndex])
		{
			continue;
		}
		const TArray<uint8>& Output = Sim.GetOutputIndices();
		TestEqual(FString::Printf(TEXT("step %d CRC32"), Step),
			StandardCrc32(Output), ExpectedCrc[CheckIndex]);
		for (int32 Sample = 0; Sample < UE_ARRAY_COUNT(SampleOffsets); ++Sample)
		{
			TestEqual(FString::Printf(TEXT("step %d sample %d"), Step, Sample),
				Output[SampleOffsets[Sample]], ExpectedSamples[CheckIndex][Sample]);
		}
		++CheckIndex;
		if (CheckIndex == UE_ARRAY_COUNT(CheckSteps))
		{
			break;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalSLightning01ExactSpec,
	"WoT.Fractal.SLightning01Exact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalSLightning01ExactSpec::RunTest(const FString& Parameters)
{
	// Constants are regenerated from fractalfire_exact.py's raw Angreal.u
	// fixture and shared verbatim with SLightning01FixtureTests.
	FWoTFractalRandomStream Random;
	Random.Initialize(1);
	FWoTFireFractalSim Sim;
	Sim.SetRandomStream(&Random);
	const FWoTFractalTextureRecord Record = MakeSLightning01Record();
	TestEqual(TEXT("absent RenderHeat resolves to zero"),
		Record.RenderHeat, static_cast<uint8>(0));
	TestTrue(TEXT("shipped lightning fixture initializes"), Sim.Initialize(Record));
	const int32 SampleOffsets[] = {23160, 27781, 30596, 33656, 36764, 40542};
	const int32 CheckSteps[] = {1, 8, 32};
	const uint32 ExpectedCrc[] = {0x247faf43, 0x12a4ee5c, 0x38e45758};
	const int32 ExpectedNonZero[] = {670, 1342, 1968};
	const uint8 ExpectedSamples[][6] = {
		{13, 0, 0, 0, 16, 0},
		{12, 0, 83, 0, 0, 0},
		{0, 31, 0, 37, 0, 5},
	};
	int32 CheckIndex = 0;
	for (int32 Step = 1; Step <= 32; ++Step)
	{
		Sim.ConstantTimeTick();
		if (Step != CheckSteps[CheckIndex])
		{
			continue;
		}
		const TArray<uint8>& Output = Sim.GetOutputIndices();
		TestEqual(FString::Printf(TEXT("step %d CRC32"), Step),
			StandardCrc32(Output), ExpectedCrc[CheckIndex]);
		int32 NonZero = 0;
		for (const uint8 Value : Output)
		{
			NonZero += Value != 0;
		}
		TestEqual(FString::Printf(TEXT("step %d nonzero pixels"), Step),
			NonZero, ExpectedNonZero[CheckIndex]);
		for (int32 Sample = 0; Sample < UE_ARRAY_COUNT(SampleOffsets); ++Sample)
		{
			TestEqual(FString::Printf(TEXT("step %d sample %d"), Step, Sample),
				Output[SampleOffsets[Sample]], ExpectedSamples[CheckIndex][Sample]);
		}
		++CheckIndex;
		if (CheckIndex == UE_ARRAY_COUNT(CheckSteps))
		{
			break;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalIceCrossPortSpec,
	"WoT.Fractal.IceCrossPort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalIceCrossPortSpec::RunTest(const FString& Parameters)
{
	// Constants come from fractalice.py and are shared verbatim with
	// test_fractalice.py (test_cross_port_slide_phase_and_blit_crc).
	FWoTIceFractalSim Sim;
	TestTrue(TEXT("ice fixture initializes"),
		Sim.Initialize(MakeIceCrossPortRecord()));
	Sim.ConstantTimeTick();
	TestEqual(TEXT("one fixed virtual tick"), Sim.GetStepCount(), 1);
	TestEqual(TEXT("MasterCount phase"), Sim.GetMasterCount(), 252059.109375f);
	TestEqual(TEXT("U slide phase"), Sim.GetUPosition(), -42433.04296875f);
	TestEqual(TEXT("V wobble phase"), Sim.GetVPosition(), 1529.0506591796875f);
	const TArray<uint8>& Output = Sim.GetOutputIndices();
	TestEqual(TEXT("BlitTexIce CRC32"), StandardCrc32(Output), 0x60d7a4e1u);
	const int32 Offsets[] = {0, 7, 9, 27, 48, 63};
	const uint8 Expected[] = {2, 4, 8, 28, 50, 60};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Offsets); ++Index)
	{
		TestEqual(FString::Printf(TEXT("ice sample %d"), Index),
			Output[Offsets[Index]], Expected[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalRealTimeIcePrimeSpec,
	"WoT.Fractal.RealTimeIcePrime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalRealTimeIcePrimeSpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	FWoTFractalTextureRecord Record = MakeIceCrossPortRecord();
	Record.TextureName = TEXT("RealTimeIce");
	Record.ObjectPath = TEXT("Fixture.RealTimeIce");
	TestTrue(TEXT("real-time Ice activates"), Subsystem->ActivateRecord(Record));
	TestEqual(TEXT("activation performs no fixed step"),
		Subsystem->GetStepCount(TEXT("Fixture.RealTimeIce")), 0);
	float MasterCount = 0.0f;
	float UPosition = 0.0f;
	float VPosition = 0.0f;
	TestTrue(TEXT("activation position is available"),
		Subsystem->GetIcePositionForTesting(
			TEXT("Fixture.RealTimeIce"), MasterCount, UPosition, VPosition));
	TestEqual(TEXT("activation preserves MasterCount"), MasterCount, Record.MasterCount);
	TestEqual(TEXT("activation preserves UPosition"), UPosition, Record.UPosition);
	TestEqual(TEXT("activation preserves VPosition"), VPosition, Record.VPosition);

	IConsoleVariable* Freeze = IConsoleManager::Get().FindConsoleVariable(
		TEXT("wot.FractalFreeze"));
	TestNotNull(TEXT("freeze cvar registered"), Freeze);
	Freeze->Set(1, ECVF_SetByCode);
	Subsystem->Tick(0.016f);
	Freeze->Set(0, ECVF_SetByCode);
	Subsystem->Tick(0.016f);
	TestEqual(TEXT("unfreeze performs no fixed step"),
		Subsystem->GetStepCount(TEXT("Fixture.RealTimeIce")), 0);
	TestTrue(TEXT("unfreeze position is available"),
		Subsystem->GetIcePositionForTesting(
			TEXT("Fixture.RealTimeIce"), MasterCount, UPosition, VPosition));
	TestEqual(TEXT("unfreeze preserves MasterCount"), MasterCount, Record.MasterCount);
	TestEqual(TEXT("unfreeze preserves UPosition"), UPosition, Record.UPosition);
	TestEqual(TEXT("unfreeze preserves VPosition"), VPosition, Record.VPosition);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalTextureTickSpec,
	"WoT.Fractal.TextureTickSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalTextureTickSpec::RunTest(const FString& Parameters)
{
	FWoTFractalTextureRecord Record = MakeFlatWetRecord();
	Record.PrimeCount = 3;
	FWoTWaterFractalSim Sim;
	TestTrue(TEXT("flat fixture initializes"), Sim.Initialize(Record));
	TestEqual(TEXT("prime starts at zero"), Sim.GetPrimeCurrent(), 0);
	int32 SourceLocks = 0;
	TestEqual(TEXT("unlimited tick primes then updates"),
		Sim.TickTexture(0.0f, [&SourceLocks]() { ++SourceLocks; }), 4);
	TestEqual(TEXT("Wet source locks once before every constant step"),
		SourceLocks, 4);
	TestEqual(TEXT("prime completes"), Sim.GetPrimeCurrent(), 3);
	TestEqual(TEXT("step count includes prime and update"), Sim.GetStepCount(), 4);

	Record.PrimeCount = 0;
	Record.MinFrameRate = 30.0f;
	Record.MaxFrameRate = 30.0f;
	TestTrue(TEXT("fixed-rate reinitialize"), Sim.Initialize(Record));
	TestEqual(TEXT("fixed-rate first slice skips"), Sim.TickTexture(0.02f), 0);
	TestEqual(TEXT("fixed-rate catch-up updates"), Sim.TickTexture(0.02f), 1);
	TestTrue(TEXT("equal-rate catch-up subtracts one max interval"),
		FMath::IsNearlyEqual(Sim.GetAccumulator(), 0.04f - 1.0f / 30.0f));

	Record.MinFrameRate = 15.0f;
	Record.MaxFrameRate = 20.0f;
	TestTrue(TEXT("ranged-rate reinitialize"), Sim.Initialize(Record));
	TestEqual(TEXT("below min time skips"), Sim.TickTexture(0.04f), 0);
	TestEqual(TEXT("between thresholds updates"), Sim.TickTexture(0.02f), 1);
	TestEqual(TEXT("large delta performs one catch-up"), Sim.TickTexture(0.2f), 1);
	TestTrue(TEXT("catch-up accumulator clamps to max time"),
		FMath::IsNearlyEqual(Sim.GetAccumulator(), 1.0f / 15.0f));
	TestEqual(TEXT("clamped debt drains one step"), Sim.TickTexture(0.0f), 1);

	FWoTFractalTextureRecord Grouped = MakeFlatWetRecord();
	Grouped.ObjectPath = TEXT("WarrenT.Effects.Rain3");
	TestEqual(TEXT("full UE1 group identity is authoritative"),
		Grouped.QualifiedName(), FString(TEXT("WarrenT.Effects.Rain3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalChainLockFixtureSpec,
	"WoT.Fractal.ChainLockFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalChainLockFixtureSpec::RunTest(const FString& Parameters)
{
	// Cross-port pin shared with test_fractalwater_exact.py
	// (DependencyChainCrossPortFixtureTests): Wave source (PrimeCount=3, one
	// D_FIXED drop) feeding a Wet parent (PrimeCount=2, one D_FIXED drop),
	// driven with UpdateStateAtTime lock semantics and
	// TextureConstructionEpsilon construction stamps (source first, per
	// UE1's texture tick order — the source's authored prime runs during the
	// parent's FIRST lock, not one interval late).
	FWoTFractalTextureRecord SourceRecord = MakeFlatWetRecord();
	SourceRecord.TextureName = TEXT("ChainWave");
	SourceRecord.FractalClass = EWoTFractalClass::Wave;
	SourceRecord.PrimeCount = 3;
	SourceRecord.BumpMapLight = 50;
	SourceRecord.BumpMapAngle = 170;
	SourceRecord.PhongRange = 180;
	SourceRecord.PhongSize = 32;
	{
		FWoTFractalDrop& Drop = SourceRecord.Drops.AddDefaulted_GetRef();
		Drop.Type = 0;  // D_FIXED: deterministic, consumes no RNG
		Drop.X = 2;
		Drop.Y = 3;
		Drop.ByteD = 200;
	}
	FWoTFractalTextureRecord ParentRecord = MakeFlatWetRecord();
	ParentRecord.TextureName = TEXT("ChainWet");
	ParentRecord.PrimeCount = 2;
	{
		FWoTFractalDrop& Drop = ParentRecord.Drops.AddDefaulted_GetRef();
		Drop.Type = 0;
		Drop.X = 5;
		Drop.Y = 1;
		Drop.ByteD = 240;
	}

	FWoTWaterFractalSim Source;
	FWoTWaterFractalSim Parent;
	TestTrue(TEXT("source initializes"), Source.Initialize(SourceRecord));
	TestTrue(TEXT("parent initializes"), Parent.Initialize(ParentRecord));

	constexpr double Epsilon = 0.000001;
	double SourceTime = 1 * Epsilon;  // activation serial 1 (source first)
	double ParentTime = 2 * Epsilon;

	auto LockSource = [&](const double Time)
	{
		if (Time == SourceTime)
		{
			return;
		}
		Source.TickTexture(static_cast<float>(Time - SourceTime));
		SourceTime = Time;
	};
	auto LockParent = [&](const double Time)
	{
		if (Time == ParentTime)
		{
			return;
		}
		const double Old = ParentTime;
		Parent.TickTexture(static_cast<float>(Time - Old), [&]()
		{
			LockSource(Old);
			Parent.UpdateSourceIndices(Source.GetOutputIndices());
		});
		ParentTime = Time;
	};

	LockParent(1.0 / 32.0);
	TestEqual(TEXT("source primes on FIRST parent lock"), Source.GetStepCount(), 4);
	TestEqual(TEXT("parent primes and steps once"), Parent.GetStepCount(), 3);
	TestEqual(TEXT("lock1 source CRC"),
		StandardCrc32(Source.GetOutputIndices()), 0x82fcf3efu);
	TestEqual(TEXT("lock1 composed parent CRC"),
		StandardCrc32(Parent.GetOutputIndices()), 0x95c453d7u);
	LockParent(1.0 / 32.0);
	TestEqual(TEXT("repeated lock time is one update"), Source.GetStepCount(), 4);
	TestEqual(TEXT("repeated lock does not step parent"), Parent.GetStepCount(), 3);
	LockParent(2.0 / 32.0);
	TestEqual(TEXT("second lock advances source once"), Source.GetStepCount(), 5);
	TestEqual(TEXT("second lock advances parent once"), Parent.GetStepCount(), 4);
	TestEqual(TEXT("lock2 source CRC"),
		StandardCrc32(Source.GetOutputIndices()), 0xcd3cbd02u);
	TestEqual(TEXT("lock2 composed parent CRC"),
		StandardCrc32(Parent.GetOutputIndices()), 0xc5f335a9u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalRootCoverageSpec,
	"WoT.Fractal.RootCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalRootCoverageSpec::RunTest(const FString& Parameters)
{
	// Sheet-scrub retirement: coverage means IDENTITY membership — every
	// fractal-bearing master keeps its parent and swaps Tex at bind.
	auto CheckRoot = [this](const FName Root)
	{
		TestTrue(FString::Printf(TEXT("root %s is covered"), *Root.ToString()),
			UWoTFractalSubsystem::UsesIdentityUVRoot(Root));
	};

	// Fast fallback for fixtures that intentionally omit the generated catalog.
	const FName PinnedRoots[] = {
		TEXT("M_WoT_World"), TEXT("M_WoT_WorldMasked"),
		TEXT("M_WoT_WorldWave"), TEXT("M_WoT_WorldWaveMasked"),
		TEXT("M_WoT_WorldT_SceneBlend"),
		TEXT("M_WoT_WorldTranslucent"), TEXT("M_WoT_WorldModulated"),
		TEXT("M_WoT_MoverLit"), TEXT("M_WoT_MoverLitMasked"),
		TEXT("M_WoT_MeshLit"), TEXT("M_WoT_MeshLit2S"),
		TEXT("M_WoT_MeshLitMasked"), TEXT("M_WoT_MeshLitT"),
		TEXT("M_WoT_MeshTranslucent"), TEXT("M_WoT_MeshModulate"),
		TEXT("M_WoT_ParticleAdd"), TEXT("M_WoT_ParticleMod"),
		TEXT("M_WoT_ParticleAddFB"), TEXT("M_WoT_ParticleModFB"),
		TEXT("M_WoT_MeshFBAddGlow"),
		TEXT("M_WoT_FractalLiveLit"), TEXT("M_WoT_FractalLiveAdd"),
	};
	UWoTFractalCatalog* Catalog = LoadObject<UWoTFractalCatalog>(nullptr,
		TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog"));
	if (!Catalog)
	{
		for (const FName Root : PinnedRoots)
		{
			CheckRoot(Root);
		}
		return true;
	}

	// Census pivot (the sheet-referencer walk died with the sheets): every
	// importer-authored instance NAME derived from the catalog identities —
	// bare MI_<Tex> plus the qualified MI_PSAddFB_/MI_PSModFB_/MI_MeshFB_ and
	// per-map MI_W_ families — the exact naming FindRecordByMaterialName keys.
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.ScanPathsSynchronous({TEXT("/Game/WoT/Materials")});
	TSet<FString> BareNames;
	TSet<FString> IdentitySlugs;
	for (const FWoTFractalTextureRecord& Record : Catalog->Textures)
	{
		BareNames.Add(FString::Printf(TEXT("MI_%s"),
			*Record.TextureName.ToString()));
		IdentitySlugs.Add(UWoTFractalSubsystem::MakeFractalIdentityKey(
			Record.PackageName, Record.TextureName).ToString());
		// Pre-identity-refactor per-map instances carry the BARE texture slug
		// (MI_W_<Tex>_2S alongside MI_W_<Pkg>__<Tex>_2S) — census both forms.
		IdentitySlugs.Add(Record.TextureName.ToString());
	}

	TArray<FAssetData> Candidates;
	AssetRegistry.GetAssetsByPath(
		TEXT("/Game/WoT/Materials/Instances"), Candidates, /*bRecursive*/ true);
	TSet<FSoftObjectPath> DiscoveredInstances;
	TSet<FName> DiscoveredRoots;
	static const TCHAR* QualifiedPrefixes[] = {
		TEXT("MI_PSAddFB_"), TEXT("MI_PSModFB_"), TEXT("MI_MeshFB_"),
		TEXT("MI_W_")};
	for (const FAssetData& Asset : Candidates)
	{
		const FString AssetName = Asset.AssetName.ToString();
		bool bFractalNamed = BareNames.Contains(AssetName);
		for (const TCHAR* Prefix : QualifiedPrefixes)
		{
			if (bFractalNamed)
			{
				break;
			}
			if (AssetName.StartsWith(Prefix))
			{
				const FString Tail = AssetName.Mid(FCString::Strlen(Prefix));
				for (const FString& Slug : IdentitySlugs)
				{
					if (Tail.StartsWith(Slug))
					{
						bFractalNamed = true;
						break;
					}
				}
			}
		}
		if (!bFractalNamed)
		{
			continue;
		}
		UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset.GetAsset());
		if (!Instance)
		{
			continue;
		}
		DiscoveredInstances.Add(Asset.GetSoftObjectPath());
		if (UMaterial* Root = Instance->GetMaterial())
		{
			DiscoveredRoots.Add(Root->GetFName());
		}
	}
	for (const FName Root : DiscoveredRoots)
	{
		CheckRoot(Root);
	}
	AddInfo(FString::Printf(TEXT("census: %d records, %d instances, %d roots"),
		Catalog->Textures.Num(), DiscoveredInstances.Num(), DiscoveredRoots.Num()));
	// G43 (2026-07-24): the catalog regenerated from the FULL retail sweep —
	// the prior 46-record catalog predated the Angreal/ice bake (it froze the
	// AMA dome on a static frame). 83 = bake_flipbooks.py over every retail
	// utx/u package; the instance/root growth is EXISTING importer-authored
	// instances newly matched by the wider record name set (no assets were
	// created by the reimport), and every discovered root passes CheckRoot.
	TestEqual(TEXT("catalog record census"), Catalog->Textures.Num(), 83);
	// The identity-name census supersedes the retired sheet-referencer walk
	// (176 instances / 11 root families at retirement, which counted the
	// dying sheet-Tex bindings); the name pivot counts the importer-authored
	// consumer instances for the catalog's runtime records across both
	// per-map naming generations.
	// G45 _SL re-import (2026-07-24): the special-lit slot split added three
	// _SL variants of record-bearing wet/lava textures (Arena_07 LavaWet01,
	// Arena_08 SLWater02, Arena_12 SLWater01) — same (package,texture)
	// identity, so they join the live-sim split with their own gated MICs.
	TestEqual(TEXT("identity-named instance census"),
		DiscoveredInstances.Num(), 178);
	TestEqual(TEXT("root-family census"), DiscoveredRoots.Num(), 11);
	TestTrue(TEXT("the regenerated FB particle master is identity-covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(TEXT("M_WoT_ParticleAddFB")));
	TestTrue(TEXT("the shared live-lit master is identity-covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(TEXT("M_WoT_FractalLiveLit")));
	TestFalse(TEXT("the retired sheet master is NOT covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(TEXT("M_WoT_FlipbookLit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalDynamicMaterialBindingSpec,
	"WoT.Fractal.DynamicMaterialBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalDynamicMaterialBindingSpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	UMaterialInterface* GridMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshFBAddGlow.M_WoT_MeshFBAddGlow"));
	TestNotNull(TEXT("gameplay grid master exists"), GridMaster);
	if (!GridMaster)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	UMaterialInstanceDynamic* GameplayMID =
		UMaterialInstanceDynamic::Create(GridMaster, Actor);
	GameplayMID->SetScalarParameterValue(TEXT("Glow"), 0.25f);
	Component->SetMaterial(0, GameplayMID);

	FWoTFractalTextureRecord Record = MakeFlatWetRecord();
	Record.TextureName = TEXT("DynamicWet");
	Record.PrimeCount = 2;
	TestTrue(TEXT("dynamic material slot binds"),
		Subsystem->BindMaterialSlotForTesting(Component, 0, Record));
	TestTrue(TEXT("component retains the gameplay-owned MID pointer"),
		Component->GetMaterial(0) == GameplayMID);
	UTexture2D* LiveTexture = Subsystem->FindLiveTexture(TEXT("Fixture.DynamicWet"));
	UTexture* BoundTexture = nullptr;
	TestTrue(TEXT("Tex resolves after in-place mutation"),
		GameplayMID->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture));
	TestTrue(TEXT("Tex carries the transient live texture"),
		BoundTexture == LiveTexture);
	// Sheet-scrub retirement: the regenerated master samples Tex at identity
	// UV — no grid parameters exist to neutralize.
	TestTrue(TEXT("the gameplay master is identity-covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(GridMaster->GetFName()));
	TestEqual(TEXT("activation primes and performs its first constant step"),
		Subsystem->GetStepCount(TEXT("Fixture.DynamicWet")), 3);

	GameplayMID->SetScalarParameterValue(TEXT("Glow"), 0.75f);
	float RenderedGlow = 0.0f;
	TestTrue(TEXT("post-bind gameplay Glow write resolves"),
		Component->GetMaterial(0)->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("Glow")), RenderedGlow));
	TestEqual(TEXT("post-bind gameplay Glow write remains rendered"),
		RenderedGlow, 0.75f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalWetFireChainSpec,
	"WoT.Fractal.WetFireChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalWetFireChainSpec::RunTest(const FString& Parameters)
{
	// Activation regression: a Wet parent must lock a Fire source through the
	// same BeforeStep/output-index seam previously limited to Wet/Wave.
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	FWoTFractalTextureRecord Fire = MakeFlatFireRecord();
	Fire.TextureName = TEXT("ChainFire");
	Fire.ObjectPath = TEXT("Fixture.ChainFire");
	FWoTFractalSpark& Burn = Fire.Sparks.AddDefaulted_GetRef();
	Burn.Type = 0;
	Burn.X = 2;
	Burn.Y = 6;
	FWoTFractalTextureRecord Wet = MakeFlatWetRecord();
	Wet.TextureName = TEXT("ChainWetFire");
	Wet.ObjectPath = TEXT("Fixture.ChainWetFire");
	Wet.SourceTextureRef = Fire.ObjectPath;
	Wet.bSourceIsFractal = true;
	Subsystem->RegisterRecordForTesting(Fire);
	TestTrue(TEXT("Wet parent activates through Fire source"),
		Subsystem->ActivateRecord(Wet));
	TestEqual(TEXT("parent and dependency are live"),
		Subsystem->GetActiveTextureCount(), 2);
	TestEqual(TEXT("Fire dependency receives bind prime/first step"),
		Subsystem->GetStepCount(TEXT("Fixture.ChainFire")), 1);
	Subsystem->SetRenderGateBypassForTesting(true);
	Subsystem->Tick(0.016f);
	TestEqual(TEXT("Wet BeforeStep advances and samples Fire"),
		Subsystem->GetStepCount(TEXT("Fixture.ChainFire")), 2);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalIceDependencySpec,
	"WoT.Fractal.IceDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalIceDependencySpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	FWoTFractalTextureRecord Source = MakeFlatFireRecord();
	Source.TextureName = TEXT("IceSourceFire");
	Source.ObjectPath = TEXT("Fixture.IceSourceFire");
	FWoTFractalTextureRecord Glass = MakeFlatFireRecord();
	Glass.TextureName = TEXT("IceGlassFire");
	Glass.ObjectPath = TEXT("Fixture.IceGlassFire");
	FWoTFractalTextureRecord Ice = MakeIceCrossPortRecord();
	Ice.TextureName = TEXT("DependentIce");
	Ice.ObjectPath = TEXT("Fixture.DependentIce");
	Ice.TimeMethod = 0;
	Ice.SourceTextureRef = Source.ObjectPath;
	Ice.bSourceIsFractal = true;
	Ice.GlassTextureRef = Glass.ObjectPath;
	Ice.bGlassIsFractal = true;
	Subsystem->RegisterRecordForTesting(Source);
	Subsystem->RegisterRecordForTesting(Glass);
	TestTrue(TEXT("Ice activates both procedural inputs"),
		Subsystem->ActivateRecord(Ice));
	TestEqual(TEXT("Ice plus two dependencies are live"),
		Subsystem->GetActiveTextureCount(), 3);
	TestEqual(TEXT("source bind step"),
		Subsystem->GetStepCount(TEXT("Fixture.IceSourceFire")), 1);
	TestEqual(TEXT("glass bind step"),
		Subsystem->GetStepCount(TEXT("Fixture.IceGlassFire")), 1);
	Subsystem->SetRenderGateBypassForTesting(true);
	Subsystem->Tick(0.016f);
	TestEqual(TEXT("Ice tick locks source"),
		Subsystem->GetStepCount(TEXT("Fixture.IceSourceFire")), 2);
	TestEqual(TEXT("Ice tick locks glass"),
		Subsystem->GetStepCount(TEXT("Fixture.IceGlassFire")), 2);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalSubsystemLifecycleSpec,
	"WoT.Fractal.SubsystemLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalSubsystemLifecycleSpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	TestNotNull(TEXT("world owns fractal subsystem"), Subsystem);
	TestEqual(TEXT("empty test map activates no catalog records"),
		Subsystem->GetActiveTextureCount(), 0);
	TestTrue(TEXT("supported record activates"),
		Subsystem->ActivateRecord(MakeFlatWetRecord()));
	TestEqual(TEXT("one record is live"), Subsystem->GetActiveTextureCount(), 1);
	TestNotNull(TEXT("activation creates transient texture"),
		Subsystem->FindLiveTexture(TEXT("Fixture.FlatWet")));
	TestEqual(TEXT("activation primes and performs its first constant step"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatWet")), 1);

	FWoTFractalTextureRecord FireRecord = MakeFlatFireRecord();
	FireRecord.PrimeCount = 2;
	TestTrue(TEXT("Fire record activates"),
		Subsystem->ActivateRecord(FireRecord));
	TestNotNull(TEXT("Fire activation creates transient texture"),
		Subsystem->FindLiveTexture(TEXT("Fixture.FlatFire")));
	TestEqual(TEXT("Fire activation performs its first constant step"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatFire")), 3);

	FWoTFractalTextureRecord Unsupported = MakeFlatWetRecord();
	Unsupported.TextureName = TEXT("AbstractWater");
	Unsupported.FractalClass = EWoTFractalClass::Water;
	TestFalse(TEXT("abstract WaterTexture remains unsupported"),
		Subsystem->ActivateRecord(Unsupported));
	FWoTFractalTextureRecord MissingSpark = MakeFlatFireRecord();
	MissingSpark.TextureName = TEXT("MissingSpark");
	MissingSpark.Sparks.AddDefaulted_GetRef().Type = 1;
	TestFalse(TEXT("absent authored spark type fails activation"),
		Subsystem->ActivateRecord(MissingSpark));
	TestEqual(TEXT("unsupported record does not add state"),
		Subsystem->GetActiveTextureCount(), 2);

	// Sheet-scrub retirement: the shared live masters ARE the authored parents
	// now — the swap is uniformly keep-parent, so coverage = identity set.
	TestTrue(TEXT("shared live lit master is identity-covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(TEXT("M_WoT_FractalLiveLit")));
	TestFalse(TEXT("the retired sheet-scrub master is not covered"),
		UWoTFractalSubsystem::UsesIdentityUVRoot(TEXT("M_WoT_FlipbookLit")));

	IConsoleVariable* Freeze = IConsoleManager::Get().FindConsoleVariable(
		TEXT("wot.FractalFreeze"));
	TestNotNull(TEXT("freeze cvar registered"), Freeze);
	Freeze->Set(1, ECVF_SetByCode);
	Subsystem->Tick(0.016f);
	TestTrue(TEXT("freeze transition is latched"), Subsystem->IsFrozen());
	const int32 FrozenSteps = Subsystem->GetStepCount(TEXT("Fixture.FlatWet"));
	TestEqual(TEXT("freeze advances the fixed pose count"), FrozenSteps, 32);
	TestEqual(TEXT("Fire freeze applies authored prime plus fixed pose"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatFire")), 34);
	Subsystem->Tick(0.5f);
	TestEqual(TEXT("frozen sim holds"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatWet")), FrozenSteps);
	Freeze->Set(0, ECVF_SetByCode);
	Subsystem->Tick(0.016f);
	TestFalse(TEXT("unfreeze resumes live mode"), Subsystem->IsFrozen());
	TestEqual(TEXT("Fire falling edge re-primes and takes one step"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatFire")), 3);
	// The falling edge re-primes and takes one constant step immediately. A
	// record with no rendered consumer must not advance again; retail never
	// locks an unrendered texture.
	TestEqual(TEXT("unrendered record holds after falling-edge prime"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatWet")), 1);
	Subsystem->SetRenderGateBypassForTesting(true);
	Subsystem->Tick(0.016f);
	TestEqual(TEXT("gated advance performs one unlimited-rate step"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatWet")), 2);
	Subsystem->SetRenderGateBypassForTesting(false);
	Subsystem->Tick(0.016f);
	TestEqual(TEXT("gate closes again without rendered consumers"),
		Subsystem->GetStepCount(TEXT("Fixture.FlatWet")), 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalIdentityBindingSpec,
	"WoT.Fractal.IdentityBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalIdentityBindingSpec::RunTest(const FString& Parameters)
{
	// Flipbook excision phase 2: surface->record binding keys on the stable
	// Pkg__Tex identity (importer material names / standalone texture paths),
	// never on the dying sheet asset (PORT_NOTES "Procedural textures animate when recently seen").
	TestEqual(TEXT("identity key folds '-' like the importers"),
		UWoTFractalSubsystem::MakeFractalIdentityKey(
			TEXT("Pkg1"), TEXT("Te-xA")),
		FName(TEXT("Pkg1__Te_xA")));

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();

	FWoTFractalTextureRecord RecordA = MakeFlatWetRecord();
	RecordA.PackageName = TEXT("Pkg1");
	RecordA.TextureName = TEXT("TexA");
	RecordA.ObjectPath = TEXT("Pkg1.TexA");
	FWoTFractalTextureRecord RecordB = MakeFlatWetRecord();
	RecordB.PackageName = TEXT("Pkg1");
	RecordB.TextureName = TEXT("Shared");
	RecordB.ObjectPath = TEXT("Pkg1.Shared");
	FWoTFractalTextureRecord RecordC = MakeFlatWetRecord();
	RecordC.PackageName = TEXT("Pkg2");
	RecordC.TextureName = TEXT("Shared");
	RecordC.ObjectPath = TEXT("Pkg2.Shared");
	Subsystem->RegisterRecordForTesting(RecordA);
	Subsystem->RegisterRecordForTesting(RecordB);
	Subsystem->RegisterRecordForTesting(RecordC);

	// Importer-authored qualified names resolve regardless of prefix family.
	UMaterialInstanceConstant* WorldMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_W_Pkg1__TexA"));
	TestTrue(TEXT("world MI_W_<slug> resolves by name"),
		Subsystem->FindRecordForMaterialForTesting(WorldMI) == &RecordA);
	UMaterialInstanceConstant* ParticleMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_PSAddFB_Pkg2__Shared"));
	TestTrue(TEXT("qualified particle instance resolves across a bare-name collision"),
		Subsystem->FindRecordForMaterialForTesting(ParticleMI) == &RecordC);
	// import_flame_fx.py:265-267 authors neutral-parent MI_MeshFB_<Pkg>__<Tex>
	// identity children for procedural mesh consumers; their name is the only key.
	UMaterialInstanceConstant* MeshFBMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_MeshFB_Pkg1__Shared"));
	TestTrue(TEXT("qualified mesh flipbook instance resolves by identity name"),
		Subsystem->FindRecordForMaterialForTesting(MeshFBMI) == &RecordB);

	// Per-triangle-type material variants (_2S/_M/_T/_Mod — the mesh2glb/
	// bsp2obj type split) carry the SAME fractal identity. The retired
	// sheet-referencer discovery was suffix-blind; identity naming must strip
	// the type suffix or suffixed surfaces lose their binding (Battle2_02's
	// ScottT__S_rainTex01_2S rain went dead in the stripped fleet boot-smoke,
	// 2026-07-19: 8 sims/9 slots -> 3/3).
	// Chained suffixes are real shipped data: Battle2_02's pond portals are
	// ScottT__s_rainwater_T_2S / JHPFireEng__SLWater01_T_2S (translucent AND
	// two-sided) — the strip must peel until it hits the identity.
	for (const TCHAR* Suffixed : {TEXT("MI_W_Pkg1__TexA_2S"),
		TEXT("MI_W_Pkg1__TexA_M"), TEXT("MI_W_Pkg1__TexA_T"),
		TEXT("MI_W_Pkg1__TexA_Mod"), TEXT("MI_W_Pkg1__TexA_T_2S")})
	{
		UMaterialInstanceConstant* SuffixedMI = NewObject<UMaterialInstanceConstant>(
			GetTransientPackage(), Suffixed);
		TestTrue(FString::Printf(TEXT("%s resolves through the type suffix"), Suffixed),
			Subsystem->FindRecordForMaterialForTesting(SuffixedMI) == &RecordA);
	}
	UMaterialInstanceConstant* BareSuffixedMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_TexA_2S"));
	TestTrue(TEXT("bare suffixed instance resolves through the type suffix"),
		Subsystem->FindRecordForMaterialForTesting(BareSuffixedMI) == &RecordA);

	// Bare MI_<Tex> resolves ONLY while unique; collisions are poisoned.
	UMaterialInstanceConstant* BareUniqueMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_TexA"));
	TestTrue(TEXT("unique bare flipbook instance resolves"),
		Subsystem->FindRecordForMaterialForTesting(BareUniqueMI) == &RecordA);
	UMaterialInstanceConstant* BareCollidingMI = NewObject<UMaterialInstanceConstant>(
		GetTransientPackage(), TEXT("MI_Shared"));
	TestNull(TEXT("colliding bare name refuses to guess"),
		Subsystem->FindRecordForMaterialForTesting(BareCollidingMI));

	// A gameplay MID resolves through its PARENT'S importer name (the chain
	// walk) — this is the shape left after the sheet assets are deleted.
	UMaterialInterface* AnyMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshFBAddGlow.M_WoT_MeshFBAddGlow"));
	{
		// The MID on top has an auto-generated name; only its parent's
		// importer-authored name identifies the record.
		UMaterialInstanceConstant* Qualified = NewObject<UMaterialInstanceConstant>(
			GetTransientPackage(), TEXT("MI_MeshT_Pkg2__Shared"));
		UMaterialInstanceDynamic* MID =
			UMaterialInstanceDynamic::Create(Qualified, GetTransientPackage());
		TestTrue(TEXT("a MID resolves via its parent instance's name"),
			Subsystem->FindRecordForMaterialForTesting(MID) == &RecordC);
	}

	// A standalone texture path resolves (the surviving asset family); the
	// transient live texture is the DONE signal and must return null.
	UPackage* TexPackage = CreatePackage(TEXT("/Game/WoT/Textures/Pkg1/TexA"));
	UTexture2D* Standalone = NewObject<UTexture2D>(
		TexPackage, TEXT("TexA"), RF_Public | RF_Standalone);
	if (AnyMaster)
	{
		UMaterialInstanceDynamic* PathMID =
			UMaterialInstanceDynamic::Create(AnyMaster, GetTransientPackage());
		PathMID->SetTextureParameterValue(TEXT("Tex"), Standalone);
		TestTrue(TEXT("a bound standalone texture path resolves"),
			Subsystem->FindRecordForMaterialForTesting(PathMID) == &RecordA);
		UTexture2D* Live = UTexture2D::CreateTransient(2, 2, PF_B8G8R8A8,
			TEXT("WoTLive_Pkg1_TexA"));
		PathMID->SetTextureParameterValue(TEXT("Tex"), Live);
		TestNull(TEXT("a bound live texture is the already-bound signal"),
			Subsystem->FindRecordForMaterialForTesting(PathMID));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFractalGuardedCatalogSpec,
	"WoT.Fractal.GuardedCatalogFromCacheObjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFractalGuardedCatalogSpec::RunTest(const FString&)
{
	// The synthetic procedural-v2 object (Tools/dist/fixtures, byte-identical
	// to the native importer's output) carries its palette bytes and
	// simulation parameters; the guarded subsystem materializes it into a
	// live FireTexture record and rebinds every "WoTProc_" placeholder.
	TArray<uint8> Fixture;
	const FString FixturePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Tools/dist/fixtures/cache-v3-procedural-fire.bin"));
	if (!TestTrue(TEXT("procedural fixture loads"),
		FFileHelper::LoadFileToArray(Fixture, *FixturePath))) return false;
	const FString CacheRoot = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
		TEXT("WoTFractalGuarded_") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
		TEXT("ContentCache"));
	IFileManager::Get().MakeDirectory(*CacheRoot, true);
	TArray<FWoTPayloadActivationTestObject> Objects;
	Objects.Add({TEXT("cache/procedural/remaining__FireA"),
		TEXT("wot-procedural-v3"), Fixture});
	FString Error;
	if (!TestTrue(TEXT("procedural generation publishes"),
		WoTPayloadActivationTest_PublishObjects(CacheRoot, Objects, Error)))
	{
		AddError(Error);
		return false;
	}
	// This spec owns the global publication state: the payload receipt comes
	// from the real cache activation below, the world receipt is published for
	// that generation, and both are reset on every exit path.
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false, false);
		IFileManager::Get().DeleteDirectory(*CacheRoot, false, true);
	};
	AddExpectedError(TEXT("Cannot read payload manifest"),
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
	auto Store = FWoTCacheStore::ForTesting(CacheRoot);
	if (Store.IsError()) return false;
	auto FixtureLease = WoTAcquireCurrentForTesting(Store.GetValue());
	if (FixtureLease.IsError()) return false;
	FWoTBootstrapStatus Activated = Payload
		? WoTPayloadActivationTest_ActivateLeaseForWorldAuthorization(
			Payload, FixtureLease.GetValue())
		: FWoTBootstrapStatus::Error(
			MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation, TEXT("no payload subsystem")));
	if (!TestNotNull(TEXT("payload subsystem fixture"), Payload)
		|| !TestFalse(TEXT("verified generation activates"), Activated.IsError()))
	{
		if (Activated.IsError()) AddError(Activated.GetError().ToString());
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	if (!TestNotNull(TEXT("fractal subsystem"), Subsystem))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// The development catalog (editor builds) may already be registered; the
	// guarded activation adds exactly the authenticated cache objects.
	const int32 RecordsBefore = Subsystem->GetRegisteredRecordCount();
	TestFalse(TEXT("the world receipt publishes for the activated generation"),
		PublishWoTBootstrapWorldReady().IsError());
	// The strict decoder and record builder on the authenticated object.
	FWoTRuntimeProceduralResource Resource;
	const FWoTNeutralResourceId Id{
		TEXT("cache/procedural/remaining__FireA"), EWoTNeutralResourceKind::Procedural};
	FWoTBootstrapStatus Loaded = Resource.LoadFirstUse(Id);
	if (!TestFalse(TEXT("procedural v2 object parses"), Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString());
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTFractalTextureRecord Record;
	TestTrue(TEXT("procedural v2 object builds a runtime record"),
		Resource.BuildRecord(Record));
	TestEqual(TEXT("record class"), Record.FractalClass, EWoTFractalClass::Fire);
	TestEqual(TEXT("record identity"), Record.ObjectPath, FString(TEXT("remaining.FireA")));
	TestEqual(TEXT("record package"), Record.PackageName, FName(TEXT("remaining")));
	TestEqual(TEXT("record texture"), Record.TextureName, FName(TEXT("FireA")));
	TestEqual(TEXT("record size"), Record.Width * Record.Height, 64);
	TestEqual(TEXT("record RenderHeat"), static_cast<int32>(Record.RenderHeat), 0xc0);
	TestEqual(TEXT("record SparksLimit"), Record.SparksLimit, 8);
	TestEqual(TEXT("record sparks"), Record.Sparks.Num(), 2);
	TestEqual(TEXT("record spark heat"), static_cast<int32>(Record.Sparks[0].Heat), 220);
	TestEqual(TEXT("record palette bytes"), Record.PaletteRGBA.Num(), 1024);
	// _synthetic_remaining_source palette: (index, 255-index, index/2, 255).
	TestTrue(TEXT("record palette entry 7"),
		Record.PaletteRGBA[7 * 4] == 7 && Record.PaletteRGBA[7 * 4 + 1] == 248
		&& Record.PaletteRGBA[7 * 4 + 2] == 3 && Record.PaletteRGBA[7 * 4 + 3] == 255);
	TestEqual(TEXT("record initial bitmap"), Record.InitialBitmap.Num(), 64);
	TestEqual(TEXT("record source"), Record.SourceTextureRef, FString());
	TestEqual(TEXT("identity key"),
		FWoTRuntimeProceduralResource::IdentityKeyForSourceExport(Resource.SourceExport()),
		FName(TEXT("remaining__FireA")));

	// A consumer materialized before the subsystem activates: the placeholder
	// still frame (mip 0 through the palette) named for discovery.
	UTexture2D* Placeholder = WoTRuntimeProceduralPlaceholderTexture(Record);
	if (TestNotNull(TEXT("placeholder texture materializes"), Placeholder))
	{
		TestTrue(TEXT("placeholder carries the discovery name"),
			Placeholder->GetFName().GetPlainNameString().StartsWith(TEXT("WoTProc_remaining__FireA")));
		TestFalse(TEXT("placeholder samples raw display bytes"), Placeholder->SRGB);
	}
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_EntryTranslucent.M_WoT_EntryTranslucent"));
	if (TestNotNull(TEXT("entry translucent master"), Master) && Placeholder)
	{
		// Discovery walks GetNumMaterials(), which a mesh-less component
		// reports as zero; the runtime BSP sections carry real meshes.
		UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr,
			TEXT("/Engine/BasicShapes/Plane.Plane"));
		AActor* Actor = World->SpawnActor<AActor>();
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
		Component->SetStaticMesh(Plane);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		TestNotNull(TEXT("consumer mesh"), Plane);
		UMaterialInstanceDynamic* SectionMaterial =
			UMaterialInstanceDynamic::Create(Master, Actor);
		SectionMaterial->SetTextureParameterValue(TEXT("Tex"), Placeholder);
		Component->SetMaterial(0, SectionMaterial);
		TestTrue(TEXT("the Entry masters are identity-UV roots"),
			UWoTFractalSubsystem::UsesIdentityUVRoot(Master->GetMaterial()->GetFName()));
		TestTrue(TEXT("world readiness is published for the guarded activation"),
			IsWoTBootstrapWorldReady());
		Subsystem->ActivateGuardedCatalogForTesting();
		TestEqual(TEXT("one authenticated procedural record is registered"),
			Subsystem->GetRegisteredRecordCount(), RecordsBefore + 1);
		TestEqual(TEXT("the placeholder consumer activated its live simulation"),
			Subsystem->GetActiveTextureCount(), 1);
		UTexture* Bound = nullptr;
		SectionMaterial->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
		UTexture2D* Live = Subsystem->FindLiveTexture(FName(TEXT("remaining.FireA")));
		TestNotNull(TEXT("live texture exists"), Live);
		TestTrue(TEXT("the section material now samples the live texture in place"),
			Bound != nullptr && Bound == Live && Component->GetMaterial(0) == SectionMaterial);
		TestTrue(TEXT("activation primed and stepped the fire"),
			Subsystem->GetStepCount(FName(TEXT("remaining.FireA"))) >= 1);
		// The direct bind API for consumers created after activation.
		UStaticMeshComponent* Late = NewObject<UStaticMeshComponent>(Actor);
		Late->SetStaticMesh(Plane);
		Actor->AddInstanceComponent(Late);
		Late->RegisterComponent();
		Late->SetMaterial(0, UMaterialInstanceDynamic::Create(Master, Actor));
		TestTrue(TEXT("direct runtime bind by identity key"),
			Subsystem->BindRuntimeProceduralSlot(Late, 0, FName(TEXT("remaining__FireA"))));
		Bound = nullptr;
		Cast<UMaterialInstanceDynamic>(Late->GetMaterial(0))->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
		TestTrue(TEXT("directly bound slot samples the live texture"), Bound == Live);
		TestFalse(TEXT("an unknown identity refuses to bind"),
			Subsystem->BindRuntimeProceduralSlot(Late, 0, FName(TEXT("Nope__Missing"))));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
