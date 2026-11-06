#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvTaint.generated.h"

// Port of Angreal.AngrealInvTaint — permanently taints every ter'angreal the
// target holds (all five elements, rare). Spec: exports/script/Angreal/
// AngrealInvTaint.uc. Pickup mesh/icons bind via the leaf-BP/data pass.
UCLASS()
class AWoTAngrealInvTaint : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvTaint();
};
