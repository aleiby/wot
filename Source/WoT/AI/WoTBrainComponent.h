#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTBrainComponent.generated.h"

// The NPC decision core: the nine goal slots, their spatial-query cache, the
// priority selection, and the duration-notifier pulse table. Port of the
// LegendPawn goal/notifier machinery (LegendPawn.uc:24-68 enums, :167-534 goal
// API, defaultproperties table) with the native behavior implemented in C++.
//
// Structural divergence (PORT_NOTES): the original shared
// goal objects between pawns via 16-slot per-invoker contexts; here every pawn
// owns its nine goals outright, so a context collapses into the goal struct
// itself (one invoker == one cache). Goal-to-goal assignment copies target +
// cache, preserving the original's AssignContexts semantics. The 16-invoker
// cap (and its silent overflow) disappears by construction.

// The nine goal slots (LegendPawn.uc:24). Declaration order is ALSO the
// priority tiebreak order — equal priorities resolve to the lowest index
// (locked by spec WoT.AI.GoalPriority).
UENUM()
enum class EWoTGoalIndex : uint8
{
	Threat,
	ExternalDirective,   // SP scripted sequences — slot exists, unpopulated in MP
	Refuge,
	Guarding,
	Colleague,
	Intermediate,
	Waiting,
	CurrentWaypoint,     // mechanical input to movement, never priority-selected
	CurrentFocus,        // mechanical look-at, never priority-selected
	None
};
static constexpr int32 WoTGoalSlotCount = 9;

// Distance gate on a goal's priority (GoalBase.uc:1050): the priority is a
// step function of distance to the goal.
UENUM()
enum class EWoTGoalDistanceUsage : uint8
{
	Unused,           // ungated
	ZeroIfGreater,    // goal only matters within PriorityDistance
	ZeroIfLess        // goal only matters beyond PriorityDistance
};

// Duration-notifier slots (LegendPawn.uc:56; cadences from the CDO,
// LegendPawn.uc:2634-2637). Period 0 = fire every tick; -1 = never.
UENUM()
enum class EWoTNotifierSlot : uint8
{
	Visibility,   // 2.0s   show-self perception pulse
	Rotation,     // 0.0    per-tick re-aim while enabled
	Movement,     // 0.0    per-tick re-accelerate while enabled
	Defensive,    // 0.05s  dodge-detection sampling
	Offensive,    // 0.5s   attack-attempt cadence
	Waiting,      // 0.25s  idle re-evaluation
	Count UMETA(Hidden)
};
static constexpr int32 WoTNotifierSlotCount = 6;

// One success/failure pair per spatial question (GoalCacheInfo.uc:120).
enum class EWoTGoalCacheItem : uint8
{
	ReachableOK, ReachableFail,
	VisibleOK,   VisibleFail,
	PathableOK,  PathableFail,
	Count
};

// A cached spatial answer: where the goal and the observer stood when the
// answer was computed (GoalCacheInfo.uc:130).
USTRUCT()
struct FWoTGoalCacheItem
{
	GENERATED_BODY()

	bool bValid = false;
	FVector GoalLocation = FVector::ZeroVector;
	FVector ObserverLocation = FVector::ZeroVector;
};

// The six-item spatial cache. Currency rule:
// an item is current while the goal (and, for Visible/Reachable only, the
// observer) has drifted less than 32 units since it was stamped. Pathable
// ignores observer motion — a fresh path stays current while the pawn walks
// it, and is recomputed only when the goal moves.
USTRUCT()
struct FWoTGoalCache
{
	GENERATED_BODY()

	static constexpr float CurrentThreshold = 32.0f;

	FWoTGoalCacheItem Items[static_cast<int32>(EWoTGoalCacheItem::Count)];

	FWoTGoalCacheItem& Get(EWoTGoalCacheItem Item) { return Items[static_cast<int32>(Item)]; }
	const FWoTGoalCacheItem& Get(EWoTGoalCacheItem Item) const { return Items[static_cast<int32>(Item)]; }

	// Stamp an answer; the sibling of the pair is invalidated so a stale
	// opposite answer can never shadow a fresh one.
	void Stamp(EWoTGoalCacheItem Item, const FVector& GoalLoc, const FVector& ObserverLoc);

	// Is this item still current for the given present-day locations?
	// bCheckObserver = false for the Pathable pair.
	bool IsCurrent(EWoTGoalCacheItem Item, const FVector& GoalLoc, const FVector& ObserverLoc,
	               bool bCheckObserver) const;

