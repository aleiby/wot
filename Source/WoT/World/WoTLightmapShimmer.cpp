#include "WoTLightmapShimmer.h"

namespace
{
void RefreshDeltaRingSlice(float* RandomDeltas, const float* RandomBases,
	const float* Randoms, const int32 FirstIndex, const uint32 EntryCount,
	const float ValuesPerCycle)
{
	for (uint32 Offset = 0; Offset < EntryCount; ++Offset)
	{
		const int32 Index = (FirstIndex + static_cast<int32>(Offset))
			& FWoTLightmapShimmerRandoms::RAND_MASK;
		RandomDeltas[Index] = (RandomBases[Index] - Randoms[Index]) / ValuesPerCycle;
	}
}
}

FWoTLightmapShimmerRandoms::FWoTLightmapShimmerRandoms()
{
	Reset(WoTLightmapShimmerSeed);
}

void FWoTLightmapShimmerRandoms::Reset(const int32 Seed)
{
	Stream.Initialize(Seed);
	for (int32 Index = 0; Index < N_RANDS; ++Index)
	{
		RandomBases[Index] = 0.0f;
		Randoms[Index] = 0.0f;
		RandomDeltas[Index] = 0.0f;
	}
	LastTicks = 0;
}

void FWoTLightmapShimmerRandoms::Tick(const float TimeSeconds)
{
	// Match the unsigned tick-counter behavior of Render/Src/UnRandom.cpp.
	const uint32 ServerTicks = ServerTicksFor(TimeSeconds);
	const float ValuesPerCycle = static_cast<float>(N_RANDS / RAND_CYCLE);

	// Every rendered frame receives a new target value for every table slot.
	for (int32 Index = 0; Index < N_RANDS; ++Index)
	{
		RandomBases[Index] = Stream.GetFraction();
	}

	const uint32 RetargetCount = (ServerTicks - LastTicks)
		* static_cast<uint32>(RAND_CYCLE);
	if (LastTicks != 0 && RetargetCount < static_cast<uint32>(N_RANDS))
	{
		// A short tick advance replaces only the newly entered ring slice.
		const int32 RetargetStart = static_cast<int32>(
			(LastTicks * static_cast<uint32>(RAND_CYCLE)) & RAND_MASK);
		RefreshDeltaRingSlice(RandomDeltas, RandomBases, Randoms,
			RetargetStart, RetargetCount, ValuesPerCycle);

		// Walk the complementary slice with an explicit count, including all 256
		// entries when no server tick elapsed.
		const int32 DriftStart = static_cast<int32>(
			(ServerTicks * static_cast<uint32>(RAND_CYCLE)) & RAND_MASK);
		const uint32 DriftCount = static_cast<uint32>(N_RANDS) - RetargetCount;
		const float DriftScale = static_cast<float>((ServerTicks - LastTicks) & RAND_MASK);
		for (uint32 Offset = 0; Offset < DriftCount; ++Offset)
		{
			const int32 Index = (DriftStart + static_cast<int32>(Offset)) & RAND_MASK;
			Randoms[Index] += RandomDeltas[Index] * DriftScale;
		}
	}
	else
	{
		// The first update, or a gap spanning a full ring, starts every slot anew.
		for (int32 Index = 0; Index < N_RANDS; ++Index)
		{
			Randoms[Index] = Stream.GetFraction();
			RandomDeltas[Index] = (RandomBases[Index] - Randoms[Index])
				/ ValuesPerCycle;
		}
	}

	// Keep interpolation drift inside the table's documented unit interval.
	for (int32 Index = 0; Index < N_RANDS; ++Index)
	{
		Randoms[Index] = FMath::Clamp(Randoms[Index], 0.0f, 1.0f);
	}

	// The next frame measures its unsigned advance from this sample.
	LastTicks = ServerTicks;
}
