#include "WoTMover.h"

#include "Inventory/WoTSeal.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Projectiles/WoTProjectile.h"
#include "WoTAmbientSound.h"
#include "WoTCarcass.h"
#include "WoTCharacter.h"
#include "WoTDecoration.h"
#include "WoTFragment.h"
#include "WoTMeshLight.h"
#include "WoTPlayerController.h"
#include "WoTSound.h"

#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

AWoTMover::AWoTMover()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	MeshLight = CreateDefaultSubobject<UWoTMeshLight>(TEXT("MeshLight"));
	MoveAmbient = CreateDefaultSubobject<UWoTAmbientSoundComponent>(TEXT("MoveAmbient"));
	MoveAmbient->SetupAttachment(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	// BlockAllDynamic, not BlockAll: same blocking responses, but the
	// WorldDynamic OBJECT TYPE keeps movers out of every ECC_WorldStatic
	// object-type gate — retail trap placement accepts Level BSP only
	// (`Other == Level`, AWoTTrap::CalcLocation), so a portcullis face must
	// not qualify as a trap floor. (C01 census caught 18 mover components
	// shipping as WorldStatic objects.)
	Mesh->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	Mesh->SetNotifyRigidBodyCollision(true);
	bReplicates = true;
	// Client-simulation model (UE1 Mover defaults): clients interpolate locally from the replicated
	// Phase, so the transform is NOT streamed (bReplicateMovement OFF). Parity net props restored.
	NetPriority = 2.7f;
	bAlwaysRelevant = true;
}

void AWoTMover::BeginPlay()
{
	Super::BeginPlay();
	if (!bHasConfiguredBasePose)
	{
		ConfigureInitialRestPose(GetActorLocation(), GetActorRotation());
	}
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	// Dynamic-light movers carry converter-authored planar surface rows. Arm the
	// existing M2 leaf/movement timer so their origin leaf and oriented planes
	// are reselected during keyframe motion. A missing table preserves the
	// import-time base-pose bake.
	if (IsValid(MeshLight) && (!MeshLight->MoverSurfaces.IsEmpty()
		|| MeshLight->bStaticMoverSurfaceLighting))
	{
		if (UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(GetWorld()))
		{
			MeshLight->StartRuntimeBake(Table);
		}
	}
	if (HasAuthority())
	{
		ApplyKeyframeLerp(RestingKeyNum, RestingKeyNum, 0.0f);
		ResolveGroupAndSlaves();
	}
	else
	{
		// A proxy's initial bunch can be processed before BeginPlay (OnRep_Phase early-outs
		// then); now that the base pose is cached, seed/apply whatever Phase already arrived.
		if (Phase == EWoTMoverPhase::Closed && PhaseStartServerTime == 0.0f)
		{
			ApplyKeyframeLerp(RestingKeyNum, RestingKeyNum, 0.0f, false);
		}
		else
		{
			OnRep_Phase();
		}
	}
}

float AWoTMover::EvaluateGlideAlpha(EWoTMoverGlideType GlideType, float Alpha)
{
	Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	if (GlideType == EWoTMoverGlideType::GlideByTime)
	{
		// UE1's execPollFinishInterpolation: 3a^2 - 2a^3.
		return 3.0f * Alpha * Alpha - 2.0f * Alpha * Alpha * Alpha;
	}
	return Alpha;
}

void AWoTMover::ConfigureInitialRestPose(const FVector& InBaseLoc,
	const FRotator& InBaseRot)
{
	BaseLoc = InBaseLoc;
	BaseRot = InBaseRot;
	bHasConfiguredBasePose = true;
	RestingKeyNum = FMath::Clamp(InitialKeyNum, 0, 7);
	const int32 NumKeys = FMath::Max(KeyPos.Num(), KeyRot.Num());
	RestingAlpha = NumKeys > 1
		? FMath::Clamp(static_cast<float>(RestingKeyNum)
			/ static_cast<float>(NumKeys - 1), 0.0f, 1.0f)
		: 0.0f;
	PhaseStartAlpha = RestingAlpha;
	if (!ApplyKeyframeLerp(RestingKeyNum, RestingKeyNum, 0.0f, false))
	{
		SetActorLocationAndRotation(BaseLoc, BaseRot, false);
	}
	LastAppliedAlpha = RestingAlpha;
}

