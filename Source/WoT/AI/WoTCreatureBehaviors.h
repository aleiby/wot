#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTCreatureBehaviors.generated.h"

class AWoTNPCCharacter;
class AWoTNPCController;
class UWoTBrainComponent;
class UWoTShieldReturnToSenderReflector;
class UWoTDecreaseHealthLeech;
class UWoTWeaponComponent;
class AWoTProjectile;
class AWoTMyrddraalSwordAngreal;

// The shared active-detector offender scan — DefensiveDetector's native
// CheckForOffending folded to a poll (DefensiveDetector.uc + LegendPawn.uc:1349-1362):
// the nearest AWoTProjectile inside CollectionRadius, within OwnerPawn's FOV cone
// (FovCos), moving toward the pawn, whose straight-line arrival time falls in
// [MinTime,MaxTime]. Two families use it with different rejections: the ShieldDeflect
// hook passes bRejectWeaponProjectiles/bRejectSeekers true (arrows + seekers are
// un-deflectable); the dodge chain passes both false — it collects the WHOLE
// WotProjectile family (CollectClass WotProjectile minus Engine.Fragment; no Fragment
// analog exists in the port, so that rejection is a no-op — PORT_NOTES).
namespace WoTDefensiveScan
{
	AWoTProjectile* FindIncomingOffender(const APawn* OwnerPawn, float CollectionRadius,
		float FovCos, float MinTime, float MaxTime,
		bool bRejectWeaponProjectiles, bool bRejectSeekers);
}

// The per-creature behavior hook base (NPC/AI slice 5b). Each MP creature leaf
// is an FWoTCreatureRow whose BehaviorClass identity binds to a concrete strategy
// component on the pawn, created by ConfigureNPC via
// Create(), and DRIVEN by the possessing controller each frame.
//
// Why controller-driven rather than self-ticking: all three hooks read the current
// brain state (shield-up posture, watched-freeze, moving-vs-still fade), so they must
// run AFTER the controller updates its FSM in the same frame. The controller pumps
// TickBehavior() at the end of its Tick, giving deterministic ordering and clean
// access to GetController()/GetBrain() — the only extension points these hooks need.
UCLASS(Abstract)
class UWoTCreatureBehavior : public UActorComponent
{
	GENERATED_BODY()

public:
	// The row's registered BehaviorClass name -> a concrete behavior on NPC. Unknown
	// names fail closed with no behavior component.
	static UWoTCreatureBehavior* Create(AWoTNPCCharacter& NPC, FName BehaviorName);

	// Controller lifecycle (from AWoTNPCController::OnPossess / OnUnPossess).
	virtual void OnPossessedBy(AWoTNPCController* Controller);
	virtual void OnUnpossessed() {}

	// Driven by the controller each frame, after its state update.
	virtual void TickBehavior(float DeltaSeconds) {}

	// Melee-damage scale the controller applies per swing (Warder conscription 2x).
	// Non-const: the shipped check IS a live FindLeader() per swing, side effects
	// included (Warder.uc:230 → Grunt.uc:1143 copies/invalidates the guard goal).
	virtual float GetMeleeDamageMultiplier() { return 1.0f; }

	// A body-melee swing just LANDED on Victim (MeleeDamageGoal succeeded). The
	// Myrddraal rides this to apply its delayed-damage leech + teleport away
	// (SetupDelayedDamageToGoal, Myrddraal.uc:554). Default no-op.
	virtual void OnMeleeHit(AActor* Victim) {}

	// The host pawn just took Damage (raw incoming, pre-reduction — the value the
	// original's TakeDamage sees). The BATrolloc rides this for its recovery stagger
	// (BATrolloc.uc:83). Default no-op. Called from AWoTNPCCharacter::TakeDamage.
	virtual void NotifyTookDamage(int32 Damage) {}

protected:
	AWoTNPCCharacter* GetNPC() const;
	AWoTNPCController* GetController() const;
	UWoTBrainComponent* GetBrain() const;

	TWeakObjectPtr<AWoTNPCController> OwningController;
};

