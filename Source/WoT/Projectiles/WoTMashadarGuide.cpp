#include "WoTMashadarGuide.h"
#include "../WoTRandom.h"

#include "../WoTSoundTable.h"
#include "../WoTWeaveFX.h"

#include "WoTProjectile.h"   // FWoTDamageEvent
#include "WoTAngrealIllusionProjectile.h"
#include "WoTMashadarSegment.h"
#include "../AI/WoTNPCCharacter.h"   // TeamOf
#include "../WoTRetailText.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTBattleGameMode.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

AWoTMashadarGuide::AWoTMashadarGuide()
{
	// Persistent, replicated defender — NOT a one-shot bNetTemporary weave.
	bNetTemporary = false;
	SetReplicateMovement(true);
	InitialLifeSpan = 0.0f;   // lives until unsummoned / killed, no timeout

	// MP movement (MashadarArenaSpawner.uc:47-52 MPSpeed/MPMaxSpeed/MPAccel).
	Speed = 200.0f;
	MaxSpeed = 400.0f;
	Acceleration = 25.0f;
	HeadType = AWoTMashadarSegment::StaticClass();
	SegmentType = AWoTMashadarSegment::StaticClass();

	// The tendril can be attacked and killed (MashadarTrailer.uc bProjTarget=True; the
	// segment collision routes damage to HurtMashadar).
	SetCanBeDamaged(true);

	// The ONLY shipped bNotifySeePlayer=True projectile (MashadarGuide.uc:1934) —
	// the Personal Illusion decoy's consumer: MyShowSelf pings tendrils in range.
	bNotifySeePlayer = true;
}

void AWoTMashadarGuide::BeginPlay()
{
	Super::BeginPlay();
	// Initialize is simulated in retail: every machine first restores the authored
	// transform/config, then builds a private RemoteRole=None chain
	// (MashadarGuide.uc:262-344). The authority captures that initial snapshot before
	// moving one head-length forward; a client consumes the COND_InitialOnly copy.
	if (HasAuthority())
	{
		if (DefenderDisplayName.IsEmpty())
		{
			if (FWoTRetailText::EnsureLoaded())
			{
				if (const FString* Name = FWoTRetailText::FindSectionValue(
					TEXT("MashadarInventory"), TEXT("Title")))
				{
					DefenderDisplayName = *Name;
				}
			}
		}
		InitialGuideLocation = GetActorLocation();
		InitialGuideRotation = GetActorRotation();
		HidingPlaceLocation = InitialGuideLocation;
		InitialSpeed = Speed;
		InitialMaxSpeed = MaxSpeed;
		InitialAcceleration = Acceleration;
	}
	else
	{
		SetActorLocationAndRotation(
			InitialGuideLocation, InitialGuideRotation, false, nullptr,
			ETeleportType::TeleportPhysics);
		Speed = InitialSpeed;
		MaxSpeed = InitialMaxSpeed;
		Acceleration = InitialAcceleration;
	}

	// PostBeginPlay (MashadarGuide.uc:247-256): a deployed citadel guide uses the
	// trigger cylinder; an arena/non-deployed guide uses the full hunt radius.
	const float InitialSightRadius = bDeployed ? TriggerRadius : SearchRadius;
	SeePlayerRadius = InitialSightRadius;
	SeePlayerHeight = InitialSightRadius;
	InitChain();
	if (HasAuthority())
	{
		SearchTimer = 0.0f;   // search on the first tick
		// MashadarGuide.uc:26-35: a level-placed citadel guide is inactive until a
		// focus pawn deploys it. Arena summons retain the immediate hunt.
		if (bCitadelPlaced && ProjectileMovement)
		{
			ProjectileMovement->Velocity = FVector::ZeroVector;
		}
	}

	// Initial RepNotify callbacks run before BeginPlay and may replay years-old edge
	// counters. Apply the separately replicated phase last: it is the current-state
	// truth for a joiner, including focus-edit speed even if FocusPawn is unresolved.
	ApplyDurableChainState();
}

void AWoTMashadarGuide::InitializeArenaHealth()
{
	if (!HasAuthority() || bCitadelPlaced || MashadarHealth > 0)
	{
		return;
	}
	// MashadarManager.AddMashadar stores int(RandRange(float,float)); match the
	// float draw and truncation: the authored upper bound occurs only on the
	// inclusive 1.0 draw (1 time in 32768, as in retail).
	MashadarHealth = FMath::TruncToInt(WoTRandom::FRandRange(
		static_cast<float>(MashadarMinHealth),
		static_cast<float>(MashadarMaxHealth)));
}

void AWoTMashadarGuide::InitChain()
{
	// MashadarGuide.uc:262 Initialize — anchor the lair behind us and grow the initially
	// hidden head cloud (MashadarTip) that follows the guide. The lair faces back
	// the way we came, so body segments spawn oriented down the chain.
	if (bInitialized)
	{
		return;
	}
	HidingPlaceRotation = (-GetActorForwardVector()).Rotation();

	// The guide leads a head-distance in front of the lair so the head segment aligns with
	// the anchor (MashadarGuide.uc:304).
	SetActorLocation(GetActorLocation() + GetActorForwardVector() * HeadDist);

	MasterSegment = SpawnSegment(/*bHead*/ true, GetActorLocation(), GetActorRotation());
	if (MasterSegment)
	{
		MasterSegment->bMaster = true;
		MasterSegment->HeadActor = this;
		// Initialize rests with the head cloud hidden (uc:320-325). BeginPlay applies the
		// durable replicated phase after this spawn; the solid DisplayMesh itself stays
		// hidden permanently.
		MasterSegment->SetActorHiddenInGame(!bHeadShouldBeVisible);
		LastSegment = MasterSegment;
		MasterSegment->Reposition(this);
	}
	bInitialized = true;
}

