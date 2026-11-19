#include "WoTZoneReverb.h"

#include "WoTMapLightTable.h"
#include "AudioMixerDevice.h"

#include <atomic>

namespace
{
std::atomic<bool> GRoomReverbEnabled{true};

// The original's limits are single-precision constants applied in double.
constexpr float TwoMilliseconds = 0.002f;
constexpr float ShortestTime = 0.001f;
constexpr float LongestTime = 0.340f;
constexpr float SmallestGain = 0.001f;
constexpr float LargestGain = 0.999f;
constexpr float InverseByte = 1.0f / 255.0f;
constexpr int32 HighestCutoffHz = 1000;
// The damping filter's angular factor: the original uses 6.282, a little
// under two pi.
constexpr float AngularFactor = 6.282f;
}

bool FWoTZoneReverbSettings::operator==(const FWoTZoneReverbSettings& Other) const
{
	if (Volume != Other.Volume || CutoffHz != Other.CutoffHz) return false;
	for (int32 Pair = 0; Pair < WoTZoneReverb::PairCount; ++Pair)
	{
		if (Time[Pair] != Other.Time[Pair] || Gain[Pair] != Other.Gain[Pair]) return false;
	}
	return true;
}

namespace WoTZoneReverb
{
FWoTZoneReverbSettings MakeSettings(const FWoTZoneReverbRecord& Zone)
{
	FWoTZoneReverbSettings Out;
	if (!Zone.bReverbZone || Zone.MasterGain == 0
		|| Zone.Delay.Num() != PairCount || Zone.Gain.Num() != PairCount)
	{
		return Out;
	}
	Out.Volume = static_cast<float>(Zone.MasterGain * (1.0 / 255.0));
	Out.CutoffHz = FMath::Clamp(Zone.CutoffHz, 0, HighestCutoffHz);
	for (int32 Pair = 0; Pair < PairCount; ++Pair)
	{
		double Time = Zone.Delay[Pair] * static_cast<double>(TwoMilliseconds);
		Time = Time < static_cast<double>(ShortestTime) ? static_cast<double>(ShortestTime)
			: Time >= static_cast<double>(LongestTime) ? static_cast<double>(LongestTime) : Time;
		Out.Time[Pair] = static_cast<float>(Time);
		double Gain = Zone.Gain[Pair] * static_cast<double>(InverseByte);
		Gain = Gain < static_cast<double>(SmallestGain) ? static_cast<double>(SmallestGain)
			: Gain >= static_cast<double>(LargestGain) ? static_cast<double>(LargestGain) : Gain;
		Out.Gain[Pair] = static_cast<float>(Gain);
	}
	return Out;
}

FStages MakeStages(const FWoTZoneReverbSettings& Settings, float SampleRate)
{
	FStages Out;
	if (!Settings.IsActive() || !(SampleRate > 0.0f)) return Out;
	Out.Volume = Settings.Volume;
	// A one-pole low-pass set from the cutoff.
	const double Omega = static_cast<double>(Settings.CutoffHz)
		* static_cast<double>(AngularFactor) / static_cast<double>(SampleRate);
	const double C = 1.0 - FMath::Cos(Omega);
	Out.Alpha = static_cast<float>(FMath::Sqrt(C * (C + 2.0)) - C);
	for (int32 Pair = 0; Pair < PairCount; ++Pair)
	{
		// Whole samples, rounded toward zero.
		Out.Delay[Pair] = static_cast<int32>(
			static_cast<double>(SampleRate) * static_cast<double>(Settings.Time[Pair]));
		// A high Gain means little recirculation.
		Out.Feedback[Pair] = 1.0 - static_cast<double>(Settings.Gain[Pair]);
	}
	return Out;
}

int32 LineFrames(float SampleRate)
{
	const int32 Needed = static_cast<int32>(
		static_cast<double>(FMath::Max(SampleRate, 1.0f)) * static_cast<double>(LongestTime)) + 1;
	return static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(Needed)));
}

void FProcessor::Configure(const FWoTZoneReverbSettings& InSettings, float SampleRate)
{
	if (InSettings == Settings && SampleRate == Rate) return;
	Settings = InSettings;
	Rate = SampleRate;
	const FStages Built = MakeStages(Settings, Rate);
	Volume = Built.Volume;
	Alpha = Built.Alpha;
	OneMinusAlpha = static_cast<float>(1.0 - static_cast<double>(Alpha));
	for (int32 Stage = 0; Stage < StageCount; ++Stage)
	{
		for (int32 Ear = 0; Ear < 2; ++Ear)
		{
			const int32 Pair = Stage * 2 + Ear;
			const double Feedback = Built.Feedback[Pair];
			FStage& Into = Stages[Stage];
			Into.Delay[Ear] = Built.Delay[Pair];
			Into.G[Ear] = static_cast<float>(Feedback);
			Into.NegG[Ear] = static_cast<float>(-Feedback);
			Into.OneMinusG2[Ear] = static_cast<float>(1.0 - Feedback * Feedback);
		}
	}
	if (IsActive())
	{
		const int32 Needed = LineFrames(Rate);
		if (Frames != Needed)
		{
			Frames = Needed;
			Lines.SetNumUninitialized(StageCount * Frames * 2);
		}
	}
	Restart();
}

void FProcessor::Restart()
{
	// Every change starts the reverb from silence: no tail of the previous
	// settings carries over.
	if (!Lines.IsEmpty())
	{
		FMemory::Memzero(Lines.GetData(), Lines.Num() * sizeof(float));
	}
	for (FStage& Stage : Stages)
	{
		Stage.LowPass[0] = Stage.LowPass[1] = 0.0f;
	}
	LastOut[0] = LastOut[1] = 0.0f;
	Write = 0;
	++Restarts;
}

