#pragma once

#include "CoreMinimal.h"
#include "WoTReflectorInstaller.h"
#include "WoTAngrealInvTarget.generated.h"

// Port of Angreal.AngrealInvTarget — the Find Target ter'angreal. Spec:
// exports/script/Angreal/AngrealInvTarget.uc. Installs an AutoTargetReflector on
// the caster for 15s so all seeking weaves auto-target the nearest victim.
UCLASS()
class AWoTAngrealInvTarget : public AWoTReflectorInstaller
{
	GENERATED_BODY()

public:
	AWoTAngrealInvTarget();
};
