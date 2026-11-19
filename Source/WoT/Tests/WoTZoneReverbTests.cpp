#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

// The zones' room reverb (WoTZoneReverb.h): the reverb itself, the zone data
// a release build reads from the purchaser's maps, the listener's zone
// selection and the voices' sends. The real-map specs materialize each map
// the way a release build does and skip with a warning when the reference
// tree is absent.

#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Settings/WoTGameSettings.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTAmbientSound.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTSound.h"
#include "../WoTSoundChannels.h"
#include "../WoTZoneReverb.h"
#include "../WoTZoneReverbSubsystem.h"
#include "../WoTZoneVolume.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioThread.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundSubmix.h"
#include "UObject/StrongObjectPtr.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace WoTZoneReverbSpec
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr float ReverbRate = 48000.0f;
constexpr float ReverbStep = 1.0f / 30.0f;

FActorSpawnParameters ReverbAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

FWoTZoneReverbRecord ReverbRecord(bool bReverbZone, uint8 MasterGain, int32 CutoffHz,
	TArray<uint8> Delay, TArray<uint8> Gain)
{
	FWoTZoneReverbRecord Out;
	Out.bReverbZone = bReverbZone;
	Out.MasterGain = MasterGain;
	Out.CutoffHz = CutoffHz;
	Out.Delay = MoveTemp(Delay);
	Out.Gain = MoveTemp(Gain);
	return Out;
}

// Arena_01's zone 1: MasterGain and CutoffHz are the class defaults.
FWoTZoneReverbRecord ArenaOneZoneOne()
{
	return ReverbRecord(true, 100, 6000, {19, 48, 94, 124, 151, 207}, {54, 87, 40, 44, 18, 7});
}

// Arena_01's zone 6.
FWoTZoneReverbRecord ArenaOneZoneSix()
{
	return ReverbRecord(true, 100, 6000, {5, 14, 24, 33, 51, 118}, {153, 31, 44, 14, 7, 1});
}

// The reverb effect as the engine makes it for a submix: an instance of the
// preset, initialized at the device rate and enabled.
struct FReverbEffect
{
	TStrongObjectPtr<UWoTZoneReverbPreset> Preset;
	TSharedPtr<FWoTZoneReverb, ESPMode::ThreadSafe> Effect;
	int32 Processed = 0;
	int32 Declined = 0;

	explicit FReverbEffect(const FWoTZoneReverbSettings& Settings, float SampleRate = ReverbRate)
	{
		Preset.Reset(NewObject<UWoTZoneReverbPreset>());
		Preset->SetSettings(Settings);
		FSoundEffectSubmixInitData Init;
		Init.SampleRate = SampleRate;
		Init.ParentPresetUniqueId = Preset->GetUniqueID();
		Effect = USoundEffectPreset::CreateInstance<FSoundEffectSubmixInitData, FWoTZoneReverb>(
			Init, *Preset.Get());
		Effect->SetEnabled(true);
	}

	// Runs stereo Input (interleaved) through the effect in blocks of Block
	// frames, appending the effect's stereo output to Output.
	void Run(TConstArrayView<float> Input, int32 Block, TArray<float>& Output)
	{
		const int32 Frames = Input.Num() / 2;
		for (int32 First = 0; First < Frames; First += Block)
		{
			const int32 Count = FMath::Min(Block, Frames - First);
			Audio::FAlignedFloatBuffer In;
			In.Append(Input.GetData() + First * 2, Count * 2);
			Audio::FAlignedFloatBuffer Out;
			Out.SetNumZeroed(Count * 2);
			FSoundEffectSubmixInputData InData;
			InData.NumFrames = Count;
			InData.NumChannels = 2;
			InData.NumDeviceChannels = 2;
			InData.AudioBuffer = &In;
			FSoundEffectSubmixOutputData OutData;
			OutData.AudioBuffer = &Out;
			OutData.NumChannels = 2;
			if (Effect->ProcessAudio(InData, OutData)) ++Processed;
			else ++Declined;
			Output.Append(Out.GetData(), Out.Num());
		}
	}
};

// An interleaved stereo signal of Frames frames: a unit impulse on one ear.
TArray<float> ReverbImpulse(int32 Frames, int32 Ear, int32 At = 0)
{
	TArray<float> Signal;
	Signal.SetNumZeroed(Frames * 2);
	Signal[At * 2 + Ear] = 1.0f;
	return Signal;
}

// A deterministic noise burst followed by silence.
TArray<float> ReverbBurst(int32 Frames, int32 BurstFrames)
{
	TArray<float> Signal;
	Signal.SetNumZeroed(Frames * 2);
	FRandomStream Random(0x7e7e);
	for (int32 Index = 0; Index < BurstFrames * 2; ++Index)
	{
		Signal[Index] = Random.FRandRange(-0.5f, 0.5f);
	}
	return Signal;
}

// The reverb, written out a second way from its description: every stage
// keeps its whole history instead of a ring, and every parameter is given.
struct FReferenceReverb
{
	int32 Delay[6] = {};
	float Feedback[6] = {};
	float Alpha = 0.0f;
	float Volume = 0.0f;

	void Run(TConstArrayView<float> Input, TArray<float>& Output) const
	{
		const int32 Frames = Input.Num() / 2;
		TArray<float> Written[6];
		for (TArray<float>& History : Written) History.SetNumZeroed(Frames);
		float LowPass[6] = {};
		float Last[2] = {};
		Output.SetNumZeroed(Frames * 2);
		for (int32 N = 0; N < Frames; ++N)
		{
			// Half of each ear's own send, half of the other ear's last output.
			float X[2] = {(Input[N * 2] + Last[1]) * 0.5f, (Input[N * 2 + 1] + Last[0]) * 0.5f};
			for (int32 Stage = 0; Stage < 3; ++Stage)
			{
				for (int32 Ear = 0; Ear < 2; ++Ear)
				{
					const int32 Pair = Stage * 2 + Ear;
					const float G = Feedback[Pair];
					const float Delayed = N >= Delay[Pair] ? Written[Pair][N - Delay[Pair]] : 0.0f;
					LowPass[Pair] = Delayed * Alpha + LowPass[Pair] * (1.0f - Alpha);
					Written[Pair][N] = X[Ear] + LowPass[Pair] * G;
					X[Ear] = X[Ear] * -G + Delayed * static_cast<float>(1.0 - static_cast<double>(G) * G);
				}
			}
			Last[0] = X[0];
			Last[1] = X[1];
			Output[N * 2] = X[0] * Volume;
			Output[N * 2 + 1] = X[1] * Volume;
		}
	}
};

// Arena_01's zone 1 at 48 kHz, from the description: each echo time is
// Delay x 2 ms in whole samples, each feedback 1 - Gain/255, the damping
// coefficient that of a 1000 Hz cutoff, the level 100/255.
FReferenceReverb ArenaOneZoneOneReference()
{
	FReferenceReverb Reference;
	const int32 Delays[6] = {1824, 4608, 9024, 11904, 14496, 16320};
	const uint8 Gains[6] = {54, 87, 40, 44, 18, 7};
	for (int32 Pair = 0; Pair < 6; ++Pair)
	{
		Reference.Delay[Pair] = Delays[Pair];
		const float Gain = static_cast<float>(Gains[Pair] * static_cast<double>(1.0f / 255.0f));
		Reference.Feedback[Pair] = static_cast<float>(1.0 - static_cast<double>(Gain));
	}
	const double C = 1.0 - FMath::Cos(1000.0 * static_cast<double>(6.282f) / 48000.0);
	Reference.Alpha = static_cast<float>(FMath::Sqrt(C * (C + 2.0)) - C);
	Reference.Volume = static_cast<float>(100 * (1.0 / 255.0));
	return Reference;
}

float ReverbMaxDifference(TConstArrayView<float> A, TConstArrayView<float> B, int32& OutAt)
{
	float Worst = A.Num() == B.Num() ? 0.0f : TNumericLimits<float>::Max();
	OutAt = INDEX_NONE;
	for (int32 Index = 0; Index < FMath::Min(A.Num(), B.Num()); ++Index)
	{
		const float Difference = FMath::Abs(A[Index] - B[Index]);
		if (Difference > Worst)
		{
			Worst = Difference;
			OutAt = Index;
		}
	}
	return Worst;
}

bool ReverbAllZero(TConstArrayView<float> Samples, int32 From = 0)
{
	for (int32 Index = From; Index < Samples.Num(); ++Index)
	{
		if (Samples[Index] != 0.0f) return false;
	}
	return true;
}
}

// ---------------------------------------------------------------------------
// The reverb itself
// ---------------------------------------------------------------------------

