#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "WoTBurningChunk.generated.h"

// Angreal.BurningChunk: a local falling gameplay projectile used by Fireball
// and Earth Tremor. Damage remains authority-only in GenericProjectile; the
// short-lived bounce/scorch/fizzle presentation is independently simulated.
UCLASS()
class AWoTBurningChunk : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:
	AWoTBurningChunk();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
		UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation,
		FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;
	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	virtual void ProcessDamage(AActor* Other, float GivenDamage,
		APawn* GivenInstigator, FVector HitLocation,
		FVector GivenMomentum) override;
	virtual void HitWater() override;

	static void SpawnFan(UWorld& World, TSubclassOf<AWoTBurningChunk> ChunkClass,
		const FVector& Location, const FVector& SurfaceNormal,
		const AWoTAngrealProjectile* Source);
	void UseLocalRandom(int32 Seed);

	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") float MinChunkLife = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") float MaxChunkLife = 3.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") float MinChunkSpeed = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") float MaxChunkSpeed = 500.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") float FadeTime = 0.5f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") FName SprayerConfig = TEXT("Flame01");
	UPROPERTY(EditDefaultsOnly, Category="WoT|Chunk") bool bLavaRockVisual = false;

	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTVertexAnimComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UPointLightComponent> Light;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> Sprayer;

protected:
	void StartSprayer();
	void StopSprayer();
	FVector LastFlightVelocity = FVector::ZeroVector;
	float Duration = 1.0f;
	float FullLightIntensity = 0.0f;
	float SprayerVolume = 0.0f;
	bool bSettled = false;
	bool bWaterFizzled = false;
	bool bUseLocalRandom = false;
	FRandomStream LocalRandom;
	FTimerHandle SprayerTimer;
	float RandomRange(float Min, float Max);
};

