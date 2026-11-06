#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTElevatorMover.generated.h"

UENUM()
enum class EWoTElevatorPhase : uint8
{
	Idle,
	Moving,
};

class AWoTStageTrigger;

// ElevatorMover.uc: an externally driven, one-key-per-leg mover that walks to
// an arbitrary key over a caller-supplied total duration.
UCLASS()
class AWoTElevatorMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTElevatorMover();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void MoveKeyframe(int32 NewKeyNum, float NewMoveTime);
	bool CanMoveKey() const { return bMoveKey; }
	int32 GetCurrentKey() const { return CurrentKeyNum; }

protected:
	virtual void ApplyAlpha(float Alpha, bool bSweep = true) override;
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState)
	EWoTElevatorPhase ElevatorPhase = EWoTElevatorPhase::Idle;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) int32 CurrentKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) int32 TargetKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) int32 DestinationKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) int32 MoveDirection = 0;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) float LegStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) float MoveTimeInterval = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_ElevatorState) bool bMoveKey = true;

	UFUNCTION() void OnRep_ElevatorState();
	void StartNextLeg();
	void FinishLeg();
	void ApplyLeg(float Alpha, bool bSweep);
	void ApplyElevatorSoundPresentation();
	int32 NumAuthoredKeys() const;

	float LegTime = 0.0f;
	TWeakObjectPtr<AWoTStageTrigger> MyTrigger;
	EWoTElevatorPhase PresentedElevatorPhase = EWoTElevatorPhase::Idle;
	int32 PresentedCurrentKeyNum = 0;
	int32 PresentedTargetKeyNum = 0;
	int32 PresentedDirection = 0;
};
