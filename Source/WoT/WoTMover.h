#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTEvents.h"
#include "WoTMover.generated.h"

UENUM()
enum class EWoTMoverState : uint8
{
	None,
	BumpOpenTimed,
	TriggerOpenTimed,
	TriggerToggle,
	TriggerControl,
	StandOpenTimed,
	TriggerPound,
	BumpButton,
	GradualTriggerOpenTimed,
	GradualTriggerPound,
	GradualTriggerToggle,
};

UENUM()
enum class EWoTMoverEncroachType : uint8
{
	StopWhenEncroach,
	CrushWhenEncroach,
	ReturnWhenEncroach,
	IgnoreWhenEncroach,
};

UENUM()
enum class EWoTMoverBumpType : uint8
{
	PlayerBump,
	PawnBump,
	AnyBump,
};

UENUM()
enum class EWoTMoverGlideType : uint8
{
	MoveByTime,
	GlideByTime,
};

// The mover's runtime FSM phase — replicated so a client re-runs the keyframe interpolation
// locally (client-simulation model) instead of receiving raw transform snapshots. A file-scope
// UENUM (a bare enum can't be a replicated UPROPERTY).
UENUM()
enum class EWoTMoverPhase : uint8
{
	Closed,
	Delaying,
	Opening,
	Open,
	Closing,
	Stopped,
};

