#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTAttachMover.generated.h"

// AttachMover.uc: attach every non-mover with AttachTag, plus mover slaves,
// preserving each actor's world transform.
UCLASS()
class AWoTAttachMover : public AWoTMover
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|AttachMover") FName AttachTag;
	virtual void BeginPlay() override;

protected:
	void AttachTaggedActors();
};
