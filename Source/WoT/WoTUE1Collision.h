#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

// UE1 actor collision cylinders have flat caps. UE capsules do not, and they
// also require HalfHeight >= Radius. Overlap-only ports use a containing
// capsule as their broad phase, then apply this exact authored-cylinder test.
namespace WoTUE1Collision
{
	// UE1 FindSpot's bCheckActors=0 path adjusts only against the level. UE5's
	// FindTeleportSpot also tests pawns through the moving actor's component
	// responses, so suppress just that channel for the duration of the native
	// placement call and restore it before returning.
	class FScopedIgnorePawnResponse
	{
	public:
		explicit FScopedIgnorePawnResponse(AActor& Actor)
		{
			TInlineComponentArray<UPrimitiveComponent*> Primitives(&Actor);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (Primitive && Primitive->IsCollisionEnabled())
				{
					Components.Add(Primitive);
					Responses.Add(Primitive->GetCollisionResponseToChannel(ECC_Pawn));
					Primitive->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
				}
			}
		}
		~FScopedIgnorePawnResponse()
		{
			for (int32 Index = 0; Index < Components.Num(); ++Index)
			{
				if (UPrimitiveComponent* Primitive = Components[Index].Get())
				{
					Primitive->SetCollisionResponseToChannel(ECC_Pawn, Responses[Index]);
				}
			}
		}

