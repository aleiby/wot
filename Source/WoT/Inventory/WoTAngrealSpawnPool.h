#pragma once

#include "CoreMinimal.h"
#include "WoTInventory.h"
#include "WoTAngrealSpawnPool.generated.h"

// Port of Angreal.AngrealSpawnPool (expands Inventory) — the weighted-random
// artifact spawner placed throughout MP maps (Common/Uncommon/Rare subclasses).
// Spec: exports/script/Angreal/AngrealSpawnPool.uc.
//
// The pool is invisible (UE1 bHidden) but keeps the Inventory pickup collision.
// It spawns one pickup from Pool[] (weighted by Weight[]), disables that item's
// own collision, and on Touch hands the item to the toucher; if bRespawn it
// respawns after RespawnTime. Pool[]/Weight[] come from the map t3d — the
// importer maps the class names to the ported AWoTAngrealInv* and drops the
// unported ones (PORT_NOTES "AngrealSpawnPool").
//
UCLASS()
class AWoTAngrealSpawnPool : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealSpawnPool();

	// single-shot vs. repeated respawn
	UPROPERTY(EditAnywhere, Category = "WoT|SpawnPool") bool bRespawn = true;
	// the inventory classes this point spawns, and their relative weights
	// (parallel arrays; RespawnTime is inherited from AWoTInventory).
	UPROPERTY(EditAnywhere, Category = "WoT|SpawnPool")
	TArray<TSubclassOf<AWoTInventory>> Pool;
	UPROPERTY(EditAnywhere, Category = "WoT|SpawnPool") TArray<int32> Weight;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> CommonSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> UncommonSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> RareSound;

	// the pickup currently waiting in this pool (None while respawning)
	UPROPERTY() TObjectPtr<AWoTInventory> PickupItem;
	// The rarity sound the last SpawnFromPool played (None: silent) — spec seam.
	FName LastAnnouncedSoundForTest;

	// Weighted pick over the contiguous prefix before the first null Pool entry;
	// INDEX_NONE when that retail-visible prefix is depleted.
	int32 GetWeightedSelection() const;
	// AngrealSpawnPool.Waiting: integer NumPlayers / max(Ideal, 1), then float divide.
	static float ComputeRespawnDelay(float InRespawnTime, int32 IdealPlayerCount,
		int32 NumPlayers);
	// state Active BeginState: spawn a pickup and let the pool own its Touch.
	// bAnnounce plays the rarity sound (RespawnA/B/C). The level-startup entry
	// (BeginPlay) passes false: retail entered the auto state Active from
	// SetInitialState during UE1's LoadMap, BEFORE any
	// player actor exists (MatchViewportsToActors -> SpawnPlayActor follows
	// later in LoadMap; remote players log in later still), and the
	// server PlaySound reaches only PawnList players — the
	// initial placement was silent. Every later (re)spawn announces.
	void SpawnFromPool(bool bAnnounce = true);
	// AngrealSpawnPool.uc:55-62 (called from BeginState :94): re-touch pawns
	// already standing in the pool on each (re)spawn — begin-overlap alone would
	// let a camper's next pickup sit uncollected forever.
	void CheckTouching();

	virtual void BeginPlay() override;
	virtual void OnPickupOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                             UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                             bool bFromSweep, const FHitResult& Hit) override;

private:
	// state Active Touch: validate + hand the waiting pickup to Other, then
	// schedule the respawn (shared by the overlap event and CheckTouching).
	void HandlePoolTouch(AActor* Other);

	UFUNCTION()
	void RespawnPool();   // state Waiting -> Active
};
