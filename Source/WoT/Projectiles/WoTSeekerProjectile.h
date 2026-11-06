#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTSeekerProjectile.generated.h"

// Port of Angreal.AngrealSeekerProjectile: the Seeker's explosive pursuit
// projectile. Spec defaults: Speed 150, Damage 60, DamageRadius 240,
// MomentumTransfer 2000, green light (Brightness 64 / Hue 96 / Sat 128).
// Deferred to the effects pass (PORT_NOTES, fireball precedent):
// AngrealSeekerGlobe lightning sprite, SeekerSmoke sprayer, GreenExplode.
UCLASS()
class AWoTSeekerProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The smoke trail (6a-iii): follows the globe, velocity-aligned; fades 2s.
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> SmokeTrail;
	UPROPERTY(Transient) TObjectPtr<class AWoTWeaveSpriteEffect> Globe;
	virtual void Destroyed() override;

	// The DT_Sprite display (6a-ii): the time-scrubbed SLightning01 flipbook quad
	// (the class's existing Light stays).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	virtual void BeginPlay() override;
	AWoTSeekerProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeker")
	TObjectPtr<class UPointLightComponent> Light;
};
