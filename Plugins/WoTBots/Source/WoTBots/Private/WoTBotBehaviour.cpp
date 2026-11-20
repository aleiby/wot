#include "WoTBotController.h"
#include "Testing/WoTSoakEvents.h"
#include "WoTBotFactionProfile.h"
#include "WoTBotSettings.h"
#include "WoTBotMetrics.h"
#include "WoTBotRoster.h"
#include "WoTBotDecision.h"
#include "AI/WoTAIUtil.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTPathNodeIterator.h"
#include "AI/WoTSight.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealInvHeal.h"
#include "Inventory/WoTAngrealSpawnPool.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTResourceSpawnerInv.h"
#include "Projectiles/WoTGenericProjectile.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshPath.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "WoTRandom.h"

// Where a pool's route ends: its projection onto the bot mesh within this box.
static const FVector PoolExtent(80, 80, 160);

bool AWoTBotController::CanAct() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	return HasAuthority() && !bFrozen && !bRetiring && !bIdle && Body && Body->IsAlive();
}

bool AWoTBotController::IsHostile(const AWoTCharacter* Candidate) const
{
	const AWoTPlayerState* Self = GetPlayerState<AWoTPlayerState>();
	return IsValid(Candidate) && Candidate != GetPawn() && Candidate->IsAlive()
		&& Self && WoTAIUtil::ApparentTeamOf(Candidate) != Self->Team;
}

bool AWoTBotController::CanSee(const AWoTCharacter* Candidate) const
{
	const APawn* Body = GetPawn();
	if (!Body || !IsHostile(Candidate)) return false;
	const UWoTBotSettings* Settings = GetDefault<UWoTBotSettings>();
	if (FVector::DistSquared(Body->GetActorLocation(), Candidate->GetActorLocation())
		> FMath::Square(Settings->SightRange)) return false;
	const float Cone = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Settings->SightConeDegrees, 1.0f, 359.0f) * .5f));
	if (!WoTSight::PawnCanSeeActor(Body, Candidate, Cone)) return false;
	float Radius, HalfHeight;
	Candidate->GetSimpleCollisionCylinder(Radius, HalfHeight);
	return !WoTVisBlockingTraceHits(GetWorld(), Body->GetPawnViewLocation(),
		Candidate->GetActorLocation() + FVector(0, 0, HalfHeight * .8f), Body);
}

void AWoTBotController::SetTarget(AWoTCharacter* NewTarget, bool bVisible)
{
	if (Target.Get() != NewTarget || bTargetVisible != bVisible)
	{
		ObserveCastUse();
		if (Caster.IsValid()) { Caster->Release(); Caster->ResetAdaptation(); }
		if (!CanAct()) return; // Releasing an artifact can kill or unpossess its owner.
		AimingItem.Reset();
		if (NewTarget && bVisible)
		{
			VisibleSince = GetWorld()->GetTimeSeconds();
			const float Angle = WoTRandom::FRand() * 2.0f * PI;
			AimDisk = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * FMath::Sqrt(WoTRandom::FRand());
		}
		NextRepathTime = 0.0f;
	}
	if (Target.Get() != NewTarget)
	{
		if (Target.IsValid()) Target->OnDamageApplied.RemoveAll(this);
		if (NewTarget) NewTarget->OnDamageApplied.AddUObject(this, &ThisClass::DamageApplied);
		LastCombatProgress = GetWorld()->GetTimeSeconds();
		LastSeenTargetHealth = 1.0f; // An unseen attacker supplies no health observation.
	}
	Target = NewTarget;
	bTargetVisible = NewTarget && bVisible;
	if (bTargetVisible)
	{
		LastKnownPosition = NewTarget->GetActorLocation();
		LastSensedTime = GetWorld()->GetTimeSeconds();
		LastCombatProgress = LastSensedTime;
		// Opponents do not expose an exact health meter to this player.
		LastSeenTargetHealth = 1.0f;
	}
}

void AWoTBotController::BodyMovementUpdated(float, FVector, FVector)
{
	if (GetPawn()) FeltVelocity = GetPawn()->GetVelocity();
}

void AWoTBotController::DamageApplied(AWoTCharacter* Victim, AController* InstigatorController,
	AActor*, float Amount)
{
	if (!CanAct() || Amount <= 0.0f) return;
	if (Victim == GetPawn() || (InstigatorController == this && CanSee(Victim))) LastCombatProgress = GetWorld()->GetTimeSeconds();
	if (Victim != GetPawn()) return;
	LastDamagedTime = GetWorld()->GetTimeSeconds();
	// Damage has already applied its impulse to our movement. The change since
	// movement's last update (or last hit) is felt by this body. Movement updates
	// refresh this baseline even when they occur between hits in the same frame.
	const auto* Movement = Victim->GetCharacterMovement();
	const FVector HitDirection = (FeltVelocity - Movement->Velocity).GetSafeNormal2D();
	FeltVelocity = Movement->Velocity;
	if (!DamageSightCandidate.IsValid() && bTargetVisible)
	{ DamageSightCandidate = Target; DamageSightSince = VisibleSince; }
	// With no directional impulse, keep looking locally rather than inventing a
	// direction from damage credit. Only subsequent sight can identify an enemy.
	DamageSearchDirection = HitDirection.IsNearlyZero() ? GetControlRotation().Vector() : HitDirection;
	DamageSearchUntil = GetWorld()->GetTimeSeconds() + FMath::Max(1.5f, GetDefault<UWoTBotSettings>()->ReactionSeconds + .5f);
	UpdateAim();
	// An independently acquired enemy can remain in view through the turn.
	// Do not interrupt its existing cast or restart continuous sight in that case.
	if (bTargetVisible && !CanSee(Target.Get())) SetTarget(Target.Get(), false);
	if (!CanAct()) return;
	if (!CanSee(DamageSightCandidate.Get())) DamageSightCandidate.Reset();
}

void AWoTBotController::Perceive()
{
	const float Now = GetWorld()->GetTimeSeconds();
	// Death is a visual observation too, independent of hostility eligibility.
	if (Target.IsValid() && CanSeeStimulus(Target.Get()) && !Target->IsAlive())
	{
		SetTarget(nullptr, false);
		if (!CanAct()) return;
	}
	const float Memory = FMath::Max(0.0f, GetDefault<UWoTBotSettings>()->TargetMemorySeconds);
	AWoTCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* Candidate = *It;
		if (!CanSee(Candidate)) continue;
		float Score = FVector::Dist(GetPawn()->GetActorLocation(), Candidate->GetActorLocation());
		if (Candidate == Target.Get()) Score *= .8f;
		if (Score < BestScore) { BestScore = Score; Best = Candidate; }
	}
	float ReactedSince = -1;
	if (Best && (Now < DamageSearchUntil || DamageSightCandidate.IsValid()))
	{
		if (DamageSightCandidate.Get() != Best) { DamageSightCandidate = Best; DamageSightSince = Now; }
		if (Now - DamageSightSince < GetDefault<UWoTBotSettings>()->ReactionSeconds)
		{
			// Waiting to identify a new enemy must not freeze the old observation.
			// Refresh real sight, or retain only unexpired memory of the old target.
			const bool bStillVisible = CanSee(Target.Get());
			SetTarget(bStillVisible || (Target.IsValid() && Now - LastSensedTime < Memory)
				? Target.Get() : nullptr, bStillVisible);
			if (!CanAct()) return;
			return;
		}
		ReactedSince = DamageSightSince;
		DamageSearchUntil = -1;
		DamageSightCandidate.Reset();
	}
	if (!Best) DamageSightCandidate.Reset();
	if (Best)
	{
		SetTarget(Best, true);
		if (!CanAct()) return;
		if (ReactedSince >= 0) VisibleSince = ReactedSince;
	}
	else if (!Target.IsValid() || Now - LastSensedTime >= Memory)
		SetTarget(nullptr, false);
	else SetTarget(Target.Get(), false);
}

