#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvShift.generated.h"

// Port of Angreal.AngrealInvShift, the shift artifact. Spec:
// exports/script/Angreal/AngrealInvShift.uc (+ ShiftEffect.uc). On cast it
// teleports the caster ShiftDistance along the view (through obstacles) if no
// blocking actor occupies the destination (Util.ActorFits) and SetLocation can
// place it there: the body must fit the level at the destination (a graze is
// nudged out; solid space, such as under the floor or behind an outer wall,
// never fits) with no blocking actor where it lands. Any weaves locked onto the
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
