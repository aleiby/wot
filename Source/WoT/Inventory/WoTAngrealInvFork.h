#pragma once

#include "CoreMinimal.h"
#include "WoTReflectorInstaller.h"
#include "WoTAngrealInvFork.generated.h"

// Port of Angreal.AngrealInvFork — the Fork ter'angreal. Spec:
// exports/script/Angreal/AngrealInvFork.uc. Installs a ForkRTSReflector on the
// caster for 20s: incoming weaves still hit, but a duplicate flies back at the
// originator.
UCLASS()
class AWoTAngrealInvFork : public AWoTReflectorInstaller
{
	GENERATED_BODY()

public:
	AWoTAngrealInvFork();
};
