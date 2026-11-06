#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "WoTBrainComponent.h"
#include "WoTRangeBands.h"
#include "WoTOrders.h"
#include "WoTNPCController.generated.h"

class AWoTAlarm;
class AWoTNPCCharacter;
class UWoTCreatureBehavior;
class UWoTWeaponComponent;
class AWoTNPCWeapon;

// The one shared NPC FSM: the original's ~90
// state{} blocks fold to these archetypes; the Reached/Reachable/Visible/
// Pathable leaf distinctions become range-band data, not states.
UENUM()
enum class EWoTBrainState : uint8
{
	StartUp,
	Waiting,
	Inactive,     // activity-LOD off-state: pulses off, proximity auto-wake
	Dormant,      // deep freeze: explicit wake only (citadel build-mode)
	Dying,
	Falling,
	TakeHit,
	Animating,
	Tracking,     // the goal-pursuit loop; band + transitioner are data
	AttemptAttack,
	PerformMelee,
	PerformMeleeWeapon,
	PerformProjectile,
	PrepareWeapon,
	CasterUse,
	CasterDefensiveUse,
	// The active-detection sidestep episode (LegendPawn PerformDodge label,
	// LegendPawn.uc:2276-2282): a Tracking-family label in retail, promoted to a state
	// here (as TakeHit/PerformMelee were). Disables the Defensive notifier, launches the
	// dodge velocity + falling physics, and waits for landing before resuming Tracking.
	Dodging,
	// The Myrddraal blink (state TeleporterReached, Myrddraal.uc:359-527), promoted to a
	// brain state (as Dodging was). Reached at the end of phase-1 navigation to a shadow
	// node beacon: INVULNERABLE, crank up, vanish (collision off + hidden), relocate to
	// the node, reappear facing the threat, then resume Tracking.
	Teleporting,
	// Appended to preserve the serialized/canonical numeric identities above.
	Returning,     // GI_Waiting's authored return-to-post tracking family
	InitialAcquisition // first visible hostile: SeeEnemy + turn, 60s entry gate
};

// The NPC brain driver: per-tick order is notifier pulses FIRST, then state
// work. Lifecycle, goal transitions, tracking,
// movement choreography, attacks, returning, and acquisition share this FSM.
UCLASS()
class AWoTNPCController : public AAIController
{
	GENERATED_BODY()

public:
	AWoTNPCController();

	// LegendPawn.uc:2646 InitialState=Dormant; Grunt overrides to InactiveIdle.
	// A bare spawn stays immediately testable in Waiting; creature configuration
	// applies the shipped per-class initial state before normal play.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	EWoTBrainState InitialState = EWoTBrainState::Waiting;

	EWoTBrainState GetBrainState() const { return State; }
	UWoTBrainComponent* GetBrain() const { return Brain; }
	bool IsWaitingCowering() const { return bWaitingCower; }

	// --- Activity LOD + dormancy ---

	// Every LegendPawn registers with (and may spawn) the shipped ActivityManager,
	// including in MP. Fold its 1s player-only observation pulse into the controller:
	// 2500uu 3D distance plus the signed 2500uu delta-Z test, no LOS or hysteresis.
	// Relevant players wake Inactive pawns and their absence force-inactivates active
	// pawns. Dormant never polls.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float ActivationRadius = 2500.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float ActivityEvalPeriod = 1.0f;

	bool IsInactive() const { return State == EWoTBrainState::Inactive; }
	bool IsDormant() const { return State == EWoTBrainState::Dormant; }

	// The citadel build-mode freeze (WOTPawn.BeginEditingResource -> ForceDormant,
	// WOTPawn.uc:558; also the DormantAll cheat). A deep freeze reached only by an
	// explicit call: all pulses + perception off, movement halted, and — unlike
	// Inactive — proximity does NOT wake it. Released only by UnForceDormant
	// (LegendPawn.uc:2497 -> TransitionOnGoalPriority).
	void ForceDormant();
	void UnForceDormant();
	// Accepted Ice temporarily uses the dormant shell to suspend all AI work. A
	// preexisting explicit Dormant is remembered and is not woken by thaw.
	void SuspendForIceFreeze(bool bHostWasDormantBeforeFreeze);
	void ResumeFromIceFreeze();
	// Controller replacement transfers the suspension latch without thawing the
	// pawn or running goal selection on the detached controller.
	void ReleaseIceFreezeOwnership();
	bool IsSuspendedForIceFreeze() const { return bSuspendedForIceFreeze; }

	// The creature behavior hook's ForceStationary gate (Minion stealth,
	// Minion.uc:176): while set, the tracking loop holds position and the Offensive
	// pulse withholds the attack. Set/cleared by the behavior each frame.
	void SetForcedStationary(bool bStationary) { bForcedStationary = bStationary; }
	bool IsForcedStationary() const { return bForcedStationary; }

