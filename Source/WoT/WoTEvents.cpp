#include "WoTEvents.h"

#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "WoTStageTrigger.h"
#include "WoTUE1Collision.h"
#include "Payload/WoTPayloadActivation.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/ScopedMovementUpdate.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"

void UWoTEventSubsystem::RegisterTag(FName Tag, AActor* Actor)
{
	if (IsWoTBootstrapWorldReady() && !Tag.IsNone() && Actor)
	{
		Registry.Add(Tag, Actor);
		RegistrationOrder.Emplace(Tag, Actor);
	}
}

void UWoTEventSubsystem::UnregisterActor(AActor* Actor)
{
	if (!IsWoTBootstrapWorldReady() || !Actor)
	{
		return;
	}
	for (auto It = Registry.CreateIterator(); It; ++It)
	{
		if (It.Value().Get() == Actor)
		{
			It.RemoveCurrent();
		}
	}
	RegistrationOrder.RemoveAll([Actor](const TPair<FName, TWeakObjectPtr<AActor>>& Entry)
	{
		return Entry.Value.Get() == Actor;
	});
}

void UWoTEventSubsystem::FireEvent(FName Event, AActor* Other, APawn* EventInstigator)
{
	FireEventImpl(Event, Other, EventInstigator, false);
}

void UWoTEventSubsystem::FirePresentationEvent(
	FName Event, AActor* Other, APawn* EventInstigator)
{
	FireEventImpl(Event, Other, EventInstigator, true);
}

void UWoTEventSubsystem::FireEventImpl(FName Event, AActor* Other,
	APawn* EventInstigator, bool bLocalPresentation)
{
	if (!IsWoTBootstrapWorldReady() || Event.IsNone())
	{
		return;
	}
	TArray<TWeakObjectPtr<AActor>> Targets;
	Registry.MultiFind(Event, Targets);
	UE_LOG(LogTemp, Verbose, TEXT("WoTEvent '%s' -> %d targets"),
	       *Event.ToString(), Targets.Num());
	if (bLocalPresentation)
	{
		++LocalPresentationDispatchDepth;
	}
	for (const TWeakObjectPtr<AActor>& T : Targets)
	{
		if (IWoTTriggerable* Triggerable = Cast<IWoTTriggerable>(T.Get()))
		{
			Triggerable->WoTTrigger(Other, EventInstigator);
		}
	}
	if (bLocalPresentation)
	{
		--LocalPresentationDispatchDepth;
	}
}

void UWoTEventSubsystem::FireUnEvent(FName Event, AActor* Other, APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady() || Event.IsNone())
	{
		return;
	}
	TArray<TWeakObjectPtr<AActor>> Targets;
	Registry.MultiFind(Event, Targets);
	for (const TWeakObjectPtr<AActor>& T : Targets)
	{
		if (IWoTTriggerable* Triggerable = Cast<IWoTTriggerable>(T.Get()))
		{
			Triggerable->WoTUnTrigger(Other, EventInstigator);
		}
	}
}

void UWoTEventSubsystem::FireDoorEvent(FName Event, bool bForward, AActor* Other,
	                                    APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady() || Event.IsNone())
	{
		return;
	}
	TArray<TWeakObjectPtr<AActor>> Targets;
	Registry.MultiFind(Event, Targets);
	for (const TWeakObjectPtr<AActor>& T : Targets)
	{
		if (IWoTTriggerable* Triggerable = Cast<IWoTTriggerable>(T.Get()))
		{
			Triggerable->WoTTriggerDirectional(bForward, Other, EventInstigator);
		}
	}
}

