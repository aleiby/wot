#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTLoopMover.generated.h"

UENUM()
enum class EWoTLoopMoverPhase : uint8
{
	Stopped,
	Moving,
};

// LoopMover.uc: each trigger starts an endless key-by-key cycle; UnTrigger lets the current leg
// finish and then stops. The wrap leg interpolates directly from the final key to key zero.
UCLASS()
class AWoTLoopMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTLoopMover();

	// Serialized by UE1 even though runtime recalculates it at each leg.
	UPROPERTY(EditAnywhere, Category = "WoT") int32 NextKeyNum = 0;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	bool IsLooping() const { return LoopPhase == EWoTLoopMoverPhase::Moving; }
	int32 GetCurrentKey() const { return CurrentKeyNum; }

protected:
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_LoopState) EWoTLoopMoverPhase LoopPhase =
		EWoTLoopMoverPhase::Stopped;
	UPROPERTY(ReplicatedUsing = OnRep_LoopState) float SegmentStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_LoopState) int32 CurrentKeyNum = 0;
	UPROPERTY(ReplicatedUsing = OnRep_LoopState) int32 ReplicatedNextKeyNum = 0;

	UFUNCTION() void OnRep_LoopState();
	void StartNextLeg();
	void ApplySegment(float Alpha, bool bSweep);
	void ApplyLoopSoundPresentation();
	int32 NumAuthoredKeys() const;

	float SegmentTime = 0.0f;
	bool bStopAfterLeg = false;
	TWeakObjectPtr<APawn> TriggerInstigator;
	EWoTLoopMoverPhase PresentedLoopPhase = EWoTLoopMoverPhase::Stopped;
	int32 PresentedCurrentKeyNum = 0;
	int32 PresentedNextKeyNum = 0;
};
