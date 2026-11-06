#include "WoTAngrealInvLevitate.h"
#include "../AI/WoTPathNodeIterator.h"

#include "WoTAngrealInvWhirlwind.h"
#include "../WoTAmbientSound.h"
#include "../WoTSound.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealInvLevitate::AWoTAngrealInvLevitate()
{
	BindStatusIcons(TEXT("I_Levitation"), TEXT("M_Levitation"));
	PrimaryActorTick.bCanEverTick = true;
	SetActorTickEnabled(false);   // only ticks while levitating

	// AngrealInvLevitate defaultproperties
	RiseHeight = 24.0f;
	LevitateSpeed = 96.0f;
	DurationType = EWoTDurationType::DT_Charge;
	bElementAir = true;
	bUncommon = true;
	MinInitialCharges = 10;
	MaxInitialCharges = 30;
	MaxCharges = 50;
	bDisplayIcon = true;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 10;
	MaxChargeUsedInterval = 1.0f;
	// LoopLT is the ONLY active presentation while levitating. The shipped class
	// has no light and spawns no visual actor (LevitateLeech is dead alternate code).
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopLT.LoopLT"));
	DeactivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/DeActivateLT.DeActivateLT"));
}

void AWoTAngrealInvLevitate::CastArtifact()
{
	ACharacter* Char = Cast<ACharacter>(GetOwner());
	if (!Char)
	{
		return;
	}

	// Can't levitate while being whirlwinded (AngrealInvWhirlwind gate).
	if (AWoTAngrealInvWhirlwind::PlayerIsBeingWhirlwinded(Char))
	{
		Failed();
		return;
	}

	if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
	{
		InitialMovementMode = Move->MovementMode;
		InitialCustomMovementMode = Move->CustomMovementMode;
	}
	bLevitating = true;
	SetActorTickEnabled(true);
	ReconcileLevitatingLoop();
	ForceNetUpdate();
	// AngrealInvLevitate.uc:46-48 engages levitation before Super.Cast. Keep the
	// retail LoopLT one-shot on top of the ambient loop and add the good HUD icon.
	Super::CastArtifact();
}

void AWoTAngrealInvLevitate::UnCastArtifact()
{
	if (!bLevitating)
	{
		return;
	}
	bLevitating = false;
	SetActorTickEnabled(false);
	ReconcileLevitatingLoop();
	ForceNetUpdate();

	if (ACharacter* Char = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->SetMovementMode(InitialMovementMode, InitialCustomMovementMode);
		}
	}

	Super::UnCastArtifact();
}

void AWoTAngrealInvLevitate::GoEmpty()
{
	UnCastArtifact();
	Super::GoEmpty();
}

void AWoTAngrealInvLevitate::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bLevitating)
	{
		return;
	}

	ACharacter* Char = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* Move = Char ? Char->GetCharacterMovement() : nullptr;
	if (!Move)
	{
		return;
	}

	// Float, don't fly: no gravity, hold RiseHeight above the ground below.
	Move->SetMovementMode(MOVE_Flying);

	const FVector Loc = Char->GetActorLocation();
	FHitResult Hit;
	// AngrealInvLevitate.uc:89 TraceRecursive(..., false). The shared helper
	// preserves the ordered retail result with one query to the UE1 world edge.
	if (WoTVisBlockingTraceRecursive(GetWorld(), Loc, FVector(0, 0, -1),
		0.0f, this, Hit))
	{
		const float HalfHeight = Char->GetCapsuleComponent()
			? Char->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
			: 0.0f;
		const float DesiredZ = Hit.ImpactPoint.Z + RiseHeight + HalfHeight;
		const float DeltaZ = DesiredZ - Loc.Z;
		// Move toward the hover height at LevitateSpeed (UE1 Velocity.Z set), with the
		// original's 2uu deadband that damps vibration (AngrealInvLevitate.uc:92 —
		// "To alleviate vibrations"; LevitateLeech's 8uu is dead code, zero references).
		Move->Velocity.Z = (FMath::Abs(DeltaZ) > 2.0f)
			? FMath::Sign(DeltaZ) * LevitateSpeed
			: 0.0f;
	}

	// Drain charges at RoundsPerMinute (UseCharge auto-UnCasts + GoEmpty at zero).
	ChargeTimer -= DeltaTime;
	while (bLevitating && ChargeTimer < 0.0f && RoundsPerMinute > 0.0f)
	{
		ChargeTimer += (60.0f / RoundsPerMinute);
		UseCharge();
	}
}

void AWoTAngrealInvLevitate::OnRep_Levitating()
{
	ReconcileLevitatingLoop();
}

void AWoTAngrealInvLevitate::OnRep_Owner()
{
	Super::OnRep_Owner();
	// Ownership can leave a client before the owner-only false state arrives.
	ReconcileLevitatingLoop();
}

void AWoTAngrealInvLevitate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bLevitating = false;
	ReconcileLevitatingLoop();
	Super::EndPlay(EndPlayReason);
}

void AWoTAngrealInvLevitate::ReconcileLevitatingLoop()
{
	ACharacter* Char = Cast<ACharacter>(GetOwner());
	const ENetMode Mode = GetNetMode();
	// AngrealInvLevitate.uc:126 routes LoopLT through the owner. Standalone
	// retains its original local behavior; network play presents it only on
	// the locally controlled owner's machine, including a listen-server host.
	const bool bShouldPlay = bLevitating && IsValid(Char)
		&& Mode != NM_DedicatedServer
		&& (Mode == NM_Standalone || Char->IsLocallyControlled());
	if (bShouldPlay && !LevitateLoop)
	{
		if (USoundBase* Loop = WoTSound::Resolve(ActivateSound, true))
		{
			LevitateLoop = NewObject<UWoTAmbientSoundComponent>(Char,
				TEXT("LevitateLoop"));
			if (LevitateLoop)
			{
				LevitateLoop->SetupAttachment(Char->GetRootComponent());
				LevitateLoop->RegisterComponent();
				LevitateLoop->ConfigureUE1Ambient(Loop, 32, 128, 64);
				LevitateLoop->Play();
			}
		}
	}
	else if (!bShouldPlay && LevitateLoop)
	{
		LevitateLoop->Stop();
		LevitateLoop->DestroyComponent();
		LevitateLoop = nullptr;
	}
}

void AWoTAngrealInvLevitate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AWoTAngrealInvLevitate, bLevitating, COND_OwnerOnly);
}
