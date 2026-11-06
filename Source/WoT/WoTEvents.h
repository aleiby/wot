#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTUE1Collision.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTEvents.generated.h"

// UE1 Tag/Event routing: actors register a TagName; firing an
// event calls WoTTrigger on every actor whose tag matches.
UINTERFACE(MinimalAPI)
class UWoTTriggerable : public UInterface
{
	GENERATED_BODY()
};

class IWoTTriggerable
{
	GENERATED_BODY()

public:
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) = 0;
	// UE1 Actor.Tag seat for systems that inspect a triggerable directly
	// (AMAPearl.uc:196). Implementers without an authored tag return None.
	virtual FName GetUE1Tag() const { return NAME_None; }
	// UE1 Actor.UnTrigger. Most triggerables ignore it; assertion-driven movers override it.
	virtual void WoTUnTrigger(AActor*, APawn*) {}
	// DoorTrigger.DispatchTrigger carries its authored side to DoorMover. Every other
	// triggerable follows the normal Trigger path, matching DoorTrigger's Super fallback.
	virtual void WoTTriggerDirectional(bool, AActor* Other, APawn* EventInstigator)
	{
		WoTTrigger(Other, EventInstigator);
	}
};

UENUM()
enum class EWoTTriggerType : uint8
{
	PlayerProximity,
	PawnProximity,
	ClassProximity,
	AnyProximity,
	Shoot,
};

UCLASS()
class UWoTEventSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void RegisterTag(FName Tag, AActor* Actor);
	void UnregisterActor(AActor* Actor);
	void FireEvent(FName Event, AActor* Other, APawn* EventInstigator);
	void FirePresentationEvent(FName Event, AActor* Other,
		APawn* EventInstigator);
	void FireUnEvent(FName Event, AActor* Other, APawn* EventInstigator);
	void FireDoorEvent(FName Event, bool bForward, AActor* Other,
	                   APawn* EventInstigator);
	AActor* FindFirstTagged(FName Tag) const;
	bool IsDispatchingLocalPresentation() const
	{
		return LocalPresentationDispatchDepth > 0;
	}

private:
	void FireEventImpl(FName Event, AActor* Other, APawn* EventInstigator,
		bool bLocalPresentation);
	TMultiMap<FName, TWeakObjectPtr<AActor>> Registry;
	TArray<TPair<FName, TWeakObjectPtr<AActor>>> RegistrationOrder;
	int32 LocalPresentationDispatchDepth = 0;
};

// UE1 Trigger: cylinder overlap by a pawn fires Event.
UCLASS()
class AWoTTrigger : public AActor, public IWoTTriggerable,
	public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTTrigger();

	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bTriggerOnceOnly = false;
	UPROPERTY(EditAnywhere, Category = "WoT") float ReTriggerDelay = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float RepeatTriggerTime = 0.0f;
	// Engine.Trigger's first enum value and retail class default.
	UPROPERTY(EditAnywhere, Category = "WoT")
	EWoTTriggerType TriggerType = EWoTTriggerType::PlayerProximity;
	UPROPERTY(EditAnywhere, Category = "WoT") TSubclassOf<AActor> ClassProximityType;

	UPROPERTY(EditAnywhere, Category = "WoT")
	TObjectPtr<class UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere, Category = "WoT") float UE1CollisionRadius = 40.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT") float UE1CollisionHalfHeight = 40.0f;
	void SetUE1CollisionSize(float Radius, float HalfHeight);

	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	virtual bool IsRetailProjectileTarget() const override
	{
		return TriggerType == EWoTTriggerType::Shoot;
	}

	// Public so the authority-gate spec can invoke it directly (a headless overlap event is
	// awkward to synthesize); it is a bound UFUNCTION either way.
	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* Comp, AActor* Other,
	               UPrimitiveComponent* OtherComp, int32 BodyIndex,
	               bool bFromSweep, const FHitResult& Hit);
	UFUNCTION()
	void OnEndOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                  UPrimitiveComponent* OtherComp, int32 BodyIndex);

	// TimedMover.EnableTriggers toggles both the Touch and Trigger probes.
	void SetTriggerEnabled(bool bEnable);
	bool IsTriggerEnabled() const { return bTriggerEnabled; }

protected:
	virtual void FireTargetEvent(AActor* Other, APawn* EventInstigator);
	bool IsRelevantOverlap(AActor* Other) const;
	bool IsWithinUE1Cylinder(const AActor* Other) const;
	// Trigger.UnTouch: a relevant toucher left the exact cylinder (or the broad
	// phase) - untrigger the Event actors.
	void UE1UnTouch(AActor* Other);
	void ScheduleRepeat();
	void OnRepeatTimer();
	float LastFireTime = -1e9f;
	bool bSpent = false;
	bool bTriggerEnabled = true;
	FTimerHandle RepeatTimerHandle;
	TSet<TWeakObjectPtr<AActor>> ActiveUE1Overlaps;
	WoTUE1Collision::FTouchTracker UE1Touches;
};

// Registry-bearing location marker for authored UE1 tags whose source actor is
// otherwise absent from the generated map.
UCLASS()
class AWoTTaggedMarker : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTTaggedMarker();

	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;

	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor*, APawn*) override {}
	virtual FName GetUE1Tag() const override { return TagName; }
};

// UE1 Dispatcher: on trigger, fires OutEvents[i] after OutDelays[i].
UCLASS()
class AWoTDispatcher : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> OutEvents;
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<float> OutDelays;

	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	void SetTriggerEnabled(bool bEnable) { bTriggerEnabled = bEnable; }
	bool IsTriggerEnabled() const { return bTriggerEnabled; }

private:
	bool bTriggerEnabled = true;
};

// UE1 Counter: fires Event after NumToCount triggerings.
UCLASS()
class AWoTCounter : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;
	UPROPERTY(EditAnywhere, Category = "WoT") int32 NumToCount = 1;

	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }

private:
	int32 Remaining = 0;
};
