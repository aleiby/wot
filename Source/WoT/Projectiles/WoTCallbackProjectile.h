#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTCallbackProjectile.generated.h"

// Port of Angreal.CallbackProjectile: invisible seeker that reports reach /
// loss back to its launching artifact (Lightning's leech link). The
// ProjLeechArtifact callbacks route through the delegate below until the
// artifact classes port.
UCLASS(Abstract)
class AWoTCallbackProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:
	AWoTCallbackProjectile();

	DECLARE_DELEGATE_OneParam(FOnReachedDestination, AActor* /*HitActor*/);
	DECLARE_DELEGATE(FOnDestinationLost);
	FOnReachedDestination OnReachedDestination;
	FOnDestinationLost OnDestinationLost;

	virtual void DestinationLost() override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void Destroyed() override;

private:
	bool bExploded = false;
};
