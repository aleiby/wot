#include "WoTElevatorMover.h"

#include "WoTStageTrigger.h"

#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTElevatorMover::AWoTElevatorMover()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AWoTElevatorMover::BeginPlay()
{
	Super::BeginPlay();
	if (IsMoverFSMInert())
	{
		return;
	}
	if (HasAuthority())
	{
		CurrentKeyNum = 0;
		TargetKeyNum = 0;
		DestinationKeyNum = 0;
		bMoveKey = true;
		for (TActorIterator<AWoTStageTrigger> It(GetWorld()); It; ++It)
		{
			if (It->EventName == TagName)
			{
				MyTrigger = *It;
				break;
			}
		}
	}
	else
	{
		OnRep_ElevatorState();
	}
}

int32 AWoTElevatorMover::NumAuthoredKeys() const
{
	return FMath::Max(KeyPos.Num(), KeyRot.Num());
}

void AWoTElevatorMover::ApplyAlpha(float Alpha, bool bSweep)
{
	// Elevator DoOpen/DoClose move one key, not across the entire key chain.
	if (ApplyKeyframeLerp(0, FMath::Min(1, NumAuthoredKeys() - 1), Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTElevatorMover::ApplyGroupReturnLocal()
{
	if (InitialState == EWoTMoverState::None ||
		ElevatorPhase == EWoTElevatorPhase::Moving)
	{
		// Deferred: ElevatorTriggerGradual has no implemented retail group-verb FSM.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTElevatorMover::ApplyGroupStopLocal()
{
	if (InitialState == EWoTMoverState::None ||
		ElevatorPhase == EWoTElevatorPhase::Moving)
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTElevatorMover::MoveKeyframe(int32 NewKeyNum, float NewMoveTime)
{
	if (!HasAuthority() || IsMoverFSMInert() || !bMoveKey ||
		NumAuthoredKeys() < 1)
	{
		return;
	}
	DestinationKeyNum = FMath::Clamp(NewKeyNum, 0, NumAuthoredKeys() - 1);
	if (DestinationKeyNum == CurrentKeyNum)
	{
		return;
	}
	MoveDirection = DestinationKeyNum > CurrentKeyNum ? 1 : -1;
	MoveTimeInterval = FMath::Max(0.0f, NewMoveTime) /
		FMath::Abs(DestinationKeyNum - CurrentKeyNum);
	bMoveKey = false;
	StartNextLeg();
}

void AWoTElevatorMover::StartNextLeg()
{
	if (MyTrigger.IsValid())
	{
		MyTrigger->EnableTriggers(false);
	}
	TargetKeyNum = CurrentKeyNum + MoveDirection;
	ElevatorPhase = EWoTElevatorPhase::Moving;
	LegTime = 0.0f;
	LegStartServerTime = GetWorld()->GetTimeSeconds();
	ApplyElevatorSoundPresentation();
}

void AWoTElevatorMover::ApplyLeg(float Alpha, bool bSweep)
{
	if (ApplyKeyframeLerp(CurrentKeyNum, TargetKeyNum, Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTElevatorMover::FinishLeg()
{
	CurrentKeyNum = TargetKeyNum;
	if (MoveDirection < 0)
	{
		// Shipped crossed pairing: a falling leg calls FinishedOpening.
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			EventName, this, nullptr);
	}
	if (MyTrigger.IsValid())
	{
		MyTrigger->EnableTriggers(true);
	}
	if (CurrentKeyNum != DestinationKeyNum)
	{
		StartNextLeg();
		return;
	}
	ElevatorPhase = EWoTElevatorPhase::Idle;
	bMoveKey = true;
	LegTime = 0.0f;
	LegStartServerTime = GetWorld()->GetTimeSeconds();
	ApplyElevatorSoundPresentation();
}

void AWoTElevatorMover::ApplyElevatorSoundPresentation()
{
	const EWoTElevatorPhase OldPhase = PresentedElevatorPhase;
	const int32 OldCurrent = PresentedCurrentKeyNum;
	const int32 OldTarget = PresentedTargetKeyNum;
	const int32 OldDirection = PresentedDirection;
	if (OldPhase == ElevatorPhase && OldCurrent == CurrentKeyNum &&
		OldTarget == TargetKeyNum && OldDirection == MoveDirection)
	{
		return;
	}
	PresentedElevatorPhase = ElevatorPhase;
	PresentedCurrentKeyNum = CurrentKeyNum;
	PresentedTargetKeyNum = TargetKeyNum;
	PresentedDirection = MoveDirection;

	auto PresentCrossedFinish = [this](int32 Direction)
	{
		PresentMoverEdge(Direction > 0 ? ClosedSoundPath : OpenedSoundPath);
	};
	if (ElevatorPhase == EWoTElevatorPhase::Moving)
	{
		if (OldPhase == EWoTElevatorPhase::Moving && OldTarget == CurrentKeyNum)
		{
			PresentCrossedFinish(OldDirection);
		}
		PresentMoverEdge(MoveDirection > 0 ? OpeningSoundPath : ClosingSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (OldPhase == EWoTElevatorPhase::Moving)
	{
		PresentCrossedFinish(OldDirection);
	}
}

void AWoTElevatorMover::Tick(float DeltaTime)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (ElevatorPhase != EWoTElevatorPhase::Moving)
	{
		Super::Tick(DeltaTime);
		return;
	}
	if (!HasAuthority())
	{
		LegTime += DeltaTime;
		ApplyLeg(FMath::Clamp(LegTime /
			FMath::Max(MoveTimeInterval, UE_SMALL_NUMBER), 0.0f, 1.0f), false);
		return;
	}
	LegTime += DeltaTime;
	if (MoveTimeInterval <= 0.0f)
	{
		const EWoTElevatorPhase AppliedPhase = ElevatorPhase;
		const int32 AppliedCurrentKey = CurrentKeyNum;
		const int32 AppliedTargetKey = TargetKeyNum;
		ApplyLeg(1.0f, true);
		if (ElevatorPhase != AppliedPhase || CurrentKeyNum != AppliedCurrentKey ||
			TargetKeyNum != AppliedTargetKey)
		{
			return;
		}
		FinishLeg();
		return;
	}
	const EWoTElevatorPhase AppliedPhase = ElevatorPhase;
	const int32 AppliedCurrentKey = CurrentKeyNum;
	const int32 AppliedTargetKey = TargetKeyNum;
	ApplyLeg(FMath::Clamp(LegTime / MoveTimeInterval, 0.0f, 1.0f), true);
	if (ElevatorPhase != AppliedPhase || CurrentKeyNum != AppliedCurrentKey ||
		TargetKeyNum != AppliedTargetKey)
	{
		return;
	}
	if (LegTime >= MoveTimeInterval)
	{
		FinishLeg();
	}
}

void AWoTElevatorMover::OnRep_ElevatorState()
{
	if (!HasActorBegunPlay())
	{
		return;
	}
	const AGameStateBase* GS = GetWorld()->GetGameState();
	LegTime = GS ? FMath::Max(0.0f,
		GS->GetServerWorldTimeSeconds() - LegStartServerTime) : 0.0f;
	if (ElevatorPhase == EWoTElevatorPhase::Moving)
	{
		ApplyLeg(FMath::Clamp(LegTime /
			FMath::Max(MoveTimeInterval, UE_SMALL_NUMBER), 0.0f, 1.0f), false);
	}
	else
	{
		ApplyKeyframeLerp(CurrentKeyNum, CurrentKeyNum, 0.0f, false);
	}
	ApplyElevatorSoundPresentation();
}

void AWoTElevatorMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTElevatorMover, ElevatorPhase);
	DOREPLIFETIME(AWoTElevatorMover, CurrentKeyNum);
	DOREPLIFETIME(AWoTElevatorMover, TargetKeyNum);
	DOREPLIFETIME(AWoTElevatorMover, DestinationKeyNum);
	DOREPLIFETIME(AWoTElevatorMover, MoveDirection);
	DOREPLIFETIME(AWoTElevatorMover, LegStartServerTime);
	DOREPLIFETIME(AWoTElevatorMover, MoveTimeInterval);
	DOREPLIFETIME(AWoTElevatorMover, bMoveKey);
}
