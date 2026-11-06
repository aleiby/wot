#include "WoTAssertMover.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTAssertMover::AWoTAssertMover()
{
	PrimaryActorTick.bCanEverTick = true;
	OpenTimes.Init(0.0f, 6);
	CloseTimes.Init(0.0f, 6);
	// The export's `InitialState=State` names its custom script state (the
	// AssertTriggerOpenTimed auto state), which has no base-FSM equivalent: by
	// default this subclass owns dispatch and the inherited FSM stays dormant.
	// An instance-authored BASE state (Battle2_02 AssertMover2's
	// TriggerOpenTimed) is importer-stamped over this and hands the whole
	// actor to the base AWoTMover FSM — UE1: an instance InitialState replaces
	// the class's auto state completely (see the InitialState gates below).
	InitialState = EWoTMoverState::None;
}

void AWoTAssertMover::BeginPlay()
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
		OnRep_AssertState();
	}
}

float AWoTAssertMover::OpenDuration() const
{
	// AssertMover.uc:43 InterpolateTo(KeyNum+1, OpenTimes[KeyNum]) from key 0.
	return FMath::Max(Indexed(OpenTimes, 0), UE_SMALL_NUMBER);
}

float AWoTAssertMover::CloseDuration() const
{
	// AssertMover.uc:53 InterpolateTo(KeyNum-1, CloseTimes[KeyNum-1]) from key 1.
	return FMath::Max(Indexed(CloseTimes, 0), UE_SMALL_NUMBER);
}

int32 AWoTAssertMover::NumAuthoredKeys() const
{
	return FMath::Max(KeyPos.Num(), KeyRot.Num());
}

float AWoTAssertMover::Indexed(const TArray<float>& Values, int32 Index) const
{
	return Values.IsValidIndex(Index) ? Values[Index] : 0.0f;
}

void AWoTAssertMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		// Authored base-FSM state: base Mover trigger dispatch, assert FSM dormant.
		Super::WoTTrigger(Other, EventInstigator);
		return;
	}
	// AssertMover.uc:92 — `Open:` begins Disable('Trigger'), and the probe is only
	// re-enabled by the terminal Enable('Trigger') after a full close
	// (AssertMover.uc:130); a Disable persists across same-state GotoState. So the
	// shipped mover ignores Trigger everywhere except the re-armed closed pose:
	// no reopening from the unassert wait, and no cancelling a pending wait
	// mid-opening-leg.
	if (!HasAuthority() || AssertPhase != EWoTAssertMoverPhase::Idle ||
		NumAuthoredKeys() < 2)
	{
		return;
	}
	TriggerInstigator = EventInstigator;
	SetMoverInstigator(EventInstigator);
	bWaitAfterOpeningLeg = false;
	StartOpeningLeg();
}

void AWoTAssertMover::WoTUnTrigger(AActor*, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		// Base Mover states define no UnTrigger handler (Mover TriggerOpenTimed
		// runs open → dwell → close regardless) — ignore it, like retail.
		return;
	}
	// The UnTrigger probe only lands while opening below the top key: the top-key
	// dwell disables it before Sleep(StayOpenTime) (AssertMover.uc:99) and `Wait:`
	// disables it before the unassert timer (AssertMover.uc:111), both re-enabled
	// only after a full close (AssertMover.uc:131). A dwelling door therefore
	// stays open the whole StayOpenTime even if the instigator steps out, and a
	// second UnTrigger cannot shortcut the WaitUnAssertTime wait.
	if (!HasAuthority() || AssertPhase != EWoTAssertMoverPhase::Opening)
	{
		return;
	}
	TriggerInstigator = EventInstigator;
	if (!bCloseImmediately)
	{
		bWaitAfterOpeningLeg = true;
		return;
	}
	// UE1 KeyNum already names the interpolation target. Complete that key bookkeeping before
	// starting the reverse leg; see PORT_NOTES for the mid-leg pose limitation.
	CurrentKeyNum = TargetKeyNum;
	StartClosingLeg();
}

