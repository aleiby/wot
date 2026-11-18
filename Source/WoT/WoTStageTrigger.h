#pragma once

#include "CoreMinimal.h"
#include "WoTEvents.h"
#include "WoTStageTrigger.generated.h"

// StageTrigger.uc: WOTDispatcher out-events increment/decrement a clamped
// stage and drive every matching ElevatorMover to that keyframe.
UCLASS()
class AWoTStageTrigger : public AWoTTrigger
{
	GENERATED_BODY()

public:
	AWoTStageTrigger();

	UPROPERTY(EditAnywhere, Category = "WoT|Stage") FName TagDec;
	UPROPERTY(EditAnywhere, Category = "WoT|Stage") FName TagInc;
	UPROPERTY(EditAnywhere, Category = "WoT|Stage") int32 MaxKeyFrame = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|Stage") TArray<FName> WaterEvents;
	UPROPERTY(EditAnywhere, Category = "WoT|Stage") TArray<FName> DispatcherTags;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Stage") int32 CurrentStage = 0;

	virtual void BeginPlay() override;
	void EnableTriggers(bool bEnable);
	void ChangeStage(AActor* Other, int32 Delta);

private:
	TArray<TWeakObjectPtr<AWoTDispatcher>> Dispatchers;
};
