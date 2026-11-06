#include "WoTFrontEndLightning.h"

bool FWoTEntryStrikeState::Initialize(const FWoTEntryStrikeSettings& InSettings, FString& Error)
{
	if (!FMath::IsFinite(InSettings.MinInterval) || InSettings.MinInterval < .05f
		|| !FMath::IsFinite(InSettings.MaxInterval) || InSettings.MaxInterval < InSettings.MinInterval
		|| !FMath::IsFinite(InSettings.MinThunderDelay) || InSettings.MinThunderDelay < 0
		|| !FMath::IsFinite(InSettings.MaxThunderDelay) || InSettings.MaxThunderDelay < InSettings.MinThunderDelay
		|| InSettings.LightningLengths.Num() > 8)
	{
		Error = TEXT("Invalid entry strike settings");
		return false;
	}
	for (float Length : InSettings.LightningLengths)
	{
		if (!FMath::IsFinite(Length) || Length < 0)
		{
			Error = TEXT("Invalid entry lightning length");
			return false;
		}
	}
	Reset();
	Settings = InSettings;
	bInitialized = true;
	Error.Reset();
	return true;
}

bool FWoTEntryStrikeState::Advance(float DeltaSeconds, TFunctionRef<float()> Random01,
	FWoTEntryStrikeFrame& Out, FString& Error)
{
	if (!bInitialized || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0)
	{
		Error = TEXT("Uninitialized strike state or invalid delta");
		return false;
	}
	// Transactional state/output; an injected stream itself cannot be rewound.
	FWoTEntryStrikeState Next = *this;
	FWoTEntryStrikeFrame Frame;
	Frame.Samples.Init(0.f, Settings.LightningLengths.Num());
	auto Draw = [&](float& Value)
	{
		Value = Random01();
		if (FMath::IsFinite(Value) && Value >= 0.f && Value <= 1.f) return true;
		Error = TEXT("Entry lightning random sample outside [0,1]");
		return false;
	};
	auto CalcNextTimes = [&](double Origin)
	{
		float IntervalR, DelayR, HalfLightR;
		if (!Draw(IntervalR) || !Draw(DelayR) || !Draw(HalfLightR)) return false;
		Next.NextStrike = Origin + FMath::Lerp(Settings.MinInterval, Settings.MaxInterval, IntervalR);
		Next.CurrentThunderDelay = FMath::Lerp(Settings.MinThunderDelay, Settings.MaxThunderDelay, DelayR);
		return true;
	};
	if (!Next.bScheduled)
	{
		if (!CalcNextTimes(0)) return false;
		Next.bScheduled = true;
	}
	Next.Time += DeltaSeconds;
	// One script Tick: a hitch triggers once at the current time. CalcNextTimes
	// precedes Trigger; its NEW delay belongs to this strike. HalfLightPct still
	// consumes the third draw although tagged AltStage1 does not use it.
	if (Next.Time >= Next.NextStrike)
	{
		// Reset the internal epoch at a trigger. Absolute elapsed float-max time
		// would otherwise swallow short intervals/lengths even in a double.
		Next.Time = 0;
		if (!CalcNextTimes(0)) return false;
		Next.StrikeTime = Next.Time;
		Next.bHasStrike = true;
		Next.bThunderPending = !Settings.LightningLengths.IsEmpty();
		Frame.bStrike = true;
	}
	// Authored RandomLightning ticks first: retrigger replaces pending thunder.
	Frame.ThunderDelay = Next.CurrentThunderDelay;
	if (Next.bThunderPending && Next.Time >= Next.StrikeTime + Next.CurrentThunderDelay)
	{
		Frame.bThunder = true;
		Next.bThunderPending = false;
	}
	for (int32 Index = 0; Index < Frame.Samples.Num(); ++Index)
	{
		// Explicit plan contract uses elapsed deadlines, not the old engine's
		// clamped actor delta / half-frame latent Sleep wake-up heuristic.
		if (Next.bHasStrike && Next.Time < Next.StrikeTime + Settings.LightningLengths[Index])
		{
			float R;
			if (!Draw(R)) return false;
			Frame.Samples[Index] = R < .5f ? 0.f : R;
		}
	}
	*this = MoveTemp(Next);
	Out = MoveTemp(Frame);
	Error.Reset();
	return true;
}

