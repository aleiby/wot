#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTTaintProjectile.generated.h"

// Port of Angreal.AngrealTaintProjectile: seeks its victim and permanently
// taints every angreal they hold (no direct damage — Damage stays 0).
// TaintAllAngrealEffect is inlined into Explode per the deferred-Invokable
// convention (PORT_NOTES). Deferred visuals: TaintVisual trail +
// TaintExpDarts/Torus/GeoA/GeoB burst.
UCLASS()
class AWoTTaintProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:
	AWoTTaintProjectile();
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
	UPROPERTY(Transient) TObjectPtr<class AWoTTaintVisual> Aura;

	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
};
