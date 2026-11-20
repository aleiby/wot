#pragma once
#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotBehaviourFixture.h"
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
struct FBotMapWriter : IWoTObjectWriter
{
	TMap<FString, TArray<uint8>> Objects;
	FWoTBootstrapStatus WriteObject(FStringView Id, FStringView, TConstArrayView<uint8> Bytes,
		const FWoTSha256&, uint64) override
	{
		Objects.Add(FString(Id), TArray<uint8>(Bytes));
		return FWoTBootstrapStatus::Success();
	}
};
struct FBotMapProgress : IWoTProgressSink
{
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override { return FWoTBootstrapStatus::Success(); }
	bool IsCancellationRequested() const override { return false; }
};

struct FMapSet
{
 FBotMapWriter Writer;
 FWoTRuntimeClassDefaults Defaults;
 TArray<FWoTWorldDescriptor> Descriptors;
 bool Load(FString& Error)
 {
	const auto Contract = WoTLoadProductionRetailContract();
	if (Contract.IsError()) { Error = (Contract.GetError().ToString()); return false; }
	FWoTRetailContract Worlds;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : Contract.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::BSP && Fact.Domain != EWoTFactDomain::World)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase)) continue;
		Worlds.Facts.Add(Fact);
		if (Fact.Domain == EWoTFactDomain::World)
		{
			FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!FFileHelper::LoadFileToArray(Source.Bytes, *WoTRepositoryOrReferencePath(FString(TEXT("original/")) + Fact.SourcePath)))
			{ Error = FString::Printf(TEXT("Shared world-set prerequisite unavailable: original/%s"), *Fact.SourcePath); return false; }
		}
	}
	FBotMapProgress Progress;
	auto Imported = ImportRequiredWorldSetPackageBytesForTesting(Sources, Worlds, Writer, Progress);
	if (Imported.IsError()) { Error = (Imported.GetError().ToString()); return false; }
	FWoTRetailContract DefaultsContract;
	TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
	TArray<FString> DefaultIds;
	for (const FWoTRequiredFact& Fact : Contract.GetValue().Facts)
	{
		if (Fact.Domain != EWoTFactDomain::ClassDefault) continue;
		DefaultsContract.Facts.Add(Fact);
		FWoTRemainingSourceBytesForTesting& Source = DefaultSources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!FFileHelper::LoadFileToArray(Source.Bytes, *WoTRepositoryOrReferencePath(FString(TEXT("original/")) + Fact.SourcePath)))
		{ Error = FString::Printf(TEXT("Shared class-default prerequisite unavailable: original/%s"), *Fact.SourcePath); return false; }
		for (const auto& Output : Fact.Outputs) DefaultIds.Add(Output.LogicalId);
	}
	auto DefaultImport = ImportRegistryLocalizationDefaultsSourcesForTesting(DefaultSources, DefaultsContract, Writer, Progress);
	if (DefaultImport.IsError()) { Error = (DefaultImport.GetError().ToString()); return false; }
	Descriptors = Imported.GetValue().Worlds;
	TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
	for (const FString& Id : DefaultIds)
	{
		auto Object = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Writer.Objects.FindChecked(Id),
			TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
		if (Object.IsError()) { Error = (Object.GetError().ToString()); return false; }
		DefaultObjects.Add(MoveTemp(Object).GetValue());
	}
	FString DefaultsError;
	if (!Defaults.LoadObjectsForTesting(MoveTemp(DefaultObjects), DefaultsError)) { Error = (DefaultsError); return false; }

 return true;
 }
};
// Shared materialization path: collision BSP plus projected actors, zones, movers,
// spawn pools and navigation bounds from the authenticated world records.
inline bool Materialize(UWorld* W, FMapSet& Maps, const FWoTWorldDescriptor& Descriptor, FString& Error,
 UWoTMapLightTable** OutTable = nullptr)
{
 TArray<FWoTBSPPayloadData> Parts;
 for (const auto& Part : Descriptor.BSPParts)
 {
  auto Decoded = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Maps.Writer.Objects.FindChecked(Part.LogicalId),
   TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
  if (Decoded.IsError()) { Error = Decoded.GetError().ToString(); return false; }
  if (!UWoTBSPPayloadLibrary::DecodePayload(Decoded.GetValue().Payload(), Parts.AddDefaulted_GetRef(), Error)) return false;
 }
 auto WorldObject = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Maps.Writer.Objects.FindChecked(Descriptor.WorldLogicalId),
  TEXT("ue1-world-records-le-v6"), TEXT("world"));
 if (WorldObject.IsError()) { Error = WorldObject.GetError().ToString(); return false; }
 FWoTRuntimeWorldResource Resource;
 const auto Loaded = Resource.LoadObjectForTesting(MoveTemp(WorldObject).GetValue());
 if (Loaded.IsError()) { Error = Loaded.GetError().ToString(); return false; }
 TArray<uint32> SurfaceFlags;
 for (const auto& Surface : Resource.Surfaces()) SurfaceFlags.Add(Surface.Flags);
 FWoTRuntimeBSPCollisionGeometryView Collision;
 Collision.Vectors = Resource.Vectors(); Collision.Points = Resource.Points();
 Collision.Nodes = Resource.Nodes(); Collision.Surfaces = Resource.Surfaces(); Collision.Vertices = Resource.Vertices();
 FWoTRuntimeInvisibleBSPCollisionData InvisibleCollision;
 if (!WoTWorldBuildInvisibleSolidBSPCollision(Collision, InvisibleCollision, Error)) return false;
 AWoTRuntimeBSPWorld* Geometry = nullptr; uint32 Fallbacks = 0;
 if (!WoTWorldBuildRuntimeBSPForTesting(*W, Descriptor, Parts, Geometry, Fallbacks, Error,
  {}, nullptr, nullptr, nullptr, nullptr, EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags, &InvisibleCollision)) return false;
 Geometry->FinishSpawning(FTransform::Identity, true);
 FWoTWorldProjectionSource Source;
 Source.MapIdentity = Descriptor.MapIdentity; Source.Records = Resource.Actors();
 Source.Points = Resource.Points(); Source.Nodes = Resource.Nodes(); Source.Vertices = Resource.Vertices();
 Source.Zones = Resource.Zones(); Source.Surfaces = Resource.Surfaces(); Source.Leaves = Resource.Leaves();
 Source.ModelLights = Resource.ModelLights(); Source.MoverModels = Resource.MoverModels();
 Source.MoverAnimationGroups = Resource.MoverAnimationGroups(); Source.MoverAnimationTerms = Resource.MoverAnimationTerms();
 Source.SemanticObjects = Resource.SemanticObjects(); Source.ReachSpecs = Resource.ReachSpecs();
 Source.bBSPRootOutside = Resource.BSPRootOutside();
 FWoTWorldProjectionResult Projected;
 if (!FWoTWorldProjector::ProjectDeferred(*W, Source, Maps.Defaults, Projected, Error)) return false;
 if (OutTable) *OutTable = Projected.MapLightTable;
 for (AActor* Actor : Projected.DeferredActors) if (IsValid(Actor)) Actor->FinishSpawning(Actor->GetActorTransform());
 return true;
}

}
#endif