// A zone's bytes become the reverb's stages: Delay x 2 ms limited to 1..340 ms
// in whole samples, feedback 1 - Gain/255 with Gain limited to 0.001..0.999
// of 255, the cutoff limited to 1000 Hz, the level MasterGain/255; a zone
// that is not a reverb zone, or has no master gain, has no reverb.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbConversionSpec,
	"WoT.Audio.ZoneReverb.Conversion", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbConversionSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	const FWoTZoneReverbSettings Zone = WoTZoneReverb::MakeSettings(ArenaOneZoneOne());
	TestTrue(TEXT("Arena_01 zone 1 is a reverb"), Zone.IsActive());
	TestEqual(TEXT("its level is 100/255"), Zone.Volume, static_cast<float>(100.0 / 255.0), 1e-7f);
	TestEqual(TEXT("its 6000 Hz cutoff is limited to 1000 Hz"), Zone.CutoffHz, 1000);
	const WoTZoneReverb::FStages At48 = WoTZoneReverb::MakeStages(Zone, 48000.0f);
	const int32 Delays48[6] = {1824, 4608, 9024, 11904, 14496, 16320};
	const double Feedback[6] = {0.7882, 0.6588, 0.8431, 0.8275, 0.9294, 0.9725};
	for (int32 Pair = 0; Pair < 6; ++Pair)
	{
		TestEqual(FString::Printf(TEXT("pair %d delay at 48 kHz"), Pair), At48.Delay[Pair], Delays48[Pair]);
		TestEqual(FString::Printf(TEXT("pair %d feedback 1 - Gain/255"), Pair),
			At48.Feedback[Pair], Feedback[Pair], 1e-4);
	}
	TestEqual(TEXT("1000 Hz damping at 48 kHz"), At48.Alpha, 0.12251f, 1e-5f);
	const WoTZoneReverb::FStages At44 = WoTZoneReverb::MakeStages(Zone, 44100.0f);
	const int32 Delays44[6] = {1675, 4233, 8290, 10936, 13318, 14994};
	for (int32 Pair = 0; Pair < 6; ++Pair)
	{
		TestEqual(FString::Printf(TEXT("pair %d delay at 44.1 kHz"), Pair), At44.Delay[Pair], Delays44[Pair]);
	}
	TestEqual(TEXT("1000 Hz damping at 44.1 kHz"), At44.Alpha, 0.13256f, 1e-5f);

	// Edges, one pair at a time.
	const FWoTZoneReverbSettings Edges = WoTZoneReverb::MakeSettings(
		ReverbRecord(true, 255, 8000, {0, 169, 171, 255, 1, 170}, {0, 255, 1, 254, 128, 127}));
	const WoTZoneReverb::FStages E = WoTZoneReverb::MakeStages(Edges, 48000.0f);
	TestEqual(TEXT("Delay 0 is a 1 ms echo"), E.Delay[0], 48);
	TestEqual(TEXT("Delay 169 is 338 ms"), E.Delay[1], 16224);
	TestEqual(TEXT("Delay 171 saturates at 340 ms"), E.Delay[2], 16320);
	TestEqual(TEXT("Delay 255 saturates at 340 ms"), E.Delay[3], 16320);
	TestEqual(TEXT("Delay 1 is 2 ms"), E.Delay[4], 96);
	TestEqual(TEXT("Delay 170 is 340 ms"), E.Delay[5], 16320);
	TestEqual(TEXT("Gain 0 recirculates by 0.999"), E.Feedback[0], 0.999, 1e-6);
	TestEqual(TEXT("Gain 255 recirculates by 0.001"), E.Feedback[1], 0.001, 1e-6);
	TestEqual(TEXT("an 8000 Hz cutoff is limited to 1000 Hz"), Edges.CutoffHz, 1000);
	TestEqual(TEXT("MasterGain 255 is full level"), Edges.Volume, 1.0f, 1e-7f);
	const FWoTZoneReverbSettings Negative = WoTZoneReverb::MakeSettings(
		ReverbRecord(true, 100, -5, {20, 34, 0, 0, 0, 0}, {150, 70, 0, 0, 0, 0}));
	TestEqual(TEXT("a negative cutoff is 0 Hz"), Negative.CutoffHz, 0);
	TestEqual(TEXT("a 0 Hz cutoff passes nothing through the damping"),
		WoTZoneReverb::MakeStages(Negative, 48000.0f).Alpha, 0.0f);
	TestFalse(TEXT("MasterGain 0 is no reverb"), WoTZoneReverb::MakeSettings(
		ReverbRecord(true, 0, 6000, {20, 34, 0, 0, 0, 0}, {150, 70, 0, 0, 0, 0})).IsActive());
	TestFalse(TEXT("a zone that is not a reverb zone has none"), WoTZoneReverb::MakeSettings(
		ReverbRecord(false, 100, 6000, {20, 34, 0, 0, 0, 0}, {150, 70, 0, 0, 0, 0})).IsActive());
	TestFalse(TEXT("a record without six pairs has none"),
		WoTZoneReverb::MakeSettings(FWoTZoneReverbRecord()).IsActive());
	TestTrue(TEXT("values past the limits compare equal to the limits"),
		WoTZoneReverb::MakeSettings(ReverbRecord(true, 100, 6000, {171, 34, 0, 0, 0, 0}, {150, 70, 0, 0, 0, 0}))
		== WoTZoneReverb::MakeSettings(ReverbRecord(true, 100, 8000, {255, 34, 0, 0, 0, 0}, {150, 70, 0, 0, 0, 0})));
	TestEqual(TEXT("a 340 ms line at 48 kHz holds 16384 frames"), WoTZoneReverb::LineFrames(48000.0f), 16384);
	TestEqual(TEXT("and 32768 at 96 kHz"), WoTZoneReverb::LineFrames(96000.0f), 32768);
	TestEqual(TEXT("and 16384 at 44.1 kHz"), WoTZoneReverb::LineFrames(44100.0f), 16384);
	return !HasAnyErrors();
}

// The effect's response to an impulse on one ear equals the reverb written
// out a second way, over two seconds of both ears. Its first sample is the
// stages' undelayed part, -0.5 x level x the three feedbacks; each ear's
// output reaches the other ear one sample later; the first echo arrives on
// the excited ear after the first stage's delay.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbImpulseResponseSpec,
	"WoT.Audio.ZoneReverb.ImpulseResponse", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbImpulseResponseSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	const int32 Frames = 2 * 48000 + 1000;
	const FReferenceReverb Reference = ArenaOneZoneOneReference();
	for (int32 Ear = 0; Ear < 2; ++Ear)
	{
		const TCHAR* Side = Ear == 0 ? TEXT("left") : TEXT("right");
		const TArray<float> Input = ReverbImpulse(Frames, Ear);
		FReverbEffect Effect(WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
		TArray<float> Output;
		Effect.Run(Input, 1024, Output);
		TestEqual(FString::Printf(TEXT("%s: every block processed"), Side), Effect.Declined, 0);
		TArray<float> Expected;
		Reference.Run(Input, Expected);
		int32 At = INDEX_NONE;
		const float Worst = ReverbMaxDifference(Output, Expected, At);
		TestTrue(FString::Printf(TEXT("%s impulse: the response matches the reference (largest difference %g at sample %d)"),
			Side, Worst, At), Worst <= 1e-6f);
		const int32 Other = 1 - Ear;
		const float Volume = Reference.Volume;
		const float* G = Reference.Feedback;
		const float Product = G[Ear] * G[2 + Ear] * G[4 + Ear];
		const float OtherProduct = G[Other] * G[2 + Other] * G[4 + Other];
		const float First = -0.5f * Product;
		TestEqual(FString::Printf(TEXT("%s: the first sample is -0.5 x level x the three feedbacks"), Side),
			Output[Ear], First * Volume, 1e-6f);
		TestEqual(FString::Printf(TEXT("%s: nothing reaches the other ear at once"), Side),
			Output[Other], 0.0f);
		TestEqual(FString::Printf(TEXT("%s: the other ear hears half of it one sample later"), Side),
			Output[2 + Other], 0.5f * First * -OtherProduct * Volume, 1e-6f);
		const int32 Echo = Reference.Delay[Ear];
		// The undelayed part dies away within a few samples; then both ears are
		// silent until the first echo of either.
		bool bQuietBefore = true;
		for (int32 N = 40; N < FMath::Min(Reference.Delay[0], Reference.Delay[1]); ++N)
		{
			bQuietBefore &= FMath::Abs(Output[N * 2]) < 1e-12f && FMath::Abs(Output[N * 2 + 1]) < 1e-12f;
		}
		TestTrue(FString::Printf(TEXT("%s: silence until the first echo"), Side), bQuietBefore);
		const float Arrival = 0.5f * (1.0f - G[Ear] * G[Ear]) * G[2 + Ear] * G[4 + Ear] * Volume;
		TestEqual(FString::Printf(TEXT("%s: the first echo arrives after %d samples"), Side, Echo),
			Output[Echo * 2 + Ear], Arrival, 1e-6f);
		TestEqual(FString::Printf(TEXT("%s: and reaches the other ear one sample later"), Side),
			Output[(Echo + 1) * 2 + Other], 0.5f * (Arrival / Volume) * -OtherProduct * Volume, 1e-6f);
	}
	return !HasAnyErrors();
}

