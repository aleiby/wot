#include "WoTRangeBands.h"
#include "../WoTRandom.h"

EWoTGoalProximity FWoTRangeBandSet::Classify(UWoTBrainComponent* Brain, EWoTGoalIndex Goal) const
{
	if (!Brain)
	{
		return EWoTGoalProximity::None;
	}

	// SelectAcquiredGoalHandler (Grunt.uc:465): a prefer-visibility fighter takes
	// the Visible (hold + ranged) band ahead of the ladder when a Visible band is
	// bound and it can see the goal — so it holds and shoots rather than closing.
	// (Deviation from the .uc, which short-circuited on the handler's entry
	// constraints alone, not on IsGoalVisible; see report — gating on real LOS
	// avoids "prefer" holding against a target it cannot see.)
	if (bPreferVisibility && HasBand(EWoTGoalProximity::Visible) && Brain->IsGoalVisible(Goal))
	{
		return EWoTGoalProximity::Visible;
	}

	// SelectGoalProximityHandler (RangeTransitioner.uc:222-259): the first
	// proximity whose band is bound and whose query holds. Reachable internally
	// gates on Visible, so the Reached→Reachable→Visible→Pathable order
	// reproduces the original's cache-stamping sequence.
	if (HasBand(EWoTGoalProximity::Reached) && Brain->IsGoalReached(Goal))
	{
		return EWoTGoalProximity::Reached;
	}
	if (HasBand(EWoTGoalProximity::Reachable) && Brain->IsGoalReachable(Goal))
	{
		return EWoTGoalProximity::Reachable;
	}
	if (HasBand(EWoTGoalProximity::Visible) && Brain->IsGoalVisible(Goal))
	{
		return EWoTGoalProximity::Visible;
	}
	if (HasBand(EWoTGoalProximity::Pathable) && Brain->IsGoalPathable(Goal))
	{
		return EWoTGoalProximity::Pathable;
	}
	// UnNavigable is the catch-all: bound but no query (RangeTransitioner.uc:250).
	if (HasBand(EWoTGoalProximity::UnNavigable))
	{
		return EWoTGoalProximity::UnNavigable;
	}
	return EWoTGoalProximity::None;
}

bool FWoTRangeBandSet::MaybeFlip()
{
	// MaybeFlipPreferAcquiredVisibilityHandler (Grunt.uc:1246): Frand() <= odds.
	if (WoTRandom::FRand() <= FlipOdds)
	{
		bPreferVisibility = !bPreferVisibility;
		return true;
	}
	return false;
}

FWoTRangeBandSet FWoTRangeBandSet::MeleeDefault(float MeleeRange)
{
	FWoTRangeBandSet Set;

	// Reached / Reachable → close + melee: hold at MeleeRange and swing
	// (AcquiredReachable's melee-weapon GetPendingWeapon, Grunt.uc:1716-1744;
	// WithinMaxMeleeDistance gate at :1728).
	FWoTRangeBand Close;
	Close.bActive = true;
	Close.bHold = true;
	Close.StandoffDistance = MeleeRange;
	Close.Attack = EWoTBandAttack::Melee;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reached)] = Close;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)] = Close;

	// Visible → approach: can see but not reach, so close in (no attack — a melee
	// creature has no ranged route; the ranged AcquiredVisible weapon is None so
	// it moves, Grunt.uc:1748-1775).
	FWoTRangeBand Approach;
	Approach.bActive = true;
	Approach.bHold = false;
	Approach.Attack = EWoTBandAttack::None;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Visible)] = Approach;

	// Pathable → route (AcquiredPathable = AcquiredBase, Grunt.uc:1779).
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Pathable)] = Approach;

	// UnNavigable → WaypointSelectorApproaching: trace a short step toward the
	// goal, or a side-step around the blocker, while retaining the acquired goal.
	Set.Bands[static_cast<int32>(EWoTGoalProximity::UnNavigable)] = Approach;

	// Melee-only: no ranged band to prefer (base-Grunt True is inert; see header).
	Set.bPreferVisibility = false;
	return Set;
}

FWoTRangeBandSet FWoTRangeBandSet::CasterDefault(float HoldRange)
{
	FWoTRangeBandSet Set;

	// Reached / Reachable / Visible → HOLD at the weave range and cast. A caster
	// never closes to melee; the moment it can engage (reachable straight-line or
	// simply visible) it stops at HoldRange and drives the weave — the fix for
	// the point-blank fizzle (PORT_NOTES "Range-band handling"). AcquiredVisible casts via the
	// offensive notifier (AngrealUser PerformUse, AngrealUser.uc:396).
	FWoTRangeBand HoldCast;
	HoldCast.bActive = true;
	HoldCast.bHold = true;
	HoldCast.StandoffDistance = HoldRange;
	HoldCast.Attack = EWoTBandAttack::Caster;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reached)] = HoldCast;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)] = HoldCast;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Visible)] = HoldCast;

	// Pathable → route: no LOS/reach yet, so path in until it can engage.
	FWoTRangeBand Approach;
	Approach.bActive = true;
	Approach.bHold = false;
	Approach.Attack = EWoTBandAttack::None;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Pathable)] = Approach;

	// UnNavigable → the same short traced approach used by AcquiredUnNavigable.
	Set.Bands[static_cast<int32>(EWoTGoalProximity::UnNavigable)] = Approach;

	Set.bPreferVisibility = false;
	return Set;
}

