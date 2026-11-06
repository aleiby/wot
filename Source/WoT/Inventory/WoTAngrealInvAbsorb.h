#pragma once

#include "CoreMinimal.h"
#include "WoTReflectorInstaller.h"
#include "WoTAngrealInvAbsorb.generated.h"

// Port of Angreal.AngrealInvAbsorb — the Absorb ter'angreal. Spec:
// exports/script/Angreal/AngrealInvAbsorb.uc. Installs an AbsorbReflector on the
// caster for 8s: the next weave that strikes you snatches its firing artifact
// into your inventory.
UCLASS()
class AWoTAngrealInvAbsorb : public AWoTReflectorInstaller
{
	GENERATED_BODY()

public:
	AWoTAngrealInvAbsorb();
};
