#include "WoTSeekingProjectile.h"

#include "../AI/WoTPathNode.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTTeleporter.h"
#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Net/UnrealNetwork.h"

AWoTSeekingProjectile::AWoTSeekingProjectile()
{
	// defaults: MaxSpeed=1200, LifeSpan=300, bNetTemporary=false,
	// bAlwaysRelevant, NetPriority=10
	MaxSpeed = 1200.0f;
	InitialLifeSpan = 300.0f;
	bNetTemporary = false;
	bAlwaysRelevant = true;
	NetPriority = 10.0f;
	// launcher/targeting defaults
	bUsesDestination = true;
	bRequiresDestination = true;
	bRequiresLeading = false;
	MaxTargetAngle = 22.5f;
}

void AWoTSeekingProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (bAlignToVelocity && ProjectileMovement)
	{
		// GenericProjectile has now derived launch velocity from the complete
		// spawn rotation. Split presentation from the flat swept root before the
		// first movement-component sweep can observe an inclined cylinder.
		AlignPresentationToVelocity(ProjectileMovement->Velocity);
	}
}

void AWoTSeekingProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTSeekingProjectile, Acceleration);
	DOREPLIFETIME(AWoTSeekingProjectile, Destination);
	DOREPLIFETIME(AWoTSeekingProjectile, bSeeking);
	DOREPLIFETIME(AWoTSeekingProjectile, bDestExists);
	DOREPLIFETIME(AWoTSeekingProjectile, bServerReachedDest);
	DOREPLIFETIME(AWoTSeekingProjectile, DestLoc);
}

void AWoTSeekingProjectile::SetDestination(AActor* Dest)
{
	if (!HasAuthority())
	{
		return;
	}
	if (bNotifiesDestination)
	{
		if (AWoTCharacter* PreviousCharacter = Cast<AWoTCharacter>(Destination))
		{
			PreviousCharacter->DecrementSeekerCount();
		}
	}
	Destination = Dest;
	if (bNotifiesDestination)
	{
		if (AWoTCharacter* NewCharacter = Cast<AWoTCharacter>(Destination))
		{
			NewCharacter->IncrementSeekerCount();
			// G16/HUD-D7 (WOTPlayer.IncrementSeekerCount, uc:2996-3008): the FIRST
			// seeker on a player warns the owning client with WOT.SeekerNotification.
			// Retail's ==1 check lives inside the player's increment; the port's
			// mutator is Mac-lane, so the equivalent check sits at the only call
			// site that just made the count 1. The controller cast gates to players
			// (retail's method exists on WOTPlayer alone).
			if (NewCharacter->SeekerCount == 1)
			{
				if (AWoTPlayerController* TargetController =
					Cast<AWoTPlayerController>(NewCharacter->GetController()))
				{
					TargetController->ClientPlayUISound(
						FName(TEXT("WOT/SeekerNotification")), 1.0f);
				}
			}
		}
	}
	if (Destination)
	{
		DestLoc = Destination->GetActorLocation();
		EnterSeeking();
	}
	else
	{
		ExitSeeking();
	}
}

void AWoTSeekingProjectile::NotifySeePawn(AActor* SeenPawn)
{
	// Minimal SeePlayer seam for G31. The existing launcher acquisition path is
	// routed here too, so later G18 specialization has one virtual entry point.
	if (HasAuthority())
	{
		SetDestination(SeenPawn);
	}
}

void AWoTSeekingProjectile::Destroyed()
{
	if (HasAuthority() && bNotifiesDestination)
	{
		if (AWoTCharacter* DestinationCharacter = Cast<AWoTCharacter>(Destination))
		{
			DestinationCharacter->DecrementSeekerCount();
		}
	}
	Super::Destroyed();
}

void AWoTSeekingProjectile::EnterSeeking()
{
	bSeeking = true;
	bWasSeeking = true;
	CurrentSpeed = Speed;
	ApplySeekingCollisionState();
}

void AWoTSeekingProjectile::ExitSeeking()
{
	bSeeking = false;
	ApplySeekingCollisionState();
}

