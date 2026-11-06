#pragma once

#include "CoreMinimal.h"
#include "WoTReflectorInstaller.h"
#include "WoTAngrealInvReflect.generated.h"

// Port of Angreal.AngrealInvReflect — the Reflect ter'angreal. Spec:
// exports/script/Angreal/AngrealInvReflect.uc. Installs a ReturnToSenderReflector
// on the caster for 5s so incoming weaves bounce back at their originators.
UCLASS()
class AWoTAngrealInvReflect : public AWoTReflectorInstaller
{
	GENERATED_BODY()

public:
	AWoTAngrealInvReflect();
};
