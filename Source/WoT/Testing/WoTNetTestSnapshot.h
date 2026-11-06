#pragma once

// Net-test snapshot serializer (net-test harness). Test-only: the
// whole file compiles out of Shipping. Produces one JSON object describing the
// net-relevant world state, consumed by the TCP command channel and the
// automation specs (WoTNetTests.cpp).

#include "CoreMinimal.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

class UWorld;
class FJsonObject;

namespace WoTNetTest
{
	// World -> structured snapshot (netmode/map/time, pawns, projectiles,
	// pickups, gamestate, players). IsValid-guarded; positions rounded to 0.1.
	TSharedPtr<FJsonObject> BuildSnapshot(UWorld* World);

	// The snapshot as a single condensed line (JSONL-safe: no embedded newline).
	FString BuildSnapshotJson(UWorld* World);
}

#endif
