#include "WoTDoorMover.h"

#include "WoTDoorTrigger.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

AWoTDoorMover::AWoTDoorMover()
{
	PrimaryActorTick.bCanEverTick = true;
	// TriggerDoor is a custom script state with no base mover-FSM equivalent.
	InitialState = EWoTMoverState::None;
	// DoorMover.uc redeclares the LockableMover presentation fields without
	// defaults, overriding the base LockableMover CDO strings/timing.
	MinLockSoundTime = 0.0f;
	LockedMessage.Empty();
	UnLockMessage.Empty();
	LockedMessageDuration = 0.0f;
	UnLockMessageDuration = 0.0f;
}

void AWoTDoorMover::BeginPlay()
{
	Super::BeginPlay();
	if (IsMoverFSMInert())
	{
		return;
	}
	if (HasAuthority())
	{
		bUnlocked = bUnlocked || !bLockable;
		OpenKeyNum = 1;
		ApplyKeyframeLerp(0, 0, 0.0f);
	}
	else
	{
		OnRep_DoorState();
	}
}

bool AWoTDoorMover::IsUnlocked(AActor* Other)
{
	if (!bLockable)
	{
		return true;
	}
	if (bStayUnlocked && bUnlocked)
	{
		return true;
	}
	if (APawn* P = Cast<APawn>(Other))
	{
		if (CheckForKey(P))
		{
			PresentUnlock(P);
			bUnlocked = true;
		}
		else
		{
			PresentLocked(P);
		}
	}
	return bUnlocked;
}

void AWoTDoorMover::WoTTriggerDirectional(bool bForward, AActor* Other,
	                                       APawn* EventInstigator)
{
	if (InitialState != EWoTMoverState::None || !HasAuthority() || IsMoverFSMInert() ||
		DoorPhase != EWoTDoorPhase::Idle || !IsUnlocked(Other))
	{
		return;
	}
	OpenKeyNum = bForward ? 1 : 2;
	const int32 NumKeys = FMath::Max(KeyPos.Num(), KeyRot.Num());
	if (OpenKeyNum >= NumKeys)
	{
		return;
	}
	TriggerInstigator = EventInstigator;
	SetMoverInstigator(EventInstigator);
	EnableTriggers(false);
	EnterDoorPhase(EWoTDoorPhase::Opening);
}

void AWoTDoorMover::EnterDoorPhase(EWoTDoorPhase NewPhase, float StartOffset)
{
	DoorPhase = NewPhase;
	DoorPhaseTime = StartOffset;
	DoorPhaseStartServerTime = GetWorld()->GetTimeSeconds() - StartOffset;
	ApplyDoorSoundPresentation();
}

void AWoTDoorMover::ApplyDoorPose(bool bSweep)
{
	const float Duration = FMath::Max(MoveTime, UE_SMALL_NUMBER);
	const float Alpha = FMath::Clamp(DoorPhaseTime / Duration, 0.0f, 1.0f);
	if (DoorPhase == EWoTDoorPhase::Opening)
	{
		if (ApplyKeyframeLerp(0, OpenKeyNum, Alpha, bSweep))
		{
			LastAppliedAlpha = Alpha;
		}
	}
	else if (DoorPhase == EWoTDoorPhase::Closing)
	{
		if (ApplyKeyframeLerp(OpenKeyNum, 0, Alpha, bSweep))
		{
			LastAppliedAlpha = 1.0f - Alpha;
		}
	}
	else if (DoorPhase == EWoTDoorPhase::Stopped)
	{
		ApplyKeyframeLerp(0, OpenKeyNum, DoorStoppedAlpha, bSweep);
	}
}