void AWoTMover::ApplyAlpha(float Alpha, bool bSweep)
{
	const int32 NumKeys = FMath::Max(KeyPos.Num(), KeyRot.Num());
	if (NumKeys < 2)
	{
		return;
	}
	// scale alpha across the key chain (most WoT movers have 2 keys)
	const float F = Alpha * (NumKeys - 1);
	const int32 A = FMath::Clamp(FMath::FloorToInt(F), 0, NumKeys - 2);
	const float T = F - A;
	if (ApplyKeyframeLerp(A, A + 1, T, bSweep))
	{
		LastAppliedAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	}
}

bool AWoTMover::ApplyKeyframeLerp(int32 FromKey, int32 ToKey, float Alpha, bool bSweep)
{
	const int32 NumKeys = FMath::Max(KeyPos.Num(), KeyRot.Num());
	if (NumKeys < 1)
	{
		return false;
	}
	FromKey = FMath::Clamp(FromKey, 0, NumKeys - 1);
	ToKey = FMath::Clamp(ToKey, 0, NumKeys - 1);
	Alpha = EvaluateGlideAlpha(MoverGlideType, Alpha);
	auto Key = [&](const TArray<FVector>& Arr, int32 I)
	{
		return Arr.IsValidIndex(I) ? Arr[I] : FVector::ZeroVector;
	};
	auto KeyR = [&](int32 I)
	{
		return KeyRot.IsValidIndex(I) ? KeyRot[I] : FRotator::ZeroRotator;
	};
	const FVector P = FMath::Lerp(Key(KeyPos, FromKey), Key(KeyPos, ToKey), Alpha);
	// component-wise rotator lerp: UE1 interpolated rotator units linearly
	const FRotator RA = KeyR(FromKey), RB = KeyR(ToKey);
	const FRotator R(FMath::Lerp(RA.Pitch, RB.Pitch, Alpha),
	                 FMath::Lerp(RA.Yaw, RB.Yaw, Alpha),
	                 FMath::Lerp(RA.Roll, RB.Roll, Alpha));
	// Authority sweeps (the mover pushes/encroaches pawns); a client proxy moves the brush
	// visually only (bSweep=false) so it never does authoritative encroach.
	return ApplyMoverPose(BaseLoc + P, BaseRot + R, bSweep);
}

