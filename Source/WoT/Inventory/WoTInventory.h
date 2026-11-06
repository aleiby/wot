#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../World/WoTRuntimeActorVisuals.h"
#include "WoTInventory.generated.h"

// UE1 Inventory states Pickup / Sleeping / Idle2 → shared enum FSM
// (plan Appendix A).
UENUM()
enum class EWoTInventoryState : uint8
{
	Pickup,     // sitting in the world, touchable
	Sleeping,   // hidden, waiting to respawn
	Held,       // in a pawn's inventory (UE1 Idle2)
};

struct FWoTInventoryFallZone
{
	FVector Gravity = FVector(0.0f, 0.0f, -950.0f);
	FVector Current = FVector::ZeroVector;
	float FluidFriction = 1.2f;
	bool bWater = false;
};

struct FWoTInventoryFallState
{
	FVector Location = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	bool bFalling = true;
	bool bLanded = false;
	FVector LandedNormal = FVector::ZeroVector;
	float ApexFraction = -1.0f;
};

// Port of Engine.Inventory + WoTBase.WOTInventory, collapsed like
// AWoTProjectile (engine layer is UE5-replaced; 'Inventory' vs 'WOTInventory'
// collide under the WoT class prefix). Spec: exports/script/WoTBase/
// WOTInventory.uc + the original engine's Inventory class (shipped Engine is
// source-stripped). Omitted subsystems (PORT_NOTES): armor (WOTInventory's
// ReduceDamage override already disabled that path), weapon chain,
// muzzle flash, first-person RenderOverlays mesh (retail-unused: no shipped
// item authors PlayerViewMesh and WOTPlayer pins Weapon=None — P01 census),
// travel.
UCLASS(Abstract)
class AWoTInventory : public AActor
{
	GENERATED_BODY()

public:
	AWoTInventory();