// ---------------------------------------------------------------------------
// Hook 1 — ShieldDeflect (Soldier + Questioner; exports/script/WoTPawns/ShieldUser.uc)
// ---------------------------------------------------------------------------
// Raises the shield's ReturnToSender reflector in response to an incoming deflectable
// weave, and drops it (deferred) when the perimeter clears / immediately when the pawn
// commits to an attack. Reproduces EnableDeflect/DisableDeflect (uc:91/110),
// UseShield/DontUseShield (uc:168/155) and Tick's deferred uninstall (uc:56-60).
//
// Trigger (slice 5b batch 4): the ACTIVE DefensiveDetectorSoldier/Questioner
// (bActiveDetection, DefensiveDetectorSoldier.uc). PendingOffenderCollision → UseShield
// is the ONLY UseShield caller (ShieldUser.uc:137-145), so the shield is purely
// reactive to detected incoming fire — NOT raised merely from seeing an enemy (that was
// the earlier FSM proxy). CollectClass WotProjectile (our AWoTProjectile base) within
// CollectionRadius 1024, FOV-gated, rejecting the un-deflectable WotWeaponProjectile
// (arrows) always and SeekingProjectile for the Soldier only (Questioner keeps its
// seeker rejection off — DefensiveDetectorQuestioner.uc omits ExplicitClassRejections(2)),
// then the moving-toward + arrival-window [0.05,1.0] test.
UCLASS()
class UWoTShieldDeflectBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnPossessedBy(AWoTNPCController* Controller) override;
	virtual void OnUnpossessed() override;
	virtual void TickBehavior(float DeltaSeconds) override;
	// ShieldUser.uc:91 EnableDeflect / :110 DisableDeflect — install/remove the shield
	// reflector on the host, select the shield weapon, ForceStationary + interrupt
	// movement (item 5), and stamp the crouch/walk tweens (item 6).
	void EnableDeflect();
	void DisableDeflect();
	bool IsDeflectEnabled() const { return bDeflectEnabled; }

	// The shield weapon exists in hand and is not thrown/in-flight — the port's fold of
	// UseShield's ShieldReflectorInventoryProxy gate (ShieldUser.uc:143,173) plus the
	// DeleteInventory/in-flight drop (uc:65-74). False -> the pawn dodges instead (item 4).
	bool HasShieldAvailable() const;

	// ShieldUser.uc:21 UnistallShieldReflectorDelay (Soldier/Questioner CDO 0.5).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float UninstallShieldReflectorDelay = 0.5f;

	// DefensiveDetector CDO (base CollectionRadius 1024; DefensiveResponses(0) window
	// 0.05..1.0 — DefensiveDetectorSoldier.uc).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float CollectionRadius = 1024.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float ResponseMinTime = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float ResponseMaxTime = 1.0f;

	// Soldier rejects incoming seekers (can't deflect them); Questioner does not
	// (ConfigureNPC clears this for the Questioner row).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	bool bRejectSeekers = true;

	// The per-creature reflector config copied onto the installed reflector
	// (ShieldUser.PostBeginPlay, uc:42-46). Soldier: bShieldReflectorDeflect=True
	// (Soldier.uc:50) + ShieldReflectorSoundName (Soldier.uc:48). Questioner: lead.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	bool bReflectorDeflect = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	bool bReflectorLeadWhenReflecting = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	FName ReflectSoundOverride;

private:
	// The active detector poll: the nearest deflectable incoming projectile inside the
	// collection sphere + FOV that will arrive within the response window, else null.
	class AWoTProjectile* DetectIncoming() const;
	bool WantsShield() const;
	bool InAttackState() const;
	// The shield weapon actor (the melee slot for both creatures). Bound at possess so
	// its OnWeaponDestroyed fires DisableDeflect immediately (DeleteInventory, uc:65-74).
	class AWoTNPCWeapon* GetShieldWeapon() const;
	void OnShieldWeaponDestroyed(class AWoTNPCWeapon* Weapon);

	TWeakObjectPtr<UWoTShieldReturnToSenderReflector> ShieldReflector;
	TWeakObjectPtr<class AWoTNPCWeapon> BoundShieldWeapon;
	bool bDeflectEnabled = false;
	bool bUninstallPending = false;
	float UninstallTimer = 0.0f;
};