bool AWoTMover::ApplyMoverPose(const FVector& TargetLocation,
	const FRotator& TargetRotation, bool bSweep)
{
	if (!HasAuthority() || !bSweep || bFSMInert)
	{
		SetActorLocationAndRotation(TargetLocation, TargetRotation, false);
		return true;
	}
	const FVector PreviousLocation = GetActorLocation();
	const FRotator PreviousRotation = GetActorRotation();

	const bool bActuallyMoving =
		!GetActorLocation().Equals(TargetLocation, UE_KINDA_SMALL_NUMBER) ||
		!GetActorRotation().Equals(TargetRotation, UE_KINDA_SMALL_NUMBER);
	if (!bActuallyMoving)
	{
		SetActorLocationAndRotation(TargetLocation, TargetRotation, false);
		return true;
	}

	FHitResult SweepHit;
	SetActorLocationAndRotation(TargetLocation, TargetRotation, true, &SweepHit,
		ETeleportType::None);

	TArray<AActor*> EncroachedActors;
	if (SweepHit.bBlockingHit && IsValid(SweepHit.GetActor()))
	{
		EncroachedActors.Add(SweepHit.GetActor());
	}

	// UE5 sweeps translation but applies rotation without a rotational sweep.
	// Query the mover shape at the achieved post-sweep pose so turning brushes and continuous
	// RotatingMovers still deliver Mover.EncroachingOn. Relevant debris/pickups
	// are retained too; they use the same retail clearing event when overlapping.
	if (IsValid(Mesh) && Mesh->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
	{
		TArray<FOverlapResult> Overlaps;
		FComponentQueryParams Params(
			SCENE_QUERY_STAT(WoTMoverEncroach), this);
		// Query with the component's actual collision geometry. Primitive collision
		// shapes cannot represent complex-as-simple mover meshes.
		if (Mesh->ComponentOverlapMulti(
			Overlaps, GetWorld(), GetActorLocation(), GetActorQuat(),
			Mesh->GetCollisionObjectType(), Params))
		{
			for (const FOverlapResult& Overlap : Overlaps)
			{
				AActor* Candidate = Overlap.GetActor();
				if (IsValid(Candidate) && Candidate != this &&
					(Cast<APawn>(Candidate) || Cast<AWoTCarcass>(Candidate) ||
					 Cast<AWoTDecoration>(Candidate) || Cast<AWoTFragment>(Candidate) ||
					 Cast<AWoTInventory>(Candidate)))
				{
					EncroachedActors.AddUnique(Candidate);
				}
			}
		}
	}

	bool bAbortMove = false;
	for (AActor* Encroached : EncroachedActors)
	{
		if (IsValid(Encroached) && EncroachingOn(Encroached))
		{
			bAbortMove = true;
			break;
		}
	}
	if (!bAbortMove && !EncroachedActors.IsEmpty())
	{
		// UE1 MoveActor retries into the occupied pose when EncroachingOn returns
		// false. One bounded unswept placement is the direct UE5 equivalent.
		SetActorLocationAndRotation(TargetLocation, TargetRotation, false);
	}
	else if (bAbortMove)
	{
		// The group verb has already changed the active FSM. Retail leaves the
		// mover at its last completed interpolation pose, not the partial sweep.
		SetActorLocationAndRotation(PreviousLocation, PreviousRotation, false);
		return false;
	}
	return true;
}

bool AWoTMover::EncroachingOn(AActor* Other)
{
	if (!HasAuthority() || !IsValid(Other))
	{
		return false;
	}

	FWoTDamageEvent Crushed;
	Crushed.WoTDamageType = TEXT("Crushed");
	AController* EventInstigator = Inst.IsValid() ? Inst->GetController() : nullptr;
	if (Cast<AWoTCarcass>(Other) || Cast<AWoTDecoration>(Other))
	{
		Other->TakeDamage(10000.0f, Crushed, EventInstigator, this);
		return false;
	}
	if (Cast<AWoTFragment>(Other))
	{
		Other->Destroy();
		return false;
	}
	if (AWoTInventory* Inventory = Cast<AWoTInventory>(Other))
	{
		if (!Cast<AWoTSeal>(Inventory) &&
			Inventory->GetOwner() == nullptr)
		{
			Inventory->Destroy();
			return false;
		}
	}

	if (EncroachDamage != 0)
	{
		Other->TakeDamage(static_cast<float>(EncroachDamage), Crushed,
			EventInstigator, this);
	}

	switch (EncroachType)
	{
	case EWoTMoverEncroachType::StopWhenEncroach:
		RewindEncroachClockToLastAppliedPose();
		if (AWoTMover* Leader = GroupLeader.IsValid() ? GroupLeader.Get() : this)
		{
			Leader->MakeGroupStop();
		}
		return true;
	case EWoTMoverEncroachType::ReturnWhenEncroach:
		RewindEncroachClockToLastAppliedPose();
		if (AWoTMover* Leader = GroupLeader.IsValid() ? GroupLeader.Get() : this)
		{
			Leader->MakeGroupReturn();
		}
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(Other))
		{
			Character->PlayTakeHitVoice(0.0f, NAME_None);
		}
		return true;
	case EWoTMoverEncroachType::CrushWhenEncroach:
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(Other))
		{
			AController* VictimController = Character->GetController();
			if (IsValid(VictimController) && VictimController->IsPlayerController())
			{
				FWoTDamageEvent Suicided;
				Suicided.WoTDamageType = TEXT("Suicided");
				Character->TakeDamage(static_cast<float>(Character->Health + 30),
					Suicided, EventInstigator, this);
			}
		}
		return false;
	case EWoTMoverEncroachType::IgnoreWhenEncroach:
	default:
		return false;
	}
}

void AWoTMover::RewindEncroachClockToLastAppliedPose()
{
	if (Phase == EWoTMoverPhase::Opening)
	{
		const float Span = 1.0f - PhaseStartAlpha;
		const float Fraction = Span > UE_SMALL_NUMBER
			? (LastAppliedAlpha - PhaseStartAlpha) / Span : 1.0f;
		PhaseTime = FMath::Clamp(Fraction, 0.0f, 1.0f) * OpenDuration();
	}
	else if (Phase == EWoTMoverPhase::Closing)
	{
		const float Fraction = PhaseStartAlpha > UE_SMALL_NUMBER
			? (PhaseStartAlpha - LastAppliedAlpha) / PhaseStartAlpha : 1.0f;
		PhaseTime = FMath::Clamp(Fraction, 0.0f, 1.0f) * CloseDuration();
	}
}

