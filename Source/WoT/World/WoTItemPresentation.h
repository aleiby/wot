#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTItemPresentation.generated.h"

class UWoTVertexAnimComponent;
struct FWoTRuntimeActorVisualPresentation;

/** Local pickup-model presentation, independent of inventory simulation. */
UCLASS()
class WOT_API AWoTItemPresentation : public AActor
{
	GENERATED_BODY()

public:
	AWoTItemPresentation();

	/** Resolve a shipped inventory class and return its caller-owned visual. */
	static AWoTItemPresentation* Spawn(UWorld* World, FName ClassName,
		const FTransform& Transform);

	/** Shared construction after class presentation has been decoded. */
	static AWoTItemPresentation* SpawnFromPresentation(UWorld* World,
		const FWoTRuntimeActorVisualPresentation& Presentation,
		const FTransform& Transform);

	// The box of the vertices the shown pose draws, in actor space: the same in
	// a development and a packaged build, unlike the mesh's culling bounds.
	virtual FBox CalculateComponentsBoundingBoxInLocalSpace(
		bool bNonColliding = false,
		bool bIncludeFromChildActors = false) const override;

	// Reflected owner properties are consumed by the shared mesh-light baker.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	uint8 AmbientGlow = 0;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	bool bStyleTranslucent = false;

private:
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;
};
