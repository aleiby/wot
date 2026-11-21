#include "WoTBotController.h"
#include "WoTBotSettings.h"
#include "WoTBotRoster.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTCasterComponent.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Projectiles/WoTExpWardProjectile.h"
#include "WoTCharacter.h"
#include "WoTPlayerState.h"
#include "AI/WoTAIUtil.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EngineUtils.h"

namespace
{
constexpr float WardNoticeRange = 1200;
constexpr float WardMemorySeconds = 12;
constexpr float NoticeInterval = .5f;
constexpr float ShotInterval = 1;
// How long a bot stopped short of a known ward waits before it takes another
// goal or route: the stuck rule's own recovery horizon (UpdateStuck replaces
// a goal after 3 s without progress), which waiting suppresses. It spans three
// shot decisions (ShotInterval), so a bot holding a damaging artifact usually
// decides to clear a blocking ward (1 - 0.4^3, about 94%) before it gives up.
constexpr float WardWaitSeconds = 3;

AWoTExpWardProjectile* WardActor(AActor* Actor)
{
	// Optional port class has no DLL export; use its reflected identity.
	static UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTExpWardProjectile"));
	return IsValid(Actor) && Class && Actor->IsA(Class) ? static_cast<AWoTExpWardProjectile*>(Actor) : nullptr;
}
}

