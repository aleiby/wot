#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvFireball.generated.h"

// Port of Angreal.AngrealInvFireball — the Fireball ter'angreal, first
// concrete artifact (M3 skeleton). Spec: exports/script/Angreal/
// AngrealInvFireball.uc. Pickup mesh (Angreal.AngrealFireballPickup,
// DrawScale 1.3) binds via the generated leaf BP; icons bind on the native CDO.
UCLASS()
class AWoTAngrealInvFireball : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvFireball();
};
