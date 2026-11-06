#include "WoTTimedMover.h"

#include "WoTAmbientSound.h"
#include "WoTSound.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTTimedMover::AWoTTimedMover()
{
	PrimaryActorTick.bCanEverTick = true;
	Delays.Init(0.0f, 8);
	MoveTimes.Init(0.0f, 8);
	Sounds.Init(NAME_None, 8);
	// Unauthored instances run the TriggerTimedMover sequence (None keeps the
	// inherited FSM dormant); an authored BASE InitialState replaces the class
	// auto state completely (UE1 semantics) and defers to the base FSM below
	// (only the SP Tutorial map authors one — TriggerToggle).
	InitialState = EWoTMoverState::None;
}

void AWoTTimedMover::BeginPlay()
{
	Super::BeginPlay();
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		return;   // base FSM owns this instance; Super seeded the base pose
	}
	if (HasAuthority())
	{
		CurrentKeyNum = 0;
		TargetKeyNum = 0;
		ApplyKeyframeLerp(0, 0, 0.0f);
	}
	else
	{
		OnRep_SequenceState();
	}
}

int32 AWoTTimedMover::NumAuthoredKeys() const
{
	return FMath::Max(KeyPos.Num(), KeyRot.Num());
}

float AWoTTimedMover::Indexed(const TArray<float>& Values, int32 Index) const
{
	return Values.IsValidIndex(Index) ? Values[Index] : 0.0f;
}

void AWoTTimedMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		// TimedMover.uc overrides no DoOpen/DoClose, so an authored base state is
		// pure base behavior (MoveTime legs, not the per-key MoveTimes sequence).
		Super::WoTTrigger(Other, EventInstigator);
		return;
	}
	if (!HasAuthority() || bSpent || SequencePhase != EWoTTimedMoverPhase::Idle ||
		NumAuthoredKeys() < 2)
	{
		return;
	}
	TriggerInstigator = EventInstigator;
	SetMoverInstigator(EventInstigator);
	EnableMatchingTriggers(false);
	Direction = EWoTTimedMoverDirection::Opening;
	SequenceIndex = 1;
	StartMove(SequenceIndex);
}

void AWoTTimedMover::StartMove(int32 NewTarget)
{
	TargetKeyNum = FMath::Clamp(NewTarget, 0, NumAuthoredKeys() - 1);
	SequencePhase = EWoTTimedMoverPhase::Moving;
	StepTime = 0.0f;
	LastAppliedAlpha = 0.0f;
	StepStartServerTime = GetWorld()->GetTimeSeconds();
	ApplyStepSound();
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::StartDelay()
{
	SequencePhase = EWoTTimedMoverPhase::Delaying;
	StepTime = 0.0f;
	StepStartServerTime = GetWorld()->GetTimeSeconds();
	StopStepSound();
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::Tick(float DeltaTime)
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::Tick(DeltaTime);
		return;
	}
	if (IsMoverFSMInert())
	{
		return;
	}
	if (SequencePhase == EWoTTimedMoverPhase::Idle ||
		SequencePhase == EWoTTimedMoverPhase::Stopped)
	{
		return;
	}
	if (!HasAuthority())
	{
		StepTime += DeltaTime;
		if (SequencePhase != EWoTTimedMoverPhase::Moving)
		{
			return;
		}
		const float Duration = FMath::Max(Indexed(MoveTimes, TargetKeyNum), UE_SMALL_NUMBER);
		ApplyStep(FMath::Clamp(StepTime / Duration, 0.0f, 1.0f), false);
		return;
	}

	// UE1 latent InterpolateTo/Sleep always yield for at least one tick, even at zero duration —
	// retail quantizes every leg/delay to >= 1 frame. We instead consume the tick's remainder
	// across boundaries, an accepted sub-frame divergence (short authored legs run at their exact
	// authored time rather than stretching a frame per label). The guard is above the fixed
	// array's maximum number of forward+reverse steps.
	float Remaining = DeltaTime;
	for (int32 Transitions = 0; Transitions < 32 &&
		SequencePhase != EWoTTimedMoverPhase::Idle &&
		SequencePhase != EWoTTimedMoverPhase::Stopped; ++Transitions)
	{
		if (SequencePhase == EWoTTimedMoverPhase::Moving)
		{
			const float Duration = Indexed(MoveTimes, TargetKeyNum);
			if (Duration <= 0.0f)
			{
				const EWoTTimedMoverPhase AppliedPhase = SequencePhase;
				const EWoTTimedMoverDirection AppliedDirection = Direction;
				const int32 AppliedCurrentKey = CurrentKeyNum;
				const int32 AppliedTargetKey = TargetKeyNum;
				ApplyStep(1.0f, true);
				if (SequencePhase != AppliedPhase || Direction != AppliedDirection ||
					CurrentKeyNum != AppliedCurrentKey || TargetKeyNum != AppliedTargetKey)
				{
					break;
				}
				CurrentKeyNum = TargetKeyNum;
				StartDelay();
				continue;
			}
			const float Used = FMath::Min(Remaining, FMath::Max(0.0f, Duration - StepTime));
			StepTime += Used;
			Remaining -= Used;
			const EWoTTimedMoverPhase AppliedPhase = SequencePhase;
			const EWoTTimedMoverDirection AppliedDirection = Direction;
			const int32 AppliedCurrentKey = CurrentKeyNum;
			const int32 AppliedTargetKey = TargetKeyNum;
			ApplyStep(FMath::Clamp(StepTime / Duration, 0.0f, 1.0f), true);
			if (SequencePhase != AppliedPhase || Direction != AppliedDirection ||
				CurrentKeyNum != AppliedCurrentKey || TargetKeyNum != AppliedTargetKey)
			{
				break;
			}
			if (StepTime < Duration)
			{
				break;
			}
			CurrentKeyNum = TargetKeyNum;
			StartDelay();
			continue;
		}

		const float Delay = Indexed(Delays, TargetKeyNum);
		if (Delay <= 0.0f)
		{
			AdvanceSequence();
			continue;
		}
		const float Used = FMath::Min(Remaining, FMath::Max(0.0f, Delay - StepTime));
		StepTime += Used;
		Remaining -= Used;
		if (StepTime < Delay)
		{
			break;
		}
		AdvanceSequence();
	}
}