void AWoTBotController::Think()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WoTBotThink);
	WOT_BOT_METRICS_SCOPE();
	if (!CanAct()) return;
	++ThinkCount;
	ObserveCastUse();
	InventoryPulse();
	Perceive();
	if (!CanAct()) return;
	UpdateAim();
	UpdateCastCompletion();
	if (!CanAct()) return;
	RememberNearbyPools();
	const float Now = GetWorld()->GetTimeSeconds();
	if (State == EWoTBotState::Engage && !bTargetVisible
		&& Now - LastCombatProgress >= WoTBotDecision::NoProgressSeconds)
	{
		SetTarget(nullptr, false);
		if (!CanAct()) return;
	}
	// A body in the air has no path to anywhere: searching from it fails for every
	// pool. Keep the goal until it lands, still firing at what it sees. A body a
	// whirlwind holds is in the air too, and cannot walk until it is let go.
	if (CastChecked<AWoTCharacter>(GetPawn())->GetCharacterMovement()->IsFalling() || IsHeldByWhirlwind())
	{
		CastArtifacts();
		return;
	}
	float CollectScore = 0;
	AWoTAngrealSpawnPool* Pool = BestPool(CollectScore);
	const bool CollectingItem = Pool != nullptr;
	if (!Pool)
	{
		// Running the map stays worthwhile when every reachable item is full or taken.
		Pool = BestPool(CollectScore, true);
		CollectScore *= WoTBotDecision::RoamWeight;
	}
	const bool Threat = Target.IsValid() && (Now - LastDamagedTime < GetDefault<UWoTBotSettings>()->TargetMemorySeconds
		|| (bTargetVisible && FVector::DistSquared(GetPawn()->GetActorLocation(), LastKnownPosition)
			<= FMath::Square(FMath::Min(WoTBotDecision::CloseRange, GetDefault<UWoTBotSettings>()->EngageWhileSeekingRange))));
	const bool Fight = WoTBotDecision::ChooseFight(CollectScore, FightScore(),
		State == EWoTBotState::Engage, State == EWoTBotState::SeekItem, Threat);
	const EWoTBotState NextState = Fight ? EWoTBotState::Engage : CollectingItem ? EWoTBotState::SeekItem : EWoTBotState::Roam;
	if (NextState == EWoTBotState::Engage) Pool = nullptr;
	if (NextState != State || SeekingPool.Get() != Pool) NextRepathTime = 0.0f;
	if (NextState != State && Caster.IsValid())
	{
		Caster->Release();
		Caster->ResetAdaptation();
		if (!CanAct()) return;
	}
	State = NextState;
	// Jumps made for an earlier goal do not count against a new one.
	if (SeekingPool.Get() != Pool) { HopPool.Reset(); HopCount = 0; }
	SeekingPool = Pool;
	if (Roster.IsValid()) Roster->ClaimPool(this, Pool);
	CastArtifacts();
	if (!CanAct()) return;
	ChooseDestination();
}

void AWoTBotController::ReleasePoolGoal()
{
	if (Roster.IsValid()) Roster->ReleasePool(this);
	SeekingPool.Reset();
	HopPool.Reset();
	HopCount = 0;
}

