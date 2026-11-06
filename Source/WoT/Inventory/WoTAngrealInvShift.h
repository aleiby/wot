#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvShift.generated.h"

// Port of Angreal.AngrealInvShift — the Shift ter'angreal. Spec:
// exports/script/Angreal/AngrealInvShift.uc (+ ShiftEffect.uc). On cast it
// teleports the caster ShiftDistance along the view (through obstacles) if no
// blocking actor occupies the destination (Util.ActorFits) and SetLocation can
// place it there (world geometry only nudges it), and any weaves locked onto the
// caster lose their target. Fails (no charge) otherwise.
//
// Deferred: none — the leech/reflector shift-escape and the arrival
// AppearEffect rings (Blue/Gold) are both live.
UCLASS()
class AWoTAngrealInvShift : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvShift();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shift") float ShiftDistance = 160.0f;
	// ShiftEffect.ActorFitsRadius (ShiftEffect.uc:191): the blocking-actor search.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shift") float ActorFitsRadius = 1024.0f;

	virtual void CastArtifact() override;   // Cast()
};
