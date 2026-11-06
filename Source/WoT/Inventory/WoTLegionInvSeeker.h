#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTLegionInvSeeker.generated.h"

// Port of WoTPawns.LegionInvSeeker — the Hound champion's launcher for its homing
// LegionProjectile spirit. A thin ProjectileLauncher leaf: infinite charges
// (ChargeCost 0), 120 RPM, spirit element, offensive. Spec:
// exports/script/WoTPawns/LegionInvSeeker.uc.
//
// The launcher's Cast() already picks the best target through the pawn's targeting
// (AWoTProjectileLauncher::GetBestTarget -> FindBestTarget with the projectile CDO's
// MaxTargetAngle 22.5) and spawns the seeker at the LaunchOffset, SetDestination(target);
// with no target it launches straight (LegionProjectile bRequiresDestination=False).
// Granted to the Legion as a caster weave (WoTCreatureData Legion row); the caster's
// offensive cadence (MinOffensiveUsageInterval 3) paces the seeker.
UCLASS()
class AWoTLegionInvSeeker : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTLegionInvSeeker();
};