AActor* UWoTEventSubsystem::FindFirstTagged(FName Tag) const
{
	if (!IsWoTBootstrapWorldReady() || Tag.IsNone())
	{
		return nullptr;
	}
	// Importer markers stand in for the first retail actor when that actor's
	// class is not otherwise represented. Prefer that authored-order proxy even
	// if level BeginPlay happened to register another same-tag carrier first.
	for (const TPair<FName, TWeakObjectPtr<AActor>>& Entry : RegistrationOrder)
	{
		if (Entry.Key == Tag)
		{
			if (AWoTTaggedMarker* Marker = Cast<AWoTTaggedMarker>(Entry.Value.Get()))
			{
				return Marker;
			}
		}
	}
	// For normal registry-bearing actors, BeginPlay order follows the level's
	// serialized actor order; keep that order explicitly instead of relying on
	// TMultiMap::MultiFind's unspecified value order.
	for (const TPair<FName, TWeakObjectPtr<AActor>>& Entry : RegistrationOrder)
	{
		if (Entry.Key == Tag)
		{
			if (AActor* Actor = Entry.Value.Get())
			{
				return Actor;
			}
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------

AWoTTrigger::AWoTTrigger()
{
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	RootComponent = Capsule;
	Capsule->SetCollisionProfileName(TEXT("Trigger"));
	// UE 5.8's Trigger profile leaves custom channels at their default response.
	// Our projectile channel defaults to Block; these are touch volumes, so they
	// must not displace a launch or produce a wall hit during flight.
	Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
}

void AWoTTrigger::SetUE1CollisionSize(float Radius, float HalfHeight)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHalfHeight = FMath::Max(0.0f, HalfHeight);
	if (Capsule)
	{
		Capsule->SetCapsuleSize(UE1CollisionRadius,
			WoTUE1Collision::ContainingCapsuleHalfHeight(
				UE1CollisionRadius, UE1CollisionHalfHeight), false);
	}
}

void AWoTTrigger::BeginPlay()
{
	Super::BeginPlay();
	const bool bShoot = TriggerType == EWoTTriggerType::Shoot;
	if (bShoot)
	{
		// Only shoot targets need a channel. Their transform is authored on both
		// peers; a dormant, always-relevant actor carries just retirement changes.
		SetReplicates(true);
		SetReplicateMovement(false);
		bAlwaysRelevant = true;
		SetNetDormancy(DORM_DormantAll);
		LastFireTime = 0.0f;
		OnRep_Spent();
	}
	if (GetNetMode() == NM_Client)
	{
		// Projected actors exist locally before their replication channel opens.
		// Set their role now so local materialization cannot dispatch events.
		if (bShoot) SetRole(ROLE_SimulatedProxy);
		else if (!GetIsReplicated())
		{
			SetRole(ROLE_None);
			Capsule->SetGenerateOverlapEvents(false);
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			return;
		}
	}
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	if (HasAuthority() || !bShoot) GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTTrigger::OnOverlap);
	Capsule->OnComponentEndOverlap.AddDynamic(this, &AWoTTrigger::OnEndOverlap);
}

void AWoTTrigger::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTTrigger, bSpent);
}

