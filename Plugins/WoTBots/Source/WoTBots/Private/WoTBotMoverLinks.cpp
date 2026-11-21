#include "WoTBotMoverLink.h"
#include "WoTMover.h"
#include "WoTEvents.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "NavigationSystem.h"

namespace WoTBotMoverLinks
{
namespace
{
// Geometry queries use the ArenaBot capsule plus a small floor clearance. All
// terminals are measured from runtime collision, never from a map coordinate table.
constexpr float Radius = 17, HalfHeight = 46, FloorClearance = 4;
FCollisionQueryParams StaticQuery(UWorld& World, const AWoTMover* Mover)
{
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BotMoverTerminals), true, Mover);
	for (TActorIterator<APawn> It(&World); It; ++It) Q.AddIgnoredActor(*It);
	return Q;
}
bool ClearStatic(UWorld& World, const AWoTMover& Mover, const FVector& A, const FVector& B)
{
	FHitResult Hit;
	const FVector Height(0, 0, HalfHeight + FloorClearance);
	return !World.SweepSingleByChannel(Hit, A + Height, B + Height, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(Radius, HalfHeight), StaticQuery(World, &Mover));
}
bool StationaryFloor(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data,
	const AWoTMover& Mover, const FVector& Guess, FVector& Floor)
{
	FHitResult Hit;
	if (!World.LineTraceSingleByChannel(Hit, Guess + FVector(0, 0, 24), Guess - FVector(0, 0, 32),
		ECC_Pawn, StaticQuery(World, &Mover)) || Hit.ImpactNormal.Z < .7 || Cast<AWoTMover>(Hit.GetActor())) return false;
	FNavLocation Projected;
	if (!Nav.ProjectPointToNavigation(Hit.ImpactPoint, Projected, FVector(32, 32, 24), &Data)
		|| FMath::Abs(Projected.Location.Z - Guess.Z) > 24) return false;
	Floor = Projected.Location;
	return ClearStatic(World, Mover, Floor, Floor);
}

bool LiftTerminals(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data,
	AWoTMover& Mover, FVector& Entry, FVector& Exit, FVector& Deck)
{
	const FVector Offset = Mover.GetActorLocation() - Mover.GetBasePosition();
	const FBox Actual = Mover.GetComponentsBoundingBox(true);
	const FBox Bounds(Actual.Min - Offset, Actual.Max - Offset);
	const FVector Rise = Mover.KeyPos.Last();
	TArray<FVector> Decks;
	float Lowest = MAX_flt;
	for (const float X : {1.f/6, .5f, 5.f/6}) for (const float Y : {1.f/6, .5f, 5.f/6})
	{
		const FVector P(FMath::Lerp(Actual.Min.X, Actual.Max.X, X), FMath::Lerp(Actual.Min.Y, Actual.Max.Y, Y), 0);
		FHitResult Hit;
		if (Mover.Mesh->LineTraceComponent(Hit, FVector(P.X, P.Y, Actual.Max.Z + 10),
			FVector(P.X, P.Y, Actual.Min.Z - 10), FCollisionQueryParams(SCENE_QUERY_STAT(BotDeck), true))
			&& Hit.ImpactNormal.Z > .7)
		{
			Decks.Add(Hit.ImpactPoint - Offset);
			Lowest = FMath::Min(Lowest, float(Hit.ImpactPoint.Z - Offset.Z));
		}
	}
	float Best = MAX_flt;
	for (const FVector& D : Decks)
	{
		if (D.Z > Lowest + 2) continue; // The post's top is not the passenger deck.
		// A capsule must fit beside the post at the observed mover pose.
		FCollisionQueryParams Q(SCENE_QUERY_STAT(BotDeckClearance), true);
		for (TActorIterator<APawn> It(&World); It; ++It) Q.AddIgnoredActor(*It);
		if (World.OverlapBlockingTestByChannel(D + Offset + FVector(0, 0, HalfHeight + FloorClearance),
			FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, HalfHeight), Q)) continue;
		TArray<FVector> Lower, Upper;
		for (const FVector& G : {FVector(Bounds.Min.X - 64, D.Y, D.Z), FVector(Bounds.Max.X + 64, D.Y, D.Z),
			FVector(D.X, Bounds.Min.Y - 64, D.Z), FVector(D.X, Bounds.Max.Y + 64, D.Z)})
		{
			FVector P;
			if (StationaryFloor(World, Nav, Data, Mover, G, P) && ClearStatic(World, Mover, P, D)) Lower.Add(P);
			if (StationaryFloor(World, Nav, Data, Mover, G + Rise, P) && ClearStatic(World, Mover, D + Rise, P)) Upper.Add(P);
		}
		for (const FVector& A : Lower) for (const FVector& B : Upper)
		{
			// A real sweep at the current deck level excludes routes through its
			// post. The remainder was checked against stationary geometry above.
			FHitResult Hit;
			const FVector H(0, 0, HalfHeight + FloorClearance);
			if (World.SweepSingleByChannel(Hit, A + Offset + H, D + Offset + H, FQuat::Identity,
				ECC_Pawn, FCollisionShape::MakeCapsule(Radius, HalfHeight), Q)) continue;
			const float Cost = FVector::Dist2D(A, D) + FVector::Dist2D(D, B);
			if (Cost < Best) { Best = Cost; Entry = A; Exit = B; Deck = D; }
		}
	}
	return Best < MAX_flt;
}