void AWoTMover::ResolveGroupAndSlaves()
{
	if (!HasAuthority())
	{
		return;
	}
	if (ReturnGroup.IsNone())
	{
		ReturnGroup = TagName;
	}
	if (!GroupLeader.IsValid())
	{
		GroupLeader = this;
		for (TActorIterator<AWoTMover> It(GetWorld()); It; ++It)
		{
			AWoTMover* Candidate = *It;
			if (!IsValid(Candidate) || Candidate == this)
			{
				continue;
			}
			if (Candidate->ReturnGroup.IsNone())
			{
				Candidate->ReturnGroup = Candidate->TagName;
			}
			if (Candidate->ReturnGroup == ReturnGroup)
			{
				Candidate->GroupLeader = this;
				Candidate->GroupFollower = GroupFollower;
				GroupFollower = Candidate;
			}
		}
	}

	if (!bSlave)
	{
		for (TActorIterator<AWoTMover> It(GetWorld()); It; ++It)
		{
			AWoTMover* Candidate = *It;
			if (IsValid(Candidate) && Candidate != this && Candidate->bSlave &&
				Candidate->TagName == TagName)
			{
				AttachSlave(Candidate);
			}
		}
	}
}

void AWoTMover::AttachSlave(AWoTMover* Slave)
{
	if (!HasAuthority() || !IsValid(Slave) || Slave == this)
	{
		return;
	}
	Slave->SetMoverFSMInert();
	Slave->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
}

bool AWoTMover::ActorMatchesWoTTag(const AActor* Actor, FName WantedTag)
{
	if (!IsValid(Actor) || WantedTag.IsNone())
	{
		return false;
	}
	if (const AWoTMover* Mover = Cast<AWoTMover>(Actor))
	{
		return Mover->TagName == WantedTag;
	}
	if (Actor->ActorHasTag(WantedTag))
	{
		return true;
	}
	// Event-graph actors store the shipped singular Actor.Tag in TagName rather
	// than AActor::Tags. Reflection keeps AttachMover generic across those native
	// classes without hard-coding every triggerable subclass.
	if (const FNameProperty* Property = FindFProperty<FNameProperty>(
		Actor->GetClass(), TEXT("TagName")))
	{
		return *Property->ContainerPtrToValuePtr<FName>(Actor) == WantedTag;
	}
	return false;
}

void AWoTMover::SetMoverFSMInert()
{
	if (!HasAuthority())
	{
		return;
	}
	bFSMInert = true;
	bNoBump = true;
	bSpent = true;
	Phase = EWoTMoverPhase::Closed;
	PhaseTime = 0.0f;
	PhaseStartAlpha = RestingAlpha;
	StoppedAlpha = 0.0f;
	StopMoverAmbient();
	SetActorTickEnabled(false);
}

float AWoTMover::CurrentMoverAlpha() const
{
	switch (Phase)
	{
	case EWoTMoverPhase::Opening:
		return FMath::Lerp(PhaseStartAlpha, 1.0f,
			FMath::Clamp(PhaseTime /
				FMath::Max(OpenDuration(), UE_SMALL_NUMBER), 0.0f, 1.0f));
	case EWoTMoverPhase::Open:
		return 1.0f;
	case EWoTMoverPhase::Closing:
		return FMath::Lerp(PhaseStartAlpha, 0.0f,
			FMath::Clamp(PhaseTime /
				FMath::Max(CloseDuration(), UE_SMALL_NUMBER), 0.0f, 1.0f));
	case EWoTMoverPhase::Stopped:
		return StoppedAlpha;
	default:
		return RestingAlpha;
	}
}

void AWoTMover::ReversePhase()
{
	if (!HasAuthority())
	{
		return;
	}
	if (Phase == EWoTMoverPhase::Opening)
	{
		const float CurrentAlpha = CurrentMoverAlpha();
		PhaseStartAlpha = 1.0f;
		SetPhaseAuthority(EWoTMoverPhase::Closing,
			CloseDuration() * (1.0f - CurrentAlpha));
	}
	else if (Phase == EWoTMoverPhase::Closing)
	{
		const float CurrentAlpha = CurrentMoverAlpha();
		PhaseStartAlpha = 0.0f;
		SetPhaseAuthority(EWoTMoverPhase::Opening,
			OpenDuration() * CurrentAlpha);
	}
}

void AWoTMover::MakeGroupReturn()
{
	if (!HasAuthority())
	{
		return;
	}
	TSet<AWoTMover*> Visited;
	AWoTMover* Current = this;
	for (int32 Guard = 0; IsValid(Current) && Guard < 1024; ++Guard)
	{
		if (Visited.Contains(Current))
		{
			break;
		}
		Visited.Add(Current);
		AWoTMover* Next = Current->GroupFollower.Get();
		Current->ApplyGroupReturnLocal();
		Current = Next;
	}
}