void FWoTEntryStrikeState::Reset()
{
	bInitialized = false;
	bScheduled = bHasStrike = bThunderPending = false;
	Time = NextStrike = StrikeTime = 0;
	CurrentThunderDelay = 0;
	Settings = {};
}

namespace
{
struct FWoTEntryFixedColor
{
	uint32 R = 0;
	uint32 G = 0;
	uint32 B = 0;
};

float WoTEntryHSVValue()
{
	float Value = static_cast<float>(255.0 * 1.4 / 255.0);
	Value = static_cast<float>(static_cast<double>(Value)
		* (0.70 / (0.01 + FMath::Sqrt(static_cast<double>(Value)))));
	return FMath::Clamp(Value, 0.0f, 1.0f);
}

FVector3f WoTEntryHSV(uint8 HueByte, uint8 SaturationByte)
{
	float HueR = 0.0f;
	float HueG = 0.0f;
	float HueB = 0.0f;
	if (HueByte < 86)
	{
		HueR = static_cast<float>(85 - HueByte) / 85.0f;
		HueG = static_cast<float>(HueByte) / 85.0f;
	}
	else if (HueByte < 171)
	{
		HueG = static_cast<float>(170 - HueByte) / 85.0f;
		HueB = static_cast<float>(HueByte - 85) / 85.0f;
	}
	else
	{
		HueR = static_cast<float>(HueByte - 170) / 85.0f;
		HueB = static_cast<float>(255 - HueByte) / 84.0f;
	}

	const float Saturation = static_cast<float>(
		static_cast<double>(SaturationByte) / 255.0);
	float R = Saturation * (1.0f - HueR);
	float G = Saturation * (1.0f - HueG);
	float B = Saturation * (1.0f - HueB);
	R = HueR + R;
	G = HueG + G;
	B = HueB + B;
	const float HSVValue = WoTEntryHSVValue();
	R *= HSVValue;
	G *= HSVValue;
	B *= HSVValue;
	return FVector3f(R, G, B);
}

FWoTEntryFixedColor WoTEntryDynamicFixedColor(
	const FWoTEntryLightRecord& Light, float Sample)
{
	float ModulatedBrightness = static_cast<float>(Light.Brightness) / 255.0f;
	ModulatedBrightness *= Sample;
	ModulatedBrightness = FMath::Clamp(ModulatedBrightness, 0.0f, 1.0f);
	const float Scale = ModulatedBrightness * Light.LevelBrightness;
	const FVector3f HSV = WoTEntryHSV(Light.Hue, Light.Saturation);
	const float R = HSV.X * Scale;
	const float G = HSV.Y * Scale;
	const float B = HSV.Z * Scale;
	return {
		static_cast<uint32>(FMath::FloorToInt64(static_cast<double>(R) * 65536.0)),
		static_cast<uint32>(FMath::FloorToInt64(static_cast<double>(G) * 65536.0)),
		static_cast<uint32>(FMath::FloorToInt64(static_cast<double>(B) * 65536.0)),
	};
}

bool WoTEntryFail(FString& Error, FString Message)
{
	Error = MoveTemp(Message);
	return false;
}

int32 WoTEntryPixelOffset(uint32 AtlasWidth, uint32 X, uint32 Y)
{
	return static_cast<int32>((uint64(Y) * AtlasWidth + X) * 4);
}
}

uint8 WoTEntryMergeIllumination(
	uint8 BaseRGBA7, uint8 Illumination, uint32 FixedColor)
{
	return uint8(FMath::Min<uint64>(127, uint64(BaseRGBA7)
		+ ((uint64(Illumination) * FixedColor) >> 16)));
}

