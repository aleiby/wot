#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../WoTRetailProjectileTarget.h"
#include "../Inventory/WoTInventory.h"
#include "WoTSpear.generated.h"

class AWoTTrapBlocker;

UENUM()
enum class EWoTSpearPhase : uint8
{
	Waiting,      // armed, retracted — a trigger touch springs it
	Activating,   // thrusting out — the damage window
	Extended,     // held out
	Reseting,     // retracting
};

// Port of WoTTraps/Spear.uc — an impale trap. A pawn approaching the trigger springs the spear, which
// thrusts out (Activating), impales anyone in reach (DamageAmount + a strong momentum shove away),
// holds, then retracts. Uses 4 AWoTTrapBlockers spaced along ImpaleRange: the first always collides
// (the trigger), the last always damages (the tip). The blockers forward their overlaps to this trap
// (non-damage → TrapTouch, damage → DamageActor); the Spear's response is phase-dependent. The latent
// anim (PlayAnim/FinishAnim) + SetTimer dwell become FTimerManager step continuations;
// the Phase replicates and drives the extend/retract animation on every machine.
UCLASS()
class AWoTSpear : public AWoTTrap, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTSpear();

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> VisualMesh;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") float ImpaleRange = 64.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") float TriggerRadius = 32.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") float DamageRadius = 4.0f;
	// Spear.MomentumConstant (20000): the pre-Mass-divide knockback passed through
	// FWoTDamageEvent to the shared Pawn.TakeDamage parity fold.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") int32 MomentumConstant = 20000;

	EWoTSpearPhase GetPhase() const { return Phase; }
	int32 GetBlockerCountForTest() const { return Blockers.Num(); }
	bool HasActivePhaseTimerForTest() const;

	// TrapBlocker forwards: the trigger blocker relays TrapTouch; the tip blocker relays DamageActor.
	virtual void TrapTouch(AActor* Other) override;
	virtual void DamageActor(AActor* Other) override;
	virtual bool DeployResource(FVector Loc, FVector Normal) override;
	virtual bool RemoveResource() override;
	virtual void BeginEditingResource(uint8 PlacedByTeam) override;
	virtual void EndEditingResource() override;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// The edit-phase collision split (see AWoTTrap::ApplyEditingCollision): the manager is
	// cursor-traceable while editing, collisionless at play.
	virtual void ApplyEditingCollision(bool bEditingNow) override;

private:
	// Spear.InitBlockers / SetBlockers: spawn 4 blockers along the reach; first always collides, last
	// always damages.
	void InitBlockers();
	void RebuildBlockersAtCurrentTransform();
	void DestroyBlockers();
	void SetBlockers(bool bCollide, bool bBlock, bool bDamage, float NewRadius, float NewHeight);
	// Spear.HideBlockers (Spear.uc:141-149): ALL blockers (including the always-collide
	// first/last of SetBlockers) drop collision for the whole edit phase.
	void HideBlockers();

	void TryActivate(AActor* Other);   // Waiting.Touch: spring on a valid enemy pawn
	void Impale(AActor* Other);        // Activating.DamageActor: DamageAmount + momentum shove
	void EnterActivating(AActor* Activator);
	void EnterExtended();
	void EnterReseting();
	void EnterWaiting();

	// Dwell timings (Spear's SetTimer(2)/SetTimer(1)). The thrust anim rate is fitted to the
	// ThrustTime sim window rather than FinishAnim-length (the noted divergence at
	// ApplyPhasePresentation).
	static constexpr float ThrustTime = 0.5f;
	static constexpr float ExtendedTime = 2.0f;
	// Reseting includes TweenAnim('Activating', .25) / FinishAnim plus Timer(1).
	static constexpr float ResetTime = 1.25f;

	UPROPERTY(ReplicatedUsing = OnRep_Phase) EWoTSpearPhase Phase = EWoTSpearPhase::Waiting;
	UFUNCTION()
	void OnRep_Phase() { ApplyPhasePresentation(); }

	// The spear mesh (Spear.uc: 'WoTTraps.Spear' (meshmap scale baked; DrawScale 1.0); Waiting=f0,
	// Activating=f1-19). Presentation follows the replicated Phase on every machine;
	// the thrust anim rate fits the port's ThrustTime sim window (the shipped state
	// ran FinishAnim-length — noted divergence; 6b).
	void ApplyPhasePresentation();

	UPROPERTY() TArray<TObjectPtr<AWoTTrapBlocker>> Blockers;
	FTimerHandle PhaseTimer;

	// One impale per victim per activation cycle; cleared on EnterActivating.
	TSet<TWeakObjectPtr<APawn>> ImpaledThisCycle;
	bool bEnablingActivationBlockers = false;
	TMap<TWeakObjectPtr<AActor>, FVector> ActivationOccupants;
};

// Citadel-editor placement adapter (SpearInventory.uc). Data-only leaf.
UCLASS()
class AWoTSpearInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTSpearInventory();
};