bool AWoTMashadarGuide::IsEnemyPawn(const AActor* Other) const
{
	const APawn* Pawn = Cast<APawn>(Other);
	if (!Pawn)
	{
		return false;
	}
	// Alive?
	if (const AWoTCharacter* Char = Cast<AWoTCharacter>(Pawn))
	{
		if (!Char->IsAlive())
		{
			return false;
		}
	}
	// A real team that isn't ours (MashadarGuide.uc:1656-1661: PRI.Team < 255 && != Team).
	const uint8 OtherTeam = AWoTNPCCharacter::TeamOf(Pawn);
	return OtherTeam != Team && OtherTeam < 255;
}

bool AWoTMashadarGuide::IsWithinGuard(const FVector& VictimLoc) const
{
	// ShouldStopGuarding (MashadarManager.uc:683): a victim only counts while it threatens
	// the guarded thing. Free mode guards nothing, so every enemy qualifies.
	switch (GuardMode)
	{
	case EWoTMashadarGuard::Free:
		return true;
	case EWoTMashadarGuard::Location:
		return FVector::DistSquared(VictimLoc, GuardPoint) <= FMath::Square(GuardRadius);
	case EWoTMashadarGuard::Actor:
		return GuardTarget.IsValid() &&
		       FVector::DistSquared(VictimLoc, GuardTarget->GetActorLocation()) <=
		           FMath::Square(GuardRadius);
	case EWoTMashadarGuard::Seal:
	{
		// IsCloseToSeal (MashadarManager.uc:249): within SealGuardRadius of ANY seal.
		const float RadiusSq = FMath::Square(SealGuardRadius);
		for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
		{
			if (IsValid(*It) &&
			    FVector::DistSquared(VictimLoc, It->GetActorLocation()) <= RadiusSq)
			{
				return true;
			}
		}
		return false;
	}
	default:
		return true;
	}
}

AActor* AWoTMashadarGuide::GetClosestPawn() const
{
	// MashadarGuide.uc:1632 GetClosestPawn: the nearest reachable enemy pawn within
	// SearchRadius of the hiding place (and, when guarding, within the guard radius of the
	// guarded thing), measured from OUR current location.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	const float EffectiveSearchRadius = bDeployed ? TriggerRadius : SearchRadius;
	AActor* Closest = nullptr;
	float ClosestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* Candidate = *It;
		if (!IsValid(Candidate) || !IsEnemyPawn(Candidate))
		{
			continue;
		}
		// GetClosestPawn (MashadarGuide.uc:1632-1676) is a pure reachable-nearest
		// selector. Chase applies its destination/state policy afterwards; folding
		// that policy into this scan prevents a new guard order from selecting a
		// farther pawn that actually threatens the newly guarded objective.
		if (!const_cast<AWoTMashadarGuide*>(this)->CanReachDestination(
			Candidate->GetActorLocation())) continue;
		const FVector CandidateLoc = Candidate->GetActorLocation();
		const FVector LairDelta = CandidateLoc - HidingPlaceLocation;
		if (LairDelta.SizeSquared() > FMath::Square(EffectiveSearchRadius))
		{
			continue;
		}
		if (!IsWithinGuard(CandidateLoc))
		{
			continue;   // it doesn't threaten what we're guarding
		}
		const float DistSq = FVector::DistSquared(CandidateLoc, GetActorLocation());
		if (DistSq < ClosestDistSq)
		{
			Closest = Candidate;
			ClosestDistSq = DistSq;
		}
	}
	return Closest;
}

void AWoTMashadarGuide::GuardSeal()
{
	// GuardSeal (MashadarGuide.uc:1830): guard contested seals. The manager's
	// IncrementSealGuardCount is a multi-guide bookkeeping counter — with one guide per
	// arena cast it reduces to setting this guide's mode.
	GuardMode = EWoTMashadarGuard::Seal;
}

void AWoTMashadarGuide::GuardLocation(const FVector& Loc)
{
	GuardMode = EWoTMashadarGuard::Location;
	GuardPoint = Loc;
}

void AWoTMashadarGuide::GuardActor(AActor* Victim)
{
	GuardMode = EWoTMashadarGuard::Actor;
	GuardTarget = Victim;
}

void AWoTMashadarGuide::UnGuard()
{
	// UnGuard (MashadarGuide.uc:1841): drop the assignment and resume free hunting.
	GuardMode = EWoTMashadarGuard::Free;
	GuardTarget = nullptr;
}

bool AWoTMashadarGuide::CanBindFocusAt(const FVector& Position) const
{
	// MashadarFocusPawn.uc:195-263 GetClosestMashadar: the shipped team test is
	// commented out at :215/:242, so eligibility intentionally remains cross-team.
	return !bFullRetract && !FollowingFocusPawn()
		&& FVector::Dist(Position, HidingPlaceLocation) < SearchRadius;
}

bool AWoTMashadarGuide::FollowingFocusPawn() const
{
	return IsValid(FocusPawn);
}

void AWoTMashadarGuide::SetFocusPawn(AWoTNPCCharacter* NewFocusPawn)
{
	// MashadarGuide.uc:1744-1790: binding cancels retraction, switches to the
	// citadel-edit speed, and follows the marker; unbinding restores speed and
	// retracts toward HidingPlace. IsValid mirrors Accessed-None tolerance.
	if (!HasAuthority() || FocusPawn.Get() == NewFocusPawn)
	{
		return;
	}
	FocusPawn = IsValid(NewFocusPawn) ? NewFocusPawn : nullptr;
	if (IsValid(FocusPawn.Get()))
	{
		// ClientSetFocusPawnA: UnRetract + editing speed (uc:1754-1782).
		HandleSetFocusPawnAReaction();
		++ClientSetFocusPawnACounter;
		SetDestination(FocusPawn.Get());
	}
	else
	{
		// ClientSetFocusPawnB: restore speed and retract (uc:1765-1790). Retail does
		// NOT force-deactivate here; the idle search path still owns bActive.
		HandleSetFocusPawnBReaction();
		++ClientSetFocusPawnBCounter;
		SetDestination(nullptr);
	}
	ForceNetUpdate();
}

