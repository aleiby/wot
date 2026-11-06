#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTSoulBarbProjectile.generated.h"

// Port of Angreal.AngrealSoulBarbProjectile (expands SeekingProjectile) — the
// Soul Barb ter'angreal's homing projectile. Spec: exports/script/Angreal/
// AngrealSoulBarbProjectile.uc.
//
// On Explode it installs a WOT.RarityBasedDamageReflector in the victim for
// Duration seconds — thereafter the victim takes damage each time it activates an
// artifact, scaled by that artifact's rarity (a ChargeUsed interception; see
// UWoTRarityBasedDamageReflector). This is NOT a leech. Deferred presentation: the smoke/Flare/Hole
// trail (Tick) and SoulBarbImpact.
UCLASS()
class AWoTSoulBarbProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The two GenericSprite children folded in (6a-ii): the Nimbus flare
	// (translucent @0.8) and the modulated hole disk (@0.6) + the green light.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Flare;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Hole;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> SmokeTrail;
	AWoTSoulBarbProjectile();

	// How long the installed reflector lasts (used once the framework lands).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|SoulBarb") float Duration = 15.0f;

	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
};
