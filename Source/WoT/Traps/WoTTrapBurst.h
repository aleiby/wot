#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTTrapBurst.generated.h"

// Port of WoTTraps/TrapBurst.uc (an Effects actor) — the cosmetic hit-burst spawned by WallSlab,
// PortcullisMover, and FireWallSlab when they take damage. The original's Explode state plays a
// random-rotated 'Explo' mesh anim then Destroys (its PlaySound(EffectSound1) is a no-op — no
// default sound); the port binds the Trapburst mesh + plays 'Explo' in BeginPlay alongside the
// GAMEPLAY-relevant MakeNoise stimulus and the self-destruct. RemoteRole=SimulatedProxy in the
// original (server-spawned, seen on clients) → bReplicates.
UCLASS()
class AWoTTrapBurst : public AActor
{
	GENERATED_BODY()

public:
	// The burst mesh ('WoTTraps.TrapBurst', meshmap scale baked, DrawScale 1.0; the play is in BeginPlay).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> VisualMesh;

public:
	AWoTTrapBurst();

protected:
	virtual void BeginPlay() override;

private:
	// Stand-in for the 'Explo' anim duration (PlayAnim 'Explo',0.6 → FinishAnim → Destroy).
	// The 6-frame play at 0.6 runs ~0.56s; the fixed 1s lifespan holds the last frame briefly
	// instead of destroying on FinishAnim (the noted minor divergence in BeginPlay).
	static constexpr float BurstLifeSpan = 1.0f;
};