	// The proximity the tracking ladder classified the active goal into on the
	// last iteration (inspection / specs). None until Tracking runs.
	EWoTGoalProximity GetTrackingProximity() const { return TrackingProximity; }
	const FWoTRangeBandSet& GetTrackingBands() const { return TrackingBands; }
	const FWoTMovementManager& GetMovementManager() const { return MovementManager; }
	float GetIntendedMovementSpeedForAnimation() const;
	bool HasActiveMovementIntentForAnimation() const;
#if WITH_AUTOMATION_TESTS
	float GetApproachingOffsetDistanceForTest() const
	{
		return ApproachingOffsetDistanceForTesting;
	}
	float GetApproachingLateralOffsetForTest() const
	{
		return ApproachingLateralOffsetForTesting;
	}
#endif
	// Grunt.Acquired*.GetPendingWeapon + bAutomaticWeaponSelection: specs may
	// drive the routing directly without waiting for a movement tick.
	void RouteWeaponForProximity(EWoTGoalProximity Proximity);
	// Archer's ranged arm is component-backed rather than an AWoTNPCWeapon. Its
	// behavior still routes through the normal queued switch so a held/preparable
	// sword completes its owner animation before EquipBodyAttack clears it.
	void RouteToComponentBackedRangedWeapon();

	// WOTPawn.TransitionToTakeHitState folded onto the brain FSM. Returns false
	// when the current state's retail stub suppresses the detour.
	bool TryEnterTakeHit(FName HitSlot, bool bHardHit);
	bool IsArcherKneeling() const;
	FName GetArcherAttackPhaseName() const;

	// Inspection seams: the slot the last melee swing picked (Minion standing/tentacle vs
	// running) and the slot the last dodge chose (DodgeLeft/DodgeRight).
	FName GetLastSwingSlot() const { return LastSwingSlot; }
	FName GetLastDodgeSlot() const { return LastDodgeSlot; }
#if WITH_AUTOMATION_TESTS
	void SetBrainStateForG20Test(EWoTBrainState NewState) { EnterState(NewState); }
	bool MeleeDamageGoalForTest(const struct FWoTMeleeAttack& Attack);
	void StartMeleeSwingForTest();
	void TickMeleeForTest(float DeltaSeconds);
	int32 MeleeNotifyAttemptsForTest = 0;
	float FirstMeleeNotifyFractionForTest = -1.0f;
	// Dodge spec seams: force the FRand()<0.5 side pick (-1 = random, 0 = left-first,
	// 1 = right-first) and mark ShiftDirs the obstruction sweep should treat as blocked
	// (a headless world has no BSP to trace against). TryBeginDodgeForTest /
	// FindDodgeDestinationForTest expose the protected dodge entry points.
	int32 DodgeForcedSidePick = -1;
	TArray<FVector> DodgeBlockedDirsForTest;
	bool TryBeginDodgeForTest() { return TryBeginDodge(); }
	bool FindDodgeDestinationForTest(FVector& OutDestination, AActor* ActorToDodge, bool bLeftFirst)
	{
		return FindDodgeDestination(OutDestination, ActorToDodge, bLeftFirst);
	}
	bool IsDodgeDestinationUnobstructedForTest(
		const FVector& ShiftDir, float ShiftDistance) const
	{
		return IsDodgeDestinationUnobstructed(ShiftDir, ShiftDistance);
	}
	void HandleIntermediateArrivalForTest() { HandleIntermediateArrival(); }
	bool IsStrikingGongForTest() const { return bGongStrikeActive; }
	void ClearMovementPatternsForFix4Test()
	{
		for (FWoTRangeBand& Band : TrackingBands.Bands)
		{
			Band.MovementBinding.Pattern = FWoTMovementPattern();
			Band.MovementBinding.WaypointSelector = NAME_None;
			Band.MovementBinding.FocusSelector = NAME_None;
		}
		for (FWoTRangeBand& Band : IntermediateBands.Bands)
		{
			Band.MovementBinding.Pattern = FWoTMovementPattern();
			Band.MovementBinding.WaypointSelector = NAME_None;
			Band.MovementBinding.FocusSelector = NAME_None;
		}
		MovementManager = FWoTMovementManager();
	}
#endif

	// The ArcherBow behavior's melee-switch toggle (Archer.uc:167 CheckForMeleeSwitch,
	// folded): clearing prefer-visibility drops the ranged fighter out of the Visible
	// (bow) band into the melee ladder, restoring it re-arms the bow. Called each frame
	// from UWoTArcherBowBehavior::TickBehavior.
	void SetTrackingPreferVisibility(bool bPrefer) { TrackingBands.bPreferVisibility = bPrefer; }

	// TransitionOnGoalPriority (LegendPawn.uc:484): walk the slots in priority
	// order, activate the first one whose transition handler accepts. Returns
	// true if some handler ran.
	bool TransitionOnGoalPriority(bool bEvaluate = true);

	// --- Myrddraal teleport (Myrddraal.uc:216-527; UWoTMyrddraalTeleportBehavior) ---

