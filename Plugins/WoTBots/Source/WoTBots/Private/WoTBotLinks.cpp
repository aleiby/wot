#include "WoTBotLinks.h"
#include "WoTBotNavigation.h"
#include "AI/WoTPathNode.h"
#include "WoTMover.h"
#include "WoTTeleporter.h"
#include "AI/NavigationModifier.h"
#include "EngineUtils.h"
#include "Navigation/NavLinkProxy.h"
#include "NavAreas/NavArea_Default.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTBotLinks, Log, All);

namespace WoTBotLinks
{
namespace
{
// Connected regions of the bot mesh: polygons joined through shared edges.
struct FIslands
{
	TMap<NavNodeRef, int32> Index;
	TArray<int32> Parent;
	int32 Find(int32 I)
	{
		while (Parent[I] != I) I = Parent[I] = Parent[Parent[I]];
		return I;
	}
	void Build(const ARecastNavMesh& Mesh)
	{
		TArray<FNavTileRef> Tiles;
		Mesh.GetAllNavMeshTiles(Tiles);
		TArray<NavNodeRef> Polys;
		for (const FNavTileRef Tile : Tiles)
		{
			TArray<FNavPoly> InTile;
			Mesh.GetPolysInTile(Tile, InTile);
			for (const FNavPoly& Poly : InTile) { Index.Add(Poly.Ref, Polys.Num()); Polys.Add(Poly.Ref); }
		}
		Parent.SetNum(Polys.Num());
		for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;
		for (int32 I = 0; I < Polys.Num(); ++I)
		{
			TArray<NavNodeRef> Neighbours;
			Mesh.GetPolyNeighbors(Polys[I], Neighbours);
			for (const NavNodeRef Neighbour : Neighbours)
				if (const int32* J = Index.Find(Neighbour)) Parent[Find(I)] = Find(*J);
		}
	}
	int32 Of(NavNodeRef Ref)
	{
		const int32* I = Index.Find(Ref);
		return I ? Find(*I) : INDEX_NONE;
	}
};
}

int32 Build(UWorld& World, TArray<FDistantTeleport>* Distant)
{
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);
	if (!Nav || Nav->IsNavigationBuildInProgress()) return INDEX_NONE;
	const ARecastNavMesh* Mesh = nullptr;
	for (const ANavigationData* Data : Nav->NavDataSet)
		if (const AWoTBotNavMesh* BotMesh = Cast<AWoTBotNavMesh>(Data)) Mesh = BotMesh;
	if (!Mesh) return INDEX_NONE;
	int32 Agent = INDEX_NONE;
	const TArray<FNavDataConfig>& Agents = Nav->GetSupportedAgents();
	for (int32 I = 0; I < Agents.Num(); ++I) if (Agents[I].Name == TEXT("ArenaBot")) Agent = I;
	if (Agent == INDEX_NONE) return 0;