void AWoTTrigger::OnRep_Spent()
{
	if (TriggerType == EWoTTriggerType::Shoot && bSpent)
	{
		Capsule->SetGenerateOverlapEvents(false);
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

bool AWoTTrigger::AcceptHit(AActor* Other, APawn* EventInstigator, AWoTProjectile* Projectile)
{
	if (!IsWoTBootstrapWorldReady() || !HasAuthority() || !bInitiallyActive
		|| !bTriggerEnabled || bSpent) return false;
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now - LastFireTime < ReTriggerDelay) return false;
	if (Projectile)
	{
		if (AcceptedProjectileFrame != GFrameCounter)
		{
			AcceptedProjectiles.Reset();
			AcceptedProjectileFrame = GFrameCounter;
		}
		if (AcceptedProjectiles.Contains(Projectile)) return false;
		// One original shoot-target Touch represents one shot. The port delivers
		// the touch before ProcessTouch -> direct damage -> Explode -> splash
		// (WoTAngrealProjectile.cpp, WoTGenericProjectile.cpp). Deduplicate that
		// synchronous impact by projectile, so zero delay still accepts distinct
		// simultaneous shots. A later frame or a new contact starts a new hit;
		// surviving projectiles must be able to leave and touch again.
		AcceptedProjectiles.Add(Projectile);
	}
	LastFireTime = Now;
	if (bTriggerOnceOnly)
	{
		// Flush before changing a dormant property so existing clients and late
		// joiners receive the same spent target. No movement stream or tick.
		if (GetIsReplicated()) FlushNetDormancy();
		bSpent = true;
	}
	FireTargetEvent(Other, EventInstigator);
	if (!Message.IsEmpty() && EventInstigator)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(EventInstigator->GetController()))
			PC->ClientTypedMessage(Message, NAME_None);
		else if (APlayerController* Player = Cast<APlayerController>(EventInstigator->GetController()))
			Player->ClientMessage(Message);
	}
	// Retire before another hit can be accepted, but release existing touches
	// only after their event and message, preserving trigger/untrigger order.
	if (bSpent) OnRep_Spent();
	return true;
}

float AWoTTrigger::TakeDamage(float DamageAmount, const FDamageEvent&,
	AController* EventInstigator, AActor* DamageCauser)
{
	if (TriggerType != EWoTTriggerType::Shoot || DamageAmount < DamageThreshold) return 0.0f;
	APawn* Pawn = EventInstigator ? EventInstigator->GetPawn() : nullptr;
	// The projectile damage entry point receives a pawn, which need not have
	// a controller. Retain that instigator when adapting to AActor::TakeDamage.
	if (!Pawn && DamageCauser) Pawn = DamageCauser->GetInstigator();
	return Pawn && AcceptHit(Pawn, Pawn, Cast<AWoTProjectile>(DamageCauser)) ? DamageAmount : 0.0f;
}

namespace
{
bool ShootTargetSweepContact(const AWoTTrigger& Target, const AActor& Projectile,
	const FVector& Start, FVector& Contact)
{
	float ProjectileRadius = 0.0f;
	float ProjectileHalfHeight = 0.0f;
	Projectile.GetSimpleCollisionCylinder(ProjectileRadius, ProjectileHalfHeight);
	const double Radius = Target.UE1CollisionRadius + FMath::Max(0.0f, ProjectileRadius);
	const double HalfHeight = Target.UE1CollisionHalfHeight + FMath::Max(0.0f, ProjectileHalfHeight);
	const FVector Offset = Start - Target.GetActorLocation();
	// TraceEnd is the requested destination and can lie beyond a blocking hit.
	// Only the segment actually travelled may activate a target.
	const FVector Delta = Projectile.GetActorLocation() - Start;
	double Enter = 0.0;
	double Leave = 1.0;
	const double A = Delta.SizeSquared2D();
	const double C = Offset.SizeSquared2D() - Radius * Radius;
	if (A <= UE_DOUBLE_SMALL_NUMBER)
	{
		if (C > 0.0) return false;
	}
	else
	{
		const double B = Offset.X * Delta.X + Offset.Y * Delta.Y;
		const double Discriminant = B * B - A * C;
		if (Discriminant < 0.0) return false;
		const double Root = FMath::Sqrt(Discriminant);
		Enter = FMath::Max(Enter, (-B - Root) / A);
		Leave = FMath::Min(Leave, (-B + Root) / A);
	}
	// Intersect the radial interval with the flat-cap slab, including vertical
	// and stationary segments without dividing by a zero direction component.
	if (FMath::Abs(Delta.Z) <= UE_DOUBLE_SMALL_NUMBER)
	{
		if (FMath::Abs(Offset.Z) > HalfHeight) return false;
	}
	else
	{
		const double First = (-HalfHeight - Offset.Z) / Delta.Z;
		const double Last = (HalfHeight - Offset.Z) / Delta.Z;
		Enter = FMath::Max(Enter, FMath::Min(First, Last));
		Leave = FMath::Min(Leave, FMath::Max(First, Last));
	}
	if (Enter > Leave) return false;
	Contact = Start + Delta * Enter;
	return true;
}
}