void AWoTDoorMover::ApplyDoorSoundPresentation()
{
	if (DoorPhase == PresentedDoorPhase)
	{
		return;
	}
	PresentedDoorPhase = DoorPhase;
	switch (DoorPhase)
	{
	case EWoTDoorPhase::Opening:
		PresentMoverEdge(OpeningSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
		break;
	case EWoTDoorPhase::Dwelling:
		PresentMoverEdge(OpenedSoundPath);
		StopMoverAmbient();
		break;
	case EWoTDoorPhase::Closing:
		PresentMoverEdge(ClosingSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
		break;
	case EWoTDoorPhase::Idle:
		PresentMoverEdge(ClosedSoundPath);
		StopMoverAmbient();
		break;
	case EWoTDoorPhase::Stopped:
		StopMoverAmbient();
		break;
	}
}

void AWoTDoorMover::Tick(float DeltaTime)
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::Tick(DeltaTime);
		return;
	}
	if (IsMoverFSMInert() || DoorPhase == EWoTDoorPhase::Idle ||
		DoorPhase == EWoTDoorPhase::Stopped)
	{
		return;
	}
	if (!HasAuthority())
	{
		DoorPhaseTime += DeltaTime;
		ApplyDoorPose(false);
		return;
	}

	// UE1 latent InterpolateTo/Sleep always yield for at least one tick, even at
	// zero duration — retail quantizes each label to >= 1 frame. We instead
	// consume the tick remainder across boundaries, an accepted sub-frame
	// divergence that keeps short authored times exact.
	float Remaining = DeltaTime;
	for (int32 Transitions = 0; Transitions < 4 &&
		DoorPhase != EWoTDoorPhase::Idle; ++Transitions)
	{
		const float Duration = DoorPhase == EWoTDoorPhase::Dwelling
			? FMath::Max(StayOpenTime, 0.0f) : FMath::Max(MoveTime, 0.0f);
		if (Duration <= 0.0f)
		{
			const EWoTDoorPhase AppliedPhase = DoorPhase;
			if (DoorPhase == EWoTDoorPhase::Opening)
			{
				if (ApplyKeyframeLerp(0, OpenKeyNum, 1.0f))
				{
					LastAppliedAlpha = 1.0f;
				}
				if (DoorPhase != AppliedPhase)
				{
					break;
				}
				FinishOpening();
			}
			else if (DoorPhase == EWoTDoorPhase::Dwelling)
			{
				EnterDoorPhase(EWoTDoorPhase::Closing);
			}
			else
			{
				if (ApplyKeyframeLerp(OpenKeyNum, 0, 1.0f))
				{
					LastAppliedAlpha = 0.0f;
				}
				if (DoorPhase != AppliedPhase)
				{
					break;
				}
				FinishClosing();
			}
			continue;
		}
		const float Used = FMath::Min(Remaining,
			FMath::Max(0.0f, Duration - DoorPhaseTime));
		DoorPhaseTime += Used;
		Remaining -= Used;
		const EWoTDoorPhase AppliedPhase = DoorPhase;
		const int32 AppliedOpenKey = OpenKeyNum;
		ApplyDoorPose(true);
		if (DoorPhase != AppliedPhase || OpenKeyNum != AppliedOpenKey)
		{
			break;
		}
		if (DoorPhaseTime < Duration)
		{
			break;
		}
		if (DoorPhase == EWoTDoorPhase::Opening)
		{
			FinishOpening();
		}
		else if (DoorPhase == EWoTDoorPhase::Dwelling)
		{
			EnterDoorPhase(EWoTDoorPhase::Closing);
		}
		else
		{
			FinishClosing();
		}
	}
}

void AWoTDoorMover::FinishOpening()
{
	ApplyKeyframeLerp(0, OpenKeyNum, 1.0f);
	EnterDoorPhase(EWoTDoorPhase::Dwelling);
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, TriggerInstigator.Get());
}

void AWoTDoorMover::FinishClosing()
{
	ApplyKeyframeLerp(OpenKeyNum, 0, 1.0f);
	EnableTriggers(true);
	EnterDoorPhase(EWoTDoorPhase::Idle);
}

