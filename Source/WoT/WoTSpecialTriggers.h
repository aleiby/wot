#pragma once

#include "CoreMinimal.h"
#include "WoTEvents.h"
#include "WoTSpecialTriggers.generated.h"

UENUM()
enum class EWoTSpecialEventAction : uint8
{
	DisplayMessage,
	DamageInstigator,
	KillInstigator,
	PlaySoundEffect,
	PlayersPlaySoundEffect,
	PlayAmbientSoundEffect,
	PlayerPath,
};

USTRUCT()
struct FWoTSpecialEventConfig
{
	GENERATED_BODY()

	UPROPERTY() EWoTSpecialEventAction Action = EWoTSpecialEventAction::DisplayMessage;
	UPROPERTY() int32 Damage = 0;
	UPROPERTY() FName DamageType;
	UPROPERTY() FString Message;
	UPROPERTY() bool bBroadcast = false;
	UPROPERTY() FName SoundSubPath;
	UPROPERTY() FName AmbientSoundSubPath;
	UPROPERTY() float SoundVolume = 1.0f;
	UPROPERTY() float SoundRadius = 2000.0f;
	UPROPERTY() float SoundPitch = 1.0f;
};

/** Engine.SpecialEvent: a tagged, non-colliding event action. */
UCLASS()
class AWoTSpecialEvent : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTSpecialEvent();
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;
	UPROPERTY(EditAnywhere, Category = "WoT") FWoTSpecialEventConfig Config;
	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }
};

/** WOT.SpecialEventTrigger: Trigger overlap/relay semantics plus SpecialEvent action states. */
UCLASS()
class AWoTSpecialEventTrigger : public AWoTTrigger
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT") FWoTSpecialEventConfig Config;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;

protected:
	virtual void FireTargetEvent(AActor* Other, APawn* EventInstigator) override;
};

/** Shared native for Engine.StochasticTrigger and WOT.SpecialEffectsTrigger. */
UCLASS()
class AWoTRandomEventTrigger : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTRandomEventTrigger();
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> Events;
	UPROPERTY(EditAnywhere, Category = "WoT") float TriggerProbability = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float MinRecheckTime = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float MaxRecheckTime = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bInitiallyActive = false;
	// SpecialEffectsTrigger is a ROLE_None simulated presentation driver.
	UPROPERTY(EditAnywhere, Category = "WoT") bool bLocalOnly = false;

	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }

private:
	void ScheduleNext();
	void OnTimer();
	bool bActive = false;
	// Exported triggerInstigator is an Actor and is distinct from Actor.Instigator:
	// first TriggeredActive activation stores EventInstigator; later activations
	// store Other. AlwaysActive therefore dispatches None for Other.
	TWeakObjectPtr<AActor> TriggerInstigatorActor;
	TWeakObjectPtr<APawn> EventInstigatorPawn;
	FTimerHandle TimerHandle;
};

UENUM()
enum class EWoTTriggerLightMode : uint8
{
	TurnsOn,
	TurnsOff,
	Toggle,
	Control,
	Pound,
};

/** Engine.TriggerLight: a tagged point light with the retail fade state machine. */
UCLASS()
class AWoTTriggerLight : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTTriggerLight();
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT")
	EWoTTriggerLightMode Mode = EWoTTriggerLightMode::Control;
	UPROPERTY(EditAnywhere, Category = "WoT") float ChangeTime = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bInitiallyOn = false;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bDelayFullOn = false;
	UPROPERTY(EditAnywhere, Category = "WoT") float RemainOnTime = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float FullIntensity = 5.0f;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_TargetAlpha) float TargetAlpha = 0.0f;
	float CurrentAlpha = 0.0f;
	float PoundElapsed = 0.0f;
	FTimerHandle PoundTimerHandle;
	UFUNCTION() void OnRep_TargetAlpha();
	void SetTarget(float NewTarget);
	void OnPoundTimer();
};
