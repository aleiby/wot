#include "WoTPayloadSoundWave.h"

#include "Sound/SoundGenerator.h"
#include "WoTPayloadSubsystem.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogWoTPayloadSound, Log, All);

namespace
{
	std::atomic<int64> GWoTPayloadGenerators{0};
	std::atomic<int64> GWoTPayloadNullGenerators{0};
	std::atomic<int64> GWoTPayloadSamples{0};
	std::atomic<int64> GWoTPayloadNonZeroSamples{0};

	class FWoTPayloadSoundGenerator final : public ISoundGenerator
	{
	public:
		FWoTPayloadSoundGenerator(
			TSharedRef<const FWoTSoundPayloadData> InPayload, bool bInLooping,
			float StartTime)
			: Payload(MoveTemp(InPayload))
			, bLooping(bInLooping)
		{
			if (bLooping && !Payload->Loops.IsEmpty())
			{
				LoopStartFrame = Payload->Loops[0].StartFrame;
				LoopEndFrame = Payload->Loops[0].EndFrameExclusive;
				bHasLoopRegion = LoopStartFrame < LoopEndFrame
					&& LoopEndFrame <= Payload->FrameCount;
			}
			if (Payload->FrameCount > 0 && StartTime > 0.0f)
			{
				const double StartFrameDouble = FMath::FloorToDouble(
					static_cast<double>(StartTime) * Payload->SampleRate);
				const uint64 StartFrame = StartFrameDouble >= static_cast<double>(MAX_uint64)
					? MAX_uint64 : static_cast<uint64>(StartFrameDouble);
				if (bHasLoopRegion && StartFrame >= LoopEndFrame)
				{
					FrameCursor = LoopStartFrame
						+ (StartFrame - LoopStartFrame)
						% (LoopEndFrame - LoopStartFrame);
				}
				else
				{
					FrameCursor = bLooping ? StartFrame % Payload->FrameCount
						: FMath::Min(StartFrame, Payload->FrameCount);
				}
			}
			bFinished = !bLooping && FrameCursor >= Payload->FrameCount;
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			if (!OutAudio || NumSamples <= 0)
			{
				return 0;
			}
			const int32 Channels = Payload->Channels;
			const int32 WholeSampleCount = NumSamples - (NumSamples % Channels);
			const int32 PartialSampleCount = NumSamples - WholeSampleCount;
			if (PartialSampleCount > 0)
			{
				// A generator callback may request a sample count that is not a whole
				// interleaved frame. Silence that tail without consuming source PCM.
				FMemory::Memzero(OutAudio + WholeSampleCount,
					PartialSampleCount * sizeof(float));
			}
			if (bFinished)
			{
				return 0;
			}
			if (Payload->FrameCount == 0)
			{
				bFinished = true;
				return 0;
			}

			int32 Written = 0;
			while (Written < WholeSampleCount)
			{
				const uint64 PlaybackEnd = bHasLoopRegion
					? LoopEndFrame : Payload->FrameCount;
				if (FrameCursor >= PlaybackEnd)
				{
					if (!bLooping)
					{
						bFinished = true;
						break;
					}
					FrameCursor = bHasLoopRegion ? LoopStartFrame : 0;
				}

				const uint64 FirstSample = FrameCursor * Channels;
				for (int32 Channel = 0; Channel < Channels; ++Channel)
				{
					const uint64 SampleIndex = FirstSample + Channel;
					if (Payload->BitsPerSample == 16)
					{
						const uint64 ByteOffset = SampleIndex * 2;
						const uint16 Packed = static_cast<uint16>(Payload->PCMBytes[
							static_cast<int32>(ByteOffset)])
							| (static_cast<uint16>(Payload->PCMBytes[
								static_cast<int32>(ByteOffset + 1)]) << 8);
						const int16 Sample = static_cast<int16>(Packed);
						OutAudio[Written + Channel] =
							static_cast<float>(Sample) / 32768.0f;
					}
					else
					{
						const uint8 Sample = Payload->PCMBytes[
							static_cast<int32>(SampleIndex)];
						OutAudio[Written + Channel] =
							(static_cast<float>(Sample) - 128.0f) / 128.0f;
					}
				}
				++FrameCursor;
				Written += Channels;
			}
			if (!bLooping && FrameCursor >= Payload->FrameCount)
			{
				bFinished = true;
			}
#if !UE_BUILD_SHIPPING
			int64 NonZero = 0;
			for (int32 Index = 0; Index < Written; ++Index)
			{
				NonZero += OutAudio[Index] != 0.0f ? 1 : 0;
			}
			GWoTPayloadSamples += Written;
			GWoTPayloadNonZeroSamples += NonZero;
#endif
			// Count the explicit zero tail only when every requested whole frame was
			// produced; a short non-looping source still reports its true prefix.
			return Written == WholeSampleCount ? NumSamples : Written;
		}

