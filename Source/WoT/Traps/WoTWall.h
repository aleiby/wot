#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../Inventory/WoTInventory.h"
#include "WoTWall.generated.h"

class UWoTVertexAnimComponent;

// Port of WoTTraps/WallSlab.uc — a blocking barricade segment with staged damage. `WithstandDamage`
// replicates to all clients; the damage-STAGE visual rode the native `Skin` in UE1 (auto-replicated),
// which UE5 does NOT auto-rep — so the port DERIVES the stage from the replicated `WithstandDamage`
// (OnRep re-applies the material on clients), the same "replicate the value, re-derive presentation"
// pattern as N4 ItemState. Spawned + owned by AWoTWall.
UCLASS()
class AWoTWallSlab : public AWoTTrap
{
	GENERATED_BODY()

public:
	AWoTWallSlab();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 SlabWidth = 28;

	// WithstandDamage=150 (WallSlab.uc). Replicated to all clients; the stage derives from it.
	UPROPERTY(ReplicatedUsing = OnRep_WithstandDamage, EditDefaultsOnly, Category = "WoT|Trap")
	int32 WithstandDamage = 150;

	// Damage stage 0..2 (Stage1/2/3) derived from WithstandDamage vs the class-default 150 (the
	// original keys off class'WallSlab'.default): >2/3 → 0, >1/3 → 1, >0 → 2; ≤0 (returns 3) is
	// destruction. This derivation is the port's replacement for the un-replicated native Skin swap —
	// a client re-derives the stage from the replicated WithstandDamage.
	int32 GetStage() const;

	// WallSlab.default.CollisionHeight, read from the capsule itself. On a class default
	// object the components are unregistered, and outside editor builds
	// AActor::GetSimpleCollisionHalfHeight answers 0 for it (Actor.cpp
	// GetComponentsBoundingCylinder ignores registration only WITH_EDITOR), which sank
	// every packaged wall by half a slab while the editor specs passed.
	float GetSlabCollisionHalfHeight() const;

	// Param named Damage (not the engine's DamageAmount): AWoTTrap carries a DamageAmount
	// member, and MSVC treats the shadow as an error (C4458).
	virtual float TakeDamage(float Damage, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	// WallSlab.GetBaseResource returns its Owner (the Wall) — the slabs of one wall are one logical trap.
	virtual AActor* GetBaseResource() override { return GetOwner() ? GetOwner() : this; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<UWoTVertexAnimComponent> SlabMesh;
	// The class-default visual (6b): mesh name + UE1 DrawScale. The #exec MESHMAP
	// SCALE is baked into the imported geometry by mesh2glb, so the component scale
	// is the defaultproperties DrawScale ONLY (unset = 1.0 for WallSlab;
	// FireWallSlab overrides to FWBaseLogs at its authored 1.2).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName VisualMeshName;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FVector VisualScale = FVector(1.0f);
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void OnRep_WithstandDamage();
	// Apply the stage skin for GetStage() to the mesh (MI_TrapWall2/MI_TrapWall3; full health
	// keeps the base skin). The gameplay observable is GetStage().
	void ApplyStagePresentation();
};

// Port of WoTTraps/Wall.uc — the server-only, invisible placement MANAGER that spawns/owns N WallSlabs
// across a room. bHidden, no collision, NOT replicated (RemoteRole=ROLE_None). The slabs are the
// visible/blocking/replicated actors.
UCLASS()
class AWoTWall : public AWoTTrap
{
	GENERATED_BODY()

public:
	AWoTWall();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") TSubclassOf<AWoTWallSlab> SlabClass;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 MaxSlabs = 32;
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") bool bNorthSouth = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 FloorTolerance = 100;

	virtual bool CalcLocation(FVector& StartLocation, FVector& StartNormal) override;
	virtual bool DeployResource(FVector StartLocation, FVector StartNormal) override;
	virtual bool RemoveResource() override;
	virtual void BeginEditingResource(uint8 PlacedByTeam) override;
	virtual void EndEditingResource() override;
	virtual FRotator GetSlabRotation(int32 SlabIndex, const FVector& Normal) const;
	// Wall.uc places by SlabClass.default.SlabWidth / CollisionHeight. A wall whose
	// slab defaults are activated at runtime answers from that data instead.
	virtual int32 GetSlabWidth() const;
	virtual float GetSlabHalfHeight() const;

	int32 GetNumSlabsForTest() const { return NumSlabs; }

	// Wall.Destroyed: tear down every WallSlab this wall owns.
	virtual void Destroyed() override;
	// Wall.TakeDamage is empty — the slabs take damage individually.
	virtual float TakeDamage(float, const FDamageEvent&, AController*, AActor*) override { return 0.0f; }

private:
	FVector Floor = FVector::ZeroVector;
	FVector WallStop = FVector::ZeroVector;
	int32 NumSlabs = 0;
};

// Citadel-editor placement adapter (WallInventory.uc). Data-only leaf.
UCLASS()
class AWoTWallInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTWallInventory();
};
