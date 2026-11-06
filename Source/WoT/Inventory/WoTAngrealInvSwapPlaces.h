#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvSwapPlaces.generated.h"

// Port of Angreal.AngrealInvSwapPlaces (expands ProjectileLauncher) — the Swap
// Places ter'angreal. Spec: exports/script/Angreal/AngrealInvSwapPlaces.uc.
// Fires AWoTSwapPlacesProjectile (a seeker) at the caster's target; the swap
// itself lives on that projectile. Pickup mesh binds via the generated leaf BP;
// icons bind on the native CDO.
UCLASS()
class AWoTAngrealInvSwapPlaces : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvSwapPlaces();
};