void AWoTMover::MakeGroupStop()
{
	if (!HasAuthority())
	{
		return;
	}
	TSet<AWoTMover*> Visited;
	AWoTMover* Current = this;
	for (int32 Guard = 0; IsValid(Current) && Guard < 1024; ++Guard)
	{
		if (Visited.Contains(Current))
		{
			break;
		}
		Visited.Add(Current);
		AWoTMover* Next = Current->GroupFollower.Get();
		Current->ApplyGroupStopLocal();
		Current = Next;
	}
}

void AWoTMover::ApplyGroupReturnLocal()
{
	if (Phase == EWoTMoverPhase::Opening || Phase == EWoTMoverPhase::Closing)
	{
		ReversePhase();
	}
	else if (Phase == EWoTMoverPhase::Open)
	{
		StartClose();
	}
	// Closed/Delaying/Stopped intentionally no-op. Encroachment only dispatches
	// while moving, so retail's instant close edge at key zero is unreachable.
}

void AWoTMover::ApplyGroupStopLocal()
{
	StoppedAlpha = CurrentMoverAlpha();
	SetPhaseAuthority(EWoTMoverPhase::Stopped);
}

void AWoTMover::StartOpen(APawn* EventInstigator)
{
	if (bFSMInert || Phase != EWoTMoverPhase::Closed || bSpent)
	{
		return;
	}
	Inst = EventInstigator;
	PhaseStartAlpha = RestingAlpha;
	SetPhaseAuthority(DelayTime > 0 ? EWoTMoverPhase::Delaying
		: EWoTMoverPhase::Opening);
}

void AWoTMover::StartClose()
{
	if (bFSMInert)
	{
		return;
	}
	PhaseStartAlpha = CurrentMoverAlpha();
	SetPhaseAuthority(EWoTMoverPhase::Closing);
}

void AWoTMover::SetPhaseAuthority(EWoTMoverPhase NewPhase, float StartOffset)
{
	// Every authority transition stamps when the phase began (server world seconds,
	// backdated by a mid-move reversal's already-elapsed time) so proxies seed their
	// local clock from it — the port of Mover.InterpolateTo replicating PhysAlpha
	// to clients (SimInterpolate) so a reversing mover resumes from its current alpha.
	Phase = NewPhase;
	PhaseTime = StartOffset;
	PhaseStartServerTime = GetWorld()->GetTimeSeconds() - StartOffset;
	ApplyPhaseSoundPresentation();
}

void AWoTMover::ApplyPhaseSoundPresentation()
{
	// Local presentation off the replicated Phase — the authority transition calls
	// this directly (listen servers hear), clients via OnRep. Dedicated servers have
	// no listeners.
	if (Phase == SoundPhase)
	{
		return;
	}
	SoundPhase = Phase;

	FName EdgeSound = NAME_None;
	switch (Phase)
	{
	case EWoTMoverPhase::Opening: EdgeSound = OpeningSoundPath; break;   // DoOpen
	case EWoTMoverPhase::Open:    EdgeSound = OpenedSoundPath;  break;   // FinishedOpening
	case EWoTMoverPhase::Closing: EdgeSound = ClosingSoundPath; break;   // DoClose
	case EWoTMoverPhase::Closed:  EdgeSound = ClosedSoundPath;  break;   // FinishedClosing
	default: break;                                                      // Delaying: none
	}
	// The pre-BeginPlay initial replication only syncs the loop — a UE1 late joiner
	// never heard an edge it missed. The subclass gate covers retail-quiet phase
	// transitions (the portcullis raise snap).
	if (HasActorBegunPlay() && ShouldPlayPhaseEdgeSound(Phase))
	{
		PresentMoverEdge(EdgeSound);
	}

	const bool bWantLoop = (Phase == EWoTMoverPhase::Opening ||
	                        Phase == EWoTMoverPhase::Closing) &&
	                       !MoveAmbientSoundPath.IsNone();
	SetMoverAmbient(MoveAmbientSoundPath, bWantLoop);
}

void AWoTMover::PresentMoverEdge(FName SoundPath)
{
	if (GetNetMode() == NM_DedicatedServer || SoundPath.IsNone() ||
		!HasActorBegunPlay())
	{
		return;
	}
	LastPhaseSound = SoundPath;
	++PhaseSoundPlayCount;
	WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(SoundPath),
		GetActorLocation(), 1600.0f);
}