void AWoTBotController::PerceiveWards()
{
	if (!CanAct()) return;
	const float Now = GetWorld()->GetTimeSeconds();
	const auto* Settings = GetDefault<UWoTBotSettings>();
	const auto* Self = GetPlayerState<AWoTPlayerState>();
	for (const auto& Candidate : PerceptionWards)
	{
		auto* Ward = WardActor(Candidate.Get());
		if (!Ward || !Ward->bCorrectlyPlaced) continue;
		const APawn* Source = Ward->SourcePawn ? Ward->SourcePawn.Get() : Ward->GetInstigator();
		const auto* SourceState = Source ? Source->GetPlayerState<AWoTPlayerState>() : nullptr;
		const auto* SourceNPC = Cast<AWoTNPCCharacter>(Source);
		const uint8 SourceTeam = SourceState ? SourceState->Team : SourceNPC ? SourceNPC->AssignedTeam : 255;
		const auto* Existing = WardBeliefs.Find(Ward);
		// Placement knowledge belongs to actual teammates. A disguised enemy
		// cannot reveal unseen wards by temporarily presenting a friendly team.
		const bool Friendly = (Existing && Existing->bFriendly) || (Source && (Source == GetPawn()
			|| (Self && SourceTeam != 255 && SourceTeam == Self->Team)));
		const float Distance = FVector::Dist(GetPawn()->GetActorLocation(), Ward->GetActorLocation());
		const bool Visible = Distance < FMath::Min(WardNoticeRange, Settings->SightRange)
			&& !Ward->IsHidden() && CanSeeStimulus(Ward);
		if (!Friendly && !Visible && !Existing) continue;
		auto& Belief = WardBeliefs.FindOrAdd(Ward);
		Belief.bFriendly = Friendly;
		if (!Visible) Belief.VisibleSince = -1;
		else if (Belief.VisibleSince < 0) Belief.VisibleSince = Now;
		if (!Friendly && Now - Belief.LastSeen >= WardMemorySeconds) Belief.bKnown = false;
		bool Noticed = false;
		if (Visible && Now - Belief.VisibleSince >= Settings->ReactionSeconds)
		{
			if (Belief.bKnown) Noticed = true;
			else if (Now >= Belief.NextNotice)
			{
				Belief.NextNotice = Now + NoticeInterval;
				// A fixed-rate opportunity, independent of frame/think frequency.
				const float Chance = .85f * (1 - Distance / WardNoticeRange)
					* (.1f + .9f * FMath::Clamp(Ward->ScaleGlow, 0.0f, 1.0f));
				Noticed = WardRandom.FRand() < Chance;
			}
		}
		if (Friendly || Noticed)
		{
			Belief.bKnown = true;
			Belief.LastSeen = Now;
			Belief.Position = Ward->GetActorLocation();
			Belief.Radius = Ward->DamageRadius;
		}
	}
	for (auto It = WardBeliefs.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	PerceptionWards.RemoveAll([](const auto& Ward) { return !Ward.IsValid(); });
	GuardWardRoute();
}

AActor* AWoTBotController::KnownWardOnSegment(const FVector& From, const FVector& To, bool bLeaving) const
{
	if (WardBeliefs.IsEmpty() || !GetPawn()) return nullptr;
	float BodyRadius, HalfHeight;
	GetPawn()->GetSimpleCollisionCylinder(BodyRadius, HalfHeight);
	const float Now = GetWorld()->GetTimeSeconds();
	for (const auto& Pair : WardBeliefs)
	{
		const auto& Belief = Pair.Value;
		if (!Pair.Key.IsValid() || !Belief.bKnown || Now - Belief.LastSeen >= WardMemorySeconds) continue;
		const auto Inside = [&](const FVector& Point)
		{
			return FMath::Abs(Point.Z - Belief.Position.Z) <= Belief.Radius + HalfHeight
				&& FVector::Dist2D(Point, Belief.Position) < Belief.Radius + BodyRadius + 8;
		};
		const FVector2D Segment(To-From), Offset(Belief.Position-From);
		if (bLeaving && Inside(From))
		{
			// Already inside this zone: the way out is never closer. Distance along
			// a straight segment is convex, so one whose start does not approach
			// the ward never does; one that approaches it goes further in.
			if (FVector2D::DotProduct(Offset, Segment) > 0) return Pair.Key.Get();
			continue;
		}
		const float Along = Segment.SizeSquared() > 0 ? FMath::Clamp(FVector2D::DotProduct(Offset,Segment)/Segment.SizeSquared(),0.0,1.0) : 0;
		if (Inside(FMath::Lerp(From,To,Along))) return Pair.Key.Get();
	}
	return nullptr;
}

AActor* AWoTBotController::KnownWardOnRoute(const TArray<FNavPathPoint>& Points, int32 First, FVector From) const
{
	const APawn* Body = GetPawn();
	if (!Body) return nullptr;
	const float BodyRadius = Body->GetSimpleCollisionRadius();
	for (int32 I = First; I < Points.Num(); ++I)
	{
		const FVector To = Points[I].Location;
		// A body inside a zone may only leave it; a route that starts inside
		// one does so only along segments that keep moving away.
		AActor* Hazard = KnownWardOnSegment(From, To, true);
		if (!Hazard && Roster.IsValid())
			for (const auto& Via : Roster->GetDistantTeleports())
			{
				// Distant links aren't in this path. Check the landing before
				// walking into their pad, including an incidental crossing.
				// An existing occupant can leave: pads re-arm on end-overlap.
				if (I == First && Via.Teleporter.IsValid() && Via.Teleporter->IsOverlappingActor(Body)) continue;
				const FVector NearPad = FMath::ClosestPointOnSegment(Via.Pad, From, To);
				if (FMath::Abs(NearPad.Z - Via.Pad.Z) > 120
					|| FVector::Dist2D(NearPad, Via.Pad) > Via.TouchRadius + BodyRadius + 8) continue;
				Hazard = KnownWardOnSegment(Via.Exit, Via.Exit);
				if (Hazard) break;
			}
		if (Hazard) return Hazard;
		From = To;
	}
	return nullptr;
}

bool AWoTBotController::GuardWardRoute()
{
	if (WardBeliefs.IsEmpty()) { BlockingWard.Reset(); bWardWaiting = false; return false; }
	auto* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body) return false;
	const float Now = GetWorld()->GetTimeSeconds();
	const auto* BlockingBelief = WardBeliefs.Find(BlockingWard);
	if (!BlockingBelief || !BlockingBelief->bKnown || Now - BlockingBelief->LastSeen >= WardMemorySeconds)
	{ BlockingWard.Reset(); bWardWaiting = false; }
	const auto* Following = GetPathFollowingComponent();
	const auto Path = Following ? Following->GetPath() : FNavPathSharedPtr();
	if (!Path.IsValid() || Following->GetStatus() == EPathFollowingStatus::Idle) return bWardWaiting;
	if (AActor* Hazard = KnownWardOnRoute(Path->GetPathPoints(),
		FMath::Max(1, int32(Following->GetCurrentPathIndex()) + 1), Body->GetActorLocation()))
	{
		// Reject only this bot's unsafe route; the shared mesh is untouched.
		BlockingWard = Hazard;
		if (!bWardWaiting) WardWaitSince = Now;
		bWardWaiting = true;
		StopMovement();
		Body->GetCharacterMovement()->StopMovementImmediately();
		bWantsMovement = false;
		LastProgressTime = LastRecoveryTime = Now;
		return true;
	}
	bWardWaiting = false;
	BlockingWard.Reset();
	return false;
}