void AWoTBotController::RememberNearbyPools()
{
	PerceivePools();
	const float Now = GetWorld()->GetTimeSeconds();
	for (auto It = PoolVisits.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || Now - It.Value() >= WoTBotDecision::VisitMemory) It.RemoveCurrent();
	for (auto It = RecoveryPools.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || Now - It.Value() >= WoTBotDecision::VisitMemory) It.RemoveCurrent();
	for (const auto& Weak : PerceptionPools)
		if (AWoTAngrealSpawnPool* Pool = Weak.Get(); Pool
			&& FVector::DistSquared(GetPawn()->GetActorLocation(), Pool->GetActorLocation()) <= FMath::Square(WoTBotDecision::ArrivalRadius))
		{
			ObservePool(Pool, true);
			if (PoolBeliefs.FindChecked(Pool).bEmpty || State == EWoTBotState::Roam)
			{
				PoolVisits.Add(Pool, Now);
				if (SeekingPool.Get() == Pool) ReleasePoolGoal();
			}
		}
	if (!SeekingPool.IsValid()) return;
	// A pool placed off the navigation mesh is reached at its projection; an item
	// still there was not collectable from it, so move on rather than idle.
	const bool bRouteEnd = !bWantsMovement && !bNavigationUnavailable
		&& FVector::Dist2D(GetPawn()->GetActorLocation(), Destination) <= 30.0f
		&& FVector::DistSquared(Destination, SeekingPool->GetActorLocation()) <= FMath::Square(200.0f);
	const bool bHopping = HopPool.Get() == SeekingPool.Get() && HopCount > 0;
	if (State == EWoTBotState::SeekItem && (bRouteEnd || bHopping))
	{
		// A pickup on a pedestal or ledge beside the route's end: jump for it, as a
		// player does, landing first between attempts.
		const AWoTCharacter* Body = CastChecked<AWoTCharacter>(GetPawn());
		if (Body->GetCharacterMovement()->IsFalling() || IsHeldByWhirlwind() || Now - HopTime < .3f) return;
		const FVector Feet = Body->GetActorLocation() - FVector(0, 0, Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		if (HopCount < WoTBotDecision::MaxHops && WoTBotDecision::PickupReach(Feet, SeekingPool->GetActorLocation(),
			SeekingPool->GetPickupCollisionRadius()) == WoTBotDecision::EPickupReach::Hop)
		{
			Hop(SeekingPool.Get());
			return;
		}
		UncollectablePools.Add(SeekingPool, Now);
	}
	if (bRouteEnd || bHopping)
	{
		PoolVisits.Add(SeekingPool, Now);
		ReleasePoolGoal();
	}
}

void AWoTBotController::Hop(const AWoTAngrealSpawnPool* Pool)
{
	AWoTCharacter* Body = CastChecked<AWoTCharacter>(GetPawn());
	if (Body->IsHardLanding()) return;
	const float Now = GetWorld()->GetTimeSeconds();
	if (HopPool.Get() != Pool) HopCount = 0;
	HopPool = Pool;
	++HopCount;
	HopTime = NextJumpTime = Now;
	// Steer through the pickup rather than to it: a move that brakes short of it
	// lands beside the pedestal. Air control carries the jump the rest of the way.
	const FVector Here = Body->GetActorLocation(), Pickup = Pool->GetActorLocation();
	const FVector Through = FVector(Pickup.X, Pickup.Y, Here.Z) + (Pickup - Here).GetSafeNormal2D() * 60.0f;
	Destination = Through;
	bWantsMovement = true;
	NextRepathTime = Now + 1.0f;
	MoveToLocation(Through, 5.0f, false, false, false, false, nullptr, true);
	Body->Jump();
	++Hops;
	LastJumpTime = Now;
}

void AWoTBotController::UpdateClimb()
{
	// The navigation mesh climbs steps of up to its voxel-rounded step height,
	// a little more than the body's own. Where a route rises into such a step
	// (out of a sunken floor, onto a pedestal), a blocked body jumps it, as the
	// original NPCs pick a wall adjustment when they walk into a wall
	// (LegendPawn.uc HitWall calls PickWallAdjust).
	AWoTCharacter* Body = CastChecked<AWoTCharacter>(GetPawn());
	const UCharacterMovementComponent* Move = Body->GetCharacterMovement();
	const float Now = GetWorld()->GetTimeSeconds();
	// Any pause in the walk restarts the wait before a climb jump.
	if (!bWantsMovement || Now < NextJumpTime || !Move->IsMovingOnGround() || Body->IsHardLanding())
	{
		ClimbBlockedSince = -1.0f;
		return;
	}
	const UPathFollowingComponent* Follow = GetPathFollowingComponent();
	if (!Follow || Follow->GetStatus() != EPathFollowingStatus::Moving)
	{
		ClimbBlockedSince = -1.0f;
		return;
	}
	const UCapsuleComponent* Capsule = Body->GetCapsuleComponent();
	const FVector Here = Body->GetActorLocation();
	const float FeetZ = Here.Z - Capsule->GetScaledCapsuleHalfHeight();
	const FVector RoutePoint = Follow->GetCurrentTargetLocation();
	const FVector Direction = (RoutePoint - Here).GetSafeNormal2D();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTBotClimb), false, Body);
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
	// A link that does not descend (the map's own reach across a gap): leave the
	// ground at the gap's edge rather than walk into it.
	if (const FNavPathSharedPtr Path = Follow->GetPath(); Path.IsValid() && !Direction.IsNearlyZero())
	{
		const int32 Index = Follow->GetCurrentPathIndex();
		const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
		FHitResult Ground;
		const FVector Ahead = FVector(Here.X, Here.Y, FeetZ) + Direction * (Capsule->GetScaledCapsuleRadius() + 15.0f);
		if (Points.IsValidIndex(Index + 1) && FNavMeshNodeFlags(Points[Index].Flags).IsNavLink()
			&& Points[Index + 1].Location.Z - Points[Index].Location.Z > -WoTBotDecision::ClimbClearance
			&& !GetWorld()->LineTraceSingleByChannel(Ground, Ahead + FVector(0, 0, 10.0f), Ahead - FVector(0, 0, 2.0f * WoTBotDecision::ClimbClearance), Channel, Params))
		{
			NextJumpTime = Now + .8f;
			Body->Jump();
			++ClimbJumps;
			LastJumpTime = Now;
			return;
		}
	}
	// Held back while pressing on: a body that keeps its pace needs no jump.
	if (Direction.IsNearlyZero() || FVector::DotProduct(Body->GetVelocity(), Direction) > .5f * Move->MaxWalkSpeed)
	{
		ClimbBlockedSince = -1.0f;
		return;
	}
	if (ClimbBlockedSince < 0.0f) ClimbBlockedSince = Now;
	if (Now - ClimbBlockedSince < WoTBotDecision::ClimbBlockedSeconds) return;
	// The route rises, or turns just past the obstacle (onto a pedestal, over a
	// sill the mesh steps across): not a wall the route merely skirts.
	if (RoutePoint.Z - FeetZ < WoTBotDecision::ClimbRise
		&& FVector::Dist2D(RoutePoint, Here) > WoTBotDecision::ClimbReach) return;
	const FCollisionShape Shape = Capsule->GetCollisionShape();
	FHitResult Wall;
	if (!GetWorld()->SweepSingleByChannel(Wall, Here, Here + Direction * 24.0f, FQuat::Identity, Channel, Shape, Params)
		|| Wall.ImpactNormal.Z >= Move->GetWalkableFloorZ() || Cast<APawn>(Wall.GetActor())) return;
	// A ledge just ahead, above what the walk steps and below the jump's reach.
	FHitResult Ledge;
	const FVector Probe = FVector(Here.X, Here.Y, FeetZ) + Direction * (Capsule->GetScaledCapsuleRadius() + 12.0f);
	if (!GetWorld()->LineTraceSingleByChannel(Ledge, Probe + FVector(0, 0, WoTBotDecision::ClimbClearance + 5.0f),
		Probe - FVector(0, 0, 2.0f), Channel, Params) || Ledge.bStartPenetrating
		|| Ledge.ImpactPoint.Z - FeetZ <= WoTBotDecision::ClimbStep || Ledge.ImpactNormal.Z < Move->GetWalkableFloorZ()) return;
	const FVector Raised = Here + FVector(0, 0, WoTBotDecision::ClimbClearance);
	FHitResult Clear;
	if (GetWorld()->SweepSingleByChannel(Clear, Here, Raised, FQuat::Identity, Channel, Shape, Params)
		|| GetWorld()->SweepSingleByChannel(Clear, Raised, Raised + Direction * 24.0f, FQuat::Identity, Channel, Shape, Params)) return;
	ClimbBlockedSince = -1.0f;
	NextJumpTime = Now + .8f;
	Body->Jump();
	++ClimbJumps;
	LastJumpTime = Now;
}

void AWoTBotController::OffensiveSupply(int32& Kinds, int32& Charges) const
{
	Kinds = Charges = 0;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	TSet<UClass*> Classes;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
		if (const AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held);
			Item && Item->bOffensive && Item->GetClass() != Body->DefaultAngrealInventory && Item->HaveEnoughCharges())
		{
			Classes.Add(Item->GetClass());
			Charges += Item->CurCharges / FMath::Max(1, Item->ChargeCost);
		}
	Kinds = Classes.Num();
}

float AWoTBotController::FightScore() const
{
	if (!Target.IsValid() || (!MovementOffence() && !ReadySummon())) return 0;
	const AWoTCharacter* Body = CastChecked<AWoTCharacter>(GetPawn());
	int32 Kinds, Charges;
	OffensiveSupply(Kinds, Charges);
	const float Distance = FVector::Dist(Body->GetActorLocation(), LastKnownPosition);
	// Recent pain increases urgency without attributing the hit to this target.
	return WoTBotDecision::Fight(Distance, bTargetVisible,
		GetWorld()->GetTimeSeconds() - LastDamagedTime < GetDefault<UWoTBotSettings>()->TargetMemorySeconds,
		FMath::Clamp(float(Body->Health) / FMath::Max(1, Body->MaxHealth), 0.0f, 1.0f), Charges,
		LastSeenTargetHealth, 0,
		bTargetVisible && Distance <= FMath::Min(WoTBotDecision::CloseRange, GetDefault<UWoTBotSettings>()->EngageWhileSeekingRange));
}

