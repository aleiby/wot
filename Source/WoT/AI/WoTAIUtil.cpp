#include "WoTAIUtil.h"

#include "WoTNPCCharacter.h"
#include "WoTNPCController.h"
#include "../Traps/WoTAlarm.h"
#include "../Inventory/WoTSeal.h"
#include "EngineUtils.h"
#include "Engine/World.h"

namespace
{
	// A candidate paired with its squared gap to the searcher, for a nearest-first
	// sort (the ItemSorter's IR_Origin distance sort, WOTUtil.uc:230-231).
	struct FRankedActor
	{
		float DistSquared;
		AActor* Actor;
	};

	void CollectSortedByDistance(UWorld* World, UClass* Class, const FVector& Origin,
	                             TArray<FRankedActor>& Out)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor->IsA(Class))
			{
				continue;
			}
			Out.Add({static_cast<float>(FVector::DistSquared(Actor->GetActorLocation(), Origin)), Actor});
		}
		Out.Sort([](const FRankedActor& A, const FRankedActor& B) { return A.DistSquared < B.DistSquared; });
	}
}

uint8 WoTAIUtil::GetActorTeam(const AActor* Other)
{
	// WOTUtil.uc:335: pawns → their team byte; seals → Seal.Team; else teamless.
	if (const APawn* Pawn = Cast<APawn>(Other))
	{
		return AWoTNPCCharacter::TeamOf(Pawn);
	}
	if (const AWoTSeal* Seal = Cast<AWoTSeal>(Other))
	{
		return Seal->Team;
	}
	return 255;   // WOTUtil.uc:341 failure sentinel
}

bool WoTAIUtil::GetClosestGoal(AWoTNPCController* Searcher, EWoTGoalIndex Slot,
                               UClass* GoalClass, bool bOnlyUnowned)
{
	UWoTBrainComponent* Brain = Searcher ? Searcher->GetBrain() : nullptr;
	const APawn* Pawn = Searcher ? Searcher->GetPawn() : nullptr;
	if (!Brain || !Pawn || !GoalClass)
	{
		return false;
	}

	TArray<FRankedActor> Ranked;
	CollectSortedByDistance(Pawn->GetWorld(), GoalClass, Pawn->GetActorLocation(), Ranked);

	for (const FRankedActor& Entry : Ranked)
	{
		// WOTUtil.uc:420 acceptance (bOnlySameTeam folded to false): unowned OR
		// same-team. The .uc's `!bOnly || (Owner==None) && !bSame || teamMatch`
		// reduces to this once bSame is false; && binds tighter than ||.
		const bool bAccept = !bOnlyUnowned || Entry.Actor->GetOwner() == nullptr ||
		                     GetActorTeam(Entry.Actor) == GetActorTeam(Pawn);
		if (!bAccept)
		{
			continue;
		}
		Brain->InitGoalWithActor(Slot, Entry.Actor);
		if (Brain->IsGoalNavigable(Slot))
		{
			return true;
		}
	}

	Brain->InvalidateGoal(Slot);
	return false;
}

bool WoTAIUtil::GetClosestUnownedSeal(AWoTNPCController* Searcher, EWoTGoalIndex Slot)
{
	return GetClosestGoal(Searcher, Slot, AWoTSeal::StaticClass(), /*bOnlyUnowned*/ true);
}

bool WoTAIUtil::GetClosestAlarm(AWoTNPCController* Searcher, EWoTGoalIndex Slot)
{
	return GetClosestGoal(Searcher, Slot, AWoTAlarm::StaticClass(), /*bOnlyUnowned*/ true);
}

