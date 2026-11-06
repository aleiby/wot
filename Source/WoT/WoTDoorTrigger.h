#pragma once

#include "CoreMinimal.h"
#include "WoTEvents.h"
#include "WoTDoorTrigger.generated.h"

UENUM()
enum class EWoTDoorDirection : uint8
{
	Forward,
	Backward,
};

// DoorTrigger.uc: a normal level-authored trigger whose Direction selects which
// of a DoorMover's two open keys is used. Non-door targets receive a plain Trigger.
UCLASS()
class AWoTDoorTrigger : public AWoTTrigger
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT")
	EWoTDoorDirection Direction = EWoTDoorDirection::Forward;

protected:
	virtual void FireTargetEvent(AActor* Other, APawn* EventInstigator) override;
};
