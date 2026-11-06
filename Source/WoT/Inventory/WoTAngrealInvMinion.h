#pragma once

#include "CoreMinimal.h"
#include "WoTResourceSpawnerInv.h"
#include "WoTAngrealInvMinion.generated.h"

// Port of Angreal.AngrealInvMinion (expands ResourceSpawner) — summons a Grunt-tier
// helper of the caster's faction. Spec: exports/script/Angreal/AngrealInvMinion.uc.
UCLASS()
class AWoTAngrealInvMinion : public AWoTResourceSpawnerInv
{
	GENERATED_BODY()

public:
	AWoTAngrealInvMinion();

	// GetResourceClass -> GetTroop(Owner, 'Grunt') (AngrealInvMinion.uc:34).
	virtual FName GetTierKeyword() const override { return TEXT("Grunt"); }
};
