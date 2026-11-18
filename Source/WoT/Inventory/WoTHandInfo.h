#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTHandInfo.generated.h"

class AWoTInventory;
class UWoTInventoryComponent;

// HandInfo.uc: one fixed-size angreal hand. This is client-local UI state, not
// replicated gameplay state.
UCLASS()
class UWoTHandInfo : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 NumSlots = 10;

	UPROPERTY() FName ClassName[10];
	// UHT rejects bool[] as UPROPERTY ("Bool arrays are not allowed"); this is
	// non-replicated view-model state, so a plain zero-init C-array is fine.
	bool bHaveThis[10] = {};
	UPROPERTY() TWeakObjectPtr<AWoTInventory> Item[10];
	UPROPERTY() int32 Selected = 0;
	UPROPERTY() FName ItemAdded;
	float ItemAddedTime = 0.0f;
	// Editor cards have no live inventory actor. INDEX_NONE means the normal HUD path
	// should read the live item; otherwise this is the owner-replicated budget count.
	int32 DisplayCount[10] = {};
	bool bHasDisplayCount[10] = {};

	void AddClassName(FName NewClassName, int32 Slot);
	void AddItem(FName ItemName, bool bSelect, UWoTInventoryComponent* Inv);
	void RemoveItem(FName ItemName);
	void Update(UWoTInventoryComponent* Inv);
	void SetDisplayCount(FName ItemName, int32 NewCount);
	int32 GetDisplayCount(int32 Index) const;

	void Select(int32 Index);
	void SelectItem(FName ItemName);
	void SelectFirst();
	/** The last held slot (the mirror of SelectFirst). */
	void SelectLast();
	void SelectNext();
	void SelectPrevious();

	FName GetClassName(int32 Index) const;
	FName GetSelectedClassName() const;
	AWoTInventory* GetItem(int32 Index) const;
	AWoTInventory* GetSelectedItem() const;
	int32 GetItemCount() const;
	bool IsEmpty() const;
	bool IsIn(FName ItemName) const;
};
