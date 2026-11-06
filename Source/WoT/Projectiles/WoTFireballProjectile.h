#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "WoTFireballProjectile.generated.h"

// Port of Angreal.FireballFire: a seven-second Flame01 emitter pinned to the
// dynamic actor struck by a fireball, with emission volume fading to zero.
UCLASS()
class AWoTFireballFire : public AActor
{
	GENERATED_BODY()

public:
	AWoTFireballFire();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	void Initialize(AActor* InFollowActor);

	UPROPERTY(Transient) TObjectPtr<AActor> FollowActor;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Flame;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball") float Duration = 7.0f;
	float InitialVolume = 0.0f;
	FVector RelativeOffset = FVector::ZeroVector;
	FRotator RelativeRotation = FRotator::ZeroRotator;
};

// Port of Angreal.AngrealFireballProjectile: a spinning LavaRock that bursts
// into flame after SprayerDelay. Spec defaults: Speed 1000, Damage 40,
// DamageRadius 220, MomentumTransfer 3500, DamageType 'xxFxx', orange light
// (Hue 20 / Sat 32 / Brightness 192 / Radius 8).
// Deferred to later passes (PORT_NOTES): FireballSprayer2/Flame/Smoke
// particle systems (Niagara rebuild), FireballFizzle water hit.
UCLASS()
class AWoTFireballProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:

	// The three trail bundles (6a-iii): the self-refreshing head burst (0.1s
	// LifeSpan, live particles glued via ShiftParticles), the flame cone, the
	// smoke — all velocity-aligned followers. Local cosmetics.
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Head;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Cone;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Smoke;
	virtual void Destroyed() override;
	AWoTFireballProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball") float SprayerDelay = 0.3f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball") float RockDuration = 0.5f;
	// RockRotationRate=(Roll=65000) — UE1 units/sec
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball") float RockRollRate = 65000.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball")
	TObjectPtr<class UWoTVertexAnimComponent> RockMesh;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireball")
	TObjectPtr<class UPointLightComponent> Light;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> BurstSound;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	static bool ShouldSpawnScorchForImpact(const AActor* ImpactActor,
		const UPrimitiveComponent* ImpactComponent);
	// AngrealFireballProjectile.uc:190 — the fireball fizzles out in water.
	virtual void HitWater() override { Super::HitWater(); Destroy(); }

private:
	virtual void CaptureImpactContext(AActor* Other,
		const UPrimitiveComponent* OtherComponent) override;
	float SpawnTime = 0.0f;
	bool bBurstSoundPlayed = false;
	bool bHasImpactSnapshot = false;
	bool bImpactWasNonPawn = false;
	bool bImpactWasLevelGeometry = false;
	UPROPERTY(Transient) TObjectPtr<AActor> ImpactFollowActor;
};
