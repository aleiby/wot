#pragma once

#include "CoreMinimal.h"
#include "WoTBrainComponent.h"
#include "WoTMovementPattern.h"
#include "WoTRangeBands.generated.h"

// The range-band ladder: the machinery that picks
// WHAT an NPC does at each distance band from its active goal, replacing a
// simpler binary melee-band-vs-chase check. It ports three .uc aggregates as
// data + one classify call:
//   RangeTransitioner (HS_GoalProximity) — the Handlers[] array indexed by
//     EGoalProximity + SelectGoalProximityHandler (RangeTransitioner.uc:207).
//   RangeHandler.Template — one band's payload (which state/behavior at this
//     proximity; RangeHandler.uc:21).
//   RangeIterator — the per-actor cursor + HasRangeTransitioned re-select
//     (RangeIterator.uc:30, LegendPawn.uc:1927).
// The FSM-state distinctions those produced (AcquiredReachable/Visible/…) fold
// into per-band DATA: a band names hold-vs-move, a
// standoff/acceptance distance, and which attack route it permits. The band set
// is per-creature config decoded into UWoTCreatureData, with the factory
// defaults below retained for bare/test pawns.

// The proximity ladder (RangeTransitioner.uc:16 EGoalProximity). Order is the
// ladder order: the first proximity whose band is bound and whose goal query
// holds wins. UnNavigable is the catch-all (no query). None = no band applied.
UENUM()
enum class EWoTGoalProximity : uint8
{
	Reached,
	Reachable,
	Visible,
	Pathable,
	UnNavigable,
	None
};
// The five bindable proximities (Reached..UnNavigable); None is "no band".
static constexpr int32 WoTGoalProximityBandCount = 5;

// Which attack route a band permits (folds Grunt's melee-vs-ranged
// GetPendingWeapon split: AcquiredReachable picks the melee weapon
// Grunt.uc:1718, AcquiredVisible picks the ranged/caster weapon Grunt.uc:1756).
UENUM()
enum class EWoTBandAttack : uint8
{
	None,       // no attack from this band (approach/route/idle bands)
	Melee,      // body/weapon melee when within the standoff
	Caster,     // drive the weave caster (UWoTCasterComponent)
	Projectile  // fire the NPC's ranged weapon (UWoTWeaponComponent — the Archer's bow)
};

// One range band = the behavior at one proximity classification. This is the
// fold of a RangeHandler.Template (which .uc FSM state it selected) into the
// three decisions that state actually made: do I hold or keep moving, at what
// gap do I stop, and how do I attack (RangeHandler.uc:21; bands are DATA, not
// states).
USTRUCT()
struct FWoTRangeBand
{
	GENERATED_BODY()
	// Decoded acquired-handler choreography takes precedence over the fallback
	// hold/standoff fold. Selection state belongs to the controller's manager.
	FWoTMovementBinding MovementBinding;

	// Is a band bound at this proximity? (Handlers[GP] != None,
	// RangeTransitioner.uc:192.) An unbound proximity is skipped by the ladder.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bActive = false;

	// Hold at StandoffDistance vs. chase to contact. A holding band stops once
	// the cylinder gap falls within the standoff and faces the goal (the melee
	// creature waiting in swing range; the caster holding at weave range). A
	// non-holding band always closes (AcquiredVisible=approach for melee,
	// AcquiredPathable=route).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bHold = false;

	// Drop (invalidate) the goal on entry. NOT used by the threat-ladder
	// defaults — the shipped acquired-threat arm (AcquiredUnNavigable,
	// Grunt.uc:1780) retains the goal and drives WaypointSelectorApproaching; the
	// invalidate-if-!Navigable
	// behavior (Grunt.uc:1955) belongs to the intermediate-navigation family
	// (SuccessfullyNavigatedTo*), whose band sets arm this when configured.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bDropGoal = false;

	// Acceptance gap (cylinder-to-cylinder) for a holding band: hold + attack
	// once GetGoalDistance <= this. Melee bands use MeleeRange; the caster hold
	// band uses the weave hold range.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float StandoffDistance = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	EWoTBandAttack Attack = EWoTBandAttack::None;
};

