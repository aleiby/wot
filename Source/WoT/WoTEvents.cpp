#include "WoTEvents.h"

#include "WoTStageTrigger.h"
#include "WoTUE1Collision.h"
#include "Payload/WoTPayloadActivation.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"

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
	// Projected UE1 triggers are local materialization products rather than
	// replicated actors.  On a network client AActor otherwise leaves a
	// non-replicated, locally spawned actor at ROLE_Authority, which would make
	// HasAuthority() an unsafe event-chain gate.  UE1's non-simulated triggers
	// instead end at ROLE_None on clients and stay inert there.
	if (!GetIsReplicated() && GetNetMode() == NM_Client)
	{
		SetRole(ROLE_None);
		Capsule->SetGenerateOverlapEvents(false);
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTTrigger::OnOverlap);
	Capsule->OnComponentEndOverlap.AddDynamic(this, &AWoTTrigger::OnEndOverlap);
}

void AWoTTrigger::OnOverlap(UPrimitiveComponent*, AActor* Other,
                            UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	// Server-only (N4): the Trigger replicates as a simulated proxy, so each client detects its
	// own pawn overlaps — without this gate every client would fire the event chain locally and
	// flip its own bSpent/LastFireTime. Shipped .uc Touch/Trigger are non-simulated; effects reach
	// clients via the replicated actors driven.
	if (!IsWoTBootstrapWorldReady()
		|| !HasAuthority() || !bTriggerEnabled)
	{
		return;
	}
	if (!IsRelevantOverlap(Other) || bSpent)
	{
		return;
	}
	const auto Inside = [this](const AActor* Candidate)
		{ return IsWithinUE1Cylinder(Candidate); };
	const auto Enter = [this](AActor* Candidate)
		{ OnOverlap(Capsule, Candidate, nullptr, 0, false, FHitResult()); };
	if (!IsWithinUE1Cylinder(Other))
	{
		// In the containing capsule but not yet the flat cylinder: Touch fires
		// on the frame the cylinders meet.
		UE1Touches.DeferOutside(*this, Capsule, Other, Inside, Enter);
		return;
	}
	ActiveUE1Overlaps.Add(Other);
	UE1Touches.WatchInside(*this, Capsule, Other, Inside, Enter,
		[this](AActor* Toucher) { UE1UnTouch(Toucher); });
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now - LastFireTime < ReTriggerDelay)
	{
		return;
	}
	LastFireTime = Now;
	bSpent = bTriggerOnceOnly;
	APawn* EventInstigator = Cast<APawn>(Other);
	if (!EventInstigator && Other)
	{
		EventInstigator = Other->GetInstigator();
	}
	FireTargetEvent(Other, EventInstigator);
	if (!bSpent) ScheduleRepeat();
}

void AWoTTrigger::OnEndOverlap(UPrimitiveComponent*, AActor* Other,
                               UPrimitiveComponent*, int32)
{
	UE1Touches.Forget(Other);
	UE1UnTouch(Other);
}

void AWoTTrigger::UE1UnTouch(AActor* Other)
{
	if (!IsWoTBootstrapWorldReady()
		|| !HasAuthority() || !bTriggerEnabled)
	{
		return;
	}
	if (ActiveUE1Overlaps.Remove(Other) > 0 && IsRelevantOverlap(Other))
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
	const APawn* Pawn = Cast<APawn>(Other);
	switch (TriggerType)
	{
	case EWoTTriggerType::PlayerProximity:
		return Pawn && Pawn->IsPlayerControlled();
	case EWoTTriggerType::PawnProximity:
		return Pawn != nullptr;
	case EWoTTriggerType::ClassProximity:
		return Other && ClassProximityType && Other->IsA(ClassProximityType);
	case EWoTTriggerType::AnyProximity:
		return IsValid(Other);
	case EWoTTriggerType::Shoot:
		// Shoot triggers enter through damage/projectile handling, not overlaps.
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
		const float Now = GetWorld()->GetTimeSeconds();
		if (Now - LastFireTime < ReTriggerDelay) continue;
		LastFireTime = Now;
		bSpent = bTriggerOnceOnly;
		APawn* EventInstigator = Cast<APawn>(Other);
		if (!EventInstigator && Other) EventInstigator = Other->GetInstigator();
		FireTargetEvent(Other, EventInstigator);
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