AWoTAngrealSpawnPool* AWoTBotController::BestPool(float& Score, bool bRoaming)
{
	Score = 0;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) return nullptr;
	const float Now = GetWorld()->GetTimeSeconds();
	const FVector Start = Body->GetNavAgentLocation();
	int32 Kinds, Charges;
	OffensiveSupply(Kinds, Charges);
	struct FCandidate
	{
		AWoTAngrealSpawnPool* Pool = nullptr;
		FVector End = FVector::ZeroVector;
		float Utility = 0, Rotation = 1, Bound = 0;
	};
	const auto Value = [&](const FCandidate& C, float Length)
	{
		return bRoaming ? WoTBotDecision::Roam(Length, C.Rotation, false)
			: C.Utility / (1.0f + Length / 1600.0f) * C.Rotation;
	};
	TArray<FCandidate> Candidates;
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It)
	{
		const float* Visit = PoolVisits.Find(*It);
		const float* Recovery = RecoveryPools.Find(*It);
		if (Visit && Now - *Visit < WoTBotDecision::RevisitBlockSeconds) continue;
		if (Recovery && Now - *Recovery < WoTBotDecision::RevisitBlockSeconds) continue;
		if (const float* Failed = UnreachablePools.Find(*It); Failed && Now - *Failed < WoTBotDecision::UnreachableRetrySeconds) continue;
		if (PoolStockProbability(*It) <= 0) continue;
		const float Utility = PoolUtility(*It, Kinds);
		if (!bRoaming && Utility <= 0 && *It != SeekingPool.Get()) continue;
		FNavLocation End;
		if (!Nav->ProjectPointToNavigation(It->GetActorLocation(), End, PoolExtent, Data)) continue;
		if (!bRoaming)
		{
			// Collecting needs a touch from the route's end, standing or jumping.
			if (const float* Failed = UncollectablePools.Find(*It); Failed && Now - *Failed < WoTBotDecision::UncollectableSeconds) continue;
			if (WoTBotDecision::PickupReach(End.Location, It->GetActorLocation(), It->GetPickupCollisionRadius())
				== WoTBotDecision::EPickupReach::None) continue;
		}
		FCandidate& C = Candidates.AddDefaulted_GetRef();
		C.Pool = *It;
		C.End = End.Location;
		C.Rotation = WoTBotDecision::Rotation(FMath::Min(
			Visit ? Now - *Visit : WoTBotDecision::VisitMemory,
			Recovery ? Now - *Recovery : WoTBotDecision::VisitMemory));
		C.Utility = Utility;
		// Roaming measures "far" in a straight line. A path is never shorter than
		// that line (less a little start projection slack), so a collection value
		// at the line's length bounds the value at the path's length from above.
		const float Line = FVector::Dist(Start, C.End);
		C.Bound = Value(C, bRoaming ? Line : FMath::Max(0.0f, Line - 100.0f));
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Bound > B.Bound; });
	TOptional<bool> bStartOnNavigation;
	TArray<float> EntryLegs;
	const auto Reach = [&](const FCandidate& C, float& Length)
	{
		FPathFindingQuery Query(this, *Data, Start, C.End);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Path = Nav->FindPathSync(Body->GetNavAgentPropertiesRef(), Query);
		if (Path.IsSuccessful() && Path.Path.IsValid()) { Length = Path.Path->GetLength(); return true; }
		// Through a teleporter no mesh link carries.
		if (DistantTeleportRoute(Start, C.End, Length, &EntryLegs)) return true;
		// Don't search for it again every think, unless the start was the problem
		// (a body in the air has no path to anywhere).
		if (!bStartOnNavigation.IsSet())
		{
			FNavLocation Projected;
			bStartOnNavigation = Nav->ProjectPointToNavigation(Start, Projected, INVALID_NAVEXTENT, Data);
		}
		if (bStartOnNavigation.GetValue()) UnreachablePools.Add(C.Pool, Now);
		return false;
	};
	const FCandidate* Current = Candidates.FindByPredicate([this](const FCandidate& C) { return C.Pool == SeekingPool.Get(); });
	if (bRoaming && Current)
	{
		// Running the map keeps its pool until arrival records the visit: the pool's
		// remaining distance shrinks on the way, so re-scoring it would chase ever
		// farther pools without reaching one.
		// The best value on offer stands for running the map against a fight.
		Score = Candidates[0].Bound;
		return SeekingPool.Get();
	}
	// The current goal's exact value sets the bar a challenger must clear.
	float CurrentScore = 0;
	bool bCurrentReachable = false;
	if (Current)
	{
		float Length = 0;
		if (bRoaming) CurrentScore = Current->Bound;
		else if (Reach(*Current, Length)) { CurrentScore = Value(*Current, Length); bCurrentReachable = true; }
	}
	const float Bar = bCurrentReachable ? CurrentScore * WoTBotDecision::SwitchRatio + WoTBotDecision::SwitchMargin : 0.0f;
	AWoTAngrealSpawnPool* Best = nullptr;
	for (const FCandidate& C : Candidates)
	{
		// No remaining candidate can beat the best so far or clear the bar.
		if (C.Bound <= FMath::Max(Score, Bar)) break;
		if (C.Pool == SeekingPool.Get()) continue;
		float Length = 0;
		if (!Reach(C, Length)) continue;
		const float Candidate = bRoaming ? C.Bound : Value(C, Length);
		if (Candidate > Score) { Score = Candidate; Best = C.Pool; }
	}
	// Retain a reachable goal unless another pool is clearly worth changing route.
	if (bCurrentReachable && !WoTBotDecision::ClearlyBetter(Score, CurrentScore))
	{ Score = CurrentScore; return SeekingPool.Get(); }
	return Best;
}

bool AWoTBotController::HasLiveHelper() const
{
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
		if (It->IsAlive() && OwnedPawns.ContainsByPredicate([&It](const auto& Body)
			{ return !Body.IsExplicitlyNull() && Body.HasSameIndexAndSerialNumber(It->SummonOwner); })) return true;
	for (TActorIterator<AWoTMashadarGuide> It(GetWorld()); It; ++It)
		if (OwnedPawns.ContainsByPredicate([&It](const auto& Body)
			{ return !Body.IsExplicitlyNull() && Body.HasSameIndexAndSerialNumber(It->SummonOwner); })) return true;
	return false;
}

