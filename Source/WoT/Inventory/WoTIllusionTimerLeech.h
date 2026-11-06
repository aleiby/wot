#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTIllusionTimerLeech.generated.h"

// Port of WOT.IllusionTimerLeech: a non-removable, beneficial countdown icon
// attached to the Personal Illusion caster. Spec:
// exports/script/WOT/IllusionTimerLeech.uc.
UCLASS()
class UWoTIllusionTimerLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTIllusionTimerLeech();
};