void AWoTTrigger::OnOverlap(UPrimitiveComponent*, AActor* Other,
                            UPrimitiveComponent*, int32, bool bFromSweep, const FHitResult& Sweep)
{
	const bool bShoot = TriggerType == EWoTTriggerType::Shoot;
	if (!IsWoTBootstrapWorldReady() || !IsValid(Other) || bSpent
		|| (bShoot ? !Other->IsA<AWoTProjectile>()
			: (!HasAuthority() || !bTriggerEnabled || !IsRelevantOverlap(Other))))
	{
		return;
	}
	const auto Inside = [this](const AActor* Candidate)
		{
			if (IsWithinUE1Cylinder(Candidate)) return true;
			if (FVector* Previous = PendingShootLocations.Find(Candidate))
			{
				FVector Point;
				if (ShootTargetSweepContact(*this, *Candidate, *Previous, Point)) return true;
				*Previous = Candidate->GetActorLocation();
			}
			return false;
		};
	const auto Enter = [this](AActor* Candidate)
		{
			FHitResult DeferredSweep;
			const FVector* Previous = PendingShootLocations.Find(Candidate);
			if (Previous) DeferredSweep.TraceStart = *Previous;
			OnOverlap(Capsule, Candidate, nullptr, 0, Previous != nullptr, DeferredSweep);
		};
	FVector Contact = Other->GetActorLocation();
	const bool bSweptContact = bShoot && bFromSweep
		&& ShootTargetSweepContact(*this, *Other, Sweep.TraceStart, Contact);
	if (!bSweptContact && !IsWithinUE1Cylinder(Other))
	{
		// In the containing capsule but not yet the flat cylinder: Touch fires
		// on the frame the cylinders meet.
		// Keep a previous sample only for these projectile/capsule contacts. The
		// existing deferred poll can then catch a crossing between later endpoints
		// without adding traces or ticking idle targets or unrelated projectiles.
		if (bShoot) PendingShootLocations.Add(Other, Other->GetActorLocation());
		UE1Touches.DeferOutside(*this, Capsule, Other, Inside, Enter);
		return;
	}
	if (bShoot)
	{
		if (ActiveUE1Overlaps.Contains(Other)) return;
		PendingShootLocations.Remove(Other);
		ActiveUE1Overlaps.Add(Other);
		AcceptedProjectiles.Remove(Cast<AWoTProjectile>(Other));
		// UE has already moved to the step endpoint before notifying overlaps.
		// Rewind this impact to its first cylinder contact so damage, splash and
		// simulated teardown share that location. Defer overlap updates until the
		// existing exactly-once impact finishes, avoiding a nested touch on rewind.
		FScopedMovementUpdate ImpactMove(Other->GetRootComponent(), EScopedUpdate::DeferredUpdates);
		if (bSweptContact) Other->SetActorLocation(Contact, false);
		UE1Touches.WatchInside(*this, Capsule, Other, Inside, Enter,
			[this](AActor* Toucher) { UE1UnTouch(Toucher); });
		if (IsRelevantOverlap(Other)
			&& AcceptHit(Other, Other->GetInstigator(), Cast<AWoTProjectile>(Other)) && !bSpent)
			ScheduleRepeat();
		// Both component notification orders come here. Finish the impact even
		// if accepting this touch just disabled the target's collision. Weak or
		// inactive hits still stop here; clients run only simulated teardown.
		if (AWoTAngrealProjectile* Projectile = Cast<AWoTAngrealProjectile>(Other); IsValid(Projectile))
			Projectile->ProcessTouch(this, Projectile->GetActorLocation());
		return;
	}
	ActiveUE1Overlaps.Add(Other);
	UE1Touches.WatchInside(*this, Capsule, Other, Inside, Enter,
		[this](AActor* Toucher) { UE1UnTouch(Toucher); });
	APawn* EventInstigator = Cast<APawn>(Other);
	if (!EventInstigator && Other)
	{
		EventInstigator = Other->GetInstigator();
	}
	if (AcceptHit(Other, EventInstigator) && !bSpent) ScheduleRepeat();
}