void FProcessor::Process(const float* Input, int32 InChannels, int32 NumFrames,
	float* Output, int32 OutChannels, const float* StereoWeights)
{
	if (NumFrames <= 0 || OutChannels <= 0 || Output == nullptr) return;
	FMemory::Memzero(Output, static_cast<SIZE_T>(NumFrames) * OutChannels * sizeof(float));
	if (!IsActive() || Input == nullptr || InChannels <= 0 || Frames <= 0) return;
	const int32 Mask = Frames - 1;
	float* const Line[StageCount] = {
		Lines.GetData(), Lines.GetData() + Frames * 2, Lines.GetData() + Frames * 4};
	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		const float* In = Input + static_cast<SIZE_T>(Frame) * InChannels;
		float InLeft = In[0];
		float InRight = InChannels > 1 ? In[1] : In[0];
		if (StereoWeights)
		{
			InLeft = InRight = 0.0f;
			for (int32 Channel = 0; Channel < InChannels; ++Channel)
			{
				InLeft += In[Channel] * StereoWeights[Channel * 2];
				InRight += In[Channel] * StereoWeights[Channel * 2 + 1];
			}
		}
		// Each ear takes half of its own send and half of the other ear's
		// last output.
		float X[2] = {(InLeft + LastOut[1]) * 0.5f, (InRight + LastOut[0]) * 0.5f};
		for (int32 Stage = 0; Stage < StageCount; ++Stage)
		{
			FStage& S = Stages[Stage];
			float* Samples = Line[Stage];
			for (int32 Ear = 0; Ear < 2; ++Ear)
			{
				const float Delayed = Samples[((Write - S.Delay[Ear]) & Mask) * 2 + Ear];
				// The damping low-pass filters only what recirculates.
				const float Damped = Delayed * Alpha + S.LowPass[Ear] * OneMinusAlpha;
				S.LowPass[Ear] = Damped;
				Samples[Write * 2 + Ear] = X[Ear] + Damped * S.G[Ear];
				// The allpass output. Its undelayed part (-g x) inverts the input,
				// so the reverb's first sample, added to the direct sound, makes
				// the direct sound slightly quieter in a reverb zone (about 1 dB).
				// That is the original's sound; keep it.
				X[Ear] = X[Ear] * S.NegG[Ear] + Delayed * S.OneMinusG2[Ear];
			}
		}
		LastOut[0] = X[0];
		LastOut[1] = X[1];
		float* Out = Output + static_cast<SIZE_T>(Frame) * OutChannels;
		if (OutChannels > 1)
		{
			Out[0] = X[0] * Volume;
			Out[1] = X[1] * Volume;
		}
		else
		{
			Out[0] = (X[0] * Volume + X[1] * Volume) * 0.5f;
		}
		Write = (Write + 1) & Mask;
	}
}

void SetRoomReverbEnabled(bool bEnabled)
{
	GRoomReverbEnabled.store(bEnabled);
}

bool IsRoomReverbEnabled()
{
	return GRoomReverbEnabled.load();
}
}

void UWoTZoneReverbPreset::SetSettings(const FWoTZoneReverbSettings& InSettings)
{
	{
		// The effect's first attach copies Settings under this lock.
		FScopeLock Lock(&SettingsCritSect);
		Settings = InSettings;
	}
	UpdateSettings(InSettings);
	// FSoundEffectBase::Update clears its changed flag after OnPresetChanged.
	// Its command queue preserves publishes that arrive during that callback.
	// Read the latest snapshot when consumed, so delayed commands cannot
	// restore an older reverb and restart a newer tail.
	EffectCommand<FWoTZoneReverb>([](FWoTZoneReverb& Effect)
	{
		Effect.OnPresetChanged();
	});
}

void FWoTZoneReverb::Init(const FSoundEffectSubmixInitData& InData)
{
	SampleRate = InData.SampleRate > 0.0f ? InData.SampleRate : 48000.0f;
	Audio::FAlignedFloatBuffer Map;
	for (int32 Channels = 1; Channels <= UE_ARRAY_COUNT(StereoWeights); ++Channels)
	{
		Map.Reset();
		Audio::FMixerDevice::Get2DChannelMap(false, Channels, 2, false, Map);
		FMemory::Memcpy(StereoWeights[Channels - 1], Map.GetData(), Channels * 2 * sizeof(float));
	}
}

void FWoTZoneReverb::OnPresetChanged()
{
	GET_EFFECT_SETTINGS(WoTZoneReverb);
	Processor.Configure(Settings, SampleRate);
}

void FWoTZoneReverb::OnProcessAudio(const FSoundEffectSubmixInputData& InData,
	FSoundEffectSubmixOutputData& OutData)
{
	if (OutData.AudioBuffer == nullptr || OutData.NumChannels <= 0) return;
	const int32 NumFrames = OutData.AudioBuffer->Num() / OutData.NumChannels;
	const float* Input = InData.AudioBuffer != nullptr && InData.NumChannels > 0
		&& InData.AudioBuffer->Num() >= NumFrames * InData.NumChannels
		? InData.AudioBuffer->GetData() : nullptr;
	Processor.Process(Input, InData.NumChannels, NumFrames,
		OutData.AudioBuffer->GetData(), OutData.NumChannels,
		InData.NumChannels > 0 && InData.NumChannels <= UE_ARRAY_COUNT(StereoWeights)
			? StereoWeights[InData.NumChannels - 1] : nullptr);
}
