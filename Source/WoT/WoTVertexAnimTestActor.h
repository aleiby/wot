#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTVertexAnimComponent.h"
#include "WoTVertexAnimTestActor.generated.h"

// Bring-up actor: loops one original sequence on a converted vertex-anim
// mesh so the screenshot harness can verify tweening visually.
UCLASS()
class AWoTVertexAnimTestActor : public AActor
{
	GENERATED_BODY()

public:
	AWoTVertexAnimTestActor();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName PlaySequence;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float PlayRate = 1.0f;

	virtual void BeginPlay() override;
};
