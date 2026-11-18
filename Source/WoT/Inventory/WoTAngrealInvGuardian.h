#pragma once

#include "CoreMinimal.h"
#include "WoTResourceSpawnerInv.h"
#include "WoTAngrealInvGuardian.generated.h"

class AWoTMashadarGuide;

// Port of Angreal.AngrealInvGuardian (expands ResourceSpawner) — summons a
// Captain-tier helper of the caster's faction. Spec:
// exports/script/Angreal/AngrealInvGuardian.uc.
//
// Hound path: the Captain troop is the MashadarFocusPawn, which the original diverts
// to the Mashadar tendril system (MashadarArenaSpawner + MashadarGuide,
// AngrealInvGuardian.uc:38-47/56-79). The port spawns an AWoTMashadarGuide (the
// tendril defender) instead of a normal captain pawn — SuperCast (grandparent sound)
// + UseCharge, and HaveHelper gates on a live guide (AngrealInvGuardian.uc:60-79). The
// other three factions' Guardian path is the normal creature summon.
UCLASS()
class AWoTAngrealInvGuardian : public AWoTResourceSpawnerInv
{
	GENERATED_BODY()

public:
	AWoTAngrealInvGuardian();

	// GetResourceClass -> GetTroop(Owner, 'Captain') (AngrealInvGuardian.uc:36). The
	// MashadarFocusPawn row is handled by the Mashadar cast path (below), so it is vetoed
	// out of the NORMAL creature flow (returns NAME_None there).
	virtual FName GetTierKeyword() const override { return TEXT("Captain"); }
	virtual FName GetResourceRow() const override;

	// AngrealInvGuardian.Cast (uc:34): the Hound-Captain troop routes to the Mashadar
	// tendril spawn; every other faction to the base creature summon.
	virtual void CastArtifact() override;

	// The tendril we spawned (AngrealInvGuardian.HaveHelper checks for a live
	// MashadarGuide owned by us — uc:60-79).
	UPROPERTY() TWeakObjectPtr<AWoTMashadarGuide> CurrentGuide;

#if WITH_AUTOMATION_TESTS
	FTransform LastValidatedCrackTransformForTest = FTransform::Identity;
	float LastValidatedCrackScaleForTest = 0.0f;
	TWeakObjectPtr<class AWoTDecal> LastSpawnedCrackForTest;
#endif

	// True if the caster's Captain troop resolves to the Mashadar (Hound) path.
	bool IsMashadarSummon() const;

private:
	void CastMashadar();
};
