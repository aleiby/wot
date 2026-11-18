#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvIce.generated.h"

// Port of Angreal.AngrealInvIce — the Freeze ter'angreal. Spec:
// exports/script/Angreal/AngrealInvIce.uc. Thin ProjectileLauncher: all
// behavior is defaultproperties. Pickup mesh (Angreal.Freeze, DrawScale 0.3),
// icons (I_Freeze/M_Freeze) bind on the native CDO through soft refs.
UCLASS()
class AWoTAngrealInvIce : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvIce();
};