	// PerformTeleportation (uc:216): a trigger (melee/perimeter/active-detector) starts
	// the blink episode — pick a shadow-node destination (the behavior's FindTeleport
	// Position), set it as the Intermediate-goal beacon, and walk there (phase 1); the
	// Teleporting state runs the blink on arrival. No-op unless the pawn carries the
	// Myrddraal behavior, a destination was found, and no episode is already running.
	bool BeginTeleportEpisode();
	bool IsTeleporting() const { return State == EWoTBrainState::Teleporting; }
	// True while a blink is underway (walking to the beacon OR mid-vanish) — the guard
	// that keeps a fresh trigger from restarting the episode.
	bool IsTeleportEpisodeActive() const
	{
		return bTeleportBeaconActive || State == EWoTBrainState::Teleporting;
	}

	virtual void Tick(float DeltaSeconds) override;

	// A pawn showing itself to this observer: alive-gate, classify by faction,
	// enemy -> Threat goal, friendly -> Colleague goal (LegendPawn.uc:880-944,
	// :632-651). May-not-override per the original.
	void DispatchSeePawn(APawn* SeenPawn, bool bAllowInactive = false);

	// Grunt.uc:694-801 immediate reactions. Damage dispatch acquires the
	// instigator, then moves 160uu from friendly fire or >=20 hostile damage;
	// the shipped friendly-NPC Bump arm returns before moving, while a moving
	// friendly player produces the sidestep.
	void HandleGruntTakeDamage(float RawDamage, APawn* Attacker);
	void HandleGruntBump(APawn* Other);

	// --- Hearing (Grunt.OnHearNoise, Grunt.uc:545; engine CheckNoiseHearing) ---

	// The reception handler on THIS observer (Grunt.OnHearNoise): a non-hidden
	// noisemaker within earshot either transfers its threat (a heard friendly NPC)
	// or is acquired as a Threat (a heard enemy pawn — the SeeEnemyPawn arm). No-op
	// unless the observer's retail HearNoise is live (IsHearingLive); the acquired/
	// attack states stub HearNoise, so hearing only helps a not-yet-locked pawn.
	void DispatchHearNoise(float Loudness, AActor* NoiseMaker);

	// Waiting and non-acquired Tracking route HearNoise; acquired/investigating
	// Tracking contexts and the attack/hit/falling states have empty handlers.
	bool IsHearingLive() const;

	// The authority-side hearing fan-out (engine AActor::CheckNoiseHearing):
	// dedupe on the instigator before CanHear and script dispatch.
	// A general actor uses its Instigator unless explicitly supplied here. The
	// heard actor is always NoiseMaker, even when another pawn caused the noise.
	static void BroadcastNoise(UWorld* World, AActor* NoiseMaker, float Loudness,
		APawn* NoiseInstigator = nullptr);

	// Native walking loss-of-floor only, never a generic MOVE_Falling edge.
	void HandleWalkingOffLedge();
	void HandleFallingLanded(float ImpactVelocityZ, const FHitResult& Hit,
		bool bOriginatedInFalling);
	void HandleFallingSplash();

	// --- Captain orders layer (Captain.uc; active only on a bIsCaptain pawn) ---
	//
	// Folded into the controller (an orders layer enabled via bIsCaptain)
	// rather than a component: the machinery reads
	// and writes the brain's goal slots, binds the FSM's active band set, and runs
	// on-arrival inside TickTracking — it is controller state through and through,
	// so a component would be a back-pointer reaching into the controller every
	// call. The grunt-side hooks (FindLeader/AlarmSounded/AlertedByCaptain) are NOT
	// captain-gated — any WotPawn can be led or alerted — so they live here as
	// plain controller methods; only the order-issuing block gates on bIsCaptain.

	// Controller-consumed Captain CDO radii (Captain.uc:847-850).
	// FindHelpRadius remains pawn-owned like the shipped editable property;
	// LeadershipRadius is public because the leadership scan reads it off a
	// candidate captain's controller (WoTAIUtil::GetLeader).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float AlarmSearchRadius = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float AlertVisibleWotPawnsRadius = 160.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float LeadershipRadius = 240.0f;

	EWoTOrders GetAssignedOrders() const { return AssignedOrders; }
	EWoTOrders GetWorkingOrders() const { return WorkingOrders; }

	// Captain.uc:230 ChangeOrders: dispatch + adopt the order, re-pinging the
	// leadership range if it changed. Captain.uc:235-246 removes both guard-order
	// variants from this path by remapping them to KillIntruder.
	void ChangeOrders(EWoTOrders NewOrders);
	// Captain.uc:332 DispatchOrders: route an order to its handler (NOT remapped —
	// OnGuardLocation/OnGuardSeal are reachable here even though ChangeOrders isn't).
	void DispatchOrders(EWoTOrders Orders);
	// Captain.uc:260 EvaluateOrders: the alarm→help→kill ladder. Mutates
	// WorkingOrders and sets the GI_Intermediate goal; run on the first threat.
	void EvaluateOrders();
	// Captain.uc:208 ResetOrders: WorkingOrders = AssignedOrders.
	void ResetOrders();
	// Captain.uc:176 MapOrders: order name → enum.
	static EWoTOrders MapOrders(FName OrdersName);