void AWoTMashadarGuide::ExitSeeking()
{
	// The base re-blocks ECC_WorldStatic on exit; the tendril ships bCollideWorld=False +
	// bAllowClipping=True for life (MashadarGuide.uc:1941) — keep gliding through walls so
	// a crack-parked citadel guide can always retract into its crack.
	bSeeking = false;
	ApplySeekingCollisionState();
}

void AWoTMashadarGuide::ApplySeekingCollisionState()
{
	if (UPrimitiveComponent* ActiveCollision = GetActiveSweptCollision())
	{
		ActiveCollision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
		ActiveCollision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	}
}

bool AWoTMashadarGuide::IsChasableTarget(const AActor* Other) const
{
	// IsChasable (MashadarGuide.uc:1283-1305). The two legs gate teams DIFFERENTLY:
	// pawns need a real team that isn't ours (uc:1300-1301, the IsEnemyPawn rule);
	// illusion decoys need only a team that differs (uc:1292 — no <255 requirement).
	if (!IsValid(Other) || IsLiveIllusion(Destination) || Other == Destination)
		return false;
	if (IsLiveIllusion(Other))
	{
		const AWoTAngrealIllusionProjectile* Illusion =
			Cast<AWoTAngrealIllusionProjectile>(Other);
		return Illusion && Illusion->Team != Team
			&& const_cast<AWoTMashadarGuide*>(this)->CanReachDestination(
				Other->GetActorLocation());
	}
	if (!IsEnemyPawn(Other)) return false;
	if (IsValid(Destination)
		&& FVector::DistSquared(Other->GetActorLocation(), GetActorLocation())
			>= FVector::DistSquared(Destination->GetActorLocation(), GetActorLocation()))
	{
		return false;
	}
	return const_cast<AWoTMashadarGuide*>(this)->CanReachDestination(
		Other->GetActorLocation());
}

void AWoTMashadarGuide::Deploy()
{
	// MashadarGuide.uc:1718-1724.
	bDeployed = true;
	SeePlayerRadius = TriggerRadius;
	SeePlayerHeight = TriggerRadius;
	ForceNetUpdate();
}

void AWoTMashadarGuide::SearchAndChase()
{
	// Search() + Chase() folded (MashadarGuide.uc:1345/1231): home on the closest
	// enemy or retract the durable chain when there is none.
	if (!HasAuthority() || bFullRetract)
	{
		return;
	}
	if (FollowingFocusPawn())
	{
		return; // the edit marker, not combat search, owns Destination
	}
	if (AActor* Target = GetClosestPawn())
	{
		if (bShocked && AWoTAngrealInvLightning::IsUsingLightning(
			Cast<APawn>(Target)))
		{
			return;
		}
		bShocked = false;
		ShockPawn = nullptr;
		if (bCitadelPlaced)
		{
			AWoTBattleGameMode* Battle =
				GetWorld()->GetAuthGameMode<AWoTBattleGameMode>();
			const bool bAdded = bDeployed && Battle
				&& Battle->AddMashadarSlot(Team, MashadarMinHealth,
					MashadarMaxHealth);
			if (!Battle || !Battle->RequestMashadarActivation(this))
			{
				return;
			}
			if (bDeployed && bAdded)
			{
				bDeployed = false;
				SeePlayerRadius = SearchRadius;
				SeePlayerHeight = SearchRadius;
			}
		}
		else if (bDeployed)
		{
			// MashadarGuide.uc:1239-1248: the first live activation assimilates the
			// deployed guide and restores its full SearchRadius.
			InitializeArenaHealth();
			bDeployed = false;
			SeePlayerRadius = SearchRadius;
			SeePlayerHeight = SearchRadius;
		}
		if (Target != Destination)
		{
			SetDestination(Target);
			// MashadarGuide.uc:793 — the hunt cry on a fresh destination.
			WoTSoundTable::PlaySlotSound(*this, VoiceTableName, FName(TEXT("SeeEnemy")));
		}
		Activate();   // a target is in reach — the body bites
	}
	else if (Destination)
	{
		// Retail Search (uc:1345-1382) only RE-targets when a closest pawn exists;
		// a SeePlayer-chased illusion is not dropped by an empty pawn scan — the
		// decoy keeps the tendril occupied until it expires or a real pawn shows.
		if (IsLiveIllusion(Destination))
		{
			return;
		}
		SetDestination(nullptr);
		Deactivate();   // nothing to hunt — withdraw
	}
}

bool AWoTMashadarGuide::IsLiveIllusion(const AActor* Other)
{
	return IsValid(Other) && Other->IsA<AWoTAngrealIllusionProjectile>();
}