// The cross feed runs sample by sample, and the state carries across audio
// blocks: the same input in blocks of 256, 480 and 1024 frames gives the
// same output to the bit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbBlockIndependenceSpec,
	"WoT.Audio.ZoneReverb.BlockIndependence", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbBlockIndependenceSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	const TArray<float> Input = ReverbBurst(48000, 2000);
	TArray<float> Outputs[3];
	const int32 Blocks[3] = {256, 480, 1024};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FReverbEffect Effect(WoTZoneReverb::MakeSettings(ArenaOneZoneSix()));
		Effect.Run(Input, Blocks[Index], Outputs[Index]);
	}
	TestFalse(TEXT("the burst reverberates"), ReverbAllZero(Outputs[0]));
	TestTrue(TEXT("480-frame blocks match 256-frame blocks to the bit"), Outputs[1] == Outputs[0]);
	TestTrue(TEXT("1024-frame blocks match 256-frame blocks to the bit"), Outputs[2] == Outputs[0]);
	return !HasAnyErrors();
}

// Any change of reverb restarts it from silence: from the change on, the
// output is the new reverb's response to the input after the change alone.
// The same reverb again (the same values after the limits) leaves the tail
// alone; no reverb is silence at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbChangeRestartsSpec,
	"WoT.Audio.ZoneReverb.ChangeRestartsFromSilence", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbChangeRestartsSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	const int32 Block = 480;
	const int32 Before = 30 * Block;   // 300 ms
	const int32 After = 60 * Block;
	const FWoTZoneReverbSettings One = WoTZoneReverb::MakeSettings(ArenaOneZoneOne());
	const FWoTZoneReverbSettings Six = WoTZoneReverb::MakeSettings(ArenaOneZoneSix());
	const TArray<float> Opening = ReverbImpulse(Before, 0);

	// Another reverb, no new input: silence from the change on.
	{
		FReverbEffect Effect(One);
		TArray<float> Output;
		Effect.Run(Opening, Block, Output);
		TestFalse(TEXT("the tail is still sounding at 300 ms"),
			ReverbAllZero(TArrayView<const float>(Output).Right(Block * 2)));
		Effect.Preset->SetSettings(Six);
		TArray<float> Tail;
		Effect.Run(ReverbImpulse(After, 0, After - 1), Block, Tail);
		TestTrue(TEXT("a different reverb cuts the old tail at once"),
			ReverbAllZero(TArrayView<const float>(Tail).Left((After - 1) * 2)));
	}
	// Another reverb with new input: exactly the new reverb's own response.
	{
		const TArray<float> Later = ReverbBurst(After, 1000);
		FReverbEffect Effect(One);
		TArray<float> Output;
		Effect.Run(Opening, Block, Output);
		Effect.Preset->SetSettings(Six);
		TArray<float> Switched;
		Effect.Run(Later, Block, Switched);
		FReverbEffect Fresh(Six);
		TArray<float> Alone;
		Fresh.Run(Later, Block, Alone);
		TestTrue(TEXT("after the change the output is the new reverb's response to the new input alone"),
			Switched == Alone);
	}
	// The same values again: the tail carries on, as if nothing happened.
	{
		const FWoTZoneReverbSettings Same = WoTZoneReverb::MakeSettings(ReverbRecord(true, 100, 8000,
			{19, 48, 94, 124, 151, 255}, {54, 87, 40, 44, 18, 7}));
		TestTrue(TEXT("Delay 207 and 255 and cutoffs 6000 and 8000 are the same reverb"), Same == One);
		FReverbEffect Switched(One);
		FReverbEffect Steady(One);
		TArray<float> A, B;
		Switched.Run(Opening, Block, A);
		Steady.Run(Opening, Block, B);
		const int32 Restarts = Switched.Effect->GetProcessor().GetRestartCount();
		Switched.Preset->SetSettings(Same);
		const TArray<float> Silence = ReverbImpulse(After, 0, After - 1);
		Switched.Run(Silence, Block, A);
		Steady.Run(Silence, Block, B);
		TestTrue(TEXT("the same reverb keeps its tail"), A == B);
		TestEqual(TEXT("and does not restart"), Switched.Effect->GetProcessor().GetRestartCount(), Restarts);
	}
	// No reverb: zeros at once, whatever the input.
	{
		FReverbEffect Effect(One);
		TArray<float> Output;
		Effect.Run(Opening, Block, Output);
		Effect.Preset->SetSettings(FWoTZoneReverbSettings());
		TArray<float> Off;
		Effect.Run(ReverbBurst(After, After), Block, Off);
		TestTrue(TEXT("leaving reverb zones silences the reverb at once"), ReverbAllZero(Off));
	}
	return !HasAnyErrors();
}

// An inactive reverb writes zeros and still processes every block: a submix
// effect that declines a block passes its input (every voice's send) through.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbInactiveWritesZerosSpec,
	"WoT.Audio.ZoneReverb.InactiveWritesZeros", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbInactiveWritesZerosSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	FReverbEffect Effect{FWoTZoneReverbSettings()};
	TArray<float> Output;
	Effect.Run(ReverbBurst(4800, 4800), 480, Output);
	TestEqual(TEXT("every block is processed"), Effect.Processed, 10);
	TestEqual(TEXT("no block passes its input through"), Effect.Declined, 0);
	TestTrue(TEXT("the output is silence"), ReverbAllZero(Output));
	// A wider submix: the reverb plays on the front pair only.
	Audio::FAlignedFloatBuffer In;
	In.SetNumZeroed(64 * 2);
	In[0] = 1.0f;
	Audio::FAlignedFloatBuffer Out;
	Out.Init(7.0f, 64 * 6);
	FReverbEffect Wide(WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
	FSoundEffectSubmixInputData InData;
	InData.NumFrames = 64;
	InData.NumChannels = 2;
	InData.NumDeviceChannels = 6;
	InData.AudioBuffer = &In;
	FSoundEffectSubmixOutputData OutData;
	OutData.AudioBuffer = &Out;
	OutData.NumChannels = 6;
	TestTrue(TEXT("a six-channel submix is processed"), Wide.Effect->ProcessAudio(InData, OutData));
	bool bOnlyFront = true;
	for (int32 Frame = 0; Frame < 64; ++Frame)
	{
		for (int32 Channel = 2; Channel < 6; ++Channel) bOnlyFront &= Out[Frame * 6 + Channel] == 0.0f;
	}
	TestTrue(TEXT("every channel but the front pair is silent"), bOnlyFront);
	TestTrue(TEXT("the front left carries the reverb"), Out[0] != 0.0f);
	TestEqual(TEXT("the effect accepts the device channels without a mixer allocation"),
		static_cast<int32>(Wide.Effect->GetDesiredInputChannelCountOverride()), INDEX_NONE);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// The zone data of the real maps
// ---------------------------------------------------------------------------

namespace WoTZoneReverbSpec
{
class FReverbObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Data, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Data.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("zone reverb retail writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3") || Schema == TEXT("wot-class-default-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Data.GetData(), Data.Num());
		}
		return FWoTBootstrapStatus::Success();
	}
	TMap<FString, TArray<uint8>> Objects;
};

class FReverbProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

// The multiplayer world set and class defaults, imported once from the
// purchaser's retail files. Only the world records are kept: the zone table
// needs no level geometry.
struct FReverbFixture
{
	bool Load(FAutomationTestBase& Test)
	{
		if (bAttempted)
		{
			if (!bLoaded) Test.AddError(Error);
			return bLoaded;
		}
		bAttempted = true;
		const FString RetailRoot = WoTReferencePath(TEXT("original"));
		TWoTBootstrapResult<FWoTRetailContract> Contract = WoTLoadProductionRetailContract();
		if (Contract.IsError())
		{
			Error = Contract.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		FWoTRetailContract WorldContract;
		FWoTRetailContract DefaultsContract;
		TArray<FWoTWorldPackageBytesForTesting> WorldSources;
		TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
		TArray<FString> DefaultLogicalIds;
		for (const FWoTRequiredFact& Fact : Contract.GetValue().Facts)
		{
			const bool bMultiplayerWorld =
				(Fact.Domain == EWoTFactDomain::World || Fact.Domain == EWoTFactDomain::BSP)
				&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
			if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
			if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
			{
				FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
			}
			if (Fact.Domain == EWoTFactDomain::ClassDefault)
			{
				DefaultsContract.Facts.Add(Fact);
				FWoTRemainingSourceBytesForTesting& Source = DefaultSources.AddDefaulted_GetRef();
				Source.SourcePath = Fact.SourcePath;
				if (!FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath)))
				{
					Error = Fact.SourcePath + TEXT(" failed to load");
					Test.AddError(Error);
					return false;
				}
				if (Fact.Outputs.Num() == 1) DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
			}
		}
		FReverbProgress Progress;
		TWoTBootstrapResult<FWoTWorldSetResult> Imported =
			ImportRequiredWorldSetPackageBytesForTesting(WorldSources, WorldContract, Writer,
				Progress);
		if (Imported.IsError())
		{
			Error = Imported.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		Worlds = Imported.GetValue().Worlds;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
			ImportRegistryLocalizationDefaultsSourcesForTesting(DefaultSources,
				DefaultsContract, Writer, Progress);
		if (ImportedDefaults.IsError())
		{
			Error = ImportedDefaults.GetError().ToString();
			Test.AddError(Error);
			return false;
		}
		TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
		for (const FString& LogicalId : DefaultLogicalIds)
		{
			const TArray<uint8>* Object = Writer.Objects.Find(LogicalId);
			if (!Object)
			{
				Error = LogicalId + TEXT(" was not imported");
				Test.AddError(Error);
				return false;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*Object,
					TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
			if (Decoded.IsError())
			{
				Error = Decoded.GetError().ToString();
				Test.AddError(Error);
				return false;
			}
			DefaultObjects.Add(MoveTemp(Decoded).GetValue());
		}
		Defaults = MakeUnique<FWoTRuntimeClassDefaults>();
		if (!Defaults->LoadObjectsForTesting(MoveTemp(DefaultObjects), Error))
		{
			Test.AddError(Error);
			return false;
		}
		bLoaded = true;
		return true;
	}

	const FWoTWorldDescriptor* Find(FStringView Map) const
	{
		return Worlds.FindByPredicate([Map](const FWoTWorldDescriptor& Item)
		{
			return FStringView(Item.MapIdentity).Equals(Map, ESearchCase::CaseSensitive);
		});
	}

	bool bAttempted = false;
	bool bLoaded = false;
	FString Error;
	FReverbObjectWriter Writer;
	TArray<FWoTWorldDescriptor> Worlds;
	TUniquePtr<FWoTRuntimeClassDefaults> Defaults;
};

FReverbFixture& ReverbFixture()
{
	static FReverbFixture Fixture;
	return Fixture;
}

// One map's placed actors projected into a world of their own, as a release
// build projects them when the map loads, then put in play.
struct FReverbProjectedMap
{
	UWorld* World = nullptr;
	FWoTRuntimeWorldResource Resource;
	FWoTWorldProjectionResult Projection;

