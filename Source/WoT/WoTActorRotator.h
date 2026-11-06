#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTActorRotator.generated.h"

// Legend.ActorRotator: moves linked presentation actors around an oriented
// circular path while radius, angular rate, and height wander between authored
// limits. Retail replicated its randomized state; this presentation-only port
// instead derives that state deterministically from the actor name so clients
// and repeat runs share the same phase without replication.
UCLASS()
class AWoTActorRotator : public AActor
{
	GENERATED_BODY()

public:
	AWoTActorRotator();

	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator")
	TArray<TSoftObjectPtr<AActor>> DrivenActors;

	// Empty uses this actor's location as the shared center. A populated array
	// supplies the per-actor centers used by UE1's bLongDistance mode.
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator")
	TArray<FVector> DrivenOrigins;

	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MinRadius = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MaxRadius = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float RadiusShiftRate = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bBounceRadius = false;

	// Rotation rates remain in UE1 rotator units/second (65536 = one turn).
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MinRotationRate = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MaxRotationRate = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float RotationShiftRate = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bBounceRotationRate = false;

	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MinHeight = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float MaxHeight = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") float HeightShiftRate = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bBounceHeight = false;

	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bUpdateActorRotation = true;

	// UE1 PHYS_Rotating for script-spawned rotators (Pixie / PixieJarRotator):
	// RotationRate pitch/yaw tumble the orbit plane each tick; the roll leg is
	// dead in retail too (the native Update() overwrites Roll from the FRoll
	// accumulator every tick). UE1 rotator units/second; zero = off (every
	// map-authored rotator).
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator")
	FRotator PhysRotationRateUnits = FRotator::ZeroRotator;

	// Stored in UE1 rotator units and converted to degrees only when applied.
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator")
	FRotator ActorRotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bFaceIn = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Actor Rotator") bool bFaceOut = false;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// Public like the retail UnrealScript helper; also permits deterministic
	// re-initialization by headless specs.
	void Initialize();

	float GetFRoll() const { return FRoll; }
	float GetNextHeight() const { return NextHeight; }

private:
	void UpdateRadius(float DeltaSeconds);
	void UpdateRotationRate(float DeltaSeconds);
	void UpdateHeight(float DeltaSeconds);
	void CalcNextRadius();
	void CalcNextRotationRate();
	void CalcNextHeight();
	bool HasResolvedDrivenActor() const;
	bool HasStaticRoot(AActor* DrivenActor);

	float CurrentRadius = 0.0f;
	float NextRadius = 0.0f;
	float CurrentRotationRate = 0.0f;
	float NextRotationRate = 0.0f;
	float FRoll = 0.0f;
	float CurrentHeight = 0.0f;
	float NextHeight = 0.0f;
	// PHYS_Rotating pitch/yaw accumulators (UE1 units; wrap like FRoll). Kept
	// outside the actor rotation so quat normalization never folds the spin.
	float PhysPitchUnits = 0.0f;
	float PhysYawUnits = 0.0f;

	FRandomStream RandomStream;
	TSet<TWeakObjectPtr<AActor>> WarnedStaticActors;
};

