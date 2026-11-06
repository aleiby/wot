#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "WoTGenericProjectile.h"
#include "WoTEarthTremorProjectile.generated.h"

// Port of Angreal.AngrealEarthTremorProjectile (expands GenericProjectile) — the
// Earth Tremor ter'angreal's lobbed weave. Spec:
// exports/script/Angreal/AngrealEarthTremorProjectile.uc.
//
// Behavior kept (simulation): fired as an upward arc — velocity is overridden to
// (Vector(Rotation) + (0,0,0.3)) * Speed and it falls under gravity
// (PHYS_Falling). On contact (pawn or ground/wall) it explodes, dealing wide
// splash (Damage 30, DamageRadius 280, MomentumTransfer 10000, Earth damage type
// 'xExxx'). bBounce=True in the original is inert here: this engine tree's
// Projectile.HitWall Explodes unconditionally and bDestroyOnExplode destroys on
// the first contact, so it erupts where it lands rather than skipping — the base
// GenericProjectile NotifyHit->Explode reproduces exactly that; this class's
// Explode override adds only the ported presentation burst.
//
// DEFERRED (presentation): the EarthTremor ground-ripple orchestrator + its
// EarthTremorRock rings (this is also what installs
// UWoTEarthTremorLeech, so the ongoing tremor drain lands with the rings), the
// LavaRock chunk spew, replicated ClientRotation alignment, and HitWater fizzle
// (needs water zones). The decoded DT_Sprite billboard and FireballExplode burst
// are live.
UCLASS()
class AWoTEarthTremorProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:

	// EarthTremor.uc:107-114: the impact burst is a 50/50 FireballExplode /
	// FireballExplode2 pick, decided per explosion.
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;

	// The decoded DT_Sprite animation @0.8 plus the warm ground light.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	AWoTEarthTremorProjectile();

	// PostBeginPlay overrode the straight-line launch with an upward arc.
	virtual void BeginPlay() override;
	virtual FRotator CalculateTrajectory(const AActor* Source,
		const AActor* Destination, float ProjectileSpeed) const override;

private:
	FRandomStream CosmeticRandom;
};
