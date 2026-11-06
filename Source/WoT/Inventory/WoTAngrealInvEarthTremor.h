#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvEarthTremor.generated.h"

// Port of Angreal.AngrealInvEarthTremor (expands ProjectileLauncher) — the Earth
// Tremor ter'angreal. Spec: exports/script/Angreal/AngrealInvEarthTremor.uc.
// Launches AWoTEarthTremorProjectile (the lobbed Earth weave). Pickup mesh
// (Angreal.AngrealEarthTremorPickup) + icons bind via the generated
// leaf-BP/data pass.
UCLASS()
class AWoTAngrealInvEarthTremor : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvEarthTremor();
	virtual float GetMinRange() override { return 300.0f; }
};