// A per-creature proximity ladder: a RangeTransitioner in HS_GoalProximity mode
// (its Handlers[] indexed by EGoalProximity) plus the Grunt prefer-visibility
// hysteresis + flip (Grunt.uc:465 SelectAcquiredGoalHandler, :1244
// MaybeFlipPreferAcquiredVisibilityHandler). Holds no UObject refs — plain data.
USTRUCT()
struct FWoTRangeBandSet
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FWoTRangeBand Bands[WoTGoalProximityBandCount];

	// Grunt's bPreferAcquiredVisibilityHandler (CDO True, Grunt.uc:2416). When
	// set and a Visible band is bound and the goal is visible, the Visible band
	// is taken before the proximity ladder — a ranged fighter holds and shoots
	// instead of closing. Only meaningful for creatures with BOTH a close
	// (melee) band and a ranged Visible band; the two defaults below leave it
	// off (see report: base-Grunt True is inert with no ranged handler bound,
	// and forcing the melee "Visible=approach" band would suppress melee).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bPreferVisibility = false;

	// The odds MaybeFlip re-rolls the preference (Grunt CDO 0.333, :2417).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float FlipOdds = 0.333333f;

	// Seconds between flip re-rolls (the DNI_Misc7 MaybeFlip notifier cadence,
	// Grunt.uc:2429/2431). Base Grunt ships this at -1 (never), so the flip is
	// dormant for in-scope creatures; a hybrid creature sets a positive period
	// in its creature row. -1 = never.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float FlipPeriod = -1.0f;

	bool HasBand(EWoTGoalProximity Proximity) const
	{
		const int32 Idx = static_cast<int32>(Proximity);
		return Idx < WoTGoalProximityBandCount && Bands[Idx].bActive;
	}

	const FWoTRangeBand& Band(EWoTGoalProximity Proximity) const
	{
		static const FWoTRangeBand Inactive;
		const int32 Idx = static_cast<int32>(Proximity);
		return Idx < WoTGoalProximityBandCount ? Bands[Idx] : Inactive;
	}

	// Classify the goal into a proximity band. This is SelectGoalProximityHandler
	// (RangeTransitioner.uc:207) with the SelectAcquiredGoalHandler prefer-
	// visibility short-circuit in front (Grunt.uc:465). The goal queries run in
	// ladder order — Reached, then Reachable (which gates on Visible internally),
	// then Visible, then Pathable — so the brain's 32uu cache side effects match
	// the original's call order. Non-const brain: the spatial queries stamp the
	// cache.
	EWoTGoalProximity Classify(UWoTBrainComponent* Brain, EWoTGoalIndex Goal) const;

	// Re-roll bPreferVisibility with FlipOdds (MaybeFlipPreferAcquiredVisibility-
	// Handler, Grunt.uc:1244). Returns true if the preference flipped.
	bool MaybeFlip();

	// The Grunt melee creature ladder: Reached/Reachable → close + melee (hold at
	// MeleeRange, melee route); Visible → approach (no attack, close until
	// reachable); Pathable → route; UnNavigable → a short traced approach or
	// side-step while retaining the goal.
	// (Grunt Acquired* arms, Grunt.uc:1671-1783.)
	static FWoTRangeBandSet MeleeDefault(float MeleeRange);

	// The caster ladder (PORT_NOTES). Reached/Reachable/Visible
	// all HOLD at the weave hold range and cast, so a caster stops chasing the
	// moment it can engage; Pathable → route (get within range/LOS);
	// UnNavigable → traced short approach. No melee route (caster MeleeRange=0).
	static FWoTRangeBandSet CasterDefault(float HoldRange);

	// The projectile-fighter ladder (the Archer, Archer.uc:246-283): prefer-
	// visibility ON, so a visible target routes to the Visible band which HOLDS at
	// bow range and fires the arrow (EWoTBandAttack::Projectile). The Reached/
	// Reachable bands are the melee-committed arm — reached only when the ArcherBow
	// behavior clears prefer-visibility (the target closed inside SwitchToMele-
	// Distance for the commit interval, Archer.uc:167 CheckForMeleeSwitch); then the
	// ladder falls through to melee (MeleeRange). Pathable → close to bow range;
	// UnNavigable → traced short approach. ProjectileHoldRange is the standoff the
	// bow holds at (MaxProjectileRange — hold+shoot anywhere in range, close if beyond).
	static FWoTRangeBandSet ProjectileFighterDefault(float MeleeRange, float ProjectileHoldRange);

	// The Legion hybrid ladder (Legion.uc:193-207 WithinMaxMeleeDistance override): a
	// caster+melee champion that arbitrates seeker-vs-stomp by VISIBILITY, not a fixed
	// hold range. Arming the seeker weave alone would hand the Legion CasterDefault(3200)
	// and it would hold at seek range forever, never closing to its 500 volcano stomp.
	// This set instead:
	//   * Reached/Reachable/Visible (goal VISIBLE) -> hold at StompRange (500): beyond it
	//     the pawn CLOSES to the stomp band (firing seekers en route, Attack=Caster);
	//     within it the pawn holds so the stomp behavior lands the volcano (the offensive
	//     pulse suppresses the seeker at melee range — the WithinMaxMeleeDistance arm).
	//   * Pathable/UnNavigable (goal NOT visible) -> hold at SeekRange (3200) and fire
	//     seekers even at melee distance ("throw in the odd seeker", uc:200; the
	//     EnableDefensiveNotifierNonVisibleNavigation searching-state arm, uc:169-181).
	// Every band casts (Attack=Caster); the seeker-vs-stomp choice is the offensive
	// pulse's visibility+range gate against the LegionSpiritsStomp behavior. No
	// prefer-visibility flip (the Legion has no ranged weapon, so the offensive notifier
	// never re-rolls it).
	static FWoTRangeBandSet LegionDefault(float StompRange, float SeekRange);

	// The intermediate-navigation ladder (Captain/Grunt NavigatingToGoal family:
	// SoundAlarm/FindHelp/NavigatingToSeal + the grunt alarm/captain nav). Pure
	// navigation, no attack: close to the goal, and on Reached the controller
	// fires the by-actor-type arrival dispatch (ring alarm / copy captain threat /
	// relinquish at seal / alert helpers). UNLIKE the threat ladder, this family
	// DROPS an unnavigable goal (bDropGoal on UnNavigable) — the shipped
	// invalidate-if-!Navigable arm (Grunt.uc:1955, Captain AlarmUnNavigable :730).
	static FWoTRangeBandSet IntermediateNavDefault();
};