	~FReverbProjectedMap()
	{
		if (World) WoTTest::DestroySpecWorld(World);
	}

	const UWoTMapLightTable* Table() const { return Projection.MapLightTable.Get(); }

	const FWoTRuntimeWorldActor* FindRecord(FStringView Identity) const
	{
		for (const FWoTRuntimeWorldActor& Actor : Resource.Actors())
		{
			if (FStringView(Actor.Identity).Equals(Identity, ESearchCase::IgnoreCase)) return &Actor;
		}
		return nullptr;
	}

	// Where a placed actor stands, by its record identity ("Map.Name").
	bool Location(FStringView Identity, FVector& Out) const
	{
		const FWoTRuntimeWorldActor* Actor = FindRecord(Identity);
		const FWoTRuntimeWorldProperty* Property =
			Actor ? WoTRuntimeWorldFindProperty(*Actor, TEXT("Location")) : nullptr;
		return Property && WoTRuntimeWorldPropertyVector(*Property, Out);
	}

	int32 ZoneAt(const FVector& Point) const
	{
		int32 Zone = INDEX_NONE;
		int32 Leaf = INDEX_NONE;
		return UWoTMeshLight::ClassifyRegion(Table(), Point, Zone, Leaf) ? Zone : INDEX_NONE;
	}

	// The location of the first placed actor that stands in Zone.
	bool FirstLocationInZone(int32 Zone, FVector& Out) const
	{
		for (const FWoTRuntimeWorldActor& Actor : Resource.Actors())
		{
			const FWoTRuntimeWorldProperty* Property = WoTRuntimeWorldFindProperty(Actor, TEXT("Location"));
			FVector Location;
			if (Property && WoTRuntimeWorldPropertyVector(*Property, Location) && ZoneAt(Location) == Zone)
			{
				Out = Location;
				return true;
			}
		}
		return false;
	}
};

bool ProjectReverbMap(FAutomationTestBase& Test, FStringView Map, FReverbProjectedMap& Out)
{
	if (!WoTReferenceTreeExists())
	{
		Test.AddWarning(TEXT("real retail data absent: the real-map reverb spec is not verified"));
		return false;
	}
	FReverbFixture& Fixture = ReverbFixture();
	if (!Fixture.Load(Test)) return false;
	const FWoTWorldDescriptor* Descriptor = Fixture.Find(Map);
	if (!Test.TestNotNull(FString(Map) + TEXT(" is in the world set"), Descriptor)) return false;
	const TArray<uint8>* WorldBytes = Fixture.Writer.Objects.Find(Descriptor->WorldLogicalId);
	if (!Test.TestNotNull(FString(Map) + TEXT(" world payload"), WorldBytes)) return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(*WorldBytes,
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!Test.TestFalse(FString(Map) + TEXT(" world frame decodes"), Decoded.IsError())
		|| !Test.TestFalse(FString(Map) + TEXT(" world payload decodes"),
			Out.Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue()).IsError()))
	{
		return false;
	}
	const FWoTRuntimeWorldResource& Resource = Out.Resource;
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = Resource.MapIdentity();
	Source.Records = Resource.Actors();
	Source.Points = Resource.Points();
	Source.Nodes = Resource.Nodes();
	Source.Vertices = Resource.Vertices();
	Source.Zones = Resource.Zones();
	Source.ReachSpecs = Resource.ReachSpecs();
	Source.Surfaces = Resource.Surfaces();
	Source.Leaves = Resource.Leaves();
	Source.ModelLights = Resource.ModelLights();
	Source.MoverModels = Resource.MoverModels();
	Source.bHasDedicatedMoverCollision = Resource.FormatVersion() >= 4;
	Source.MoverAnimationGroups = Resource.MoverAnimationGroups();
	Source.MoverAnimationTerms = Resource.MoverAnimationTerms();
	Source.SemanticObjects = Resource.SemanticObjects();
	Source.bBSPRootOutside = Resource.BSPRootOutside();
	WoTTest::FSpecWorldOptions Options;
	Options.bStartPlay = false;
	Out.World = WoTTest::CreateSpecWorld(Options);
	if (!Test.TestNotNull(FString(Map) + TEXT(" spec world"), Out.World)) return false;
	UWorld& World = *Out.World;
	FString Error;
	if (!Test.TestTrue(FString(Map) + TEXT(" actors project"),
		FWoTWorldProjector::ProjectDeferred(World, Source, *Fixture.Defaults, Out.Projection, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	for (AActor* Actor : Out.Projection.DeferredActors)
		if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
	World.UpdateWorldComponents(false, false);
	World.GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
	FURL URL;
	World.SetGameMode(URL);
	World.InitializeActorsForPlay(URL);
	World.BeginPlay();
	return Test.TestNotNull(FString(Map) + TEXT(" zone table"), Out.Table());
}

FString ReverbBytes(TConstArrayView<uint8> Values)
{
	TArray<FString> Parts;
	for (const uint8 Value : Values) Parts.Add(FString::FromInt(Value));
	return FString::Join(Parts, TEXT(","));
}
}

// Arena_01 at runtime: one reverb row per compiled zone; zones 1, 6, 7 and 9
// are reverb zones with exactly their authored bytes, and zone 1's MasterGain
// 100 and CutoffHz 6000, which Arena_01 does not author, come from the retail
// ZoneInfo class defaults. Every other zone is dry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbRuntimeTableArenaOneSpec,
	"WoT.Audio.ZoneReverb.RuntimeTable.Arena_01", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbRuntimeTableArenaOneSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	FReverbProjectedMap Map;
	if (!ProjectReverbMap(*this, TEXT("Arena_01"), Map)) return !HasAnyErrors();
	const UWoTMapLightTable& Table = *Map.Table();
	if (!TestEqual(TEXT("one reverb row per compiled zone"), Table.ZoneReverbs.Num(),
		Map.Resource.Zones().Num()) || !TestEqual(TEXT("Arena_01 has 11 zones"),
		Table.ZoneReverbs.Num(), 11)) return false;
	TArray<int32> ReverbZones;
	for (int32 Zone = 0; Zone < Table.ZoneReverbs.Num(); ++Zone)
	{
		if (Table.ZoneReverbs[Zone].bReverbZone) ReverbZones.Add(Zone);
	}
	TestEqual(TEXT("zones 1, 6, 7 and 9 are the reverb zones"), ReverbZones, TArray<int32>({1, 6, 7, 9}));
	const FWoTZoneReverbRecord& One = Table.ZoneReverbs[1];
	TestEqual(TEXT("zone 1's delays"), ReverbBytes(One.Delay), FString(TEXT("19,48,94,124,151,207")));
	TestEqual(TEXT("zone 1's gains"), ReverbBytes(One.Gain), FString(TEXT("54,87,40,44,18,7")));
	TestEqual(TEXT("zone 1's MasterGain"), static_cast<int32>(One.MasterGain), 100);
	TestEqual(TEXT("zone 1's CutoffHz"), One.CutoffHz, 6000);
	const FWoTRuntimeWorldActor* Actor = Map.FindRecord(Map.Resource.Zones()[1].ActorIdentity);
	if (TestNotNull(TEXT("zone 1's zone actor record"), Actor))
	{
		TestNull(TEXT("which authors no MasterGain"), WoTRuntimeWorldFindProperty(*Actor, TEXT("MasterGain")));
		TestNull(TEXT("and no CutoffHz"), WoTRuntimeWorldFindProperty(*Actor, TEXT("CutoffHz")));
	}
	TestEqual(TEXT("zone 6's delays"), ReverbBytes(Table.ZoneReverbs[6].Delay), FString(TEXT("5,14,24,33,51,118")));
	TestEqual(TEXT("zone 7's gains"), ReverbBytes(Table.ZoneReverbs[7].Gain), FString(TEXT("93,58,26,31,36,6")));
	TestEqual(TEXT("zone 9's delays"), ReverbBytes(Table.ZoneReverbs[9].Delay), FString(TEXT("5,20,43,60,76,122")));
	// Zone 0 follows the LevelInfo, which authors no reverb: every class default.
	const FWoTZoneReverbRecord& Zero = Table.ZoneReverbs[0];
	TestTrue(TEXT("zone 0 carries the retail ZoneInfo class defaults"),
		!Zero.bReverbZone && Zero.MasterGain == 100 && Zero.CutoffHz == 6000
		&& ReverbBytes(Zero.Delay) == TEXT("20,34,0,0,0,0") && ReverbBytes(Zero.Gain) == TEXT("150,70,0,0,0,0"));
	return !HasAnyErrors();
}

// All 19 multiplayer maps at runtime: the number of reverb zones per map;
// Arena_06's only one is zone 0, which follows its LevelInfo; Arena_08's
// water zone authors reverb numbers but is no reverb zone; Battle2_05's zone
// 0 follows its first LevelInfo, not the stray second one that authors a
// reverb; Arena_07 and Arena_08 carry MasterGain 90 and CutoffHz 8000.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbRuntimeTableCensusSpec,
	"WoT.Audio.ZoneReverb.RuntimeTable.Census", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbRuntimeTableCensusSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	struct FExpected
	{
		const TCHAR* Map;
		int32 ReverbZones;
	};
	const FExpected Census[] = {
		{TEXT("Arena_01"), 4}, {TEXT("Arena_02"), 0}, {TEXT("Arena_03"), 2},
		{TEXT("Arena_04"), 0}, {TEXT("Arena_05"), 5}, {TEXT("Arena_06"), 1},
		{TEXT("Arena_07"), 1}, {TEXT("Arena_08"), 1}, {TEXT("Arena_09"), 6},
		{TEXT("Arena_11"), 2}, {TEXT("Arena_12"), 3},
		{TEXT("Battle2_01"), 6}, {TEXT("Battle2_02"), 0}, {TEXT("Battle2_03"), 0},
		{TEXT("Battle2_04"), 0}, {TEXT("Battle2_05"), 17}, {TEXT("Battle3_01"), 7},
		{TEXT("Battle3_02"), 11}, {TEXT("Battle4_01"), 13}};
	for (const FExpected& Expected : Census)
	{
		FReverbProjectedMap Map;
		if (!ProjectReverbMap(*this, Expected.Map, Map)) return !HasAnyErrors();
		const UWoTMapLightTable& Table = *Map.Table();
		const FString Name(Expected.Map);
		int32 Count = 0;
		for (const FWoTZoneReverbRecord& Zone : Table.ZoneReverbs) Count += Zone.bReverbZone ? 1 : 0;
		TestEqual(Name + TEXT(" reverb zones"), Count, Expected.ReverbZones);
		TestEqual(Name + TEXT(" has one row per compiled zone"), Table.ZoneReverbs.Num(),
			FMath::Max(Map.Resource.Zones().Num(), 1));
		if (Name == TEXT("Arena_06"))
		{
			TestTrue(TEXT("Arena_06's reverb zone is zone 0, from its LevelInfo"),
				Table.ZoneReverbs.IsValidIndex(0) && Table.ZoneReverbs[0].bReverbZone);
		}
		if (Name == TEXT("Arena_08"))
		{
			int32 WaterZone = INDEX_NONE;
			const TConstArrayView<FWoTRuntimeWorldZone> Zones = Map.Resource.Zones();
			for (int32 Zone = 0; Zone < Zones.Num(); ++Zone)
			{
				if (Zones[Zone].ActorIdentity.EndsWith(TEXT(".WaterZone0"))) WaterZone = Zone;
			}
			if (TestTrue(TEXT("Arena_08's WaterZone0 controls a zone"), Table.ZoneReverbs.IsValidIndex(WaterZone)))
			{
				const FWoTZoneReverbRecord& Water = Table.ZoneReverbs[WaterZone];
				TestTrue(TEXT("its authored numbers without the flag are no reverb zone"),
					!Water.bReverbZone && Water.MasterGain == 90 && ReverbBytes(Water.Delay) == TEXT("30,50,0,0,0,0"));
			}
		}
		if (Name == TEXT("Battle2_05"))
		{
			TestFalse(TEXT("Battle2_05's zone 0 follows LevelInfo0, which is dry"),
				Table.ZoneReverbs.IsValidIndex(0) && Table.ZoneReverbs[0].bReverbZone);
		}
		if (Name == TEXT("Arena_07") || Name == TEXT("Arena_08"))
		{
			TestTrue(Name + TEXT(" zone 2 has MasterGain 90 and CutoffHz 8000"),
				Table.ZoneReverbs.IsValidIndex(2) && Table.ZoneReverbs[2].bReverbZone
				&& Table.ZoneReverbs[2].MasterGain == 90 && Table.ZoneReverbs[2].CutoffHz == 8000);
		}
	}
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Choosing the zone
// ---------------------------------------------------------------------------

namespace WoTZoneReverbSpec
{
// A local player whose body stays where it is put (a pawn without gravity).
struct FReverbListener
{
	APlayerController* Controller = nullptr;
	APawn* Body = nullptr;