void AWoTDoorMover::ApplyGroupReturnLocal()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::ApplyGroupReturnLocal();
		return;
	}
	const float Duration = FMath::Max(MoveTime, UE_SMALL_NUMBER);
	if (DoorPhase == EWoTDoorPhase::Opening)
	{
		const float OpenFrac = FMath::Clamp(DoorPhaseTime / Duration, 0.0f, 1.0f);
		EnterDoorPhase(EWoTDoorPhase::Closing, Duration * (1.0f - OpenFrac));
	}
	else if (DoorPhase == EWoTDoorPhase::Closing)
	{
		const float CloseFrac = FMath::Clamp(DoorPhaseTime / Duration, 0.0f, 1.0f);
		EnterDoorPhase(EWoTDoorPhase::Opening, Duration * (1.0f - CloseFrac));
	}
}

void AWoTDoorMover::ApplyGroupStopLocal()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::ApplyGroupStopLocal();
		return;
	}
	const float Duration = FMath::Max(MoveTime, UE_SMALL_NUMBER);
	if (DoorPhase == EWoTDoorPhase::Opening)
	{
		DoorStoppedAlpha = FMath::Clamp(DoorPhaseTime / Duration, 0.0f, 1.0f);
	}
	else if (DoorPhase == EWoTDoorPhase::Closing)
	{
		DoorStoppedAlpha = 1.0f -
			FMath::Clamp(DoorPhaseTime / Duration, 0.0f, 1.0f);
	}
	else if (DoorPhase == EWoTDoorPhase::Dwelling)
	{
		DoorStoppedAlpha = 1.0f;
	}
	else
	{
		DoorStoppedAlpha = 0.0f;
	}
	EnterDoorPhase(EWoTDoorPhase::Stopped);
	EnableTriggers(true);
}

void AWoTDoorMover::RewindEncroachClockToLastAppliedPose()
{
	if (InitialState != EWoTMoverState::None)
	{
		Super::RewindEncroachClockToLastAppliedPose();
		return;
	}
	const float Duration = FMath::Max(MoveTime, 0.0f);
	if (DoorPhase == EWoTDoorPhase::Opening)
	{
		DoorPhaseTime = LastAppliedAlpha * Duration;
	}
	else if (DoorPhase == EWoTDoorPhase::Closing)
	{
		DoorPhaseTime = (1.0f - LastAppliedAlpha) * Duration;
	}
}

void AWoTDoorMover::EnableTriggers(bool bEnable)
{
	for (TActorIterator<AWoTDoorTrigger> It(GetWorld()); It; ++It)
	{
		if (It->EventName == TagName)
		{
			It->SetTriggerEnabled(bEnable);
		}
	}
}

void AWoTDoorMover::OnRep_DoorState()
{
	if (!HasActorBegunPlay())
	{
		return;
	}
	const AGameStateBase* GS = GetWorld()->GetGameState();
	DoorPhaseTime = GS
		? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - DoorPhaseStartServerTime)
		: 0.0f;
	if (DoorPhase == EWoTDoorPhase::Idle)
	{
		ApplyKeyframeLerp(0, 0, 0.0f, false);
	}
	else if (DoorPhase == EWoTDoorPhase::Dwelling)
	{
		ApplyKeyframeLerp(OpenKeyNum, OpenKeyNum, 0.0f, false);
	}
	else if (DoorPhase == EWoTDoorPhase::Stopped)
	{
		ApplyKeyframeLerp(0, OpenKeyNum, DoorStoppedAlpha, false);
	}
	else
	{
		ApplyDoorPose(false);
	}
	ApplyDoorSoundPresentation();
}

void AWoTDoorMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDoorMover, DoorPhase);
	DOREPLIFETIME(AWoTDoorMover, OpenKeyNum);
	DOREPLIFETIME(AWoTDoorMover, DoorPhaseStartServerTime);
	DOREPLIFETIME(AWoTDoorMover, DoorStoppedAlpha);
}
