#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../WoTBudgetInfo.h"
#include "../WoTEvents.h"
#include "Engine/World.h"
#include "WoTTestSoundDispatcher.generated.h"

// Test-only event sink for the BudgetInfo/SoundDispatcher specs. The distinct
// class and helper prefix keep this wave's fixtures independent of the trap specs.
UCLASS()
class AWoTTestSoundDispatcherEventSink : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	UPROPERTY() FName TagName;
	int32 TriggerCount = 0;
	int32 OrderToken = 0;
	int32 ObservedBudgetEditCount = INDEX_NONE;
	TWeakObjectPtr<AActor> LastOther;
	TWeakObjectPtr<APawn> LastInstigator;

	// `inline static` so there is no out-of-line definition to strand. UHT emits
	// this class's vtable and reflection unconditionally, so definitions that
	// lived inside `#if WITH_AUTOMATION_TESTS` in the .cpp vanished in
	// Test/Shipping while the vtable still referenced them, and the monolithic
	// link failed. Declaration and definition stay together.
	inline static TArray<int32> TriggerOrder;
	static void ResetTriggerOrder() { TriggerOrder.Reset(); }

	virtual void BeginPlay() override
	{
		Super::BeginPlay();
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	}

	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override
	{
		++TriggerCount;
		LastOther = Other;
		LastInstigator = EventInstigator;
		TriggerOrder.Add(OrderToken);
		if (const AWoTBudgetInfo* Budget = Cast<AWoTBudgetInfo>(Other))
		{
			ObservedBudgetEditCount = Budget->PlayerEditCount;
		}
	}
	virtual FName GetUE1Tag() const override { return TagName; }
};