void AWoTMashadarGuide::NotifySeePawn(AActor* SeenPawn)
{
	// SeePlayer -> Chase (MashadarGuide.uc:504-552). The 16-entry randomized retry
	// table runs before Chase, as shipped. RestTimer's only live write belongs to
	// the unported partial-chain death path.
	// The Waiting state ignores SeePlayer (uc:750); resting-at-home is our analog.
	if (!HasAuthority() || bFullRetract || !IsValid(SeenPawn) || FollowingFocusPawn())
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	int32 TimerIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(SeePlayerTimerInfos); ++Index)
	{
		if (SeePlayerTimerInfos[Index].Seen.Get() != SeenPawn) continue;
		TimerIndex = Index;
		if (Now < SeePlayerTimerInfos[Index].NextNotifyTime) return;
		break;
	}
	if (TimerIndex == INDEX_NONE)
	{
		TimerIndex = SeePlayerTimerInfoIndex;
		SeePlayerTimerInfoIndex = (SeePlayerTimerInfoIndex + 1)
			% UE_ARRAY_COUNT(SeePlayerTimerInfos);
		SeePlayerTimerInfos[TimerIndex].Seen = SeenPawn;
	}
	SeePlayerTimerInfos[TimerIndex].NextNotifyTime = Now
		+ WoTRandom::FRandRange(3.0f, 6.0f);
	// IsChasable gates Chase BEFORE any assimilation (MashadarGuide.uc:1235): a friendly
	// pawn or friendly illusion must not consume bDeployed, widen radii, or retarget.
	if (!IsChasableTarget(SeenPawn))
	{
		return;
	}
	if (bShocked && AWoTAngrealInvLightning::IsUsingLightning(
		Cast<APawn>(SeenPawn)))
	{
		return;
	}
	bShocked = false;
	ShockPawn = nullptr;
	if (bCitadelPlaced)
	{
		AWoTBattleGameMode* Battle = GetWorld()->GetAuthGameMode<AWoTBattleGameMode>();
		const bool bAdded = bDeployed && Battle
			&& Battle->AddMashadarSlot(Team, MashadarMinHealth,
				MashadarMaxHealth);
		if (!Battle || !Battle->RequestMashadarActivation(this))
		{
			return;
		}
		if (bDeployed && bAdded)
		{
			bDeployed = false;
			SeePlayerRadius = SearchRadius;
			SeePlayerHeight = SearchRadius;
		}
	}
	else if (bDeployed)
	{
		// Chase (MashadarGuide.uc:1239-1248): every first live chase, including
		// SeePlayer, assimilates a deployed guide and restores its full radius.
		InitializeArenaHealth();
		bDeployed = false;
		SeePlayerRadius = SearchRadius;
		SeePlayerHeight = SearchRadius;
	}
	if (SeenPawn != Destination)
	{
		SetDestination(SeenPawn);
		WoTSoundTable::PlaySlotSound(*this, VoiceTableName, FName(TEXT("SeeEnemy")));
	}
	Activate();
}

void AWoTMashadarGuide::Activate()
{
	// MashadarGuide.uc:565 — the tendril is live; its body segments now bite.
	if (HasAuthority() && !bShocked)
	{
		bActive = true;
		ChainPhase = EWoTMashadarChainPhase::Extending;
		bUseCitadelEditingSpeed = false;
	}
}

void AWoTMashadarGuide::Shock(APawn* LightningPawn)
{
	if (!HasAuthority())
	{
		return;
	}
	bShocked = true;
	ShockPawn = LightningPawn;
	Deactivate();
}

void AWoTMashadarGuide::SetDestination(AActor* Dest)
{
	if (!HasAuthority())
	{
		return;
	}
	if (Dest && bRetracting)
	{
		// Search/Chase explicitly UnRetracts before assigning a new victim
		// (MashadarGuide.uc:1256-1264, 1370-1378).
		HandleUnRetractReaction();
		++ClientUnRetractCounter;
	}
	// Destination is the event payload and is assigned by the base before the paired
	// counter below, so a same-bunch RepNotify observes the matching actor.
	Super::SetDestination(Dest);
	// Retail ClientSetDestination reveals and restarts the master cloud whenever
	// a real destination is acquired (MashadarGuide.uc:782-816). DisplayMesh
	// itself remains never-rendered; unhiding the segment exposes its quad pool.
	if (Dest)
	{
		ChainPhase = EWoTMashadarChainPhase::Extending;
		bUseCitadelEditingSpeed = FollowingFocusPawn();
		HandleSetDestinationReaction();
		++ClientSetDestinationCounter;
	}
	else
	{
		ChainPhase = EWoTMashadarChainPhase::Retracting;
		bUseCitadelEditingSpeed = false;
		bRetracting = true;
	}
	ForceNetUpdate();
}