	// ---- Engine.Inventory ----
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Inventory") uint8 AutoSwitchPriority = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Inventory") uint8 InventoryGroup = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Inventory") bool bDisplayableInv = false;
	bool bActive = false;
	bool bHeldItem = false;    // Latched after pickup presentation ends (Engine.Inventory).
	// A spawn pool's SetCollision(false,false,false) on the item it just spawned
	// (AngrealSpawnPool.uc:74-75): the pool owns the Touch until it hands the
	// item over. Set before FinishSpawning so the item is never collidable at
	// spawn — UE1's SpawnActor raises no Touch,
	// whereas UE5 would fire the initial overlap and let a pawn standing in the
	// pool take the item behind the pool's back. Cleared when the item is held.
	bool bCollisionHeldByPool = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup") bool bInstantRespawn = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup") bool bRotatingPickup = true;
	FRotator PickupRotationRate = FRotator(0.0, 5000.0 * 360.0 / 65536.0, 0.0);
	// Localized (Engine.int [Inventory] and each subclass section): FWoTRetailText.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup") FString PickupMessage;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup") FString ItemName;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup") FString ItemArticle = TEXT("a");
	// EditAnywhere (not DefaultsOnly): retail Inventory/AngrealSpawnPool declare this
	// as var() — per-instance editable. Spawn pools carry rarity RespawnTime per placed
	// actor (Common 20 / Uncommon 40 / Rare 80), set by import_map.py from map data
	// rather than via near-empty rarity subclasses.
	UPROPERTY(EditAnywhere, Category = "WoT|Pickup") float RespawnTime = 0.0f;
	// Engine.Actor defaults: Mass=100; Buoyancy is zero-initialized.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement") float Mass = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement") float Buoyancy = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float MaxDesireability = 0.005f;

	// asset refs come from generated leaf BPs / data assets (ground rule 2)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display")
	TSoftObjectPtr<UTexture2D> StatusIcon;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> PickupSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> ActivateSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> DeactivateSound;
	// EditAnywhere (not DefaultsOnly): retail fixed AngrealInv authors override
	// RespawnSound per instance (Arena_03 T3Ds). G15 established that Python
	// cannot stamp an EditDefaultsOnly property into a generated map actor;
	// this follows G38's bMPAppearEffect precedent.
	UPROPERTY(EditAnywhere, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> RespawnSound;

	// ---- WoTBase.WOTInventory ----
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") FString Title = TEXT("[Title Missing]");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") FString Description = TEXT("[Description Missing]");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") FString Quote = TEXT("[Quote Missing]");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI")
	TSoftObjectPtr<UTexture2D> StatusIconSelected;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI")
	TSoftObjectPtr<UTexture2D> StatusIconFrame;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") bool bShowInfoHint = false;

	// The citadel-editor resource-count surface (WOTInventory.uc:28-29). Count replicates
	// COND_OwnerOnly — `reliable if( Role==ROLE_Authority && bNetOwner )` (WOTInventory.uc:31-36) —
	// the editing player's own resource count. Slice 2b does not spawn client inventory actors for
	// palette cards, so its live palette instead uses the controller's owner-only budget-row mirror;
	// this legacy parity surface remains for real inventory instances. SpawnTempResource is a static
	// editor flag (default True, WOTInventory.uc:69). BudgetInfo itself stays server-only.
	UPROPERTY(Replicated) int32 Count = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") bool SpawnTempResource = true;

	// WOTInventory.uc:25-26 — the citadel-editor placement adapter surface. ResourceClass is the
	// concrete resource (trap / pawn) this "hand card" places; BaseResourceType is the base-class
	// name for pawn resources (Grunt/Captain/Champion). Not replicated (only StatusIconFrame/Count
	// are). The editor place→spawn path that consumes these is DEFERRED with the citadel editor; the
	// fields land now so the trap `*Inventory` adapters (Slice 1) can set them in their CDOs.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") TSubclassOf<AActor> ResourceClass;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") FName BaseResourceType;
	// G15: the data-row identity for folded pawn resources. Retail ResourceClass named a
	// concrete WOTPawn leaf; the UE5 port spawns the common AWoTNPCCharacter class and
	// applies this row instead. NAME_None keeps traps and SealInventory on their class path.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI") FName CreatureRowName;

	// Inventory.Pickup.ValidTouch fires this through the event graph
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;

	// UE1 cylinder 30x30; touch = pawn overlap
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup")
	TObjectPtr<class USphereComponent> Collision;
	float GetPickupCollisionRadius() const;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Pickup")
	TObjectPtr<class UWoTVertexAnimComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") float AmbientGlow = 0.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") bool bStyleTranslucent = false;

	void ConfigureRuntimeVisualPresentation(
		const FWoTRuntimeActorVisualPresentation& Presentation);
	bool ConfigureRuntimeClassDefaultVisuals(
		FStringView ClassIdentity, FString& OutError);

	// ---- item-state FSM ----
	EWoTInventoryState GetItemState() const { return ItemState; }
	void GotoItemState(EWoTInventoryState NewState);

	// ---- pickup / ownership (Engine.Inventory) ----
	// Existing items get a chance to absorb/deny a pickup (returns true =
	// handled, no copy given). Chain iteration lives in UWoTInventoryComponent.
	virtual bool HandlePickupQuery(AWoTInventory* Item);
	AWoTInventory* SpawnCopy(APawn* Other);
	// copies over state that survives the pickup-respawn split; subclasses
	// must call Super
	virtual void InitializeCopy(AWoTInventory* Copy);
	virtual void GiveTo(APawn* Other);
	virtual void SetRespawn();
	// Inventory.Touch: run the full pickup path (ValidTouch -> PickupQuery charge-steal
	// -or- SpawnCopy+give). Returns true if the item was actually granted. A spawn pool
	// hands its item off through here (retail PickupItem.Touch(Other)) so the item's own
	// pickup query — including AngrealInventory's duplicate-charge steal — still runs.
	bool TryPickupBy(AActor* Other);
	virtual void DropFrom(FVector StartLocation);
	// Engine Inventory.Pickup.Landed: stop falling and flatten pitch only.
	virtual void Landed(const FVector& HitNormal);
	bool IsFalling() const { return bFalling; }
	const FVector& GetFallingVelocity() const { return FallingVelocity; }
	// Non-pawn AActor::physFalling with zone lookup and movement sweep injected.
	static void StepFalling(FWoTInventoryFallState& State, float DeltaTime,
		float InMass, float InBuoyancy,
		TFunctionRef<void(const FVector&, FWoTInventoryFallZone&)> ZoneAt,
		TFunctionRef<bool(const FVector&, const FVector&, FHitResult&)> Trace);
	virtual void BecomePickup();
	virtual void BecomeItem();

	class UWoTInventoryComponent* GetOwnerInventory() const;
	// Pawn.ClientMessage(Msg, Type) to the owning player. A WoT controller routes the
	// type to its HUD channel (WOTPlayer.uc:1862-1890); NAME_None keeps the console path.
	void ClientMessageToOwner(const FString& Msg, FName Type = NAME_None) const;

	virtual void PostInitializeComponents() override;  // SetInitialState
	virtual void Tick(float DeltaTime) override;       // PHYS_Rotating pickup spin
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Bind the shipped immutable I_*/M_* HUD pair without loading either asset in
	// the constructor. Artifact leaf CDOs call this with their .uc default names;
	// generated pickup Blueprints inherit the same defaults.
	void BindStatusIcons(const TCHAR* IconName, const TCHAR* FrameName);
	// Directory-aware overload: the citadel resource adapters keep their shipped UI
	// pairs under per-package texture folders (WoTTraps / WoTPawns / WOT); the two-arg
	// form above stays pinned to the angreal folder for its existing callers.
	void BindStatusIcons(const TCHAR* Directory, const TCHAR* IconName,
	                     const TCHAR* FrameName);

	// Resolve the CDO for a shipped ORIGINAL class name ("AngrealInvDart"), accepting
	// the port's WoT-prefixed native name too — the C++ stand-in for the .uc pattern
	// class'Util'.static.LoadClassFromName(ItemName).default (BaseHUD.uc:555,
	// WOTPlayer.uc:1181). Hits are cached by name (HUD-frame hot path); the cached
	// lookup semantics are identical to the original scan.
	static const AWoTInventory* FindDefaultByOriginalName(FName OriginalClassName);

