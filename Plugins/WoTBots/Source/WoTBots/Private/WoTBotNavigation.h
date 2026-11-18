#pragma once

#include "CoreMinimal.h"
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
};

// A null preferred class matches every nav class in the engine. Keep the
// pre-plugin main-data policy for every caller except an explicit bot opt-in.
UCLASS(Transient)
class UWoTBotNavigationSystem : public UNavigationSystemV1
{
	GENERATED_BODY()
public:
	virtual void PostInitProperties() override;
	virtual ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props) override;
	virtual const ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props) const override;
	virtual ANavigationData* GetNavDataForProps(const FNavAgentProperties& Props,
		const FVector& Location, const FVector& Extent = INVALID_NAVEXTENT) const override;
};

namespace WoTBotNavigation
{
	void Register();
	void Unregister();
}