void AWoTMover::SetMoverAmbient(FName SoundPath, bool bEnable)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const bool bWantLoop = bEnable && !SoundPath.IsNone();
	if (!bWantLoop)
	{
		if (IsValid(MoveAmbient))
		{
			MoveAmbient->Stop();
		}
		bMoveLoopOn = false;
		ActiveMoveLoopPath = NAME_None;
		return;
	}
	if (bMoveLoopOn && ActiveMoveLoopPath == SoundPath)
	{
		return;
	}
	if (!IsValid(MoveAmbient))
	{
		return;
	}
	MoveAmbient->Stop();
	MoveAmbient->SetSound(nullptr);
	if (USoundBase* Wave = WoTSound::LoadBySubPath(SoundPath, true))
	{
		MoveAmbient->ConfigureUE1Ambient(Wave, SoundRadius, SoundVolume, SoundPitch);
		MoveAmbient->Play();
	}
	// The observable tracks the intended UE1 AmbientSound assignment even when
	// stripped/headless content has no loadable wave.
	bMoveLoopOn = true;
	ActiveMoveLoopPath = SoundPath;
}

void AWoTMover::NotifyHit(UPrimitiveComponent*, AActor* Other,
                          UPrimitiveComponent*, bool, FVector, FVector,
                          FVector, const FHitResult&)
{
	if (!HasAuthority() || bFSMInert || bNoBump)
	{
		return;
	}
	APawn* P = Cast<APawn>(Other);
	float PawnMass = 100.0f;
	if (const ACharacter* Character = Cast<ACharacter>(P))
	{
		if (const UCharacterMovementComponent* Movement =
			Character->GetCharacterMovement())
		{
			PawnMass = Movement->Mass;
		}
	}
	const bool bAcceptedBump = BumpType == EWoTMoverBumpType::AnyBump
		? IsValid(Other)
		: BumpType == EWoTMoverBumpType::PawnBump
			? P != nullptr && PawnMass >= 10.0f
			: P != nullptr && P->IsPlayerControlled();
	if (bAcceptedBump && (InitialState == EWoTMoverState::BumpOpenTimed ||
		InitialState == EWoTMoverState::BumpButton))
	{
		if (bLockable && InitialState == EWoTMoverState::BumpOpenTimed &&
			!bUnlocked)
		{
			if (!CheckForKey(P))
			{
				PresentLocked(P);
				return;
			}
			if (bStayUnlocked)
			{
				bUnlocked = true;
			}
			PresentUnlock(P);
			if (UnLockDuration > 0.0f)
			{
				UnlockingPawn = P;
				bNoBump = true;
				GetWorld()->GetTimerManager().SetTimer(
					UnlockTimer, this, &AWoTMover::FinishUnlock,
					UnLockDuration, false);
				return;
			}
		}
		StartOpen(P);
	}
}

void AWoTMover::FinishUnlock()
{
	bNoBump = false;
	APawn* Pawn = UnlockingPawn.Get();
	UnlockingPawn.Reset();
	if (HasAuthority() && IsValid(Pawn))
	{
		StartOpen(Pawn);
	}
}

void AWoTMover::SendHandMessage(APawn* Pawn, const FString& Message,
	float Duration) const
{
	if (Message.IsEmpty() || !IsValid(Pawn))
	{
		return;
	}
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Pawn->GetController()))
	{
		PC->ClientHandMessage(Message, Duration);
	}
}

void AWoTMover::PresentLocked(APawn* Pawn)
{
	if (!HasAuthority())
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now < LastLockedSoundTime + MinLockSoundTime)
	{
		return;
	}
	SendHandMessage(Pawn, LockedMessage, LockedMessageDuration);
	TriggerLockPresentation(0);
	LastLockedSoundTime = Now;
}

void AWoTMover::PresentUnlock(APawn* Pawn)
{
	if (!HasAuthority())
	{
		return;
	}
	SendHandMessage(Pawn, UnLockMessage, UnLockMessageDuration);
	TriggerLockPresentation(1);
}

void AWoTMover::TriggerLockPresentation(uint8 Kind)
{
	LockPresentationKind = Kind;
	++LockPresentationCounter;
	ApplyLockPresentation();
	ForceNetUpdate();
}