bool DoorTerminals(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data,
	AWoTMover& Mover, AWoTTrigger& Trigger, FVector& Entry, FVector& Exit)
{
	const FBox Box = Mover.GetComponentsBoundingBox(true);
	const FVector Center = Box.GetCenter();
	FVector Out = Box.GetExtent().X < Box.GetExtent().Y ? FVector::ForwardVector : FVector::RightVector;
	if (FVector::DotProduct(Trigger.GetActorLocation() - Center, Out) > 0) Out *= -1;
	FNavLocation Start;
	if (!Nav.ProjectPointToNavigation(Trigger.GetActorLocation(), Start, FVector(50, 50, 150), &Data)) return false;
	Entry = Start.Location;
	const float Thickness = FMath::Min(Box.GetExtent().X, Box.GetExtent().Y);
	// The trigger must actually be on this side, and reachable by a walking
	// body here. Event/Tag equality alone never licenses an exterior entrance.
	if (FVector::DotProduct(Entry - Center, Out) >= -Thickness - Radius
		|| FVector::Dist2D(Entry, Trigger.GetActorLocation()) >= Trigger.UE1CollisionRadius
		|| FMath::Abs(Entry.Z + HalfHeight - Trigger.GetActorLocation().Z) > Trigger.UE1CollisionHalfHeight + HalfHeight) return false;
	const FVector Portal(Center.X, Center.Y, Entry.Z);
	if (!StationaryFloor(World, Nav, Data, Mover, Portal + Out * (Thickness + 80), Exit)) return false;
	return ClearStatic(World, Mover, Entry, Exit);
}

// Waiting spots for a bot whose turn has not come: stationary floor to either
// side of the entry or behind it, clear of the deck's footprint by a capsule,
// a straight walk from the entry and near enough that the handler still holds
// it there (its displacement reach is 120).
TArray<FVector> HoldSpots(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data,
	const AWoTMover& Mover, const FVector& Entry, const FVector& Deck)
{
	const FBox Box = Mover.GetComponentsBoundingBox(true);
	const FVector Toward = (Deck - Entry).GetSafeNormal2D();
	const FVector Across(-Toward.Y, Toward.X, 0);
	TArray<FVector> Spots;
	for (const FVector& Offset : {Across * 80, -Across * 80, -Toward * 80})
	{
		FVector Spot;
		if (!StationaryFloor(World, Nav, Data, Mover, Entry + Offset, Spot)) continue;
		if (Spot.X > Box.Min.X - Radius * 2 && Spot.X < Box.Max.X + Radius * 2
			&& Spot.Y > Box.Min.Y - Radius * 2 && Spot.Y < Box.Max.Y + Radius * 2) continue;
		const float Away = FVector::Dist2D(Spot, Entry);
		if (Away < Radius * 3 || Away > 100 || !ClearStatic(World, Mover, Entry, Spot)) continue;
		Spots.Add(Spot);
	}
	return Spots;
}
}

int32 Build(UWorld& World, UNavigationSystemV1& Nav, const ANavigationData& Data, int32 Agent)
{
	int32 Added = 0;
	for (TActorIterator<AWoTMover> It(&World); It; ++It)
	{
		AWoTMover& Mover = **It;
		bool bExists = false;
		for (TActorIterator<AWoTBotMoverLink> Link(&World); Link; ++Link) bExists |= Link->GetMover() == &Mover;
		if (bExists || Mover.KeyPos.Num() != 2 || Mover.bLockable || Mover.bTriggerOnceOnly || Mover.bDamageTriggered
			|| Mover.KeyPos[1].Z <= 50 || Mover.KeyPos[1].Size2D() > 1) continue;
		bool bRotates = false;
		for (const FRotator& R : Mover.KeyRot) bRotates |= !R.IsNearlyZero();
		if (bRotates) continue;
		FVector Entry, Exit, Deck = FVector::ZeroVector;
		AWoTTrigger* Trigger = nullptr;
		bool bFound = false;
		if (Mover.InitialState == EWoTMoverState::BumpOpenTimed)
			bFound = LiftTerminals(World, Nav, Data, Mover, Entry, Exit, Deck);
		else if (Mover.InitialState == EWoTMoverState::TriggerControl && !Mover.TagName.IsNone())
			for (TActorIterator<AWoTTrigger> T(&World); T; ++T)
				if (T->EventName == Mover.TagName && T->bInitiallyActive && !T->bTriggerOnceOnly
					&& (T->TriggerType == EWoTTriggerType::PlayerProximity || T->TriggerType == EWoTTriggerType::PawnProximity)
					&& DoorTerminals(World, Nav, Data, Mover, **T, Entry, Exit))
				{ Trigger = *T; bFound = true; break; }
		if (!bFound) continue;
		FActorSpawnParameters Params; Params.ObjectFlags |= RF_Transient; Params.bDeferConstruction = true;
		AWoTBotMoverLink* Link = World.SpawnActor<AWoTBotMoverLink>(AWoTBotMoverLink::StaticClass(), FTransform(Entry), Params);
		if (!Link) continue;
		const TArray<FVector> Holds = Trigger ? TArray<FVector>() : HoldSpots(World, Nav, Data, Mover, Entry, Deck);
		Link->Configure(&Mover, Trigger, Agent, Entry, Exit, Deck, Holds);
		Link->FinishSpawning(FTransform(Entry));
		UE_LOG(LogTemp, Log, TEXT("Bot mover link %s kind=%s entry=%s exit=%s deck=%s holds=%d"),
			*Mover.GetName(), Trigger ? TEXT("door") : TEXT("lift"), *Entry.ToString(), *Exit.ToString(), *Deck.ToString(), Holds.Num());
		++Added;
	}
	return Added;
}
}
