#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "WoTIceLeech.generated.h"

// Port of Angreal.IceLeech (expands Leech) — the freeze effect installed by
// AngrealIceProjectile. Spec: exports/script/Angreal/IceLeech.uc.
//
// Simulation (parity-locked): while attached the host is immobilized — movement
// disabled and its location pinned to where it was frozen (OriginalLocation),
// every tick, exactly as the original's `Owner.SetLocation(OriginalLocation)`.
// Telefrag guard: the host stops blocking other pawns (UE1 SetCollision(
// true,false,false)), while a separate 40x48 ice cylinder blocks pawns and
// projectiles. Lifespan (set from the projectile's Duration=7s) runs down in the
// base UWoTLeech; on expiry Expired()->Unattach() restores movement + collision.
//
// The accepted replicated component owns the local Frozen shell. That keeps
// presentation in lockstep with leech acceptance/replacement/cleanse: a blocked
// install never creates one, and Unattach always retires the one it owns.
UCLASS()
class UWoTIceLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTIceLeech();

	// Where the victim stood when frozen; the host is pinned here each tick.
	UPROPERTY(Replicated) FVector OriginalLocation = FVector::ZeroVector;

	// How long the melt visual lasts at the end of the freeze (presentation).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Ice") float MeltTime = 2.0f;

	virtual void AttachTo(APawn* NewHost) override;
	virtual void OnHostDetached(APawn* OldHost) override;
	virtual void Unattach() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Local-only presentation owned by this accepted replicated leech instance.
	// Public so the lifecycle specs can assert the production object directly.
	UPROPERTY(Transient) TObjectPtr<class AWoTMeshEffect> FrozenShell;
	// Runtime equivalent of IceLeech's blocking Ice actor (40 radius, 48 height).
	// Local to every role, just like the visual shell; the authority copy owns
	// gameplay collision.
	UPROPERTY(Transient) TObjectPtr<class AWoTCollisionBlocker> IceBlock;

	// Possession is not stable across a seven-second freeze. Rebind the live
	// suspension to the pawn's current controller without losing pre-freeze
	// Dormant provenance.
	void RefreshNPCControllerSuspension();

private:
	// Saved host state, restored on Unattach.
	bool bFrozeHost = false;
	TEnumAsByte<EMovementMode> SavedMovementMode = MOVE_Walking;
	uint8 SavedCustomMovementMode = 0;
	ECollisionResponse SavedPawnResponse = ECR_Block;
	UPROPERTY(Transient) TWeakObjectPtr<class AWoTNPCController> SuspendedController;
	bool bHostWasDormantBeforeFreeze = false;

	void FreezeHost(class APawn* Host);
	void ThawHost(class APawn* Host);
	void EnsureFrozenPresentation();
	void RetireFrozenPresentation();
	void EnsureIceBlock();
	void RetireIceBlock();
};
