#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvTrapDetect.generated.h"

// Port of Angreal.AngrealInvTrapDetect (expands LeechAttacher) — the Trap Detect
// ter'angreal: for Duration seconds it makes nearby traps obvious (a quickening
// heartbeat + an on-trap indicator). Spec:
// exports/script/Angreal/AngrealInvTrapDetect.uc.
//
// LeechAttacher folds (as for Remove Curse): CastArtifact attaches a
// UWoTTrapDetectLeech to the caster for Duration, then spends a charge. The leech
// does the detection; the heartbeat/indicator output is deferred presentation.
UCLASS()
class AWoTAngrealInvTrapDetect : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvTrapDetect();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|TrapDetect") float Duration = 15.0f;

	virtual void CastArtifact() override;   // Cast(): attach the detector
};