// ---------------------------------------------------------------------------
// Hook 2 — WarderFadeCloak (exports/script/WoTPawns/Warder.uc)
// ---------------------------------------------------------------------------
// The camouflage fade: still (Waiting/guarding) -> fade out toward MinVisibility;
// active (Tracking/attacking) -> fade in toward the full default Visibility. Drives
// the pawn's Visibility byte, which the sight model (WoTSight) reads to shrink an
// observer's detection range. Reproduces SetFadeLevel (uc:175-194) + OnFadeNotification
// integration (uc:198-226) + the WaitingIdle EnableFade/DisableFade drive (uc:396-406).
UCLASS()
class UWoTWarderFadeCloakBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnPossessedBy(AWoTNPCController* Controller) override;
	virtual void OnUnpossessed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickBehavior(float DeltaSeconds) override;
	virtual float GetMeleeDamageMultiplier() override;
	// SetFadeLevel (uc:175): clamp the level, re-derive Visibility, and select the
	// authored camouflage skin. Public so presentation specs can drive exact levels.
	void SetFadeLevel(float NewFadeLevel);
	void IncreaseVisibilitySlow();
	void DecreaseVisibilitySlow();
	float GetFadeLevel() const { return FadeLevel; }
	float GetActiveFadeChangeTime() const
	{
		return bFadeOut ? CurrentFadeOutChangeTime : CurrentFadeInChangeTime;
	}

	// Warder.uc defaultproperties (:456-474).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float FadeLevels = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float MinVisibility = 16.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float FadeInChangeTime = 1.0f;    // fully faded -> fully visible
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float FadeOutChangeTime = 2.0f;   // fully visible -> fully faded
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float FadeInSlowChangeTime = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float FadeOutSlowChangeTime = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float MinScaleGlow = 0.04f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float MaxScaleGlow = 1.0f;
	// GetStrengthMultiplier (uc:230): 2x melee when conscripted by a guarding Sister.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fade")
	float ConscriptedStrengthMultiplier = 2.0f;

private:
	// default.Visibility — the pawn's fully-visible byte (captured at possess, 128).
	float DefaultVisibility = 128.0f;
	// FadeLevel starts fully visible (PreBeginPlay, uc:108).
	float FadeLevel = 1000.0f;
	void BeginFadeIn(bool bSlow);
	void BeginFadeOut(bool bSlow);
	void StartWaitingIdleAnim();
	void TickWaitingIdleAnim(float DeltaSeconds);
	void RestoreWaitingSoundVolume();
	bool bFadeOut = false;
	bool bFadeNotifierEnabled = false;
	bool bWaitingSoundDucked = false;
	float SavedWaitingSoundVolumeMultiplier = 1.0f;
	float CurrentFadeInChangeTime = 1.0f;
	float CurrentFadeOutChangeTime = 2.0f;
	uint8 LastBrainState = 255;
	bool bLastWaitingCower = false;
	float WaitingAnimElapsed = 0.0f;
	float WaitingAnimDuration = 0.0f;
	float WaitingFadeInNotifyTime = -1.0f;
	float WaitingFadeOutNotifyTime = -1.0f;
};

