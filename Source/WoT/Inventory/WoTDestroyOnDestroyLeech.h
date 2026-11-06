#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTDestroyOnDestroyLeech.generated.h"

// Port of Angreal.DestroyOnDestroyLeech (expands Leech) — a lifetime-tie leech:
// while attached to a host pawn it keeps a DestroyActor alive, and destroys that
// actor when the leech itself is removed (host dies / is destroyed). Conversely,
// if the DestroyActor vanishes first, the leech removes itself. Spec:
// exports/script/Angreal/DestroyOnDestroyLeech.uc.
//
// Wall of Air installs two of these (one on the caster, one on the target) so the
// wall projectile unravels the moment either pawn dies. The wall projectile also
// self-destructs on caster/victim death in its own Tick, so this is belt-and-
// suspenders for the death case; the load-bearing part it adds is the retail
// AttachLeechEffect->ProcessEffect gate — an element shield on the pawn refuses
// the leech (UWoTLeech::AttachTo -> IsEffectBlocked), which fails the whole cast
// (AngrealInvWallOfAir.uc:71-75). bDisplayIcon controls the live HUD registry.
UCLASS()
class UWoTDestroyOnDestroyLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTDestroyOnDestroyLeech();

	// The actor we destroy when we go away (DestroyOnDestroyLeech.DestroyActor).
	UPROPERTY() TObjectPtr<AActor> DestroyActor;

	void SetDestroyActor(AActor* A) { DestroyActor = A; }

	// AffectHost: if our DestroyActor is already gone, remove ourselves
	// (DestroyOnDestroyLeech.uc:38-43).
	virtual void AffectHost(int32 Iterations = 0) override;

	// UE1 Leech.Destroyed -> DestroyActor.Destroy(): the component's removal (host
	// death, Unattach) drags the tied actor down with it.
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
};