	bool Spawn(FAutomationTestBase& Test, UWorld& World, const FVector& Location)
	{
		Controller = World.SpawnActor<APlayerController>(ReverbAlwaysSpawn());
		Body = World.SpawnActor<ADefaultPawn>(Location, FRotator::ZeroRotator, ReverbAlwaysSpawn());
		if (!Test.TestNotNull(TEXT("listener controller"), Controller)
			|| !Test.TestNotNull(TEXT("listener body"), Body)) return false;
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Body);
		Controller->SetViewTarget(Body);
		return true;
	}

	void MoveTo(const FVector& Location)
	{
		Body->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
};

UWoTZoneReverbSubsystem* ReverbOf(FAutomationTestBase& Test, UWorld& World)
{
	UWoTZoneReverbSubsystem* Subsystem = UWoTZoneReverbSubsystem::Get(&World);
	Test.TestNotNull(TEXT("the world has the zone reverb"), Subsystem);
	return Subsystem;
}
}

// Arena_01: the reverb follows the zone the local player stands in, changing
// once per zone change; the water zone (zone 5) has none; a view target
// standing in another zone selects that zone, wherever the player is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbListenerArenaOneSpec,
	"WoT.Audio.ZoneReverb.Listener.Arena_01", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbListenerArenaOneSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	FReverbProjectedMap Map;
	if (!ProjectReverbMap(*this, TEXT("Arena_01"), Map)) return !HasAnyErrors();
	UWorld& World = *Map.World;
	const UWoTMapLightTable& Table = *Map.Table();
	FVector ZoneOne, ZoneSix, Water, ZoneSeven;
	if (!TestTrue(TEXT("PlayerStart3 is placed"), Map.Location(TEXT("Arena_01.PlayerStart3"), ZoneOne))
		|| !TestTrue(TEXT("PlayerStart21 is placed"), Map.Location(TEXT("Arena_01.PlayerStart21"), ZoneSix))
		|| !TestTrue(TEXT("WaterZone0 is placed"), Map.Location(TEXT("Arena_01.WaterZone0"), Water))
		|| !TestTrue(TEXT("an actor stands in zone 7"), Map.FirstLocationInZone(7, ZoneSeven)))
	{
		return false;
	}
	TestEqual(TEXT("PlayerStart3 stands in zone 1"), Map.ZoneAt(ZoneOne), 1);
	TestEqual(TEXT("PlayerStart21 stands in zone 6"), Map.ZoneAt(ZoneSix), 6);
	TestEqual(TEXT("WaterZone0 stands in zone 5"), Map.ZoneAt(Water), 5);
	UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, World);
	FReverbListener Listener;
	if (!Subsystem || !Listener.Spawn(*this, World, ZoneOne)) return false;
	WoTTest::TickWorld(&World, 2, ReverbStep);
	const FWoTZoneReverbSettings One = WoTZoneReverb::MakeSettings(Table.ZoneReverbs[1]);
	TestEqual(TEXT("at PlayerStart3 the listener is in zone 1"), Subsystem->GetListenerZone(), 1);
	TestTrue(TEXT("zone 1's reverb is in use"), Subsystem->GetCurrentSettings() == One);
	TestTrue(TEXT("which is Arena_01 zone 1's"), One == WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
	if (World.GetAudioDeviceRaw())
	{
		TestNotNull(TEXT("a reverb submix exists once a reverb is in use"), Subsystem->GetSubmix());
		if (TestNotNull(TEXT("with its preset"), Subsystem->GetPreset()))
		{
			TestTrue(TEXT("holding zone 1's reverb"), Subsystem->GetPreset()->GetSettings() == One);
		}
	}
	const int32 Changes = Subsystem->GetChangeCount();
	Listener.MoveTo(ZoneSix);
	WoTTest::TickWorld(&World, 3, ReverbStep);
	TestEqual(TEXT("at PlayerStart21 the listener is in zone 6"), Subsystem->GetListenerZone(), 6);
	TestTrue(TEXT("zone 6's reverb is in use"),
		Subsystem->GetCurrentSettings() == WoTZoneReverb::MakeSettings(Table.ZoneReverbs[6]));
	TestEqual(TEXT("one change for one zone change"), Subsystem->GetChangeCount(), Changes + 1);
	Listener.MoveTo(Water);
	WoTTest::TickWorld(&World, 1, ReverbStep);
	TestFalse(TEXT("in the water zone there is no reverb"), Subsystem->GetCurrentSettings().IsActive());
	if (Subsystem->GetPreset())
	{
		TestFalse(TEXT("and the effect is told so"), Subsystem->GetPreset()->GetSettings().IsActive());
	}
	Listener.MoveTo(ZoneOne);
	WoTTest::TickWorld(&World, 1, ReverbStep);
	TestTrue(TEXT("back in zone 1 its reverb returns"), Subsystem->GetCurrentSettings() == One);
	// The view target, not the player's body, chooses the zone.
	AActor* Watched = World.SpawnActor<AActor>(AActor::StaticClass(), ZoneSeven, FRotator::ZeroRotator, ReverbAlwaysSpawn());
	if (Watched && !Watched->GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(Watched);
		Watched->SetRootComponent(Root);
		Root->RegisterComponent();
		Watched->SetActorLocation(ZoneSeven);
	}
	if (TestNotNull(TEXT("an actor standing in zone 7"), Watched))
	{
		Listener.Controller->SetViewTarget(Watched);
		WoTTest::TickWorld(&World, 1, ReverbStep);
		TestEqual(TEXT("viewing it, the listener is in zone 7"), Subsystem->GetListenerZone(), 7);
		TestTrue(TEXT("zone 7's reverb is in use while the player stands in zone 1"),
			Subsystem->GetCurrentSettings() == WoTZoneReverb::MakeSettings(Table.ZoneReverbs[7]));
	}
	return !HasAnyErrors();
}

