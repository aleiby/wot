#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTBurningChunk.h"
#include "WoTEarthTremorFX.generated.h"

UCLASS()
class AWoTETSoundProxy : public AActor
{
	GENERATED_BODY()
public:
	AWoTETSoundProxy();
	void AddRock(class AWoTEarthTremorRock* Rock);
	void RemoveRock(class AWoTEarthTremorRock* Rock);
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	float GetFadeScalar() const { return FadeScalar; }

	UPROPERTY(VisibleAnywhere) TObjectPtr<class UPointLightComponent> Light;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTAmbientSoundComponent> LoopAudio;
private:
	TArray<TWeakObjectPtr<AWoTEarthTremorRock>> Rocks;
	float FadeScalar = 1.0f;
	float FullLightIntensity = 0.0f;
	bool bHadRock = false;
};

UCLASS()
class AWoTEarthTremorChunk : public AWoTBurningChunk
{
	GENERATED_BODY()
public:
	AWoTEarthTremorChunk();
};

UCLASS()
class AWoTEarthTremorRock : public AWoTAngrealProjectile
{
	GENERATED_BODY()
public:
	AWoTEarthTremorRock();
	void InitializeRock(bool bGameplay, bool bInnerRock, float Scale,
		AWoTETSoundProxy* InProxy);
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	void TouchActor(AActor* Other);
	void UnTouchActor(AActor* Other);

	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTVertexAnimComponent> RockMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UCapsuleComponent> RockCollision;
	UPROPERTY() TObjectPtr<AWoTETSoundProxy> SoundProxy;
	UPROPERTY(VisibleAnywhere) FVector RelativeUp = FVector::UpVector;
	UPROPERTY() TObjectPtr<APawn> IgnoredPawn;
	bool IsAuthorityGameplayRock() const { return bAuthorityGameplay; }

protected:
	UFUNCTION() void OnRockBeginOverlap(UPrimitiveComponent* OverlappedComponent,
		AActor* Other, UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);
	UFUNCTION() void OnRockEndOverlap(UPrimitiveComponent* OverlappedComponent,
		AActor* Other, UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex);

private:
	bool bAuthorityGameplay = false;
	bool bInner = false;
	float AuthoredScale = 1.0f;
	float NextAnimationTime = 0.0f;
	float AnimationFrequency = 1.0f;
	float NextSpewTime = 0.0f;
	float DamageTimer = 0.0f;
	void SpawnLavaChunks();
};

// One replicated marker per impact. The marker itself is networked; each
// machine creates its own ROLE_None rings, while authority rocks alone own
// overlaps, leeches, and damage.
UCLASS()
class AWoTEarthTremor : public AWoTAngrealProjectile
{
	GENERATED_BODY()
public:
	AWoTEarthTremor();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void Go();
	int32 GetSpawnedRockCount() const { return SpawnedRockCount; }
	const TArray<float>& GetSpawnedRingRadii() const { return SpawnedRingRadii; }
	AWoTETSoundProxy* GetSoundProxyForTest() const { return SoundProxy.Get(); }
	AWoTEarthTremorRock* GetAnyAuthorityRockForTest() const;
	bool IsRingSlotClearForTest(const FVector& Slot) const;

private:
	UPROPERTY() TObjectPtr<AWoTETSoundProxy> SoundProxy;
	UPROPERTY() TArray<TObjectPtr<AWoTEarthTremorRock>> LocalRocks;
	TArray<float> SpawnedRingRadii;
	float StepElapsed = 0.0f;
	int32 NextRing = 0;
	int32 SpawnedRockCount = 0;
	bool bRunning = false;
	void SpawnCenter();
	void SpawnRing(float Scale, float StartAngle, float StepAngle, float Radius);
	bool IsRingSlotClear(const FVector& Slot) const;
	AWoTEarthTremorRock* SpawnRock(const FVector& Location,
		const FRotator& Rotation, float Scale, bool bInner);
};
