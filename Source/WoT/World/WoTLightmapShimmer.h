#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

/**
 * The retail renderer's shimmer randoms (FGlobalRandoms over
 * FGlobalRandomsBase). Two 256-entry tables
 * of [0,1] floats: RandomBases are regenerated on every tick (temporally
 * discontinuous; LE_TorchWaver and LE_FireWaver both read them), and Randoms
 * move toward the bases in 35 Hz server ticks (temporally continuous;
 * LE_WateryShimmer reads them). FLightManager::Merge indexes both by the
 * surface-local light-map key, incremented per texel.
 *
 * appFrand is replaced by a seeded FRandomStream so a spec can replay the
 * exact sequence; the seed is WoTLightmapShimmerSeed.
 */
struct WOT_API FWoTLightmapShimmerRandoms
{
	static constexpr int32 N_RANDS = 256;
	static constexpr int32 RAND_CYCLE = 16;
	static constexpr int32 RAND_MASK = N_RANDS - 1;

	FWoTLightmapShimmerRandoms();

	/** Re-seed and clear the tick history (the "first time" branch runs on the next Tick). */
	void Reset(int32 Seed);
	/** FGlobalRandoms::Tick, called once per rendered frame with the world clock. */
	void Tick(float TimeSeconds);

	float RandomBase(const int32 Key) const { return RandomBases[Key & RAND_MASK]; }
	float Random(const int32 Key) const { return Randoms[Key & RAND_MASK]; }
	uint32 GetLastTicks() const { return LastTicks; }
	static uint32 ServerTicksFor(const float TimeSeconds)
	{
		return static_cast<uint32>(static_cast<double>(TimeSeconds) * 35.0);
	}

	float RandomBases[N_RANDS];
	float Randoms[N_RANDS];
	float RandomDeltas[N_RANDS];

private:
	uint32 LastTicks = 0;
	FRandomStream Stream;
};

/** The documented seed of the runtime shimmer tables. */
constexpr int32 WoTLightmapShimmerSeed = 0x57544C57;
