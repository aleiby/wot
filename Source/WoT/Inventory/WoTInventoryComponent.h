#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTInventoryComponent.generated.h"

class AWoTAngrealInventory;
class AWoTInventory;

// UE1 Pawn.Inventory linked list → array, preserving
// the original chain order: AddInventory prepends (newest first), and cycling
// walks that order. Also folds DefaultCastReflector's cast gate (UseAngreal /
// CeaseUsingAngreal / NextFireTime) — the reflector framework is deferred
// (PORT_NOTES); it re-inserts here when the effects framework ports. The
// HandInfo groups that flat chain for the HUD; BagHolding is a world pickup
// whose server-owned item array feeds this component again when looted.
UCLASS(ClassGroup = (WoT), meta = (BlueprintSpawnableComponent))
class UWoTInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTInventoryComponent();

	UPROPERTY(ReplicatedUsing = OnRep_Inventory) TArray<TObjectPtr<AWoTInventory>> Items;
	UPROPERTY(ReplicatedUsing = OnRep_SelectedItem) TObjectPtr<AWoTInventory> SelectedItem;

	// Convert the port's WoT-prefixed C++ class name back to the shipped script
	// class name used by HandSet/HandInfo.
	static FName GetHandSetItemName(const AWoTInventory* Item);

	// Pawn.AddInventory + WOTPlayer auto-select of the first artifact
	bool AddInventory(AWoTInventory* NewItem);
	// Pawn.DeleteInventory + WOTPlayer's uncast/Reset of angreal
	bool DeleteInventory(AWoTInventory* Item);
	AWoTInventory* FindInventoryType(TSubclassOf<AWoTInventory> Type) const;
	// UE1 FindInventoryName matches the original script class name
	AWoTInventory* FindInventoryName(FName ClassName) const;
	// GameInfo.PickupQuery chain: true = an existing item handled the pickup
	bool HandlePickupQuery(AWoTInventory* Item) const;

	// WOTPlayer.ServerSelectItem
	void SelectItem(AWoTInventory* Item);
	// flat-chain Next/PrevInventory (hand cycling arrives with the player port)
	void NextInventory();
	void PrevInventory();

	// ---- DefaultCastReflector fold ----
	// Returns true only when the request passed the cast/interception/time gate
	// and the selected artifact accepted the activation.
	bool UseAngreal();
	void CeaseUsingAngreal();
	void NotifyCastFailed(AWoTAngrealInventory* FailedArtifact);
	// WOTPlayer.ChargeUsed — the reflectable charge-use notification (fans out to
	// the pawn's reflectors, e.g. RarityBasedDamage). HUD icon still deferred.
	void ChargeUsed(AWoTAngrealInventory* Artifact);

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION() void OnRep_Inventory();
	UFUNCTION() void OnRep_SelectedItem(AWoTInventory* PreviousSelectedItem);

	float NextFireTime = 0.0f;
	UPROPERTY() TObjectPtr<AWoTAngrealInventory> LastCastAngrealInventory;
	bool bLastCastSuccess = true;

	bool IsSelectable(const AWoTInventory* Item) const;
	void CycleSelection(int32 Direction);
	bool DispatchCastInterception(bool bUse);
};
