#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "WoTMatchParticipant.h"
#include "WoTBotCastPolicy.h"
#include "WoTBotController.generated.h"

class UWoTCasterComponent;
class UWoTBotRoster;
namespace WoTBotLinks { struct FDistantTeleport; }
class AWoTAngrealInventory;
class AWoTAngrealSpawnPool;
class AWoTResourceSpawnerInv;
#if WITH_AUTOMATION_TESTS
namespace WoTBotTest
{
struct FWoTBotRegressionAccess;
struct FWoTBotCastCompletionAccess;
struct FWoTBotBehaviourAccess;
struct FWoTBotDecisionAccess;
struct FWoTBotTraversalAccess;
struct FWoTBotRosterAccess;
struct FWoTBotPoolAccess;
struct FWoTBotSeekingAccess;
}
#endif

enum class EWoTBotState : uint8 { Roam, SeekItem, Engage, Dead, Frozen };

struct FWoTBotPoolBelief
{
	float LastPerceived = -1e9f;
	TWeakObjectPtr<UClass> Contents;
	bool bEmpty = false;
	uint8 Rarity = 0;
	float EstimatedRestock = -1;
	float EmptyRecovery = -1;
	float Delay = 0;
	float VisibleSince = -1;
	uint64 ObservationOrder = 0;
};

