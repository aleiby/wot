#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTEvents.h"
#include "WoTWorldFX.generated.h"

class UPointLightComponent;
class UWoTVertexAnimComponent;
class UCapsuleComponent;

/** Environment.Lightning / RandomLightning presentation and replicated strike clock. */
UCLASS()
class AWoTLightning : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()
public:
	AWoTLightning();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void WoTTrigger(AActor*, APawn*) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

	UPROPERTY(EditAnywhere) FName TagName;
	UPROPERTY(EditAnywhere) FName EventName;
	UPROPERTY(EditAnywhere, Replicated) FName LightTag;
	UPROPERTY(EditAnywhere, Replicated) FName ThunderSound = TEXT("Environment/Thunder10");
	UPROPERTY(EditAnywhere, Replicated) float ThunderDelay = 1.0f;
	UPROPERTY(EditAnywhere, Replicated) float HalfLightPct = 0.5f;
	UPROPERTY(EditAnywhere) float BrightnessPerSecond = 2000.0f;
	UPROPERTY(EditAnywhere) float LightningLength = 0.5f;
	UPROPERTY(EditAnywhere) float ThunderRadius = 5000.0f;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	UPROPERTY(ReplicatedUsing=OnRep_Strike) uint16 StrikeSerial = 0;
	UFUNCTION() void OnRep_Strike();

private:
	void StartStrike();
	UPointLightComponent* ResolvePresentationLight();
	float StrikeAge = -1.0f;
	bool bThundered = true;
	bool bLightFinished = true;
	float PresentationPeakIntensity = 5.0f;
	TWeakObjectPtr<UPointLightComponent> TaggedLight;
};

/** WOT.DistanceLightning: long dark interval followed by a 0.4s flicker. */
UCLASS()
class AWoTDistanceLightning final : public AActor
{
	GENERATED_BODY()
public:
	AWoTDistanceLightning();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	UPROPERTY(ReplicatedUsing=OnRep_Flash) bool bFlashing = false;
	UFUNCTION() void OnRep_Flash();
private:
	float ChangeAt = 0.0f;
};

UCLASS()
class AWoTRandomLightning final : public AWoTLightning
{
	GENERATED_BODY()
public:
	AWoTRandomLightning();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void WoTTrigger(AActor*, APawn*) override {}
	UPROPERTY(EditAnywhere) float MinThunderDelay = 0.0f;
	UPROPERTY(EditAnywhere) float MaxThunderDelay = 3.0f;
	UPROPERTY(EditAnywhere) float MinLightningTime = 3.0f;
	UPROPERTY(EditAnywhere) float MaxLightningTime = 7.0f;
	UPROPERTY(EditAnywhere) bool bSpawnsLightning = true;
private:
	float NextStrike = 0.0f;
};

/** ParticleSystems.Wind: client-simulated gravity offset on tagged sprayers. */
UCLASS()
class AWoTWind final : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()
public:
	AWoTWind();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	virtual void WoTTrigger(AActor*, APawn*) override;
	virtual void WoTUnTrigger(AActor*, APawn*) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
	UPROPERTY(EditAnywhere, Replicated) FName TagName;
	UPROPERTY(EditAnywhere, Replicated) FName EventName;
	UPROPERTY(EditAnywhere, Replicated) FName InitialState;
	UPROPERTY(EditAnywhere, Replicated) float MinImpulseTime = 1.0f;
	UPROPERTY(EditAnywhere, Replicated) float MaxImpulseTime = 1.0f;
	UPROPERTY(EditAnywhere, Replicated) float MinWindspeed = 0.0f;
	UPROPERTY(EditAnywhere, Replicated) float MaxWindspeed = 0.0f;
	UPROPERTY(EditAnywhere, Replicated) float TimerDuration = 0.0f;
	UPROPERTY(EditAnywhere, Replicated) bool bInitiallyOn = true;
	UPROPERTY(ReplicatedUsing=OnRep_Enabled) bool bOn = true;
	UFUNCTION() void OnRep_Enabled();
	UPROPERTY(ReplicatedUsing=OnRep_TimedTrigger) uint8 TimedTriggerSerial = 0;
	UPROPERTY(ReplicatedUsing=OnRep_TimedTrigger) float TimedTriggerServerTime = 0.0f;
	UFUNCTION() void OnRep_TimedTrigger();
