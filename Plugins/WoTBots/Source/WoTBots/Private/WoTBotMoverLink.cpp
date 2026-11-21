#include "WoTBotMoverLink.h"
#include "WoTBotController.h"
#include "WoTCharacter.h"
#include "WoTMover.h"
#include "WoTEvents.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTBotMover, Log, All);

namespace
{
// Knock-back, a swap or the bot's own cast can set a body down away from where
// its stage holds it. Farther than this (2D) and the traversal no longer
// describes the body: the handler lets it go and the bot plans from there.
constexpr float DisplacedDistance = 120.0f;
// A rider standing on its deck while the deck rests at the bottom (it came back
// after a missed exit, or never left) steps off after this long.
constexpr float StoppedOnDeckSeconds = 1.0f;

void Steer(AWoTCharacter& Body, const FVector& Feet)
{
	const FVector Delta = Feet - Body.GetActorLocation();
	const float Distance = Delta.Size2D();
	UCharacterMovementComponent* Move = Body.GetCharacterMovement();
	if (Distance < 10) Move->StopMovementImmediately();
	else Move->RequestDirectMove(Delta.GetSafeNormal2D() * FMath::Min(Move->MaxWalkSpeed, Distance * 6.0f), false);
}
bool ClearPassage(AWoTCharacter& Body, const FVector& Feet)
{
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(BotMoverPassage), false, &Body);
	const auto* Capsule = Body.GetCapsuleComponent();
	return !Body.GetWorld()->SweepSingleByChannel(Hit, Body.GetActorLocation(),
		Feet + FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight() + 3), FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight()), Query);
}
}

bool UWoTBotMoverLinkComponent::IsLinkPathfindingAllowed(const UObject* Querier) const
{
	// A move request names its controller; a component or pawn querier names its owner.
	if (const UActorComponent* Component = Cast<UActorComponent>(Querier)) Querier = Component->GetOwner();
	if (const APawn* Pawn = Cast<APawn>(Querier)) Querier = Pawn->GetController();
	const AWoTBotController* Bot = Cast<AWoTBotController>(Querier);
	return !Bot || Bot->CanPlanThroughMoverLink(Cast<AWoTBotMoverLink>(GetOwner()));
}

AWoTBotMoverLink::AWoTBotMoverLink(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UWoTBotMoverLinkComponent>(TEXT("SmartLinkComp")))
{
	PointLinks.Reset();
	bSmartLinkIsRelevant = true;
	SetReplicates(false);
	OnSmartLinkReached.AddDynamic(this, &ThisClass::Reached);
}

void AWoTBotMoverLink::Configure(AWoTMover* InMover, AWoTTrigger* InTrigger, int32 Agent,
	const FVector& Entry, const FVector& Exit, const FVector& Deck, const TArray<FVector>& InHolds)
{
	Mover = InMover; Trigger = InTrigger; bLift = !InTrigger;
	EntryFeet = Entry; ExitFeet = Exit;
	BottomPosition = InMover->GetBasePosition();
	TopPosition = BottomPosition + InMover->KeyPos.Last();
	DeckOffset = Deck - BottomPosition;
	Holds = InHolds;
	const FBox Box = InMover->GetComponentsBoundingBox(true);
	FootprintMin = FVector2D(Box.Min.X, Box.Min.Y); FootprintMax = FVector2D(Box.Max.X, Box.Max.Y);
	// A body retreating off the deck starts as far from the entry as the deck point.
	RetreatReach = FMath::Max(DisplacedDistance, float(FVector::Dist2D(Entry, Deck)) + 40.0f);
	FNavAgentSelector Supported(0); Supported.Set(Agent); Supported.MarkInitialized();
	GetSmartLinkComp()->SetSupportedAgents(Supported);
	GetSmartLinkComp()->SetLinkData(Entry - GetActorLocation(), Exit - GetActorLocation(), ENavLinkDirection::LeftToRight);
}

bool AWoTBotMoverLink::InFootprint(const FVector& Point, float Radius) const
{
	return Point.X > FootprintMin.X - Radius && Point.X < FootprintMax.X + Radius
		&& Point.Y > FootprintMin.Y - Radius && Point.Y < FootprintMax.Y + Radius;
}