// ---------------------------------------------------------------------------
// Hook 3 — MinionStealth (exports/script/WoTPawns/Minion.uc, behavior half)
// ---------------------------------------------------------------------------
// The freeze-when-watched: while the enemy the Minion is threatening is LOOKING at
// it (the enemy's FOV cone, HoldingAngleCos), the Minion holds still and withholds
// its attack. Reproduces GetPriorityGoalIndex's ForceStationary/UnForceStationary
// (uc:167-182) and the watch test PawnCanSeeActor(threat, self, HoldingAngleCos)
// (uc:171-174) via WoTSight::PawnCanSeeActor. The blinking eyes (presentation) and the
// tentacle-grab special are out of scope (flagged in PORT_NOTES).
UCLASS()
class UWoTMinionStealthBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnUnpossessed() override;
	virtual void TickBehavior(float DeltaSeconds) override;
	bool IsFrozen() const { return bFrozen; }

	// bStealthAttacks is cleared the moment the Minion commits to (or is forced out of)
	// stealth: on taking damage (already ticked), on dodging (PerformDefensiveDodge,
	// Minion.uc:188), on starting any attack (PerformAttack, Minion.uc:291), and when the
	// threat enters tentacle range (InUseTentacleDistance, Minion.uc:212). The controller
	// calls this at the dodge/attack seams it drives (G21 item 10).
	void ClearStealthAttacks() { bStealthAttacks = false; }

	// InUseTentacleDistance (Minion.uc:202-216): the threat's cylinder gap is in the
	// tentacle-grab band [Min,Max]. The shipped side effect (clearing bStealthAttacks) is
	// applied by the caller (the attack selector already ran ClearStealthAttacks).
	bool InUseTentacleDistance(float GoalDistance) const
	{
		return GoalDistance >= MinTentacleAttackDistance &&
		       GoalDistance <= MaxTentacleAttackDistance;
	}

	// TentacleGrabAttackDamage1 draw-in (Minion.uc:417-441): only a PLAYER victim is
	// reeled toward the Minion (NPC victims take the grab damage but no draw-in — retail
	// PlayerPawn-only, PORT_NOTES). Zero the victim's velocity/accel, then
	// AddVelocity(DrawInVelocity * Normal(MinionLoc - VictimLoc) / VictimMass *
	// GrabDistance / GrabDistanceDivisor). Authority mirrors the impulse and
	// ShakeView to the affected player's owning prediction copy.
	void ApplyGrabDrawIn(AActor* Victim, float GrabDistance);

	// TentacleGrabAttackDamage2 (Minion.uc:443-464): the @0.50 "nail him again" is dead
	// (no damage — the comment is aspirational) and the walking/swimming GotoState restore
	// is inert in the port (the victim is mid-flight from the draw-in, so retail's
	// Physics==Walking test never fires). Kept for episode-shape parity (PORT_NOTES).
	void ReleaseGrabVictim(AActor* Victim);

	// Minion.uc:61 HoldingAngleCos — the watcher's cone dot threshold.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float HoldingAngleCos = 0.65f;

	// Minion.uc:66-70 + CDO (:478-481): the tentacle-grab band and draw-in tuning. Carried
	// on the behavior (the closest existing pattern — HoldingAngleCos/MaxStareTime live
	// here too); the controller's attack selector reads them.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float MinTentacleAttackDistance = 40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float MaxTentacleAttackDistance = 200.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float DrawInVelocity = 100000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float GrabDistanceDivisor = 55.0f;

	// The stare latch (Minion.uc:60-63 + CDO :474-476) — SHIPPED-FAITHFUL, which
	// means effectively inert: bStealthAttacks arms the freeze, but the stare
	// window is MaxStareTime = 0.000001s from the FIRST threat, expires the next
	// tick (:148-150, armed once at :159-161, never re-armed), and any damage
	// clears it for good (:188/:196). So a retail Minion freezes for at most one
	// frame — the same disabled-behavior family as morale/idle-heal (PORT_NOTES);
	// the mechanism is ported verbatim and a data change re-enables it.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	bool bStealthAttacks = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stealth")
	float MaxStareTime = 0.000001f;

private:
	// The watch test: the Threat goal is an enemy Pawn whose FOV cone sees the Minion.
	bool IsWatchedByThreat() const;
	void SetFrozen(bool bNewFrozen);

	bool bFrozen = false;
	float StareTimeThreshold = 0.0f;
	int32 LastSeenHealth = -1;
};

