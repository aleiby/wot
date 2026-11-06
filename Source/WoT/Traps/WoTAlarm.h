#pragma once

#include "CoreMinimal.h"
#include "WoTTrap.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTRetailProjectileTarget.h"
#include "WoTAlarm.generated.h"

// Port of WoTTraps/Alarm.uc — the citadel alarm gong. A Captain with SoundAlarm orders navigates
// to it and rings it (ActivateAlarm), fanning AlarmSounded out to every NPC within SoundRadius*16;
// a PLAYER can also ring it by touching or damaging it (the Waiting-state Touch/Bump/TakeDamage
// handlers, Alarm.uc:58-84). Ringing enters the Sound window (Alarm.uc:90-94): the gong plays
// (WoTTraps/ActivateAM), Bump re-rings are disabled for AlarmDelay seconds, then re-enabled; fresh
// Touches and TakeDamage stay live throughout (the .uc Disables only 'Bump'). Derives from
// AWoTTrap (the shipped parent); the editor placement half
// (the vertical-surface CalcLocation gate + DeployResource, Alarm.uc:28-48) is implemented by
// the citadel create path. No TrapType — the Alarm is not in the TrapDetect table
// (TrapDetectLeech.uc:286-291).
//
// Divergences from the .uc (PORT_NOTES): (1) the shipped fan-out is Util.TriggerRadiusInstances
// (an engine radius-trigger helper); folded here to a direct radius scan calling each NPC
// controller's AlarmSounded. (2) The grunt on-arrival reads Alarm.Target — the inherited UE1
// Actor.Target, which nothing in the alarm path ever writes (Grunt.uc:1952) — while the Captain
// writes Alarm.ThreatActor/ThreatLocation (a latent .uc mismatch; the shipped threat handoff was
// dead). This port carries the captain-written ThreatActor/ThreatLocation so the threat actually
// propagates. (3) UE1 Bump (blocking contact) has no analog on a query-only capsule — the Bump
// seam exists and honors the Sound window's Disable('Bump'), but overlap entry drives Touch.
UCLASS()
class AWoTAlarm : public AWoTTrap, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	// The gong mesh (Alarm.uc:106-110: DT_Mesh 'WoTTraps.Alarm' (meshmap scale baked; DrawScale 1.0); the
	// 'Waiting' loop is a single frame — a static bind; 6b).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> VisualMesh;
	virtual void BeginPlay() override;

public:
	AWoTAlarm();
	virtual bool CalcLocation(FVector& Loc, FVector& Normal) override;
	virtual bool DeployResource(FVector Loc, FVector Normal) override;

	// Set externally by the ringing Captain before ActivateAlarm (Alarm.uc:20-21).
	// Grunts that navigate to the rung alarm adopt this as their threat.
	UPROPERTY()
	TWeakObjectPtr<AActor> ThreatActor;

	UPROPERTY()
	FVector ThreatLocation = FVector::ZeroVector;

	// Alarm.uc:101,109 CDO. The fan-out reaches pawns within SoundRadius*16 (Alarm.uc:54);
	// AlarmDelay gates Bump re-rings while the gong sounds (Alarm.uc:91-94).
	UPROPERTY(EditAnywhere, Category = "WoT|Alarm")
	int32 AlarmDelay = 3;

	UPROPERTY(EditAnywhere, Category = "WoT|Alarm")
	float SoundRadius = 255.0f;

	// True once rung (the Sound label). GetClosestAlarm treats a rung alarm as no different
	// structurally; the ladder just re-picks it.
	UPROPERTY()
	bool bRung = false;

	// Alarm.uc:52 ActivateAlarm: ring — fan AlarmSounded out to every NPC within SoundRadius*16
	// (excluding the ringer), then enter the Sound window. Ringer may be null (damage/bump ring).
	void ActivateAlarm(AActor* Ringer);

	// Waiting.Touch (Alarm.uc:68-84): only PLAYERS ring by touch (a Captain rings via
	// ActivateAlarm). The gate reads IsInactive(None) — deliberately not the toucher, so a player
	// of ANY team rings it ("ring for everyone"); only the editing gate applies.
	void Touch(AActor* Other);

	// Waiting.Bump → Touch (Alarm.uc:63-66); disabled during the Sound window.
	void Bump(AActor* Other);

	// Waiting.TakeDamage → Touch(InstigatedBy) (Alarm.uc:58-61): shooting the alarm rings it (the
	// bProjTarget=True path). The alarm itself is indestructible — no damage is applied.
	virtual float TakeDamage(float Damage, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;

	virtual void NotifyActorBeginOverlap(AActor* Other) override;

	bool IsBumpEnabled() const { return bBumpEnabled; }
	bool HasActiveSoundTimerForTest() const;

protected:
	// The edit-phase collision split (see AWoTTrap::ApplyEditingCollision): the gong blocks
	// the editor's ECC_Visibility cursor while editing, drops the block at play.
	virtual void ApplyEditingCollision(bool bEditingNow) override;

private:
	// The Sound label (Alarm.uc:90-94) as a named-step continuation: Disable('Bump'), the
	// ActivateAM gong, Sleep(AlarmDelay), Enable('Bump'). A re-ring mid-window restarts the
	// sleep (the .uc GotoState re-runs the label).
	void EnterSoundWindow();
	void ReenableBump();

	bool bBumpEnabled = true;
	FTimerHandle SoundTimer;
};

// Citadel-editor placement adapter (AlarmInventory.uc). Data-only leaf.
UCLASS()
class AWoTAlarmInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTAlarmInventory();
};