	// Captain.uc:108 Trigger's Seal branch: a seal event (ownership change /
	// destroy / drop — Util.TriggerAllInstances over every pawn) re-targets a
	// GuardSeal captain. CENSUS: the gate is RETAIL-DEAD —
	// SetInitialState routes every captain through ChangeOrders (Captain.uc:91),
	// which collapses GuardSeal out of AssignedOrders (:235-246), and no other
	// author exists; kept for call-shape parity with the shipped .uc. Grunts
	// ignore seal triggers (Grunt.uc:805 handles only Alarm/WotWeapon).
	void NotifySealTriggered();
	int32 SealTriggerNotifiesForTest = 0;

	// Order handlers (Captain.uc:368-417).
	void OnGuardLocation();
	void OnKillIntruder();
	void OnGuardSeal();
	void OnSoundAlarm();
	void OnGetHelp();
	void OnAutomaticOrders();
	void OnInvalidOrder();

	// Captain.uc:433 IsLeader: this (captain) has a valid guarding goal.
	bool IsLeader() const;
	// Grunt.uc:1143 FindLeader (grunt-side): copy the nearest in-range leader
	// captain's guarding goal, or invalidate guarding if none. Captains return
	// false without acting (Captain.uc:440). Returns whether a leader was found.
	bool FindLeader();
	// Grunt.uc:831 CanBeGivenNewOrders: not occupied by a Threat/Guarding/
	// Intermediate/Refuge goal.
	bool CanBeGivenNewOrders() const;

	// Grunt.uc:567 AlarmSounded: a rung alarm gives an available pawn a
	// GI_Intermediate goal toward the alarm.
	void AlarmSounded(AWoTAlarm* Alarm);
	// Grunt.uc:578 AlertedByCaptain: an alerting captain gives an available pawn a
	// GI_Intermediate goal toward that captain.
	void AlertedByCaptain(AWoTNPCController* AlertingCaptain);
	// Captain.uc:775 AlertVisibleWotPawns: on HelpReached, alert every LOS'd
	// friendly WotPawn within CollisionRadius + AlertVisibleWotPawnsRadius.
	void AlertVisibleWotPawns();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	// --- Perception ---

	// Player-sighting sample: the engine's ~0.2s SightCounter cadence. Cone
	// (PeripheralVision stimulus), range (SightRadius scaled by the target's
	// Visibility/128), and LOS. NPC-NPC sighting does NOT run here.
	void SamplePlayerSight();

	// The inverted NPC-NPC path: on OUR Visibility pulse, this pawn shows
	// itself to every NPC observer with 360° range+LOS (no cone). Only runs in
	// show-self states (Tracking family + Waiting).
	void ShowSelfToNPCs();

	// --- Activity LOD ---

	// The 1s proximity poll while Inactive: on a waker in range, ForceActive
	// (LegendPawn.uc:1220 -> TransitionOnGoalPriority) leaves the off-state. No-op in
	// every other state (Dormant is explicit-wake only).
	void TickActivityLOD(float DeltaSeconds);

	// Reports both whether any player pawn exists and whether one is within the
	// distance-only activation cylinder. An empty player set produces no notice.
	bool HasWakerInRange(bool& bOutHasPlayers) const;

	// --- Tracking / movement ---

	void TickTracking(float DeltaSeconds);
	bool TickPatternMovement(float DeltaSeconds, const FWoTRangeBand& Band);
	bool SelectApproachingWaypoint(EWoTGoalIndex Goal, FVector& OutDestination);
	bool SelectRouteWaypoint(EWoTGoalIndex Goal, FVector& OutDestination);
	void HandleMovementHint(FName Hint);
	void StartMoveLeg();
	void TickDirectMove(float DeltaSeconds);
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

	// Build the tracking band set from the pawn's capabilities (the per-creature
	// RangeHandlerFactory, folded to data): a caster pawn gets the caster ladder
	// (hold + cast), a melee pawn gets the Grunt melee ladder. Creature row
	// data overrides this.
	FWoTRangeBandSet BuildTrackingBands() const;

	// --- Attack (Grunt.uc AttemptAttack/PerformMeleeAttack) ---

	void StartMeleeSwing();
	void TickMelee(float DeltaSeconds);
	void MatchGoalAcceleration();

