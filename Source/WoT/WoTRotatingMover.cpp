#include "WoTRotatingMover.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTRotatingMover::AWoTRotatingMover()
{
	PrimaryActorTick.bCanEverTick = true;
	// RotatingMover.uc defines no states of its own: unauthored instances get the
	// subclass spin behavior (None keeps the inherited FSM dormant); an authored
	// BASE InitialState (Battle2_02's six author TriggerToggle — keyless, so
	// behavior-neutral) replaces it completely and defers to the base FSM below.
	InitialState = EWoTMoverState::None;
}

void AWoTRotatingMover::BeginPlay()
{
	Super::BeginPlay();
	InitialRotation = GetActorRotation();
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		return;   // base FSM owns this instance — keep Tick enabled for it
	}
	SetActorTickEnabled(bRotating);
	if (!HasAuthority())
	{
		OnRep_RotationState();
	}
}

void AWoTRotatingMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		// UE1: in an authored base state, the STATE's Trigger shadows the class's
		// plain Trigger override — the spin never engages.
		Super::WoTTrigger(Other, EventInstigator);
		return;
	}
	if (!HasAuthority() || bRotating)
	{
		return;
	}
	bRotating = true;
	RotationTime = 0.0f;
	RotationStartServerTime = GetWorld()->GetTimeSeconds();
	SetActorTickEnabled(true);
}

void AWoTRotatingMover::WoTUnTrigger(AActor*, APawn*)
{
	if (IsMoverFSMInert())
	{
		return;
	}
	if (InitialState != EWoTMoverState::None)
	{
		return;   // base Mover states define no UnTrigger handler
	}
	if (!HasAuthority() || !bRotating)
	{
		return;
	}
	AccumulatedRotationTime += RotationTime;
	ApplyRotationAt(AccumulatedRotationTime);
	bRotating = false;
	RotationTime = 0.0f;
	RotationStartServerTime = GetWorld()->GetTimeSeconds();
	SetActorTickEnabled(false);
}

void AWoTRotatingMover::Tick(float DeltaTime)
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
	if (!bRotating)
	{
		return;
	}
	RotationTime += DeltaTime;
	ApplyRotationAt(AccumulatedRotationTime + RotationTime);
}

void AWoTRotatingMover::ApplyRotationAt(float Elapsed)
{
	ApplyMoverPose(GetActorLocation(), InitialRotation + RotateRate * Elapsed,
		HasAuthority());
}

void AWoTRotatingMover::ApplyGroupReturnLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred: native continuous rotation has no alpha-aware retail group verb.
		return;
	}
	Super::ApplyGroupReturnLocal();
}

void AWoTRotatingMover::ApplyGroupStopLocal()
{
	if (InitialState == EWoTMoverState::None)
	{
		// Deferred with ApplyGroupReturnLocal; do not stamp ignored base Phase state.
		return;
	}
	Super::ApplyGroupStopLocal();
}

void AWoTRotatingMover::OnRep_RotationState()
{
	if (!HasActorBegunPlay() || InitialState != EWoTMoverState::None)
	{
		return;   // base-FSM instances replicate through the base Phase notify
	}
	if (bRotating)
	{
		const AGameStateBase* GS = GetWorld()->GetGameState();
		RotationTime = GS
			? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - RotationStartServerTime)
			: 0.0f;
		ApplyRotationAt(AccumulatedRotationTime + RotationTime);
	}
	else
	{
		RotationTime = 0.0f;
		ApplyRotationAt(AccumulatedRotationTime);
	}
	SetActorTickEnabled(bRotating);
}

void AWoTRotatingMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTRotatingMover, bRotating);
	DOREPLIFETIME(AWoTRotatingMover, RotationStartServerTime);
	DOREPLIFETIME(AWoTRotatingMover, AccumulatedRotationTime);
}