// UE1 Mover port (semantics from the original engine's Mover):
// keyframed brush actor. Key 0 = closed at BasePos/BaseRot; keys are relative
// offsets. MoverGlideType selects retail linear or cubic-smooth interpolation
// over MoveTime; StayOpenTime and Tag/Event wiring retain the script FSM.
// Server-authoritative; movement replicates.
UCLASS()
class AWoTMover : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTMover();

	UPROPERTY(EditAnywhere, Category = "WoT")
	TObjectPtr<class UStaticMeshComponent> Mesh;
	// Generation-owned mover geometry/materials. Explicit ownership keeps the
	// authenticated transient graph alive in guarded runtime maps.
	UPROPERTY(Transient) TObjectPtr<class UStaticMesh> RuntimeMesh;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> RuntimeMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<class UTexture2D>> RuntimeTextures;

	// UE1 SURFACE lighting (gamma scheme): movers are BSP brushes lit per-surface
	// by the engine's surface lighter — steep surface falloff and LE_Spotlight
	// cones, NOT the actor/Gouraud model decorations use. They carry NO baked
	// lightmap (verified 0/39). Dynamic-light movers carry one retail plane and
	// light selection per material section; runtime reselects them from the
	// mover-origin leaf on the shared movement timer. BeginPlay pushes the rows to
	// M_WoT_MoverLit MIDs. Off-cone/no-light slots use point-in-BSP zone ambient
	// (FGetHSV; LevelInfo when the pivot sits in solid). Empty/inert in the linear
	// scheme and on ordinary actor-lit movers.
	UPROPERTY(EditAnywhere, Category = "WoT")
	TObjectPtr<class UWoTMeshLight> MeshLight;

	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FVector> KeyPos;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FRotator> KeyRot;
	UPROPERTY(EditAnywhere, Category = "WoT") int32 InitialKeyNum = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") EWoTMoverBumpType BumpType =
		EWoTMoverBumpType::PlayerBump;
	UPROPERTY(EditAnywhere, Category = "WoT") EWoTMoverGlideType MoverGlideType =
		EWoTMoverGlideType::GlideByTime;
	UPROPERTY(EditAnywhere, Category = "WoT") float MoveTime = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float StayOpenTime = 4.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float DelayTime = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") EWoTMoverState InitialState =
		EWoTMoverState::BumpOpenTimed;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bTriggerOnceOnly = false;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bDamageTriggered = false;
	UPROPERTY(EditAnywhere, Category = "WoT") EWoTMoverEncroachType EncroachType =
		EWoTMoverEncroachType::ReturnWhenEncroach;
	UPROPERTY(EditAnywhere, Category = "WoT") int32 EncroachDamage = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") FName ReturnGroup;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bSlave = false;
	// Actor.bSpecialLit is part of retail SetupMoverSurface's equality gate.
	// The importer stamps the raw mover property; false is UE1 Actor's default.
	UPROPERTY(EditAnywhere, Category = "WoT") bool bSpecialLit = false;
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;

	// LockableMover (LockableMover.uc): a bump-open door that stays shut unless the
	// bumping pawn carries the key — an inventory item whose Event matches this mover's
	// Tag. bStayUnlocked latches it open after the first key use. Off by default, so a
	// plain Mover is unaffected.
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") bool bLockable = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") bool bUnlocked = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") bool bStayUnlocked = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") FName LockedSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") FName UnLockSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") float MinLockSoundTime = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") FString LockedMessage =
		TEXT("This door is locked.");
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") FString UnLockMessage =
		TEXT("The key opens this door.");
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") float LockedMessageDuration = 4.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") float UnLockMessageDuration = 4.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Lock") float UnLockDuration = 0.0f;

	UPROPERTY(Replicated) uint8 LockPresentationKind = 0;
	UPROPERTY(ReplicatedUsing = OnRep_LockPresentation)
	uint8 LockPresentationCounter = 0;
	FName LastLockSound;

	// Mover sounds (UE1 Mover MoverSounds group): one-shots at the FSM edges (DoOpen /
	// FinishedOpening / DoClose / FinishedClosing) + the
	// MoveAmbientSound loop while moving (UE1 set AmbientSound=MoveAmbientSound for
	// the move). Subpaths under /Game/WoT/Sounds, importer-set from the map T3D.
	// Played locally on every machine off the replicated Phase (authority transition
	// + OnRep — the trap phase-presentation pattern).
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName OpeningSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName OpenedSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName ClosingSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName ClosedSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName MoveAmbientSoundPath;
	// The move loop's UE1 ambient bytes (Actor defaults; instance overrides carried
	// by the importer).
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") uint8 SoundRadius = 32;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") uint8 SoundVolume = 128;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") uint8 SoundPitch = 64;

	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> MoveAmbient;

	// Spec observables (headless suites can't hear): the last edge one-shot played
	// and total edge cadence, plus whether the move loop is on.
	FName LastPhaseSound;
	int32 PhaseSoundPlayCount = 0;
	bool bMoveLoopOn = false;

	// UE1 Mover.RealPosition analog: the rest (key-0) pose cached at BeginPlay, independent
	// of the current keyframe interpolation. Pit placement measures against this, not the
	// mid-travel actor location. ZeroVector until BeginPlay.
	FVector GetBasePosition() const { return BaseLoc; }
	FRotator GetBaseRotation() const { return BaseRot; }
	int32 GetCurrentKeyNum() const { return RestingKeyNum; }
	// The projector knows BasePos/BaseRot separately from the saved KeyNum pose.
	// Preserve both before BeginPlay so the deferred actor already rests where
	// retail placed it without teaching BeginPlay the wrong base transform.
	void ConfigureInitialRestPose(const FVector& InBaseLoc,
		const FRotator& InBaseRot);
	static float EvaluateGlideAlpha(EWoTMoverGlideType GlideType, float Alpha);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void MakeGroupReturn();
	virtual void MakeGroupStop();
	// Mover.EncroachingOn returns true when the attempted move must abort.
	bool EncroachingOn(AActor* Other);
	void SetMoverFSMInert();
	bool IsMoverFSMInert() const { return bFSMInert; }
	AWoTMover* GetGroupLeader() const { return GroupLeader.Get(); }
	AWoTMover* GetGroupFollower() const { return GroupFollower.Get(); }
	EWoTMoverPhase GetMoverPhase() const { return Phase; }
	float GetStoppedAlpha() const { return StoppedAlpha; }
	float GetPhaseStartServerTime() const { return PhaseStartServerTime; }
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// Exposed to subclasses: AWoTPortcullisMover drives the raise/drop directly (inverted mover
	// semantics — starts raised, drops on trigger, re-raises on destruction — that the base
	// Closed→Open FSM doesn't express). Widening these to protected is an access change only; the
	// base FSM behavior is unchanged.
	virtual void ApplyAlpha(float Alpha, bool bSweep = true);   // 0 = key0, 1 = last key
	// Interpolate directly between two authored keys. Unlike ApplyAlpha, this does not walk the
	// intermediate key chain; LoopMover needs that distinction for its last-key -> key-0 leg.
	bool ApplyKeyframeLerp(int32 FromKey, int32 ToKey, float Alpha, bool bSweep = true);
	// Shared transform seam for keyframed and continuously rotating movers.
	bool ApplyMoverPose(const FVector& TargetLocation, const FRotator& TargetRotation,
		bool bSweep = true);
	void StartClose();
	void ReversePhase();

	// Replicated so clients interpolate locally (RemoteRole=SimulatedProxy, kept). Only the FSM
	// phase + the pawn-facing lifecycle replicate — NOT the full transform (bReplicateMovement is
	// off) nor the static keyframe config (level data on map movers).
	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	EWoTMoverPhase Phase = EWoTMoverPhase::Closed;
	// Server world-seconds when the current Phase began, backdated by a mid-move
	// reversal's already-elapsed time. Port of Mover.InterpolateTo replicating
	// PhysAlpha (SimInterpolate) — proxies seed their local clock from it, so
	// reversals resume at the server's alpha and mid-travel joiners don't pop to the
	// phase start. Shares Phase's notify so a same-phase restart still re-seeds.
	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	float PhaseStartServerTime = 0.0f;
	// Global key-chain alpha at the start of this phase. An authored nonzero
	// KeyNum still receives a full MoveTime interpolation, even when its target
	// key is already the current key.
	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	float PhaseStartAlpha = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	float StoppedAlpha = 0.0f;
	float PhaseTime = 0.0f;
	bool bSpent = false;

	// Authority-side phase transition: sets Phase + PhaseTime and stamps
	// PhaseStartServerTime. StartOffset > 0 = a reversal's already-elapsed time.
	void SetPhaseAuthority(EWoTMoverPhase NewPhase, float StartOffset = 0.0f);
	// Base-FSM leg durations. UE1: an instance's authored InitialState replaces
	// the class's auto state COMPLETELY, but plain-FUNCTION overrides stay live
	// under it — AssertMover.uc:38-56 overrides DoOpen/DoClose to select per-leg
	// OpenTimes/CloseTimes instead of MoveTime, so a subclass running the base
	// FSM (Battle2_02 AssertMover2, InitialState=TriggerOpenTimed) keeps that
	// leg-time selection by overriding these. Base movers are unchanged.
	virtual float OpenDuration() const { return MoveTime; }
	virtual float CloseDuration() const { return MoveTime; }
	// LockableMover.CheckForKey: does the pawn carry an item whose Event == our Tag?
	// DoorMover reuses the identical inventory-key gate.
	bool CheckForKey(APawn* P) const;
	void SetMoverInstigator(APawn* Pawn) { Inst = Pawn; }

	virtual void ApplyGroupReturnLocal();
	virtual void ApplyGroupStopLocal();
	// Encroachment dispatch happens inside pose application. Rewind the active
	// FSM's clock to the last pose that completed before a Return/Stop verb reads it.
	virtual void RewindEncroachClockToLastAppliedPose();
	void PresentLocked(APawn* Pawn);
	void PresentUnlock(APawn* Pawn);
	// Per-phase audibility gate consulted before the edge one-shot (the SoundPhase
	// dedup and ambient-loop bookkeeping run regardless). Default: every edge plays.
	// AWoTPortcullisMover suppresses the Open-family edges — its only raise is the
	// retail HideMover snap (InterpolateTo(1,0)), which is movement-only and silent
	// (Mover.InterpolateTo; OpeningSound/OpenedSound live solely in DoOpen /
	// FinishedOpening, which the snap never runs).
	virtual bool ShouldPlayPhaseEdgeSound(EWoTMoverPhase NewPhase) const { return true; }
	void PresentMoverEdge(FName SoundPath);
	void SetMoverAmbient(FName SoundPath, bool bEnable);
	void StopMoverAmbient() { SetMoverAmbient(NAME_None, false); }
	static bool ActorMatchesWoTTag(const AActor* Actor, FName WantedTag);
	float CurrentMoverAlpha() const;
	void ApplyPhaseSoundPresentation();
	float LastAppliedAlpha = 0.0f;

