#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvTracer.generated.h"

class AWoTSeal;

// Port of Angreal.AngrealInvTracer (exports/script/Angreal/AngrealInvTracer.uc):
// a rare Fire+Air info ter'angreal that launches a Tracer projectile at the
// NEAREST seal, leaving a fiery trail to it. GetBestTarget picks the closest
// reachable seal (world-pickup seals + seals carried by pawns), preferring
// reachable over unreachable. The executable retail loop includes the owner;
// its nearby prose comment says otherwise, as documented below.
//
// The default paths backend ranks blocked-but-reachable seals by retail
// PathNodeIteratorII.NodeCost. The NavMesh A/B arm retains the port's former
// reachability bucket, straight-line ranking, and owner-carried-seal skip.
UCLASS()
class AWoTAngrealInvTracer : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvTracer();

	virtual AActor* GetBestTarget() override;

private:
	// Retained NavMesh-arm reachability bucket.
	bool IsSealReachable(const FVector& SealLoc, const FVector& From) const;
};