void AWoTSeekingProjectile::ApplySeekingCollisionState()
{
	if (UPrimitiveComponent* ActiveCollision = GetActiveSweptCollision())
	{
		// Seeking sets bCollideWorld = bCollideWorldWhileSeeking (SeekingProjectile.uc:389),
		// and UE1 lets a brush (a mover) block only an actor with bCollideWorld
		// (AActor::IsBlockedBy). So a seeking Seeker ignores movers as
		// well as level geometry. Blocking on ECC_WorldDynamic pinned a Seeker under the
		// Battle2_03 portcullis it was cast beside (gate rounds 10 and 12).
		const ECollisionResponse WorldResponse =
			bSeeking && !bCollideWorldWhileSeeking ? ECR_Ignore : ECR_Block;
		ActiveCollision->SetCollisionResponseToChannel(ECC_WorldStatic, WorldResponse);
		ActiveCollision->SetCollisionResponseToChannel(ECC_WorldDynamic, WorldResponse);
	}
}

void AWoTSeekingProjectile::OnRep_Seeking()
{
	ApplySeekingCollisionState();
}

bool AWoTSeekingProjectile::PreTeleport(const AWoTTeleporter* Teleporter) const
{
	return !bSeeking || !IsValid(Teleporter) || !IsValid(NavPointNode)
		|| !NavPointNode->IsTeleporter()
		|| !NavPointNode->GetActorLocation().Equals(
			Teleporter->GetActorLocation(), 2.0f);
}

void AWoTSeekingProjectile::DestinationLost()
{
	SetDestination(nullptr);
}

void AWoTSeekingProjectile::NotifyReachedDestination()
{
	const FVector Loc = Destination ? Destination->GetActorLocation() : DestLoc;
	Explode(GetActorLocation(), (GetActorLocation() - Loc).GetSafeNormal());
}

void AWoTSeekingProjectile::CannotReachDestination()
{
	// bee-line fallback, verbatim
	const FVector Loc = Destination ? Destination->GetActorLocation() : DestLoc;
	ProjectileMovement->Velocity =
		CurrentSpeed * (Loc - GetActorLocation()).GetSafeNormal();
}

void AWoTSeekingProjectile::NotifySeekerBounced(AWoTPathNode*)
{
	// SeekingProjectile.uc:337-351: a seeker trapped on one navigation point
	// self-destructs. Tracer supplies the one shipped exception.
	Destroy();
}

bool AWoTSeekingProjectile::CanDirectlyReachDestination(const FVector& Dest) const
{
	// SeekingProjectile.uc:644-657: Trace(..., bTraceActors=false), i.e.
	// TRACE_VisBlocking (level geometry and movers). The destination pawn's own
	// capsule and the other seekers in the train must not block it.
	return !WoTVisBlockingTraceHits(GetWorld(), GetActorLocation(), Dest, this);
}

bool AWoTSeekingProjectile::CanReachDestination(const FVector& Dest)
{
	// SeekingProjectile.uc:663-681: the direct world-only trace always wins;
	// the selected backend is an oracle, never a second layered fallback.
	if (CanDirectlyReachDestination(Dest))
	{
		return true;
	}
	if (WoTSeekNavigationUsesPathNodes())
	{
		return PathNodeIterator.BuildPath(
			GetWorld(), GetActorLocation(), Dest, bDesperateSeeker);
	}

	UNavigationSystemV1* Nav =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	UNavigationPath* Path = Nav
		? Nav->FindPathToLocationSynchronously(GetWorld(), GetActorLocation(), Dest)
		: nullptr;
	return Path && Path->IsValid() && !Path->IsPartial();
}

bool AWoTSeekingProjectile::DestinationExists()
{
	if (HasAuthority())
	{
		// G16/HUD-D8 (SeekingProjectile.uc:688-715): gone, dead, or respawned. The
		// retail NumDeaths snapshot (uc:260 acquire / uc:700 poll) guarded UE1's
		// REUSED pawn actor across respawn; the port spawns a fresh pawn per
		// respawn, so actor identity subsumes the snapshot — the dead-window check
		// (uc:705 Health>0) is the live remainder.
		if (const AWoTCharacter* Char = Cast<AWoTCharacter>(Destination))
		{
			bDestExists = IsValid(Destination) && Char->IsAlive();
		}
		else
		{
			bDestExists = IsValid(Destination);
		}
	}
	return bDestExists;
}