AWoTResourceSpawnerInv* AWoTBotController::ReadySummon() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body || HasLiveHelper()) return nullptr;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
		if (AWoTResourceSpawnerInv* Summon = Cast<AWoTResourceSpawnerInv>(Held);
			Summon && Summon->HaveEnoughCharges() && Summon->GetPriority() > Summon->PriorityUnusable) return Summon;
	return nullptr;
}

void AWoTBotController::CastItem(AWoTAngrealInventory* Item)
{
	if (!Item || !Caster.IsValid() || !CanAct()) return;
	ObserveCastUse();
	UpdateCastCompletion();
	if (!CanAct()) return;
	// Release the previous item before changing aim or selecting its replacement.
	// The release itself may apply lethal self-damage.
	if (Caster->IsCasting())
	{
		if (AimingItem.Get() == Item) return;
		Caster->Release();
		if (!CanAct()) return;
	}
	AimingItem = Item;
	ObservedUses = Item->CastUseCount;
	UpdateAim();
	if (AWoTProjectileLauncher* Launcher = Cast<AWoTProjectileLauncher>(Item);
		Launcher && Launcher->ProjectileClass)
	{
		const AWoTGenericProjectile* Projectile = Launcher->ProjectileClass.GetDefaultObject();
		if (Projectile->bUsesDestination)
		{
			// Defensive selection uses the incoming shot to score the artifact.
			// Acquire its destination from the updated view using the authored cone.
			// Bot seeking casts only lock pawns, including when player acquisition
			// offers a special shootable projectile as its best candidate.
			APawn* DestinationPawn = Cast<APawn>(Launcher->FindBestTarget(
				Launcher->GetTrajectorySource(), GetPawn()->GetViewRotation(), Projectile->MaxTargetAngle));
			Launcher->SetVictim(DestinationPawn);
			if (Projectile->bRequiresDestination && !DestinationPawn) return;
		}
	}
	CastHold.Begin(Item, GetWorld()->GetTimeSeconds());
	Caster->BeginCast(Item);
	ObserveCastUse();
}

void AWoTBotController::UpdateCastCompletion()
{
	if (!CastHold.IsTracking()) return;
	if (!Caster.IsValid()) { CastHold.Clear(); return; }
	// The shared caster still owns charge-use release. Observe that completion
	// before selecting again, retaining its normal adapted-item exclusion.
	if (!Caster->IsCasting())
	{
		CastHold.Clear();
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const bool bEffectEnded = CastHold.EffectEnded();
	const bool bBudgetExpired = CastHold.BudgetExpired(Now);
	const bool bSafetyExpired = CastHold.SafetyExpired(Now);
	if (!bEffectEnded && !bBudgetExpired && !bSafetyExpired) return;
	if (bSafetyExpired && !bEffectEnded && !bBudgetExpired)
	{
		if (const AWoTAngrealInventory* Item = CastHold.GetItem())
		{
			static TSet<FName> WarnedClasses;
			const FName ClassName = Item->GetClass()->GetFName();
			if (!WarnedClasses.Contains(ClassName))
			{
				WarnedClasses.Add(ClassName);
				UE_LOG(LogTemp, Warning, TEXT("Bot cast safety cap reached for class %s"), *ClassName.ToString());
			}
		}
	}
	// Release can fire a shot and synchronously kill/unpossess the bot. Consume
	// the hold first; callers recheck CanAct before using the pawn again.
	CastHold.Clear();
	Caster->Release();
	ObserveCastUse();
}

void AWoTBotController::ObserveCastUse()
{
	if (AimingItem.IsValid() && AimingItem->CastUseCount != ObservedUses)
	{
		ObservedUses = AimingItem->CastUseCount;
		if (AimingItem->bDefensive) LastDefensiveCastTime = GetWorld()->GetTimeSeconds();
		else LastOffensiveCastTime = GetWorld()->GetTimeSeconds();
	}
}

AWoTAngrealInventory* AWoTBotController::SelectArtifact(AActor* Victim, bool bDefensive)
{
	const auto Select = [&]()
	{
		return bDefensive ? Caster->SelectDefensiveWeave(Victim,
			[](const AWoTAngrealInventory* Item) { return !Item->IsA<AWoTAngrealInvHeal>(); })
			: Caster->SelectOffensiveWeave(Victim);
	};
	AWoTAngrealInventory* Selected = Select();
	const float LastUse = bDefensive ? LastDefensiveCastTime : LastOffensiveCastTime;
	const float Interval = bDefensive ? Caster->MinDefensiveUsageInterval : Caster->MinOffensiveUsageInterval;
	if (!Selected && !Caster->IsCasting() && AimingItem.IsValid() && AimingItem->bDefensive == bDefensive
		&& GetWorld()->GetTimeSeconds() > LastUse + Interval)
	{
		// Preserve the normal preference for another artifact. Only when that hand
		// is ready and no alternative is usable, retry without the old exclusion so
		// a single-item kit cannot lock out. Both selectors still enforce cadence.
		Caster->ResetAdaptation();
		Selected = Select();
	}
	return Selected;
}

AActor* AWoTBotController::FindVisibleIncomingProjectile() const
{
	const auto* Body = CastChecked<AWoTCharacter>(GetPawn());
	const auto* Capsule = Body->GetCapsuleComponent();
	const float RadiusSq = FMath::Square(Capsule->GetScaledCapsuleRadius() * Caster->DefensivePerimeterRadiusFactor);
	const float Height = Capsule->GetScaledCapsuleHalfHeight() * Caster->DefensivePerimeterHeightFactor;
	AActor* Best = nullptr;
	float BestDistance = TNumericLimits<float>::Max();
	for (TActorIterator<AWoTAngrealProjectile> It(GetWorld()); It; ++It)
	{
		// Perceive before ranking: a hidden near projectile cannot mask a visible
		// threat. Predict from observed motion, not a seeker's private destination.
		const FVector Delta = It->GetActorLocation() - Body->GetActorLocation();
		if (Delta.SizeSquared2D() > RadiusSq || FMath::Abs(Delta.Z) > Height) continue;
		if (!CanSeeStimulus(*It)) continue;
		const FVector Velocity = It->ProjectileMovement ? It->ProjectileMovement->Velocity : It->GetVelocity();
		const float Distance = Delta.Size(), Speed = Velocity.Size();
		if (Speed <= 0 || (Delta + Velocity.GetSafeNormal()).Size() >= Distance) continue;
		const float Arrival = Distance / Speed;
		if (Arrival < .01f || Arrival > 5.0f || Distance >= BestDistance) continue;
		Best = *It;
		BestDistance = Distance;
	}
	return Best;
}

void AWoTBotController::CastArtifacts()
{
	if (!CanAct()) return;
	AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Caster.IsValid()) return;
	if (!Body->CanCastNow()) { Caster->Release(); Caster->ResetAdaptation(); return; }
	const float Now = GetWorld()->GetTimeSeconds();
	if (AActor* Incoming = FindVisibleIncomingProjectile())
		if (AWoTAngrealInventory* Defence = SelectArtifact(Incoming, true))
		{ CastItem(Defence); return; }
	if (Body->Health < Body->MaxHealth * GetDefault<UWoTBotSettings>()->HealBelowHealth
		&& Now > LastDefensiveCastTime + Caster->MinDefensiveUsageInterval)
	{
		for (AWoTInventory* Held : Body->InventoryComponent->Items)
			if (AWoTAngrealInvHeal* Heal = Cast<AWoTAngrealInvHeal>(Held);
				Heal && Heal->HaveEnoughCharges() && Heal->GetPriority() > Heal->PriorityUnusable)
			{ Heal->SetVictim(Body); Heal->InitChargeGroup(); CastItem(Heal); return; }
	}
	if (!bTargetVisible || !Target.IsValid())
	{ Caster->Release(); Caster->ResetAdaptation(); return; }
	if (Now < VisibleSince + GetDefault<UWoTBotSettings>()->ReactionSeconds) return;
	if (Caster->IsCasting()) return;
	if (State == EWoTBotState::Engage && Now > LastOffensiveCastTime + Caster->MinOffensiveUsageInterval)
	{
		if (AWoTResourceSpawnerInv* Summon = ReadySummon())
		{ Summon->SetVictim(Target.Get()); Summon->InitChargeGroup(); CastItem(Summon); return; }
	}
	if (AWoTAngrealInventory* Attack = SelectArtifact(Target.Get(), false)) CastItem(Attack);
}

