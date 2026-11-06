#include "WoTLoopMover.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTLoopMover::AWoTLoopMover()
{
	PrimaryActorTick.bCanEverTick = true;
	// Unauthored instances run the LoopMove cycle (None keeps the inherited FSM
	// dormant); an authored BASE InitialState replaces the class auto state
	// completely (UE1 semantics) and defers to the base FSM below. No shipped
	// LoopMover authors one (they author LoopMove itself, which maps to None).
	InitialState = EWoTMoverState::None;
}

void AWoTLoopMover::BeginPlay()
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
		NextKeyNum = 0;
		ReplicatedNextKeyNum = 0;
		ApplyKeyframeLerp(0, 0, 0.0f);
	}
	else
	{
		NextKeyNum = ReplicatedNextKeyNum;
		OnRep_LoopState();
	}
}

int32 AWoTLoopMover::NumAuthoredKeys() const
{
	return FMath::Max(KeyPos.Num(), KeyRot.Num());
}

void AWoTLoopMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		Super::WoTTrigger(Other, EventInstigator);   // authored base state: base dispatch
		return;
	}
	if (!HasAuthority() || NumAuthoredKeys() < 2)
	{
		return;
	}
	if (LoopPhase == EWoTLoopMoverPhase::Moving)
	{
		// UnTrigger re-enables Trigger before its FinishInterpolation. Reasserting during that
		// final leg cancels the pending stop and resumes the endless loop.
		bStopAfterLeg = false;
		TriggerInstigator = EventInstigator;
		return;
	}
	TriggerInstigator = EventInstigator;
	SetMoverInstigator(EventInstigator);
	bStopAfterLeg = false;
	StartNextLeg();
}

void AWoTLoopMover::WoTUnTrigger(AActor*, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		return;   // base Mover states define no UnTrigger handler
	}
	if (!HasAuthority() || LoopPhase != EWoTLoopMoverPhase::Moving)
	{
		return;
	}
	TriggerInstigator = EventInstigator;
	bStopAfterLeg = true;
}

void AWoTLoopMover::StartNextLeg()
{
	NextKeyNum = CurrentKeyNum + 1;
	if (NextKeyNum >= NumAuthoredKeys())
	{
		NextKeyNum = 0;
	}
	ReplicatedNextKeyNum = NextKeyNum;
	LoopPhase = EWoTLoopMoverPhase::Moving;
	SegmentTime = 0.0f;
	SegmentStartServerTime = GetWorld()->GetTimeSeconds();
	ApplyLoopSoundPresentation();
}

void AWoTLoopMover::ApplyLoopSoundPresentation()
{
	const EWoTLoopMoverPhase OldPhase = PresentedLoopPhase;
	const int32 OldCurrent = PresentedCurrentKeyNum;
	const int32 OldNext = PresentedNextKeyNum;
	if (OldPhase == LoopPhase && OldCurrent == CurrentKeyNum &&
		OldNext == ReplicatedNextKeyNum)
	{
		return;
	}
	PresentedLoopPhase = LoopPhase;
	PresentedCurrentKeyNum = CurrentKeyNum;
	PresentedNextKeyNum = ReplicatedNextKeyNum;
	if (LoopPhase == EWoTLoopMoverPhase::Moving)
	{
		if (OldPhase == EWoTLoopMoverPhase::Moving && OldNext == CurrentKeyNum)
		{
			PresentMoverEdge(OpenedSoundPath);
		}
		PresentMoverEdge(OpeningSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (OldPhase == EWoTLoopMoverPhase::Moving)
	{
		PresentMoverEdge(OpenedSoundPath);
		// LoopMover.InterpolateEnd is empty: the move ambient intentionally
		// remains assigned after InactiveState parks the mover.
	}
}

void AWoTLoopMover::ApplySegment(float Alpha, bool bSweep)
{
	if (ApplyKeyframeLerp(CurrentKeyNum, ReplicatedNextKeyNum, Alpha, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

void AWoTLoopMover::ApplyGroupReturnLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred: LoopMove has no retail Close label; native group verbs are inert.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTLoopMover::ApplyGroupStopLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTLoopMover::Tick(float DeltaTime)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		Super::Tick(DeltaTime);
		return;
	}
	if (LoopPhase != EWoTLoopMoverPhase::Moving)
	{
		return;
	}
	SegmentTime += DeltaTime;
	const float Duration = FMath::Max(MoveTime, UE_SMALL_NUMBER);
	const EWoTLoopMoverPhase AppliedPhase = LoopPhase;
	const int32 AppliedCurrentKey = CurrentKeyNum;
	const int32 AppliedNextKey = ReplicatedNextKeyNum;
	ApplySegment(FMath::Clamp(SegmentTime / Duration, 0.0f, 1.0f), HasAuthority());
	if (LoopPhase != AppliedPhase || CurrentKeyNum != AppliedCurrentKey ||
		ReplicatedNextKeyNum != AppliedNextKey)
	{
		return;
	}
	if (!HasAuthority() || SegmentTime < Duration)
	{
		return;
	}
	CurrentKeyNum = ReplicatedNextKeyNum;
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, TriggerInstigator.Get());
	if (bStopAfterLeg)
	{
		LoopPhase = EWoTLoopMoverPhase::Stopped;
		SegmentTime = 0.0f;
		SegmentStartServerTime = GetWorld()->GetTimeSeconds();
		ApplyLoopSoundPresentation();
		return;
	}
	StartNextLeg();
}

void AWoTLoopMover::OnRep_LoopState()
{
	if (!HasActorBegunPlay() || InitialState != EWoTMoverState::None)
	{
		return;   // base-FSM instances replicate through the base Phase notify
	}
	if (LoopPhase == EWoTLoopMoverPhase::Moving)
	{
		const AGameStateBase* GS = GetWorld()->GetGameState();
		SegmentTime = GS
			? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - SegmentStartServerTime)
			: 0.0f;
		ApplySegment(FMath::Clamp(SegmentTime / FMath::Max(MoveTime, UE_SMALL_NUMBER),
			0.0f, 1.0f), false);
	}
	else
	{
		SegmentTime = 0.0f;
		ApplyKeyframeLerp(CurrentKeyNum, CurrentKeyNum, 0.0f, false);
	}
	ApplyLoopSoundPresentation();
}

void AWoTLoopMover::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTLoopMover, LoopPhase);
	DOREPLIFETIME(AWoTLoopMover, SegmentStartServerTime);
	DOREPLIFETIME(AWoTLoopMover, CurrentKeyNum);
	DOREPLIFETIME(AWoTLoopMover, ReplicatedNextKeyNum);
}
