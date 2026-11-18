#include "WoTGradualMover.h"

#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTGradualMover::AWoTGradualMover()
{
	PrimaryActorTick.bCanEverTick = true;
	OpenTimes.Init(0.0f, 6);
	CloseTimes.Init(0.0f, 6);
	Tags.Init(NAME_None, 6);
	Events.Init(NAME_None, 6);
}

bool AWoTGradualMover::IsGradualState() const
{
	return IsGradualMode(EWoTMoverState::GradualTriggerOpenTimed) ||
		IsGradualMode(EWoTMoverState::GradualTriggerPound) ||
		IsGradualMode(EWoTMoverState::GradualTriggerToggle);
}

bool AWoTGradualMover::IsGradualMode(EWoTMoverState Mode) const
{
	if (InitialState == Mode)
	{
		return true;
	}
	if (InitialState != EWoTMoverState::None)
	{
		return false;
	}
	if (Mode == EWoTMoverState::GradualTriggerOpenTimed)
	{
		return NativeInitialState == FName(TEXT("GradualTriggerOpenTimed"));
	}
	if (Mode == EWoTMoverState::GradualTriggerPound)
	{
		return NativeInitialState == FName(TEXT("GradualTriggerPound"));
	}
	return NativeInitialState == FName(TEXT("GradualTriggerToggle"));
}

void AWoTGradualMover::BeginPlay()
{
	if (!Tags.IsValidIndex(0))
	{
		Tags.SetNum(6);
	}
	if (!Events.IsValidIndex(0))
	{
		Events.SetNum(6);
	}
	if (ShouldInitializeTagEventAtBeginPlay())
	{
		TagName = Tags[0];
		EventName = Events[0];
	}
	if (HasAuthority())
	{
		CurrentKeyNum = 0;
		TargetKeyNum = 0;
		bIsFullyOpen = false;
	}
	Super::BeginPlay();
	if (IsMoverFSMInert())
	{
		return;
	}
	if (!HasAuthority() && IsGradualState())
	{
		OnRep_GradualState();
	}
}

int32 AWoTGradualMover::NumAuthoredKeys() const
{
	return FMath::Max(KeyPos.Num(), KeyRot.Num());
}

int32 AWoTGradualMover::OpenTimedTopKey() const
{
	return FMath::Max(0, NumAuthoredKeys() - 1);
}

float AWoTGradualMover::Indexed(const TArray<float>& Values, int32 Index) const
{
	return Values.IsValidIndex(Index) ? Values[Index] : 0.0f;
}

void AWoTGradualMover::UpdateTagEvent(int32 Index)
{
	if (!HasAuthority())
	{
		return;
	}
	UWoTEventSubsystem* EventsSubsystem = GetWorld()->GetSubsystem<UWoTEventSubsystem>();
	EventsSubsystem->UnregisterActor(this);
	TagName = Tags.IsValidIndex(Index) ? Tags[Index] : NAME_None;
	EventName = Events.IsValidIndex(Index) ? Events[Index] : NAME_None;
	EventsSubsystem->RegisterTag(TagName, this);
}

void AWoTGradualMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (!IsGradualState())
	{
		Super::WoTTrigger(Other, EventInstigator);
		return;
	}
	if (!HasAuthority() || GradualPhase != EWoTGradualPhase::Idle ||
		NumAuthoredKeys() < 2)
	{
		return;
	}
	SavedTrigger = Other;
	TriggerInstigator = EventInstigator;
	SetMoverInstigator(EventInstigator);
	if (IsGradualMode(EWoTMoverState::GradualTriggerPound))
	{
		bPoundActive = IsValid(Other);
	}
	if (IsGradualMode(EWoTMoverState::GradualTriggerToggle) && bIsFullyOpen)
	{
		StartClosingLeg();
	}
	else
	{
		StartOpeningLeg();
	}
}

void AWoTGradualMover::WoTUnTrigger(AActor*, APawn* EventInstigator)
{
	if (IsMoverFSMInert() || !IsGradualState() ||
		!IsGradualMode(EWoTMoverState::GradualTriggerPound) || !HasAuthority())
	{
		return;
	}
	bPoundActive = false;
	SavedTrigger.Reset();
	TriggerInstigator = EventInstigator;
	if (GradualPhase == EWoTGradualPhase::Opening)
	{
		const int32 PreviousKeyNum = CurrentKeyNum;
		const float OpenFraction = FMath::Clamp(GradualPhaseTime /
			CurrentGradualDuration(), 0.0f, 1.0f);
		CurrentKeyNum = TargetKeyNum;
		TargetKeyNum = PreviousKeyNum;
		LastKeyNum = CurrentKeyNum;
		const float CloseDuration = FMath::Max(
			Indexed(CloseTimes, TargetKeyNum), UE_SMALL_NUMBER);
		EnterGradualPhase(EWoTGradualPhase::Closing,
			CloseDuration * (1.0f - OpenFraction), true);
		return;
	}
	if (CurrentKeyNum > 0)
	{
		StartClosingLeg();
	}
}

