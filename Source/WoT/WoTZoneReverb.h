#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundEffectSubmix.h"
#include "WoTZoneReverb.generated.h"

struct FWoTZoneReverbRecord;

// The original game gave rooms an echo. Each zone of a map may be a reverb
// zone (ZoneInfo bReverbZone, MasterGain, CutoffHz, Delay[6] and Gain[6], the
// set WOTPlayer.uc LogReverb reports from the player's zone). The zone the
// local player's view target stands in selects the reverb heard, once per
// frame (UWoTZoneReverbSubsystem). A reverb zone's MasterGain sets the
// reverb's level (/255) and its CutoffHz, limited to 1000 Hz, damps the
// echoes' treble. Its six Delay/Gain pairs configure three echo stages per ear
// (pairs 0/2/4 left, 1/3/5 right): each stage delays by Delay x 2 ms (1 ms to
// 340 ms) and recirculates by 1 - Gain/255 (Gain limited to 0.001..0.999 of
// 255), through a gentle low-pass; the two ears' outputs feed each other back
// at half level. Every game sound enters the reverb at its heard level, after
// panning; the reverb is added on top of the unchanged direct sound. Changing
// to a different reverb restarts it from silence; leaving reverb zones
// silences it at once.

// A zone's reverb after the original's limits: the values the game compares
// from one frame to the next. All zero (Volume 0) is no reverb.
struct FWoTZoneReverbSettings
{
	// MasterGain / 255: the level the reverb is added at.
	float Volume = 0.0f;
	// The damping filter's cutoff: CutoffHz limited to 0..1000 Hz.
	int32 CutoffHz = 0;
	// Each pair's echo time in seconds: Delay x 2 ms, limited to 1..340 ms.
	float Time[6] = {};
	// Each pair's Gain / 255, limited to 0.001..0.999.
	float Gain[6] = {};

	bool IsActive() const { return Volume != 0.0f; }
	WOT_API bool operator==(const FWoTZoneReverbSettings& Other) const;
	bool operator!=(const FWoTZoneReverbSettings& Other) const { return !(*this == Other); }
};

namespace WoTZoneReverb
{
inline constexpr int32 PairCount = 6;
inline constexpr int32 StageCount = 3;

// Every sound enters the reverb at 127/128 of the level it is heard at.
inline constexpr float SendLevel = 127.0f / 128.0f;

// The reverb a zone selects; no reverb unless the zone is a reverb zone with a
// master gain. A record without six delay/gain pairs is no reverb.
WOT_API FWoTZoneReverbSettings MakeSettings(const FWoTZoneReverbRecord& Zone);

// The reverb's stages at one sample rate.
struct FStages
{
	float Volume = 0.0f;
	// The damping low-pass: each stage's filter keeps 1 - Alpha of its last
	// value and takes Alpha of the delayed sample.
	float Alpha = 0.0f;
	// Per pair (left 0/2/4, right 1/3/5): the delay in whole samples and the
	// feedback 1 - Gain.
	int32 Delay[6] = {};
	double Feedback[6] = {};
};

WOT_API FStages MakeStages(const FWoTZoneReverbSettings& Settings, float SampleRate);

// Frames in each stage's delay line at SampleRate: the power of two that holds
// a 340 ms echo.
WOT_API int32 LineFrames(float SampleRate);

// The reverb itself: three damped allpass stages per ear, in series, whose
// last outputs feed the other ear's input. Linear and time-invariant between
// settings changes; the per-sample cross feed keeps it sample by sample.
class WOT_API FProcessor
{
public:
	// Takes Settings at SampleRate. Different settings restart the reverb from
	// silence (every delay line, filter and the cross feed cleared); the same
	// settings again change nothing.
	void Configure(const FWoTZoneReverbSettings& Settings, float SampleRate);

	// Runs NumFrames of interleaved input, folded with the fixed stereo weights
	// supplied by the effect (without weights, the first pair feeds the ears).
	// Writes the reverb alone into
	// interleaved Output: left and right in its first two channels (their mean
	// in a single channel), zero in every other. Inactive settings write zeros.
	void Process(const float* Input, int32 InChannels, int32 NumFrames,
		float* Output, int32 OutChannels, const float* StereoWeights = nullptr);

	bool IsActive() const { return Settings.IsActive(); }
	const FWoTZoneReverbSettings& GetSettings() const { return Settings; }
	int32 GetRestartCount() const { return Restarts; }

private:
	struct FStage
	{
		int32 Delay[2] = {};
		float G[2] = {};
		float NegG[2] = {};
		float OneMinusG2[2] = {};
		float LowPass[2] = {};
	};

	void Restart();

	FWoTZoneReverbSettings Settings;
	float Rate = 0.0f;
	FStage Stages[StageCount];
	float Volume = 0.0f;
	float Alpha = 0.0f;
	float OneMinusAlpha = 1.0f;
	TArray<float> Lines;
	int32 Frames = 0;
	int32 Write = 0;
	float LastOut[2] = {};
	int32 Restarts = 0;
};

// The "Room reverb" audio setting (FWoTGameSettings::bRoomReverb, applied by
// FWoTGameSettingsAdapter::ApplyAudio). Off makes every zone dry, as the
// original's own reverb option did.
WOT_API void SetRoomReverbEnabled(bool bEnabled);
WOT_API bool IsRoomReverbEnabled();
}

// The reverb as a submix effect on a wet-only submix: its input is every
// voice's reverb send, its output the reverb alone. It always writes its
// output (zeros when inactive): a submix effect that declines to process
// passes its input through instead.
class WOT_API FWoTZoneReverb : public FSoundEffectSubmix
{
public:
	virtual void OnPresetChanged() override;
	// Accept device channels; fold inside the processor without the mixer's
	// per-block channel-map allocation (AudioMixerSubmix.cpp DownmixBuffer).
	virtual void OnProcessAudio(const FSoundEffectSubmixInputData& InData,
		FSoundEffectSubmixOutputData& OutData) override;

	const WoTZoneReverb::FProcessor& GetProcessor() const { return Processor; }

private:
	virtual void Init(const FSoundEffectSubmixInitData& InData) override;

	WoTZoneReverb::FProcessor Processor;
	float SampleRate = 48000.0f;
	// UE's stereo fold for each supported input count, captured once at Init.
	float StereoWeights[8][16] = {};
};

// The reverb's submix effect preset. Runtime only: one per reverb submix,
// owned and kept alive by UWoTZoneReverbSubsystem.
UCLASS()
class WOT_API UWoTZoneReverbPreset : public USoundEffectSubmixPreset
{
	GENERATED_BODY()

public:
	EFFECT_PRESET_METHODS(WoTZoneReverb)

	// Hands the game thread's settings to the effect, which takes them before
	// its next audio block.
	void SetSettings(const FWoTZoneReverbSettings& InSettings);

	// Copied to the effect's view when the first effect attaches (the
	// macro's Init), so it always holds the latest settings too.
	FWoTZoneReverbSettings Settings;
};
