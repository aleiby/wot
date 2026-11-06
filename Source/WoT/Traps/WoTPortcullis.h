#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../WoTMover.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTUE1Collision.h"
#include "WoTPortcullis.generated.h"

// Port of WoTTraps/PortcullisMover.uc — the iron drop-gate brush (an AWoTMover subclass). INVERTED
// mover semantics: frame 0 = closed/down/blocking (alpha 0), frame 1 = open/raised (alpha 1). It
// STARTS raised (HideMover), DROPS once on trigger (StartClose → the base Tick interpolates 1→0), and
// on DESTRUCTION re-raises (HideMover) + deactivates — the actor is NOT destroyed. Overrides TakeDamage
// (WithstandDamage accounting; the base's bDamageTriggered "open on any hit" is off), so a portcullis
// opens only when its withstand is spent. WithstandDamage replicates for parity but has no client
// consumer (the client sees the gate move via the replicated Phase).
UCLASS()
class AWoTPortcullisMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTPortcullisMover();

	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Trap") int32 WithstandDamage = 120;

	// PortcullisMover.Activate/Deactivate — the owning AWoTPortcullis toggles these (the Inactive vs
	// TriggerToggle state). Activate resets withstand + re-arms the once-trigger + raises the gate.
	void Activate();
	void Deactivate();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;

protected:
	// The Open-family edges are retail-quiet: the portcullis's ONLY raise is HideMover's
	// InterpolateTo(1,0) snap. UE1 InterpolateTo is movement-only (no PlaySound,
	// in the engine Mover); OpeningSound plays solely in DoOpen and OpenedSound solely
	// in FinishedOpening, both reachable only from state-label code the portcullis
	// never runs on a raise (PreBeginPlay/Activate/destroyed-branch call HideMover
	// directly; engine InterpolateEnd plays nothing). The authored
	// OpeningSound/OpenedSound=OpenPT defaults ship unreachable; Closing/Closed play.
	virtual bool ShouldPlayPhaseEdgeSound(EWoTMoverPhase NewPhase) const override
	{
		return NewPhase != EWoTMoverPhase::Opening && NewPhase != EWoTMoverPhase::Open;
	}

private:
	// PortcullisMover.HideMover: InterpolateTo(1,0) — jump to the raised keyframe quietly (Phase=Open).
	void HideMover();
	// Portcullis(Owner).IsInactive(None): the team/editing gate lives on the owning trap.
	bool OwnerInactive() const;

	// The Inactive (false) / TriggerToggle (true) state.
	bool bActive = false;
};

UENUM()
enum class EWoTPortcullisState : uint8
{
	Inactive,
	Active,
};

// Port of WoTTraps/Portcullis.uc — the invisible archway TRIGGER that owns a PortcullisMover. When
// armed (Active), a pawn walking under (Touch) drops the gate (Mover.WoTTrigger). Pre-placed +
// toggled via the citadel editor (SpawnTempResource=False), not drag-deployed. The Mover object-ref
// is NOT replicated (the original's `reliable Mover` is vestigial — no client reads it for gameplay;
// the port resolves an authority-side pointer by tag, per the scoping note).
UCLASS()
class AWoTPortcullis : public AWoTTrap
{
	GENERATED_BODY()

public:
	AWoTPortcullis();

	// The map Tag of the PortcullisMover this trigger owns (Portcullis.Event; the mover's Tag).
	// Resolved in BeginPlay (the original's PreBeginPlay foreach AllActors). Map data — no default.
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") FName MoverTag;

	// ActivateResource/DeactivateResource(bChangeState) — arm/disarm (also toggles the Mover). Public
	// so the spec (and the deferred editor) drive them.
	void Activate();
	void Deactivate();
	bool IsActive() const { return State == EWoTPortcullisState::Active; }
	virtual bool IsTrapDeployed() const override
	{
		return IsActive() && !IsPreviewMode();
	}

	// Active.Touch. Public + UFUNCTION so a spec drives it directly (headless overlaps are awkward).
	UFUNCTION()
	void OnPortcullisOverlap(UPrimitiveComponent* Comp, AActor* Other, UPrimitiveComponent* OtherComp,
	                         int32 BodyIndex, bool bFromSweep, const FHitResult& Hit);

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	AWoTPortcullisMover* GetMover() const { return Mover; }
	void SetTriggerSize(float Radius, float HalfHeight);
	float GetUE1TriggerRadius() const { return UE1CollisionRadius; }
	float GetUE1TriggerHalfHeight() const { return UE1CollisionHalfHeight; }
	class UWoTFlatCylinderComponent* GetCursorTraceCylinder() const
	{
		return CursorTraceCylinder;
	}

protected:
	virtual FName GetCitadelEditorMarkerTexture(
		bool bActive, bool bHighlight) const override;
	// The edit-phase collision split (see AWoTTrap::ApplyEditingCollision), palette-
	// dependent per Portcullis.uc:50-77: while editing, an INACTIVE archway blocks the
	// editor's ECC_Visibility cursor only while its class is the palette selection
	// (Begin/EndSelectResource); an ACTIVE one is always traceable (EndSelectResource's
	// sprite-only hide keeps its collision). Never at play.
	virtual void ApplyEditingCollision(bool bEditingNow) override;

private:
	// The active half of the traceability matrix keys off State, so clients re-derive
	// their local cursor collision when it replicates (the bEditing OnRep pattern).
	UFUNCTION() void OnRep_State();

	UPROPERTY(ReplicatedUsing = OnRep_State)
	EWoTPortcullisState State = EWoTPortcullisState::Inactive;
	UPROPERTY() TObjectPtr<AWoTPortcullisMover> Mover;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<class UWoTFlatCylinderComponent> CursorTraceCylinder;
	float UE1CollisionRadius = 64.0f;
	float UE1CollisionHalfHeight = 160.0f;
	WoTUE1Collision::FTouchTracker UE1Touches;
};

// Citadel-editor placement adapter (PortcullisInventory.uc). Data-only leaf.
UCLASS()
class AWoTPortcullisInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTPortcullisInventory();
};
