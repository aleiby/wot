#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvRemoveCurse.generated.h"

// Port of Angreal.AngrealInvRemoveCurse (expands LeechAttacher) — the Unravel
// ter'angreal: an instant self-cleanse that destroys weaves affecting you (and
// anyone in a small area). Spec: exports/script/Angreal/AngrealInvRemoveCurse.uc
// + RemoveCurseLeech.uc + RCAMAPearl.uc + RemoveCurseEffect.uc.
//
// The original attaches a RemoveCurseLeech to the caster which spawns an
// RCAMAPearl (a small, short AMAPearl) that follows the caster and strips the bad
// weaves off everyone it touches. That leech->pearl indirection is folded: casting
// spawns the anti-magic dome (AWoTAMAPearl, configured with RCAMAPearl's params)
// on the caster directly. See AWoTAMAPearl for the deferred dead-zone half.
UCLASS()
class AWoTAngrealInvRemoveCurse : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvRemoveCurse();

	virtual void CastArtifact() override;   // Cast(): drop the cleanse dome
};
