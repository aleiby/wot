#pragma once

#include "CoreMinimal.h"
#include "WoTResourceSpawnerInv.h"
#include "WoTAngrealInvChampion.generated.h"

// Port of Angreal.AngrealInvChampion (expands ResourceSpawner) — summons the
// Champion-tier helper of the caster's faction. Spec:
// exports/script/Angreal/AngrealInvChampion.uc.
UCLASS()
class AWoTAngrealInvChampion : public AWoTResourceSpawnerInv
{
	GENERATED_BODY()

public:
	AWoTAngrealInvChampion();

	// GetResourceClass -> GetTroop(Owner, 'Champion') (AngrealInvChampion.uc:34).
	virtual FName GetTierKeyword() const override { return TEXT("Champion"); }
};