void AWoTBotController::UpdateWardWait()
{
	// A shot being aimed at a ward finishes first.
	const float Now = GetWorld()->GetTimeSeconds();
	if (AimingWard.IsValid() || Now < NextWardLeave || !GetPawn()) return;
	const FVector Here = GetPawn()->GetActorLocation();
	// Standing inside a known ward's zone is no place to wait: leave at once.
	if (Now - WardWaitSince < WardWaitSeconds && !KnownWardOnSegment(Here, Here)) return;
	NextWardLeave = Now + 1;
	LeaveWard();
}

bool AWoTBotController::LeaveWard()
{
	const float Now = GetWorld()->GetTimeSeconds();
	// With no clear route found, the next attempt follows another full wait.
	WardWaitSince = Now;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav && Body ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) return false;
	// Give up the blocked goal as the stuck rule's recovery does.
	if (SeekingPool.IsValid()) { RecoveryPools.Add(SeekingPool, Now); ReleasePoolGoal(); }
	const FVector Here = Body->GetActorLocation();
	const AActor* Holding = KnownWardOnSegment(Here, Here);
	const FBotWardBelief* Ward = WardBeliefs.Find(Holding ? Holding : BlockingWard.Get());
	FVector Away = Ward ? (Here - Ward->Position).GetSafeNormal2D() : FVector::ZeroVector;
	if (Away.IsNearlyZero()) Away = -Body->GetActorForwardVector().GetSafeNormal2D();
	// The first reachable point, preferring the way away from the ward, whose
	// whole route keeps clear of every ward this bot knows.
	for (const float Distance : { 400.0f, 800.0f })
		for (const float Angle : { 0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f })
		{
			FNavLocation Point;
			if (!Nav->ProjectPointToNavigation(Here + Away.RotateAngleAxis(Angle, FVector::UpVector) * Distance,
				Point, FVector(100, 100, 200), Data)) continue;
			FPathFindingQuery Query(this, *Data, Body->GetNavAgentLocation(), Point.Location);
			Query.SetAllowPartialPaths(false);
			const FPathFindingResult Found = Nav->FindPathSync(Body->GetNavAgentPropertiesRef(), Query);
			if (!Found.IsSuccessful() || !Found.Path.IsValid() || Found.Path->IsPartial()
				|| KnownWardOnRoute(Found.Path->GetPathPoints(), 1, Here)) continue;
			// NavigateTo checks the route it actually follows the same way.
			if (!NavigateTo(Point.Location) || bWardWaiting) continue;
			WardDetourUntil = Now + WardWaitSeconds;
			return true;
		}
	return false;
}

bool AWoTBotController::WardShotSafe(AActor* Ward) const
{
	const APawn* Body = GetPawn();
	const auto* Shot = WardBeliefs.Find(Ward);
	if (!Body || !Ward || !Shot) return false;
	const float Now = GetWorld()->GetTimeSeconds();
	// Every blast a detonation could set off among the wards this bot knows:
	// a blast damages, and so detonates, each ward within its radius (the
	// splash in WoTGenericProjectile.cpp ProcessSplashDamage), and a ward's
	// Event triggers the wards tagged with it (AngrealExpWardProjectile.uc
	// TriggerOthers). Beliefs only: a ward this bot never noticed adds nothing.
	struct FBlast { FVector At; float Radius; const AWoTExpWardProjectile* Ward; };
	TArray<FBlast> Chain;
	Chain.Add({ Ward->GetActorLocation(), Shot->Radius, WardActor(Ward) });
	TSet<const AActor*> Reached;
	Reached.Add(Ward);
	for (int32 I = 0; I < Chain.Num(); ++I)
		for (const auto& Pair : WardBeliefs)
		{
			AActor* Other = Pair.Key.Get();
			const auto& Belief = Pair.Value;
			if (!Other || Reached.Contains(Other) || !Belief.bKnown || Now - Belief.LastSeen >= WardMemorySeconds) continue;
			const AWoTExpWardProjectile* OtherWard = WardActor(Other);
			const bool Triggered = Chain[I].Ward && OtherWard && !Chain[I].Ward->EventName.IsNone()
				&& Chain[I].Ward->EventName == OtherWard->TagName;
			if (!Triggered && FVector::Dist(Chain[I].At, Belief.Position) > Chain[I].Radius) continue;
			Reached.Add(Other);
			Chain.Add({ Belief.Position, Belief.Radius, OtherWard });
		}
	const auto Blasted = [&Chain](const APawn* Who)
	{
		const float Margin = Who->GetSimpleCollisionRadius() + 8;
		return Chain.ContainsByPredicate([&](const FBlast& Blast)
			{ return FVector::Dist(Who->GetActorLocation(), Blast.At) <= Blast.Radius + Margin; });
	};
	if (Blasted(Body)) return false;
	// Nor a teammate in view, by the team this player sees (as IsHostile does).
	const auto* Self = GetPlayerState<AWoTPlayerState>();
	if (!Self) return true;
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
		if (*It != Body && It->IsAlive() && Blasted(*It)
			&& WoTAIUtil::ApparentTeamOf(*It) == Self->Team && CanSeeStimulus(*It)) return false;
	return true;
}

