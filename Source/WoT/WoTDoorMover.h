#pragma once

#include "CoreMinimal.h"
#include "WoTMover.h"
#include "WoTDoorMover.generated.h"

UENUM()
enum class EWoTDoorPhase : uint8
{
	Idle,
	Opening,
	Dwelling,
	Closing,
	Stopped,
};

// DoorMover.uc TriggerDoor: key 0 is closed, key 1 is forward-open, and key 2
// is backward-open. Direction is authored on the DoorTrigger, never inferred.
UCLASS()
class AWoTDoorMover : public AWoTMover
{
	GENERATED_BODY()

public:
	AWoTDoorMover();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void WoTTriggerDirectional(bool bForward, AActor* Other,
	                                   APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	EWoTDoorPhase GetDoorPhase() const { return DoorPhase; }
	int32 GetOpenKey() const { return OpenKeyNum; }
	float GetDoorStoppedAlpha() const { return DoorStoppedAlpha; }

protected:
	virtual void ApplyGroupReturnLocal() override;
	virtual void ApplyGroupStopLocal() override;
	virtual void RewindEncroachClockToLastAppliedPose() override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_DoorState)
	EWoTDoorPhase DoorPhase = EWoTDoorPhase::Idle;
	// Remains 1 or 2 for the whole sequence, so it identifies both the opening
	// 0->key leg and the closing key->0 leg on simulated proxies.
	UPROPERTY(ReplicatedUsing = OnRep_DoorState)
	int32 OpenKeyNum = 1;
	UPROPERTY(ReplicatedUsing = OnRep_DoorState)
	float DoorPhaseStartServerTime = 0.0f;
	UPROPERTY(ReplicatedUsing = OnRep_DoorState)
	float DoorStoppedAlpha = 0.0f;

	UFUNCTION()
	void OnRep_DoorState();
	bool IsUnlocked(AActor* Other);
	void EnterDoorPhase(EWoTDoorPhase NewPhase, float StartOffset = 0.0f);
	void EnableTriggers(bool bEnable);
	void ApplyDoorPose(bool bSweep);
	void ApplyDoorSoundPresentation();
	void FinishOpening();
	void FinishClosing();

	float DoorPhaseTime = 0.0f;
	EWoTDoorPhase PresentedDoorPhase = EWoTDoorPhase::Idle;
	TWeakObjectPtr<APawn> TriggerInstigator;
};
