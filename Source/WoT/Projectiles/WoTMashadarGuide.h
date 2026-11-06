#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTMashadarGuide.generated.h"

class AWoTCharacter;
class AWoTMashadarSegment;
class AWoTNPCCharacter;

// The tendril's guard assignment (MashadarGuide.uc:1808-1898 GuardLocation/GuardActor/
// GuardSeal). Free = the roaming hunter; the guard modes confine hunting to
// enemies that threaten a guarded seal/location/actor (the citadel-defense command).
UENUM()
enum class EWoTMashadarGuard : uint8
{
	Free,
	Seal,
	Location,
	Actor
};

// Durable current chain state for replicas that did not observe the transition edge.
// RepNotify counters below preserve the shipped edge reactions for connected clients;
// this phase is the late-join truth applied after initial RepNotify replay.
UENUM()
enum class EWoTMashadarChainPhase : uint8
{
	Extending,
	Retracting,
	Resting
};

// Mashadar tendril — the Hound faction's citadel defender, summoned by the Hound
// Guardian ter'angreal (the Captain-tier troop MashadarFocusPawn).
// Spec: exports/script/WoTPawns/{MashadarGuide,MashadarArenaSpawner,MashadarTrailer}.uc
//
// The guide is the homing LEADER (AWoTSeekingProjectile): it deploys from its spawn
// point, searches for the nearest reachable enemy within SearchRadius each
// SearchResolution, homes via SetDestination, and glides through whatever it touches
// (persistent — never explodes). The DAMAGE lives on the body: a chain of
// AWoTMashadarSegment IK joints (an initially hidden head cloud at the guide,
// body segments back toward the lair) that deal Spirit ('xxxxS') touch damage while bActive; the chain
// grows with the guide's reach and withdraws when idle. A hit on any segment routes to
// this guide's shared health (killable → frag credit), and killing the guide tears the
// whole chain down. GuardSeal/GuardLocation/GuardActor confine the hunt (citadel
// defense). DEFERRED with the segment-mesh import (content): the body's render + its
// per-frame growth/fade choreography, pain particles, sounds; and the citadel-editor
// command INPUT/UI (with the rest of the citadel UI). See the scoping doc.
UCLASS()
class AWoTMashadarGuide : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:
	AWoTMashadarGuide();

	// The owning team (copied from the caster — MashadarArenaSpawner.uc:61). Targets
	// on other teams are hunted.
	// EditAnywhere: the map importer/patcher stamps per-instance Team from the T3D —
	// a bare Replicated UPROPERTY is invisible to python set_editor_property.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Mashadar") uint8 Team = 255;

	// Who summoned us — for owner kill credit (the hidden MashadarFocusPawn's PRI role,
	// folded onto the ported SummonOwner path). Server-side.
	UPROPERTY() TWeakObjectPtr<AWoTCharacter> SummonOwner;
	// Runtime-derived focus-victim name for arena guides without a materialized
	// focus pawn. Never contains compiled retail prose.
	FString DefenderDisplayName;
	// Ephemeral focus-PRI counters. Retail increments these before consuming the
	// hidden focus pawn; keep the same scoring state even though it is short-lived.
	int32 DefenderDeaths = 0;
	int32 DefenderSuicides = 0;

	// The anchor we deploy from and retract to (MashadarGuide.HidingPlace — self-spawned
	// at deploy; here just the spawn location, so no map RestPoint placement is needed).
	// Replicated COND_InitialOnly (retail InitialX/Y/Z, MashadarGuide.uc:213-300): the
	// client rebuilds its RemoteRole=None trailer chain from this authored transform.
	UPROPERTY(Replicated) FVector HidingPlaceLocation = FVector::ZeroVector;
	UPROPERTY(Replicated) FVector InitialGuideLocation = FVector::ZeroVector;
	UPROPERTY(Replicated) FRotator InitialGuideRotation = FRotator::ZeroRotator;
	// Speed is not a replicated AActor field. Retail sent these remembered settings
	// explicitly (MashadarGuide.uc:224-228, 297-309) so focus A/B reactions can make
	// the same local speed changes on every machine.
	UPROPERTY(Replicated) float InitialSpeed = 200.0f;
	UPROPERTY(Replicated) float InitialMaxSpeed = 400.0f;
	UPROPERTY(Replicated) float InitialAcceleration = 25.0f;

	// The guide's shipped voice (SoundTableClass=SoundTableMashadar2, multipliers
	// 1.0, NO timer list — MashadarGuide.uc:1922-1925): Breath on fast growth,
	// SeeEnemy on a new destination, the melee taunts from the biting segments.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar")
	FName VoiceTableName = FName(TEXT("Mashadar2"));
	// The shipped 'Breath' creak fired when >2 segments landed in ONE growth tick
	// (MashadarGuide.uc:432 "jerkiness"); the port grows one segment per tick, so
	// the count is adapted to a 1s rolling window (PORT_NOTES).
	float AppendWindowStart = 0.0f;
	int32 AppendsInWindow = 0;

	// Hunt range (MashadarArenaSpawner sets SearchRadius 1000; the guide CDO is 600).
	// EditAnywhere: Battle-map guides author per-instance SearchRadius (e.g. 1600) —
	// EditDefaultsOnly rejects per-instance python stamps.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Mashadar") float SearchRadius = 1000.0f;
	// Re-target cadence (MashadarGuide.uc SearchResolution 2.0s).
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float SearchResolution = 2.0f;
	// MashadarGuide.uc:90/93 and :1718-1724. Imported Battle-map guides begin
	// parked; placement deploys the guide and narrows its first live trigger range.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Mashadar")
	bool bCitadelPlaced = false;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Mashadar")
	bool bDeployed = false;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Mashadar")
	bool bFullRetract = false;
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Mashadar") float TriggerRadius = 200.0f;
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar")
	float CitadelEditingSpeed = 400.0f;
	// Withdraw speed toward the hiding place when idle (MashadarGuide.uc RetractSpeed 700);
	// RestRadius = how close counts as "home" (then it rests).
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float RetractSpeed = 700.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") float RestRadius = 50.0f;
	// The tendril's own health — it can be killed (MashadarManager.uc MashadarSlot health,
	// seeded int(RandRange(MashadarMinHealth 150, MashadarMaxHealth 200))). Held on the guide
	// (the 256-slot manager reduces to one guide per arena cast).
	UPROPERTY(Replicated) int32 MashadarHealth = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") int32 MashadarMinHealth = 150;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") int32 MashadarMaxHealth = 200;

	// --- Guard commands (the citadel-defense orders; MashadarGuide.uc:1808-1898) ---
	// The full GuardData[256]/GuardActor slot-claiming in the MashadarManager is
	// `/*OBE: Orders are automatic now*/` (dead) in the shipped code; the LIVE path is
	// seal-guarding (GuardSeal → IncrementSealGuardCount, ShouldStopGuarding's
	// IsCloseToSeal — MashadarManager.uc:689-693). Folded to a per-guide mode (the
	// arena spawns one guide per cast, so the 256-slot manager reduces to this).

	UPROPERTY(Replicated) EWoTMashadarGuard GuardMode = EWoTMashadarGuard::Free;
	UPROPERTY() FVector GuardPoint = FVector::ZeroVector;
	UPROPERTY() TWeakObjectPtr<AActor> GuardTarget;

	// How close a victim must be to the guarded seal / point / actor to be pursued
	// (MashadarManager.uc GuardRadius / SealGuardRadius, both 1000).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") float GuardRadius = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") float SealGuardRadius = 1000.0f;

	void GuardSeal();                 // guard any friendly-contested seal (uc:1830)
	void GuardLocation(const FVector& Loc);   // guard a fixed point (uc:1808)
	void GuardActor(AActor* Victim);  // guard a moving actor (uc:1818)
	void UnGuard();                   // release the assignment, resume free hunting (uc:1841)

	// Citadel editor path (MashadarGuide.uc:1718-1798): follow/extend toward the
	// editor-only focus marker, then retract home when it is unbound. Deploy leaves
	// the parked guide armed for its first enemy inside TriggerRadius.
	void SetFocusPawn(AWoTNPCCharacter* NewFocusPawn);
	AWoTNPCCharacter* GetFocusPawn() const { return FocusPawn.Get(); }
	// Body in the .cpp: IsValid() needs the complete AWoTNPCCharacter type, which this
	// header only forward-declares (MSVC C2664; Clang accepted it).
	bool FollowingFocusPawn() const;
	bool CanBindFocusAt(const FVector& Position) const;
	void Deploy();
	bool IsDeployed() const { return bDeployed; }

	// ShouldStopGuarding (MashadarManager.uc:683): a victim only counts while it is inside
	// the guard radius of the guarded thing. Free mode guards nothing → always true.
	bool IsWithinGuard(const FVector& VictimLoc) const;

	// The nearest reachable enemy pawn within SearchRadius of the hiding place that also
	// satisfies the guard (IsWithinGuard), else null.
	AActor* GetClosestPawn() const;

	// Re-run the target search (Search()/Chase() folded): pick the closest guard-valid
	// enemy and home on it; drop the target when none qualifies.
	void SearchAndChase();

	// Is Other a live pawn on a real team that isn't ours? (Public: the body segments
	// consult it to decide whether to bite.)
	bool IsEnemyPawn(const AActor* Other) const;

	// --- The visible/damaging body chain (MashadarGuide.uc MasterSegment/LastSegment) ---
	// The tendril is a chain of AWoTMashadarSegment IK joints trailing the guide: an
	// initially hidden MashadarTip head at the guide, then MashadarSegment bodies back
	// toward the lair. The segments carry the touch damage; the guide is the homing leader.
	// bActive gates biting (MashadarGuide.uc bActive) — only an activated tendril damages.

	UPROPERTY(Replicated) bool bActive = false;
	UPROPERTY(Replicated)
	EWoTMashadarChainPhase ChainPhase = EWoTMashadarChainPhase::Resting;
	// FocusPawn actor-reference resolution can lag the initial bunch. Preserve the
	// movement-speed context separately so a late join still reconstructs it exactly.
	UPROPERTY(Replicated)
	bool bUseCitadelEditingSpeed = false;
	virtual void SetDestination(AActor* Dest) override;
	void Activate();     // begin biting (a target was acquired)
	void Deactivate();   // stop biting (target lost / gave up / shocked)
	void Shock(APawn* LightningPawn);
	bool IsShocked() const { return bShocked; }

	// MashadarGuide.uc:145-163/436-478 used replicated FID/LastFID pairs as fake
	// client RPCs. Five RepNotify counters are the UE5 spelling: the server increments a
	// counter after updating its payload, and the client remembers the last counter it
	// consumed so re-notification is harmless. AWoTSeekingProjectile is always relevant,
	// so these eventual reliable property updates reach every guide copy.
	UPROPERTY(ReplicatedUsing = OnRep_ClientDeactivateCounter)
	uint32 ClientDeactivateCounter = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ClientUnRetractCounter)
	uint32 ClientUnRetractCounter = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ClientSetDestinationCounter)
	uint32 ClientSetDestinationCounter = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ClientSetFocusPawnACounter)
	uint32 ClientSetFocusPawnACounter = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ClientSetFocusPawnBCounter)
	uint32 ClientSetFocusPawnBCounter = 0;

	UFUNCTION() void OnRep_ClientDeactivateCounter();
	UFUNCTION() void OnRep_ClientUnRetractCounter();
	UFUNCTION() void OnRep_ClientSetDestinationCounter();
	UFUNCTION() void OnRep_ClientSetFocusPawnACounter();
	UFUNCTION() void OnRep_ClientSetFocusPawnBCounter();

	// Unlike the durable edge counters, lethal delivery cannot wait for a property
	// update: Destroy closes the guide's actor channel. Queue the reliable sever RPC
	// first, carrying the one-based private-chain index as its payload.
	UFUNCTION(NetMulticast, Reliable)
	void MulticastBreakOffMashadarAt(int32 OneBasedIndex);

	// A lethal hit carries the hit segment's one-based index to client-local chains
	// before the authority guide finishes its normal death path (uc:894-916).
	void NotifyLethalSegmentHit(AWoTMashadarSegment* HitSegment);
	bool IsManagedSegment(const AWoTMashadarSegment* Segment) const;

	// Narrow observability seams for the role-simulation specs. Counter counts
	// advance only for a new value; break-off advances only for the first multicast.
	uint32 GetDeactivateReactionCountForTesting() const { return DeactivateReactionCount; }
	uint32 GetUnRetractReactionCountForTesting() const { return UnRetractReactionCount; }
	uint32 GetSetDestinationReactionCountForTesting() const { return SetDestinationReactionCount; }
	AActor* GetLastSetDestinationPayloadForTesting() const
	{
		return LastSetDestinationPayload.Get();
	}
	uint32 GetSetFocusPawnAReactionCountForTesting() const { return SetFocusPawnAReactionCount; }
	uint32 GetSetFocusPawnBReactionCountForTesting() const { return SetFocusPawnBReactionCount; }
	uint32 GetBreakOffReactionCountForTesting() const { return BreakOffReactionCount; }
	bool IsRetractingForTesting() const { return bRetracting; }

	// The head of the chain (MashadarTip), for tests and the segment damage seam.
	AWoTMashadarSegment* GetMasterSegment() const { return MasterSegment; }
	// A body segment tore down (its Destroyed): drop stale chain pointers.
	void NotifySegmentDestroyed(AWoTMashadarSegment* Seg);

	// Cap on body segments before the tendril must retract (MashadarGuide.uc
	// MaxNumSegments 100 — a safety ceiling; the chain length otherwise tracks how far
	// the guide has ranged from its lair).
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") int32 MaxNumSegments = 100;
	// HeadType/SegmentType were replicated config in retail (uc:193-201). The port's
	// two leaves normally share AWoTMashadarSegment and select MashTip/MashSeg through
	// bIsHead, while these class fields preserve authored subclass overrides.
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar")
	TSubclassOf<AWoTMashadarSegment> HeadType;
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar")
	TSubclassOf<AWoTMashadarSegment> SegmentType;
	// Segment spacing / collision reach (MashadarTip head: Dist 48 / radius 42;
	// MashadarSegment body: Dist 42 / radius 25).
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float HeadDist = 48.0f;
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float HeadRadius = 42.0f;
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float BodyDist = 42.0f;
	UPROPERTY(EditDefaultsOnly, Replicated, Category = "WoT|Mashadar") float BodyRadius = 25.0f;

	// The guide never collides with the world (MashadarGuide.uc:1941 bCollideWorld=False,
	// bAllowClipping=True): the base ExitSeeking re-blocks ECC_WorldStatic, which would
	// strand a crack-parked citadel guide unable to retract home.
	virtual void ExitSeeking() override;
	virtual void ApplySeekingCollisionState() override;
	// Chase's IsChasable gate (MashadarGuide.uc:1235, 1283-1305): only hostile live pawns
	// or hostile live illusion decoys may take (or assimilate) the tendril.
	bool IsChasableTarget(const AActor* Other) const;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	// MashadarGuide.uc:972-1012: reaching a target never runs GenericProjectile's
	// destructive explosion path. The guide stops at an editor focus/illusion and
	// coils around ordinary combat targets.
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void NotifyReachedDestination() override;
	virtual void DestinationLost() override;
	virtual bool DestinationExists() override;
	// SeePlayer -> Chase (MashadarGuide.uc:504-552): the Personal Illusion decoy
	// notifies through here (the guide is retail's only bNotifySeePlayer weave).
	virtual void NotifySeePawn(AActor* SeenPawn) override;
	// A valid, still-live illusion decoy — a holdable chase target that isn't a pawn.
	static bool IsLiveIllusion(const AActor* Other);
	// HurtMashadar (MashadarGuide.uc:894 → MashadarManager.uc:163): drain the tendril's
	// health; the killing blow destroys it (the whole tendril "breaks off").
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	// Guide-wide MashadarGuide.HurtMashadar bAlreadyHurtThisTick latch.
	bool ConsumeSegmentDamageThisTick();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// MashadarManager.AddMashadar is reached on the first live arena chase, not
	// when a guide/crack actor is constructed.
	void InitializeArenaHealth();
	// Spawn the hidden head segment following the guide, and align the lair anchor
	// (MashadarGuide.uc Initialize).
	void InitChain();
	// Grow/trim the body so its length tracks the guide's distance from the lair while
	// active, and withdraw to just the head when idle (the net effect of the shipped
	// AddNewSegment/retract, minus the invisible per-frame reverse-link choreography).
	void UpdateChain();
	// Number of body segments currently trailing the head (walks the chain).
	int32 CountBodySegments() const;
	AWoTMashadarSegment* SpawnSegment(bool bHead, const FVector& Loc, const FRotator& Rot);
	void AppendTailSegment();
	void RemoveTailSegment();
	int32 GetTrailerIndex(const AWoTMashadarSegment* Segment) const;
	AWoTMashadarSegment* GetTrailerAt(int32 OneBasedIndex) const;
	void BreakOffMashadarAt(AWoTMashadarSegment* Segment);
	// FadeAway the whole chain (guide death / teardown).
	void TeardownChain();

	void HandleDeactivateReaction();
	void HandleUnRetractReaction();
	void HandleSetDestinationReaction();
	void HandleSetFocusPawnAReaction();
	void HandleSetFocusPawnBReaction();
	void HandleBreakOffMashadarAtReaction(int32 OneBasedIndex);
	void ApplyDurableChainState();
	void ResetAtHome();

	UPROPERTY() TObjectPtr<AWoTMashadarSegment> MasterSegment;
	UPROPERTY() TObjectPtr<AWoTMashadarSegment> LastSegment;

	FRotator HidingPlaceRotation = FRotator::ZeroRotator;
	bool bInitialized = false;
	UPROPERTY(ReplicatedUsing = OnRep_FocusPawn) TObjectPtr<AWoTNPCCharacter> FocusPawn;
	UFUNCTION() void OnRep_FocusPawn();
	bool bRetracting = false;
	bool bHeadShouldBeVisible = false;

	uint32 LastClientDeactivateCounter = 0;
	uint32 LastClientUnRetractCounter = 0;
	uint32 LastClientSetDestinationCounter = 0;
	uint32 LastClientSetFocusPawnACounter = 0;
	uint32 LastClientSetFocusPawnBCounter = 0;
	uint32 DeactivateReactionCount = 0;
	uint32 UnRetractReactionCount = 0;
	uint32 SetDestinationReactionCount = 0;
	uint32 SetFocusPawnAReactionCount = 0;
	uint32 SetFocusPawnBReactionCount = 0;
	uint32 BreakOffReactionCount = 0;
	bool bBreakOffHandled = false;
	TWeakObjectPtr<AActor> LastSetDestinationPayload;

	float SearchTimer = 0.0f;
	// MashadarGuide.SeePlayer keeps a 16-entry rotating per-actor retry table.
	// Native perception may present the same player every frame, but Chase/path
	// reachability is evaluated only once per randomized 3-6 second window.
	struct FSeePlayerTimerInfo
	{
		TWeakObjectPtr<AActor> Seen;
		float NextNotifyTime = 0.0f;
	};
	FSeePlayerTimerInfo SeePlayerTimerInfos[16];
	int32 SeePlayerTimerInfoIndex = 0;
	bool bShocked = false;
	uint64 LastSegmentDamageFrame = MAX_uint64;
	TWeakObjectPtr<APawn> ShockPawn;
};