void AWoTTimedMover::ApplyStep(float Alpha, bool bSweep)
{
	if (ApplyKeyframeLerp(CurrentKeyNum, TargetKeyNum, Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTTimedMover::AdvanceSequence()
{
	if (Direction == EWoTTimedMoverDirection::Opening)
	{
		++SequenceIndex;
		if (SequenceIndex < NumAuthoredKeys())
		{
			StartMove(SequenceIndex);
			return;
		}
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			EventName, this, TriggerInstigator.Get());
		if (bTriggerOnceOnly)
		{
			bSpent = true;
			FinishSequence();
			return;
		}
		Direction = EWoTTimedMoverDirection::Closing;
		SequenceIndex = NumAuthoredKeys() - 2;
		StartMove(SequenceIndex);
		return;
	}

	--SequenceIndex;
	if (SequenceIndex >= 0)
	{
		StartMove(SequenceIndex);
		return;
	}
	EnableMatchingTriggers(true);
	FinishSequence();
}

void AWoTTimedMover::FinishSequence()
{
	SequencePhase = EWoTTimedMoverPhase::Idle;
	StepTime = 0.0f;
	StepStartServerTime = GetWorld()->GetTimeSeconds();
	StopStepSound();
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::EnableMatchingTriggers(bool bEnable)
{
	for (TActorIterator<AWoTTrigger> It(GetWorld()); It; ++It)
	{
		if (It->EventName == TagName)
		{
			It->SetTriggerEnabled(bEnable);
		}
	}
}

void AWoTTimedMover::ApplyStepSound()
{
	SetMoverAmbient(Sounds.IsValidIndex(TargetKeyNum)
		? Sounds[TargetKeyNum] : NAME_None, true);
}

void AWoTTimedMover::ApplySequenceSoundPresentation()
{
	const EWoTTimedMoverPhase OldPhase = PresentedSequencePhase;
	const EWoTTimedMoverDirection OldDirection = PresentedDirection;
	const int32 OldTarget = PresentedTargetKeyNum;
	if (OldPhase == SequencePhase && OldDirection == Direction &&
		OldTarget == TargetKeyNum)
	{
		return;
	}
	PresentedSequencePhase = SequencePhase;
	PresentedDirection = Direction;
	PresentedTargetKeyNum = TargetKeyNum;

	if (SequencePhase == EWoTTimedMoverPhase::Moving)
	{
		if (Direction != OldDirection)
		{
			if (Direction == EWoTTimedMoverDirection::Closing)
			{
				if (CurrentKeyNum >= NumAuthoredKeys() - 1)
				{
					PresentMoverEdge(OpenedSoundPath);
				}
				PresentMoverEdge(ClosingSoundPath);
			}
			else
			{
				PresentMoverEdge(OpeningSoundPath);
			}
		}
		else if (OldPhase == EWoTTimedMoverPhase::Idle)
		{
			PresentMoverEdge(Direction == EWoTTimedMoverDirection::Opening
				? OpeningSoundPath : ClosingSoundPath);
		}
		ApplyStepSound();
	}
	else if (SequencePhase == EWoTTimedMoverPhase::Delaying)
	{
		StopStepSound();
	}
	else if (SequencePhase == EWoTTimedMoverPhase::Stopped)
	{
		StopStepSound();
	}
	else
	{
		StopStepSound();
		if (OldPhase != EWoTTimedMoverPhase::Idle)
		{
			if (Direction == EWoTTimedMoverDirection::Opening &&
				CurrentKeyNum >= NumAuthoredKeys() - 1)
			{
				PresentMoverEdge(OpenedSoundPath);
			}
			else if (Direction == EWoTTimedMoverDirection::Closing &&
				CurrentKeyNum <= 0)
			{
				PresentMoverEdge(ClosedSoundPath);
			}
		}
	}
}

void AWoTTimedMover::StopStepSound()
{
	StopMoverAmbient();
}

void AWoTTimedMover::ApplyGroupReturnLocal()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::ApplyGroupReturnLocal();
		return;
	}
	if (!HasAuthority() || SequencePhase != EWoTTimedMoverPhase::Moving)
	{
		return;
	}
	const float OldDuration = FMath::Max(
		Indexed(MoveTimes, TargetKeyNum), UE_SMALL_NUMBER);
	const float OldAlpha = FMath::Clamp(StepTime / OldDuration, 0.0f, 1.0f);
	const int32 OldCurrentKey = CurrentKeyNum;
	CurrentKeyNum = TargetKeyNum;
	TargetKeyNum = OldCurrentKey;
	StopStepSound();
	Direction = Direction == EWoTTimedMoverDirection::Opening
		? EWoTTimedMoverDirection::Closing : EWoTTimedMoverDirection::Opening;
	SequenceIndex = TargetKeyNum;
	const float NewDuration = FMath::Max(
		Indexed(MoveTimes, TargetKeyNum), UE_SMALL_NUMBER);
	LastAppliedAlpha = 1.0f - OldAlpha;
	StepTime = LastAppliedAlpha * NewDuration;
	StepStartServerTime = GetWorld()->GetTimeSeconds() - StepTime;
	ApplyStepSound();
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::ApplyGroupStopLocal()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::ApplyGroupStopLocal();
		return;
	}
	if (!HasAuthority() || SequencePhase != EWoTTimedMoverPhase::Moving)
	{
		return;
	}
	EnableMatchingTriggers(true);
	StoppedStepAlpha = LastAppliedAlpha;
	SequencePhase = EWoTTimedMoverPhase::Stopped;
	StepTime = 0.0f;
	StepStartServerTime = GetWorld()->GetTimeSeconds();
	StopStepSound();
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::RewindEncroachClockToLastAppliedPose()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::RewindEncroachClockToLastAppliedPose();
		return;
	}
	if (SequencePhase == EWoTTimedMoverPhase::Moving)
	{
		StepTime = LastAppliedAlpha * FMath::Max(
			Indexed(MoveTimes, TargetKeyNum), 0.0f);
	}
}

