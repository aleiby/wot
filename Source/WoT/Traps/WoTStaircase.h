#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTUE1Collision.h"
#include "WoTStaircase.generated.h"

class AWoTMover;

UENUM()
enum class EWoTStaircaseState : uint8
{
	Inactive,
	Active,
};

// Port of WoTTraps/Staircase.uc — an invisible trap TRIGGER (NOT a mover itself) that, once armed
// (Active), fires a map Mover (the tilting stairs) on touch to stumble the victim down the flattened
// steps, then re-arms after the mover's StayOpenTime. Pre-placed + toggled via the citadel editor
// (SpawnTempResource=False), not drag-deployed. The displacement/damage is entirely the Mover's
// (encroachment) — the staircase deals none. Reuses the ported AWoTMover as-is.
UCLASS()
class AWoTStaircase : public AWoTTrap
{
	GENERATED_BODY()

public:
	AWoTStaircase();

	// The map Tag of the Mover this staircase fires (Staircase.Event / the mover's Tag). Resolved to
	// TheStaircase in BeginPlay by iterating AWoTMover (the original's PreBeginPlay foreach AllActors).
	UPROPERTY(EditAnywhere, Category = "WoT|Trap") FName MoverTag = TEXT("StaircaseX");

	// ActivateResource/DeactivateResource(bChangeState) — the arm/disarm the citadel editor drives.
	// Only an Active staircase fires. Public so the spec (and the deferred editor) drive them.
	void Activate();
	void Deactivate();
	bool IsActive() const { return State == EWoTStaircaseState::Active; }
	virtual bool IsTrapDeployed() const override
	{
		return IsActive() && !IsPreviewMode();
	}

	// Active.Touch. Public + UFUNCTION so a spec drives it directly (a headless overlap is awkward
	// to synthesize, as with AWoTTrigger::OnOverlap).
	UFUNCTION()
	void OnStaircaseOverlap(UPrimitiveComponent* Comp, AActor* Other, UPrimitiveComponent* OtherComp,
	                        int32 BodyIndex, bool bFromSweep, const FHitResult& Hit);

	virtual void BeginPlay() override;
	void SetTriggerSize(float Radius, float HalfHeight);
	float GetUE1TriggerRadius() const { return UE1CollisionRadius; }
	float GetUE1TriggerHalfHeight() const { return UE1CollisionHalfHeight; }
	class UWoTFlatCylinderComponent* GetCursorTraceCylinder() const
	{
		return CursorTraceCylinder;
	}
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual FName GetCitadelEditorMarkerTexture(
		bool bActive, bool bHighlight) const override;
	// The edit-phase collision split (see AWoTTrap::ApplyEditingCollision), palette-
	// dependent per Staircase.uc:40-66: while editing, an INACTIVE trigger blocks the
	// editor's ECC_Visibility cursor only while its class is the palette selection
	// (Begin/EndSelectResource); an ACTIVE one is always traceable (EndSelectResource's
	// sprite-only hide keeps its collision). Never at play.
	virtual void ApplyEditingCollision(bool bEditingNow) override;

private:
	// Active.Timer: re-enable Touch after StayOpenTime (a re-arm cooldown).
	void ReArm();
	// The active half of the traceability matrix keys off State, so clients re-derive
	// their local cursor collision when it replicates (the bEditing OnRep pattern).
	UFUNCTION() void OnRep_State();

	UPROPERTY(ReplicatedUsing = OnRep_State)
	EWoTStaircaseState State = EWoTStaircaseState::Inactive;
	// The Disable/Enable('Touch') latch inside the Active state — fires once, re-arms after the dwell.
	bool bArmed = false;
	UPROPERTY() TObjectPtr<AWoTMover> TheStaircase;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<class UWoTFlatCylinderComponent> CursorTraceCylinder;
	FTimerHandle ReArmTimer;
	float UE1CollisionRadius = 32.0f;
	float UE1CollisionHalfHeight = 32.0f;
	WoTUE1Collision::FTouchTracker UE1Touches;
};

// Citadel-editor placement adapter (StaircaseInventory.uc). Data-only leaf.
UCLASS()
class AWoTStaircaseInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTStaircaseInventory();
};
