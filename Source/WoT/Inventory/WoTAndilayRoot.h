#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAndilayRoot.generated.h"

// Port of Angreal.AndilayRoot — a placed inventory pickup that immediately
// converts all 15-20 charges into a singular healing leech. Spec:
// exports/script/Angreal/AndilayRoot.uc.
UCLASS()
class AWoTAndilayRoot : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAndilayRoot();

	int32 GetPotentialHealth(const class AWoTCharacter* Other) const;
	virtual void GiveTo(APawn* Other) override;

protected:
	virtual bool ValidTouch(AActor* Other) override;

private:
	class UWoTAndilayRootLeech* FindHealingLeech(APawn* Host) const;
};
