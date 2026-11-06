#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvExpWard.generated.h"

// Port of Angreal.AngrealInvExpWard — the Explosive Ward ter'angreal. Spec:
// exports/script/Angreal/AngrealInvExpWard.uc. Thin ProjectileLauncher that
// places an AWoTExpWardProjectile landmine. Pickup mesh
// (Angreal.AngrealLandMinePickup) + icons bind via the leaf-BP / soft-ref pass.
UCLASS()
class AWoTAngrealInvExpWard : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvExpWard();
};
