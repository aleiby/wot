#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTGradualMover.generated.h"

UENUM()
enum class EWoTGradualPhase : uint8
{
	Idle,
	Opening,
	DwellingOpen,
	Closing,
	DwellingClosed,
	Spent,
};

// GradualMover.uc: one-key legs whose Tag/Event rotate between authored slots.
UCLASS()
class AWoTGradualMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTGradualMover();

	UPROPERTY(EditAnywhere, Category = "WoT|GradualMover") TArray<float> OpenTimes;
	UPROPERTY(EditAnywhere, Category = "WoT|GradualMover") TArray<float> CloseTimes;
	// When an authored UnrealScript subclass state maps InitialState to None, retain
	// the label here so the native FSM can select OpenTimed/Pound/Toggle behavior.
	UPROPERTY(EditAnywhere, Category = "WoT|GradualMover") FName NativeInitialState;
	// The shipped `Tags[6]` uses AActor::Tags as its native storage; the
	// constructor fixes that inherited array to six slots.
	UPROPERTY(EditAnywhere, Category = "WoT|GradualMover") TArray<FName> Events;
	UPROPERTY(VisibleAnywhere, Category = "WoT|GradualMover") int32 LastKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState, VisibleAnywhere,
		Category = "WoT|GradualMover") bool bIsFullyOpen = false;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	int32 GetCurrentKey() const { return CurrentKeyNum; }
	EWoTGradualPhase GetGradualPhase() const { return GradualPhase; }

protected:
	UPROPERTY(ReplicatedUsing = OnRep_GradualState)
	EWoTGradualPhase GradualPhase = EWoTGradualPhase::Idle;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState) float GradualPhaseStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState) int32 CurrentKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState) int32 TargetKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState) bool bPoundActive = false;
	UPROPERTY(ReplicatedUsing = OnRep_GradualState) bool bGradualReversing = false;

	virtual int32 OpenTimedTopKey() const;
	virtual void FinishOpeningLeg();
	virtual void FinishClosingLeg();
	void StartOpeningLeg();
	void StartClosingLeg();
	void EnterGradualPhase(EWoTGradualPhase NewPhase, float StartOffset = 0.0f,
		bool bReversal = false);
	virtual void ApplyGradualLeg(float Alpha, bool bSweep);
	float CurrentGradualDuration() const;
	float Indexed(const TArray<float>& Values, int32 Index) const;
	int32 NumAuthoredKeys() const;
	void UpdateTagEvent(int32 Index);
	virtual bool IsGradualState() const;
	bool IsGradualMode(EWoTMoverState Mode) const;
	virtual bool ShouldInitializeTagEventAtBeginPlay() const { return true; }
	virtual bool ClearAmbientAfterOpenDwell() const { return true; }
	virtual bool ClearAmbientWhenPoundSpent() const { return true; }
	virtual void ApplyGradualSoundPresentation();
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

	float GradualPhaseTime = 0.0f;
	TWeakObjectPtr<AActor> SavedTrigger;
	TWeakObjectPtr<APawn> TriggerInstigator;

private:
	UFUNCTION() void OnRep_GradualState();
	EWoTGradualPhase PresentedGradualPhase = EWoTGradualPhase::Idle;
	int32 PresentedCurrentKeyNum = 0;
	int32 PresentedTargetKeyNum = 0;
};
