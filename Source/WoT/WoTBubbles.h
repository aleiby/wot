#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTBubbles.generated.h"

// Port of WOT.BubbleSpawner + WOTBubble (exports/script/WOT/BubbleSpawner.uc,
// WOTBubble.uc): the underwater damage bubbles — GenericDamageHelper.uc:131-134
// spawns one group at the victim's mouth on ANY damaging hit while the head is
// in a water zone. One group = 1-8 bubbles at 0.1s cadence (bDestroy=True kills
// the spawner after its group); each bubble is a tiny translucent sprite
// (bubble1-4, 25% each; DrawScale 0.03-0.11 +0-50%) rising at
// g*(Buoyancy/Mass - 1) with Mass 3 / Buoyancy 3.1-4.1 (UE1's
// buoyant-falling model, g=950), dying on leaving water or after 3-7s.

// The local cosmetic bubble group (never replicated; each machine runs its own
// from the burst marker). One actor ticks its whole group.
UCLASS()
class AWoTBubbles : public AActor
{
	GENERATED_BODY()

public:
	AWoTBubbles();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FBubble
	{
		TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
		FVector Position = FVector::ZeroVector;
		float RiseSpeed = 0.0f;    // integrates RiseAccel
		float RiseAccel = 0.0f;    // 950 * (Buoyancy/Mass - 1), per bubble
		float DieAt = 0.0f;        // world seconds (LifeSpan 3-7)
	};
	TArray<FBubble> Bubbles;
	int32 GroupSize = 0;      // rolled 1-8 (MinBubbles/MaxBubbles)
	int32 NumSpawned = 0;
	float NextBubbleTime = 0.0f;   // 0.1s cadence (TimeBetweenBubbles)

	void SpawnBubble();
};

// The networked one-shot marker (the AWoTWeaveBurst pattern): the server decides
// the bubbles at the damage site; every machine's BeginPlay runs the local group.
UCLASS()
class AWoTBubbleBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTBubbleBurst();

	virtual void BeginPlay() override;
};

namespace WoTBubbles
{
	// Server-side: spawn the replicated bubble marker (no-op off authority).
	AWoTBubbleBurst* SpawnBurst(UWorld& World, const FVector& Location,
	                            const FRotator& Rotation);
}