private:
	void StartOpen(APawn* EventInstigator);
	void FinishUnlock();
	void ResolveGroupAndSlaves();
	void AttachSlave(AWoTMover* Slave);
	void TriggerLockPresentation(uint8 Kind);
	void ApplyLockPresentation();
	void SendHandMessage(APawn* Pawn, const FString& Message, float Duration) const;
	UFUNCTION()
	void OnRep_LockPresentation();
	EWoTMoverPhase SoundPhase = EWoTMoverPhase::Closed;
	FName ActiveMoveLoopPath;
	// A proxy re-runs the keyframe interpolation locally from the replicated Phase — advancing a
	// LOCAL PhaseTime (reset by OnRep_Phase), NOT a server clock — instead of a transform snapshot.
	// It does NOT run the FSM transitions; the next Phase arrives via replication.
	void SimulateClientInterp(float DeltaTime);
	UFUNCTION()
	void OnRep_Phase();

	// Cached at BeginPlay; a pre-BeginPlay OnRep_Phase early-outs rather than read these
	// (BeginPlay re-applies the replicated phase once they're valid).
	FVector BaseLoc = FVector::ZeroVector;
	FRotator BaseRot = FRotator::ZeroRotator;
	int32 RestingKeyNum = 0;
	float RestingAlpha = 0.0f;
	bool bHasConfiguredBasePose = false;
	TWeakObjectPtr<APawn> Inst;
	TWeakObjectPtr<APawn> UnlockingPawn;
	FTimerHandle UnlockTimer;
	bool bNoBump = false;
	bool bFSMInert = false;
	float LastLockedSoundTime = 0.0f;
	TWeakObjectPtr<AWoTMover> GroupLeader;
	TWeakObjectPtr<AWoTMover> GroupFollower;
};
