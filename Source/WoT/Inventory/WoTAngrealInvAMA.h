#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvAMA.generated.h"

// Port of Angreal.AngrealInvAMA (expands ProjectileLauncher) — the Aura of
// Unraveling ter'angreal: lobs an AWoTAngrealAMAProjectile land-mine that, when
// tripped, plants an anti-magic dome (AWoTAMAPearl). Spec:
// exports/script/Angreal/AngrealInvAMA.uc. Pickup mesh binds via the generated
// leaf BP; the immutable shipped icon pair binds on the native CDO.
UCLASS()
class AWoTAngrealInvAMA : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvAMA();
};
