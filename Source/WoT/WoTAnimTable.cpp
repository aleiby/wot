#include "WoTAnimTable.h"
#include "WoTRandom.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

// Activated animation rows are payload-owned. Selection semantics remain native
// (AnimationTableImpl.uc:131-248 and AnimationTableWOT.uc:22-101).

namespace
{
	TUniquePtr<FWoTAnimSet>& InstalledAnimRegistry()
	{
		static TUniquePtr<FWoTAnimSet> Installed;
		return Installed;
	}

	const TMap<FName, TArray<FWoTAnimTableEntry>>& ActiveAnimRegistry()
	{
		const TUniquePtr<FWoTAnimSet>& Installed = InstalledAnimRegistry();
		if (Installed) return Installed->Tables;
		static const TMap<FName, TArray<FWoTAnimTableEntry>> Empty;
		return Empty;
	}
}

void FWoTAnimTableBuilder::Install(FWoTAnimSet&& Set)
{
	TUniquePtr<FWoTAnimSet> Candidate = MakeUnique<FWoTAnimSet>(MoveTemp(Set));
	Swap(InstalledAnimRegistry(), Candidate);
}

#if WITH_AUTOMATION_TESTS
void FWoTAnimTableBuilder::Reset()
{
	InstalledAnimRegistry().Reset();
}
#endif

namespace WoTAnimTable
{
	const TArray<FWoTAnimTableEntry>* Find(FName TableName)
	{
		return TableName.IsNone() ? nullptr : ActiveAnimRegistry().Find(TableName);
	}

	void GetAllTableNames(TArray<FName>& OutNames)
	{
		ActiveAnimRegistry().GetKeys(OutNames);
	}

	bool AnimCanBeUsed(const AActor& Source, const FWoTAnimTableEntry& Entry)
	{
		if (Entry.EndVector.IsNearlyZero())
		{
			return true;
		}
		UWorld* World = Source.GetWorld();
		if (!World)
		{
			return true;
		}
		// AnimationTableWOT.uc:26-37: rotate the authored vector by the actor's facing;
		// its size is the shift in units of collision radius (RadiusMultiplier = 1.0).
		// util.CanActorMoveTo becomes a collision-shape sweep against blocking geometry.
		const FVector Dir = Source.GetActorRotation().RotateVector(Entry.EndVector);
		float Radius = 0.0f, HalfHeight = 0.0f;
		Source.GetSimpleCollisionCylinder(Radius, HalfHeight);
		const float Distance = Dir.Size() * Radius;
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			return true;
		}
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTAnimCanBeUsed), false, &Source);
		const FVector Start = Source.GetActorLocation();
		const FVector End = Start + Dir.GetSafeNormal() * Distance;
		return !World->SweepTestByChannel(Start, End, FQuat::Identity, ECC_Pawn,
		                                  FCollisionShape::MakeCapsule(Radius, HalfHeight),
		                                  Params);
	}

	const FWoTAnimTableEntry* PickSlotAnim(FName TableName, FName SlotOrAnim,
	                                       const AActor* Source, bool bNotSlot)
	{
		const TArray<FWoTAnimTableEntry>* Table = Find(TableName);
		if (!Table || SlotOrAnim.IsNone())
		{
			return nullptr;
		}

		// Scan for matches (AnimationTableImpl.uc:146-183; MatchingEntries[10] cap).
		constexpr int32 MaxMatches = 10;
		TArray<const FWoTAnimTableEntry*, TInlineAllocator<MaxMatches>> Matches;
		float TotalOdds = 0.0f;
		for (const FWoTAnimTableEntry& Entry : *Table)
		{
			if (bNotSlot ? (Entry.AnimName == SlotOrAnim) : (Entry.Slot == SlotOrAnim))
			{
				Matches.Add(&Entry);
				TotalOdds += Entry.Odds;
				if (bNotSlot || Matches.Num() >= MaxMatches)
				{
					break;
				}
			}
		}
		if (Matches.Num() == 0)
		{
			return nullptr;
		}

		// Single match: its odds gate whether it plays at all (unless bNotSlot picked
		// an exact anim) — AnimationTableImpl.uc:187-197.
		if (Matches.Num() == 1)
		{
			return (bNotSlot || WoTRandom::FRand() < TotalOdds) ? Matches[0] : nullptr;
		}

		// Death picks strip anims that would clip geometry; keep all if every one
		// would strip (AnimationTableWOT.uc:44-101).
		static const FName DeathSlot(TEXT("Death"));
		if (SlotOrAnim == DeathSlot && Source)
		{
			decltype(Matches) Usable;
			float UsableOdds = 0.0f;
			for (const FWoTAnimTableEntry* Entry : Matches)
			{
				if (AnimCanBeUsed(*Source, *Entry))
				{
					Usable.Add(Entry);
					UsableOdds += Entry->Odds;
				}
			}
			if (Usable.Num() > 0 && Usable.Num() < Matches.Num())
			{
				Matches = Usable;
				TotalOdds = UsableOdds;
			}
			if (Matches.Num() == 1)
			{
				return Matches[0];
			}
		}

		// Weighted pick (AnimationTableImpl.uc:210-223). An all-zero-odds multi-match
		// slot picks the FIRST entry, exactly like the spec (its `TotalOdds >= 0.0`
		// guard passes, RandVal = 0, and the cumulative walk stops at entry 0) — no
		// shipped table authors one, but stay byte-faithful.
		const float RandVal = WoTRandom::FRand() * TotalOdds;
		float Accum = 0.0f;
		for (const FWoTAnimTableEntry* Entry : Matches)
		{
			Accum += Entry->Odds;
			if (Accum >= RandVal)
			{
				return Entry;
			}
		}
		return Matches.Last();
	}

	FVector FindAnimEndVector(FName TableName, FName AnimName)
	{
		if (const TArray<FWoTAnimTableEntry>* Table = Find(TableName))
		{
			for (const FWoTAnimTableEntry& Entry : *Table)
			{
				if (Entry.AnimName == AnimName)
				{
					return Entry.EndVector;   // first match, like GetAnimEndVector
				}
			}
		}
		return FVector::ZeroVector;
	}

	float ResolveRate(const FWoTAnimTableEntry* Entry)
	{
		return (Entry && Entry->Rate > KINDA_SMALL_NUMBER) ? Entry->Rate : 1.0f;
	}

	float ResolveTweenTime(const FWoTAnimTableEntry* Entry, float CallerTween)
	{
		if (CallerTween > KINDA_SMALL_NUMBER)
		{
			return CallerTween;
		}
		if (Entry && Entry->TweenTime > KINDA_SMALL_NUMBER)
		{
			return Entry->TweenTime;
		}
		return 0.2f;   // DefaultAnimTweenToTime (AnimationTableInterf.uc:70)
	}
}