FVector AWoTBotMoverLink::HoldFor(const AWoTBotController* Bot, const FVector& From) const
{
	// The least used waiting spot, then the nearest; the entry only when none qualified.
	FVector Best = EntryFeet;
	int32 BestUsers = MAX_int32;
	double BestDistance = DBL_MAX;
	for (const FVector& Hold : Holds)
	{
		int32 Users = 0;
		for (const auto& Pair : Traversals)
			if (Pair.Key.Get() != Bot && Pair.Value.Stage == EStage::Contended && Pair.Value.Hold.Equals(Hold)) ++Users;
		const double Distance = FVector::DistSquared2D(From, Hold);
		if (Users < BestUsers || (Users == BestUsers && Distance < BestDistance))
		{ Best = Hold; BestUsers = Users; BestDistance = Distance; }
	}
	return Best;
}

void AWoTBotMoverLink::Reached(AActor* Actor, const FVector&)
{
	APawn* Pawn = Cast<APawn>(Actor);
	AWoTBotController* Bot = Pawn ? Cast<AWoTBotController>(Pawn->GetController()) : nullptr;
	if (!Bot || !Mover.IsValid() || !Bot->BeginMoverTraversal(this))
	{
		ResumePathFollowing(Actor);
		return;
	}
	FTraversal& T = Traversals.FindOrAdd(Bot);
	T = FTraversal();
	T.Pawn = Pawn;
	T.Started = T.StageStarted = T.LastMotion = GetWorld()->GetTimeSeconds();
	T.LastPosition = Mover->GetActorLocation();
	T.Stage = bLift && Rider.IsValid() && Rider != Bot ? EStage::Contended : EStage::Wait;
	T.Hold = T.Stage == EStage::Contended ? HoldFor(Bot, Pawn->GetActorLocation()) : EntryFeet;
	if (bLift && T.Stage != EStage::Contended) Rider = Bot;
}