	FIslands Islands;
	Islands.Build(*Mesh);
	if (Islands.Index.IsEmpty()) return INDEX_NONE;
	struct FEnd { FVector Location = FVector::ZeroVector; int32 Island = INDEX_NONE; };
	TMap<const AWoTPathNode*, FEnd> Ends;
	const auto EndOf = [&](const AWoTPathNode* Node) -> FEnd
	{
		if (const FEnd* Known = Ends.Find(Node)) return *Known;
		FEnd End;
		FNavLocation Projected;
		// A NavigationPoint stands about a body's half height above its floor.
		if (Nav->ProjectPointToNavigation(Node->GetActorLocation(), Projected, FVector(50, 50, 150), Mesh))
		{
			End.Location = Projected.Location;
			End.Island = Islands.Of(Projected.NodeRef);
		}
		return Ends.Add(Node, End);
	};
	// Doors and other movers: the reach was recorded through them open, and a
	// bot body does not open a door its player-proximity trigger is waiting for.
	TArray<FBox> Movers;
	for (TActorIterator<AWoTMover> It(&World); It; ++It) Movers.Add(It->GetComponentsBoundingBox(true));
	const auto ThroughMover = [&Movers](const FVector& A, const FVector& B)
	{
		const FVector Lift(0, 0, 40);
		for (const FBox& Box : Movers)
			if (FMath::LineBoxIntersection(Box, A + Lift, B + Lift, B - A)) return true;
		return false;
	};
	int32 Added = 0;
	for (TActorIterator<AWoTPathNode> It(&World); It; ++It)
	{
		const FEnd From = EndOf(*It);
		if (From.Island == INDEX_NONE) continue;
		for (const FWoTPathEdge& Edge : It->OutEdges)
		{
			if (!Edge.Other) continue;
			const FEnd To = EndOf(Edge.Other);
			if (To.Island == INDEX_NONE || To.Island == From.Island) continue;
			// A teleporter's reach to its destination (Teleporter URL) is walked
			// into; any other reach is walked off, or jumped up when it rises.
			const bool bTeleport = It->IsTeleporter() && Edge.Other->IsTeleporter();
			// On land the projected floors are what a body stands on. In water,
			// projection attaches an end to the submerged floor, below the height
			// the body swims at, so the authored reach's own rise also qualifies.
			// The projected ends still attach the link to the mesh.
			const auto WithinReach = [](float Rise) { return Rise <= MaxRise && Rise >= -MaxDrop; };
			const bool bReachable = WithinReach(To.Location.Z - From.Location.Z)
				|| WithinReach(Edge.Other->GetActorLocation().Z - It->GetActorLocation().Z);
			if (!bTeleport && (!bReachable || ThroughMover(From.Location, To.Location))) continue;
			// The mesh joins a link's far end only within the neighbouring tiles; a
			// longer link would never carry a route.
			int32 FromX, FromY, ToX, ToY;
			if (!Mesh->GetNavMeshTileXY(From.Location, FromX, FromY) || !Mesh->GetNavMeshTileXY(To.Location, ToX, ToY)
				|| FMath::Abs(FromX - ToX) > 1 || FMath::Abs(FromY - ToY) > 1)
			{
				if (bTeleport && Distant)
				{
					FDistantTeleport& Reach = Distant->AddDefaulted_GetRef();
					Reach.Entry = From.Location;
					Reach.Exit = To.Location;
					Reach.Pad = It->GetActorLocation();
					for (TActorIterator<AWoTTeleporter> Pad(&World); Pad; ++Pad)
						if (FVector::Dist2D(Pad->GetActorLocation(), It->GetActorLocation()) < 10.0f)
						{
							Reach.Pad = Pad->GetActorLocation();
							Reach.TouchRadius = Pad->UE1CollisionRadius;
							Reach.Teleporter = *Pad;
						}
					UE_LOG(LogWoTBotLinks, Log, TEXT("Bot teleporter reach %s -> %s (%.0f apart) is routed without a mesh link."),
						*It->GetName(), *Edge.Other->GetName(), FVector::Dist2D(From.Location, To.Location));
				}
				else
				{
					UE_LOG(LogWoTBotLinks, Warning, TEXT("Bot link %s -> %s skipped: its ends are %.0f apart, beyond the neighbouring mesh tile."),
						*It->GetName(), *Edge.Other->GetName(), FVector::Dist2D(From.Location, To.Location));
				}
				continue;
			}
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Params.bDeferConstruction = true;
			ANavLinkProxy* Proxy = World.SpawnActor<ANavLinkProxy>(ANavLinkProxy::StaticClass(), FTransform(From.Location), Params);
			if (!Proxy) continue;
			Proxy->SetReplicates(false);
			Proxy->PointLinks.Reset();
			FNavigationLink Link(FVector::ZeroVector, To.Location - From.Location);
			Link.Direction = ENavLinkDirection::LeftToRight;
			Link.SetAreaClass(UNavArea_Default::StaticClass());
			Link.SupportedAgents = FNavAgentSelector(0);
			Link.SupportedAgents.Set(Agent);
			Link.SupportedAgents.MarkInitialized();
			Proxy->PointLinks.Add(Link);
			Proxy->FinishSpawning(FTransform(From.Location));
			++Added;
		}
	}
	return Added;
}
}
