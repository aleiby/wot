#pragma once

#include "CoreMinimal.h"
#include "WoTBrainComponent.h"   // EWoTGoalIndex

class AActor;
class AWoTNPCCharacter;
class AWoTNPCController;

// Port of the WOTUtil radius-scan / sorter statics (exports/script/WOT/WOTUtil.uc)
// the Captain orders layer leans on. The shipped helpers route through
// ItemCollector/ItemSorter (radius collect + distance sort + accept/reject);
// folded here to direct TActorIterator scans + a distance sort, which reproduce
// the observable selection (nearest navigable candidate) without the aggregate
// objects. Follows the WoTSight namespace-of-statics shape.
//
// Goal-setting helpers assign into the searcher's brain slot and gate on the
// brain's IsGoalNavigable (forced open in the headless test brain), matching
// GetClosestGoal / GetNextAvailableWotPawn (WOTUtil.uc:424-425,:247-248).
namespace WoTAIUtil
{
	// WOTUtil.uc:335 GetActorTeam — the polymorphic team read. Pawns present
	// their team byte (AWoTNPCCharacter::TeamOf), seals present Seal.Team; other
	// actors return the teamless sentinel (255) rather than asserting.
	uint8 GetActorTeam(const AActor* Other);

	// WOTUtil.uc:399 GetClosestGoal: the nearest instance of GoalClass that is
	// accepted (unowned OR same-team when bOnlyUnowned; the || same-team clause is
	// the .uc's operator-precedence behavior with bOnlySameTeam=false) AND
	// navigable from the searcher. Assigns it into Slot; invalidates Slot if none.
	bool GetClosestGoal(AWoTNPCController* Searcher, EWoTGoalIndex Slot,
	                    UClass* GoalClass, bool bOnlyUnowned);

	// WOTUtil.uc:311 GetClosestUnownedSeal → GetClosestGoal(Seal, unowned).
	bool GetClosestUnownedSeal(AWoTNPCController* Searcher, EWoTGoalIndex Slot);

	// WOTUtil.uc:325 GetClosestAlarm → GetClosestGoal(Alarm, unowned). NOTE: the
	// shipped helper IGNORES its AlarmRadius argument (it forwards only the class +
	// unowned flag to GetClosestGoal), so the alarm search is unbounded — the
	// closest navigable alarm on the map, regardless of AlarmSearchRadius. Preserved.
	bool GetClosestAlarm(AWoTNPCController* Searcher, EWoTGoalIndex Slot);

	// WOTUtil.uc:194 CollectAvailableWotPawns: WotPawns within HelpRadius+500 (the
	// //xxxrlo slop) that CanBeGivenNewOrders. Returns whether any remain. The
	// collected set persists on the Captain as the reinforcements cursor.
	bool CollectAvailableWotPawns(AWoTNPCController* Searcher, float HelpRadius,
	                              TArray<TWeakObjectPtr<AWoTNPCCharacter>>& OutCandidates);

	// WOTUtil.uc:223 GetNextAvailableWotPawn: from the collected set (nearest
	// first), the first still-available + navigable pawn, assigned into Slot and
	// removed from the set (the sorter reject). Invalidates Slot + returns false if
	// none.
	bool GetNextAvailableWotPawn(AWoTNPCController* Searcher, EWoTGoalIndex Slot,
	                             TArray<TWeakObjectPtr<AWoTNPCCharacter>>& Candidates);

	// WOTUtil.uc:450 GetLeader: the nearest captain that IsLeader() and whose gap
	// to the searcher is within its own LeadershipRadius. Null if none.
	AWoTNPCController* GetLeader(AWoTNPCController* Searcher);

	// WOTUtil.uc:177 InvokeWotPawnFindLeader: ping every WotPawn within the
	// captain's LeadershipRadius (excluding itself) to (re)pick its leader.
	void InvokeWotPawnFindLeader(AWoTNPCController* Captain);
}