void AWoTBotMoverLink::UpdateTraversal(AWoTBotController* Bot, float)
{
	FTraversal* T = Traversals.Find(Bot);
	AWoTCharacter* Body = Cast<AWoTCharacter>(Bot->GetPawn());
	if (!T || !Body || T->Pawn != Body || !Mover.IsValid() || !Body->IsAlive())
	{ Finish(Bot, false); return; }
	const float Now = GetWorld()->GetTimeSeconds();
	const FVector Position = Mover->GetActorLocation();
	if (!Position.Equals(T->LastPosition, .02f)) T->LastMotion = Now;
	T->LastPosition = Position;
	const bool bStopped = Now - T->LastMotion > .06f;
	const auto* Base = Cast<UPrimitiveComponent>(Body->GetMovementBaseObject());
	const bool bOnDeck = Base && Base->GetOwner() == Mover.Get();
	const bool bGrounded = Body->GetCharacterMovement()->IsMovingOnGround();
	const FVector Here = Body->GetActorLocation();
	const float FeetZ = Here.Z - Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const auto Stage = [&](EStage Next) { T->Stage = Next; T->StageStarted = Now; T->BottomSince = -1; };
	if (Now - T->Started > 24 || Now - T->StageStarted > 12)
	{ Finish(Bot, false); return; }
	// Waiting stages hold the body beside the entry (or its own waiting spot); a
	// retreat holds it near the deck it is leaving. Anywhere else it was displaced.
	if ((T->Stage == EStage::Wait && FVector::Dist2D(Here, EntryFeet) > DisplacedDistance)
		|| (T->Stage == EStage::Contended && FVector::Dist2D(Here, T->Hold) > DisplacedDistance)
		|| (T->Stage == EStage::Retreat && !bOnDeck && FVector::Dist2D(Here, EntryFeet) > RetreatReach))
	{ Finish(Bot, false); return; }
	if (T->Stage == EStage::Contended)
	{
		// A second body waits at its own spot on stationary floor, beside the entry
		// and outside the shaft: the entry stays clear for the rider and anyone else.
		// The same finite budget applies even if the rider never releases it.
		Steer(*Body, T->Hold);
		if (!Rider.IsValid()) { Rider = Bot; Stage(EStage::Wait); }
		return;
	}
	if (!bLift)
	{
		if (!Trigger.IsValid()) { Finish(Bot, false); return; }
		// Entry is on the trigger's own side. Walk into its real cylinder and
		// keep the touch asserted until the capsule can pass the observed door.
		if (T->Stage == EStage::Wait)
		{
			Steer(*Body, EntryFeet);
			if (ClearPassage(*Body, ExitFeet)) Stage(EStage::Exit);
		}
		else
		{
			Steer(*Body, ExitFeet);
			if (FVector::Dist2D(Here, ExitFeet) < 25 && bGrounded) Finish(Bot, true);
		}
		return;
	}
	const FVector Deck = Position + DeckOffset;
	const bool bAtBottom = bStopped && Position.Equals(BottomPosition, 1.0f);
	switch (T->Stage)
	{
	case EStage::Wait:
		Steer(*Body, EntryFeet);
		if (bAtBottom) Stage(EStage::Board);
		break;
	case EStage::Board:
		if (bOnDeck) { Stage(EStage::Ride); Steer(*Body, Deck); }
		else if (Position.Z > BottomPosition.Z + 20 || Now - T->StageStarted > 3)
			Stage(EStage::Retreat);
		else Steer(*Body, Deck);
		break;
	case EStage::Ride:
		if (!bOnDeck) { Finish(Bot, false); return; }
		Steer(*Body, Deck);
		if (bStopped && Position.Equals(TopPosition, 1.0f) && ClearPassage(*Body, ExitFeet)) Stage(EStage::Exit);
		else if (!bAtBottom) T->BottomSince = -1;
		else if (T->BottomSince < 0) T->BottomSince = Now;
		// The deck rests at its bottom with the rider on it: it came back after an
		// exit blocked at the top, or never left. Waiting there only spends the
		// stage budget, so step off (a bump-delayed start gets its delay too).
		else if (Now - T->BottomSince > StoppedOnDeckSeconds + Mover->DelayTime) Stage(EStage::Retreat);
		break;
	case EStage::Exit:
	{
		Steer(*Body, ExitFeet);
		const bool bAtExitHeight = bGrounded && !bOnDeck && FMath::Abs(FeetZ - ExitFeet.Z) < 25;
		if (bAtExitHeight && FVector::Dist2D(Here, ExitFeet) < 30) Finish(Bot, true);
		else if (Position.Z < TopPosition.Z - 20)
		{
			// The deck left. A body still on it rides on; one already standing on
			// the landing has arrived, wherever another body left it room.
			if (bOnDeck) Stage(EStage::Ride);
			else if (bAtExitHeight) Finish(Bot, true);
			else if (bGrounded) Finish(Bot, false);
		}
		else if (!bOnDeck && bGrounded && FeetZ < ExitFeet.Z - 40) Finish(Bot, false); // knocked off the landing
		break;
	}
	case EStage::Retreat:
		// Stationary floor anywhere outside the deck's footprint ends a retreat;
		// the entry only gives it a direction, and may itself be occupied.
		Steer(*Body, EntryFeet);
		if (!bOnDeck && bGrounded && !InFootprint(Here, Body->GetCapsuleComponent()->GetScaledCapsuleRadius())) Finish(Bot, false);
		break;
	default: break;
	}
}

void AWoTBotMoverLink::Cancel(AWoTBotController* Bot)
{
	Traversals.Remove(Bot);
	if (Rider == Bot) Rider.Reset();
	ResumePathFollowing(Bot->GetPawn());
}

void AWoTBotMoverLink::Finish(AWoTBotController* Bot, bool bSuccess)
{
	if (bSuccess) { if (bLift) ++LiftRides; else ++DoorExits; }
	else ++Abandons;
	const FTraversal* T = Traversals.Find(Bot);
	UE_LOG(LogWoTBotMover, Log, TEXT("Bot mover %s %s bot=%s stage=%d body=%s mover=%s"), *GetNameSafe(Mover.Get()),
		bSuccess ? TEXT("completed") : TEXT("abandoned"), *Bot->GetName(), T ? int32(T->Stage) : -1,
		Bot->GetPawn() ? *Bot->GetPawn()->GetActorLocation().ToString() : TEXT("missing"),
		Mover.IsValid() ? *Mover->GetActorLocation().ToString() : TEXT("missing"));
	Cancel(Bot);
	Bot->FinishMoverTraversal(this, bSuccess);
}

void AWoTBotMoverLink::EndPlay(const EEndPlayReason::Type Reason)
{
	TArray<TWeakObjectPtr<AWoTBotController>> Bots; Traversals.GetKeys(Bots);
	for (const auto& Bot : Bots) if (Bot.IsValid()) Bot->CancelMoverTraversal();
	Super::EndPlay(Reason);
}