// ---------------------------------------------------------------------------
// Hook 4 — MyrddraalTeleport (exports/script/WoTPawns/Myrddraal.uc)
// ---------------------------------------------------------------------------
// The blink, ported as a true two-phase episode (Myrddraal.uc:216-527). One of
// four triggers converges on PerformTeleportation (uc:216):
//   1. a landed melee-weapon swing (SetupDelayedDamageToGoal → PerformTeleport
//      ation, uc:557), which ALSO drops a DecreaseHealthLeech on the victim
//      (a 5s decaying health drain, uc:567-576);
//   2. a seeker breaching the passive DEFENSIVE PERIMETER (DefensivePerimiter
//      CompromisedByOffender, uc:121-129) — a SeekingProjectile whose Destination
//      is this pawn, folded to the tick-poll below;
//   3. the ACTIVE DefensiveDetectorMyrddraal window (Myrddraal.uc:618 → the
//      detector CDO: SeekingProjectiles Destination==self, radius 2048, FOV-gated,
//      arrival window [1.0,7.5]s) — driven by the controller's Defensive pulse;
//   4. MoveAwayFromTakeDamageArea's escape fallback (uc:133-143) — DEFERRED, no
//      MoveAwayFromTakeDamageArea analog exists in the controller yet (PORT_NOTES).
// PerformTeleportation picks the closest DARK navigable AWoTPathNode within the
// collection radius (uc:229-306) and the pawn WALKS to it (an Intermediate-goal
// beacon, phase 1); on arrival the controller runs the blink (state
// EWoTBrainState::Teleporting, the TeleporterReached fold, uc:359-527: invulnerable,
// crank-up, vanish, relocate, reappear facing the threat, paired particle sprayers,
// and the body/weapon scale-glow fade.
UCLASS()
class UWoTMyrddraalTeleportBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnPossessedBy(AWoTNPCController* Controller) override;
	virtual void OnUnpossessed() override;
	virtual void TickBehavior(float DeltaSeconds) override;
	virtual void OnMeleeHit(AActor* Victim) override;

	// Myrddraal.uc defaultproperties (:583-594).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float TeleportPositionCollectionRadius = 1600.0f;   // node collection radius
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float DelayedDamageRate = 1.0f;                     // leech AffectResolution
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float DelayedDamageRateScale = 1.2f;               // leech AffectResolutionScale
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float DelayedDamageLifeSpanSecs = 5.0f;            // leech Lifespan
	// TeleportEffectCrankupDuration / TeleportDuration (uc:593-594): the blink's
	// crank-up (2.0), hidden (2.0), and exit crank-up (2.0) segments — the controller
	// Teleporting state reads these to time the episode.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float TeleportEffectCrankupDuration = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float TeleportDuration = 2.0f;

	// The active DefensiveDetectorMyrddraal (Myrddraal.uc:618; the detector CDO):
	// CollectionRadius 2048, FOV-restricted, DefensiveResponses(0) arrival window
	// [1.0,7.5]s before the MyrddraalTeleport hint fires.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float ActiveDetectorRadius = 2048.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float ResponseMinTime = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float ResponseMaxTime = 7.5f;
	// DefensivePerimeterRadiusFactor (DefensiveDetector.uc:278): the passive
	// perimeter proxy is 1.5x the collision cylinder — a seeker inside it has
	// breached (trigger 2).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Teleport")
	float DefensivePerimeterRadiusFactor = 1.5f;

	// FindTeleportPosition (uc:229-306): the closest DARK reachable AWoTPathNode
	// within the collection radius of the anchor (threat-if-a-pawn else self,
	// uc:431-434), excluding LastTeleportPosition. No qualifying node fails the
	// episode exactly as retail. Public so BeginTeleportEpisode + specs drive it.
	// Non-const: stamps LastTeleportPosition on the chosen node at selection (uc:283),
	// so a beacon dropped before arrival is excluded on the immediate retry.
	bool FindTeleportPosition(FVector& OutDestination);

#if WITH_AUTOMATION_TESTS
	// Reachability spec seam (the AWoTNPCController::DodgeBlockedDirsForTest precedent):
	// a headless world builds no navmesh, so a spec marks specific node positions
	// unreachable to exercise the real "closest reachable, else next-closest" pick.
	TArray<FVector> UnreachableNodesForTest;