	private:
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Components;
		TArray<TEnumAsByte<ECollisionResponse>> Responses;
	};

	inline bool FindTeleportSpotWorldOnly(UWorld& World, AActor& Actor,
		FVector& InOutSpot, const FRotator& Rotation)
	{
		FScopedIgnorePawnResponse IgnorePawns(Actor);
		return World.FindTeleportSpot(&Actor, InOutSpot, Rotation);
	}

	inline bool TeleportWorldOnly(AActor& Actor, const FVector& Destination,
		const FRotator& Rotation)
	{
		FScopedIgnorePawnResponse IgnorePawns(Actor);
		return Actor.TeleportTo(Destination, Rotation);
	}

	// SpawnActor/SetLocation in UE1 perform the world FindSpot first, then reject
	// the move when the resolved cylinder still encroaches a mutually-blocking
	// actor (UE1's SpawnActor/CheckEncroachment).  UE5's AlwaysSpawn path
	// deliberately omits that second half, so callers which need the retail split
	// use this after FindTeleportSpotWorldOnly.
	inline bool EncroachesBlockingActor(const AActor& Actor)
	{
		const UWorld* World = Actor.GetWorld();
		if (!World) return false;
		TInlineComponentArray<UPrimitiveComponent*> Primitives(&Actor);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (!Primitive || !Primitive->IsQueryCollisionEnabled()) continue;
			TArray<FOverlapResult> Overlaps;
			FComponentQueryParams Params(SCENE_QUERY_STAT(WoTUE1Encroachment),
				&Actor);
			Primitive->ComponentOverlapMulti(Overlaps, World,
				Primitive->GetComponentLocation(), Primitive->GetComponentQuat(),
				Primitive->GetCollisionObjectType(), Params);
			for (const FOverlapResult& Overlap : Overlaps)
			{
				const UPrimitiveComponent* Other = Overlap.Component.Get();
				const AActor* OtherActor = Overlap.GetActor();
				if (!Other || !IsValid(OtherActor) || OtherActor == &Actor) continue;
				// UE1 CheckEncroachment's actor pass excludes the Level model; the
				// preceding world-only FindSpot already resolved that geometry. UE5
				// exposes floors/BSP as WorldStatic actor components, so do not reject
				// a pawn merely because its cylinder is seated flush on the floor.
				if (Other->GetCollisionObjectType() == ECC_WorldStatic) continue;
				if (Primitive->GetCollisionResponseToChannel(
						Other->GetCollisionObjectType()) == ECR_Block
					&& Other->GetCollisionResponseToChannel(
						Primitive->GetCollisionObjectType()) == ECR_Block)
				{
					return true;
				}
			}
		}
		return false;
	}

	// A pawn capsule is rounded: where its UE1 cylinder's flat cap meets the
	// authored cylinder's cap, the capsule surface sits up to one pawn radius
	// further away. The broad phase is extended vertically by this margin so a
	// pawn whose UE1 cylinder meets the authored cylinder always overlaps the
	// broad phase too. 32 covers every retail player class (WOTPawns player
	// CollisionRadius <= 32).
	inline constexpr float PawnCapMargin = 32.0f;

	inline bool CylindersOverlap(const FVector& Center, float Radius,
		float HalfHeight, const AActor* Other)
	{
		if (!IsValid(Other) || Radius < 0.0f || HalfHeight < 0.0f)
		{
			return false;
		}
		float OtherRadius = 0.0f;
		float OtherHalfHeight = 0.0f;
		Other->GetSimpleCollisionCylinder(OtherRadius, OtherHalfHeight);
		const FVector Delta = Other->GetActorLocation() - Center;
		const float CombinedRadius = Radius + FMath::Max(0.0f, OtherRadius);
		return FVector2D(Delta.X, Delta.Y).SizeSquared()
				<= FMath::Square(CombinedRadius)
			&& FMath::Abs(Delta.Z)
				<= HalfHeight + FMath::Max(0.0f, OtherHalfHeight);
	}

	// Half-height of the containing capsule (radius = the authored radius) used
	// as the overlap broad phase: its straight section spans the whole authored
	// height plus the pawn-cap margin, so the capsule contains the flat-ended
	// cylinder and every pawn whose UE1 cylinder meets it.
	inline float ContainingCapsuleHalfHeight(float Radius, float HalfHeight)
	{
		return FMath::Max(0.0f, HalfHeight) + FMath::Max(0.0f, Radius)
			+ PawnCapMargin;
	}

	// UE1 raises Touch the moment two cylinders meet and UnTouch the moment they
	// part, whatever movement brought that about. A containing broad phase only
	// reports entering and leaving the capsule, so an actor that enters the
	// capsule outside the exact cylinder (falling or jumping in, a neighbouring
	// floor) and then moves into it would never touch. The tracker polls such
	// candidates once per frame while they remain in the broad phase and re-runs
	// the owner's touch path on the first frame they meet the cylinder; owners
	// that implement UnTouch can also watch touching actors for leaving the
	// cylinder while still inside the capsule.
	class FTouchTracker
	{
	public:
		using FInside = TFunction<bool(const AActor*)>;
		using FEvent = TFunction<void(AActor*)>;

		// Other overlaps BroadPhase but not the exact cylinder: Enter(Other)
		// runs on the first frame Inside(Other) holds while it still overlaps.
		void DeferOutside(AActor& Owner, UPrimitiveComponent* BroadPhase,
			AActor* Other, FInside InInside, FEvent InEnter)
		{
			if (!IsValid(Other) || !BroadPhase)
			{
				return;
			}
			Bind(Owner, BroadPhase, MoveTemp(InInside), MoveTemp(InEnter));
			Watched.Remove(Other);
			Pending.AddUnique(Other);
			SchedulePoll();
		}

		// Other is touching: Leave(Other) runs on the first frame it leaves the
		// exact cylinder while still overlapping BroadPhase, after which it is a
		// pending candidate again (so re-entry touches again).
		void WatchInside(AActor& Owner, UPrimitiveComponent* BroadPhase,
			AActor* Other, FInside InInside, FEvent InEnter, FEvent InLeave)
		{
			if (!IsValid(Other) || !BroadPhase)
			{
				return;
			}
			Bind(Owner, BroadPhase, MoveTemp(InInside), MoveTemp(InEnter));
			Leave = MoveTemp(InLeave);
			Pending.Remove(Other);
			Watched.AddUnique(Other);
			SchedulePoll();
		}

		// The broad-phase end overlap (the owner's own UnTouch path runs there).
		void Forget(const AActor* Other)
		{
			Pending.RemoveAll([Other](const TWeakObjectPtr<AActor>& Entry)
				{ return !Entry.IsValid() || Entry.Get() == Other; });
			Watched.RemoveAll([Other](const TWeakObjectPtr<AActor>& Entry)
				{ return !Entry.IsValid() || Entry.Get() == Other; });
		}

		bool IsTracking(const AActor* Other) const
		{
			for (const TWeakObjectPtr<AActor>& Entry : Pending)
			{
				if (Entry.Get() == Other) return true;
			}
			for (const TWeakObjectPtr<AActor>& Entry : Watched)
			{
				if (Entry.Get() == Other) return true;
			}
			return false;
		}

	private:
		void Bind(AActor& Owner, UPrimitiveComponent* BroadPhase,
			FInside InInside, FEvent InEnter)
		{
			OwnerActor = &Owner;
			Broad = BroadPhase;
			Inside = MoveTemp(InInside);
			Enter = MoveTemp(InEnter);
		}

		void SchedulePoll()
		{
			AActor* Owner = OwnerActor.Get();
			UWorld* World = Owner ? Owner->GetWorld() : nullptr;
			if (bPollScheduled || !World)
			{
				return;
			}
			bPollScheduled = true;
			World->GetTimerManager().SetTimerForNextTick(
				FTimerDelegate::CreateWeakLambda(Owner, [this]() { Poll(); }));
		}

		void Poll()
		{
			bPollScheduled = false;
			UPrimitiveComponent* BroadPhase = Broad.Get();
			if (!OwnerActor.IsValid() || !IsValid(BroadPhase) || !Inside)
			{
				Pending.Reset();
				Watched.Reset();
				return;
			}
			TArray<AActor*> Entered;
			TArray<AActor*> Left;
			for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
			{
				AActor* Candidate = Pending[Index].Get();
				if (!IsValid(Candidate) || !BroadPhase->IsOverlappingActor(Candidate))
				{
					Pending.RemoveAt(Index);
				}
				else if (Inside(Candidate))
				{
					Pending.RemoveAt(Index);
					Entered.Add(Candidate);
				}
			}
			for (int32 Index = Watched.Num() - 1; Index >= 0; --Index)
			{
				AActor* Toucher = Watched[Index].Get();
				if (!IsValid(Toucher) || !BroadPhase->IsOverlappingActor(Toucher))
				{
					Watched.RemoveAt(Index);
				}
				else if (!Inside(Toucher))
				{
					Watched.RemoveAt(Index);
					Pending.AddUnique(Toucher);
					Left.Add(Toucher);
				}
			}
			// Copies: a callback may re-enter DeferOutside/WatchInside.
			const FEvent LeaveCallback = Leave;
			const FEvent EnterCallback = Enter;
			for (AActor* Toucher : Left)
			{
				if (LeaveCallback && IsValid(Toucher)) LeaveCallback(Toucher);
			}
			for (AActor* Candidate : Entered)
			{
				if (EnterCallback && IsValid(Candidate)) EnterCallback(Candidate);
			}
			if (Pending.Num() > 0 || Watched.Num() > 0)
			{
				SchedulePoll();
			}
		}

		TArray<TWeakObjectPtr<AActor>> Pending;
		TArray<TWeakObjectPtr<AActor>> Watched;
		TWeakObjectPtr<AActor> OwnerActor;
		TWeakObjectPtr<UPrimitiveComponent> Broad;
		FInside Inside;
		FEvent Enter;
		FEvent Leave;
		bool bPollScheduled = false;
	};
}
