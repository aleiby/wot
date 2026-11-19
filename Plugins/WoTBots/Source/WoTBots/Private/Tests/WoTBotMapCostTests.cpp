#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "Payload/WoTRuntimeRegistry.h"
#include "Payload/WoTRuntimeWorldResource.h"
#include "World/WoTWorldMaterializer.h"
#include "World/WoTWorldProjector.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeBSPRenderer.h"
#include "AI/NavigationSystemConfig.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
namespace WoTBotTest
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotMapCostSpec, "WoT.Bots.Measurements.LargestArenaNavigationBuild", Flags)
bool FBotMapCostSpec::RunTest(const FString&)
{
	FMapSet Maps; FString LoadError;
	if (!Maps.Load(LoadError)) { AddError(LoadError); return false; }
	auto& Writer = Maps.Writer;
	auto& Defaults = Maps.Defaults;
	TArray<FWoTBSPPayloadData> LargestParts;
	const FWoTWorldDescriptor* Largest = nullptr;
	double LargestArea = -1;
	for (const FWoTWorldDescriptor& Descriptor : Maps.Descriptors)
	{
		if (!Descriptor.MapIdentity.StartsWith(TEXT("Arena_"))) continue;
		FBox Bounds(ForceInit); TArray<FWoTBSPPayloadData> Parts; int32 Triangles = 0;
		for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
		{
			auto Object = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Writer.Objects.FindChecked(Part.LogicalId),
				TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
			if (Object.IsError()) { AddError(Object.GetError().ToString()); return false; }
			FString Error;
			FWoTBSPPayloadData& Payload = Parts.AddDefaulted_GetRef();
			if (!UWoTBSPPayloadLibrary::DecodePayload(Object.GetValue().Payload(), Payload, Error)) { AddError(Error); return false; }
			if (!Payload.bPortal)
			{
				Bounds += FVector(Payload.BoundsMin); Bounds += FVector(Payload.BoundsMax);
				Triangles += Payload.Indices.Num() / 3;
			}
		}
		const double Area = Bounds.GetSize().X * Bounds.GetSize().Y;
		AddInfo(FString::Printf(TEXT("ARENA_SIZE map=%s xy_area=%.0f triangles=%d bounds=%s"),
			*Descriptor.MapIdentity, Area, Triangles, *Bounds.GetSize().ToString()));
		if (Area > LargestArea) { LargestArea = Area; Largest = &Descriptor; LargestParts = MoveTemp(Parts); }
	}
	if (!TestNotNull(TEXT("largest Arena found from all map bounds"), Largest)) return false;
	auto WorldObject = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Writer.Objects.FindChecked(Largest->WorldLogicalId),
		TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (WorldObject.IsError()) { AddError(WorldObject.GetError().ToString()); return false; }
	FWoTRuntimeWorldResource Resource;
	const auto Loaded = Resource.LoadObjectForTesting(MoveTemp(WorldObject).GetValue());
	if (Loaded.IsError()) { AddError(Loaded.GetError().ToString()); return false; }
	TArray<uint32> SurfaceFlags;
	for (const auto& Surface : Resource.Surfaces()) SurfaceFlags.Add(Surface.Flags);
	FWoTRuntimeBSPCollisionGeometryView Collision;
	Collision.Vectors = Resource.Vectors(); Collision.Points = Resource.Points();
	Collision.Nodes = Resource.Nodes(); Collision.Surfaces = Resource.Surfaces(); Collision.Vertices = Resource.Vertices();
	FWoTRuntimeInvisibleBSPCollisionData InvisibleCollision; FString CollisionError;
	if (!WoTWorldBuildInvisibleSolidBSPCollision(Collision, InvisibleCollision, CollisionError))
	{ AddError(CollisionError); return false; }
	for (int32 Sample = 0; Sample < 6; ++Sample)
	{
		const bool bBots = Sample % 2 != 0;
		WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false;
		UWorld* W = WoTTest::CreateSpecWorld(Options);
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
		FString Error; AWoTRuntimeBSPWorld* Geometry = nullptr; uint32 Fallbacks = 0;
		if (!WoTWorldBuildRuntimeBSPForTesting(*W, *Largest, LargestParts, Geometry, Fallbacks, Error,
			{}, nullptr, nullptr, nullptr, nullptr, EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags, &InvisibleCollision))
		{ AddError(Error); return false; }
		Geometry->FinishSpawning(FTransform::Identity, true);
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = Largest->MapIdentity; Source.Records = Resource.Actors();
		Source.Points = Resource.Points(); Source.Nodes = Resource.Nodes(); Source.Vertices = Resource.Vertices();
		Source.Zones = Resource.Zones(); Source.Surfaces = Resource.Surfaces(); Source.Leaves = Resource.Leaves();
		Source.ModelLights = Resource.ModelLights(); Source.MoverModels = Resource.MoverModels();
		Source.SemanticObjects = Resource.SemanticObjects(); Source.ReachSpecs = Resource.ReachSpecs();
		Source.bBSPRootOutside = Resource.BSPRootOutside();
		FWoTWorldProjectionResult Projected;
		if (!FWoTWorldProjector::ProjectDeferred(*W, Source, Defaults, Projected, Error)) { AddError(Error); return false; }
		for (AActor* Actor : Projected.DeferredActors) if (IsValid(Actor)) Actor->FinishSpawning(Actor->GetActorTransform());
		UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
		Config->NavigationSystemClass = FSoftClassPath(bBots ? UWoTBotNavigationSystem::StaticClass() : UNavigationSystemV1::StaticClass());
		W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
		const double Begin = FPlatformTime::Seconds();
		FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
		if (!TestNotNull(TEXT("map navigation initialized"), Nav)) return false;
		Nav->Build();
		for (ANavigationData* Data : Nav->NavDataSet) if (Data) Data->EnsureBuildCompletion();
		const double Milliseconds = (FPlatformTime::Seconds() - Begin) * 1000;
		int32 Tiles = 0, PopulatedTiles = 0;
		for (ANavigationData* Data : Nav->NavDataSet)
			if (const ARecastNavMesh* Mesh = Cast<ARecastNavMesh>(Data))
			{
				Tiles += Mesh->GetNavMeshTilesCount();
				TArray<FNavTileRef> TileRefs; Mesh->GetAllNavMeshTiles(TileRefs);
				for (FNavTileRef Tile : TileRefs)
				{
					TArray<FNavPoly> Polys; Mesh->GetPolysInTile(Tile, Polys);
					if (!Polys.IsEmpty()) ++PopulatedTiles;
				}
			}
		TestTrue(TEXT("real map produced navigation polygons"), PopulatedTiles > 0);
		AddInfo(FString::Printf(TEXT("NAV_BUILD map=%s sample=%d agents=%d build_ms=%.3f tile_slots=%d populated_tiles=%d"),
			*Largest->MapIdentity, Sample / 2, bBots ? 2 : 1, Milliseconds, Tiles, PopulatedTiles));
	}
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
