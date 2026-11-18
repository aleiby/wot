#pragma once

#include "CoreMinimal.h"
#include "WoTGradualMover.h"
#include "WoTMixMover.generated.h"

// MixMover.uc: GradualMover's state surface plus AttachTag and an externally
// driven keyframe walk using OpenTimes/CloseTimes.
UCLASS()
class AWoTMixMover : public AWoTGradualMover
{
	GENERATED_BODY()

public:
	AWoTMixMover();

	UPROPERTY(EditAnywhere, Category = "WoT|MixMover") FName AttachTag;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void MoveKeyframe(int32 NewKeyNum);
	bool CanMoveKey() const { return bMoveKey; }

protected:
	virtual int32 OpenTimedTopKey() const override;
	virtual bool IsGradualState() const override;
	virtual bool ShouldInitializeTagEventAtBeginPlay() const override;
	virtual bool ClearAmbientAfterOpenDwell() const override { return false; }
	virtual bool ClearAmbientWhenPoundSpent() const override { return false; }
	virtual void FinishOpeningLeg() override;
	virtual void FinishClosingLeg() override;
	virtual void ApplyGradualLeg(float Alpha, bool bSweep) override;
	virtual void ApplyGradualSoundPresentation() override;
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_MixState) bool bMoveKey = true;
	UPROPERTY(ReplicatedUsing = OnRep_MixState) bool bExternalMove = false;
	UPROPERTY(ReplicatedUsing = OnRep_MixState) int32 MixDestinationKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_MixState) int32 MixMoveDirection = 0;

	UFUNCTION() void OnRep_MixState();
	void ApplyExternalSoundPresentation();
	void StartExternalLeg();
	void FinishExternalLeg();
	void AttachTaggedActors();
	bool bPresentedExternalMove = false;
	EWoTGradualPhase PresentedMixPhase = EWoTGradualPhase::Idle;
	int32 PresentedMixCurrentKeyNum = 0;
	int32 PresentedMixTargetKeyNum = 0;
	int32 PresentedMixDirection = 0;
};