void AWoTGradualMover::EnterGradualPhase(EWoTGradualPhase NewPhase,
	float StartOffset, bool bReversal)
{
	GradualPhase = NewPhase;
	bGradualReversing = bReversal;
	GradualPhaseTime = StartOffset;
	GradualPhaseStartServerTime = GetWorld()->GetTimeSeconds() - StartOffset;
	ApplyGradualSoundPresentation();
}

void AWoTGradualMover::StartOpeningLeg()
{
	if (CurrentKeyNum >= OpenTimedTopKey())
	{
		if (IsGradualMode(EWoTMoverState::GradualTriggerToggle))
		{
			bIsFullyOpen = true;
			StopMoverAmbient();
			EnterGradualPhase(EWoTGradualPhase::Idle);
		}
		else if (IsGradualMode(EWoTMoverState::GradualTriggerPound))
		{
			StartClosingLeg();
		}
		else
		{
			EnterGradualPhase(EWoTGradualPhase::DwellingOpen);
		}
		return;
	}
	LastKeyNum = CurrentKeyNum;
	TargetKeyNum = CurrentKeyNum + 1;
	EnterGradualPhase(EWoTGradualPhase::Opening);
}

void AWoTGradualMover::StartClosingLeg()
{
	if (CurrentKeyNum <= 0)
	{
		FinishClosingLeg();
		return;
	}
	LastKeyNum = CurrentKeyNum;
	TargetKeyNum = CurrentKeyNum - 1;
	EnterGradualPhase(EWoTGradualPhase::Closing);
}

float AWoTGradualMover::CurrentGradualDuration() const
{
	const float Value = GradualPhase == EWoTGradualPhase::Opening
		? Indexed(OpenTimes, CurrentKeyNum) : Indexed(CloseTimes, TargetKeyNum);
	return FMath::Max(Value, UE_SMALL_NUMBER);
}

