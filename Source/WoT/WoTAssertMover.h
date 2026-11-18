#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTAssertMover.generated.h"

UENUM()
enum class EWoTAssertMoverPhase : uint8
{
	Idle,
	Opening,
	WaitingUnasserted,
	DwellingOpen,
	Closing,
	Spent,
};

// AssertMover.uc: advances one key at a time while asserted, then returns fully to key zero after
// unassertion (optionally immediately). Per-leg OpenTimes/CloseTimes replace base MoveTime.
UCLASS()
class AWoTAssertMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTAssertMover();

	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") TArray<float> OpenTimes;
	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") TArray<float> CloseTimes;
	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") bool bOnceOnlyStopOpen = false;
	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") float WaitUnAssertTime = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") bool bCloseImmediately = false;
	UPROPERTY(EditAnywhere, Category = "WoT|AssertMover") int32 LastKeyNum = 0;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	int32 GetCurrentKey() const { return CurrentKeyNum; }
	EWoTAssertMoverPhase GetAssertPhase() const { return AssertPhase; }

protected:
	// AssertMover.uc:38-56 DoOpen/DoClose select OpenTimes/CloseTimes as the leg
	// durations. Those are plain-FUNCTION overrides, still live when an authored
	// base InitialState runs the base FSM (Battle2_02 AssertMover2's
	// TriggerOpenTimed): the base open/close legs read these instead of MoveTime.
	// The only shipped base-state instance is 2-key, so the whole base open is
	// the single key-0 leg (OpenTimes[0]; CloseTimes[KeyNum-1] == CloseTimes[0]).
	virtual float OpenDuration() const override;
	virtual float CloseDuration() const override;
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_AssertState) EWoTAssertMoverPhase AssertPhase =
		EWoTAssertMoverPhase::Idle;
	UPROPERTY(ReplicatedUsing = OnRep_AssertState) float AssertPhaseStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_AssertState) int32 CurrentKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_AssertState) int32 TargetKeyNum = 0;

	UFUNCTION() void OnRep_AssertState();
	void StartOpeningLeg();
	void StartClosingLeg();
	void EnterTimedPhase(EWoTAssertMoverPhase NewPhase);
	void FinishOpeningLeg();
	void FinishClosingLeg();
	void ApplyLeg(float Alpha, bool bSweep);
	void ApplyAssertSoundPresentation();
	float Indexed(const TArray<float>& Values, int32 Index) const;
	float CurrentMoveDuration() const;
	int32 NumAuthoredKeys() const;

	float AssertPhaseTime = 0.0f;
	bool bWaitAfterOpeningLeg = false;
	TWeakObjectPtr<APawn> TriggerInstigator;
	EWoTAssertMoverPhase PresentedAssertPhase = EWoTAssertMoverPhase::Idle;
	int32 PresentedCurrentKeyNum = 0;
	int32 PresentedTargetKeyNum = 0;
};