float AWoTBotController::GetAimErrorLimit() const
{
	return FMath::Max(0.0f, GetDefault<UWoTBotSettings>()->AimErrorDegrees)
		/ (1.0f + (bTargetVisible ? FMath::Max(0.0f, GetWorld()->GetTimeSeconds() - VisibleSince) : 0.0f));
}

void AWoTBotController::UpdateAim()
{
	APawn* Body = GetPawn();
	if (!Body) return;
	FVector AimPoint = bWantsMovement ? FVector(Destination.X, Destination.Y, Body->GetPawnViewLocation().Z)
		: Body->GetPawnViewLocation() + GetControlRotation().Vector() * 100;
	if (Target.IsValid()) AimPoint = LastKnownPosition;
	const bool bDamageSearch = GetWorld()->GetTimeSeconds() < DamageSearchUntil || DamageSightCandidate.IsValid();
	if (bDamageSearch) AimPoint = Body->GetPawnViewLocation() + DamageSearchDirection * 100;
	FRotator Aim = (AimPoint - Body->GetPawnViewLocation()).Rotation();
	if (!bDamageSearch && bTargetVisible && Target.IsValid())
	{
		Aim = (Target->GetActorLocation() - Body->GetPawnViewLocation()).Rotation();
		if (AWoTProjectileLauncher* Launcher = Cast<AWoTProjectileLauncher>(AimingItem.Get());
			Launcher && Launcher->bOffensive && Launcher->IsLeadable())
		{
			Launcher->SetVictim(Target.Get());
			Aim = Launcher->GetBestTrajectory();
		}
		const FVector Forward = Aim.Vector();
		const FVector Right = FRotationMatrix(Aim).GetUnitAxis(EAxis::Y);
		const FVector Up = FRotationMatrix(Aim).GetUnitAxis(EAxis::Z);
		const FVector Axis = (Right * AimDisk.X + Up * AimDisk.Y).GetSafeNormal();
		Aim = FQuat(Axis, FMath::DegreesToRadians(GetAimErrorLimit() * AimDisk.Size())).RotateVector(Forward).Rotation();
	}
	if (IsHeldByWhirlwind())
	{
		// A whirlwind turns the body it holds every tick (WhirlwindLeech.uc Tick),
		// and turns a held player's view with it (AttachTo sets bForceRotation,
		// which WOTPlayer.uc UpdateRotation adds to the view): the body is left to
		// the lift, and the view, and so every cast, turns with the body.
		Aim.Yaw = Body->GetActorRotation().Yaw;
		SetControlRotation(Aim);
		return;
	}
	SetControlRotation(Aim);
	Body->FaceRotation(FRotator(0, Aim.Yaw, 0), 0.0f);
}

bool AWoTBotController::IsHeldByWhirlwind() const
{
	// The game module's whirlwind effect class, found once by its script path (it
	// is not exported); the effect is attached to the body for as long as it holds it.
	static UClass* LeechClass = nullptr;
	if (!LeechClass) LeechClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTWhirlwindLeech"));
	const APawn* Body = GetPawn();
	if (!LeechClass || !Body) return false;
	for (const UActorComponent* Component : Body->GetComponents())
		if (IsValid(Component) && Component->IsRegistered() && Component->IsA(LeechClass)) return true;
	return false;
}

void AWoTBotController::UpdateControlRotation(float, bool)
{
	// Tick owns aim, including projectile lead; the base focal-point update
	// would replace it with a look at the current path segment.
}

AWoTAngrealInventory* AWoTBotController::MovementOffence() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body || !Body->InventoryComponent) return nullptr;
	AWoTAngrealInventory* Best = nullptr;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
	{
		AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held);
		if (!Item || !Item->bOffensive || !Item->HaveEnoughCharges() || Item->Priority <= Item->PriorityUnusable) continue;
		// Preserve an active attack's range; otherwise approach for the best charged
		// item even before it is in selector range. Temporary cadence/group waits
		// must not make a stocked bot behave as though it has no offence.
		if (Item == AimingItem.Get() && Caster.IsValid() && Caster->IsCasting()) return Item;
		if (!Best || Item->Priority > Best->Priority) Best = Item;
	}
	return Best;
}

const WoTBotLinks::FDistantTeleport* AWoTBotController::DistantTeleportRoute(const FVector& From, const FVector& To, float& Length, TArray<float>* EntryLegs) const
{
	if (!Roster.IsValid() || Roster->GetDistantTeleports().IsEmpty()) return nullptr;
	const TArray<WoTBotLinks::FDistantTeleport>& Teleports = Roster->GetDistantTeleports();
	const APawn* Body = GetPawn();
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav && Body ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) return nullptr;
	// A complete route's length, or a negative length for none.
	const auto Leg = [&](const FVector& A, const FVector& B)
	{
		FPathFindingQuery Query(this, *Data, A, B);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Path = Nav->FindPathSync(Body->GetNavAgentPropertiesRef(), Query);
		return Path.IsSuccessful() && Path.Path.IsValid() && !Path.Path->IsPartial() ? float(Path.Path->GetLength()) : -1.0f;
	};
	TArray<float> Local;
	TArray<float>& Legs = EntryLegs ? *EntryLegs : Local;
	if (Legs.Num() != Teleports.Num())
	{
		Legs.Reset();
		for (const WoTBotLinks::FDistantTeleport& T : Teleports) Legs.Add(Leg(From, T.Entry));
	}
	const WoTBotLinks::FDistantTeleport* Best = nullptr;
	for (int32 I = 0; I < Teleports.Num(); ++I)
	{
		if (Legs[I] < 0.0f || (Best && Legs[I] >= Length)) continue;
		const float Out = Leg(Teleports[I].Exit, To);
		if (Out < 0.0f || (Best && Legs[I] + Out >= Length)) continue;
		Best = &Teleports[I];
		Length = Legs[I] + Out;
	}
	return Best;
}