void AWoTTrigger::OnEndOverlap(UPrimitiveComponent*, AActor* Other,
                               UPrimitiveComponent*, int32)
{
	// A crossing can end outside the capsule before its deferred poll runs.
	// Resolve that final segment before discarding the pending contact.
	if (IsValid(Other))
	{
		if (const FVector* Previous = PendingShootLocations.Find(Other))
		{
			FHitResult DeferredSweep;
			DeferredSweep.TraceStart = *Previous;
			OnOverlap(Capsule, Other, nullptr, 0, true, DeferredSweep);
		}
	}
	PendingShootLocations.Remove(Other);
	UE1Touches.Forget(Other);
	UE1UnTouch(Other);
}

void AWoTTrigger::UE1UnTouch(AActor* Other)
{
	const bool bWasTouching = ActiveUE1Overlaps.Remove(Other) > 0;
	if (!IsWoTBootstrapWorldReady()
		|| !HasAuthority() || !bTriggerEnabled)
	{
		return;
	}
	// Release an accepted touch even after possession or collision changes.
	// Pawn teardown can clear possession before it ends the overlap.
	if (bWasTouching && (TriggerType != EWoTTriggerType::Shoot || IsRelevantOverlap(Other)))
	{
		APawn* P = Cast<APawn>(Other);
		if (!P && Other)
		{
			P = Other->GetInstigator();
		}
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireUnEvent(EventName, Other, P);
	}
}

bool AWoTTrigger::IsRelevantOverlap(AActor* Other) const
{
	if (!bInitiallyActive) return false;
	const APawn* Pawn = Cast<APawn>(Other);
	switch (TriggerType)
	{
	case EWoTTriggerType::PlayerProximity:
		// The engine Trigger's player-proximity test reads the toucher's bIsPlayer
		// (as ElevatorTrigger.uc:38 spells it out): a player body, whoever drives
		// it, so an AI-driven player body fires the trigger and an NPC does not.
		return WoTIsPlayerBody(Pawn);
	case EWoTTriggerType::PawnProximity:
		return Pawn != nullptr;
	case EWoTTriggerType::ClassProximity:
		return Other && ClassProximityType && Other->IsA(ClassProximityType);
	case EWoTTriggerType::AnyProximity:
		return IsValid(Other);
	case EWoTTriggerType::Shoot:
		if (const AWoTProjectile* Projectile = Cast<AWoTProjectile>(Other))
			return Projectile->Damage >= DamageThreshold;
		return false;
	default:
		return false;
	}
}

void AWoTTrigger::SetTriggerEnabled(bool bEnable)
{
	// UE1 probe semantics (DoorMover.uc:85-104, TimedMover.uc:23-44): Disable/
	// Enable('Touch') masks the notification only — re-enabling does NOT re-fire
	// Touch for an actor still in the touching list; it must exit and re-enter.
	// Toggling actor collision here would re-run UpdateOverlaps on enable and
	// synthesize a Begin-overlap for a pawn camping the volume, cycling its door
	// forever. So collision stays on and this flag masks OnOverlap/OnEndOverlap
	// instead: a Begin that arrives while disabled is dropped for good, exactly
	// like a masked UE1 probe, and the engine's own end-overlap bookkeeping keeps
	// working (pawn death/destruction inside the volume leaves no stale state).
	bTriggerEnabled = bEnable;
}

