#include "WoTMixMover.h"

#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

AWoTMixMover::AWoTMixMover()
{
	bMoveKey = true;
}

void AWoTMixMover::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		bMoveKey = true;
	}
	if (HasAuthority() && !bSlave && !AttachTag.IsNone())
	{
		AttachTaggedActors();
	}
}

int32 AWoTMixMover::OpenTimedTopKey() const
{
	// Shipped MixMover compares KeyNum == NumKeys, unlike GradualMover's
	// NumKeys-1. Preserve the extra leg into UE1's zero-valued sentinel slot.
	return NumAuthoredKeys();
}

bool AWoTMixMover::IsGradualState() const
{
	return bExternalMove || Super::IsGradualState();
}

bool AWoTMixMover::ShouldInitializeTagEventAtBeginPlay() const
{
	// MixMover sets Tags[0]/Events[0] only from the Begin labels of its two
	// Gradual states; its global BeginPlay preserves the actor's authored Tag/Event.
	return IsGradualMode(EWoTMoverState::GradualTriggerOpenTimed) ||
		IsGradualMode(EWoTMoverState::GradualTriggerPound);
}

void AWoTMixMover::MoveKeyframe(int32 NewKeyNum)
{
	if (!HasAuthority() || IsMoverFSMInert() || !bMoveKey ||
		NumAuthoredKeys() < 1)
	{
		return;
	}
	MixDestinationKeyNum = FMath::Clamp(NewKeyNum, 0, NumAuthoredKeys() - 1);
	if (MixDestinationKeyNum == CurrentKeyNum)
	{
		return;
	}
	MixMoveDirection = MixDestinationKeyNum > CurrentKeyNum ? 1 : -1;
	bMoveKey = false;
	bExternalMove = true;
	StartExternalLeg();
}

void AWoTMixMover::StartExternalLeg()
{
	LastKeyNum = CurrentKeyNum;
	TargetKeyNum = CurrentKeyNum + MixMoveDirection;
	EnterGradualPhase(MixMoveDirection > 0 ? EWoTGradualPhase::Opening
		: EWoTGradualPhase::Closing);
}

void AWoTMixMover::FinishExternalLeg()
{
	CurrentKeyNum = TargetKeyNum;
	if (MixMoveDirection < 0)
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			EventName, this, TriggerInstigator.Get());
	}
	if (CurrentKeyNum != MixDestinationKeyNum)
	{
		StartExternalLeg();
		return;
	}
	EnterGradualPhase(EWoTGradualPhase::Idle);
	bExternalMove = false;
	bMoveKey = true;
}

void AWoTMixMover::FinishOpeningLeg()
{
	if (bExternalMove)
	{
		FinishExternalLeg();
	}
	else
	{
		Super::FinishOpeningLeg();
	}
}

void AWoTMixMover::FinishClosingLeg()
{
	if (bExternalMove)
	{
		FinishExternalLeg();
	}
	else
	{
		Super::FinishClosingLeg();
	}
}

void AWoTMixMover::ApplyGradualLeg(float Alpha, bool bSweep)
{
	const int32 SentinelKey = NumAuthoredKeys();
	if (CurrentKeyNum != SentinelKey && TargetKeyNum != SentinelKey)
	{
		Super::ApplyGradualLeg(Alpha, bSweep);
		return;
	}

	// MixMover.uc intentionally reads fixed-array slot KeyPos[NumKeys] on its
	// full-open sentinel leg. UE1's unauthored slot is a zero offset, so the
	// target pose is the mover's base pose rather than the clamped last key.
	Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	auto PositionAt = [this, SentinelKey](int32 Key)
	{
		return Key == SentinelKey || !KeyPos.IsValidIndex(Key)
			? FVector::ZeroVector : KeyPos[Key];
	};
	auto RotationAt = [this, SentinelKey](int32 Key)
	{
		return Key == SentinelKey || !KeyRot.IsValidIndex(Key)
			? FRotator::ZeroRotator : KeyRot[Key];
	};
	const FVector Offset = FMath::Lerp(
		PositionAt(CurrentKeyNum), PositionAt(TargetKeyNum), Alpha);
	const FRotator From = RotationAt(CurrentKeyNum);
	const FRotator To = RotationAt(TargetKeyNum);
	const FRotator Rotation(
		FMath::Lerp(From.Pitch, To.Pitch, Alpha),
		FMath::Lerp(From.Yaw, To.Yaw, Alpha),
		FMath::Lerp(From.Roll, To.Roll, Alpha));
	if (ApplyMoverPose(GetBasePosition() + Offset, GetBaseRotation() + Rotation, bSweep))
	{
		LastAppliedAlpha = Alpha;
	}
}