bool WoTComposeEntryLightning(
	const FWoTBSPPayloadData& Base,
	TConstArrayView<float> Samples,
	TArray<uint8>& OutBGRA,
	FString& Error)
{
	const int32 LightCount = Base.EntryLights.Num();
	if (Samples.Num() != LightCount)
	{
		return WoTEntryFail(Error, FString::Printf(
			TEXT("Entry lightning sample count %d does not match light count %d."),
			Samples.Num(), LightCount));
	}

	bool bHasActiveLight = false;
	for (int32 LightIndex = 0; LightIndex < LightCount; ++LightIndex)
	{
		const float Sample = Samples[LightIndex];
		if (!FMath::IsFinite(Sample))
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning sample %d is not finite."), LightIndex));
		}
		if (Sample < 0.0f || Sample > 1.0f)
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning sample %d is outside [0,1]."), LightIndex));
		}
		const float LevelBrightness = Base.EntryLights[LightIndex].LevelBrightness;
		if (!FMath::IsFinite(LevelBrightness)
			|| LevelBrightness < 0.0f || LevelBrightness > 1024.0f)
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning light %d has invalid level brightness."),
				LightIndex));
		}
		bHasActiveLight = bHasActiveLight || Sample > 0.0f;
	}

	const uint64 PixelCount = uint64(Base.LightmapWidth) * Base.LightmapHeight;
	if (Base.LightmapWidth > 8192 || Base.LightmapHeight > 8192
		|| PixelCount == 0 || PixelCount > 4ull * 1024ull * 1024ull)
	{
		return WoTEntryFail(Error,
			TEXT("Entry lightning base atlas dimensions do not match its bytes."));
	}
	const uint64 AtlasBytes = PixelCount * 4;
	if (AtlasBytes != static_cast<uint64>(Base.LightmapBGRA.Num()))
	{
		return WoTEntryFail(Error,
			TEXT("Entry lightning base atlas dimensions do not match its bytes."));
	}

	TArray<uint8> ActiveSurfaces;
	ActiveSurfaces.Init(0, Base.EntrySurfaces.Num());
	TArray<uint8> PairedSurfaces;
	PairedSurfaces.Init(0, Base.EntrySurfaces.Num());
	TSet<uint64> PairKeys;
	for (int32 PairIndex = 0; PairIndex < Base.EntryIllumination.Num(); ++PairIndex)
	{
		const FWoTEntryIlluminationRecord& Pair = Base.EntryIllumination[PairIndex];
		if (Pair.LightIndex >= static_cast<uint32>(LightCount)
			|| Pair.SurfaceIndex >= static_cast<uint32>(Base.EntrySurfaces.Num()))
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning pair %d references an absent record."), PairIndex));
		}
		const uint64 PairKey = (uint64(Pair.LightIndex) << 32) | Pair.SurfaceIndex;
		if (PairKeys.Contains(PairKey))
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning pair %d duplicates a light/surface row."),
				PairIndex));
		}
		PairKeys.Add(PairKey);

		const FWoTEntrySurfaceRecord& Surface =
			Base.EntrySurfaces[static_cast<int32>(Pair.SurfaceIndex)];
		const uint64 SurfacePixels = uint64(Surface.W) * Surface.H;
		if (Surface.SurfaceIndex != Pair.SurfaceIndex
			|| Surface.W == 0 || Surface.H == 0
			|| SurfacePixels != static_cast<uint64>(Pair.Illumination.Num()))
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning pair %d disagrees with its surface rectangle."),
				PairIndex));
		}
		if (Surface.X == 0 || Surface.Y == 0
			|| uint64(Surface.X) + Surface.W >= Base.LightmapWidth
			|| uint64(Surface.Y) + Surface.H >= Base.LightmapHeight)
		{
			return WoTEntryFail(Error, FString::Printf(
				TEXT("Entry lightning surface %u has no in-bounds one-pixel gutter."),
				Pair.SurfaceIndex));
		}
		PairedSurfaces[static_cast<int32>(Pair.SurfaceIndex)] = 1;
		if (Samples[static_cast<int32>(Pair.LightIndex)] > 0.0f)
		{
			ActiveSurfaces[static_cast<int32>(Pair.SurfaceIndex)] = 1;
		}
	}

	for (int32 SurfaceIndex = 0; SurfaceIndex < PairedSurfaces.Num(); ++SurfaceIndex)
	{
		if (PairedSurfaces[SurfaceIndex] == 0) continue;
		const FWoTEntrySurfaceRecord& Surface = Base.EntrySurfaces[SurfaceIndex];
		for (uint32 Y = Surface.Y - 1; Y <= Surface.Y + Surface.H; ++Y)
		{
			for (uint32 X = Surface.X - 1; X <= Surface.X + Surface.W; ++X)
			{
				const int32 Offset = WoTEntryPixelOffset(Base.LightmapWidth, X, Y);
				for (uint32 Channel = 0; Channel < 3; ++Channel)
				{
					const uint8 Value = Base.LightmapBGRA[Offset + Channel];
					if (Value > 254 || (Value & 1) != 0)
					{
						return WoTEntryFail(Error, FString::Printf(
							TEXT("Entry lightning surface %d has a non-RGBA7 base channel."),
							SurfaceIndex));
					}
				}
			}
		}
	}

	if (!bHasActiveLight)
	{
		TArray<uint8> Reset = Base.LightmapBGRA;
		OutBGRA = MoveTemp(Reset);
		Error.Reset();
		return true;
	}

	TArray<uint8> Composed = Base.LightmapBGRA;
	for (int32 LightIndex = 0; LightIndex < LightCount; ++LightIndex)
	{
		const float Sample = Samples[LightIndex];
		if (Sample <= 0.0f) continue;
		const FWoTEntryFixedColor Fixed = WoTEntryDynamicFixedColor(
			Base.EntryLights[LightIndex], Sample);
		const uint32 FixedBGRA[3] = {Fixed.B, Fixed.G, Fixed.R};
		for (const FWoTEntryIlluminationRecord& Pair : Base.EntryIllumination)
		{
			if (Pair.LightIndex != static_cast<uint32>(LightIndex)) continue;
			const FWoTEntrySurfaceRecord& Surface =
				Base.EntrySurfaces[static_cast<int32>(Pair.SurfaceIndex)];
			for (uint32 LocalY = 0; LocalY < Surface.H; ++LocalY)
			{
				for (uint32 LocalX = 0; LocalX < Surface.W; ++LocalX)
				{
					const uint32 IlluminationIndex = LocalY * Surface.W + LocalX;
					const uint8 Illumination =
						Pair.Illumination[static_cast<int32>(IlluminationIndex)];
					const int32 Offset = WoTEntryPixelOffset(Base.LightmapWidth,
						Surface.X + LocalX, Surface.Y + LocalY);
					for (uint32 Channel = 0; Channel < 3; ++Channel)
					{
						const uint8 BaseRGBA7 = Composed[Offset + Channel] / 2;
						Composed[Offset + Channel] = 2 * WoTEntryMergeIllumination(
							BaseRGBA7, Illumination, FixedBGRA[Channel]);
					}
					Composed[Offset + 3] = 255;
				}
			}
		}
	}

	for (int32 SurfaceIndex = 0; SurfaceIndex < ActiveSurfaces.Num(); ++SurfaceIndex)
	{
		if (ActiveSurfaces[SurfaceIndex] == 0) continue;
		const FWoTEntrySurfaceRecord& Surface = Base.EntrySurfaces[SurfaceIndex];
		for (uint32 Y = Surface.Y; Y < Surface.Y + Surface.H; ++Y)
		{
			const int32 Left = WoTEntryPixelOffset(
				Base.LightmapWidth, Surface.X - 1, Y);
			const int32 LeftSource = WoTEntryPixelOffset(
				Base.LightmapWidth, Surface.X, Y);
			const int32 Right = WoTEntryPixelOffset(
				Base.LightmapWidth, Surface.X + Surface.W, Y);
			const int32 RightSource = WoTEntryPixelOffset(
				Base.LightmapWidth, Surface.X + Surface.W - 1, Y);
			FMemory::Memcpy(Composed.GetData() + Left,
				Composed.GetData() + LeftSource, 4);
			FMemory::Memcpy(Composed.GetData() + Right,
				Composed.GetData() + RightSource, 4);
		}
		for (uint32 X = Surface.X - 1; X <= Surface.X + Surface.W; ++X)
		{
			const int32 Top = WoTEntryPixelOffset(
				Base.LightmapWidth, X, Surface.Y - 1);
			const int32 TopSource = WoTEntryPixelOffset(
				Base.LightmapWidth, X, Surface.Y);
			const int32 Bottom = WoTEntryPixelOffset(
				Base.LightmapWidth, X, Surface.Y + Surface.H);
			const int32 BottomSource = WoTEntryPixelOffset(
				Base.LightmapWidth, X, Surface.Y + Surface.H - 1);
			FMemory::Memcpy(Composed.GetData() + Top,
				Composed.GetData() + TopSource, 4);
			FMemory::Memcpy(Composed.GetData() + Bottom,
				Composed.GetData() + BottomSource, 4);
		}
	}

	OutBGRA = MoveTemp(Composed);
	Error.Reset();
	return true;
}
