#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTRotatingMover.generated.h"

// RotatingMover.uc: dormant at startup, spins at RotateRate while asserted.
UCLASS()
class AWoTRotatingMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTRotatingMover();

	// Stored in UE5 degrees/second; the importer converts UE1 rotator units.
	UPROPERTY(EditAnywhere, Category = "WoT") FRotator RotateRate = FRotator::ZeroRotator;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// UnrealScript UnTrigger counterpart. The current event seam only routes Trigger; overlap/end
	// sources that support assertion can call this directly.
	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	bool IsRotating() const { return bRotating; }

protected:
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_RotationState) bool bRotating = false;
	UPROPERTY(ReplicatedUsing = OnRep_RotationState) float RotationStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_RotationState) float AccumulatedRotationTime = 0.0f;

	UFUNCTION() void OnRep_RotationState();
	void ApplyRotationAt(float Elapsed);

	FRotator InitialRotation = FRotator::ZeroRotator;
	float RotationTime = 0.0f;
};
