#pragma once

#include "CoreMinimal.h"
#include "../WoTGameMode.h"
#include "../Inventory/WoTInventory.h"
#include "WoTTestG38Classes.generated.h"

// Standalone normally rejects inventory respawn. This fixture exposes the
// virtual ShouldRespawn seam so G38 can exercise the real pickup gate.
UCLASS()
class AWoTTestG38GameMode : public AWoTGameMode
{
	GENERATED_BODY()

public:
	virtual bool ShouldRespawn(AWoTInventory* Item) const override
	{
		return Item && Item->RespawnTime != 0.0f;
	}
};