void AWoTMixMover::ApplyGroupReturnLocal()
{
	if (IsGradualState())
	{
		// Deferred: Mix Open/Close label re-entry needs a dedicated replicated reversal.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTMixMover::ApplyGroupStopLocal()
{
	if (IsGradualState())
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTMixMover::ApplyGradualSoundPresentation()
{
	if (bExternalMove)
	{
		ApplyExternalSoundPresentation();
	}
	else
	{
		Super::ApplyGradualSoundPresentation();
	}
}

void AWoTMixMover::ApplyExternalSoundPresentation()
{
	const bool bOldExternal = bPresentedExternalMove;
	const EWoTGradualPhase OldPhase = PresentedMixPhase;
	const int32 OldCurrent = PresentedMixCurrentKeyNum;
	const int32 OldTarget = PresentedMixTargetKeyNum;
	const int32 OldDirection = PresentedMixDirection;
	if (bOldExternal && OldPhase == GradualPhase &&
		OldCurrent == CurrentKeyNum && OldTarget == TargetKeyNum &&
		OldDirection == MixMoveDirection)
	{
		return;
	}
	bPresentedExternalMove = true;
	PresentedMixPhase = GradualPhase;
	PresentedMixCurrentKeyNum = CurrentKeyNum;
	PresentedMixTargetKeyNum = TargetKeyNum;
	PresentedMixDirection = MixMoveDirection;

	auto PresentCrossedFinish = [this](int32 Direction)
	{
		PresentMoverEdge(Direction > 0 ? ClosedSoundPath : OpenedSoundPath);
	};
	if (GradualPhase == EWoTGradualPhase::Opening ||
		GradualPhase == EWoTGradualPhase::Closing)
	{
		if (bOldExternal &&
			(OldPhase == EWoTGradualPhase::Opening ||
			 OldPhase == EWoTGradualPhase::Closing) &&
			OldTarget == CurrentKeyNum)
		{
			PresentCrossedFinish(OldDirection);
		}
		PresentMoverEdge(MixMoveDirection > 0
			? OpeningSoundPath : ClosingSoundPath);
		SetMoverAmbient(MoveAmbientSoundPath, true);
	}
	else if (bOldExternal &&
		(OldPhase == EWoTGradualPhase::Opening ||
		 OldPhase == EWoTGradualPhase::Closing))
	{
		PresentCrossedFinish(OldDirection);
	}
}

void AWoTMixMover::AttachTaggedActors()
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor == this)
		{
			continue;
		}
		AWoTMover* Mover = Cast<AWoTMover>(Actor);
		if (!ActorMatchesWoTTag(Actor, AttachTag))
		{
			continue;
		}
		if (!Mover)
		{
			Actor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		}
		else if (Mover->bSlave)
		{
			Mover->SetMoverFSMInert();
			Mover->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		}
	}
}

void AWoTMixMover::OnRep_MixState()
{
	if (!HasActorBegunPlay())
	{
		return;
	}
	ApplyGradualSoundPresentation();
}

void AWoTMixMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTMixMover, bMoveKey);
	DOREPLIFETIME(AWoTMixMover, bExternalMove);
	DOREPLIFETIME(AWoTMixMover, MixDestinationKeyNum);
	DOREPLIFETIME(AWoTMixMover, MixMoveDirection);
}