UCLASS()
class AWoTBotController : public AAIController, public IWoTMatchParticipant
{
	GENERATED_BODY()
public:
	AWoTBotController();
	void InitializeIdentity(UWoTBotRoster* InRoster, UClass* PlayerClass, const FString& Name);
	void Start(bool bInitiallyIdle);
	void Restart();
	void SilentRespawn();
	void Retire();
	void SetDeparturePending(bool bPending);
	void SetIdle(bool bNewIdle);
	bool IsIdle() const { return bIdle; }
	virtual bool CanCollectPickups() const override;
	bool IsDeparturePending() const { return bDeparturePending; }
	bool IsFrozen() const { return bFrozen; }
	EWoTBotState GetBehaviourState() const { return State; }
	AWoTCharacter* GetTarget() const { return Target.Get(); }
	bool IsTargetVisible() const { return bTargetVisible; }
	FVector GetLastKnownPosition() const { return LastKnownPosition; }
	FVector GetDestination() const { return Destination; }
	AWoTAngrealSpawnPool* GetSeekingPool() const { return SeekingPool.Get(); }
	bool IsNavigationUnavailable() const { return bNavigationUnavailable; }
	uint64 GetThinkCount() const { return ThinkCount; }
	uint64 GetRepathCount() const { return RepathCount; }
	uint64 GetClimbJumpCount() const { return ClimbJumps; }
	uint64 GetHopCount() const { return Hops; }
	float GetLastJumpTime() const { return LastJumpTime; }
	float GetAimErrorLimit() const;
	bool CanSee(const AWoTCharacter* Candidate) const;
	virtual void Tick(float DeltaSeconds) override;
	virtual void UpdateControlRotation(float DeltaTime, bool bUpdatePawn = true) override;
	virtual void OnParticipantPawnDied(AWoTCharacter* DeadPawn, float RespawnDelaySeconds) override;
	virtual void OnMatchEnded() override;
protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
#if WITH_AUTOMATION_TESTS
	friend struct WoTBotTest::FWoTBotRegressionAccess;
	friend struct WoTBotTest::FWoTBotCastCompletionAccess;
	friend struct WoTBotTest::FWoTBotBehaviourAccess;
	friend struct WoTBotTest::FWoTBotDecisionAccess;
	friend struct WoTBotTest::FWoTBotTraversalAccess;
	friend struct WoTBotTest::FWoTBotRosterAccess;
	friend struct WoTBotTest::FWoTBotPoolAccess;
	friend struct WoTBotTest::FWoTBotSeekingAccess;
#endif
	void InventoryPulse();
	void ResetBehaviour(AWoTCharacter* Body);
	void StartThinking();
	void CancelCasting();
	void SuspendMovement();
	void SpawnBody();
	void Think();
	bool CanAct() const;
	bool IsHostile(const AWoTCharacter* Candidate) const;
	void Perceive();
	void SetTarget(AWoTCharacter* NewTarget, bool bVisible);
	void DamageApplied(AWoTCharacter* Victim, AController* InstigatorController, AActor* Causer, float Amount);
	UFUNCTION()
	void BodyMovementUpdated(float DeltaSeconds, FVector OldLocation, FVector OldVelocity);
	void UpdateAim();
	// Whether a whirlwind holds the body: its effect lifts and spins the body and
	// disables its movement until it lets go.
	bool IsHeldByWhirlwind() const;
	static bool HasMovementHold(const AWoTCharacter* Body);
	void UpdateStuck();
	void UpdateClimb();
	void Hop(const AWoTAngrealSpawnPool* Pool);
	void ChooseDestination(bool bForce = false);
	bool NavigateTo(const FVector& Point);
	AWoTAngrealSpawnPool* BestPool(float& Score, bool bRoaming = false);
	void ReleasePoolGoal();
	void RememberNearbyPools();
	void InitializePoolPerception();
	void PoolActorSpawned(AActor* Actor);
	void PerceivePools();
	bool CanSeePool(const AWoTAngrealSpawnPool* Pool) const;
	bool CanSeeStimulus(const AActor* Actor) const;
	void ObservePool(AWoTAngrealSpawnPool* Pool, bool bVisit);
	float PoolStockProbability(const AWoTAngrealSpawnPool* Pool) const;
	float PoolUtility(const AWoTAngrealSpawnPool* Pool, int32 OffensiveKinds) const;
	float PoolRespawnDelay(const AWoTAngrealSpawnPool* Pool) const;
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, FWoTBotPoolBelief> PoolBeliefs;
	TArray<TWeakObjectPtr<AWoTAngrealSpawnPool>> PerceptionPools;
	int32 PoolSightCursor = 0;
	FRandomStream PoolRandom;
	FDelegateHandle PoolSpawnHandle;
	uint64 PoolPerceptionOrder = 0;
	struct FPendingPoolSound
	{
		TWeakObjectPtr<AActor> Actor;
		FVector Listener;
		float Time;
		uint64 Order;
	};
	TArray<FPendingPoolSound> PendingPoolSounds;
	void OffensiveSupply(int32& Kinds, int32& Charges) const;
	float FightScore() const;
	bool HasLiveHelper() const;
	AWoTResourceSpawnerInv* ReadySummon() const;
	void CastArtifacts();
	AActor* FindVisibleIncomingProjectile() const;
	AWoTAngrealInventory* SelectArtifact(AActor* Victim, bool bDefensive);
	void CastItem(AWoTAngrealInventory* Item);
	void ObserveCastUse();
	void UpdateCastCompletion();
	AWoTAngrealInventory* MovementOffence() const;
	float StandoffRange() const;
	// The shortest complete route from From to To through a teleporter whose reach
	// no mesh link carries, and the route's length; null when none. Legs from From
	// to each entry are measured once and kept in EntryLegs when given.
	const WoTBotLinks::FDistantTeleport* DistantTeleportRoute(const FVector& From, const FVector& To, float& Length, TArray<float>* EntryLegs = nullptr) const;
	// Walks to a point on the mesh far enough beyond Reach of the teleporter's pad
	// that the walk, wherever it stops, ends off the pad.
	bool StepOffPad(const WoTBotLinks::FDistantTeleport& Via, float Reach);
	EWoTBotState State = EWoTBotState::Dead;
	TWeakObjectPtr<AWoTCharacter> Target;
	TWeakObjectPtr<AWoTAngrealSpawnPool> SeekingPool;
	// Pools collected from, found empty or reached recently (kept across respawns).
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, float> PoolVisits;
	// Failed movement is independent of what was last heard or seen in a pool.
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, float> RecoveryPools;
	// Pools with no complete path from this body, retried after a delay.
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, float> UnreachablePools;
	// Pools reached but not collected, even by jumping for them (kept across respawns).
	TMap<TWeakObjectPtr<AWoTAngrealSpawnPool>, float> UncollectablePools;
	// The pool this body is jumping for, how often, and when it last left the ground.
	TWeakObjectPtr<const AWoTAngrealSpawnPool> HopPool;
	int32 HopCount = 0;
	float HopTime = -1e9f;
	float NextJumpTime = 0.0f;
	float ClimbBlockedSince = -1.0f;
	// Where a teleporter (or a swap) last set this body down.
	FVector ArrivalLocation = FVector::ZeroVector;
	bool bArrivedByTeleport = false;
	float LastCombatProgress = -1e9f;
	float LastSeenTargetHealth = 1.0f;
	TWeakObjectPtr<AWoTAngrealInventory> AimingItem;
	FWoTBotCastHold CastHold;
	float LastDamagedTime = -1e9f;
	FVector LastKnownPosition = FVector::ZeroVector;
	FVector Destination = FVector::ZeroVector;
	FVector ProgressPosition = FVector::ZeroVector;
	FVector2D AimDisk = FVector2D::ZeroVector;
	float LastSensedTime = -1e9f;
	float VisibleSince = 0.0f;
	FVector DamageSearchDirection = FVector::ZeroVector;
	float DamageSearchUntil = -1.0f;
	TWeakObjectPtr<AWoTCharacter> DamageSightCandidate;
	float DamageSightSince = 0.0f;
	FVector FeltVelocity = FVector::ZeroVector;
	float NextRepathTime = 0.0f;
	float LastProgressTime = 0.0f;
	float LastRecoveryTime = 0.0f;
	float LastOffensiveCastTime = -1e9f;
	float LastDefensiveCastTime = -1e9f;
	int32 ObservedUses = 0;
	bool bTargetVisible = false;
	bool bWantsMovement = false;
	bool bNavigationUnavailable = false;
	uint64 ThinkCount = 0;
	uint64 RepathCount = 0;
	uint64 ClimbJumps = 0;
	uint64 Hops = 0;
	float LastJumpTime = -1e9f;
	FVector LastBodyLocation = FVector::ZeroVector;
	void ScheduleRespawn(float Delay);
	void DestroyLivePawn();
	void DestroyRespawnCorpse();
	void RetireSummons();
	TWeakObjectPtr<UWoTBotRoster> Roster;
	TWeakObjectPtr<UWoTCasterComponent> Caster;
	TArray<TWeakObjectPtr<AWoTCharacter>> OwnedPawns;
	// The hidden dead body kept until the respawn replaces it, as a player
	// controller keeps RespawnCorpse; player bodies are never auto-destroyed.
	TWeakObjectPtr<AWoTCharacter> RespawnCorpse;
	FTimerHandle RespawnTimer;
	FTimerHandle ThinkTimer;
	bool bDeparturePending = false;
	bool bIdle = false;
	uint8 IdleMovementMode = 0;
	uint8 IdleCustomMovementMode = 0;
	bool bIdleMovementTickEnabled = true;
	bool bFrozen = false;
	bool bRetiring = false;
};
