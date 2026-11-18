#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WoTRetailProjectileTarget.generated.h"

// Explicit port of UE1 Actor.bProjTarget. Collision responses cannot represent
// the bit: retail has both overlap-only projectile targets and overlap-only
// non-targets (for example TrapBlocker).
UINTERFACE(MinimalAPI)
class UWoTRetailProjectileTarget : public UInterface
{
	GENERATED_BODY()
};

class IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	virtual bool IsRetailProjectileTarget() const { return true; }
	// AngrealProjectile.Touch accepts bProjTarget OR actors carrying both retail
	// block flags. Most implementers only model bProjTarget, so that remains the
	// default; decorations override this with their retained retail flags.
	virtual bool IsRetailProjectileTouchTarget() const
	{
		return IsRetailProjectileTarget();
	}
};

inline bool WoTActorIsRetailProjectileTarget(const AActor* Actor)
{
	const IWoTRetailProjectileTarget* Target =
		Actor ? Cast<IWoTRetailProjectileTarget>(Actor) : nullptr;
	return Target && Target->IsRetailProjectileTarget();
}
