#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvSoulBarb.generated.h"

// Port of Angreal.AngrealInvSoulBarb — the Soul Barb ter'angreal. Spec:
// exports/script/Angreal/AngrealInvSoulBarb.uc. Thin ProjectileLauncher; pickup
// mesh (Angreal.AngrealSoulBarbPickup) binds via the generated leaf BP; icons
// bind on the native CDO. Its projectile's damage-reflector payload is deferred on the
// Reflector framework (see WoTSoulBarbProjectile).
UCLASS()
class AWoTAngrealInvSoulBarb : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvSoulBarb();
};
