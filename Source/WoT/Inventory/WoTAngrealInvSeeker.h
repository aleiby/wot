#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvSeeker.generated.h"

// Port of Angreal.AngrealInvSeeker — launches a seeking explosive at the
// best target; fails ("requires a target") with none in view. Spec:
// exports/script/Angreal/AngrealInvSeeker.uc. Pickup mesh/icons bind via
// the leaf-BP/data pass.
UCLASS()
class AWoTAngrealInvSeeker : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvSeeker();
};