void AWoTMashadarGuide::Deactivate()
{
	if (!HasAuthority())
	{
		return;
	}
	// Deactivate(true) starts Retract, whose GuideToHidingPlace clears Destination
	// (MashadarGuide.uc:573-599, 681-713, 1490-1520).
	if (Destination)
	{
		Super::SetDestination(nullptr);
	}
	// ClientDeactivate performs the same local visual-chain withdrawal
	// (uc:436-457). bActive also replicates as durable late-join state.
	bActive = false;
	if (bCitadelPlaced)
	{
		if (AWoTBattleGameMode* Battle =
			GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
		{
			Battle->ReleaseMashadarActivation(this);
		}
	}
	ChainPhase = EWoTMashadarChainPhase::Retracting;
	bUseCitadelEditingSpeed = false;
	HandleDeactivateReaction();
	++ClientDeactivateCounter;
	ForceNetUpdate();
}

void AWoTMashadarGuide::HandleDeactivateReaction()
{
	++DeactivateReactionCount;
	bActive = false;
	bRetracting = true;
}

void AWoTMashadarGuide::HandleUnRetractReaction()
{
	++UnRetractReactionCount;
	bRetracting = false;
}

void AWoTMashadarGuide::HandleSetDestinationReaction()
{
	++SetDestinationReactionCount;
	LastSetDestinationPayload = Destination;
	bRetracting = false;
	bHeadShouldBeVisible = true;
	if (MasterSegment)
	{
		MasterSegment->SetActorHiddenInGame(false);
		MasterSegment->FadeIn();
	}
}

void AWoTMashadarGuide::HandleSetFocusPawnAReaction()
{
	++SetFocusPawnAReactionCount;
	bRetracting = false;
	Speed = CitadelEditingSpeed;
	MaxSpeed = CitadelEditingSpeed;
}

void AWoTMashadarGuide::HandleSetFocusPawnBReaction()
{
	++SetFocusPawnBReactionCount;
	Speed = InitialSpeed;
	MaxSpeed = InitialMaxSpeed;
	bRetracting = true;
}

void AWoTMashadarGuide::ApplyDurableChainState()
{
	if (bUseCitadelEditingSpeed)
	{
		Speed = CitadelEditingSpeed;
		MaxSpeed = CitadelEditingSpeed;
	}
	else
	{
		Speed = InitialSpeed;
		MaxSpeed = InitialMaxSpeed;
		Acceleration = InitialAcceleration;
	}

	switch (ChainPhase)
	{
	case EWoTMashadarChainPhase::Extending:
		bRetracting = false;
		bHeadShouldBeVisible = true;
		break;
	case EWoTMashadarChainPhase::Retracting:
		bRetracting = true;
		bHeadShouldBeVisible = true;
		break;
	case EWoTMashadarChainPhase::Resting:
	default:
		bRetracting = false;
		bHeadShouldBeVisible = false;
		break;
	}

	if (MasterSegment)
	{
		MasterSegment->SetActorHiddenInGame(!bHeadShouldBeVisible);
		if (bHeadShouldBeVisible)
		{
			MasterSegment->FadeIn();
		}
	}
}

void AWoTMashadarGuide::OnRep_ClientDeactivateCounter()
{
	if (ClientDeactivateCounter == LastClientDeactivateCounter)
	{
		return;
	}
	LastClientDeactivateCounter = ClientDeactivateCounter;
	HandleDeactivateReaction();
}

void AWoTMashadarGuide::OnRep_ClientUnRetractCounter()
{
	if (ClientUnRetractCounter == LastClientUnRetractCounter)
	{
		return;
	}
	LastClientUnRetractCounter = ClientUnRetractCounter;
	HandleUnRetractReaction();
}

void AWoTMashadarGuide::OnRep_ClientSetDestinationCounter()
{
	if (ClientSetDestinationCounter == LastClientSetDestinationCounter)
	{
		return;
	}
	LastClientSetDestinationCounter = ClientSetDestinationCounter;
	HandleSetDestinationReaction();
}

void AWoTMashadarGuide::OnRep_ClientSetFocusPawnACounter()
{
	if (ClientSetFocusPawnACounter == LastClientSetFocusPawnACounter)
	{
		return;
	}
	LastClientSetFocusPawnACounter = ClientSetFocusPawnACounter;
	HandleSetFocusPawnAReaction();
}

void AWoTMashadarGuide::OnRep_ClientSetFocusPawnBCounter()
{
	if (ClientSetFocusPawnBCounter == LastClientSetFocusPawnBCounter)
	{
		return;
	}
	LastClientSetFocusPawnBCounter = ClientSetFocusPawnBCounter;
	HandleSetFocusPawnBReaction();
}

void AWoTMashadarGuide::OnRep_FocusPawn()
{
	// Actor-reference resolution may lag the counter that announced focus A. The
	// explicit replicated speed context, rather than reference timing, is authoritative.
	if (bUseCitadelEditingSpeed)
	{
		Speed = CitadelEditingSpeed;
		MaxSpeed = CitadelEditingSpeed;
	}
	else
	{
		Speed = InitialSpeed;
		MaxSpeed = InitialMaxSpeed;
		Acceleration = InitialAcceleration;
	}
}

void AWoTMashadarGuide::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);   // seeker homing
	// Retail Tick called simulated Initialize on every machine until it succeeded
	// (MashadarGuide.uc:337-344). BeginPlay normally does this; this retry also makes a
	// late component/world initialization harmless.
	if (!bInitialized)
	{
		InitChain();
	}

	if (HasAuthority())
	{
		// Native projectile observers receive every relevant player through a
		// no-LOS cylinder test (2D radius plus absolute Z height).
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
			It; ++It)
		{
			APawn* Player = It->Get() ? It->Get()->GetPawn() : nullptr;
			if (!IsValid(Player) || Player->IsHidden()) continue;
			const FVector Delta = Player->GetActorLocation() - GetActorLocation();
			if (FVector2D(Delta.X, Delta.Y).SizeSquared()
				<= FMath::Square(SeePlayerRadius)
				&& FMath::Abs(Delta.Z) <= SeePlayerHeight)
			{
				NotifySeePawn(Player);
			}
		}
		// The shipped polling Search block is disabled. Native SeePlayer dispatch above
		// is the only ordinary pawn-acquisition path; explicit illusion/guard calls may
		// still invoke SearchAndChase directly. This preserves the native 3-6 second
		// per-pawn retry table instead of bypassing it every SearchResolution seconds.

		// Retract to the hiding place while idle. Only authority writes movement; a
		// simulated proxy follows AActor's replicated movement and solves its local IK.
		if (bRetracting && ProjectileMovement)
		{
			const FVector ToHome = HidingPlaceLocation - GetActorLocation();
			const bool bHome = ToHome.SizeSquared() <= FMath::Square(RestRadius);
			ProjectileMovement->Velocity = bHome ? FVector::ZeroVector
			                                     : ToHome.GetSafeNormal() * RetractSpeed;
		}
	}

	// Chain growth/retract/reposition is simulated on every machine. The segments are
	// non-replicated local actors (WoTIKTrailer.cpp; MashadarGuide.uc:337-392).
	UpdateChain();

	// Retail Reset is reached only after the retract has peeled back to the head. Apply
	// its complete simulated reset block, and author/replicate the cleared durable state
	// on the server (MashadarGuide.uc:1393-1404).
	if (bRetracting && MasterSegment && LastSegment == MasterSegment
		&& FVector::DistSquared(GetActorLocation(), HidingPlaceLocation)
			<= FMath::Square(RestRadius))
	{
		ResetAtHome();
	}
}

void AWoTMashadarGuide::ResetAtHome()
{
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
	}
	SetActorLocation(InitialGuideLocation, false, nullptr, ETeleportType::TeleportPhysics);
	Speed = InitialSpeed;
	MaxSpeed = InitialMaxSpeed;
	Acceleration = InitialAcceleration;
	bFullRetract = false;
	bUseCitadelEditingSpeed = false;
	ChainPhase = EWoTMashadarChainPhase::Resting;
	bRetracting = false;
	bHeadShouldBeVisible = false;
	if (MasterSegment)
	{
		MasterSegment->SetActorHiddenInGame(true);
	}
	if (HasAuthority())
	{
		SearchTimer = 0.0f;
		ForceNetUpdate();
	}
}

