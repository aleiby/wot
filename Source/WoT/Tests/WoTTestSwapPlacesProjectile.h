#pragma once

#include "CoreMinimal.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "WoTTestSwapPlacesProjectile.generated.h"

// Test-only swap weave that counts how often it is told to strike (Explode).
// A weave strikes once; a second strike is counted and goes no further, so a
// spec that provokes one fails on the count instead of recursing until the
// stack runs out. Inert in play.
UCLASS()
class AWoTTestCountingSwapProjectile : public AWoTSwapPlacesProjectile
{
	GENERATED_BODY()

public:
	int32 Strikes = 0;

	virtual void Explode(FVector HitLocation, FVector HitNormal) override
	{
		if (++Strikes > 1)
		{
			return;
		}
		Super::Explode(HitLocation, HitNormal);
	}
};
