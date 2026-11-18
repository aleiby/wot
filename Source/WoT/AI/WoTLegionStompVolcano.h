#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTLegionStompVolcano.generated.h"

class UCapsuleComponent;
class UPrimitiveComponent;
class UWoTVertexAnimComponent;
class AWoTCharacter;

// LegionStompVolcano.uc: a server-spawned, replicated Effects actor.  Its live
// retail-cylinder membership (rather than a one-shot radius query) preserves UE1
// Touch semantics: leaving and entering again during the eruption damages again.
UCLASS()
class AWoTLegionStompVolcano : public AActor
{
	GENERATED_BODY()

public:
	AWoTLegionStompVolcano();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// LegionStompVolcano.uc:30,44,46.
	static constexpr float VolcanoRadius = 48.0f;
	static constexpr float VolcanoHalfHeight = 16.0f;
	static constexpr float VolcanoDamage = 10.0f;
	static constexpr float VolcanoLaunchZ = 256.0f;
	static constexpr float VolcanoAnimRate = 0.5f;

	// The two FinishAnim waits resolved from DT_WoTAnimSequences.  Public for the
	// parity spec; the fallback is the same authored 10 frames at 30 fps.
	static float ResolveEruptionLifetimeSeconds();
	float GetEruptionLifetimeSeconds() const { return EruptionLifetimeSeconds; }

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UCapsuleComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;

private:
	UFUNCTION()
	void OnVolcanoBeginOverlap(UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor, UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnVolcanoEndOverlap(UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor, UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex);

	UFUNCTION()
	void OnVolcanoAnimEnd(FName Sequence);

	void ApplyTouch(AActor* OtherActor);
	void BindAndPlayVisual();
	bool IsInsideRetailCylinder(const AWoTCharacter* Victim) const;
	void UpdateCylinderMembership(AActor* OtherActor);
	void SweepCylinderMembership();
	void SpawnSprayers();

	// The UE component is broadphase only: UCapsuleComponent clamps its 16-unit
	// half-height up to the 48-unit radius.  Retail Touch state instead follows
	// the exact CollisionRadius=48 / CollisionHeight=16 cylinder below.
	TSet<TWeakObjectPtr<AWoTCharacter>> BroadphasePawns;
	TSet<TWeakObjectPtr<AWoTCharacter>> CylinderMembers;

	float EruptionLifetimeSeconds = 0.0f;
};