int32 AWoTMashadarGuide::CountBodySegments() const
{
	int32 N = 0;
	for (AWoTIKTrailer* Seg = MasterSegment ? MasterSegment->ChildTrailer : nullptr;
	     Seg; Seg = Seg->ChildTrailer)
	{
		++N;
	}
	return N;
}

AWoTMashadarSegment* AWoTMashadarGuide::SpawnSegment(bool bHead, const FVector& Loc, const FRotator& Rot)
{
	const TSubclassOf<AWoTMashadarSegment> SpawnType = bHead ? HeadType : SegmentType;
	UClass* SegmentClass = SpawnType.Get();
	if (!SegmentClass)
	{
		SegmentClass = AWoTMashadarSegment::StaticClass();
	}
	const FTransform Transform(Rot, Loc);
	AWoTMashadarSegment* Seg = GetWorld()->SpawnActorDeferred<AWoTMashadarSegment>(
		SegmentClass, Transform, this, GetInstigator(),
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Seg)
	{
		Seg->Guide = this;
		Seg->bIsHead = bHead;
		Seg->Dist = bHead ? HeadDist : BodyDist;
		Seg->TouchRadius = bHead ? HeadRadius : BodyRadius;
		// The client-local collision radius and MashTip/MashSeg render binding must see
		// replicated config before BeginPlay, just as Spawn(HeadType/SegmentType) did.
		Seg->FinishSpawning(Transform);
	}
	return Seg;
}

void AWoTMashadarGuide::AppendTailSegment()
{
	// Seed one body-length nearer the lair than the current tail. That preserves the
	// distance progress of retail's reverse-link choreography and lets its enclosing
	// while-loop catch up a whole movement jump in one tick (MashadarGuide.uc:1430-1459).
	if (!MasterSegment || !LastSegment)
	{
		return;
	}
	const FVector TailOffset = LastSegment->GetActorLocation() - HidingPlaceLocation;
	const float SeedDistance = FMath::Max(TailOffset.Size() - BodyDist, 0.0f);
	const FVector SeedLocation = HidingPlaceLocation
		+ TailOffset.GetSafeNormal() * SeedDistance;
	AWoTMashadarSegment* NewSeg = SpawnSegment(
		/*bHead*/ false, SeedLocation, HidingPlaceRotation);
	if (!NewSeg)
	{
		return;
	}
	LastSegment->ChildTrailer = NewSeg;
	LastSegment = NewSeg;

	// The growth creak (MashadarGuide.uc:432: 'Breath' when >2 segments were added
	// in one shipped growth tick). The third append in this catch-up pass triggers it.
	if (HasAuthority())
	{
		const float Now = GetWorld()->GetTimeSeconds();
		if (Now - AppendWindowStart > 1.0f)
		{
			AppendWindowStart = Now;
			AppendsInWindow = 0;
		}
		if (++AppendsInWindow == 3)
		{
			WoTSoundTable::PlaySlotSound(*this, VoiceTableName, FName(TEXT("Breath")));
		}
	}
}

void AWoTMashadarGuide::RemoveTailSegment()
{
	// Peel the last body segment off the tail (MashadarGuide.uc:371 retract step).
	if (!LastSegment || LastSegment == MasterSegment)
	{
		return;   // never remove the head
	}
	// Find the segment whose child is the tail.
	AWoTMashadarSegment* Prev = MasterSegment;
	while (Prev && Prev->ChildTrailer != LastSegment)
	{
		Prev = Cast<AWoTMashadarSegment>(Prev->ChildTrailer);
	}
	AWoTMashadarSegment* OldTail = LastSegment;
	if (Prev)
	{
		Prev->ChildTrailer = nullptr;
		LastSegment = Prev;
	}
	OldTail->FadeAway();
}

int32 AWoTMashadarGuide::GetTrailerIndex(const AWoTMashadarSegment* Segment) const
{
	int32 Index = 1;
	for (const AWoTIKTrailer* It = MasterSegment; It; It = It->ChildTrailer, ++Index)
	{
		if (It == Segment)
		{
			return Index;
		}
	}
	return 0;
}

bool AWoTMashadarGuide::IsManagedSegment(
	const AWoTMashadarSegment* Segment) const
{
	return Segment && GetTrailerIndex(Segment) > 0;
}

AWoTMashadarSegment* AWoTMashadarGuide::GetTrailerAt(int32 OneBasedIndex) const
{
	if (OneBasedIndex <= 0)
	{
		return nullptr;
	}
	AWoTIKTrailer* It = MasterSegment;
	while (It && --OneBasedIndex > 0)
	{
		It = It->ChildTrailer;
	}
	return Cast<AWoTMashadarSegment>(It);
}

void AWoTMashadarGuide::BreakOffMashadarAt(AWoTMashadarSegment* Segment)
{
	// Retail keeps the master head, moves it to the hit, attaches the portion beyond
	// that hit, and fades the severed proximal half (MashadarGuide.uc:632-653).
	if (!MasterSegment || !Segment || Segment == MasterSegment)
	{
		return;
	}
	AWoTMashadarSegment* FirstSevered =
		Cast<AWoTMashadarSegment>(MasterSegment->ChildTrailer);
	AWoTMashadarSegment* FirstRemaining =
		Cast<AWoTMashadarSegment>(Segment->ChildTrailer);
	MasterSegment->ChildTrailer = FirstRemaining;
	Segment->ChildTrailer = nullptr;
	SetActorLocation(Segment->GetActorLocation());
	if (!FirstRemaining)
	{
		LastSegment = MasterSegment;
	}
	if (FirstSevered)
	{
		FirstSevered->FadeAway();
	}
	MasterSegment->Reposition(this);
}