#endif

	// The controller sets the chosen phase-1 destination here, then the pawn walks
	// to it; CommitReappear relocates the pawn there at the exit (phase 2).
	void SetPendingDestination(const FVector& Destination)
	{
		PendingDestination = Destination;
		bHasPendingDestination = true;
	}
	FVector GetPendingDestination() const { return PendingDestination; }

	// ExitMyrddraalTeleporting (uc:508-522): land at the chosen node, record it as
	// LastTeleportPosition, count the blink, and face the threat. Driven by the
	// controller Teleporting state at the reappear step.
	void CommitReappear();

	// DestroySeekers (uc:465-478): drop the destination of every seeker homing on us
	// (so the blink shakes them) PLUS ShiftOutOfLeechesAndReflectors(Self)
	// (WOTUtil.uc:119-173) — shed removable/deleterious/non-projectile leeches and
	// reflectors. Driven by the controller at the vanish step.
	void DestroySeekers();

	// The active DefensiveDetectorMyrddraal poll (trigger 3): a seeker whose
	// Destination is this pawn, within ActiveDetectorRadius + FOV, arriving inside
	// the response window. Driven by the controller's Defensive pulse for this row.
	bool DetectActiveTeleportOffender() const;

	int32 GetTeleportCount() const { return TeleportCount; }
	AWoTMyrddraalSwordAngreal* GetSourceAngreal() const { return SourceAngreal.Get(); }

private:
	// ClearSeekersTargetingSelf (uc:469-476): SetDestination(None) on every seeker
	// whose Destination is this pawn.
	void ClearSeekersTargetingSelf();
	// ShiftOutOfLeechesAndReflectors (WOTUtil.uc:119-173): shed removable, bad,
	// non-projectile leeches and reflectors from the pawn.
	void ShiftOutOfLeechesAndReflectors();

	// ActorReachable / FindPathToward (uc:274-276): a real navmesh path test from the
	// pawn to a candidate node, with a projection pre-filter. No nav system (headless):
	// accept-all, save for the UnreachableNodesForTest spec seam.
	bool IsTeleportNodeReachable(const AWoTNPCCharacter* Self,
		const class UNavigationSystemV1* Nav, const FVector& NodeLoc) const;

	// CurrentDecreaseHealthLeech (uc:53): the live sword-drain, replaced each hit.
	TWeakObjectPtr<UWoTDecreaseHealthLeech> CurrentLeech;
	// DelayedDamageAngreal (uc:52): the hidden Spirit-element source spawned once at
	// possession (PreBeginPlay, uc:83), stamped onto the melee leech (uc:573).
	TWeakObjectPtr<AWoTMyrddraalSwordAngreal> SourceAngreal;
	// LastTeleportPosition (uc:68): the RejectTeleportPosition exclusion (uc:297).
	FVector LastTeleportPosition = FVector(FLT_MAX);
	bool bHasLastTeleportPosition = false;
	// The phase-1 destination (the beacon the pawn walks to, and the reappear spot).
	FVector PendingDestination = FVector::ZeroVector;
	bool bHasPendingDestination = false;
	int32 TeleportCount = 0;
};

// ---------------------------------------------------------------------------
// Hook 5 — BATrollocRecovery (exports/script/WoTPawns/BATrolloc.uc)
// ---------------------------------------------------------------------------
// The stagger: a single hit harder than RecoveryDamageAmount knocks the champion
// into a Recover animation that suspends it, no more than once per RecoveryInterval
// (TakeDamage, uc:83-97; state Recovering, uc:197-208). bNoInitialRecovery makes the
// FIRST qualifying hit merely ARM the cooldown instead of staggering (uc:88-91). The
// suspend reuses the controller ForcedStationary gate (hold + withhold attack) for
// the Recover duration. Also folds OnAdjustCollisionNotification (uc:109): the
// "bigger-ass radius" that grows the collision cylinder when the threat is close, to
// stop the champion running through its target.
UCLASS()
class UWoTBATrollocRecoveryBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnPossessedBy(AWoTNPCController* Controller) override;
	virtual void OnUnpossessed() override;
	virtual void TickBehavior(float DeltaSeconds) override;
	virtual void NotifyTookDamage(int32 Damage) override;

	// BATrolloc.uc defaultproperties (:237-239) + consts (:78-79).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float RecoveryInterval = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	bool bNoInitialRecovery = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float RecoveryDamageAmount = 15.0f;
	// The Recover-anim length (PlayAnim 'Recover' + FinishAnim, uc:205-206). A 1.0
	// stand-in until the creature anims bind (same convention as FWoTMeleeAttack).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float RecoverDuration = 1.0f;
	// OnAdjustCollisionNotification (uc:78-79/109): grow to BiggerAssRadius while the
	// threat is inside AdjustCollisionThreshold, on the DNI_Misc2 0.5s cadence (:265).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float AdjustCollisionThreshold = 750.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float BiggerAssRadius = 90.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Recovery")
	float CollisionAdjustInterval = 0.5f;

	bool IsRecovering() const { return bRecovering; }
	float GetLastRecoveryTime() const { return LastRecoveryTime; }

