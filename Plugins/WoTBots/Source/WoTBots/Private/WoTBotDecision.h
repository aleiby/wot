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
// Spread: another bot's claim leaves a pool this share of its value.
constexpr float ClaimedWeight = .15f;
// Running the map with nothing worth collecting competes at this share.
constexpr float RoamWeight = .45f;
// Disengage after this long without sight of the target or damage either way.
constexpr float NoProgressSeconds = 3.0f;
constexpr float CloseRange = 600.0f;

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