	// --- Retail state episodes restored by parity-f1 ---
	void BeginReturning();
	void TickReturning(float DeltaSeconds);
	void FinishReturning();
	void BeginInitialAcquisition();
	void TickInitialAcquisition();
	bool TryEnterInitialAcquisitionForThreat(bool bVisibleHandler);
	void EnterAcquiredVoiceHandler();
	void UpdateWaitingCower();
	void TickWaitingCower(float DeltaSeconds);
	void BeginWaitingCower();
	void EndWaitingCower();
	void BeginWeaponSwitch(class AWoTNPCWeapon* Destination, bool bRanged,
		bool bDestinationIsBodyAttack = false);
	void StartWeaponPreparationPhase(class AWoTNPCWeapon* Weapon, bool bRanged,
		bool bPuttingAway);
	void TickWeaponPreparation(float DeltaSeconds);
	void CompleteWeaponPreparation();
	void ApplyPreparedWeaponDestination();
	void FinishWeaponPreparationEpisode();

	// --- Defensive dodge (DefensiveDetector*.uc + MovementManager.uc + LegendPawn.uc) ---

	// The Defensive-pulse trigger: an armed pawn (bDefensiveDodge) scans for an incoming
	// offender projectile and, if a clear sidestep exists, commits to the dodge
	// (LegendPawn PendingOffenderCollision → MovementManager.DefensiveDodge). Returns
	// whether a dodge began.
	bool TryBeginDodge();

	// FindDodgeDestination (MovementManager.uc:432-506): test the two perpendicular
	// sidesteps in the selected order, returning false only when both are obstructed.
	bool FindDodgeDestination(FVector& OutDestination, AActor* ActorToDodge, bool bLeftFirst) const;

	// IsActorDestinationUnobstructed (MovementManager.uc:515-525): a box sweep of the
	// pawn's cylinder ShiftDistance units along ShiftDir hits no world geometry.
	bool IsDodgeDestinationUnobstructed(const FVector& ShiftDir, float ShiftDistance) const;

	// PerformDodge label body (LegendPawn.uc:2276-2282 + TrackingBase
	// PerformDefensiveDodge :2121-2132 + Grunt anim :211-224): launch the 400/160 profile
	// into falling physics and play the local-Y-signed dodge slot.
	void StartDodge();
	// On landing (WaitForLanding returns): zero Velocity.Z, resume Tracking (which
	// re-enables the Defensive notifier).
	void CompleteDodge();

	// The Minion attack selector (Minion.uc PerformAttack :287-327): choose the swing
	// slot from goal speed + distance (standing bite/claws, tentacle grab, or the running
	// set). Non-Minion swings use GetEffectiveMeleeAnimSlot. Sets bGrabSwing.
	FName SelectSwingSlot(AWoTNPCCharacter* Self, bool& bOutGrabSwing);

	// The Archer kneel chain (Archer.uc PerformProjectileAttack): real Kneel/Draw/
	// Release/GetUp durations, a repeating visibility poll while drawn, and the
	// post-release five-second melee-switch gate. Non-Archers retain the generic
	// FireNotifyTime/DrawTime episode.
	void StartProjectileAttack();
	void TickProjectile(float DeltaSeconds);
	void BeginArcherPhase(uint8 NewPhase);
	void FireProjectileAtCurrentGoal();
	float PickedAnimTotalTime(const struct FWoTAnimTableEntry* Pick,
	                         bool bIncludeSequence) const;
	bool IsTakeHitSuppressed() const;
	EWoTBrainState GetStateForReturn() const;

	// Grunt.MeleeDamageGoal (Grunt.uc:1436): still-in-range gate, then a trace
	// from our height-offset location to the clamped point on the goal
	// cylinder; damage lands only if the trace hits the goal actor.
	bool MeleeDamageGoal(const struct FWoTMeleeAttack& Attack);
	// LegendPawn.uc:1122-1159: try the preferred heading, then the shipped
	// deterministic yaw ladder until the Intermediate goal is navigable.
	void MoveAwayFromAreaDirected(float MoveDistance,
		const FRotator& PreferredRotation, bool bOverrideCurrent = false);
	void ApplyCreatureImpulse(class AWoTCharacter* Victim,
		const FVector& Delta, const FRotator* NewRotation = nullptr,
		float ShakeTime = 0.0f, float RollMag = 0.0f, float VertMag = 0.0f);

	// GoalIndexTransition (LegendPawn.uc:518): may the FSM act on this slot?
	// Base accepts Threat (→ Tracking) and Waiting (→ Waiting); empty-priority
	// slots are skipped by the caller. Grunt/Captain add Guarding (return to
	// guard spot) and Intermediate (the alarm/help/seal navigation). Subclass
	// layers (casting) extend this.
	virtual bool GoalIndexTransition(EWoTGoalIndex Index);

	// SetInitialState (Captain.uc:83 / Champion.uc:14): copy the pawn's
	// AssignedOrders (forced Automatic in MP unless bChampionOrders), issue them,
	// and ping the leadership range. No-op on a non-captain pawn.
	void InitCaptainOrders();

	// The GI_Intermediate on-arrival dispatch (a delegate table): fired from
	// TickTracking when the intermediate goal classifies
	// Reached. Two live arrivals, keyed by goal actor type: an ALARM (captain
	// SoundAlarm rings it + drops to kill / grunt adopts its threat) and a
	// captain/help WOTPAWN (captain HelpReached alerts nearby friendlies / grunt
	// copies the captain's threat). No seal arrival — that route is dead in shipped
	// (Captain.uc:494-535/:800-825 commented out); GuardSeal is a GI_Guarding goal.
	void HandleIntermediateArrival();