private:
	void EnterRecovering();
	void AdjustCollision();

	bool bRecovering = false;
	float RecoverTimer = 0.0f;
	float LastRecoveryTime = 0.0f;         // LastRecoveryTime (uc:76)
	float DefaultCollisionRadius = 30.0f;  // Default.CollisionRadius (uc:123)
	float CollisionAdjustTimer = 0.0f;
};

// ---------------------------------------------------------------------------
// Hook 6 — LegionSpiritsStomp (exports/script/WoTPawns/Legion.uc)
// ---------------------------------------------------------------------------
// The volcano stomp: the Hound champion's melee is a ground slam (MeleeRange 500)
// that spawns a LegionStompVolcano at its target (SpawnDamageWave1, uc:246). The
// volcano actor deals flat damage in its live overlap and launches victims up
// (LegionStompVolcano.Touch, uc:37-52), then ShakeRadius knocks nearby pawns back
// with a distance falloff (uc:279-306). Since the Legion carries no simple swing set
// (MeleeRange 500 is the reach, not a Grunt swing), the behavior drives the stomp on
// the offensive cadence once the threat is inside range. The 3 orbiting LegionSpirits
// (uc:97-134) are cosmetic ActorRotators — stored as data, flagged presentation. The
// LegionTargetingReflector + LegionInvSeeker/LegionStompAngrealInv (weave/seeker
// weapons) need the WotWeapon/targeting-reflector frameworks — deferred (PORT_NOTES).
UCLASS()
class UWoTLegionSpiritsStompBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void TickBehavior(float DeltaSeconds) override;

	// Legion.uc stomp cadence/range and ShakeRadius constants.  The volcano's
	// own constants live with AWoTLegionStompVolcano.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stomp")
	float StompRange = 500.0f;      // MeleeRange (Legion.uc:518)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stomp")
	float StompInterval = 0.5f;     // DNI_Offensive cadence
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stomp")
	float ShakeRange = 2500.0f;     // ShakeRadius foreach (Legion.uc:284)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stomp")
	float TargetLead = 0.3f;        // GoalActor.Velocity * 0.3 (Legion.uc:265)

	// SpawnDamageWave1 (uc:246): spawn the live volcano at ImpactPoint, then run the
	// separate ShakeRadius knockback around the Legion. Public for deterministic specs.
	void ApplyStompAt(const FVector& ImpactPoint, AActor* Target = nullptr);

	// ShakeRadius momentum Z (uc:297): 7e6 / (0.75*(Distance+500)*Mass) — the knockback
	// falloff, static for the spec (nearer pawns are launched harder).
	static float StompKnockbackZ(float Distance, float Mass);

	// SpiritOffsets (uc:488-489): 3 orbiting cosmetic spirits (presentation).
	const TArray<FVector>& GetSpiritOffsets() const { return SpiritOffsets; }
	bool IsStompPending() const { return bStompPending; }
	float GetPendingStompNotifyTime() const { return PendingStompNotifyTime; }