#if WITH_AUTOMATION_TESTS
	bool IsEnabledForTesting() const { return bOn; }
	const FVector& GetLastWindForTesting() const { return LastWind; }
	uint8 GetTimedTriggerSerialForTesting() const { return TimedTriggerSerial; }
	float GetTimedTriggerServerTimeForTesting() const
		{ return TimedTriggerServerTime; }
	float GetRestoreAtForTesting() const { return RestoreAt; }
	int32 GetTimedTriggerObserveCountForTesting() const
		{ return TimedTriggerObserveCountForTesting; }
	void SetTimedTriggerSerialForTesting(uint8 Value)
		{ TimedTriggerSerial = Value; ForceNetUpdate(); }
	void SetTimedTriggerServerTimeForTesting(float Value)
		{ TimedTriggerServerTime = Value; ForceNetUpdate(); }
#endif
private:
	void SetEnabled(bool bNewOn);
	void ApplySpeed(float Speed);
	void ApplyTimedTriggerReplication();
	void RestoreTimedClientState();
	float NextImpulse = 0.0f;
	float RestoreAt = -1.0f;
	FVector LastWind = FVector::ZeroVector;
	uint8 LastAppliedTimedTriggerSerial = 0;
	float LastAppliedTimedTriggerServerTime = 0.0f;
	bool bHasAppliedTimedTriggerPair = false;
	bool bHoldExpiredTimedPulse = false;
	uint8 ExpiredTimedPulseUpdatesRemaining = 0;
#if WITH_AUTOMATION_TESTS
	int32 TimedTriggerObserveCountForTesting = 0;
#endif
};

USTRUCT()
struct FWoTSpewThing
{
	GENERATED_BODY()
	UPROPERTY() FName ClassIdentity;
	UPROPERTY() FName MeshName;
	UPROPERTY() TArray<FName> SkinIdentities;
	UPROPERTY() int32 Count = 0;
	UPROPERTY() float MinSpeed = 0.0f;
	UPROPERTY() float MaxSpeed = 0.0f;
	UPROPERTY() float DrawScale = 0.0f;
	UPROPERTY() int32 DetailLevel = 0;
	UPROPERTY() float CollisionRadius = 0.0f;
	UPROPERTY() float CollisionHeight = 0.0f;
	UPROPERTY() float LifeSpan = 0.0f;
	UPROPERTY() float MinBounceSoundSpeed = 0.0f;
	UPROPERTY() float MinFirstHitSpeed = 0.0f;
	UPROPERTY() float MinBounceSpeed = 0.0f;
	UPROPERTY() float SpeedDampeningRatio = 0.0f;
	UPROPERTY() float RotationDampeningRatio = 0.0f;
	UPROPERTY() float BaseBounceVolume = 1.0f;
	UPROPERTY() float BaseBouncePitch = 1.0f;
	UPROPERTY() float VolumePerturbPercent = 0.0f;
	UPROPERTY() float PitchPerturbPercent = 0.0f;
	UPROPERTY() FName LandSound;
};

