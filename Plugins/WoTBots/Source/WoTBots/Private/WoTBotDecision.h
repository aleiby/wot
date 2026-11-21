#pragma once
#include "CoreMinimal.h"

// Arena goal policy: collect versus fight, independent of perception and of
// casting difficulty (aim error, reaction time and sight are unchanged).
namespace WoTBotDecision
{
// Hysteresis: a challenger must beat the current goal by this ratio plus margin.
constexpr float SwitchRatio = 1.25f;
constexpr float SwitchMargin = .15f;
// Rotation: a visited pool is skipped briefly, then regains value over the memory.
constexpr float VisitMemory = 45.0f;
constexpr float RevisitBlockSeconds = 3.0f;
constexpr float ArrivalRadius = 120.0f;
// A pool with no complete path is not searched for again until this has passed.
constexpr float UnreachableRetrySeconds = 5.0f;
// A failed lift or door traversal (a contended deck, a blocked exit, a missed
// boarding, a displaced body) leaves that link out of this bot's planning for
// this long. A returning lift goes round in its two moves plus its stay open
// (MoveTime, StayOpenTime: 5 s for Arena_04's): three rounds give a rider ahead,
// a body at the exit or a crowd at the entry time to clear, instead of queuing
// straight back behind the same deck. It is well inside VisitMemory, which keeps
// the abandoned pool out of this bot's choices for 45 s anyway.
constexpr float MoverLinkRetrySeconds = 15.0f;
// Spread: another bot's claim leaves a pool this share of its value.
constexpr float ClaimedWeight = .15f;
// Running the map with nothing worth collecting competes at this share.
constexpr float RoamWeight = .45f;
// Disengage after this long without sight of the target or damage either way.
constexpr float NoProgressSeconds = 3.0f;
constexpr float CloseRange = 600.0f;
// A pickup on a ledge or pedestal is collected by jumping toward it (player
// jump: 325 up against 950 gravity, about 55 units of rise, with full air
// control). At the top of the jump the 92-unit body reaches 147 above its
// floor, touching a 30-unit pickup sphere centred up to about 160 above it.
// Beyond this reach from the route's end it is not chosen.
constexpr float HopReach = 110.0f;
constexpr float HopRiseMin = -10.0f;
constexpr float HopRiseMax = 160.0f;
constexpr int32 MaxHops = 2;
// A pool reached but not collected, even by jumping, is not chosen again for this long.
constexpr float UncollectableSeconds = 60.0f;
// A blocked body jumps a step its walk cannot climb when the route rises at
// least this much, and the jump clears it at this height.
constexpr float ClimbRise = 15.0f;
constexpr float ClimbReach = 100.0f;
// Only a ledge higher than the walk steps (25, plus its 2.4 hover) is jumped,
// after the body has been held back this long.
constexpr float ClimbStep = 22.0f;
constexpr float ClimbBlockedSeconds = .2f;
constexpr float ClimbClearance = 50.0f;

enum class EPickupReach : uint8 { Stand, Hop, None };
// Collecting means overlapping the pickup sphere. From feet on the route's end,
// a standing body (radius 17, height 92) touches it, a jump toward it does, or
// nothing does (below the floor, out of reach).
inline EPickupReach PickupReach(const FVector& Feet, const FVector& Pickup, float PickupRadius,
	float BodyRadius = 17.0f, float BodyHeight = 92.0f)
{
	const FVector Bottom = Feet + FVector(0, 0, BodyRadius), Top = Feet + FVector(0, 0, BodyHeight - BodyRadius);
	if (FVector::Dist(FMath::ClosestPointOnSegment(Pickup, Bottom, Top), Pickup) < BodyRadius + PickupRadius)
		return EPickupReach::Stand;
	const float Rise = Pickup.Z - Feet.Z;
	return FVector::Dist2D(Feet, Pickup) <= HopReach && Rise >= HopRiseMin && Rise <= HopRiseMax
		? EPickupReach::Hop : EPickupReach::None;
}

inline bool ClearlyBetter(float Challenger, float Current)
{
	return Challenger > Current * SwitchRatio + SwitchMargin;
}
inline float Rotation(float SecondsSinceVisit)
{
	return FMath::Lerp(.08f, 1.0f, FMath::Clamp(SecondsSinceVisit / VisitMemory, 0.0f, 1.0f));
}
inline float Collect(float Priority, bool Missing, float ChargeFraction, int32 OffensiveKinds,
	bool Offensive, float PathLength, float RotationWeight, bool Claimed)
{
	const float Need = .45f + (Missing ? 1.2f : 1.0f - ChargeFraction)
		+ (Offensive ? .4f * FMath::Max(0, 3 - OffensiveKinds) : 0.0f);
	return (.5f + FMath::Max(Priority, 0.0f) / 10.0f) * Need
		/ (1.0f + PathLength / 1600.0f) * RotationWeight * (Claimed ? ClaimedWeight : 1.0f);
}
// Unscaled roaming value; far and long-unvisited pools are preferred.
inline float Roam(float PathLength, float RotationWeight, bool Claimed)
{
	return RotationWeight * (.5f + FMath::Min(PathLength / 3000.0f, 1.5f)) * (Claimed ? ClaimedWeight : 1.0f);
}
inline float Fight(float Distance, bool Visible, bool Attacker, float Health, int32 Charges,
	float TargetHealth, int32 OtherFighters, bool Close)
{
	float Score = (.15f + 1.4f / (1.0f + Distance / 600.0f))
		* (Visible ? 1.0f : .3f) * (.25f + .75f * Health)
		* (.25f + .75f * FMath::Clamp(Charges / 12.0f, 0.0f, 1.0f))
		* (1.0f + 1.5f * FMath::Max(0.0f, Health - TargetHealth));
	if (Close) Score += 1.0f;
	if (Attacker) Score += 1.5f;
	return Score * (!Attacker && OtherFighters >= 2 ? .25f : 1.0f);
}
// WasCollecting means heading for a stocked pool: such a route survives distant
// opportunities (the bot fires on the way) and yields only to a close threat or
// an attacker. Roaming and fighting compare scores with hysteresis.
inline bool ChooseFight(float CollectScore, float FightScore, bool WasFighting, bool WasCollecting, bool Threat)
{
	if (WasCollecting && CollectScore > 0 && !Threat) return false;
	if (FightScore <= 0) return false;
	if (CollectScore <= 0) return true;
	return WasFighting ? !ClearlyBetter(CollectScore, FightScore) : ClearlyBetter(FightScore, CollectScore);
}
}
