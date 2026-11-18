#pragma once

#include "CoreMinimal.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "WoTTestBotCasting.generated.h"

UCLASS()
class AWoTTestBotCastingItem : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	int32 ActivationCount = 0;
	virtual void CastArtifact() override
	{
		++ActivationCount;
		Super::CastArtifact();
	}
};
