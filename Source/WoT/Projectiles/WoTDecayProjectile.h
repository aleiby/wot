#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "WoTSeekingProjectile.h"
#include "WoTDecayProjectile.generated.h"

class UWoTLeech;

// Port of Angreal.AngrealDecayProjectile (expands SeekingProjectile) — the Decay
// ter'angreal's homing projectile. Spec: exports/script/Angreal/
// AngrealDecayProjectile.uc.
//
// On hitting a pawn it installs charge-drain leeches (and, in the original, a
// DecreaseHealthLeech) for Duration seconds and deals NO direct hit-point damage
// (ProcessDamage does not call Super — the damage comes from the health leech
// over time). The AttachLeechEffect installer is inlined, as for Ice/Taint.
//
// Installs all four leeches now that health exists: the charge-drain trio plus
// WOT.DecreaseHealthLeech (per-tick HP drain). The Player/NPC DamageRate split on
// the health leech uses its player-rate default (0.2) until NPCs land.
// Deferred (presentation): the DecaySplatter drip trail (Tick) and the
// DecayExplosion spawned in Explode.
UCLASS()
class AWoTDecayProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The DecaySplatter drip (6a-iii): while flying, a flying decal drops every
	// MinDripTime..MaxDripTime (1-2s). Local cosmetic.
	virtual void Tick(float DeltaTime) override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Decay") FName LastDripConfig;

private:
	float NextDripTime = 0.0f;
	FRandomStream CosmeticRandom;

public:

	// The decoded DT_Sprite animation @0.8 plus the green light.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	virtual void BeginPlay() override;
	AWoTDecayProjectile();

	// Shared lifespan for every installed leech (Duration=15 in the original).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay") float Duration = 15.0f;

	// The leech(es) to install on the victim (the charge-drain trio; the health
	// leech is deferred — see the class comment).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay")
	TArray<TSubclassOf<UWoTLeech>> LeechClasses;

	virtual void ProcessDamage(AActor* Other, float GivenDamage,
	                           APawn* GivenInstigator, FVector HitLocation,
	                           FVector GivenMomentum) override;
};
