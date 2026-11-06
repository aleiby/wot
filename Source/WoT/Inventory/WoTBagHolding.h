#pragma once

#include "CoreMinimal.h"
#include "WoTInventory.h"
#include "WoTBagHolding.generated.h"

// WOT.BagHolding's live Items path. The bag is the replicated world pickup;
// its contents remain authority-owned actors until GiveTo dispatches them.
UCLASS()
class AWoTBagHolding : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTBagHolding();

	static constexpr int32 MaxItems = 40;

	bool AddItem(AWoTInventory* Item);
	virtual void GiveTo(APawn* Other) override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;

	bool IsInitialized() const { return bInitialized; }
	bool IsDestroyingBag() const { return bDestroyingBag; }
	const TArray<TObjectPtr<AWoTInventory>>& GetItems() const { return Items; }

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Inventory") bool bCanTeleport = true;

private:
	UPROPERTY() TArray<TObjectPtr<AWoTInventory>> Items;
	UPROPERTY() TObjectPtr<APawn> WaitingPawn;
	bool bInitialized = false;
	bool bDestroyingBag = false;
	FTimerHandle DestroyTimer;

	void DestroyBag();
};