/** WOT.SpewTrigger plus its replicated, cosmetic falling chunks. */
UCLASS()
class AWoTSpewChunk final : public AActor
{
	GENERATED_BODY()
public:
	AWoTSpewChunk();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	UPROPERTY(Replicated) FName MeshName;
	UPROPERTY(Replicated) FName ClassIdentity;
	UPROPERTY(Replicated) TArray<FName> SkinIdentities;
	UPROPERTY(Replicated) float MeshScale = 1.0f;
	UPROPERTY(Replicated) FVector InitialVelocity;
	UPROPERTY(Replicated) bool bTumble = true;
	UPROPERTY(Replicated) bool bCollision = false;
	UPROPERTY(Replicated) bool bSilent = false;
	UPROPERTY(Replicated) int32 DetailLevel = 0;
	UPROPERTY(Replicated) float CollisionRadius = 0.0f;
	UPROPERTY(Replicated) float CollisionHeight = 0.0f;
	UPROPERTY(Replicated) float AuthoredLifeSpan = 0.0f;
	UPROPERTY(Replicated) float MinBounceSoundSpeed = 0.0f;
	UPROPERTY(Replicated) float MinFirstHitSpeed = 0.0f;
	UPROPERTY(Replicated) float MinBounceSpeed = 0.0f;
	UPROPERTY(Replicated) float SpeedDampeningRatio = 0.0f;
	UPROPERTY(Replicated) float RotationDampeningRatio = 0.0f;
	UPROPERTY(Replicated) float BaseBounceVolume = 1.0f;
	UPROPERTY(Replicated) float BaseBouncePitch = 1.0f;
	UPROPERTY(Replicated) float VolumePerturbPercent = 0.0f;
	UPROPERTY(Replicated) float PitchPerturbPercent = 0.0f;
	UPROPERTY(Replicated) FName LandSound;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> CollisionCapsule;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
#if WITH_AUTOMATION_TESTS
	bool IsBouncingForTest() const { return bBounce; }
	const FVector& GetVelocityForTest() const { return Velocity; }
	int32 GetBlockingHitCountForTest() const { return BlockingHitCountForTesting; }
	int32 GetBounceSoundCountForTest() const { return BounceSoundCountForTesting; }
	const FVector& GetLastPreImpactVelocityForTest() const
	{
		return LastPreImpactVelocityForTesting;
	}
	const FVector& GetLastPostImpactVelocityForTest() const
	{
		return LastPostImpactVelocityForTesting;
	}
	const FVector& GetLastImpactNormalForTest() const
	{
		return LastImpactNormalForTesting;
	}
#endif
private:
	FVector Velocity;
	FRotator TumbleRate;
	bool bFirstHit = true;
	bool bBounce = true;
#if WITH_AUTOMATION_TESTS
	int32 BlockingHitCountForTesting = 0;
	int32 BounceSoundCountForTesting = 0;
	FVector LastPreImpactVelocityForTesting = FVector::ZeroVector;
	FVector LastPostImpactVelocityForTesting = FVector::ZeroVector;
	FVector LastImpactNormalForTesting = FVector::ZeroVector;
#endif
};

UCLASS()
class AWoTSpewTrigger final : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()
public:
	AWoTSpewTrigger();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void WoTTrigger(AActor*, APawn*) override;
	virtual void WoTUnTrigger(AActor*, APawn*) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	UPROPERTY(EditAnywhere) FName TagName;
	UPROPERTY(EditAnywhere) FName InitialState;
	UPROPERTY(EditAnywhere) TArray<FWoTSpewThing> Things;
	UPROPERTY(EditAnywhere) float TimeBetweenSpewsSecs = 1.0f;
	UPROPERTY(EditAnywhere) int32 SpewCount = 1;
	UPROPERTY(EditAnywhere) bool bTumble = true;
	UPROPERTY(EditAnywhere) bool bInheritVelocity = false;
	UPROPERTY(EditAnywhere) bool bCollision = false;
	UPROPERTY(EditAnywhere) bool bSilent = false;
	UPROPERTY(EditAnywhere) float SpewVisibilityHeight = 1024.0f;
	UPROPERTY(EditAnywhere) float SpewVisibilityRadius = 1024.0f;
	UPROPERTY(EditAnywhere) bool bEnabled = false;
	UPROPERTY(EditAnywhere) FRotator SpewRotationSpread;
private:
	void SetEnabled(bool bValue);
	void Spew();
	int32 Remaining = 0;
	float NextSpew = 0.0f;
};
