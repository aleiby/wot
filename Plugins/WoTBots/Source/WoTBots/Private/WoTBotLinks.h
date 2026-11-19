#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

// Off-mesh links for the bot navigation mesh along the map's own path network.
// The retail path network (the stamped NavigationPoint list and its reach
// specs) records where a player-sized pawn walks, drops, jumps or teleports.
// Where the bot mesh has no route between the two ends of such a reach (a drop
// off a ledge, a jump onto one, a teleporter), a one-way bot-only link supplies it.
namespace WoTBotLinks
{
// Highest rise a link may climb: the jump (325 up against 950 gravity) peaks
// at about 55 units.
constexpr float MaxRise = 50.0f;
// Deepest drop a link may fall: falling damage starts at an impact speed of
// 1100 (WOTPlayer.uc Landed), reached after about 637 units.
constexpr float MaxDrop = 500.0f;

// A teleporter's reach whose ends lie farther apart than the mesh's neighbouring
// tiles: Detour joins a point link's far end only to polygons in the link's own
// tile or the eight around it (DetourNavMesh.cpp connectExtOffMeshLinks), so no
// link carries it. Bots route through it themselves: walk into the entry, and
// the teleporter sets the body down at the exit. Both ends are on the bot mesh.
// The entry's pad (its teleporter) fires on a body reaching within TouchRadius
// of Pad, plus the body's own radius. A body it sets down (at its own exit) it
// sends on again only once that body's overlap with Teleporter has ended
// (WoTTeleporter.cpp NotifyActorEndOverlap re-arms it).
struct FDistantTeleport
{
	FVector Entry = FVector::ZeroVector;
	FVector Exit = FVector::ZeroVector;
	FVector Pad = FVector::ZeroVector;
	float TouchRadius = 0.0f;
	TWeakObjectPtr<const AActor> Teleporter;
};

// Links the bot mesh once it is built. Returns the number of links added, or
// INDEX_NONE while the bot mesh is absent or still building. Teleporter reaches
// too long for a link are added to Distant; other reaches that long are skipped.
int32 Build(UWorld& World, TArray<FDistantTeleport>* Distant = nullptr);
}
