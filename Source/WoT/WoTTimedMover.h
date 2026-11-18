#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTTimedMover.generated.h"

UENUM()
enum class EWoTTimedMoverDirection : uint8
{
	Opening,
	Closing,
};

UENUM()
enum class EWoTTimedMoverPhase : uint8
{
	Idle,
	Moving,
	Delaying,
	Stopped,
};

// TimedMover.uc: one trigger runs an authored per-key duration/delay/ambient sequence forward,
// then backward unless bTriggerOnceOnly leaves it at the final key.
UCLASS()
class AWoTTimedMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTTimedMover();

	UPROPERTY(EditAnywhere, Category = "WoT|TimedMover") TArray<float> Delays;
	UPROPERTY(EditAnywhere, Category = "WoT|TimedMover") TArray<float> MoveTimes;
	UPROPERTY(EditAnywhere, Category = "WoT|TimedMover") TArray<FName> Sounds;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool IsSequenceIdle() const { return SequencePhase == EWoTTimedMoverPhase::Idle; }
	int32 GetCurrentKey() const { return CurrentKeyNum; }

protected:
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;
	virtual void RewindEncroachClockToLastAppliedPose() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) EWoTTimedMoverPhase SequencePhase =
		EWoTTimedMoverPhase::Idle;
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) EWoTTimedMoverDirection Direction =
		EWoTTimedMoverDirection::Opening;
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) float StepStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) int32 CurrentKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) int32 TargetKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_SequenceState) float StoppedStepAlpha = 0.0f;

	UFUNCTION() void OnRep_SequenceState();
	void StartMove(int32 NewTarget);
	void StartDelay();
	void AdvanceSequence();
	void FinishSequence();
	void EnableMatchingTriggers(bool bEnable);
	void ApplyStep(float Alpha, bool bSweep);
	void ApplyStepSound();
	void StopStepSound();
	void ApplySequenceSoundPresentation();
	float Indexed(const TArray<float>& Values, int32 Index) const;
	int32 NumAuthoredKeys() const;

	float StepTime = 0.0f;
	int32 SequenceIndex = 0;
	TWeakObjectPtr<APawn> TriggerInstigator;
	EWoTTimedMoverPhase PresentedSequencePhase = EWoTTimedMoverPhase::Idle;
	EWoTTimedMoverDirection PresentedDirection = EWoTTimedMoverDirection::Opening;
	int32 PresentedTargetKeyNum = 0;
};
