#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "WoTBudgetInfo.generated.h"

class AController;
class AWoTSealAltar;

// One resource line in a team's citadel-build budget (BudgetInfo.Resource, BudgetInfo.uc:14-19).
USTRUCT()
struct FWoTBudgetResource
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName ResourcePackageName;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName ResourceAdapterName;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") int32 ItemCount = 0;
};

// Port of BudgetInfo (exports/script/WOT/BudgetInfo.uc): a team's citadel-build budget, placed
// per team per Battle map (server-side, bStatic). This slice (citadel block step 5b) ports the
// budget bookkeeping — Reset / Increment / Decrement / GetItemCount / GetIndex, the edit
// reference-count (BeginEdit/EndEdit, PlayerEditLimit=1), and PlaceSeals (spend the seal budget
// onto an altar). The resource-package DynamicLoadObject side (ServerPreLoad / GetResourceType —
// which classes the adapters name) is deferred with the editor-resource subsystem.
UCLASS()
class AWoTBudgetInfo : public AInfo
{
	GENERATED_BODY()

public:
	// BudgetInfo.PlayerEditLimit (const 1): max players editing this team's budget at once.
	static constexpr int32 PlayerEditLimit = 1;

	// BudgetInfo.Team: which team this budget belongs to. BudgetInfo.Budget[12]: the resource
	// lines (map data). Kept as a TArray of the actual lines rather than a fixed [12] with empty
	// slots (divergence — GetArrayCount returns the real count).
	// No Team default is authored in BudgetInfo.uc; UE1 byte zero is therefore
	// the shipped value for the one budget per map whose T3D omits Team.
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") uint8 Team = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") TArray<FWoTBudgetResource> Budget;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName BeginEditEvent;
	UPROPERTY(EditAnywhere, Category = "WoT|Battle") FName EndEditEvent;

	// BudgetInfo.PlayerEditCount: reference count of players editing this budget.
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") int32 PlayerEditCount = 0;

	int32 GetArrayCount() const { return Budget.Num(); }
	int32 GetItemCount(int32 Index) const;
	FName GetResourceAdapterName(int32 Index) const;
	// BudgetInfo.GetResourceType (uc:70-88): scan in authored budget order and
	// return the first adapter whose CDO BaseResourceType matches the tier keyword.
	FName GetResourceAdapterForTier(FName TierKeyword) const;
	// BudgetInfo.GetIndex: the budget line with this adapter name (INDEX_NONE if absent — the
	// original asserts; the port guards).
	int32 GetIndex(FName ResourceAdapterName) const;

	// BudgetInfo.Reset: set the seal-budget line to SealCount. (un-hide AActor::Reset().)
	using AActor::Reset;
	void Reset(int32 SealCount);
	// BudgetInfo.Increment / Decrement: bump a named resource line's count.
	void Increment(FName ResourceName);
	void Decrement(FName ResourceName);

	// BudgetInfo.BeginEdit / EndEdit: the per-team edit reference count (PlayerEditLimit cap).
	// Player is the editing player's controller (its team must match this budget's Team).
	bool BeginEdit(AController* Player);
	void EndEdit(AController* Player);
	int32 SealBudgetRemaining() const;

	// BudgetInfo.PlaceSeals: spend the whole seal budget — spawn that many seals owned by the
	// player and force-bank them on the altar. Returns the number placed.
	int32 PlaceSeals(AWoTSealAltar* Altar, AController* Player);
};