void AWoTMashadarGuide::HandleBreakOffMashadarAtReaction(const int32 OneBasedIndex)
{
	++BreakOffReactionCount;
	AWoTMashadarSegment* Segment = GetTrailerAt(OneBasedIndex);
	if (!Segment)
	{
		Segment = MasterSegment; // retail's missing/out-of-sync fallback (uc:659-669)
	}
	BreakOffMashadarAt(Segment);
	bFullRetract = true;
	bActive = false;
	bRetracting = true;
}

void AWoTMashadarGuide::MulticastBreakOffMashadarAt_Implementation(
	const int32 OneBasedIndex)
{
	if (bBreakOffHandled)
	{
		return;
	}
	bBreakOffHandled = true;
	HandleBreakOffMashadarAtReaction(OneBasedIndex);
}

void AWoTMashadarGuide::NotifyLethalSegmentHit(AWoTMashadarSegment* HitSegment)
{
	if (!HasAuthority() || !HitSegment || bBreakOffHandled)
	{
		return;
	}
	const int32 HitIndex = GetTrailerIndex(HitSegment);
	if (HitIndex <= 0)
	{
		return;
	}
	ChainPhase = EWoTMashadarChainPhase::Retracting;
	bUseCitadelEditingSpeed = false;
	// Queue the reliable RPC before TakeDamage can reach Destroy(), which closes the
	// guide's channel. Its parameter is the complete atomic payload (uc:894-908 intent).
	MulticastBreakOffMashadarAt(HitIndex);
}