	// Captain SuccessfullyNavigatedToAlarmToSound PerformInactivity (Captain.uc:723-727):
	// TweenPlaySlotAnim(Gong), FinishAnim, then SoundAlarm. TickGongStrike holds the
	// captain facing the alarm until the strike finishes and returns true while it
	// does; leaving Tracking or the alarm goal abandons the latent code, and the next
	// arrival restarts it (TakeHit returns to the state's Begin).
	void BeginGongStrike(class AWoTAlarm* AlarmActor);
	bool TickGongStrike(float DeltaSeconds);
	void SoundAlarmAtGoal(class AWoTAlarm* AlarmActor);

	// Captain.uc:448 OnWaitingNotification: heal 1 per idle pulse while unhealthy.
	// Inert in shipped (IsHealthy is always true — morale disabled); carried faithful.
	void CaptainIdleHeal();

	// Grunt.uc:897 IsHealthy: morale disabled → always true.
	bool IsHealthy() const;

	// The band set the FSM pursues right now: the intermediate-navigation ladder
	// while acting on GI_Intermediate, otherwise the threat/melee-or-caster ladder.
	const FWoTRangeBandSet& ActiveBands() const;
	// RangeHandlerSniping gate: a preferred Visible/hold band is eligible only
	// while the actual ranged weapon classifies WU_Projectile for this goal.
	EWoTGoalProximity ClassifyWithWeaponGate(const FWoTRangeBandSet& Bands,
		EWoTGoalIndex Goal);

	void EnterState(EWoTBrainState NewState);
	virtual void BeginState(EWoTBrainState InState);
	virtual void EndState(EWoTBrainState InState);
	virtual void TickState(float DeltaSeconds);

	virtual void OnNotifierPulse(EWoTNotifierSlot Slot);

	UPROPERTY()
	TObjectPtr<UWoTBrainComponent> Brain;

	// The per-creature behavior hook on the pawn, found at possession and
	// pumped each frame after the state update. Null when the creature has no hook.
	UPROPERTY()
	TObjectPtr<UWoTCreatureBehavior> Behavior;

	EWoTBrainState State = EWoTBrainState::StartUp;
	EWoTBrainState FallingReturnState = EWoTBrainState::Waiting;
	bool bFallingLanded = false;
	float FallingPoseRemaining = 0.0f;
	float FallingLandingRemaining = 0.0f;

	// Behavior ForceStationary gate (Minion stealth freeze). Consumed by TickTracking
	// (hold position) and the Offensive pulse (withhold attack).
	bool bForcedStationary = false;
	bool bSuspendedForIceFreeze = false;
	bool bWasDormantBeforeIceFreeze = false;

	// The captain's gong strike in progress (BeginGongStrike): the alarm it will ring
	// and the strike time left (tween + Gong sequence, the FinishAnim wait).
	bool bGongStrikeActive = false;
	TWeakObjectPtr<class AWoTAlarm> GongAlarm;
	float GongStrikeRemaining = 0.0f;

	// The active goal-pursuit ladder (RangeTransitioner in HS_GoalProximity mode,
	// its band set). Built at possess from the pawn's capabilities.
	FWoTRangeBandSet TrackingBands;
	FWoTMovementManager MovementManager;
	bool bMovementCycleActive = false;
	bool bMovementCycleTurning = false;
	bool bMovementCycleMoving = false;
	bool bMovementCyclePostHandled = false;
	float MovementCycleStart = 0;
	EWoTGoalIndex MovementCycleGoal = EWoTGoalIndex::Threat;
	TWeakObjectPtr<AActor> MovementCycleActor;
	EWoTGoalProximity MovementCycleProximity = EWoTGoalProximity::None;

	// The navigation ladder used while acting on a GI_Intermediate goal (alarm/
	// help/seal). Data-only; built once at possess (FWoTRangeBandSet::Intermediate-
	// NavDefault). ActiveBands() selects it vs TrackingBands by current goal.
	FWoTRangeBandSet IntermediateBands;

	// --- Orders state (Captain.uc:44-45; meaningful only when bIsCaptain) ---
	// Mirrored from the pawn at possession; the pawn is the placed-data source.
	bool bIsCaptain = false;
	EWoTOrders AssignedOrders = EWoTOrders::KillIntruder;   // GUI/map sets this
	EWoTOrders WorkingOrders = EWoTOrders::KillIntruder;    // the AI reads this

	// The reinforcements cursor (Captain.ReinforcementsSorter, Captain.uc:47):
	// the collected help candidates, consumed one per HelpReached iteration.
	TArray<TWeakObjectPtr<AWoTNPCCharacter>> ReinforcementCandidates;

