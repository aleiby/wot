#include "WoTUE1LightRegistry.h"

#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "Projectiles/WoTAngrealProjectile.h"

EWoTDynLightShape FWoTUE1LightRow::DynamicShape() const
{
	switch (Shape)
	{
	case EWoTUE1LightShape::NonIncidence: return EWoTDynLightShape::NonIncidence;
	case EWoTUE1LightShape::Shell: return EWoTDynLightShape::Shell;
	case EWoTUE1LightShape::None:
	default: return EWoTDynLightShape::Incidence;
	}
}

namespace
{
	TUniquePtr<FWoTUE1LightSet>& InstalledLightRegistry()
	{
		static TUniquePtr<FWoTUE1LightSet> Installed;
		return Installed;
	}
}

void FWoTUE1LightRegistryBuilder::Install(FWoTUE1LightSet&& Set)
{
	TUniquePtr<FWoTUE1LightSet> Candidate =
		MakeUnique<FWoTUE1LightSet>(MoveTemp(Set));
	Swap(InstalledLightRegistry(), Candidate);
}

#if WITH_AUTOMATION_TESTS
void FWoTUE1LightRegistryBuilder::Reset()
{
	InstalledLightRegistry().Reset();
}
#endif

const TMap<FName, FWoTUE1LightRow>& WoTUE1LightRegistry::All()
{
	const TUniquePtr<FWoTUE1LightSet>& Installed = InstalledLightRegistry();
	if (Installed) return Installed->Rows;
	static const TMap<FName, FWoTUE1LightRow> Empty;
	return Empty;
}

const FWoTUE1LightRow* WoTUE1LightRegistry::Find(FName ClassName)
{
	return All().Find(ClassName);
}

bool WoTUE1LightRegistry::Configure(UPointLightComponent* Light,
	FName ClassName)
{
	if (Light == nullptr)
	{
		return false;
	}
	if (const UWorld* World = Light->GetWorld();
		World != nullptr && World->GetNetMode() == NM_DedicatedServer)
	{
		Light->SetVisibility(false);
		return false;
	}
	const FWoTUE1LightRow* Row = Find(ClassName);
	if (Row == nullptr)
	{
		// Activation is transactional. An unavailable family is an expected
		// pre-install/failure state, not a constructor-time assertion.
		Light->SetVisibility(false);
		return false;
	}
	return Configure(Light, *Row);
}

bool WoTUE1LightRegistry::Configure(UPointLightComponent* Light,
	const FWoTUE1LightRow& Row)
{
	if (!Light)
	{
		return false;
	}
	if (const UWorld* World = Light->GetWorld();
		World != nullptr && World->GetNetMode() == NM_DedicatedServer)
	{
		Light->SetVisibility(false);
		return false;
	}
	AWoTAngrealProjectile::ConfigureUE1Light(Light, Row.Brightness, Row.Hue,
		Row.Saturation, Row.Radius, Row.DynamicShape(), Row.Type,
		Row.Period, Row.Phase);
	Light->SetVisibility(true);
	return true;
}
