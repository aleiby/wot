#include "WoTAngrealInvTracer.h"

#include "WoTSeal.h"
#include "WoTInventoryComponent.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTTracerProjectile.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

AWoTAngrealInvTracer::AWoTAngrealInvTracer()
{
	BindStatusIcons(TEXT("I_Tracer"), TEXT("M_Tracer"));
	// AngrealInvTracer defaultproperties
	ProjectileClass = AWoTTracerProjectile::StaticClass();
	bElementFire = true;
	bElementAir = true;
	bRare = true;
	bInfo = true;
	MinInitialCharges = 2;
	MaxInitialCharges = 4;
	MaxCharges = 10;
	MaxChargesInGroup = 5;
	MinChargeGroupInterval = 6.0f;
}

// AngrealInvTracer.GetBestTarget: the closest reachable seal, checking both
// world-pickup seals and carried seals; falls back to the closest unreachable
// seal when none is reachable.
AActor* AWoTAngrealInvTracer::GetBestTarget()
{
	AActor* CasterOwner = GetOwner();
	if (!CasterOwner || !GetWorld())
	{
		return nullptr;
	}
	const FVector From = CasterOwner->GetActorLocation();
	if (WoTSeekNavigationUsesPathNodes())
	{
		// AngrealInvTracer.uc:30-127: direct candidates use plain distance;
		// blocked candidates use the desperate iterator's full NodeCost; only
		// failed candidates enter the straight-distance fallback bucket.
		AActor* Best = nullptr;
		float BestCost = 0.0f;
		AActor* BestUnreachable = nullptr;
		float BestUnreachableCost = 0.0f;
		FWoTPathNodeIterator Iterator;

		auto ConsiderRetail = [&](AActor* SealActor, const FVector& SealLoc)
		{
			float Cost = FVector::Dist(SealLoc, From);
			// AngrealInvTracer.uc:151 Trace(..., false): a carried seal and
			// its carrier capsule do not obstruct TRACE_VisBlocking.
			const bool bDirect = !WoTVisBlockingTraceHits(
				GetWorld(), From, SealLoc, this);
			const bool bPath = !bDirect
				&& Iterator.BuildPath(GetWorld(), From, SealLoc, true);
			if (bDirect || bPath)
			{
				if (bPath)
				{
					Cost = Iterator.NodeCost;
				}
				if (!Best || Cost < BestCost)
				{
					Best = SealActor;
					BestCost = Cost;
				}
			}
			else if (!BestUnreachable || Cost < BestUnreachableCost)
			{
				BestUnreachable = SealActor;
				BestUnreachableCost = Cost;
			}
		};

		for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
		{
			AWoTSeal* Seal = *It;
			if (Seal->GetItemState() == EWoTInventoryState::Pickup)
			{
				ConsiderRetail(Seal, Seal->GetActorLocation());
			}
		}
		// The executable .uc iterates the complete PawnList including Owner
		// (a zero-cost self-target when the caster carries a seal), but its
		// "ignore your own seal" prose is honored as a RULED divergence
		// (PORT_NOTES "Tracer ignores the caster's own seals") — both backend arms keep the owner skip
		// until the divergence audit re-adjudicates it.
		for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
		{
			AWoTCharacter* Pawn = *It;
			if (Pawn == CasterOwner)
			{
				continue;
			}
			if (Pawn->InventoryComponent
				&& Cast<AWoTSeal>(Pawn->InventoryComponent->FindInventoryType(
					AWoTSeal::StaticClass())))
			{
				ConsiderRetail(Pawn, Pawn->GetActorLocation());
			}
		}
		return Best ? Best : BestUnreachable;
	}

	// Retained pre-G41 NavMesh A/B arm.
	AActor* Best = nullptr;
	float BestCost = 0.0f;
	AActor* BestUnreach = nullptr;
	float BestUnreachCost = 0.0f;

	auto Consider = [&](AActor* SealActor, const FVector& SealLoc)
	{
		const float Cost = FVector::Dist(SealLoc, From);
		if (IsSealReachable(SealLoc, From))
		{
			if (!Best || Cost < BestCost)
			{
				Best = SealActor;
				BestCost = Cost;
			}
		}
		else if (!BestUnreach || Cost < BestUnreachCost)
		{
			BestUnreach = SealActor;
			BestUnreachCost = Cost;
		}
	};

	// Seals lying in the world (Pickup state), not currently carried.
	for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
	{
		AWoTSeal* S = *It;
		if (S->GetItemState() != EWoTInventoryState::Pickup)
		{
			continue;
		}
		Consider(S, S->GetActorLocation());
	}

	// Seals carried by other pawns — target the carrier. Skip the caster's own
	// seal (the class ignores seals you are holding).
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* P = *It;
		if (P == CasterOwner || !P->InventoryComponent)
		{
			continue;
		}
		if (Cast<AWoTSeal>(P->InventoryComponent->FindInventoryType(AWoTSeal::StaticClass())))
		{
			Consider(P, P->GetActorLocation());
		}
	}

	return Best ? Best : BestUnreach;
}

bool AWoTAngrealInvTracer::IsSealReachable(const FVector& SealLoc, const FVector& From) const
{
	// CanDirectlyReach: a world-geometry-only trace (Trace bTraceActors=false).
	// AngrealInvTracer.uc:151 Trace(..., false); ownership filtering follows
	// UE1's ULevel::SingleLineCheck.
	if (!WoTVisBlockingTraceHits(GetWorld(), From, SealLoc, this))
	{
		return true;
	}
	// Blocked NavMesh A/B arm: preserve the pre-G41 path-existence query.
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (Nav)
	{
		UNavigationPath* Path =
			Nav->FindPathToLocationSynchronously(GetWorld(), From, SealLoc);
		if (Path && Path->IsValid() && !Path->IsPartial())
		{
			return true;
		}
	}
	return false;
}