// Arena_09's zones 1 and 2 have the same reverb: walking between them changes
// nothing, so the tail carries on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbIdenticalZonesSpec,
	"WoT.Audio.ZoneReverb.Listener.IdenticalZonesDoNotReset", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbIdenticalZonesSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	FReverbProjectedMap Map;
	if (!ProjectReverbMap(*this, TEXT("Arena_09"), Map)) return !HasAnyErrors();
	UWorld& World = *Map.World;
	const UWoTMapLightTable& Table = *Map.Table();
	FVector ZoneOne, ZoneTwo;
	if (!TestTrue(TEXT("an actor stands in zone 1"), Map.FirstLocationInZone(1, ZoneOne))
		|| !TestTrue(TEXT("an actor stands in zone 2"), Map.FirstLocationInZone(2, ZoneTwo))) return false;
	TestTrue(TEXT("zones 1 and 2 carry the same reverb bytes"),
		ReverbBytes(Table.ZoneReverbs[1].Delay) == ReverbBytes(Table.ZoneReverbs[2].Delay)
		&& ReverbBytes(Table.ZoneReverbs[1].Gain) == ReverbBytes(Table.ZoneReverbs[2].Gain)
		&& Table.ZoneReverbs[1].bReverbZone && Table.ZoneReverbs[2].bReverbZone);
	UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, World);
	FReverbListener Listener;
	if (!Subsystem || !Listener.Spawn(*this, World, ZoneOne)) return false;
	WoTTest::TickWorld(&World, 2, ReverbStep);
	TestEqual(TEXT("the listener starts in zone 1"), Subsystem->GetListenerZone(), 1);
	TestTrue(TEXT("with zone 1's reverb"), Subsystem->GetCurrentSettings().IsActive());
	const int32 Changes = Subsystem->GetChangeCount();
	Listener.MoveTo(ZoneTwo);
	WoTTest::TickWorld(&World, 2, ReverbStep);
	TestEqual(TEXT("the listener is now in zone 2"), Subsystem->GetListenerZone(), 2);
	TestEqual(TEXT("the same reverb is not handed over again"), Subsystem->GetChangeCount(), Changes);
	Listener.MoveTo(ZoneOne);
	WoTTest::TickWorld(&World, 2, ReverbStep);
	TestEqual(TEXT("nor on the way back"), Subsystem->GetChangeCount(), Changes);
	return !HasAnyErrors();
}

namespace WoTZoneReverbSpec
{
// A two-zone table split by the plane Z = 0: zone 1 below (Arena_01 zone 1's
// reverb), zone 2 on and above (Arena_01 zone 6's), registered for World.
UWoTMapLightTable* TwoReverbZoneTable(UWorld& World, bool bBothReverb = true)
{
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(&World);
	Table->ZoneBspPlanes = {FVector4(0.0, 0.0, 1.0, 0.0)};
	Table->ZoneBspFront = {-3};
	Table->ZoneBspBack = {-2};
	Table->ZoneReverbs = {FWoTZoneReverbRecord(), ArenaOneZoneOne(),
		bBothReverb ? ArenaOneZoneSix() : FWoTZoneReverbRecord()};
	UWoTMeshLight::RegisterRuntimeMapLightTable(&World, Table);
	return Table;
}
}

// The zone is the one the view target's centre stands in, not the one its
// eyes or the camera are in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbActorCentreSpec,
	"WoT.Audio.ZoneReverb.Listener.ActorCentreNotCamera", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbActorCentreSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	TwoReverbZoneTable(*World);
	APlayerController* Controller = World->SpawnActor<APlayerController>(ReverbAlwaysSpawn());
	AWoTCharacter* Body = World->SpawnActor<AWoTCharacter>(
		FVector(0.0, 0.0, -10.0), FRotator::ZeroRotator, ReverbAlwaysSpawn());
	if (!TestNotNull(TEXT("controller"), Controller) || !TestNotNull(TEXT("body"), Body)) return false;
	Body->GetCharacterMovement()->SetMovementMode(MOVE_None);
	Body->GetCharacterMovement()->GravityScale = 0.0f;
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Body);
	Controller->SetViewTarget(Body);
	UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem) return false;
	WoTTest::TickWorld(World, 2, ReverbStep);
	// The eyes, where the player's camera sits.
	const FVector Eye = Body->GetPawnViewLocation();
	TestTrue(FString::Printf(TEXT("the body's centre is below the zone boundary (%s)"),
		*Body->GetActorLocation().ToString()), Body->GetActorLocation().Z < 0.0);
	TestTrue(FString::Printf(TEXT("its eyes are above it (%s)"), *Eye.ToString()), Eye.Z > 0.0);
	TestEqual(TEXT("the eyes are in zone 2"),
		UWoTZoneIndex::ZoneNumberAt(World, Eye), 2);
	TestEqual(TEXT("the listener's zone is the centre's"), Subsystem->GetListenerZone(), 1);
	TestTrue(TEXT("and so is the reverb"),
		Subsystem->GetCurrentSettings() == WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
	return !HasAnyErrors();
}

// Without a local player, on a dedicated server, or on a map without a zone
// table (the front end) there is no reverb: nothing is handed over, a server
// makes no submix and adds no send, and nothing breaks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbNoListenerSpec,
	"WoT.Audio.ZoneReverb.Listener.NoListenerNoServer", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbNoListenerSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		if (!TestNotNull(TEXT("spec world"), World)) return false;
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		TwoReverbZoneTable(*World);
		UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, *World);
		if (!Subsystem) return false;
		WoTTest::TickWorld(World, 3, ReverbStep);
		TestEqual(TEXT("no local player: no listener zone"), Subsystem->GetListenerZone(), INDEX_NONE);
		TestEqual(TEXT("no local player: nothing handed over"), Subsystem->GetChangeCount(), 0);
		TestNull(TEXT("no local player: no submix"), Subsystem->GetSubmix());
	}
	{
		// A map without a zone table, like the front end.
		UWorld* World = WoTTest::CreateSpecWorld();
		if (!TestNotNull(TEXT("spec world"), World)) return false;
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, *World);
		FReverbListener Listener;
		if (!Subsystem || !Listener.Spawn(*this, *World, FVector::ZeroVector)) return false;
		WoTTest::TickWorld(World, 3, ReverbStep);
		TestNull(TEXT("the map has no zone table"), UWoTMeshLight::FindMapLightTable(World));
		TestFalse(TEXT("no zone table: no reverb"), Subsystem->GetCurrentSettings().IsActive());
		TestEqual(TEXT("no zone table: nothing handed over"), Subsystem->GetChangeCount(), 0);
	}