	// Last-classified proximity of the current goal (the RangeIterator cursor).
	EWoTGoalProximity TrackingProximity = EWoTGoalProximity::None;

	// Prefer-visibility flip cadence (DNI_Misc7 MaybeFlip notifier, Grunt.uc:2429).
	// Dormant when FlipPeriod <= 0 (base Grunt ships -1); only hybrid creatures
	// with both a melee and a ranged band arm it.
	float FlipTimer = 0.0f;

	// Player-sighting sampler (the 0.2s SightCounter model, randomized phase).
	float SightSampleTimer = 0.0f;

	// Activity-LOD proximity poll (the 1s eval cadence; phase randomized at possess
	// like the shipped manager's first fire).
	float ActivityEvalTimer = 0.0f;

	// Direct-move leg state (the moveToward primitive): give-up budget seeded
	// 1.0 + 1.3·dist/speed (pawn targets capped 1.2s), decremented per tick;
	// negative at completion = a strike.
	bool bDirectMoveActive = false;
	float MoveTimer = 0.0f;
	// One authoritative destination for ordinary direct, routed, and movement-
	// pattern legs. Routed movement refines this to the path's current segment.
	FVector MoveDestination = FVector::ZeroVector;
	bool bPathMoveActive = false;
	FVector GetActiveLatentDestination() const;

#if WITH_AUTOMATION_TESTS
	float ApproachingOffsetDistanceForTesting = 0.0f;
	float ApproachingLateralOffsetForTesting = 0.0f;
#endif

	// Returning keeps the authored waiting post/facing separate from ordinary
	// Tracking and movement-pattern state. A changed post restarts this leg.
	FVector ReturningTarget = FVector::ZeroVector;
	FRotator ReturningFacing = FRotator::ZeroRotator;
	bool bReturningFacing = false;
	// Returning's shipped tracking loop has a 0.01s minimum iteration. Keep a
	// failed UE path submission on that cadence; only stable walking or swimming
	// starts which project on the agent's nav data consume the bounded adaptation
	// before retail UnableToReturn adopts the pawn's current transform.
	float ReturningPathRetryRemaining = 0.0f;
	int32 ReturningPathFailureCount = 0;
	FVector WaitingGoalSnapshot = FVector::ZeroVector;
	FRotator WaitingFacingSnapshot = FRotator::ZeroRotator;

	// Acquired visible component: non-reentrant for 60 seconds. PlayAcquiredSound
	// runs only when a folded Acquired handler is entered; its first entry arms the
	// five-second deadline and a later entry at/after that deadline may bark.
	float NextInitialAcquisitionTime = -1.0f;
	// Provenance of the last selected Threat range handler. This is deliberately
	// separate from controller state/current goal: the same Threat can first be
	// selected through a nonvisible hearing handler and later enter Visible.
	TWeakObjectPtr<AActor> AcquisitionHandlerThreat;
	bool bAcquisitionHandlerWasVisible = false;
	float NextAcquiredSoundTime = -1.0f;
	EWoTGoalProximity AcquiredVoiceProximity = EWoTGoalProximity::None;
	bool bAcquiredVoiceHandlerActive = false;

	// WaitingCower is a label inside Waiting, not a distinct goal state. The
	// intro is one-shot; notifier pulses leave the looping pose untouched.
	bool bWaitingCower = false;
	bool bWaitingCowerLoop = false;
	float WaitingCowerRemaining = 0.0f;
	float WaitingCowerElapsed = 0.0f;
	float WaitingCowerNotifyTime = -1.0f;
	bool bWaitingCowerNotifyFired = false;
	float WaitingCowerLoopElapsed = 0.0f;
	float WaitingCowerLoopDuration = 0.0f;
	float WaitingCowerLoopNotifyTime = -1.0f;

	// PrepareMeleeWeapon / PrepareProjectileWeapon named-step continuation. The
	// active phase can be the currently held/down weapon or the queued bring-up.
	TWeakObjectPtr<class AWoTNPCWeapon> PreparationWeapon;
	TWeakObjectPtr<class AWoTNPCWeapon> PreparationDestination;
	EWoTBrainState PreparationReturnState = EWoTBrainState::Tracking;
	bool bPreparationRanged = false;
	bool bPreparationPuttingAway = false;
	bool bPreparationDestinationRanged = false;
	bool bPreparationDestinationBodyAttack = false;
	bool bPreparationSwitched = false;
	float PreparationElapsed = 0.0f;
	float PreparationSwitchTime = 0.0f;
	float PreparationEndTime = 0.0f;
	bool bCompletingWeaponPreparationEpisode = false;

