#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTHandPresentation.generated.h"

class APawn;
class UPoseableMeshComponent;

/** Local first-person hand-and-forearm visual, lit like the world's meshes.
    Uses imported hand art (Art/hands via Tools/import/import_first_person_hands.py),
    so it exists only where that content does; guarded builds return null. */
UCLASS()
class WOT_API AWoTHandPresentation : public AActor
{
	GENERATED_BODY()

public:
	AWoTHandPresentation();

	/** A caller-owned hand for a class key and player colour, or null. */
	static AWoTHandPresentation* Spawn(UWorld* World, FName HandClass,
		int32 Color, const FTransform& Transform);

	/** The hand class key and player colour for a playable pawn. */
	static bool StyleForPawn(const APawn* Pawn, FName& OutHandClass, int32& OutColor);

	// Reflected owner properties are consumed by the shared mesh-light baker.
	// A first-person hand sits at the eye, where nearby lights swing it from
	// near black to washed out; it takes the world's lights at reduced strength
	// and a little ambient glow, and the first-person view adds a key light.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	float ScaleGlow = 0.6f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	uint8 AmbientGlow = 16;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	bool bStyleTranslucent = false;

private:
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	TObjectPtr<UPoseableMeshComponent> Hand;
};