void AWoTGradualMover::ApplyGradualLeg(float Alpha, bool bSweep)
{
	if (ApplyKeyframeLerp(CurrentKeyNum, TargetKeyNum, Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTGradualMover::ApplyGroupReturnLocal()
{
	if (IsGradualState())
	{
		// Deferred: Gradual's Open/Close label re-entry needs a dedicated replicated reversal.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTGradualMover::ApplyGroupStopLocal()
{
	if (IsGradualState())
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTGradualMover::FinishOpeningLeg()
{
	CurrentKeyNum = TargetKeyNum;
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, TriggerInstigator.Get());
	if (CurrentKeyNum >= OpenTimedTopKey())
	{
		StartOpeningLeg();
		return;
	}
	if (!IsGradualMode(EWoTMoverState::GradualTriggerToggle))
	{
		const FName CurrentTag = Tags.IsValidIndex(CurrentKeyNum)
			? Tags[CurrentKeyNum] : NAME_None;
		const FName PreviousTag = Tags.IsValidIndex(LastKeyNum)
			? Tags[LastKeyNum] : NAME_None;
		if (CurrentTag != PreviousTag)
		{
			UpdateTagEvent(CurrentKeyNum);
			if (IsGradualMode(EWoTMoverState::GradualTriggerOpenTimed))
			{
				EnterGradualPhase(EWoTGradualPhase::Idle);
				return;
			}
		}
	}
	StartOpeningLeg();
}

void AWoTGradualMover::FinishClosingLeg()
{
	if (GradualPhase == EWoTGradualPhase::Closing)
	{
		CurrentKeyNum = TargetKeyNum;
	}
	if (CurrentKeyNum > 0)
	{
		StartClosingLeg();
		return;
	}
	UpdateTagEvent(0);
	if (IsGradualMode(EWoTMoverState::GradualTriggerPound))
	{
		EnterGradualPhase(EWoTGradualPhase::DwellingClosed);
	}
	else
	{
		bIsFullyOpen = false;
		StopMoverAmbient();
		EnterGradualPhase(EWoTGradualPhase::Idle);
	}
}

void AWoTGradualMover::ApplyGradualSoundPresentation()
{
	const EWoTGradualPhase OldPhase = PresentedGradualPhase;
	const int32 OldCurrent = PresentedCurrentKeyNum;
	const int32 OldTarget = PresentedTargetKeyNum;
	if (OldPhase == GradualPhase && OldCurrent == CurrentKeyNum &&
		OldTarget == TargetKeyNum)
	{
		return;
	}
	PresentedGradualPhase = GradualPhase;
	PresentedCurrentKeyNum = CurrentKeyNum;
	PresentedTargetKeyNum = TargetKeyNum;
	if (GradualPhase == EWoTGradualPhase::Opening)
	{
		if (OldPhase == EWoTGradualPhase::Opening && OldTarget == CurrentKeyNum)
		{
			PresentMoverEdge(OpenedSoundPath);
		}
		PresentMoverEdge(OpeningSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (GradualPhase == EWoTGradualPhase::Closing)
	{
		if (OldPhase == EWoTGradualPhase::Closing && OldTarget == CurrentKeyNum)
		{
			PresentMoverEdge(ClosedSoundPath);
		}
		else if (OldPhase == EWoTGradualPhase::Opening && !bGradualReversing)
		{
			PresentMoverEdge(OpenedSoundPath);
		}
		PresentMoverEdge(ClosingSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (OldPhase == EWoTGradualPhase::Opening)
	{
		PresentMoverEdge(OpenedSoundPath);
	}
	else if (OldPhase == EWoTGradualPhase::Closing)
	{
		PresentMoverEdge(ClosedSoundPath);
	}
}

void AWoTGradualMover::Tick(float DeltaTime)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (!IsGradualState())
	{
		Super::Tick(DeltaTime);
		return;
	}
	if (GradualPhase == EWoTGradualPhase::Idle ||
		GradualPhase == EWoTGradualPhase::Spent)
	{
		return;
	}
	GradualPhaseTime += DeltaTime;
	if (GradualPhase == EWoTGradualPhase::Opening ||
		GradualPhase == EWoTGradualPhase::Closing)
	{
		const float Duration = CurrentGradualDuration();
		const EWoTGradualPhase AppliedPhase = GradualPhase;
		const int32 AppliedCurrentKey = CurrentKeyNum;
		const int32 AppliedTargetKey = TargetKeyNum;
		ApplyGradualLeg(FMath::Clamp(GradualPhaseTime / Duration, 0.0f, 1.0f),
			HasAuthority());
		if (GradualPhase != AppliedPhase || CurrentKeyNum != AppliedCurrentKey ||
			TargetKeyNum != AppliedTargetKey)
		{
			return;
		}
		if (HasAuthority() && GradualPhaseTime >= Duration)
		{
			if (GradualPhase == EWoTGradualPhase::Opening)
			{
				FinishOpeningLeg();
			}
			else
			{
				FinishClosingLeg();
			}
		}
		return;
	}
	if (!HasAuthority())
	{
		return;
	}
	if (GradualPhase == EWoTGradualPhase::DwellingOpen &&
		GradualPhaseTime >= StayOpenTime)
	{
		if (ClearAmbientAfterOpenDwell())
		{
			StopMoverAmbient();
		}
		if (bTriggerOnceOnly)
		{
			EnterGradualPhase(EWoTGradualPhase::Spent);
		}
		else
		{
			StartClosingLeg();
		}
	}
	else if (GradualPhase == EWoTGradualPhase::DwellingClosed &&
		GradualPhaseTime >= StayOpenTime)
	{
		if (bTriggerOnceOnly)
		{
			if (ClearAmbientWhenPoundSpent())
			{
				StopMoverAmbient();
			}
			EnterGradualPhase(EWoTGradualPhase::Spent);
		}
		else if (bPoundActive && SavedTrigger.IsValid())
		{
			StartOpeningLeg();
		}
		else
		{
			bPoundActive = false;
			SavedTrigger.Reset();
			EnterGradualPhase(EWoTGradualPhase::Idle);
		}
	}
}

void AWoTGradualMover::OnRep_GradualState()
{
	if (!HasActorBegunPlay() || !IsGradualState())
	{
		return;
	}
	const AGameStateBase* GS = GetWorld()->GetGameState();
	GradualPhaseTime = GS ? FMath::Max(0.0f,
		GS->GetServerWorldTimeSeconds() - GradualPhaseStartServerTime) : 0.0f;
	if (GradualPhase == EWoTGradualPhase::Opening ||
		GradualPhase == EWoTGradualPhase::Closing)
	{
		ApplyGradualLeg(FMath::Clamp(GradualPhaseTime /
			CurrentGradualDuration(), 0.0f, 1.0f), false);
	}
	else
	{
		ApplyKeyframeLerp(CurrentKeyNum, CurrentKeyNum, 0.0f, false);
	}
	ApplyGradualSoundPresentation();
}

void AWoTGradualMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTGradualMover, GradualPhase);
	DOREPLIFETIME(AWoTGradualMover, GradualPhaseStartServerTime);
	DOREPLIFETIME(AWoTGradualMover, CurrentKeyNum);
	DOREPLIFETIME(AWoTGradualMover, TargetKeyNum);
	DOREPLIFETIME(AWoTGradualMover, bPoundActive);
	DOREPLIFETIME(AWoTGradualMover, bGradualReversing);
	DOREPLIFETIME(AWoTGradualMover, bIsFullyOpen);
}