bool AWoTSeekingProjectile::ReachedDestination()
{
	if (HasAuthority())
	{
		bServerReachedDest =
			Destination && FVector::Dist(Destination->GetActorLocation(),
			                             GetActorLocation()) < ReachTolerance;
	}
	return bServerReachedDest;
}

void AWoTSeekingProjectile::CalcNextDirection(bool bDesperate)
{
	if (bDisableCalcNextDirection)
	{
		return;
	}
	const FVector Loc = Destination ? Destination->GetActorLocation() : DestLoc;
	if (WoTSeekNavigationUsesPathNodes())
	{
		// SeekingProjectile.uc:491-494: a forced re-plan starts with no old
		// restriction; the new desperate cycle may establish one below.
		if (bDesperate)
		{
			RestrictedPoint = nullptr;
		}
		if (CanDirectlyReachDestination(Loc))
		{
			ProjectileMovement->Velocity =
				Speed * (Loc - GetActorLocation()).GetSafeNormal();
			NavPointNode = nullptr;
			bHasNavPoint = false;
			return;
		}
		if (!PathNodeIterator.BuildPath(
			GetWorld(), GetActorLocation(), Loc, bDesperateSeeker))
		{
			CannotReachDestination();
			return;
		}

		AWoTPathNode* SelectedPoint = nullptr;
		if (WoTIsPointOutsideWorld(GetWorld(), GetActorLocation()))
		{
			SelectedPoint = PathNodeIterator.GetFirst();
		}
		else if (bTakeShortcuts)
		{
			SelectedPoint = PathNodeIterator.GetLast();
			while (SelectedPoint
				&& !CanDirectlyReachDestination(SelectedPoint->GetActorLocation()))
			{
				SelectedPoint = PathNodeIterator.GetPrevious();
			}
		}
		else
		{
			SelectedPoint = PathNodeIterator.GetFirst();
			AWoTPathNode* NextPoint = PathNodeIterator.GetNext();
			if (NextPoint
				&& CanDirectlyReachDestination(NextPoint->GetActorLocation()))
			{
				SelectedPoint = NextPoint;
			}
		}

		// SeekingProjectile.uc:562-585: teleporters bypass the anti-bounce
		// restriction because the apparent discontinuity is intentional.
		if (!SelectedPoint || !SelectedPoint->IsTeleporter())
		{
			if (SelectedPoint == RestrictedPoint)
			{
				SelectedPoint = LastPoint;
			}
			if (SelectedPoint == LastPoint && bDesperate)
			{
				RestrictedPoint = LastPoint;
				PathNodeIterator.GetFirst();
				SelectedPoint = PathNodeIterator.GetNext();
				if (!SelectedPoint)
				{
					NotifySeekerBounced(RestrictedPoint);
					return;
				}
			}
		}

		if (!SelectedPoint)
		{
			// The successful retail selection guarantees a node. Keep malformed
			// custom graphs safe while retaining the shipped bee-line response.
			CannotReachDestination();
			return;
		}
		NavPointNode = SelectedPoint;
		NavPoint = SelectedPoint->GetActorLocation();
		bHasNavPoint = true;
		ProjectileMovement->Velocity =
			Speed * (NavPoint - GetActorLocation()).GetSafeNormal();
		LastPoint = SelectedPoint;
		PathCheckTimer += PathCheckResolution;
		return;
	}

	// The retained NavMesh A/B arm is the pre-G41 implementation verbatim.
	if (CanDirectlyReachDestination(Loc))
	{
		ProjectileMovement->Velocity =
			CurrentSpeed * (Loc - GetActorLocation()).GetSafeNormal();
		NavPointNode = nullptr;
		bHasNavPoint = false;
		return;
	}
	// Blocked NavMesh A/B arm: preserve the pre-G41 synchronous query.
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	UNavigationPath* Path =
		Nav ? Nav->FindPathToLocationSynchronously(GetWorld(), GetActorLocation(), Loc)
		    : nullptr;
	if (!Path || Path->PathPoints.Num() < 2)
	{
		CannotReachDestination();
		return;
	}
	// bTakeShortcuts: aim at the farthest directly-reachable path point;
	// otherwise the next one.
	int32 Target = 1;
	if (bTakeShortcuts)
	{
		for (int32 i = Path->PathPoints.Num() - 1; i >= 1; i--)
		{
			if (CanDirectlyReachDestination(Path->PathPoints[i]))
			{
				Target = i;
				break;
			}
		}
	}
	NavPoint = Path->PathPoints[Target];
	NavPointNode = nullptr;
	bHasNavPoint = true;
	ProjectileMovement->Velocity =
		CurrentSpeed * (NavPoint - GetActorLocation()).GetSafeNormal();
	PathCheckTimer += PathCheckResolution;
}

void AWoTSeekingProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Destination)
	{
		DestLoc = Destination->GetActorLocation();
	}
	if (!HasAuthority() || !bSeeking)
	{
		if (bAlignToVelocity)
		{
			// Persistent simulated proxies do not run projectile simulation; their
			// component can retain the class's tiny initialization vector. The
			// authoritative replicated-movement velocity is the flight direction.
			FVector FlightVelocity = GetLocalRole() == ROLE_SimulatedProxy
				? GetReplicatedMovement().LinearVelocity : GetVelocity();
			if (FlightVelocity.IsNearlyZero() && ProjectileMovement)
			{
				FlightVelocity = ProjectileMovement->Velocity;
			}
			if (FlightVelocity.IsNearlyZero())
			{
				FlightVelocity = GetReplicatedMovement().LinearVelocity;
			}
			AlignPresentationToVelocity(FlightVelocity);
		}
		return;
	}
	// state Seeking, Tick():
	if (DestinationExists())
	{
		if (!ReachedDestination())
		{
			CurrentSpeed = FMath::Min(CurrentSpeed + Acceleration * DeltaTime,
			                          MaxSpeed);
			if (WoTSeekNavigationUsesPathNodes())
			{
				Speed = CurrentSpeed;
			}
			ProjectileMovement->Velocity =
				ProjectileMovement->Velocity.GetSafeNormal() * CurrentSpeed;
			const FVector ActiveNavPoint =
				WoTSeekNavigationUsesPathNodes() && IsValid(NavPointNode)
					? NavPointNode->GetActorLocation() : NavPoint;
			if (bHasNavPoint &&
			    !ProjectileMovement->Velocity.GetSafeNormal().Equals(
			        (ActiveNavPoint - GetActorLocation()).GetSafeNormal(), 0.01))
			{
				PathCheckTimer = 0.0f;
				CalcNextDirection(true);
			}
			else if (PathCheckTimer > 0.0f)
			{
				PathCheckTimer -= DeltaTime;
			}
			else
			{
				CalcNextDirection();
			}
			if (bAlignToVelocity)
			{
				AlignPresentationToVelocity(ProjectileMovement->Velocity);
			}
		}
		else
		{
			NotifyReachedDestination();
		}
	}
	else
	{
		DestinationLost();
	}
}

void AWoTSeekingProjectile::ProcessTouch(AActor* Other, FVector HitLocation)
{
	if (bSeeking)
	{
		// state Seeking: only the destination triggers us
		if (Other == Destination)
		{
			Super::ProcessTouch(Other, HitLocation);
		}
		return;
	}
	// global: pawn touches pass through only when not/never seeking
	if (!Cast<APawn>(Other) || Destination || !bWasSeeking)
	{
		Super::ProcessTouch(Other, HitLocation);
	}
}

void AWoTSeekingProjectile::NotifyHit(UPrimitiveComponent* MyComp, AActor* Wall,
                                      UPrimitiveComponent* OtherComp, bool bSelfMoved,
                                      FVector HitLocation, FVector HitNormal,
                                      FVector NormalImpulse, const FHitResult& Hit)
{
	if (bSeeking)
	{
		return;   // state Seeking: HitWall() {}
	}
	Super::NotifyHit(MyComp, Wall, OtherComp, bSelfMoved, HitLocation, HitNormal,
	                 NormalImpulse, Hit);
}