void AWoTAssertMover::EnterTimedPhase(EWoTAssertMoverPhase NewPhase)
{
	AssertPhase = NewPhase;
	AssertPhaseTime = 0.0f;
	AssertPhaseStartServerTime = GetWorld()->GetTimeSeconds();
	ApplyAssertSoundPresentation();
}

void AWoTAssertMover::ApplyAssertSoundPresentation()
{
	const EWoTAssertMoverPhase OldPhase = PresentedAssertPhase;
	const int32 OldCurrent = PresentedCurrentKeyNum;
	const int32 OldTarget = PresentedTargetKeyNum;
	if (OldPhase == AssertPhase && OldCurrent == CurrentKeyNum &&
		OldTarget == TargetKeyNum)
	{
		return;
	}
	PresentedAssertPhase = AssertPhase;
	PresentedCurrentKeyNum = CurrentKeyNum;
	PresentedTargetKeyNum = TargetKeyNum;

	if (AssertPhase == EWoTAssertMoverPhase::Opening)
	{
		if (OldPhase == EWoTAssertMoverPhase::Opening && OldTarget == CurrentKeyNum)
		{
			PresentMoverEdge(OpenedSoundPath);
		}
		PresentMoverEdge(OpeningSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (AssertPhase == EWoTAssertMoverPhase::WaitingUnasserted)
	{
		if (OldPhase == EWoTAssertMoverPhase::Opening)
		{
			// UnTrigger's GotoState('Wait') abandons the Open label before its
			// FinishedOpening call; only Wait's edge is presented.
			PresentMoverEdge(OpenedSoundPath);
		}
	}
	else if (AssertPhase == EWoTAssertMoverPhase::DwellingOpen)
	{
		if (OldPhase == EWoTAssertMoverPhase::Opening)
		{
			PresentMoverEdge(OpenedSoundPath);
		}
	}
	else if (AssertPhase == EWoTAssertMoverPhase::Closing)
	{
		if (OldPhase == EWoTAssertMoverPhase::Closing && OldTarget == CurrentKeyNum)
		{
			PresentMoverEdge(ClosedSoundPath);
		}
		PresentMoverEdge(ClosingSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if ((AssertPhase == EWoTAssertMoverPhase::Idle ||
		AssertPhase == EWoTAssertMoverPhase::Spent) &&
		OldPhase == EWoTAssertMoverPhase::Closing)
	{
		PresentMoverEdge(ClosedSoundPath);
	}
}

void AWoTAssertMover::StartOpeningLeg()
{
	if (CurrentKeyNum + 1 >= NumAuthoredKeys())
	{
		if (bTriggerOnceOnly && bOnceOnlyStopOpen)
		{
			EnterTimedPhase(EWoTAssertMoverPhase::Spent);
		}
		else
		{
			EnterTimedPhase(EWoTAssertMoverPhase::DwellingOpen);
		}
		return;
	}
	LastKeyNum = CurrentKeyNum;
	TargetKeyNum = CurrentKeyNum + 1;
	EnterTimedPhase(EWoTAssertMoverPhase::Opening);
}

void AWoTAssertMover::StartClosingLeg()
{
	if (CurrentKeyNum <= 0)
	{
		EnterTimedPhase(bTriggerOnceOnly ? EWoTAssertMoverPhase::Spent
		                                  : EWoTAssertMoverPhase::Idle);
		return;
	}
	LastKeyNum = CurrentKeyNum;
	TargetKeyNum = CurrentKeyNum - 1;
	EnterTimedPhase(EWoTAssertMoverPhase::Closing);
}

float AWoTAssertMover::CurrentMoveDuration() const
{
	const float Authored = AssertPhase == EWoTAssertMoverPhase::Opening
		? Indexed(OpenTimes, CurrentKeyNum) : Indexed(CloseTimes, TargetKeyNum);
	return FMath::Max(Authored, UE_SMALL_NUMBER);
}

void AWoTAssertMover::ApplyLeg(float Alpha, bool bSweep)
{
	if (ApplyKeyframeLerp(CurrentKeyNum, TargetKeyNum, Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTAssertMover::ApplyGroupReturnLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred: native Assert state-label return semantics need a dedicated FSM.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTAssertMover::ApplyGroupStopLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTAssertMover::Tick(float DeltaTime)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		Super::Tick(DeltaTime);   // base FSM motion (durations via Open/CloseDuration)
		return;
	}
	if (AssertPhase == EWoTAssertMoverPhase::Idle ||
		AssertPhase == EWoTAssertMoverPhase::Spent)
	{
		return;
	}
	AssertPhaseTime += DeltaTime;
	if (AssertPhase == EWoTAssertMoverPhase::Opening ||
		AssertPhase == EWoTAssertMoverPhase::Closing)
	{
		const float Duration = CurrentMoveDuration();
		const EWoTAssertMoverPhase AppliedPhase = AssertPhase;
		const int32 AppliedCurrentKey = CurrentKeyNum;
		const int32 AppliedTargetKey = TargetKeyNum;
		ApplyLeg(FMath::Clamp(AssertPhaseTime / Duration, 0.0f, 1.0f), HasAuthority());
		if (AssertPhase != AppliedPhase || CurrentKeyNum != AppliedCurrentKey ||
			TargetKeyNum != AppliedTargetKey)
		{
			return;
		}
		if (HasAuthority() && AssertPhaseTime >= Duration)
		{
			if (AssertPhase == EWoTAssertMoverPhase::Opening)
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
	if (AssertPhase == EWoTAssertMoverPhase::WaitingUnasserted &&
		AssertPhaseTime >= WaitUnAssertTime)
	{
		StartClosingLeg();
	}
	else if (AssertPhase == EWoTAssertMoverPhase::DwellingOpen &&
		AssertPhaseTime >= StayOpenTime)
	{
		StartClosingLeg();
	}
}

void AWoTAssertMover::FinishOpeningLeg()
{
	CurrentKeyNum = TargetKeyNum;
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, TriggerInstigator.Get());
	if (bWaitAfterOpeningLeg)
	{
		bWaitAfterOpeningLeg = false;
		EnterTimedPhase(EWoTAssertMoverPhase::WaitingUnasserted);
	}
	else
	{
		StartOpeningLeg();
	}
}

void AWoTAssertMover::FinishClosingLeg()
{
	CurrentKeyNum = TargetKeyNum;
	if (CurrentKeyNum > 0)
	{
		StartClosingLeg();
	}
	else
	{
		EnterTimedPhase(bTriggerOnceOnly ? EWoTAssertMoverPhase::Spent
		                                  : EWoTAssertMoverPhase::Idle);
	}
}

void AWoTAssertMover::OnRep_AssertState()
{
	if (!HasActorBegunPlay() || InitialState != EWoTMoverState::None)
	{
		return;   // base-FSM instances replicate through the base Phase notify
	}
	const AGameStateBase* GS = GetWorld()->GetGameState();
	AssertPhaseTime = GS
		? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - AssertPhaseStartServerTime)
		: 0.0f;
	if (AssertPhase == EWoTAssertMoverPhase::Opening ||
		AssertPhase == EWoTAssertMoverPhase::Closing)
	{
		ApplyLeg(FMath::Clamp(AssertPhaseTime / CurrentMoveDuration(), 0.0f, 1.0f), false);
	}
	else
	{
		ApplyKeyframeLerp(CurrentKeyNum, CurrentKeyNum, 0.0f, false);
	}
	ApplyAssertSoundPresentation();
}

void AWoTAssertMover::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAssertMover, AssertPhase);
	DOREPLIFETIME(AWoTAssertMover, AssertPhaseStartServerTime);
	DOREPLIFETIME(AWoTAssertMover, CurrentKeyNum);
	DOREPLIFETIME(AWoTAssertMover, TargetKeyNum);
}