	// Tri-state cached answer for a question pair: 1 = cached true,
	// 0 = cached false, -1 = miss (run the real query).
	int32 CachedAnswer(EWoTGoalCacheItem OKItem, const FVector& GoalLoc,
	                   const FVector& ObserverLoc, bool bCheckObserver) const;

	void InvalidatePair(EWoTGoalCacheItem OKItem, EWoTGoalCacheItem FailItem)
	{
		Get(OKItem).bValid = false;
		Get(FailItem).bValid = false;
	}

	void Invalidate();
};

// One goal slot: the target (actor XOR location), the priority/speed knobs the
// factory table configures, and the invoker's cache (GoalAbstracterImpl.uc:11).
USTRUCT()
struct FWoTGoal
{
	GENERATED_BODY()

	// True when this slot has a goal object at all (descriptor != None).
	// A slot without one scores the -1 sentinel and is never selected.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bSlotUsed = false;

	// Target: exactly one of actor / vector is valid at a time.
	TWeakObjectPtr<AActor> GoalActor;
	FVector GoalVector = FVector::ZeroVector;
	bool bUseGoalVector = false;

	// A dead pawn target is an invalid goal (set for 'Pawn' descriptors —
	// GoalFactory.uc:1116).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bPawnsMustBeAlive = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Priority = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float PriorityDistance = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	EWoTGoalDistanceUsage DistanceUsage = EWoTGoalDistanceUsage::Unused;

	// Walk-speed hint fed to movement while pursuing this goal (uu/s).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float SuggestedSpeed = 0.0f;

	// Facing to adopt on arrival (a waiting pawn faces its spawn rotation).
	FRotator AssociatedRotation = FRotator::ZeroRotator;
	bool bUseAssociatedRotation = false;

	FWoTGoalCache Cache;

	bool HasActor() const { return GoalActor.IsValid() && !bUseGoalVector; }
	bool HasVector() const { return !GoalActor.IsValid() && bUseGoalVector; }
};

// A timed pulse that fires a named callback on the brain's owner.
// Period 0 fires every tick;
// -1 never fires. Enabling seeds a random phase so a crowd's pulses desync.
USTRUCT()
struct FWoTDurationNotifier
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Period = -1.0f;

	float Elapsed = 0.0f;
	bool bEnabled = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bRandomizePhase = true;

	// Advance and report whether the pulse fires this tick.
	bool Tick(float DeltaSeconds);
};

DECLARE_MULTICAST_DELEGATE_OneParam(FWoTNotifierPulse, EWoTNotifierSlot);