FWoTRangeBandSet FWoTRangeBandSet::ProjectileFighterDefault(float MeleeRange, float ProjectileHoldRange)
{
	FWoTRangeBandSet Set;

	// Visible → HOLD at bow range and fire (kneel-fire, Archer PerformProjectile-
	// Attack). Prefer-visibility routes here whenever the target is visible, so the
	// Archer stops and shoots instead of closing. StandoffDistance = the bow's max
	// range: within it the pawn holds and shoots; beyond it the hold branch is off so
	// the pawn closes until in range.
	FWoTRangeBand HoldShoot;
	HoldShoot.bActive = true;
	HoldShoot.bHold = true;
	HoldShoot.StandoffDistance = ProjectileHoldRange;
	HoldShoot.Attack = EWoTBandAttack::Projectile;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Visible)] = HoldShoot;

	// Reached / Reachable → the melee-committed arm (AcquiredReachable melee weapon,
	// Grunt.uc:1716). Only reached when the ArcherBow behavior clears prefer-
	// visibility (target inside SwitchToMeleeDistance for the commit interval); then
	// the ladder falls here and the Archer closes to MeleeRange and swings.
	FWoTRangeBand Melee;
	Melee.bActive = true;
	Melee.bHold = true;
	Melee.StandoffDistance = MeleeRange;
	Melee.Attack = EWoTBandAttack::Melee;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reached)] = Melee;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)] = Melee;

	// Pathable → route in until the target is visible / in range.
	FWoTRangeBand Approach;
	Approach.bActive = true;
	Approach.bHold = false;
	Approach.Attack = EWoTBandAttack::None;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Pathable)] = Approach;

	// UnNavigable → the same short traced approach used by AcquiredUnNavigable.
	Set.Bands[static_cast<int32>(EWoTGoalProximity::UnNavigable)] = Approach;

	// Archer inherits Grunt's bPreferAcquiredVisibilityHandler=True (Grunt.uc:2416):
	// kneel and shoot from range by default. The behavior toggles it for the melee
	// switch. No auto-flip (FlipPeriod stays -1 — Grunt never arms the flip cadence).
	Set.bPreferVisibility = true;
	return Set;
}

FWoTRangeBandSet FWoTRangeBandSet::LegionDefault(float StompRange, float SeekRange)
{
	FWoTRangeBandSet Set;

	// Goal VISIBLE (Reached/Reachable/Visible) -> hold at the stomp reach. Beyond it the
	// hold branch is off so the Legion CLOSES to the 500 band (Legion.uc:202 falls through
	// to Super.WithinMaxMeleeDistance once in range); within it the Legion holds and the
	// stomp behavior lands the volcano. Attack=Caster fires seekers while closing; the
	// offensive pulse suppresses the seeker once the goal is visible AND inside StompRange
	// (the melee arm, Legion.uc:213-219).
	FWoTRangeBand VisibleClose;
	VisibleClose.bActive = true;
	VisibleClose.bHold = true;
	VisibleClose.StandoffDistance = StompRange;
	VisibleClose.Attack = EWoTBandAttack::Caster;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reached)] = VisibleClose;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)] = VisibleClose;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Visible)] = VisibleClose;

	// Without line of sight (Pathable/UnNavigable), hold at seek range while the offensive
	// notifier continues launching seekers even inside stomp distance
	// (Legion.uc:169-181,197-201). The
	// no-LOS window is 50..SeekRange (the adapter's 50..3200); the hold branch stops the
	// Legion from charging blind, seeking from wherever it last saw the threat.
	FWoTRangeBand SeekHold;
	SeekHold.bActive = true;
	SeekHold.bHold = true;
	SeekHold.StandoffDistance = SeekRange;
	SeekHold.Attack = EWoTBandAttack::Caster;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Pathable)] = SeekHold;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::UnNavigable)] = SeekHold;

	// No ranged weapon -> the offensive notifier never re-rolls prefer-visibility; the
	// seeker/stomp arbitration is the visibility+range gate, not a handler flip.
	Set.bPreferVisibility = false;
	Set.FlipPeriod = -1.0f;
	return Set;
}

FWoTRangeBandSet FWoTRangeBandSet::IntermediateNavDefault()
{
	FWoTRangeBandSet Set;

	// Reached → hold at contact and let the controller run the on-arrival dispatch
	// (SuccessfullyNavigatedToGoal, Grunt.uc:1905 / Captain SoundAlarm + HelpReached
	// — the only live intermediate arrivals). Zero standoff: hold only once the
	// cylinders actually touch.
	FWoTRangeBand Arrive;
	Arrive.bActive = true;
	Arrive.bHold = true;
	Arrive.StandoffDistance = 0.0f;
	Arrive.Attack = EWoTBandAttack::None;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reached)] = Arrive;

	// Reachable / Visible / Pathable → close in (no attack — this is navigation to
	// an alarm or a help pawn, not combat).
	FWoTRangeBand Approach;
	Approach.bActive = true;
	Approach.bHold = false;
	Approach.Attack = EWoTBandAttack::None;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Reachable)] = Approach;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Visible)] = Approach;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::Pathable)] = Approach;

	// UnNavigable → DROP the goal (bDropGoal). The navigation family does not idle
	// on an unreachable intermediate the way the acquired-threat ladder does; it
	// invalidates and re-selects (Grunt.uc:1955, Captain AlarmUnNavigable :730).
	FWoTRangeBand Drop;
	Drop.bActive = true;
	Drop.bDropGoal = true;
	Set.Bands[static_cast<int32>(EWoTGoalProximity::UnNavigable)] = Drop;

	Set.bPreferVisibility = false;
	return Set;
}
