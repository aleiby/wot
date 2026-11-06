#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "WoTFireworksProjectile.generated.h"

struct FWoTFireworkSet
{
	FName ProjectileConfig;
	FName ExplosionConfig;
	FName LaunchSoundPath;
	FName ExplosionSoundPath;
};

// Port of Angreal.AngrealFireworksProjectile (expands GenericProjectile) — the
// Fireworks ter'angreal's projectile. Spec: exports/script/Angreal/
// AngrealFireworksProjectile.uc.
//
// NOTE: Fireworks is an unfinished dev artifact (its launcher has Title="Fireball",
// Description/Quote="None" and reuses the Fireball pickup). Ported for checklist
// completeness. Behavior: a bouncing (PHYS_Falling) projectile that detonates on
// a random MinExpTime..MaxExpTime fuse OR on pawn contact, dealing wide splash
// (DamageRadius 320). Wall hits bounce (0.6 damping) instead of exploding.
//
// Deferred presentation: the five random FireworkSet particle systems (launch
// spray + explosion burst) and their per-set sounds; HitWater fizzle.
UCLASS()
class AWoTFireworksProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:
	AWoTFireworksProjectile();

	// Random fuse bounds — detonates after RandRange(MinExpTime, MaxExpTime).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireworks") float MinExpTime = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Fireworks") float MaxExpTime = 6.0f;

	virtual void BeginPlay() override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void Destroyed() override;
	static const TArray<FWoTFireworkSet>& GetFireworkSets();
	UPROPERTY(VisibleAnywhere, Category = "WoT|Fireworks") int32 SelectedFirework = INDEX_NONE;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Trail;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Fireworks") FName LastLaunchSound;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Fireworks") FName LastExplosionSound;
	// Wall hits bounce (PMC) rather than detonate — suppress the base HitWall.
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;

	// AngrealFireworksProjectile.uc:100 — fizzles out in water.
	virtual void HitWater() override { Super::HitWater(); Destroy(); }

	// Fuse expiry -> detonate.
	UFUNCTION()
	void Detonate();

private:
	FTimerHandle FuseTimer;
};