#if WITH_EDITOR
	{
		UE::Net::FTestWorlds Worlds;
		if (!TestTrue(TEXT("the dedicated server loads"), Worlds.Server.IsLoaded())
			|| !TestTrue(TEXT("a client connects"), Worlds.CreateAndConnectClient()))
		{
			return false;
		}
		UWorld* Server = Worlds.Server.GetWorld();
		UWorld* Client = Worlds.Clients[0].GetWorld();
		if (!TestNotNull(TEXT("server world"), Server) || !TestNotNull(TEXT("client world"), Client)) return false;
		TestEqual(TEXT("the server is dedicated"), Server->GetNetMode(), NM_DedicatedServer);
		TwoReverbZoneTable(*Server);
		TwoReverbZoneTable(*Client);
		Worlds.TickAll(10);
		UWoTZoneReverbSubsystem* ServerReverb = UWoTZoneReverbSubsystem::Get(Server);
		UWoTZoneReverbSubsystem* ClientReverb = UWoTZoneReverbSubsystem::Get(Client);
		if (ServerReverb)
		{
			UAudioComponent* Voice = NewObject<UAudioComponent>(Server);
			UWoTZoneReverbSubsystem::AddSend(Server, *Voice);
			TestEqual(TEXT("the server hands no reverb over"), ServerReverb->GetChangeCount(), 0);
			TestNull(TEXT("the server makes no submix, even for a send"), ServerReverb->GetSubmix());
		}
		if (TestNotNull(TEXT("the client has the zone reverb"), ClientReverb))
		{
			TestTrue(TEXT("the client's listener hears its zone's reverb"),
				ClientReverb->GetCurrentSettings().IsActive());
		}
	}
#endif
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Which sounds reach the reverb
// ---------------------------------------------------------------------------

namespace WoTZoneReverbSpec
{
// The submix sends the engine's active sound for Voice carries.
TArray<FSoundSubmixSendInfo> ReverbActiveSends(UWorld& World, const UAudioComponent& Voice)
{
	TArray<FSoundSubmixSendInfo> Sends;
	FAudioDevice* Device = World.GetAudioDeviceRaw();
	if (!Device) return Sends;
	TArray<FSoundSubmixSendInfo>* Into = &Sends;
	Device->SendCommandToActiveSounds(Voice.GetAudioComponentID(), [Into](FActiveSound& Sound)
	{
		Sound.GetSoundSubmixSends(*Into);
	});
	FAudioCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();
	return Sends;
}

TArray<UAudioComponent*> PlayingVoices(UWorld& World)
{
	TArray<UAudioComponent*> Out;
	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		if (It->GetWorld() == &World && It->IsPlaying()) Out.Add(*It);
	}
	return Out;
}

// Exactly one send to the reverb submix, at 127/128, after distance.
bool HasReverbSend(FAutomationTestBase& Test, UWorld& World, const UAudioComponent& Voice,
	const USoundSubmix* Submix, const FString& What)
{
	int32 Count = 0;
	bool bLevel = true;
	for (const FSoundSubmixSendInfo& Send : ReverbActiveSends(World, Voice))
	{
		if (Send.SoundSubmix != Submix) continue;
		++Count;
		bLevel &= Send.SendLevel == 127.0f / 128.0f
			&& Send.SendLevelControlMethod == ESendLevelControlMethod::Manual
			&& Send.SendStage == ESubmixSendStage::PostDistanceAttenuation;
	}
	Test.TestEqual(What + TEXT(" has one send to the reverb"), Count, 1);
	Test.TestTrue(What + TEXT(" sends at 127/128 of its heard level"), bLevel);
	return Count == 1 && bLevel;
}
}

// Every effects voice feeds the reverb at 127/128 of its heard level: a
// one-shot, each of the four interface copies of a ClientPlaySound, and an
// ambient loop. A sound played outside the effects channels (as the front
// end's menu sounds are) does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbRoutingSpec,
	"WoT.Audio.ZoneReverb.Routing", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbRoutingSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	USoundBase* Wave = WoTSound::LoadBySubPath(FName(TEXT("FootStubA/STON")));
	if (!Wave)
	{
		AddWarning(TEXT("the imported sound is absent: the reverb sends are not verified"));
		return true;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!World->GetAudioDeviceRaw())
	{
		AddWarning(TEXT("no audio device: the reverb sends are not verified"));
		return true;
	}
	TwoReverbZoneTable(*World);
	FReverbListener Listener;
	if (!Listener.Spawn(*this, *World, FVector(0.0, 0.0, -100.0))) return false;
	UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem) return false;
	WoTTest::TickWorld(World, 1, ReverbStep);

	UAudioComponent* OneShot = WoTSound::PlayOneShot(World, Wave,
		FVector(100.0, 0.0, -100.0), 2000.0f, 1.0f, 1.0f);
	if (TestNotNull(TEXT("the one-shot starts"), OneShot))
	{
		HasReverbSend(*this, *World, *OneShot, Subsystem->GetSubmix(), TEXT("a one-shot"));
	}

	const TArray<UAudioComponent*> Earlier = PlayingVoices(*World);
	WoTSound::PlayClientSound(*Listener.Controller, Wave, 1.0f);
	int32 InterfaceVoices = 0;
	for (UAudioComponent* Voice : PlayingVoices(*World))
	{
		if (Earlier.Contains(Voice)) continue;
		++InterfaceVoices;
		HasReverbSend(*this, *World, *Voice, Subsystem->GetSubmix(),
			FString::Printf(TEXT("interface copy %d"), InterfaceVoices));
	}
	TestEqual(TEXT("ClientPlaySound plays four interface copies"), InterfaceVoices, 4);

	AWoTAmbientSound* Ambient = World->SpawnActorDeferred<AWoTAmbientSound>(
		AWoTAmbientSound::StaticClass(), FTransform(FVector(200.0, 0.0, -100.0)));
	Ambient->SoundSubPath = FName(TEXT("FootStubA/STON"));
	Ambient->SoundRadius = 64;
	Ambient->SoundVolume = 190;
	Ambient->SoundPitch = 64;
	Ambient->FinishSpawning(FTransform(FVector(200.0, 0.0, -100.0)));
	WoTTest::TickWorld(World, 2, ReverbStep);
	if (TestTrue(TEXT("the ambient loop plays"), Ambient->Ambient && Ambient->Ambient->IsPlaying()))
	{
		HasReverbSend(*this, *World, *Ambient->Ambient, Subsystem->GetSubmix(), TEXT("an ambient loop"));
	}

	UAudioComponent* Menu = UGameplayStatics::SpawnSound2D(World, Wave);
	if (TestNotNull(TEXT("a sound outside the effects channels plays"), Menu))
	{
		int32 ToReverb = 0;
		for (const FSoundSubmixSendInfo& Send : ReverbActiveSends(*World, *Menu))
		{
			ToReverb += Send.SoundSubmix == Subsystem->GetSubmix() ? 1 : 0;
		}
		TestEqual(TEXT("it does not reach the reverb"), ToReverb, 0);
	}
	return !HasAnyErrors();
}

// The "Room reverb" audio setting: on by default, saved with the other
// settings (version 12; older records keep it on), and heard at once: off is
// no reverb in any zone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbRoomReverbSettingSpec,
	"WoT.Audio.ZoneReverb.RoomReverbSetting", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbRoomReverbSettingSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	const FWoTGameSettings Defaults = FWoTGameSettings::Defaults();
	TestTrue(TEXT("room reverb is on by default"), Defaults.bRoomReverb);
	FWoTGameSettings Off = Defaults;
	Off.bRoomReverb = false;
	const FString Encoded = FWoTGameSettingsCodec::Serialize(Off);
	TestTrue(TEXT("the setting is saved"), Encoded.Contains(TEXT("\nGameRoomReverb=0\n")));
	auto Parse = [](const FString& Text, FWoTGameSettings& Out)
	{
		const FTCHARToUTF8 Utf8(*(FString(TEXT("Schema=EarlyUI-v1\n")) + Text));
		FString Error;
		return FWoTGameSettingsCodec::Parse(MakeArrayView(
			reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, Error);
	};
	FWoTGameSettings Parsed;
	TestTrue(TEXT("a saved off parses"), Parse(Encoded, Parsed));
	TestFalse(TEXT("and stays off"), Parsed.bRoomReverb);
	FString Older = Encoded;
	Older.ReplaceInline(TEXT("GameSettingsVersion=12"), TEXT("GameSettingsVersion=11"));
	FString WithRecord = Older;
	Older.ReplaceInline(TEXT("GameRoomReverb=0\n"), TEXT(""));
	TestTrue(TEXT("a version 11 record parses"), Parse(Older, Parsed));
	TestTrue(TEXT("with room reverb on"), Parsed.bRoomReverb);
	TestFalse(TEXT("a version 11 record cannot carry the room reverb record"), Parse(WithRecord, Parsed));

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	const bool bWasEnabled = WoTZoneReverb::IsRoomReverbEnabled();
	ON_SCOPE_EXIT
	{
		WoTZoneReverb::SetRoomReverbEnabled(bWasEnabled);
		WoTTest::DestroySpecWorld(World);
	};
	TwoReverbZoneTable(*World);
	FReverbListener Listener;
	UWoTZoneReverbSubsystem* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem || !Listener.Spawn(*this, *World, FVector(0.0, 0.0, -100.0))) return false;
	FWoTGameSettingsAdapter::ApplyAudio(Defaults);
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestTrue(TEXT("on: the zone's reverb"), Subsystem->GetCurrentSettings().IsActive());
	FWoTGameSettingsAdapter::ApplyAudio(Off);
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestFalse(TEXT("off: no reverb from the next frame"), Subsystem->GetCurrentSettings().IsActive());
	FWoTGameSettingsAdapter::ApplyAudio(Defaults);
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestTrue(TEXT("on again: the reverb returns"), Subsystem->GetCurrentSettings().IsActive());
	return !HasAnyErrors();
}