void AWoTTimedMover::OnRep_SequenceState()
{
	if (!HasActorBegunPlay() || InitialState != EWoTMoverState::None)
	{
		return;   // base-FSM instances replicate through the base Phase notify
	}
	const AGameStateBase* GS = GetWorld()->GetGameState();
	StepTime = GS ? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - StepStartServerTime)
	              : 0.0f;
	if (SequencePhase == EWoTTimedMoverPhase::Moving)
	{
		const float Duration = FMath::Max(Indexed(MoveTimes, TargetKeyNum), UE_SMALL_NUMBER);
		ApplyStep(FMath::Clamp(StepTime / Duration, 0.0f, 1.0f), false);
	}
	else if (SequencePhase == EWoTTimedMoverPhase::Stopped)
	{
		ApplyStep(StoppedStepAlpha, false);
	}
	else
	{
		ApplyKeyframeLerp(CurrentKeyNum, CurrentKeyNum, 0.0f, false);
	}
	ApplySequenceSoundPresentation();
}

void AWoTTimedMover::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTTimedMover, SequencePhase);
	DOREPLIFETIME(AWoTTimedMover, Direction);
	DOREPLIFETIME(AWoTTimedMover, StepStartServerTime);
	DOREPLIFETIME(AWoTTimedMover, CurrentKeyNum);
	DOREPLIFETIME(AWoTTimedMover, TargetKeyNum);
	DOREPLIFETIME(AWoTTimedMover, StoppedStepAlpha);
}