	// Melee swing state: the anim-table pick sets the swing sequence; every
	// MeleeAttacks row with that AnimName fires once at its notify time (fraction x
	// the sequence's duration at the table rate), and the swing ends at the
	// sequence's end — the shipped PlayStateAnimation + mesh-notify + FinishAnim
	// cadence. Table-less pawns keep the 1s stand-in swing.
	TArray<int32, TInlineAllocator<4>> PendingSwingAttacks;   // indices into MeleeAttacks
	TArray<float, TInlineAllocator<4>> PendingSwingTimes;     // seconds into the swing
	// The swing's PlaySlashSound notifies (sound 5d): whoosh at the authored fraction.
	TArray<float, TInlineAllocator<2>> PendingSlashTimes;
	TArray<FName, TInlineAllocator<2>> PendingSlashSlots;
	float SwingElapsed = 0.0f;
	float SwingEndTime = 1.0f;
	// The slot the last swing picked (Minion standing/tentacle vs running); inspection.
	FName LastSwingSlot;
	bool bStationaryMeleeSwing = false;

	// Minion tentacle-grab episode (Minion.uc PerformTentacleGrabAttack :403-465): the
	// grab disables the Movement notifier, draws a PLAYER victim in at the @0.40 damage
	// notify, and releases at @0.50. GrabReleaseTime is measured from the swing start.
	bool bGrabSwing = false;
	bool bGrabDrawInApplied = false;
	bool bGrabReleased = false;
	float GrabReleaseTime = 0.0f;
	TWeakObjectPtr<AActor> GrabVictim;
	bool bBATHeadTossSwing = false;
	bool bBATHeadTossHit = false;
	bool bBATHeadTossApplied = false;
	float BATHeadTossTime = 0.0f;
	TWeakObjectPtr<AWoTCharacter> BATHeadTossVictim;

	// Dodge episode (LegendPawn PerformDodge). The chosen destination + the offender we
	// responded to. Retail's RespondedToActor runs with bRecollect=true at the dodge
	// site (LegendPawn.uc:1376) so the SAME offender may be re-dodged on the next poll
	// (a re-acquiring seeker keeps getting dodged); the only double-response guard is
	// the Defensive notifier being off through the episode. LastDodgeSlot records the
	// DodgeLeft/DodgeRight pick.
	FVector DodgeDestination = FVector::ZeroVector;
	TWeakObjectPtr<AActor> DodgeOffender;
	FName LastDodgeSlot;

	// Myrddraal teleport episode (Myrddraal.uc:216-527). Phase 1 walks to the shadow-
	// node beacon (bTeleportBeaconActive, the Intermediate goal); on arrival the
	// Teleporting state (phase 2) runs the crank-up → vanish → relocate → reappear
	// timeline, stepping through TeleportPhase on the behavior's crank-up/duration
	// constants. The step boundaries (vanish, reappear) call into the behavior.
	bool bTeleportBeaconActive = false;
	// 0 Windup (PreTeleport), 1 CrankUp, 2 Hidden, 3 ExitCrankUp, 4 FadeIn.
	uint8 TeleportPhase = 0;
	float TeleportPhaseElapsed = 0.0f;
	float TeleportPhaseDuration = 0.0f;
	void EnterTeleportState();
	void AdvanceTeleportPhase();
	void SpawnTeleportSprayer(bool bExit, const FVector& Location);
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> EnterTeleportSprayer;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> ExitTeleportSprayer;
	UPROPERTY(Transient) TObjectPtr<class AWoTActorRotator> TeleportSprayerRotator;

	// TakeHitState stores the interrupted state and returns to its Begin label.
	EWoTBrainState TakeHitReturnState = EWoTBrainState::Tracking;
	float TakeHitElapsed = 0.0f;
	float TakeHitDuration = 0.0f;
	// True only across the EnterState(TakeHit) call for a SOFT hit: retail's soft
	// arm keeps the pawn moving (Grunt.uc:2128-2136), so EndState(Tracking) must
	// not kill the active leg for that transition.
	bool bSoftTakeHitTransition = false;

	// Kneel-fire episode state: the arrow releases at FireNotifyTime, the episode
	// ends at DrawTime (the weapon component supplies both).
	float DrawElapsed = 0.0f;
	bool bArrowReleased = false;
	// Archer.uc's labels, kept as a compact sub-state only while the generic brain
	// state is PerformProjectile. 0=None, 1=Kneel, 2=Draw, 3=Hold,
	// 4=RickyPause, 5=Release, 6=GetUp.
	uint8 ArcherPhase = 0;
	float ArcherPhaseElapsed = 0.0f;
	float ArcherPhaseDuration = 0.0f;
	float ArcherStateElapsed = 0.0f;
	float ArcherVisibilityPollRemaining = 0.0f;
	float ArcherHoldElapsed = 0.0f;
	// A post-release melee switch returns through PerformProjectile only to play
	// the retail GetUp continuation; it must not start a second kneel/fire cycle.
	bool bResumeArcherGetUpAfterPreparation = false;
	static constexpr float ArcherBowDrawnTimerDuration = 0.15f;
	static constexpr float ArcherBowDrawnSleepDuration = 5.0f;
	static constexpr float ArcherSwitchToMeleeDistance = 320.0f;
	static constexpr float ArcherRickyPauseTime = 0.15f;
	static constexpr float ArcherMinProjectileExitInterval = 5.0f;
};