#if WITH_AUTOMATION_TESTS
	void SetPendingStompTargetForTest(AActor* Target) { PendingStompTarget = Target; }
	FVector ResolvePendingStompImpactForTest() const
	{
		return ResolvePendingStompImpact();
	}
#endif

private:
	FVector ResolvePendingStompImpact() const;
	// Verbatim Legion.uc:488-489 (index 0 unset → zero).
	TArray<FVector> SpiritOffsets = {
		FVector::ZeroVector, FVector(-50.0f, 0.0f, 0.0f), FVector(50.0f, 0.0f, 50.0f)};
	float StompTimer = 0.0f;
	TWeakObjectPtr<AActor> PendingStompTarget;
	bool bStompPending = false;
	bool bStompDamageFired = false;
	float PendingStompElapsed = 0.0f;
	float PendingStompNotifyTime = 0.0f;
	float PendingStompEndTime = 0.0f;
};

// ---------------------------------------------------------------------------
// Hook 7 — ArcherBow (exports/script/WoTPawns/Archer.uc)
// ---------------------------------------------------------------------------
// The Whitecloak Archer's kneel-fire bow + melee switch. Two halves:
//   (1) At possession it equips the bow — a UWoTWeaponComponent configured from
//       ArcherBowProxy.uc (MinProjectileRange 150, MaxProjectileRange 10000, the
//       ArcherArrow projectile, MissOdds 0.05). BuildTrackingBands then hands the
//       Archer the projectile-fighter ladder (prefer-visibility: kneel and shoot).
//   (2) The melee switch (CheckForMeleeSwitch, Archer.uc:167): once the Archer has
//       been engaging with the bow for MinProjectileAttackingStateExitInterval and
//       the target is inside SwitchToMeleeDistance, it drops prefer-visibility so the
//       range-band ladder falls through to melee (SwitchToBestWeapon → the sword,
//       whose damage is the pawn's MeleeAttacks set — the ArcherSword weapon column
//       is cosmetic, see PORT_NOTES). It re-arms the bow when the target retreats
//       back past SwitchToMeleeDistance.
// The kneel-draw-release cadence itself lives in the controller's PerformProjectile
// state; this hook owns bow equip and records its post-release melee commitment.
UCLASS()
class UWoTArcherBowBehavior : public UWoTCreatureBehavior
{
	GENERATED_BODY()

public:
	virtual void OnPossessedBy(AWoTNPCController* Controller) override;
	virtual void TickBehavior(float DeltaSeconds) override;
	// Called only from PerformProjectileAttack after a Release animation and the
	// five-second in-state exit gate (Archer.uc:167-178).
	void CommitToMelee();

	// Archer.uc defaultproperties (:252) + const (:49).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float SwitchToMeleeDistance = 320.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float MinProjectileAttackingStateExitInterval = 5.0f;
	// WOTPawn.SwitchToBestWeapon's gate (WOTPawn.uc:172): a successful switch
	// stamps LastWeaponSwitchTime and no further switch runs for
	// MinTimeBetweenWeaponSwitch seconds (Archer CDO 5.0, Archer.uc:258).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float MinTimeBetweenWeaponSwitch = 5.0f;

	// ArcherBowProxy.uc:11-19 + WotWeapon CDO (:848). The bow's usage-model window
	// and miss odds, applied to the equipped UWoTWeaponComponent.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float MinProjectileRange = 150.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float MaxProjectileRange = 10000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Bow")
	float MissOdds = 0.05f;

	bool IsCommittedToMelee() const { return !bBowArmed; }

private:
	// Equip the bow (a configured UWoTWeaponComponent) on the host, once.
	void EquipBow();

	// The equipped bow, so a spec can inspect / drive it.
	TWeakObjectPtr<UWoTWeaponComponent> Bow;

	// bBowArmed mirrors the tracking band's prefer-visibility: true = kneel-and-shoot,
	// false = committed to melee.
	bool bBowArmed = true;

	// LastWeaponSwitchTime (WOTPawn.uc:172): world seconds of the last successful
	// bow<->sword switch; -infinity-ish so the first switch is never gated.
	float LastWeaponSwitchTime = -1000.0f;
};