bool AWoTBotController::CastAtWard()
{
	if (WardBeliefs.IsEmpty() || !Caster.IsValid() || GetWorld()->GetTimeSeconds() < DamageSearchUntil) return false;
	ObserveCastUse();
	const float Now = GetWorld()->GetTimeSeconds();
	if (AimingWard.IsValid() && Caster->IsCasting())
	{
		if (CanSeeStimulus(AimingWard.Get()) && WardShotSafe(AimingWard.Get())) return true;
		Caster->Release();
		if (!CanAct()) return true;
	}
	AimingWard.Reset();
	if (Caster->IsCasting() || Now < NextWardShot
		|| Now <= LastOffensiveCastTime + Caster->MinOffensiveUsageInterval) return false;
	for (const auto& Pair : WardBeliefs)
	{
		auto* Ward = WardActor(Pair.Key.Get());
		const auto& Belief = Pair.Value;
		if (!Ward || !Ward->bArmed || !Belief.bKnown || Belief.VisibleSince < 0
			|| Now - Belief.LastSeen >= WardMemorySeconds
			|| Now - Belief.VisibleSince < GetDefault<UWoTBotSettings>()->ReactionSeconds
			|| !CanSeeStimulus(Ward)) continue;
		const bool Opportunity = bTargetVisible && Target.IsValid()
			&& Now - VisibleSince >= GetDefault<UWoTBotSettings>()->ReactionSeconds
			&& FVector::Dist(Target->GetActorLocation(), Ward->GetActorLocation()) < Belief.Radius;
		if (BlockingWard != Ward && !Opportunity) continue;
		// Nothing the shot could set off may reach this body or a teammate in view.
		if (!WardShotSafe(Ward)) continue;
		NextWardShot = Now + ShotInterval;
		if (WardRandom.FRand() >= (Opportunity ? .45f : .6f)) return false;
		// The generic selector scores projectile victims as defensive counters.
		// Deliberately damaging a ward uses offensive priority instead, retaining
		// the item's range, charges and group clock, and the caster hand cadence.
		AWoTAngrealInventory* Item = nullptr;
		float BestPriority = 0;
		for (AWoTInventory* Held : CastChecked<AWoTCharacter>(GetPawn())->InventoryComponent->Items)
		{
			auto* Launcher = Cast<AWoTProjectileLauncher>(Held);
			const auto* Projectile = Launcher && Launcher->ProjectileClass ? Launcher->ProjectileClass.GetDefaultObject() : nullptr;
			if (!Projectile || !Launcher->bOffensive || Projectile->Damage <= 0 || Projectile->Speed <= 0 || Projectile->bUsesDestination) continue;
			float MinRange, MaxRange, Priority;
			Launcher->GetEffectiveRange(MinRange, MaxRange);
			const float Distance = FVector::Dist(GetPawn()->GetActorLocation(), Ward->GetActorLocation());
			if (Distance < MinRange || Distance > MaxRange) continue;
			Launcher->SetVictim(Ward);
			if (Launcher->GetOffensivePriority(Priority, Ward) && (!Item || Priority > BestPriority))
			{ Item = Launcher; BestPriority = Priority; }
		}
		if (!Item) return false;
		Item->InitChargeGroup();
		AimingWard = Ward;
		const float Angle = WardRandom.FRand() * 2 * PI;
		AimDisk = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * FMath::Sqrt(WardRandom.FRand());
		CastItem(Item);
		return true;
	}
	return false;
}
