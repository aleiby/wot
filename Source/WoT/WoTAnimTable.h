#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "Engine/DataTable.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTAnimTable.generated.h"

class FWoTRuntimeClassDefaults;

// One AnimList entry of a shipped AnimationTable*.uc (AnimationTableInterf.uc:48-65
// AnimInfoT): the slot it answers to, its weighted odds within that slot, the
// death-filter end vector, and the engine play parameters. GameType is dropped —
// AnimIsValidForGameType always returns true and no shipped table sets it
// (AnimationTableImpl.uc:25-28); see PORT_NOTES.
USTRUCT()
struct FWoTAnimTableEntry
{
	GENERATED_BODY()

	// Authored AnimList index (the tables are sparse 50-slot arrays; order is the
	// scan order and TrollocSideArm merges onto Trolloc by this index).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 Index = 0;

	// Anim category ('MeleeAttack', 'Death', ...). A Name in the original, not an enum.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName Slot;

	// Odds of this anim within its slot. With a single matching entry the odds are a
	// play-probability gate; with several they are weighted-pick weights
	// (AnimationTableImpl.uc:185-231).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Odds = 0.0f;

	// Direction (relative to forward, in units of collision radius) the mesh sticks
	// out the most; zero = never filtered. Death-slot picks drop entries whose end
	// vector runs into geometry (AnimationTableWOT.uc:22-101).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FVector EndVector = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName AnimName;

	// Play-rate multiplier on the sequence's authored fps. ~0 = unset -> 1.0 at
	// play time (DefaultAnimRate, AnimationTableImpl.uc:37-41).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Rate = 0.0f;

	// Tween-in seconds. ~0 = unset -> 0.2 at play time (DefaultAnimTweenToTime,
	// AnimationTableInterf.uc:70). Carried for the blend-in pass; unused until then.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float TweenTime = 0.0f;
};

// One whole AnimationTable as a DataTable row (row name = table name), mirroring the
// creature registry's CSV-swappable shape (WoTCreatureData.h:23-26).
USTRUCT()
struct FWoTAnimTableRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TArray<FWoTAnimTableEntry> Entries;
};

/** A complete, generation-local animation-table family built from retail CDOs. */
struct FWoTAnimSet
{
	TMap<FName, TArray<FWoTAnimTableEntry>> Tables;
};

/** Pure construction and explicit publication boundary used by payload activation. */
struct FWoTAnimTableBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("anim-tables");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTAnimSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTAnimSet& Set);
	static void Install(FWoTAnimSet&& Set);

	// Stable canonical bytes and structural equivalence support for fixtures.
	static void SerializeCanonical(const FWoTAnimSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTAnimSet& Built,
		const FWoTAnimSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// Activated animation-table rows plus the PickRandomSlotAnim selection
// semantics (AnimationTableImpl.uc:131-248).
namespace WoTAnimTable
{
	// The entry list for a table name ("Trolloc", "Soldier", ...). Null for unknown
	// names; the deliberately-empty MashFocusPawn table returns a valid empty array.
	const TArray<FWoTAnimTableEntry>* Find(FName TableName);

	// Every registered table name, for specs.
	void GetAllTableNames(TArray<FName>& OutNames);

	// PickRandomSlotAnim: scan entries whose Slot matches (capped at 10 matches like
	// the original's MatchingEntries[10]), then a single match plays only if
	// FRand() < its odds (unless bNotSlot), several matches weighted-pick by odds.
	// Death-slot picks geometry-filter via AnimCanBeUsed when Source is given (all
	// kept if every candidate would strip). bNotSlot treats SlotOrAnim as an exact
	// anim name. Returns null when nothing matches or the single-entry odds gate
	// rolls "skip".
	const FWoTAnimTableEntry* PickSlotAnim(FName TableName, FName SlotOrAnim,
	                                       const AActor* Source = nullptr,
	                                       bool bNotSlot = false);

	// AnimCanBeUsed (AnimationTableWOT.uc:22-38): true when the entry's end vector is
	// zero or the actor's collision can sweep that far (rotated by actor rotation,
	// distance = |EndVector| * collision radius) without hitting geometry. The
	// original's util.CanActorMoveTo becomes a capsule sweep on ECC_Pawn.
	bool AnimCanBeUsed(const AActor& Source, const FWoTAnimTableEntry& Entry);

	// GetAnimEndVector (AnimationTableWOT.uc:105-122): the end vector of the entry
	// whose AnimName matches exactly (the carcass cylinder shift); zero when absent.
	FVector FindAnimEndVector(FName TableName, FName AnimName);

	// ProcessAnim's rate fallback: unset (~0) -> 1.0.
	float ResolveRate(const FWoTAnimTableEntry* Entry);

	// ProcessAnim's tween fallback chain: caller value, else entry value, else 0.2.
	float ResolveTweenTime(const FWoTAnimTableEntry* Entry, float CallerTween = 0.0f);
}