		virtual bool IsFinished() const override { return bFinished; }
		virtual int32 GetNumChannels() const override { return Payload->Channels; }

	private:
		TSharedRef<const FWoTSoundPayloadData> Payload;
		uint64 FrameCursor = 0;
		uint64 LoopStartFrame = 0;
		uint64 LoopEndFrame = 0;
		bool bHasLoopRegion = false;
		bool bLooping = false;
		bool bFinished = false;
	};
}

FWoTPayloadSoundStats GetWoTPayloadSoundStats()
{
	FWoTPayloadSoundStats Stats;
	Stats.Generators = GWoTPayloadGenerators.load();
	Stats.NullGenerators = GWoTPayloadNullGenerators.load();
	Stats.Samples = GWoTPayloadSamples.load();
	Stats.NonZeroSamples = GWoTPayloadNonZeroSamples.load();
	return Stats;
}

UWoTPayloadSoundWave::UWoTPayloadSoundWave(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UWoTPayloadSoundWave::PostLoad()
{
	Super::PostLoad();
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	if (!ApplyStructuralMetadata())
	{
		UE_LOG(LogWoTPayloadSound, Warning,
			TEXT("Payload sound %s has invalid stripped structural metadata"),
			*GetPathName());
		return;
	}
	UWoTPayloadSubsystem::TryBindSoundWaveDuringPostLoad(this);
}

bool UWoTPayloadSoundWave::ApplyStructuralMetadata()
{
	if (PayloadChannels < 1 || PayloadChannels > 8
		|| PayloadSampleRate < 1 || PayloadSampleRate > 384000
		|| (PayloadBitsPerSample != 8 && PayloadBitsPerSample != 16)
		|| PayloadFrameCount < 0)
	{
		return false;
	}
	NumChannels = PayloadChannels;
	SetSampleRate(static_cast<uint32>(PayloadSampleRate));
	SetNumFrames(static_cast<uint32>(PayloadFrameCount));
	Duration = static_cast<float>(PayloadFrameCount)
		/ static_cast<float>(PayloadSampleRate);
	TotalSamples = static_cast<float>(PayloadFrameCount);
	return true;
}

bool UWoTPayloadSoundWave::IsPayloadStructurallyCompatible(
	const FWoTSoundPayloadData& Payload) const
{
	return PayloadChannels == Payload.Channels
		&& PayloadSampleRate == static_cast<int32>(Payload.SampleRate)
		&& PayloadBitsPerSample == Payload.BitsPerSample
		&& PayloadFrameCount == static_cast<int32>(Payload.FrameCount);
}

bool UWoTPayloadSoundWave::PreparePayloadSnapshot(
	const FWoTSoundPayloadData& Payload)
{
	if (!IsPayloadStructurallyCompatible(Payload) || !ApplyStructuralMetadata())
	{
		UE_LOG(LogWoTPayloadSound, Warning,
			TEXT("Payload sound %s structural metadata disagrees with authenticated cache entry %s"),
			*GetPathName(), *PayloadKey);
		return false;
	}
	return true;
}

bool UWoTPayloadSoundWave::IsPayloadBound() const
{
	return UWoTPayloadSubsystem::ResolveSoundPayloadForGenerator(this).IsValid();
}

ISoundGeneratorPtr UWoTPayloadSoundWave::CreateSoundGenerator(
	const FSoundGeneratorInitParams& InParams)
{
	// Resolve once from the current subsystem's synchronized binding table. The
	// generator owns this immutable shared snapshot for the rest of its life and
	// never follows a subsystem or wave pointer on the render thread.
	const TSharedPtr<const FWoTSoundPayloadData> Payload =
		UWoTPayloadSubsystem::ResolveSoundPayloadForGenerator(this);
	if (!Payload.IsValid() || !IsPayloadStructurallyCompatible(*Payload))
	{
		++GWoTPayloadNullGenerators;
		return nullptr;
	}
	++GWoTPayloadGenerators;
	// bLooping is a bit-field; copy to a plain bool before passing by value/ref.
	const bool bLoopPlayback = bLooping != 0;
	return MakeShared<FWoTPayloadSoundGenerator, ESPMode::ThreadSafe>(
		Payload.ToSharedRef(), bLoopPlayback, InParams.StartTime);
}
