#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvDecay.generated.h"

// Port of Angreal.AngrealInvDecay — the Decay ter'angreal. Spec:
// exports/script/Angreal/AngrealInvDecay.uc. Thin ProjectileLauncher; pickup
// mesh (Angreal.AngrealDecayPickup, DrawScale 0.75) binds via the generated
// leaf BP; the immutable shipped icon pair binds on the native CDO.
UCLASS()
class AWoTAngrealInvDecay : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvDecay();
};
