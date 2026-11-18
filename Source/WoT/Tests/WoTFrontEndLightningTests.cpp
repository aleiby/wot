#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTFrontEndTestData.h"
#include "../FrontEnd/WoTFrontEndLightning.h"
#include "../FrontEnd/WoTFrontEndSceneController.h"
#include "Engine/Texture2D.h"
#include "Components/PointLightComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundWave.h"
#include "Async/Async.h"
#include "UObject/GarbageCollection.h"
#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryStrikeBoundaryTest,
	"WoT.FrontEnd.Lightning.StrikeBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTEntryStrikeBoundaryTest::RunTest(const FString& Parameters)
{
	FWoTEntryStrikeSettings S;
	S.MinInterval = S.MaxInterval = 10.f;
	S.MinThunderDelay = S.MaxThunderDelay = 1.f;
	S.LightningLengths = {.5f};
	FWoTEntryStrikeState State;
	FWoTEntryStrikeFrame Frame;
	FString Error;
	auto Random = [] { return .75f; };
	TestTrue(TEXT("settings accepted"), State.Initialize(S, Error));
	TestTrue(TEXT("initial tick"), State.Advance(0.f, Random, Frame, Error));
	TestEqual(TEXT("no initial flash"), Frame.Samples[0], 0.f);
	TestTrue(TEXT("before strike"), State.Advance(9.99f, Random, Frame, Error));
	TestFalse(TEXT("not early"), Frame.bStrike);
	TestTrue(TEXT("strike tick"), State.Advance(.02f, Random, Frame, Error));
	TestTrue(TEXT("strike fires"), Frame.bStrike);
	TestEqual(TEXT("flicker sample"), Frame.Samples[0], .75f);
	TestTrue(TEXT("active zero delta"), State.Advance(0.f, Random, Frame, Error));
	TestFalse(TEXT("zero delta cannot retrigger"), Frame.bStrike);
	TestEqual(TEXT("active still flickers"), Frame.Samples[0], .75f);
	TestTrue(TEXT("end tick"), State.Advance(.51f, Random, Frame, Error));
	TestEqual(TEXT("flash expired"), Frame.Samples[0], 0.f);
	TestFalse(TEXT("thunder not early"), Frame.bThunder);
	TestTrue(TEXT("thunder tick"), State.Advance(.5f, Random, Frame, Error));
	TestTrue(TEXT("thunder after flash"), Frame.bThunder);
	TestEqual(TEXT("current strike delay"), Frame.ThunderDelay, 1.f);
	State.Advance(0.f, Random, Frame, Error);
	TestFalse(TEXT("thunder once"), Frame.bThunder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryStrikeRegressionTest,
	"WoT.FrontEnd.Lightning.StrikeRegressions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTEntryStrikeRegressionTest::RunTest(const FString& Parameters)
{
	FWoTEntryStrikeSettings S;
	S.MinInterval = S.MaxInterval = 10.f;
	S.MinThunderDelay = S.MaxThunderDelay = 1.f;
	S.LightningLengths = {0.f, .25f, .5f, .75f, 1.f};
	FWoTEntryStrikeState State;
	FWoTEntryStrikeFrame F;
	FString Error;
	int32 Draws = 0;
	auto Random = [&] { ++Draws; return .75f; };
	State.Initialize(S, Error);
	TestEqual(TEXT("initialize consumes no draws"), Draws, 0);
	State.Advance(0, Random, F, Error);
	TestEqual(TEXT("initial scheduling consumes three draws"), Draws, 3);
	State.Advance(30, Random, F, Error);
	TestTrue(TEXT("hitch strikes once"), F.bStrike);
	TestEqual(TEXT("hitch draws one schedule and four active endpoints"), Draws, 10);
	TestTrue(TEXT("five samples in routing order"), F.Samples == TArray<float>({0, .75f, .75f, .75f, .75f}));
	State.Advance(.25f, Random, F, Error);
	TestTrue(TEXT("short endpoint expires exactly"), F.Samples == TArray<float>({0, 0, .75f, .75f, .75f}));
	State.Advance(.25f, Random, F, Error);
	TestEqual(TEXT("half-second endpoint expires exactly"), F.Samples[2], 0.f);
	State.Advance(.5f, Random, F, Error);
	TestTrue(TEXT("all expired"), F.Samples == TArray<float>({0, 0, 0, 0, 0}));
	TestTrue(TEXT("thunder exact deadline"), F.bThunder);
	State.Advance(9.f, Random, F, Error);
	TestTrue(TEXT("interval is strike-to-strike"), F.bStrike);
	State.Advance(30.f, Random, F, Error);
	TestTrue(TEXT("second hitch retriggers once"), F.bStrike);
	TestFalse(TEXT("retrigger replaces older pending thunder"), F.bThunder);
	State.Advance(1.f, Random, F, Error);
	TestTrue(TEXT("replacement thunder fires once"), F.bThunder);
	State.Advance(0, Random, F, Error);
	TestFalse(TEXT("zero delta never repeats thunder"), F.bThunder);

	// Discriminates the original three-draw order and NEW current-strike delay.
	S.MinInterval = 10; S.MaxInterval = 20;
	S.MinThunderDelay = 0; S.MaxThunderDelay = 3;
	S.LightningLengths = {.5f};
	State.Initialize(S, Error);
	const float Stream[] = {0, 1, .2f, 1, 0, .3f, .75f};
	int32 Cursor = 0;
	auto Varying = [&] { return Cursor < UE_ARRAY_COUNT(Stream) ? Stream[Cursor++] : .75f; };
	State.Advance(0, Varying, F, Error);
	State.Advance(10, Varying, F, Error);
	TestEqual(TEXT("three scheduling draws plus flicker"), Cursor, 7);
	TestEqual(TEXT("flicker uses draw seven"), F.Samples[0], .75f);
	TestTrue(TEXT("new zero delay thunder fires immediately"), F.bThunder);
	State.Advance(19, Random, F, Error);
	TestFalse(TEXT("new interval is twenty seconds"), F.bStrike);
	State.Advance(1, Random, F, Error);
	TestTrue(TEXT("twenty second boundary retriggers"), F.bStrike);

	for (float R : {0.f, .499f, .5f, 1.f})
	{
		S.MinInterval = S.MaxInterval = .05f;
		State.Initialize(S, Error);
		auto Boundary = [R] { return R; };
		State.Advance(.05f, Boundary, F, Error);
		TestTrue(TEXT("minimum interval allowed"), F.bStrike);
		TestEqual(TEXT("flicker cutoff"), F.Samples[0], R < .5f ? 0.f : R);
	}
	S.LightningLengths.Reset();
	State.Initialize(S, Error);
	State.Advance(.05f, Random, F, Error);
	TestTrue(TEXT("no endpoint yields empty samples"), F.Samples.IsEmpty());
	TestFalse(TEXT("no endpoint cannot thunder"), F.bThunder);

	// Invalid settings/deltas/randomness must preserve both state and output.
	S.MinInterval = S.MaxInterval = 10;
	S.LightningLengths = {.5f};
	State.Initialize(S, Error);
	State.Advance(9, Random, F, Error);
	for (float Bad : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		F.Samples = {123.f};
		TestFalse(TEXT("bad delta rejected"), State.Advance(Bad, Random, F, Error));
		TestEqual(TEXT("failed advance preserves output"), F.Samples[0], 123.f);
		FWoTEntryStrikeSettings Invalid = S; Invalid.LightningLengths[0] = Bad;
		TestFalse(TEXT("bad length rejected"), State.Initialize(Invalid, Error));
	}
	for (float Bad : {-.001f, 1.001f, std::numeric_limits<float>::quiet_NaN()})
	{
		auto InvalidRandom = [Bad] { return Bad; };
		TestFalse(TEXT("bad scheduling random rejected"), State.Advance(1, InvalidRandom, F, Error));
		TestEqual(TEXT("bad random preserves output"), F.Samples[0], 123.f);
	}
	State.Advance(1, Random, F, Error);
	TestTrue(TEXT("failed advances did not consume elapsed time"), F.bStrike);
	F.Samples = {123.f};
	auto BadFlicker = [] { return std::numeric_limits<float>::quiet_NaN(); };
	TestFalse(TEXT("invalid active flicker rejected"), State.Advance(.25f, BadFlicker, F, Error));
	TestEqual(TEXT("bad active flicker preserves caller output"), F.Samples[0], 123.f);
	State.Advance(.25f, Random, F, Error);
	TestEqual(TEXT("bad flicker did not age the flash"), F.Samples[0], .75f);
	FWoTEntryStrikeSettings Invalid = S;
	Invalid.MinInterval = .049f;
	TestFalse(TEXT("subminimum interval rejected"), State.Initialize(Invalid, Error));
	Invalid = S; Invalid.MaxInterval = 9;
	TestFalse(TEXT("reversed interval rejected"), State.Initialize(Invalid, Error));
	Invalid = S; Invalid.MinThunderDelay = -1;
	TestFalse(TEXT("negative delay rejected"), State.Initialize(Invalid, Error));
	Invalid = S; Invalid.LightningLengths.Init(.5f, 9);
	TestFalse(TEXT("endpoint work cap"), State.Initialize(Invalid, Error));
	State.Initialize(S, Error);
	State.Advance(0, Random, F, Error);
	TestFalse(TEXT("reinitialize starts quiet"), F.bStrike);
	TestEqual(TEXT("reinitialize clears flash"), F.Samples[0], 0.f);
	State.Reset();
	TestFalse(TEXT("reset requires initialize"), State.Advance(0, Random, F, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryStrikeFiniteHitchTest,
	"WoT.FrontEnd.Lightning.FiniteHitchPrecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTEntryStrikeFiniteHitchTest::RunTest(const FString& Parameters)
{
	FWoTEntryStrikeSettings Settings;
	Settings.MinInterval = Settings.MaxInterval = .05f;
	Settings.LightningLengths = {.5f};
	FWoTEntryStrikeState State;
	FWoTEntryStrikeFrame Frame;
	FString Error;
	auto Random = [] { return .75f; };
	State.Initialize(Settings, Error);
	TestTrue(TEXT("finite largest hitch accepted"), State.Advance(std::numeric_limits<float>::max(), Random, Frame, Error));
	TestTrue(TEXT("finite hitch strikes once"), Frame.bStrike);
	TestEqual(TEXT("new flash still active after largest finite hitch"), Frame.Samples[0], .75f);
	State.Advance(0, Random, Frame, Error);
	TestFalse(TEXT("zero delta after largest hitch cannot retrigger"), Frame.bStrike);
	TestEqual(TEXT("flash survives zero delta after largest hitch"), Frame.Samples[0], .75f);
	return true;
}

namespace
{
FWoTBSPPayloadData MakeOverlapPayload()
{
	FWoTBSPPayloadData Payload;
	Payload.FormatVersion = 3;
	Payload.LightmapWidth = 3;
	Payload.LightmapHeight = 3;
	Payload.LightmapBGRA = {
		2, 4, 6, 255, 8, 10, 12, 255, 14, 16, 18, 255,
		20, 22, 24, 255, 60, 40, 20, 255, 26, 28, 30, 255,
		32, 34, 36, 255, 38, 42, 44, 255, 46, 48, 50, 255,
	};

	FWoTEntrySurfaceRecord& Surface = Payload.EntrySurfaces.AddDefaulted_GetRef();
	Surface.SurfaceIndex = 0;
	Surface.X = 1;
	Surface.Y = 1;
	Surface.W = 1;
	Surface.H = 1;

	FWoTEntryLightRecord& White = Payload.EntryLights.AddDefaulted_GetRef();
	White.Identity = TEXT("Fixture.White");
	White.Brightness = 255;
	White.Hue = 0;
	White.Saturation = 255;
	White.LevelBrightness = 1.0f;

	FWoTEntryLightRecord& Red = Payload.EntryLights.AddDefaulted_GetRef();
	Red.Identity = TEXT("Fixture.Red");
	Red.Brightness = 255;
	Red.Hue = 0;
	Red.Saturation = 0;
	Red.LevelBrightness = 1.0f;

	FWoTEntryIlluminationRecord& WhitePair =
		Payload.EntryIllumination.AddDefaulted_GetRef();
	WhitePair.LightIndex = 0;
	WhitePair.SurfaceIndex = 0;
	WhitePair.Illumination = {64};

	FWoTEntryIlluminationRecord& RedPair =
		Payload.EntryIllumination.AddDefaulted_GetRef();
	RedPair.LightIndex = 1;
	RedPair.SurfaceIndex = 0;
	RedPair.Illumination = {32};
	return Payload;
}

FWoTBSPPayloadData MakeDisjointPayload()
{
	FWoTBSPPayloadData Payload;
	Payload.FormatVersion = 3;
	Payload.LightmapWidth = 7;
	Payload.LightmapHeight = 3;
	Payload.LightmapBGRA.SetNumUninitialized(7 * 3 * 4);
	for (int32 Pixel = 0; Pixel < 7 * 3; ++Pixel)
	{
		Payload.LightmapBGRA[Pixel * 4 + 0] = 4;
		Payload.LightmapBGRA[Pixel * 4 + 1] = 6;
		Payload.LightmapBGRA[Pixel * 4 + 2] = 8;
		Payload.LightmapBGRA[Pixel * 4 + 3] = 255;
	}
	for (int32 Y = 0; Y < 3; ++Y)
	{
		const int32 Neutral = (Y * 7 + 3) * 4;
		Payload.LightmapBGRA[Neutral + 0] = 33;
		Payload.LightmapBGRA[Neutral + 1] = 35;
		Payload.LightmapBGRA[Neutral + 2] = 37;
		Payload.LightmapBGRA[Neutral + 3] = 17;
	}

	FWoTEntrySurfaceRecord& Left = Payload.EntrySurfaces.AddDefaulted_GetRef();
	Left.SurfaceIndex = 0;
	Left.X = 1;
	Left.Y = 1;
	Left.W = 1;
	Left.H = 1;
	FWoTEntrySurfaceRecord& Right = Payload.EntrySurfaces.AddDefaulted_GetRef();
	Right.SurfaceIndex = 1;
	Right.X = 5;
	Right.Y = 1;
	Right.W = 1;
	Right.H = 1;

	FWoTEntryLightRecord& Red = Payload.EntryLights.AddDefaulted_GetRef();
	Red.Identity = TEXT("Fixture.Red");
	Red.Brightness = 255;
	Red.Hue = 0;
	Red.Saturation = 0;
	FWoTEntryLightRecord& Blue = Payload.EntryLights.AddDefaulted_GetRef();
	Blue.Identity = TEXT("Fixture.Blue");
	Blue.Brightness = 255;
	Blue.Hue = 170;
	Blue.Saturation = 0;

	FWoTEntryIlluminationRecord& RedPair =
		Payload.EntryIllumination.AddDefaulted_GetRef();
	RedPair.LightIndex = 0;
	RedPair.SurfaceIndex = 0;
	RedPair.Illumination = {64};
	FWoTEntryIlluminationRecord& BluePair =
		Payload.EntryIllumination.AddDefaulted_GetRef();
	BluePair.LightIndex = 1;
	BluePair.SurfaceIndex = 1;
	BluePair.Illumination = {32};
	return Payload;
}

bool PixelEquals(const TArray<uint8>& BGRA, uint32 Width, uint32 X, uint32 Y,
	uint8 B, uint8 G, uint8 R, uint8 A = 255)
{
	const int32 Offset = static_cast<int32>((uint64(Y) * Width + X) * 4);
	return Offset >= 0 && Offset + 3 < BGRA.Num()
		&& BGRA[Offset + 0] == B && BGRA[Offset + 1] == G
		&& BGRA[Offset + 2] == R && BGRA[Offset + 3] == A;
}

bool LoadLightningAuthenticatedBSP(const FString& LogicalId,
	TSharedPtr<const FWoTBSPPayloadData>& BSP, FString& Error)
{
	auto Object = FWoTRuntimeRegistry::Get().ReadObject(
		{LogicalId, EWoTNeutralResourceKind::BSP},
		1024ull * 1024ull * 1024ull,
		TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
	if (Object.IsError())
	{
		Error = MoveTemp(Object).GetError().ToString();
		return false;
	}
	TSharedRef<FWoTBSPPayloadData> Decoded = MakeShared<FWoTBSPPayloadData>();
	if (!UWoTBSPPayloadLibrary::DecodePayload(
		Object.GetValue().Payload(), *Decoded, Error))
	{
		return false;
	}
	BSP = Decoded;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndLightningLiteralTest,
	"WoT.FrontEnd.Lightning.LiteralPaletteAndOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndLightningLiteralTest::RunTest(const FString&)
{
	TestEqual(TEXT("half fixed palette"),
		WoTEntryMergeIllumination(10, 3, 32768), uint8(11));
	TestEqual(TEXT("saturating addition"),
		WoTEntryMergeIllumination(126, 255, 65536), uint8(127));
	TestEqual(TEXT("zero illumination"),
		WoTEntryMergeIllumination(10, 0, 65536), uint8(10));

	const FWoTBSPPayloadData Payload = MakeOverlapPayload();
	TArray<uint8> Actual;
	FString Error;
	const TArray<float> Zero = {0.0f, 0.0f};
	if (TestTrue(TEXT("zero samples compose"),
		WoTComposeEntryLightning(Payload, Zero, Actual, Error)))
	{
		TestTrue(TEXT("zero samples copy the base exactly"),
			Actual == Payload.LightmapBGRA);
	}

	const TArray<float> Intermediate = {0.25f, 0.75f};
	const TArray<uint8> IntermediateExpected = {
		86, 66, 84, 255, 86, 66, 84, 255, 86, 66, 84, 255,
		86, 66, 84, 255, 86, 66, 84, 255, 86, 66, 84, 255,
		86, 66, 84, 255, 86, 66, 84, 255, 86, 66, 84, 255,
	};
	if (TestTrue(TEXT("intermediate samples compose"),
		WoTComposeEntryLightning(Payload, Intermediate, Actual, Error)))
	{
		TestTrue(TEXT("intermediate overlap matches the literal 3x3 oracle"),
			Actual == IntermediateExpected);
	}
	if (TestTrue(TEXT("zero reset after intermediate composes"),
		WoTComposeEntryLightning(Payload, Zero, Actual, Error)))
	{
		TestTrue(TEXT("intermediate reset restores the exact base"),
			Actual == Payload.LightmapBGRA);
	}

	const TArray<float> Full = {1.0f, 1.0f};
	const TArray<uint8> FullExpected = {
		164, 144, 176, 255, 164, 144, 176, 255, 164, 144, 176, 255,
		164, 144, 176, 255, 164, 144, 176, 255, 164, 144, 176, 255,
		164, 144, 176, 255, 164, 144, 176, 255, 164, 144, 176, 255,
	};
	if (TestTrue(TEXT("full samples compose"),
		WoTComposeEntryLightning(Payload, Full, Actual, Error)))
	{
		TestTrue(TEXT("full overlap matches the literal 3x3 oracle"),
			Actual == FullExpected);
	}
	if (TestTrue(TEXT("zero reset after full composes"),
		WoTComposeEntryLightning(Payload, Zero, Actual, Error)))
	{
		TestTrue(TEXT("full reset restores the exact base"),
			Actual == Payload.LightmapBGRA);
	}

	FWoTBSPPayloadData Grouping = Payload;
	Grouping.EntryLights.SetNum(1);
	Grouping.EntryLights[0].Brightness = 240;
	Grouping.EntryLights[0].Hue = 0;
	Grouping.EntryLights[0].Saturation = 185;
	Grouping.EntryLights[0].LevelBrightness = 1.1f;
	Grouping.EntryIllumination.SetNum(1);
	Grouping.EntryIllumination[0].Illumination = {214};
	Grouping.LightmapBGRA[16] = 0;
	Grouping.LightmapBGRA[17] = 0;
	Grouping.LightmapBGRA[18] = 0;
	const TArray<float> Quarter = {0.25f};
	if (TestTrue(TEXT("nonwhite nonunit grouping discriminator composes"),
		WoTComposeEntryLightning(Grouping, Quarter, Actual, Error)))
	{
		for (uint32 Y = 0; Y < 3; ++Y)
		{
			for (uint32 X = 0; X < 3; ++X)
			{
				TestTrue(TEXT("grouping discriminator matches original arithmetic"),
					PixelEquals(Actual, 3, X, Y, 66, 66, 90));
			}
		}
	}
	const TArray<float> GroupingZero = {0.0f};
	if (TestTrue(TEXT("zero reset after grouping discriminator composes"),
		WoTComposeEntryLightning(Grouping, GroupingZero, Actual, Error)))
	{
		TestTrue(TEXT("grouping reset restores the exact base"),
			Actual == Grouping.LightmapBGRA);
	}

	const FWoTBSPPayloadData Disjoint = MakeDisjointPayload();
	const TArray<float> DisjointFull = {1.0f, 1.0f};
	TArray<uint8> DisjointExpected;
	if (TestTrue(TEXT("disjoint rectangles compose"),
		WoTComposeEntryLightning(Disjoint, DisjointFull, Actual, Error)))
	{
		for (uint32 Y = 0; Y < 3; ++Y)
		{
			for (uint32 X = 0; X < 3; ++X)
			{
				TestTrue(TEXT("left rectangle and gutter use the red result"),
					PixelEquals(Actual, 7, X, Y, 4, 6, 112));
			}
			TestTrue(TEXT("untouched neutral column remains byte exact"),
				PixelEquals(Actual, 7, 3, Y, 33, 35, 37, 17));
			for (uint32 X = 4; X < 7; ++X)
			{
				TestTrue(TEXT("right rectangle and gutter use the blue result"),
					PixelEquals(Actual, 7, X, Y, 56, 6, 8));
			}
		}
		DisjointExpected = Actual;
	}
	const TArray<float> DisjointZero = {0.0f, 0.0f};
	if (TestTrue(TEXT("zero reset after disjoint composition"),
		WoTComposeEntryLightning(Disjoint, DisjointZero, Actual, Error)))
	{
		TestTrue(TEXT("disjoint reset restores the exact base"),
			Actual == Disjoint.LightmapBGRA);
	}

	FWoTBSPPayloadData ReorderedPairs = Disjoint;
	Swap(ReorderedPairs.EntryIllumination[0], ReorderedPairs.EntryIllumination[1]);
	TArray<uint8> ReorderedActual;
	if (TestTrue(TEXT("reordered pair rows compose"),
		WoTComposeEntryLightning(
			ReorderedPairs, DisjointFull, ReorderedActual, Error)))
	{
		TestTrue(TEXT("pair storage order does not change source-light results"),
			ReorderedActual == DisjointExpected);
	}
	if (TestTrue(TEXT("zero reset after reordered composition"),
		WoTComposeEntryLightning(
			ReorderedPairs, DisjointZero, ReorderedActual, Error)))
	{
		TestTrue(TEXT("reordered reset restores the exact base"),
			ReorderedActual == ReorderedPairs.LightmapBGRA);
	}

	FWoTBSPPayloadData Saturating = Payload;
	Saturating.LightmapBGRA[18] = 252;
	const TArray<float> RedOnly = {0.0f, 1.0f};
	if (TestTrue(TEXT("overlap saturation fixture composes"),
		WoTComposeEntryLightning(Saturating, RedOnly, Actual, Error)))
	{
		for (uint32 Y = 0; Y < 3; ++Y)
		{
			for (uint32 X = 0; X < 3; ++X)
			{
				TestTrue(TEXT("saturated interior is duplicated through gutter corners"),
					PixelEquals(Actual, 3, X, Y, 60, 40, 254));
			}
		}
	}
	if (TestTrue(TEXT("zero reset after saturated composition"),
		WoTComposeEntryLightning(Saturating, Zero, Actual, Error)))
	{
		TestTrue(TEXT("saturated reset restores the exact base"),
			Actual == Saturating.LightmapBGRA);
	}

	TArray<uint8> Sentinel = {91, 92, 93};
	const TArray<float> ShortSamples = {1.0f};
	TestFalse(TEXT("incorrect sample count is rejected"),
		WoTComposeEntryLightning(Payload, ShortSamples, Sentinel, Error));
	TestTrue(TEXT("sample-count failure publishes no partial output"),
		Sentinel == TArray<uint8>({91, 92, 93}));
	const TArray<float> NaNSamples = {
		0.0f, std::numeric_limits<float>::quiet_NaN()};
	TestFalse(TEXT("NaN sample is rejected"),
		WoTComposeEntryLightning(Payload, NaNSamples, Sentinel, Error));
	TestTrue(TEXT("NaN failure publishes no partial output"),
		Sentinel == TArray<uint8>({91, 92, 93}));

	FWoTBSPPayloadData OddAffected = Payload;
	OddAffected.LightmapBGRA[16] = 61;
	Sentinel = {91, 92, 93};
	TestFalse(TEXT("odd affected base channel is rejected"),
		WoTComposeEntryLightning(OddAffected, Full, Sentinel, Error));
	TestTrue(TEXT("odd-channel failure publishes no partial output"),
		Sentinel == TArray<uint8>({91, 92, 93}));
	Sentinel = {91, 92, 93};
	TestFalse(TEXT("odd paired v3 channel is rejected at zero samples"),
		WoTComposeEntryLightning(OddAffected, Zero, Sentinel, Error));
	TestTrue(TEXT("odd paired zero failure publishes no partial output"),
		Sentinel == TArray<uint8>({91, 92, 93}));

	FWoTBSPPayloadData OddInactive = Disjoint;
	OddInactive.LightmapBGRA[(1 * 7 + 5) * 4] = 5;
	const TArray<float> LeftOnly = {1.0f, 0.0f};
	Sentinel = {91, 92, 93};
	TestFalse(TEXT("odd paired inactive surface is rejected beside an active surface"),
		WoTComposeEntryLightning(OddInactive, LeftOnly, Sentinel, Error));
	TestTrue(TEXT("odd inactive-surface failure publishes no partial output"),
		Sentinel == TArray<uint8>({91, 92, 93}));

	FWoTBSPPayloadData LegacyOdd = OddAffected;
	LegacyOdd.FormatVersion = 2;
	LegacyOdd.EntrySurfaces.Reset();
	LegacyOdd.EntryLights.Reset();
	LegacyOdd.EntryIllumination.Reset();
	const TArray<float> LegacySamples;
	TestTrue(TEXT("provenance-free v2 odd fixture still copies exactly"),
		WoTComposeEntryLightning(LegacyOdd, LegacySamples, Actual, Error));
	TestTrue(TEXT("provenance-free v2 odd bytes remain exact"),
		Actual == LegacyOdd.LightmapBGRA);

	FWoTBSPPayloadData NoLights = Payload;
	NoLights.EntryLights.Reset();
	NoLights.EntryIllumination.Reset();
	const TArray<float> EmptySamples;
	TestTrue(TEXT("zero-light payload accepts empty samples"),
		WoTComposeEntryLightning(NoLights, EmptySamples, Actual, Error));
	TestTrue(TEXT("zero-light payload copies the base exactly"),
		Actual == NoLights.LightmapBGRA);
	TestFalse(TEXT("zero-light payload rejects nonempty samples"),
		WoTComposeEntryLightning(NoLights, Quarter, Sentinel, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndLightningRealEntryTest,
	"WoT.FrontEnd.Lightning.RealEntryOwnedAtlases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndLightningRealEntryTest::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString EntryPath = WoTReferencePath(TEXT("original/Maps/WheelOfTime.wot"));
	if (!FPaths::FileExists(EntryPath))
	{
		AddError(TEXT("required original/Maps/WheelOfTime.wot is missing"));
		return false;
	}
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("genuine entry imports into the authenticated weather fixture"),
		WoTFrontEndTest_BuildRealWeatherStageFixture(Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTRuntimeWorldResource World;
	FWoTBootstrapStatus Loaded = World.LoadFirstUse({
		Fixture.Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World});
	if (!TestFalse(TEXT("genuine world authenticates through the runtime registry"),
		Loaded.IsError()))
	{
		if (Loaded.IsError()) AddError(MoveTemp(Loaded).GetError().ToString());
		return false;
	}
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	if (!TestTrue(TEXT("genuine v3 BSP authenticates and decodes"),
		LoadLightningAuthenticatedBSP(Fixture.Descriptor.BspLogicalId, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("genuine entry has five lightning lights"),
		BSP->EntryLights.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("genuine entry retains all 175 shadow-aware light pairs"),
		BSP->EntryIllumination.Num(), 175);

	struct FSampleCase
	{
		const TCHAR* Label;
		TArray<float> Samples;
	};
	const TArray<FSampleCase> Cases = {
		{TEXT("zero"), {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
		{TEXT("intermediate"), {0.25f, 0.5f, 0.75f, 0.5f, 0.25f}},
		{TEXT("full"), {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}},
		{TEXT("overlap"), {0.0f, 1.0f, 0.0f, 1.0f, 0.0f}},
	};
	const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("ReleaseEvidence/entry-sky-weather/t3-runtime-samples"));
	if (!TestTrue(TEXT("real lightning evidence directory is available"),
		IFileManager::Get().MakeDirectory(*OutputDirectory, true)))
	{
		return false;
	}

	const TArray<float> ZeroSamples = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	for (const FSampleCase& Case : Cases)
	{
		TArray<uint8> Composed;
		if (!TestTrue(*FString::Printf(TEXT("%s real atlas composes"), Case.Label),
			WoTComposeEntryLightning(*BSP, Case.Samples, Composed, Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(*FString::Printf(TEXT("%s owns a complete atlas"), Case.Label),
			Composed.Num(), BSP->LightmapBGRA.Num());
		if (FCString::Strcmp(Case.Label, TEXT("zero")) == 0)
		{
			TestTrue(TEXT("real zero sample atlas is byte-identical to the base"),
				Composed == BSP->LightmapBGRA);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s changes the real atlas"), Case.Label),
				Composed != BSP->LightmapBGRA);
			TArray<uint8> Reset;
			if (TestTrue(*FString::Printf(TEXT("%s zero reset composes"), Case.Label),
				WoTComposeEntryLightning(*BSP, ZeroSamples, Reset, Error)))
			{
				TestTrue(*FString::Printf(
					TEXT("%s zero reset restores the exact base"), Case.Label),
					Reset == BSP->LightmapBGRA);
			}
		}
		const FString OutputPath = FPaths::Combine(OutputDirectory,
			FString::Printf(TEXT("%s.bgra"), Case.Label));
		TestTrue(*FString::Printf(TEXT("%s owned atlas writes for oracle comparison"),
			Case.Label), FFileHelper::SaveArrayToFile(Composed, *OutputPath));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryControllerTest,
	"WoT.FrontEnd.Scene.EntryLightningController",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTEntryControllerTest::RunTest(const FString& Parameters)
{
	const auto Base = MakeShared<const FWoTBSPPayloadData>(MakeOverlapPayload());
	FWoTEntryStrikeSettings Settings;
	Settings.MinInterval = Settings.MaxInterval = 10;
	Settings.LightningLengths = {100, 100};
	FString Error;
	TArray<TFunction<void()>> Completions;
	TArray<TWeakPtr<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>> Jobs;
	TArray<uint8> LatestBytes;
	uint64 LatestGeneration = 0;
	auto Upload = [&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> Job,
		TFunction<void()> Done)
	{
		Jobs.Add(Job);
		LatestBytes = Job->Bytes;
		LatestGeneration = Job->Generation;
		TestEqual(TEXT("whole atlas region width"), Job->Region.Width, 3u);
		TestEqual(TEXT("whole atlas region height"), Job->Region.Height, 3u);
		Completions.Add(MoveTemp(Done));
		return true;
	};
	UTexture2D* Atlas = UTexture2D::CreateTransient(3, 3, PF_B8G8R8A8);
	Atlas->NeverStream = true;
	TStrongObjectPtr<UTexture2D> FixtureAtlasOwner(Atlas);
	TSharedPtr<FWoTFrontEndSceneController> Controller = MakeShared<FWoTFrontEndSceneController>(false, 0.f, Upload);
	TestTrue(TEXT("entry configuration accepted"), Controller->ConfigureEntryLightning(Base, Atlas, Settings, Error));
	UPointLightComponent* Light = NewObject<UPointLightComponent>();
	Controller->AddLightningLight(Light);
	Controller->ApplyInitialLightningGate();
	UMaterialInstanceDynamic* Sky = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	Controller->AddSkyMaterial(Sky);
	Controller->TickForTesting(100);
	TestEqual(TEXT("unprepared time inert"), Controller->SceneTimeForTesting(), 0.f);
	TestTrue(TEXT("prepare"), Controller->Prepare());
	TestTrue(TEXT("prepared ticker survives inert frame"), Controller->TickForTesting(100));
	TestEqual(TEXT("no prepublish scheduling or uploads"), Completions.Num(), 0);
	TestEqual(TEXT("entry has no prepublish strike deadline"), Controller->NextStrikeSeconds(), 0.f);
	Controller->Publish();
	TestEqual(TEXT("publication resets time"), Controller->SceneTimeForTesting(), 0.f);
	TestEqual(TEXT("publication schedules entry interval"), Controller->NextStrikeSeconds(), 10.f);
	Controller->TickForTesting(9);
	TestEqual(TEXT("entry observation tracks actual remaining interval"), Controller->NextStrikeSeconds(), 1.f);
	TestEqual(TEXT("idle base needs no upload"), Completions.Num(), 0);
	Controller->TickForTesting(1);
	for (int32 I = 0; I < 20; ++I) Controller->TickForTesting(.01f);
	TestEqual(TEXT("muted controller still uploads visuals; cap two"), Completions.Num(), 2);
	TestEqual(TEXT("two accepted jobs in flight"), Controller->UploadsInFlightForTesting(), 2);
	TestTrue(TEXT("only newest pending state retained"), Controller->HasPendingUploadForTesting());
	TestFalse(TEXT("entry point light never also brightens"), Light->IsVisible());
	float SkyTime = -1;
	TestTrue(TEXT("sky has controlled time"), Sky->GetScalarParameterValue(FMaterialParameterInfo(TEXT("WoTSceneTime")), SkyTime));
	TestEqual(TEXT("sky clock follows scene clock"), SkyTime, Controller->SceneTimeForTesting());
	if (Completions.Num() >= 2)
	{
		TFunction<void()> Done = MoveTemp(Completions[0]);
		Async(EAsyncExecution::Thread, [Done = MoveTemp(Done)]() mutable { Done(); Done = {}; }).Get();
		TestFalse(TEXT("completion releases first immutable owner"), Jobs[0].IsValid());
		TestEqual(TEXT("cross-thread retirement counted"), Controller->UploadsInFlightForTesting(), 1);
		Controller->TickForTesting(0);
		TestEqual(TEXT("next game tick sends newest pending"), Completions.Num(), 3);
		TestTrue(TEXT("coalesced generation skips superseded work"), LatestGeneration > 2);
		TArray<uint8> Expected;
		TestTrue(TEXT("observed latest samples compose"), WoTComposeEntryLightning(*Base,
			Controller->LightningSamplesForTesting(), Expected, Error));
		TestTrue(TEXT("accepted job is current complete atlas"), LatestBytes == Expected);
	}
	Controller->Stop();
	TestFalse(TEXT("stop drops pending"), Controller->HasPendingUploadForTesting());
	TestFalse(TEXT("stopped generation cannot prepare again"), Controller->Prepare());
	const int32 BeforeLate = Completions.Num();
	Controller.Reset();
	for (TFunction<void()>& Done : Completions) if (Done) { Done(); Done = {}; }
	for (const auto& Job : Jobs) TestFalse(TEXT("late callbacks release detached job owners"), Job.IsValid());
	TestEqual(TEXT("late completion cannot resubmit"), Completions.Num(), BeforeLate);

	// Reduced motion freezes clock and keeps the steady atlas even after a hitch.
	int32 ReducedUploads = 0;
	auto Reduced = MakeShared<FWoTFrontEndSceneController>(true, 1.f,
		[&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>, TFunction<void()>) { ++ReducedUploads; return false; });
	Reduced->ConfigureEntryLightning(Base, Atlas, Settings, Error);
	Reduced->Prepare(); Reduced->Publish(); Reduced->TickForTesting(30);
	TestEqual(TEXT("reduced motion clock frozen"), Reduced->SceneTimeForTesting(), 0.f);
	TestEqual(TEXT("reduced motion base never uploaded"), ReducedUploads, 0);
	TestEqual(TEXT("reduced motion weather frozen"), Reduced->AnimatedWeatherCount(5), 0u);
	Reduced->Stop();

	// A rejected uploader cannot leak the two-slot allowance; it retries latest.
	int32 Rejections = 0;
	auto Rejected = MakeShared<FWoTFrontEndSceneController>(false, 0.f,
		[&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>, TFunction<void()>) { ++Rejections; return false; });
	Rejected->ConfigureEntryLightning(Base, Atlas, Settings, Error);
	Rejected->Prepare(); Rejected->Publish(); Rejected->TickForTesting(10);
	for (int32 I = 0; I < 8; ++I) Rejected->TickForTesting(.01f);
	TestTrue(TEXT("rejection retries after more than two attempts"), Rejections > 2);
	TestEqual(TEXT("rejections retire synchronously"), Rejected->UploadsInFlightForTesting(), 0);
	Rejected->Stop();

	auto Invalid = MakeShared<FWoTFrontEndSceneController>(false, 0.f);
	TestFalse(TEXT("null atlas rejected"), Invalid->ConfigureEntryLightning(Base, nullptr, Settings, Error));
	FWoTEntryStrikeSettings WrongRoutes = Settings; WrongRoutes.LightningLengths = {.5f};
	TestFalse(TEXT("sample routing mismatch rejected"), Invalid->ConfigureEntryLightning(Base, Atlas, WrongRoutes, Error));
	// CreateTransient with no data does not create a render resource. Exercise
	// the real wrapper's early-return handling; an injected uploader cannot prove it.
	TestTrue(TEXT("resource-not-ready atlas configures"), Invalid->ConfigureEntryLightning(Base, Atlas, Settings, Error));
	TestNull(TEXT("fixture has no texture resource"), Atlas->GetResource());
	Invalid->Prepare(); Invalid->Publish(); Invalid->TickForTesting(10);
	for (int32 I = 0; I < 8; ++I) Invalid->TickForTesting(.01f);
	TestEqual(TEXT("missing resource never counts accepted upload"), Invalid->AcceptedUploadsForTesting(), uint64(0));
	TestEqual(TEXT("missing resource never leaks inflight allowance"), Invalid->UploadsInFlightForTesting(), 0);
	TestTrue(TEXT("not-ready resource retains newest pending"), Invalid->HasPendingUploadForTesting());
	Invalid->Stop();

	// End-of-flash is a byte-exact base reset; later idle ticks upload nothing.
	TArray<TArray<uint8>> ImmediateBytes;
	auto Immediate = MakeShared<FWoTFrontEndSceneController>(false, 0.f,
		[&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> Job, TFunction<void()> Done)
		{
			ImmediateBytes.Add(Job->Bytes);
			Done();
			return true;
		});
	Settings.LightningLengths = {.5f, .5f};
	Immediate->ConfigureEntryLightning(Base, Atlas, Settings, Error);
	Immediate->Prepare(); Immediate->Publish(); Immediate->TickForTesting(10);
	for (int32 I = 0; I < 10; ++I) Immediate->TickForTesting(.01f);
	Immediate->TickForTesting(.5f);
	TestTrue(TEXT("visual uploads occurred before expiry"), ImmediateBytes.Num() > 1);
	if (!ImmediateBytes.IsEmpty()) TestTrue(TEXT("expiry restores exact base bytes"), ImmediateBytes.Last() == Base->LightmapBGRA);
	const int32 ResetCount = ImmediateBytes.Num();
	Immediate->TickForTesting(1);
	TestEqual(TEXT("steady idle emits no upload"), ImmediateBytes.Num(), ResetCount);
	TestEqual(TEXT("synchronous completions retire safely"), Immediate->UploadsInFlightForTesting(), 0);
	Immediate->Stop();

	// Accepted callbacks alone keep the detached texture alive across GC.
	TArray<TFunction<void()>> LifetimeCompletions;
	UTexture2D* DetachedAtlas = UTexture2D::CreateTransient(3, 3, PF_B8G8R8A8);
	DetachedAtlas->NeverStream = true;
	TWeakObjectPtr<UTexture2D> DetachedWeak(DetachedAtlas);
	TSharedPtr<FWoTFrontEndSceneController> Detached = MakeShared<FWoTFrontEndSceneController>(false, 0.f,
		[&](TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>, TFunction<void()> Done)
		{
			LifetimeCompletions.Add(MoveTemp(Done));
			return true;
		});
	Detached->ConfigureEntryLightning(Base, DetachedAtlas, Settings, Error);
	Detached->Prepare(); Detached->Publish(); Detached->TickForTesting(10);
	for (int32 I = 0; I < 10; ++I) Detached->TickForTesting(.01f);
	TestTrue(TEXT("detached lifetime fixture accepted work"), !LifetimeCompletions.IsEmpty());
	Detached->Stop(); Detached.Reset(); DetachedAtlas = nullptr;
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("late callbacks retain texture through GC"), DetachedWeak.IsValid());
	for (TFunction<void()>& Done : LifetimeCompletions)
		Async(EAsyncExecution::Thread, [Done = MoveTemp(Done)]() mutable { Done(); Done = {}; }).Get();
	LifetimeCompletions.Reset();
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("completion thread releases last strong texture owner"), DetachedWeak.IsValid());

	// Source chooses first sound whose delay threshold is >= the strike delay.
	auto Sounds = MakeShared<FWoTFrontEndSceneController>(false, 0.f);
	Sounds->ConfigureLightningTiming(10, 20, 0, 3);
	for (int32 I = 0; I < 10; ++I) Sounds->AddThunderWave(NewObject<USoundWave>());
	TestEqual(TEXT("zero delay first sound"), Sounds->ThunderSoundIndexForDelay(0), 0);
	TestEqual(TEXT("inclusive first bucket boundary"), Sounds->ThunderSoundIndexForDelay(.3f), 0);
	TestEqual(TEXT("after first bucket"), Sounds->ThunderSoundIndexForDelay(.3001f), 1);
	TestEqual(TEXT("last boundary"), Sounds->ThunderSoundIndexForDelay(3), 9);
	const float Thresholds[] = {.3f, .6f, .9f, 1.2f, 1.5f, 1.8f, 2.1f, 2.4f, 2.7f, 3.f};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Thresholds); ++Index)
	{
		TestEqual(TEXT("every sound threshold is inclusive"), Sounds->ThunderSoundIndexForDelay(Thresholds[Index]), Index);
		if (Index < 9) TestEqual(TEXT("sound advances after each threshold"),
			Sounds->ThunderSoundIndexForDelay(Thresholds[Index] + .0001f), Index + 1);
	}
	return true;
}

#endif
