#include "WoTBotNavigation.h"
#include "AI/NavigationSystemConfig.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "Projectiles/WoTProjectile.h"

AWoTBotNavMesh::AWoTBotNavMesh()
{
	bCanBeMainNavData = false;
	AgentRadius = 17.0f;
	AgentHeight = 92.0f;
}

void AWoTBotNavMesh::PostInitProperties()
{
	Super::PostInitProperties();
	// Config is loaded after the constructor. Apply plugin-only resolution
	// here so inherited default mesh settings cannot erase narrow passages.
	bCanBeMainNavData = false;
	for (int32 Index = 0; Index < static_cast<int32>(ENavigationDataResolution::MAX); ++Index)
	{
		SetAgentMaxStepHeight(static_cast<ENavigationDataResolution>(Index), 25.0f);
		SetCellSize(static_cast<ENavigationDataResolution>(Index), 8.0f);
	}
}

void AWoTBotNavMesh::OnNavMeshTilesUpdated(const TArray<FNavTileRef>& ChangedTiles)
{
	Super::OnNavMeshTilesUpdated(ChangedTiles);
	TileUpdates += ChangedTiles.Num();
#if WITH_AUTOMATION_TESTS
	WoTBotNavigation::TilesObserved.Broadcast(this, ChangedTiles);
#endif
}

#if WITH_AUTOMATION_TESTS
WoTBotNavigation::FDirtyAreasObserved WoTBotNavigation::DirtyAreasObserved;
WoTBotNavigation::FTilesObserved WoTBotNavigation::TilesObserved;
void UWoTBotNavigationSystem::RebuildDirtyAreas(float DeltaSeconds)
{
	// Observe exactly the batch UE 5.8 NavigationDirtyAreasController dispatches.
	const auto& Controller = DefaultDirtyAreasController;
	const bool bDispatch = (Controller.DirtyAreasUpdateFreq != 0.f
		&& Controller.DirtyAreasUpdateTime + DeltaSeconds >= 1.f / Controller.DirtyAreasUpdateFreq)
		|| !GetWorld()->IsGameWorld();
	if (bDispatch) WoTBotNavigation::DirtyAreasObserved.Broadcast(GetWorld(), Controller.DirtyAreas);
	Super::RebuildDirtyAreas(DeltaSeconds);
}
#endif

void UWoTBotNavigationSystem::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		SupportedAgents = GetDefault<UNavigationSystemV1>()->GetSupportedAgents();
		if (SupportedAgents.IsEmpty())
			SupportedAgents.Add(FNavDataConfig(FNavigationSystem::FallbackAgentRadius,
				FNavigationSystem::FallbackAgentHeight));
		FNavDataConfig Bot(17.0f, 92.0f);
		Bot.Name = TEXT("ArenaBot");
		Bot.AgentStepHeight = 25.0f;
		Bot.SetNavDataClass(AWoTBotNavMesh::StaticClass());
		Bot.SetPreferredNavData(AWoTBotNavMesh::StaticClass());
		SupportedAgents.Add(Bot);
	}
}

void UWoTBotNavigationSystem::ApplyProjectileRelevance(AActor* Actor)
{
	if (!Actor || !Actor->IsA<AWoTProjectile>()) return;
	// UE 5.8 PrimitiveComponent::IsNavigationRelevant also accepts vehicle
	// blocking. Projectiles keep that collision response, but are not surfaces
	// or route obstacles. Apply before registration to avoid spawn/remove dirties.
	for (UActorComponent* Component : Actor->GetComponents())
		if (Component) Component->SetCanEverAffectNavigation(false);
}

void UWoTBotNavigationSystem::InitializeForWorld(UWorld& World, FNavigationSystemRunMode Mode)
{
	World.RemoveOnActorPreSpawnInitialization(ProjectileSpawnHandle);
	ProjectileSpawnHandle = World.AddOnActorPreSpawnInitialization(
		FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::ApplyProjectileRelevance));
	// Navigation may be initialized after world projection or gameplay spawns.
	// Remove existing projectiles before the initial octree/mesh is constructed.
	for (TActorIterator<AActor> It(&World); It; ++It) ApplyProjectileRelevance(*It);
	Super::InitializeForWorld(World, Mode);
}

void UWoTBotNavigationSystem::CleanUp(FNavigationSystem::ECleanupMode Mode)
{
	if (UWorld* World = GetWorld()) World->RemoveOnActorPreSpawnInitialization(ProjectileSpawnHandle);
	ProjectileSpawnHandle.Reset();
	Super::CleanUp(Mode);
}

const ANavigationData* UWoTBotNavigationSystem::GetNavDataForProps(const FNavAgentProperties& Props) const
{
	if (Props.PreferredNavData == FSoftClassPath(AWoTBotNavMesh::StaticClass()))
	{
		// Never fall back to the larger mesh while the bot mesh is building.
		return GetNavDataForAgentName(TEXT("ArenaBot"));
	}
	return GetDefaultNavDataInstance();
}

ANavigationData* UWoTBotNavigationSystem::GetNavDataForProps(const FNavAgentProperties& Props)
{
	return const_cast<ANavigationData*>(AsConst(*this).GetNavDataForProps(Props));
}

ANavigationData* UWoTBotNavigationSystem::GetNavDataForProps(const FNavAgentProperties& Props,
	const FVector&, const FVector&) const
{
	return const_cast<ANavigationData*>(GetNavDataForProps(Props));
}

namespace WoTBotNavigation
{
namespace
{
	FDelegateHandle WorldInit;
	void BeforeWorldInit(UWorld* World, const UWorld::InitializationValues)
	{
		if (!World || World->GetNetMode() == NM_Client) return;
		AWorldSettings* Settings = World->GetWorldSettings();
		UNavigationSystemConfig* Existing = Settings ? Settings->GetNavigationSystemConfig() : nullptr;
		if (!Existing || Existing->NavigationSystemClass != FSoftClassPath(UNavigationSystemV1::StaticClass())) return;
		// Duplicate the per-world mask and default-agent name; never write config
		// or modify a serialized map. This runs before navigation is instantiated.
		UNavigationSystemConfig* Config = DuplicateObject<UNavigationSystemConfig>(Existing, Settings);
		Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
		Config->SupportedAgentsMask.Set(GetDefault<UWoTBotNavigationSystem>()->GetSupportedAgents().Num() - 1);
		Settings->SetNavigationSystemConfigOverride(Config);
	}
}
void Register()
{
	if (!WorldInit.IsValid()) WorldInit = FWorldDelegates::OnPreWorldInitialization.AddStatic(&BeforeWorldInit);
}
void Unregister()
{
	FWorldDelegates::OnPreWorldInitialization.Remove(WorldInit);
	WorldInit.Reset();
}
}