void AWoTMashadarGuide::UpdateChain()
{
	if (!MasterSegment || BodyDist <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	int32 Body = CountBodySegments();
	if (!bRetracting && !bFullRetract
		&& (bActive || FollowingFocusPawn() || bHeadShouldBeVisible))
	{
		// The trailer actors may tick after their guide, so solve the current replicated
		// movement first. Retail then appended until the actual last joint linked back
		// within its own reach of the lair (MashadarGuide.uc:355-359).
		MasterSegment->Reposition(this);
		while (LastSegment
			&& FVector::Dist(LastSegment->GetActorLocation(), HidingPlaceLocation)
				> LastSegment->Dist)
		{
			AWoTMashadarSegment* PreviousLast = LastSegment;
			AppendTailSegment();
			if (LastSegment == PreviousLast)
			{
				break;
			}
			++Body;

			// Retail increments after the append and retracts only once the new count is
			// greater than MaxNumSegments (MashadarGuide.uc:1461-1474). Clients never
			// make this authoritative decision.
			if (HasAuthority() && Body > MaxNumSegments)
			{
				Deactivate();
				bFullRetract = true;
				ForceNetUpdate();
				break;
			}
		}
	}
	else if (bRetracting || bFullRetract)
	{
		// Replicated guide reach is the visual truth during withdrawal. Trim every body
		// segment now outside that reach in this tick, rather than peeling one per frame.
		const float Reach = FVector::Dist(GetActorLocation(), HidingPlaceLocation);
		const int32 TargetBody = Reach <= RestRadius
			? 0
			: FMath::Max(FMath::CeilToInt(Reach / BodyDist), 0);
		while (Body > TargetBody)
		{
			RemoveTailSegment();
			--Body;
		}
	}
}

void AWoTMashadarGuide::TeardownChain()
{
	if (MasterSegment)
	{
		MasterSegment->FadeAway();   // cascades down the chain
	}
	MasterSegment = nullptr;
	LastSegment = nullptr;
}

void AWoTMashadarGuide::NotifySegmentDestroyed(AWoTMashadarSegment* Seg)
{
	if (Seg == MasterSegment)
	{
		MasterSegment = nullptr;
	}
	if (Seg == LastSegment)
	{
		LastSegment = MasterSegment;
	}
}

void AWoTMashadarGuide::ProcessTouch(AActor* /*Other*/, FVector /*HitLocation*/)
{
	// The guide is the homing LEADER, not a weapon: it glides through whatever it touches
	// and never explodes (it is persistent). The damage lives on the body segments
	// (AWoTMashadarSegment) trailing behind it — MashadarTrailer.uc:52 Touch.
}

void AWoTMashadarGuide::Explode(FVector /*HitLocation*/, FVector /*HitNormal*/)
{
	// MashadarGuide.uc:972-979 deliberately bypasses Super.Explode(): the guide is
	// persistent and arrival only changes its steering state.
	NotifyReachedDestination();
}

void AWoTMashadarGuide::NotifyReachedDestination()
{
	// MashadarGuide.uc:982-1012. Illusions and citadel focus markers hold the head
	// in place; a combat target makes it coil instead of repeatedly colliding with
	// or destroying itself on the victim.
	if (!ProjectileMovement)
	{
		return;
	}
	if (AWoTAngrealIllusionProjectile* Illusion =
		Cast<AWoTAngrealIllusionProjectile>(Destination))
	{
		ProjectileMovement->StopMovementImmediately();
		const AWoTAngrealIllusionProjectile* DefaultIllusion =
			Illusion->GetClass()
				->GetDefaultObject<AWoTAngrealIllusionProjectile>();
		const float FadeLife = DefaultIllusion
			? DefaultIllusion->InitialLifeSpan * Illusion->FadePercent : 0.0f;
		if (FadeLife > 0.0f)
		{
			Illusion->SetLifeSpan(FMath::Min(Illusion->GetLifeSpan(), FadeLife));
		}
		return;
	}
	if (FollowingFocusPawn())
	{
		ProjectileMovement->StopMovementImmediately();
		return;
	}

	const FVector TargetLocation = Destination
		? Destination->GetActorLocation() : DestLoc;
	FRotator CoilRotation = (TargetLocation - GetActorLocation()).Rotation();
	CoilRotation.Yaw += 87.890625f; // 16000 Unreal rotator units, just under 90 degrees.
	CoilRotation.Pitch = 0.0f;
	CoilRotation.Roll = GetActorRotation().Roll;
	ProjectileMovement->Velocity = CoilRotation.Vector() * Speed;
	SetActorRotation(CoilRotation);
}

void AWoTMashadarGuide::DestinationLost()
{
	// MashadarGuide.uc:1022-1029: losing a destination is an authoritative
	// deactivate decision; clients perform the paired local-chain reaction from FID.
	if (HasAuthority())
	{
		Deactivate();
	}
}

bool AWoTMashadarGuide::DestinationExists()
{
	if (!Super::DestinationExists()) return false;
	if (!Destination || Destination == FocusPawn.Get()) return true;
	return FVector::DistSquared(Destination->GetActorLocation(), HidingPlaceLocation)
		<= FMath::Square(SeePlayerRadius);
}

void AWoTMashadarGuide::Destroyed()
{
	// The whole tendril goes with the guide (MashadarGuide.uc — the segments follow the
	// guide's life; killing it "breaks off" the chain).
	TeardownChain();
	if (HasAuthority() && bCitadelPlaced)
	{
		if (AWoTBattleGameMode* Battle =
			GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
		{
			Battle->ReleaseMashadarActivation(this);
		}
	}
	Super::Destroyed();
}

float AWoTMashadarGuide::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                    AController* EventInstigator, AActor* DamageCauser)
{
	// HurtMashadar (MashadarGuide.uc:894 → MashadarManager.uc:163): drain health; the
	// killing blow destroys the whole tendril.
	if (!HasAuthority() || MashadarHealth <= 0)
	{
		return 0.0f;
	}
	const int32 Applied = FMath::TruncToInt(DamageAmount);
	MashadarHealth -= Applied;
	if (bCitadelPlaced)
	{
		if (AWoTBattleGameMode* Battle =
			GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
		{
			Battle->UpdateMashadarHealth(this, MashadarHealth);
		}
	}
	if (MashadarHealth <= 0)
	{
		// MashadarManager tests the post-damage slot health against
		// DieHardThreshold=25. Every lethal result is therefore DieHard.
		WoTSoundTable::PlaySlotSound(*this, VoiceTableName, TEXT("DieHard"));
		// Death: credit the killer a frag for destroying the defender (MashadarManager.uc:
		// 187 Level.Game.Killed(instigator, focusPawn) — the focus pawn carried our team),
		// then destroy the whole tendril ("breaks off, retracts into the walls").
		AWoTBattleGameMode* Battle = bCitadelPlaced
			? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
		FString VictimDisplayName = DefenderDisplayName;
		if (Battle)
		{
			if (const AWoTNPCCharacter* Focus = Battle->FindMashadarFocus(Team);
				Focus && !Focus->SummonDisplayName.IsEmpty())
			{
				VictimDisplayName = Focus->SummonDisplayName;
			}
		}
		if (AWoTGameMode* Game = GetWorld()
			? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
		{
			Game->ScoreMashadarKill(EventInstigator, Team, VictimDisplayName,
				DefenderDeaths, DefenderSuicides);
		}
		if (Battle)
		{
			Battle->ConsumeMashadarFocus(this, Team);
		}
		Destroy();
	}
	else
	{
		WoTSoundTable::PlaySlotSound(*this, VoiceTableName,
			Applied >= 25 ? FName(TEXT("HitHard")) : FName(TEXT("HitSoft")));
	}
	return static_cast<float>(Applied);
}

bool AWoTMashadarGuide::ConsumeSegmentDamageThisTick()
{
	if (LastSegmentDamageFrame == GFrameCounter)
	{
		return false;
	}
	LastSegmentDamageFrame = GFrameCounter;
	return true;
}

void AWoTMashadarGuide::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Authored initial transform/config. Retail's Initialize consumed these on each
	// machine before spawning its local chain (MashadarGuide.uc:193-228, 262-331).
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, Team, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, InitialGuideLocation, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, InitialGuideRotation, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, HidingPlaceLocation, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, InitialSpeed, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, InitialMaxSpeed, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, InitialAcceleration, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, HeadType, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, SegmentType, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, HeadDist, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, HeadRadius, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, BodyDist, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, BodyRadius, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, MaxNumSegments, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, SearchRadius, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, TriggerRadius, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, SearchResolution, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, RetractSpeed, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, CitadelEditingSpeed, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AWoTMashadarGuide, bCitadelPlaced, COND_InitialOnly);

	DOREPLIFETIME(AWoTMashadarGuide, GuardMode);
	DOREPLIFETIME(AWoTMashadarGuide, MashadarHealth);
	DOREPLIFETIME(AWoTMashadarGuide, bActive);
	DOREPLIFETIME(AWoTMashadarGuide, ChainPhase);
	DOREPLIFETIME(AWoTMashadarGuide, bUseCitadelEditingSpeed);
	DOREPLIFETIME(AWoTMashadarGuide, bDeployed);
	DOREPLIFETIME(AWoTMashadarGuide, bFullRetract);
	DOREPLIFETIME(AWoTMashadarGuide, FocusPawn);

	// Five reliable FID equivalents (MashadarGuide.uc:145-163, 203-211). Property
	// replication is eventual/reliable and this inherited seeker is bAlwaysRelevant;
	// each idempotent RepNotify consumes a counter value once on every client.
	DOREPLIFETIME(AWoTMashadarGuide, ClientDeactivateCounter);
	DOREPLIFETIME(AWoTMashadarGuide, ClientUnRetractCounter);
	DOREPLIFETIME(AWoTMashadarGuide, ClientSetDestinationCounter);
	DOREPLIFETIME(AWoTMashadarGuide, ClientSetFocusPawnACounter);
	DOREPLIFETIME(AWoTMashadarGuide, ClientSetFocusPawnBCounter);
}