bool AWoTBotController::StepOffPad(const WoTBotLinks::FDistantTeleport& Via, float Reach)
{
	WOT_SOAK_BODY_SCOPE("bot pad step-off request");
	WOT_SOAK_BODY_EVENT(GetPawn(), "pad step-off request");
	const APawn* Body = GetPawn();
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) return false;
	FVector Away = (Body->GetActorLocation() - Via.Pad).GetSafeNormal2D();
	if (Away.IsNearlyZero()) Away = FVector::ForwardVector;
	// A walk ends anywhere up to its acceptance radius (20, NavigateTo) short of
	// its point. A point at least Reach + 20 out leaves the body beyond Reach
	// wherever the walk stops, and lies more than 30 from any spot on the pad, so
	// the walk stays wanted (and watched by the stuck rule) until the body is off.
	for (const float Angle : { 0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f })
	{
		const FVector Out = Via.Pad + Away.RotateAngleAxis(Angle, FVector::UpVector) * (Reach + 30.0f);
		FNavLocation Point;
		if (Nav->ProjectPointToNavigation(FVector(Out.X, Out.Y, Body->GetNavAgentLocation().Z), Point, FVector(30, 30, 100), Data)
			&& FVector::Dist2D(Point.Location, Via.Pad) >= Reach + 20.0f && NavigateTo(Point.Location)) return true;
	}
	return false;
}

float AWoTBotController::StandoffRange() const
{
	AWoTAngrealInventory* Item = MovementOffence();
	if (!Item) return 0.0f;
	float Minimum, Maximum;
	Item->GetEffectiveRange(Minimum, Maximum);
	Maximum = FMath::Min(Maximum, GetDefault<UWoTBotSettings>()->SightRange * .75f);
	return FMath::Max(40.0f, FMath::Lerp(FMath::Min(Minimum, Maximum), Maximum, .65f));
}

bool AWoTBotController::NavigateTo(const FVector& Point)
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	FNavLocation Projected;
	if (!Data || !Nav->ProjectPointToNavigation(Point, Projected, FVector(100, 100, 200), Data))
	{ bNavigationUnavailable = true; return false; }
	bNavigationUnavailable = false;
	Destination = Projected.Location;
	bWantsMovement = FVector::Dist2D(Body->GetActorLocation(), Destination) > 30.0f;
	++RepathCount;
	// A failed request (no complete route) leaves the body idle with movement
	// still wanted, so the stuck rule replaces the goal unless the caller does.
	return MoveToLocation(Destination, 20.0f, false, true, false, false, nullptr, false)
		!= EPathFollowingRequestResult::Failed;
}

void AWoTBotController::ChooseDestination(bool bForce)
{
	WOT_SOAK_BODY_SCOPE("bot route/off-mesh rescue request");
	if (!CanAct()) return;
	const float Now = GetWorld()->GetTimeSeconds();
	if (bForce && SeekingPool.IsValid())
	{
		// Recovery abandons this route too; don't immediately reclaim the same corner.
		RecoveryPools.Add(SeekingPool, Now);
		ReleasePoolGoal();
	}
	if (!bForce && Now < NextRepathTime) return;
	NextRepathTime = Now + FMath::Max(.1f, GetDefault<UWoTBotSettings>()->StrafeRepathSeconds);
	APawn* Body = GetPawn();
	if ((State == EWoTBotState::SeekItem || State == EWoTBotState::Roam) && SeekingPool.IsValid() && !bForce)
	{
		// The route ends where the choice measured it: the pool's own projection
		// (a wider search could settle on a ledge above it with no route).
		UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
		FNavLocation End;
		const bool bProjected = Data && Nav->ProjectPointToNavigation(SeekingPool->GetActorLocation(), End, PoolExtent, Data);
		if (NavigateTo(bProjected ? End.Location : SeekingPool->GetActorLocation())) return;
		// Through a teleporter no mesh link carries: walk into its pad. A pad that
		// set this body down sends it on only after the body's overlap with the pad
		// has ended (that re-arms it, WoTTeleporter.cpp NotifyActorEndOverlap), so
		// step off first; the overlap itself, not a distance, says when the body is
		// off. A body at the entry that was not taken closes the route.
		float Length;
		if (const WoTBotLinks::FDistantTeleport* Via = bProjected ? DistantTeleportRoute(Body->GetNavAgentLocation(), End.Location, Length) : nullptr)
		{
			const float Reach = Via->TouchRadius + Body->GetSimpleCollisionRadius() + 15.0f;
			const AActor* Pad = Via->Teleporter.Get();
			const bool bOnPad = Pad ? Pad->IsOverlappingActor(Body)
				: FVector::Dist2D(Body->GetActorLocation(), Via->Pad) < Via->TouchRadius + Body->GetSimpleCollisionRadius();
			if (!bOnPad) bArrivedByTeleport = false;
			if (bOnPad && bArrivedByTeleport && FVector::Dist2D(ArrivalLocation, Via->Pad) < Reach)
			{
				if (StepOffPad(*Via, Reach)) return;
			}
			else if (FVector::Dist2D(Body->GetActorLocation(), Via->Entry) > 30.0f && NavigateTo(Via->Entry)) return;
		}
		UnreachablePools.Add(SeekingPool, Now);
		ReleasePoolGoal();
	}
	if (State == EWoTBotState::Engage && !bForce)
	{
		// An unseen target is chased to where it was last sensed. When no complete
		// route leads there (another floor, a ledge, water below), a standoff point
		// around it, then the roaming fallback, replace an idle wait for recovery.
		if (!bTargetVisible && NavigateTo(LastKnownPosition)) return;
		const FVector Away = (Body->GetActorLocation() - LastKnownPosition).GetSafeNormal2D();
		if (const float Range = StandoffRange(); Range > 0.0f)
			for (int32 Attempt = 0; Attempt < 4; ++Attempt)
			{
				const float Side = WoTRandom::RandBool() ? 1.0f : -1.0f;
				const FVector Offset = Away.RotateAngleAxis(Side * WoTRandom::FRandRange(25.0f, 65.0f), FVector::UpVector);
				if (NavigateTo(LastKnownPosition + Offset * Range)) return;
			}
	}
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) { bNavigationUnavailable = true; return; }
	// Unchanged fallback: no standoff point, no reachable pool, or recovery.
	// Engage relies on it in tight rooms; the Recast query keeps its engine
	// stream, which the integration fixtures seed (SeedDecisions).
	FVector Bias = Body->GetActorLocation();
	TArray<AWoTAngrealSpawnPool*> Pools;
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It) Pools.Add(*It);
	if (!Pools.IsEmpty()) Bias = FMath::Lerp(Bias, Pools[WoTRandom::RandHelper(Pools.Num())]->GetActorLocation(), .5f);
	FNavLocation Point;
	// A point reachable from the midpoint need not be reachable from this body
	// (another floor, an island); then one reachable from the body itself.
	if (Nav->GetRandomReachablePointInRadius(Bias, bForce ? 600.0f : 1600.0f, Point, Data) && NavigateTo(Point.Location)) return;
	if (Nav->GetRandomReachablePointInRadius(Body->GetActorLocation(), 1600.0f, Point, Data) && NavigateTo(Point.Location)) return;
	WOT_SOAK_BODY_EVENT(Body, "off-mesh rescue request");
	// Off the mesh altogether (on a sill or ledge a jump or a blast left it on):
	// a projection below the feet is a landing, not a direction off the edge.
	// Find a clear horizontal departure and a walkable landing within a safe
	// drop. The ordinary movement component still performs the walk and fall.
	const AWoTCharacter* PlayerBody = CastChecked<AWoTCharacter>(Body);
	const UCapsuleComponent* Capsule = PlayerBody->GetCapsuleComponent();
	const FVector Here = Body->GetActorLocation();
	const FVector Feet = Body->GetNavAgentLocation();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float StepHeight = PlayerBody->GetCharacterMovement()->MaxStepHeight;
	const FCollisionShape Shape = Capsule->GetCollisionShape();
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BotLedgeEscape), false, Body);
	const auto TryLanding = [&](const FNavLocation& Landing)
	{
		const FVector Delta = Landing.Location - Feet;
		if (Delta.Size2D() <= Capsule->GetScaledCapsuleRadius() + 30.0f
			|| Delta.Z > StepHeight || Delta.Z < -WoTBotLinks::MaxDrop) return false;
		const FVector OverLanding(Landing.Location.X, Landing.Location.Y, Here.Z);
		FHitResult Hit;
		if (GetWorld()->SweepSingleByChannel(Hit, Here, OverLanding, FQuat::Identity, Channel, Shape, Params)) return false;
		const FVector LandingCenter = Landing.Location + FVector(0, 0, HalfHeight);
		if (!GetWorld()->SweepSingleByChannel(Hit, OverLanding, LandingCenter - FVector(0, 0, StepHeight),
			FQuat::Identity, Channel, Shape, Params) || Hit.bStartPenetrating
			|| !PlayerBody->GetCharacterMovement()->IsWalkable(Hit)
			|| FMath::Abs(Hit.Location.Z - LandingCenter.Z) > StepHeight) return false;
		if (MoveToLocation(Landing.Location, 5.0f, false, false, false, false, nullptr, true) == EPathFollowingRequestResult::Failed) return false;
		Destination = Landing.Location;
		bWantsMovement = true;
		bNavigationUnavailable = false;
		return true;
	};
	FNavLocation Nearest;
	const bool bNearest = Nav->ProjectPointToNavigation(Feet, Nearest, FVector(400, 400, WoTBotLinks::MaxDrop), Data);
	if (bNearest && TryLanding(Nearest)) return;
	const FVector TowardMesh = bNearest ? (Nearest.Location - Feet).GetSafeNormal2D() : FVector::ZeroVector;
	const FVector FirstDirection = TowardMesh.IsNearlyZero() ? FVector::ForwardVector : TowardMesh;
	for (const float Distance : { 100.0f, 200.0f, 400.0f })
	{
		for (int32 Side = 0; Side < 8; ++Side)
		{
			const FVector Probe = Feet + FirstDirection.RotateAngleAxis(Side * 45.0f, FVector::UpVector) * Distance;
			FNavLocation Landing;
			if (Nav->ProjectPointToNavigation(Probe, Landing, FVector(40, 40, WoTBotLinks::MaxDrop), Data)
				&& TryLanding(Landing)) return;
		}
	}
	bNavigationUnavailable = true;
}

