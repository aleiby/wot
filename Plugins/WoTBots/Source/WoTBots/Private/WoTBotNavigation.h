#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "WoTBotNavigation.generated.h"

UCLASS(Transient)
class AWoTBotNavMesh : public ARecastNavMesh
{
	GENERATED_BODY()
public:
	AWoTBotNavMesh();
	virtual void PostInitProperties() override;
	virtual void OnNavMeshTilesUpdated(const TArray<FNavTileRef>& ChangedTiles) override;
	// Tiles built or rebuilt since this mesh was created (soak diagnostics).
	int32 GetTileUpdateCount() const { return TileUpdates; }
private:
	int32 TileUpdates = 0;
};

// A null preferred class matches every nav class in the engine. Keep the
// pre-plugin main-data policy for every caller except an explicit bot opt-in.
UCLASS(Transient)
class UWoTBotNavigationSystem : public UNavigationSystemV1
{
	GENERATED_BODY()
public:
	virtual void PostInitProperties() override;
	virtual void InitializeForWorld(UWorld& World, FNavigationSystemRunMode Mode) override;
	virtual void CleanUp(FNavigationSystem::ECleanupMode Mode) override;
	virtual ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props) override;
	virtual const ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props) const override;
	virtual ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props,
		const FVector& Location, const FVector& Extent = INVALID_NAVEXTENT) const override;
#if WITH_AUTOMATION_TESTS
	virtual void RebuildDirtyAreas(float DeltaSeconds) override;
#endif
private:
	void ApplyProjectileRelevance(AActor* Actor);
	FDelegateHandle ProjectileSpawnHandle;
};

namespace WoTBotNavigation
{
#if WITH_AUTOMATION_TESTS
	DECLARE_MULTICAST_DELEGATE_TwoParams(FDirtyAreasObserved, UWorld*, const TArray<FNavigationDirtyArea>&);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FTilesObserved, AWoTBotNavMesh*, const TArray<FNavTileRef>&);
	extern FDirtyAreasObserved DirtyAreasObserved;
	extern FTilesObserved TilesObserved;
#endif
	void Register();
	void Unregister();
}