UCLASS(ClassGroup = (WoT))
class UWoTBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTBrainComponent();

	// --- Goal slots ---

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FWoTGoal Goals[WoTGoalSlotCount];

	// The slot the FSM is currently acting on (LegendPawn.uc CurrentGoalIdx).
	EWoTGoalIndex CurrentGoalIndex = EWoTGoalIndex::Threat;

	FWoTGoal& GetGoal(EWoTGoalIndex Index) { return Goals[static_cast<int32>(Index)]; }
	const FWoTGoal& GetGoal(EWoTGoalIndex Index) const { return Goals[static_cast<int32>(Index)]; }

	// InitGoalWithObject / InitGoalWithVector (LegendPawn.uc:986,1002). The
	// original's optional cache pre-seed hints shipped disabled (commented
	// 'xxxrloperf') and are ported as the same no-ops.
	void InitGoalWithActor(EWoTGoalIndex Index, AActor* Target);
	void InitGoalWithVector(EWoTGoalIndex Index, const FVector& Target);
	void InvalidateGoal(EWoTGoalIndex Index);

	// Copy another slot's target + cache into this slot — the original's
	// goal-to-goal AssignObject, cache preserved.
	// Threat assignment also runs GoalInitialized after the cache copy, matching
	// retail InitGoalWithObject when a grunt adopts its captain's threat.
	void AssignGoalFrom(EWoTGoalIndex Index, const UWoTBrainComponent* Other, EWoTGoalIndex OtherIndex);

	// TransferThreatGoalsFrom (LegendPawn.uc:656): adopt Other's Threat goal, but
	// only when it is currently valid for Other — a pawn hearing an engaged
	// friendly inherits WHO it is fighting. Initializes our Threat slot with the
	// target actor (retail InitGoalWithObject(GI_Threat, Other.GetGoal(GI_Threat)));
	// no-op on an invalid/vector source. Returns whether a threat was adopted.
	bool TransferThreatGoalsFrom(const UWoTBrainComponent* Other);

	// SetWaitingGoal (LegendPawn.uc:1448): wait at a spot, adopt a facing there.
	void SetWaitingGoal(const FVector& Location, const FRotator& Facing);

	// --- Validity / priority ---

	bool IsGoalValid(EWoTGoalIndex Index) const;

	// The distance-gated step function. Invalid goals score 0.
	float GetGoalPriority(EWoTGoalIndex Index) const;

	// All nine slots, sorted descending by priority; empty slots score the -1
	// sentinel. Equal priorities keep slot order (lowest EWoTGoalIndex first) —
	// the locked tiebreak.
	void GetSortedGoalIndices(TArray<EWoTGoalIndex>& OutIndices, TArray<float>& OutPriorities) const;

	// Highest-priority slot right now (GetPriorityGoalIndex, LegendPawn.uc:514).
	EWoTGoalIndex GetPriorityGoalIndex() const;

	// --- Spatial queries (cached) ---

	// Cylinder gap from the owner to the goal, 2D. False if not computable.
	bool GetGoalDistance(EWoTGoalIndex Index, float& OutDistance) const;
	bool GetGoalDistanceToCylinder(EWoTGoalIndex Index, const FVector& Origin,
		float Radius, float HalfHeight, float& OutDistance) const;
	bool GetGoalCylinder(EWoTGoalIndex Index, FVector& Origin, float& Radius, float& Height) const
	{ return GetGoalParams(Index, Origin, Radius, Height); }
	static float DistanceBetweenCylinders(const FVector& A, float RadiusA, float HalfA,
		const FVector& B, float RadiusB, float HalfB);

	bool IsGoalVisible(EWoTGoalIndex Index);
	bool IsGoalReachable(EWoTGoalIndex Index);   // gated on visibility
	bool IsGoalPathable(EWoTGoalIndex Index);
	bool IsGoalNavigable(EWoTGoalIndex Index);   // reachable || pathable
	bool IsGoalReached(EWoTGoalIndex Index) const;

	// Where the goal was last seen (the VisibleOK stamp's goal location).
	bool GetLastVisibleLocation(EWoTGoalIndex Index, FVector& OutLocation) const;

	// The concrete point to move toward this iteration: hold when reached, a
	// contact point when directly reachable, the goal location when routed
	// (the path-node hop collapses into NavMesh path following).
	bool GetNavigationGoalLocation(EWoTGoalIndex Index, FVector& OutLocation);

	// The reached predicate: 2D distance vs the product of the two cylinder
	// radii over the 15uu engagement unit.
	static bool IsCloseEnough(const FVector& A, float RadiusA, const FVector& B, float RadiusB);

	// --- Stuck-move strikes ---

	// Report a finished move segment. A clean arrival resets the strike count;
	// a give-up (blocked / timed out) is one strike: the first drops the
	// current waypoint, the second drops the current goal and resets.
	void NotifyMoveResult(bool bReachedCleanly);

	int32 GetNavigationStrikes() const { return NavigationStrikes; }

	// --- Duration notifiers ---

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FWoTDurationNotifier Notifiers[WoTNotifierSlotCount];

	FWoTNotifierPulse OnNotifierPulse;

	// Pulse the notifier table. The controller calls this FIRST each tick,
	// before any state work — callbacks resolve at the top of the frame.
	void ReflectTick(float DeltaSeconds);

	void SetNotifierEnabled(EWoTNotifierSlot Slot, bool bEnabled);
	bool IsNotifierEnabled(EWoTNotifierSlot Slot) const;

	// Enable exactly this set, disable the rest (the per-state notifier sets).
	void SetActiveNotifiers(std::initializer_list<EWoTNotifierSlot> Slots);

protected:
	// Resolve a goal target to a cylinder. Location
	// targets take the fallback radius 225/ownerRadius and half-height 8.
	bool GetGoalParams(EWoTGoalIndex Index, FVector& OutLocation, float& OutRadius, float& OutHalfHeight) const;

	bool GetOwnerCylinder(FVector& OutLocation, float& OutRadius, float& OutHalfHeight) const;

	// The navigable spot standing in for UE1 actorReachable's FindSpot push-out:
	// the navmesh point nearest where the owner would touch an actor goal.
	bool GetGoalStandSpot(EWoTGoalIndex Index, FVector& OutSpot) const;

	// Uncached primitives. Virtual so headless
	// specs (no navmesh) can force the navigability answers.
	virtual bool QueryVisible(const FWoTGoal& Goal) const;    // LOS trace, eye to target
	virtual bool QueryReachable(EWoTGoalIndex Index) const;   // straight-line NavMesh raycast
	virtual bool QueryPathable(EWoTGoalIndex Index) const;    // a route exists (partial counts)

private:
	int32 NavigationStrikes = 0;
};
