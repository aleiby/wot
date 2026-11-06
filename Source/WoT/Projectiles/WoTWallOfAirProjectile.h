#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTWallOfAirProjectile.generated.h"

// Port of Angreal.AngrealWallOfAirProjectile (expands AngrealProjectile) — the
// wall itself: a stationary, semi-invisible barrier planted a third of the way
// from the caster to the target. Spec:
// exports/script/Angreal/AngrealWallOfAirProjectile.uc.
//
// It never moves under its own power (PHYS_None); each authority Tick it re-plants
// itself between the (living) caster and target and re-faces the target, and dies
// the instant either of them dies. It does NOT block pawns — it intercepts weaves:
// an incoming AngrealProjectile (a weave) is unravelled (destroyed), and any other
// WoTProjectile is knocked to the ground. MashadarGuide/MachinShin pass through
// (UnAffectedTypes); MashadarGuide is live and MachinShin remains a nonmatch.
//
// Mesh/loop, light, sounds, endpoint interpolation, deflection, and WOTSparks
// presentation are all bound to the shipped class.
UCLASS()
class AWoTWallOfAirProjectile : public AWoTAngrealProjectile
{
	GENERATED_BODY()

public:

	// The shimmer wall (AngrealWallOfAirProjectile.uc: Mesh WallOfAir, LoopAnim
	// 'All' 1.0, violet light 88/204/204 r12; 6a-ii).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	AWoTWallOfAirProjectile();

	// The caster and target the wall spans (set by AngrealInvWallOfAir.Cast).
	UPROPERTY() TObjectPtr<AActor> Source;
	UPROPERTY() TObjectPtr<AActor> Destination;

	// Replicated so clients can re-plant the wall without the endpoints
	// (AngrealWallOfAirProjectile.uc:43-47, unreliable if Role==Authority).
	UPROPERTY(Replicated) FVector SourceLocation = FVector::ZeroVector;
	UPROPERTY(Replicated) FVector DestinationLocation = FVector::ZeroVector;

	// Fraction of the caster->target span at which the wall sits.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|WallOfAir") float PctDistFromSource = 0.333333f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> DeflectSound;

	// Projectile class names the wall is not allowed to destroy (UnAffectedTypes).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|WallOfAir") FName UnAffectedTypes[2];

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// AngrealWallOfAirProjectile.IsAffectable: false for UnAffectedTypes.
	bool IsAffectable(const AActor* Other) const;
	void TouchActor(AActor* Other);
	int32 GetSparkBurstCountForTest() const { return SparkBurstCount; }

protected:
	// AngrealWallOfAirProjectile.Touch: unravel weaves, knock down other projectiles
	// (its own overlap handler, bypassing the base pawn/blocker Touch filter).
	UFUNCTION()
	void OnWallTouch(UPrimitiveComponent* OverlappedComp, AActor* Other,
	                 UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                 bool bFromSweep, const FHitResult& Hit);

private:
	int32 SparkBurstCount = 0;
};