protected:
	// Movement query used by the common retail PHYS_Falling integrator. Ordinary
	// inventory blocks only level geometry; Seal overrides after DropFrom because
	// its shipped SetCollision(true,true,true) is solid to blocking actors.
	virtual bool TraceFallingMovement(const FVector& Start, const FVector& End,
		FHitResult& OutHit) const;
	void EnsureRuntimeVisualPresentation();
	void ApplyRuntimeVisualPresentation();
	UFUNCTION()
	virtual void OnPickupOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                             UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                             bool bFromSweep, const FHitResult& Hit);
	virtual bool ValidTouch(AActor* Other);
	void RespawnTimerExpired();

	// Re-derive visibility/collision/tick from the item state — the PRESENTATION half of
	// GotoItemState, minus the server-only respawn timer. Shared by GotoItemState (authority)
	// and OnRep_ItemState (proxy), so a client reproduces UE1's state-driven rendering.
	void ApplyItemStatePresentation(EWoTInventoryState State);
	UFUNCTION()
	void OnRep_ItemState();

	// Replicated UNCONDITIONALLY (COND_None): world pickups must be visible to ALL clients and
	// held items hidden on all — UE5 does NOT re-hide on the native bHidden bit (unlike UE1),
	// so the OnRep drives presentation explicitly. (The inventory LIST is the separate
	// COND_OwnerOnly channel on UWoTInventoryComponent.)
	UPROPERTY(ReplicatedUsing = OnRep_ItemState)
	EWoTInventoryState ItemState = EWoTInventoryState::Pickup;
	FTimerHandle RespawnTimer;
	FVector FallingVelocity = FVector::ZeroVector;
	bool bFalling = false;
	// Retail enters state Pickup at the Dropped LABEL, bypassing the Begin-label
	// PHYS_Rotating block — a dropped item lies flat where it fell and never
	// spins. Replicated so proxies suppress the client-side pickup spin too.
	UPROPERTY(Replicated)
	bool bDropped = false;
	FWoTRuntimeActorVisualPresentation RuntimeVisualPresentation;
	bool bHasRuntimeVisualPresentation = false;
};