bool WoTAIUtil::CollectAvailableWotPawns(AWoTNPCController* Searcher, float HelpRadius,
                                         TArray<TWeakObjectPtr<AWoTNPCCharacter>>& OutCandidates)
{
	OutCandidates.Reset();
	const APawn* Pawn = Searcher ? Searcher->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	// WOTUtil.uc:200: the collect radius is HelpRadius + 500 (the //xxxrlo slop).
	const float Reach = HelpRadius + 500.0f;
	const float ReachSquared = Reach * Reach;
	const FVector Origin = Pawn->GetActorLocation();

	TArray<FRankedActor> Ranked;
	CollectSortedByDistance(Pawn->GetWorld(), AWoTNPCCharacter::StaticClass(), Origin, Ranked);
	for (const FRankedActor& Entry : Ranked)
	{
		if (Entry.Actor == Pawn || Entry.DistSquared > ReachSquared)
		{
			continue;
		}
		AWoTNPCCharacter* Candidate = Cast<AWoTNPCCharacter>(Entry.Actor);
		const AWoTNPCController* Controller =
			Candidate ? Cast<AWoTNPCController>(Candidate->GetController()) : nullptr;
		// WOTUtil.uc:207 reject the occupied ones (CanBeGivenNewOrders).
		if (Controller && Controller->CanBeGivenNewOrders())
		{
			OutCandidates.Add(Candidate);
		}
	}
	return OutCandidates.Num() > 0;
}

bool WoTAIUtil::GetNextAvailableWotPawn(AWoTNPCController* Searcher, EWoTGoalIndex Slot,
                                        TArray<TWeakObjectPtr<AWoTNPCCharacter>>& Candidates)
{
	UWoTBrainComponent* Brain = Searcher ? Searcher->GetBrain() : nullptr;
	if (!Brain)
	{
		return false;
	}
	// WOTUtil.uc:229-262: the collected set is already nearest-first; take the
	// first still-available + navigable candidate, assign it, and reject (remove)
	// it so a later call picks the next.
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		AWoTNPCCharacter* Candidate = Candidates[i].Get();
		const AWoTNPCController* Controller =
			Candidate ? Cast<AWoTNPCController>(Candidate->GetController()) : nullptr;
		if (!Controller || !Controller->CanBeGivenNewOrders())
		{
			Candidates.RemoveAt(i--);
			continue;
		}
		Brain->InitGoalWithActor(Slot, Candidate);
		if (Brain->IsGoalNavigable(Slot))
		{
			Candidates.RemoveAt(i);
			return true;
		}
	}
	// WOTUtil.uc:264-268: none available → invalidate the goal.
	Brain->InvalidateGoal(Slot);
	return false;
}

AWoTNPCController* WoTAIUtil::GetLeader(AWoTNPCController* Searcher)
{
	const APawn* Pawn = Searcher ? Searcher->GetPawn() : nullptr;
	if (!Pawn)
	{
		return nullptr;
	}
	// WOTUtil.uc:450: nearest captain that IsLeader() within its LeadershipRadius.
	TArray<FRankedActor> Ranked;
	CollectSortedByDistance(Pawn->GetWorld(), AWoTNPCCharacter::StaticClass(),
	                        Pawn->GetActorLocation(), Ranked);
	for (const FRankedActor& Entry : Ranked)
	{
		const AWoTNPCCharacter* Candidate = Cast<AWoTNPCCharacter>(Entry.Actor);
		if (!Candidate || !Candidate->bIsCaptain || Candidate == Pawn)
		{
			continue;
		}
		AWoTNPCController* Controller = Cast<AWoTNPCController>(Candidate->GetController());
		if (Controller && Controller->IsLeader() &&
		    Entry.DistSquared < Controller->LeadershipRadius * Controller->LeadershipRadius)
		{
			return Controller;
		}
	}
	return nullptr;
}

void WoTAIUtil::InvokeWotPawnFindLeader(AWoTNPCController* Captain)
{
	const APawn* Pawn = Captain ? Captain->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	// WOTUtil.uc:177: ping every WotPawn within LeadershipRadius (excluding self).
	const float Reach = Captain->LeadershipRadius;
	const float ReachSquared = Reach * Reach;
	const FVector Origin = Pawn->GetActorLocation();
	for (TActorIterator<AWoTNPCCharacter> It(Pawn->GetWorld()); It; ++It)
	{
		AWoTNPCCharacter* NPC = *It;
		if (NPC == Pawn || FVector::DistSquared(NPC->GetActorLocation(), Origin) > ReachSquared)
		{
			continue;
		}
		if (AWoTNPCController* Controller = Cast<AWoTNPCController>(NPC->GetController()))
		{
			Controller->FindLeader();
		}
	}
}
