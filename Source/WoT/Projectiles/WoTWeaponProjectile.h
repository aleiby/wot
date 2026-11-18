#pragma once

#include "CoreMinimal.h"
#include "WoTProjectile.h"
#include "WoTWeaponProjectile.generated.h"

class UTexture;
class USkeletalMesh;
class UWoTVertexAnimComponent;

// WOT.WotWeaponProjectile: the shared, non-angreal NPC ammunition layer. These
// actors deliberately do not derive from AWoTAngrealProjectile, so the retail
// reflect/absorb chain never intercepts an arrow, bolt, axe, halberd, or shield
// (WotWeaponProjectile.uc:1; ReturnToSenderReflector.uc:207).
UCLASS()
class AWoTWeaponProjectile : public AWoTProjectile
{
	GENERATED_BODY()

public:
	AWoTWeaponProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") FName DamageTypeName = TEXT("shot");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") bool bStickInWalls = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") bool bPassThroughActors = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float StayStuckTime = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float FadeAwayTime = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float FadeStartGlow = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float FadeEndGlow = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") bool bMotionBlur = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float ClearOtherDelaySecs = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") int32 YawSpinRate = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") int32 RollSpinRate = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") int32 PitchSpinRate = 0;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> SoundHitPawn;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> SoundHitWall;

	// WotThrowableWeapon copies its Skin and Style onto the thrown projectile
	// (WotThrowableWeapon.uc:27-31). Soft material + the masked-style bit are the
	// UE5 data equivalent and are intentionally public for weapon-cycle specs.
	UPROPERTY(ReplicatedUsing = OnRep_SkinOverride, EditAnywhere, Category = "WoT|Display") TSoftObjectPtr<UTexture> SkinOverride;
	UPROPERTY(Replicated, EditAnywhere, Category = "WoT|Display") bool bStyleMasked = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") TObjectPtr<UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") TSoftObjectPtr<USkeletalMesh> ProjectileMeshAsset;
	void ApplyProjectileSkin();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UFUNCTION() void OnRep_SkinOverride() { ApplyProjectileSkin(); }
	UFUNCTION() void OnWeaponProjectileOverlap(UPrimitiveComponent* Component, AActor* Other,
		UPrimitiveComponent* OtherComponent, int32 BodyIndex, bool bFromSweep,
		const FHitResult& Hit);
	UFUNCTION() void OnWeaponProjectileHit(UPrimitiveComponent* Component, AActor* Other,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

	void ProcessTouch(AActor* Other, const FVector& HitLocation);
	void StickInSomething();
	virtual void Explode(const FVector& HitLocation, const FVector& HitNormal);
	virtual void PlayExplodeCosmetics(const FVector& HitLocation,
		const FVector& HitNormal);
	virtual void PlayWallImpactCosmetics(const FVector& HitLocation,
		const FVector& HitNormal);
	void PlayTouchCosmetics(AActor* Other, const FVector& HitLocation,
		const FVector& HitNormal);
	void PlayWallCosmetics(const FVector& HitLocation,
		const FVector& HitNormal);
	void ApplyReplicatedInitialVelocity();
	void UpdateMotionBlur();
	void ClearMotionBlur();
	bool IsStuck() const { return bStuck; }

	bool bResolved = false;
	UPROPERTY(Replicated)
	bool bStuck = false;
	float StuckElapsed = 0.0f;
	TWeakObjectPtr<AActor> PreviousOther;
	float ClearOtherTime = 0.0f;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWoTVertexAnimComponent>> MotionBlurGhosts;
	TArray<FTransform> MotionBlurHistory;
};

UCLASS()
class AWoTMyrddraalBolt : public AWoTWeaponProjectile
{
	GENERATED_BODY()
public:
	AWoTMyrddraalBolt();
	virtual void BeginPlay() override;
	virtual void Destroyed() override;

protected:
	virtual void PlayExplodeCosmetics(const FVector& HitLocation,
		const FVector& HitNormal) override;
	virtual void PlayWallImpactCosmetics(const FVector& HitLocation,
		const FVector& HitNormal) override;

private:
	void StopSprayers();
	void SpawnImpactBundle(const FVector& HitLocation);
	UPROPERTY(Transient) TArray<TObjectPtr<class AWoTParticleSprayer>> Sprayers;
	bool bStoppedSprayers = false;
	bool bSpawnedImpactBundle = false;
	bool bSpawnedCrack = false;
};

UCLASS()
class AWoTTrollocAxeProjectile : public AWoTWeaponProjectile
{
	GENERATED_BODY()
public:
	AWoTTrollocAxeProjectile();
};

UCLASS()
class AWoTBATHalberdProjectile : public AWoTWeaponProjectile
{
	GENERATED_BODY()
public:
	AWoTBATHalberdProjectile();
};

UCLASS()
class AWoTQuestShieldProjectile : public AWoTWeaponProjectile
{
	GENERATED_BODY()
public:
	AWoTQuestShieldProjectile();
};