void AWoTMover::ApplyLockPresentation()
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const FName SoundPath = LockPresentationKind == 0
		? LockedSoundPath : UnLockSoundPath;
	if (SoundPath.IsNone())
	{
		return;
	}
	LastLockSound = SoundPath;
	WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(SoundPath),
		GetActorLocation(), 1600.0f);
}

void AWoTMover::OnRep_LockPresentation()
{
	ApplyLockPresentation();
}

bool AWoTMover::CheckForKey(APawn* P) const
{
	if (!P || TagName.IsNone())
	{
		return false;
	}
	// The key is any inventory item whose Event matches this mover's Tag.
	if (const UWoTInventoryComponent* Inv = P->FindComponentByClass<UWoTInventoryComponent>())
	{
		for (const TObjectPtr<AWoTInventory>& Item : Inv->Items)
		{
			if (IsValid(Item) && Item->EventName == TagName)
			{
				return true;
			}
		}
	}
	return false;
}

void AWoTMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!HasAuthority() || bFSMInert)
	{
		return;
	}
	switch (InitialState)
	{
	case EWoTMoverState::TriggerToggle:
		// Mover.TriggerToggle: at/heading-to key0 → open, else close. InterpolateTo
		// reverses smoothly ONLY mid-move (PhysAlpha = 1 - PhysAlpha); from rest it
		// always starts at alpha 0. The reversal offset resumes at the SAME alpha,
		// scaled into the new direction's duration (identical to the old
		// MoveTime - PhaseTime when both legs run on MoveTime).
		if (Phase == EWoTMoverPhase::Closing)
		{
			ReversePhase();
		}
		else if (Phase == EWoTMoverPhase::Opening)
		{
			ReversePhase();
		}
		else if (Phase == EWoTMoverPhase::Closed)
		{
			if (RestingKeyNum == 0)
			{
				StartOpen(EventInstigator);
			}
			else
			{
				StartClose();
			}
		}
		else if (Phase == EWoTMoverPhase::Open)
		{
			StartClose();
		}
		break;
	case EWoTMoverState::TriggerOpenTimed:
	case EWoTMoverState::TriggerControl:
	case EWoTMoverState::TriggerPound:
	case EWoTMoverState::StandOpenTimed:
		StartOpen(EventInstigator);
		break;
	default:
		break;
	}
}

void AWoTMover::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bFSMInert)
	{
		return;
	}
	if (!HasAuthority())
	{
		SimulateClientInterp(DeltaTime);   // a proxy re-runs the keyframe motion locally
		return;
	}
	if (Phase == EWoTMoverPhase::Closed || Phase == EWoTMoverPhase::Stopped)
	{
		return;
	}
	PhaseTime += DeltaTime;
	switch (Phase)
	{
	case EWoTMoverPhase::Delaying:
		if (PhaseTime >= DelayTime)
		{
			PhaseStartAlpha = RestingAlpha;
			SetPhaseAuthority(EWoTMoverPhase::Opening);
		}
		break;
	case EWoTMoverPhase::Opening:
	{
		const float OpenDur = FMath::Max(OpenDuration(), UE_SMALL_NUMBER);
		const EWoTMoverPhase AppliedPhase = Phase;
		ApplyAlpha(CurrentMoverAlpha());
		if (Phase != AppliedPhase)
		{
			break;
		}
		if (PhaseTime >= OpenDur)
		{
			RestingKeyNum = FMath::Max(0,
				FMath::Max(KeyPos.Num(), KeyRot.Num()) - 1);
			RestingAlpha = 1.0f;
			PhaseStartAlpha = 1.0f;
			SetPhaseAuthority(EWoTMoverPhase::Open);
			// UE1 FinishedOpening fires the mover's own Event
			GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
				EventName, this, Inst.Get());
			// UE1 Mover: every timed/button state goes dormant on bTriggerOnceOnly, but the
			// TriggerToggle state has NO once-only handling — a toggle mover toggles forever.
			if (bTriggerOnceOnly && InitialState != EWoTMoverState::TriggerToggle)
			{
				bSpent = true;
			}
		}
		break;
	}
	case EWoTMoverPhase::Open:
		if (InitialState == EWoTMoverState::TriggerToggle || bSpent)
		{
			break;   // stays until re-triggered / forever
		}
		if (PhaseTime >= StayOpenTime)
		{
			StartClose();
		}
		break;
	case EWoTMoverPhase::Closing:
	{
		const float CloseDur = FMath::Max(CloseDuration(), UE_SMALL_NUMBER);
		const EWoTMoverPhase AppliedPhase = Phase;
		ApplyAlpha(CurrentMoverAlpha());
		if (Phase != AppliedPhase)
		{
			break;
		}
		if (PhaseTime >= CloseDur)
		{
			RestingKeyNum = 0;
			RestingAlpha = 0.0f;
			PhaseStartAlpha = 0.0f;
			SetPhaseAuthority(EWoTMoverPhase::Closed);
		}
		break;
	}
	default:
		break;
	}
}

