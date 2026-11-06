#pragma once

#include "CoreMinimal.h"
#include "WoTSeekingProjectile.h"
#include "WoTIceProjectile.generated.h"

class UWoTLeech;

// Port of Angreal.AngrealIceProjectile (expands SeekingProjectile) — the Freeze
// ter'angreal's homing projectile. Spec: exports/script/Angreal/
// AngrealIceProjectile.uc.
//
// On hitting a live pawn it installs the IceLeech(es) for Duration seconds and
// deals NO hit-point damage (the original ProcessDamage does not call Super) —
// Freeze immobilizes, it doesn't hurt. The AttachLeechEffect / Invokable
// installer is inlined here, exactly as the Taint projectile inlines its effect
// (PORT_NOTES). The "S"-pattern SnowSprayer trail, frozen shell, and ambient
	// loop are bound presentation. The Frozen shell is deliberately owned by the
	// accepted UWoTIceLeech, not by this pre-acceptance impact path.
UCLASS()
class AWoTIceProjectile : public AWoTSeekingProjectile
{
	GENERATED_BODY()

public:

	// The three snow bundles (6a-iii): velocity-aligned followers (the shipped
	// Tick's SetLocation/SetRotation drive); fade 5s on destroy.
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Snow[3];
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Ice") float SRate = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Ice") float SRadius = 20.0f;
	float SAngle = 0.0f;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	AWoTIceProjectile();
	virtual void BeginPlay() override;

	// Give every installed leech one shared expiry instant
	// (AngrealIceProjectile.uc:23-26; AngrealDecayProjectile.uc:55-58).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Ice") float Duration = 7.0f;

	// The leech(es) to install on the victim (LeechClasses[10] in the original).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Ice")
	TArray<TSubclassOf<UWoTLeech>> LeechClasses;

	virtual void ProcessDamage(AActor* Other, float GivenDamage,
	                           APawn* GivenInstigator, FVector HitLocation,
	                           FVector GivenMomentum) override;
};