void AWoTBotController::UpdateStuck()
{
	const float Now = GetWorld()->GetTimeSeconds();
	// A body falling through a gap in a floor's collision can fall until KillZ,
	// minutes away; that fall is not path progress, so the stuck rule ends it.
	const bool bFalling = CastChecked<AWoTCharacter>(GetPawn())->GetCharacterMovement()->IsFalling();
	if (!bFalling && !bNavigationUnavailable && (!bWantsMovement || FVector::Dist2D(GetPawn()->GetActorLocation(), Destination) <= 30.0f))
	{
		bWantsMovement = false;
		LastProgressTime = LastRecoveryTime = Now;
		ProgressPosition = GetPawn()->GetActorLocation();
		return;
	}
	// A capsule shuffling against a corner is not useful path progress.
	if (!bFalling && FVector::DistSquared(GetPawn()->GetActorLocation(), ProgressPosition) >= FMath::Square(75.0f))
	{
		ProgressPosition = GetPawn()->GetActorLocation();
		LastProgressTime = LastRecoveryTime = Now;
	}
	if (Now - LastProgressTime >= FMath::Max(3.0f, GetDefault<UWoTBotSettings>()->StuckRespawnSeconds))
	{ SilentRespawn(); return; }
	if (Now - LastProgressTime >= 3.0f && Now - LastRecoveryTime >= 3.0f)
	{ WOT_SOAK_EVENT(Recovery, this, GetPawn()); LastRecoveryTime = Now; ChooseDestination(true); }
}

void AWoTBotController::Tick(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WoTBotTick);
	WOT_BOT_METRICS_SCOPE();
	Super::Tick(DeltaSeconds);
	if (!CanAct()) return;
	ObserveCastUse();
	if (bTargetVisible && !CanSee(Target.Get())) SetTarget(Target.Get(), false);
	if (!CanAct()) return;
	if (Caster.IsValid() && !CastChecked<AWoTCharacter>(GetPawn())->CanCastNow())
	{
		Caster->Release();
		// A landing interruption must not permanently exclude the remaining kit
		// when the same target stays visible. Usage and input cadence still apply.
		Caster->ResetAdaptation();
	}
	if (!CanAct()) return;
	UpdateAim();
	UpdateCastCompletion();
	if (!CanAct()) return;
	const FVector Here = GetPawn()->GetActorLocation();
	if (IsHeldByWhirlwind())
	{
		// The whirlwind holds the body with its movement disabled (WhirlwindLeech.uc
		// AttachTo, PHYS_None): it can neither jump nor walk, and makes no progress
		// it could be missing. The stuck clocks start again when it lets go, and the
		// route is planned again from wherever the body comes down.
		ClimbBlockedSince = -1.0f;
		NextRepathTime = 0.0f;
		LastProgressTime = LastRecoveryTime = GetWorld()->GetTimeSeconds();
		ProgressPosition = LastBodyLocation = Here;
		return;
	}
	UpdateClimb();
	// A teleporter (or a swap) moved the body: its route now starts elsewhere.
	if (FVector::DistSquared(Here, LastBodyLocation) > FMath::Square(250.0f))
	{
		ArrivalLocation = Here;
		bArrivedByTeleport = true;
		if (bWantsMovement)
		{
			NextRepathTime = 0.0f;
			ChooseDestination();
			if (!CanAct()) return;
		}
	}
	LastBodyLocation = Here;
	UpdateStuck();
}