bool AWoTTrigger::IsWithinUE1Cylinder(const AActor* Other) const
{
	return WoTUE1Collision::CylindersOverlap(
		GetActorLocation(), UE1CollisionRadius, UE1CollisionHalfHeight, Other);
}

void AWoTTrigger::ScheduleRepeat()
{
	if (RepeatTriggerTime > 0.0f)
	{
		GetWorldTimerManager().SetTimer(RepeatTimerHandle, this,
			&AWoTTrigger::OnRepeatTimer, RepeatTriggerTime, false);
	}
}

void AWoTTrigger::OnRepeatTimer()
{
	if (!IsWoTBootstrapWorldReady() || !HasAuthority()
		|| !bTriggerEnabled || bSpent || !Capsule) return;
	TArray<AActor*> Overlapping;
	Capsule->GetOverlappingActors(Overlapping);
	bool bKeepTiming = false;
	for (AActor* Other : Overlapping)
	{
		if (!IsRelevantOverlap(Other) || !IsWithinUE1Cylinder(Other)) continue;
		bKeepTiming = true;
		APawn* EventInstigator = Cast<APawn>(Other);
		if (!EventInstigator && Other) EventInstigator = Other->GetInstigator();
		AcceptHit(Other, EventInstigator);
		if (bSpent) break;
	}
	if (bKeepTiming && !bSpent) ScheduleRepeat();
}

void AWoTTrigger::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady()
		|| !HasAuthority() || !bTriggerEnabled)   // server-only relay (N4)
	{
		return;
	}
	// triggers can themselves be triggered (relay)
	FireTargetEvent(Other, EventInstigator);
}

void AWoTTrigger::FireTargetEvent(AActor* Other, APawn* EventInstigator)
{
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, Other, EventInstigator);
}

// ---------------------------------------------------------------------------

AWoTTaggedMarker::AWoTTaggedMarker()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetActorEnableCollision(false);
}

void AWoTTaggedMarker::BeginPlay()
{
	Super::BeginPlay();
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
}

// ---------------------------------------------------------------------------

void AWoTDispatcher::BeginPlay()
{
	Super::BeginPlay();
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
}

void AWoTDispatcher::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady() || !bTriggerEnabled)
	{
		return;
	}
	if (!HasAuthority())   // server-only chain (N4) — no per-client timers/dispatch
	{
		return;
	}
	for (int32 i = 0; i < OutEvents.Num(); i++)
	{
		if (OutEvents[i].IsNone())
		{
			continue;
		}
		const float Delay = OutDelays.IsValidIndex(i) ? OutDelays[i] : 0.0f;
		FName Event = OutEvents[i];
		TWeakObjectPtr<APawn> Inst = EventInstigator;
		TWeakObjectPtr<AActor> LastOther = Other;
		auto Fire = [this, Event, Inst, LastOther]()
		{
			if (!IsWoTBootstrapWorldReady())
			{
				return;
			}
			GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
				Event, this, Inst.Get());
			for (TActorIterator<AWoTStageTrigger> It(GetWorld()); It; ++It)
			{
				if (It->TagInc == Event)
				{
					It->ChangeStage(LastOther.Get(), 1);
				}
				if (It->TagDec == Event)
				{
					It->ChangeStage(LastOther.Get(), -1);
				}
			}
		};
		if (Delay <= 0.0f)
		{
			Fire();
		}
		else
		{
			FTimerHandle H;
			GetWorld()->GetTimerManager().SetTimer(
				H, FTimerDelegate::CreateWeakLambda(this, Fire), Delay, false);
		}
	}
}

// ---------------------------------------------------------------------------

void AWoTCounter::BeginPlay()
{
	Super::BeginPlay();
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	Remaining = NumToCount;
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
}

void AWoTCounter::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady()
		|| !HasAuthority())   // server-only count (N4) — Remaining must not diverge per client
	{
		return;
	}
	if (Remaining > 0 && --Remaining == 0)
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			EventName, Other, EventInstigator);
	}
}