// Publish at the end of the preset callback, before FSoundEffectBase::Update
// clears its changed flag. The next block must still take the new settings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbLatestPublishSpec,
	"WoT.Audio.ZoneReverb.LatestPublishSurvivesCallback", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbLatestPublishSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	struct FInterleavedEffect : FWoTZoneReverb
	{
		TFunction<void()> Publish;
		virtual void OnPresetChanged() override
		{
			FWoTZoneReverb::OnPresetChanged();
			if (Publish) { auto Once = MoveTemp(Publish); Once(); }
		}
	};
	const auto One = WoTZoneReverb::MakeSettings(ArenaOneZoneOne());
	const auto Six = WoTZoneReverb::MakeSettings(ArenaOneZoneSix());
	FReverbEffect Harness(One);
	TSoundEffectPtr Old = Harness.Effect;
	USoundEffectPreset::UnregisterInstance(Old);
	auto Interleaved = MakeShared<FInterleavedEffect, ESPMode::ThreadSafe>();
	FSoundEffectSubmixInitData Init;
	Init.SampleRate = ReverbRate;
	Interleaved->Setup(Init);
	USoundEffectPreset::RegisterInstance(*Harness.Preset, Interleaved);
	Interleaved->SetEnabled(true);
	Harness.Effect = Interleaved;
	Interleaved->Publish = [&] { Harness.Preset->SetSettings(Six); };
	TArray<float> Output;
	Harness.Run(ReverbImpulse(64, 0), 32, Output);
	TestTrue(TEXT("a publish during the callback reaches the next block"),
		Harness.Effect->GetProcessor().GetSettings() == Six);
	Harness.Preset->SetSettings(One);
	Interleaved->Publish = [&] { Harness.Preset->SetSettings({}); };
	Output.Reset();
	Harness.Run(ReverbImpulse(64, 0), 32, Output);
	TestFalse(TEXT("an off publish during the callback also wins"),
		Harness.Effect->GetProcessor().IsActive());
	TestTrue(TEXT("off silences the next block"), ReverbAllZero(MakeArrayView(Output).Right(64)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbDeviceFoldSpec,
	"WoT.Audio.ZoneReverb.DeviceChannelsMatchStereoFold", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbDeviceFoldSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	constexpr int32 Frames = 64;
	for (int32 Channels : {1, 2, 4, 6, 8})
	{
		Audio::FAlignedFloatBuffer Map;
		Audio::FMixerDevice::Get2DChannelMap(false, Channels, 2, false, Map);
		for (int32 Source = 0; Source < Channels; ++Source)
		{
			FReverbEffect Device(WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
			FReverbEffect Stereo(WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
			Audio::FAlignedFloatBuffer In, Out;
			In.SetNumZeroed(Frames * Channels);
			In[Source] = 1.0f;
			Out.Init(7.0f, Frames * Channels);
			TArray<float> Folded, Expected;
			Folded.SetNumZeroed(Frames * 2);
			// UE's DownmixBuffer indexes weights by input channel, then output.
			Folded[0] = Map[Source * 2];
			Folded[1] = Map[Source * 2 + 1];
			Stereo.Run(Folded, 32, Expected);
			FSoundEffectSubmixInputData Input;
			Input.NumFrames = Frames;
			Input.NumChannels = Input.NumDeviceChannels = Channels;
			Input.AudioBuffer = &In;
			FSoundEffectSubmixOutputData Output;
			Output.NumChannels = Channels;
			Output.AudioBuffer = &Out;
			Device.Effect->ProcessAudio(Input, Output);
			bool bMatches = true;
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				for (int32 Channel = 0; Channel < Channels; ++Channel)
				{
					const float Want = Channels == 1
						? (Expected[Frame * 2] + Expected[Frame * 2 + 1]) * 0.5f
						: Channel < 2 ? Expected[Frame * 2 + Channel] : 0.0f;
					bMatches &= FMath::IsNearlyEqual(Out[Frame * Channels + Channel], Want, 1.e-7f);
				}
			}
			TestTrue(FString::Printf(TEXT("%d channels, source %d matches UE's stereo fold"), Channels, Source), bMatches);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbLocalControllerSpec,
	"WoT.Audio.ZoneReverb.Listener.FirstLocalControllerOnly", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbLocalControllerSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	TwoReverbZoneTable(*World);
	auto* Remote = World->SpawnActor<APlayerController>(ReverbAlwaysSpawn());
	if (!TestNotNull(TEXT("remote controller"), Remote)) return false;
	TestFalse(TEXT("the first controller is not local"), Remote->IsLocalController());
	auto* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem) return false;
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestFalse(TEXT("remote controllers alone select no reverb"), Subsystem->GetCurrentSettings().IsActive());
	TestEqual(TEXT("remote controllers alone select no zone"), Subsystem->GetListenerZone(), INDEX_NONE);
	FReverbListener Local;
	if (!Local.Spawn(*this, *World, FVector(0, 0, -100))) return false;
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestTrue(TEXT("a later local controller selects its own zone"),
		Subsystem->GetCurrentSettings() == WoTZoneReverb::MakeSettings(ArenaOneZoneOne()));
	Local.Controller->Destroy();
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestFalse(TEXT("losing the local controller cuts the reverb"), Subsystem->GetCurrentSettings().IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbPausedSettingSpec,
	"WoT.Audio.ZoneReverb.RoomReverbSettingWhilePaused", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbPausedSettingSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	const bool bWasEnabled = WoTZoneReverb::IsRoomReverbEnabled();
	ON_SCOPE_EXIT { WoTZoneReverb::SetRoomReverbEnabled(bWasEnabled); WoTTest::DestroySpecWorld(World); };
	WoTZoneReverb::SetRoomReverbEnabled(true);
	TwoReverbZoneTable(*World);
	FReverbListener Listener;
	if (!Listener.Spawn(*this, *World, FVector(0, 0, -100))) return false;
	auto* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem) return false;
	WoTTest::TickWorld(World, 1, ReverbStep);
	TestTrue(TEXT("the room starts active"), Subsystem->GetCurrentSettings().IsActive());
	// Drive the engine's paused tick dispatch, not the subsystem directly.
	WoTZoneReverb::SetRoomReverbEnabled(false);
	FTickableGameObject::TickObjects(World, LEVELTICK_All, true, ReverbStep);
	TestFalse(TEXT("a paused frame applies off"), Subsystem->GetCurrentSettings().IsActive());
	if (Subsystem->GetPreset()) TestFalse(TEXT("the effect receives off while paused"), Subsystem->GetPreset()->GetSettings().IsActive());
	WoTZoneReverb::SetRoomReverbEnabled(true);
	FTickableGameObject::TickObjects(World, LEVELTICK_All, true, ReverbStep);
	TestTrue(TEXT("a paused frame also restores on"), Subsystem->GetCurrentSettings().IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneReverbDeviceOwnershipSpec,
	"WoT.Audio.ZoneReverb.SubmixBelongsToOwningDevice", WoTZoneReverbSpec::Flags)

bool FWoTZoneReverbDeviceOwnershipSpec::RunTest(const FString&)
{
	using namespace WoTZoneReverbSpec;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { if (World) WoTTest::DestroySpecWorld(World); };
	FAudioDeviceHandle Owner = World->GetAudioDevice();
	if (!TestTrue(TEXT("the owning audio device exists"), Owner.IsValid())) return false;
	TwoReverbZoneTable(*World);
	FReverbListener Listener;
	if (!Listener.Spawn(*this, *World, FVector(0, 0, -100))) return false;
	WoTTest::TickWorld(World, 1, ReverbStep);
	auto* Subsystem = ReverbOf(*this, *World);
	if (!Subsystem || !TestNotNull(TEXT("the reverb submix exists"), Subsystem->GetSubmix())) return false;
	TStrongObjectPtr<USoundSubmix> Submix(Subsystem->GetSubmix());
	TestTrue(TEXT("the world submix is dynamic"), Submix->IsDynamic(false));
	FAudioDeviceParams Params;
	Params.Scope = EAudioDeviceScope::Unique;
	Params.bIsNonRealtime = true;
	FAudioDeviceHandle Foreign = FAudioDeviceManager::Get()->RequestAudioDevice(Params);
	if (!TestTrue(TEXT("a second audio device exists"), Foreign.IsValid())) return false;
	auto HasInstance = [&](const FAudioDeviceHandle& Handle)
	{
		bool bFound = false;
		FAudioThread::RunCommandOnAudioThread([&]
		{
			bFound = static_cast<Audio::FMixerDevice*>(Handle.GetAudioDevice())->GetSubmixInstance(Submix.Get()).IsValid();
		});
		FAudioCommandFence Fence;
		Fence.BeginFence(); Fence.Wait();
		return bFound;
	};
	TestTrue(TEXT("the owning device loads the reverb"), HasInstance(Owner));
	TestFalse(TEXT("a newly initialized foreign device does not load it"), HasInstance(Foreign));
	WoTTest::DestroySpecWorld(World);
	World = nullptr;
	TestFalse(TEXT("world teardown removes the owning device's reverb"), HasInstance(Owner));
	TestFalse(TEXT("world teardown leaves no foreign reverb"), HasInstance(Foreign));
	return !HasAnyErrors();
}

#endif
