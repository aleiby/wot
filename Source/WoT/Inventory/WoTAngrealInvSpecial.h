#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvSpecial.generated.h"

// Port of Angreal.AngrealInvSpecial — the "Unknown" ter'angreal. Spec:
// exports/script/Angreal/AngrealInvSpecial.uc. A deliberately non-functional
// placeholder: every element, ChargeCost 0, no Cast effect (the base CastArtifact
// sound is all it does), and it can't be dropped (empty DropFrom).
UCLASS()
class AWoTAngrealInvSpecial : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvSpecial();

	// An empty override makes this artifact non-droppable
	// (AngrealInvSpecial.uc:29-32).
	virtual void DropFrom(FVector StartLocation) override {}
};