float AWoTMover::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                            AController* EventInstigator, AActor* DamageCauser)
{
	// Mover.TakeDamage: damage-triggered movers open when damaged.
	if (!bFSMInert && bDamageTriggered && HasAuthority())
	{
		StartOpen(EventInstigator ? EventInstigator->GetPawn() : nullptr);
	}
	return 0.0f;
}

void AWoTMover::SimulateClientInterp(float DeltaTime)
{
	// The FSM TRANSITIONS stay authority-only; a proxy just re-runs the keyframe interpolation for
	// the current replicated Phase, advancing a LOCAL clock (reset by OnRep_Phase). bSweep=false —
	// the proxy moves the brush visually, never doing authoritative encroach.
	switch (Phase)
	{
	case EWoTMoverPhase::Opening:
		PhaseTime += DeltaTime;
		ApplyAlpha(CurrentMoverAlpha(), false);
		break;
	case EWoTMoverPhase::Closing:
		PhaseTime += DeltaTime;
		ApplyAlpha(CurrentMoverAlpha(), false);
		break;
	case EWoTMoverPhase::Open:
		ApplyAlpha(1.0f, false);
		break;
	case EWoTMoverPhase::Delaying:
	case EWoTMoverPhase::Closed:
		ApplyKeyframeLerp(RestingKeyNum, RestingKeyNum, 0.0f, false);
		break;
	case EWoTMoverPhase::Stopped:
		ApplyAlpha(StoppedAlpha, false);
		break;
	}
}

void AWoTMover::OnRep_Phase()
{
	if (!HasActorBegunPlay())
	{
		// Initial bunch before BeginPlay: BaseLoc/BaseRot aren't cached yet, so applying a
		// pose here would teleport the brush to world origin. BeginPlay re-invokes us.
		return;
	}
	// A new phase (or a same-phase restart — PhaseStartServerTime shares this notify)
	// arrived. Seed the local clock from the server's phase-start stamp so a mid-move
	// reversal resumes at the server's current alpha and a mid-travel joiner doesn't pop
	// to the phase start (UE1 Mover clients got PhysAlpha via SimInterpolate; Timer()'s
	// RealPosition snap covered residual drift). Open/Closed still snap exactly.
	const AGameStateBase* GS = GetWorld()->GetGameState();
	PhaseTime = GS ? FMath::Max(0.0f, GS->GetServerWorldTimeSeconds() - PhaseStartServerTime)
	               : 0.0f;
	if (Phase == EWoTMoverPhase::Open)
	{
		RestingKeyNum = FMath::Max(0,
			FMath::Max(KeyPos.Num(), KeyRot.Num()) - 1);
		RestingAlpha = 1.0f;
		PhaseStartAlpha = 1.0f;
		ApplyAlpha(1.0f, false);
	}
	else if (Phase == EWoTMoverPhase::Closed)
	{
		if (PhaseStartServerTime == 0.0f)
		{
			// The initial closed phase is the authored KeyNum seat, not
			// necessarily key zero. PhaseStartAlpha itself may invoke this notify.
			PhaseStartAlpha = RestingAlpha;
			ApplyKeyframeLerp(RestingKeyNum, RestingKeyNum, 0.0f, false);
		}
		else
		{
			RestingKeyNum = 0;
			RestingAlpha = 0.0f;
			PhaseStartAlpha = 0.0f;
			ApplyAlpha(0.0f, false);
		}
	}
	else if (Phase == EWoTMoverPhase::Stopped)
	{
		ApplyAlpha(StoppedAlpha, false);
	}
	ApplyPhaseSoundPresentation();
}

void AWoTMover::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTMover, Phase);
	DOREPLIFETIME(AWoTMover, PhaseStartServerTime);
	DOREPLIFETIME(AWoTMover, PhaseStartAlpha);
	DOREPLIFETIME(AWoTMover, StoppedAlpha);
	DOREPLIFETIME(AWoTMover, LockPresentationKind);
	DOREPLIFETIME(AWoTMover, LockPresentationCounter);
}
