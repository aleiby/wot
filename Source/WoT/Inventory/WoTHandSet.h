#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTHandSet.generated.h"

class FWoTRuntimeClassDefaults;

class UDataTable;
class UWoTHandInfo;
class UWoTInventoryComponent;

// HandSet.uc: ten fixed hands over the character's flat replicated inventory.
UCLASS()
class UWoTHandSet : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 NumHands = 10;

	UPROPERTY() TObjectPtr<UWoTHandInfo> Hands[10];
	UPROPERTY() int32 Selected = 0;

	void Init(UObject* Outer);
	void SetHand(int32 Index, UWoTHandInfo* Hand);
	UWoTHandInfo* GetHand(int32 Index) const;
	UWoTHandInfo* GetSelectedHand() const;

	void Select(int32 Index);
	void SelectItem(FName ItemName);
	int32 GetNext() const;
	int32 GetPrevious() const;
	void AddItem(FName ItemName, UWoTInventoryComponent* Inv);
	void RemoveItem(FName ItemName);
	void Update(UWoTInventoryComponent* Inv);
	void PopulateFromTable(const UDataTable* Table);
	/** Guarded builds cook no /Game/WoT/Data, so DT_WoTAngrealHands is absent:
	 * fill from the retail WOTPlayer class default AngrealHandOrder[100]
	 * exactly as WOTPlayer.CreateAngrealHands (WOTPlayer.uc:4719-4729).
	 * Returns the number of planned entries added. */
	int32 PopulateFromRuntimeClassDefaults(const FWoTRuntimeClassDefaults& Defaults);
};
