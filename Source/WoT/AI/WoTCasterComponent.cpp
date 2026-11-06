#include "WoTCasterComponent.h"
#include "../WoTRandom.h"

#include "WoTSight.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTProjectileLauncher.h"
#include "../Projectiles/WoTAngrealProjectile.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "WoTNPCController.h"
#include "../WoTCharacter.h"

UWoTCasterComponent::UWoTCasterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UWoTCasterComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		BuildLoadout();
	}
}

UWoTInventoryComponent* UWoTCasterComponent::GetInventory() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
}

void UWoTCasterComponent::BuildLoadout()
{
	// AddDefaultInventoryItems (AngrealUser.uc:87): spawn each weave as a
	// held item straight into the inventory, priming per-slot overrides only
	// where the sentinel is set. Same held-spawn pattern as the game mode's
	// grant (never enters the collidable Pickup state).
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	UWoTInventoryComponent* Inv = GetInventory();
	if (!OwnerPawn || !Inv)
	{
		return;
	}
	for (const FWoTWeaveSlot& Slot : Weaves)
	{
		if (!Slot.WeaveClass)
		{
			continue;
		}
		const FTransform Xform(OwnerPawn->GetActorRotation(), OwnerPawn->GetActorLocation());
		AWoTAngrealInventory* Weave = GetWorld()->SpawnActorDeferred<AWoTAngrealInventory>(
			Slot.WeaveClass, Xform, OwnerPawn, OwnerPawn,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Weave)
		{
			continue;
		}
		Weave->bHeldItem = true;
		Weave->FinishSpawning(Xform);
		if (Slot.Priority != -1.0f)
		{
			Weave->Priority = Slot.Priority;
		}
		if (Slot.MinChargeGroupInterval != -1.0f)
		{
			Weave->MinChargeGroupInterval = Slot.MinChargeGroupInterval;
		}
		if (Slot.MinInitialCharges != -1 && Slot.MaxInitialCharges != -1)
		{
			Weave->CurCharges = WoTRandom::RandRange(Slot.MinInitialCharges, Slot.MaxInitialCharges);
		}
		if (Slot.MinChargesInGroup != -1)
		{
			Weave->MinChargesInGroup = Slot.MinChargesInGroup;
		}
		if (Slot.MaxChargesInGroup != -1)
		{
			Weave->MaxChargesInGroup = Slot.MaxChargesInGroup;
		}
		Weave->GiveTo(OwnerPawn);
	}
}

AWoTAngrealInventory* UWoTCasterComponent::SelectOffensiveWeave(AActor* Victim)
{
	// SelectOffensiveAngreal (AngrealWeaponAdapter.uc:319): interval gate,
	// then the highest GetEffectivePriority among bOffensive items with the
	// victim set. (bRequireTargetVisibility ships false — no visibility gate
	// at selection; the volley check enforces it during the cast.)
	UWoTInventoryComponent* Inv = GetInventory();
	const UWorld* World = GetWorld();
	const AWoTCharacter* OwnerCharacter = Cast<AWoTCharacter>(GetOwner());
	if (!Inv || !World || !Victim ||
		(OwnerCharacter && OwnerCharacter->IsCastingBlocked()) ||
	    World->GetTimeSeconds() <= LastOffensiveUsageTime + MinOffensiveUsageInterval)
	{
		return nullptr;
	}
	AWoTAngrealInventory* Best = nullptr;
	float BestPriority = 0.0f;
	for (AWoTInventory* Item : Inv->Items)
	{
		AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item);
		if (!Weave || Weave == AdaptedWeave || !Weave->bOffensive)
		{
			continue;
		}
		Weave->SetVictim(Victim);
		float Priority = 0.0f;
		if (Weave->GetEffectivePriority(Priority) && (!Best || Priority > BestPriority))
		{
			Best = Weave;
			BestPriority = Priority;
		}
	}
	if (Best)
	{
		// DetermineOffensiveUsage (AngrealWeaponAdapter.uc:246).
		Best->InitChargeGroup();
	}
	return Best;
}

AActor* UWoTCasterComponent::FindIncomingWeave() const
{
	// CheckForOffending (native DefensiveDetector) folded to a poll: collect the
	// CollectClass (AngrealProjectile) actors inside the perimeter cylinder, and return
	// the nearest that survives the rejection ladder and the seeker-destination gate.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const UWorld* World = GetWorld();
	if (!OwnerPawn || !World)
	{
		return nullptr;
	}

	// Perimeter dimensions (DefensiveDetectorAngrealUser: CollisionRadius*8 x
	// CollisionHeight*3). CollisionHeight is the UE1 half-height == capsule half-height.
	float Radius = 34.0f;
	float HalfHeight = 88.0f;
	if (const ACharacter* Char = Cast<ACharacter>(OwnerPawn))
	{
		if (const UCapsuleComponent* Capsule = Char->GetCapsuleComponent())
		{
			Radius = Capsule->GetScaledCapsuleRadius();
			HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}
	}
	const float PerimeterRadiusSq = FMath::Square(Radius * DefensivePerimeterRadiusFactor);
	const float PerimeterHalfHeight = HalfHeight * DefensivePerimeterHeightFactor;
	const FVector Origin = OwnerPawn->GetActorLocation();

	AActor* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (TActorIterator<AWoTAngrealProjectile> It(GetWorld()); It; ++It)
	{
		AWoTAngrealProjectile* Proj = *It;
		if (!IsValid(Proj))
		{
			continue;
		}
		const FVector Delta = Proj->GetActorLocation() - Origin;
		if (FVector2D(Delta.X, Delta.Y).SizeSquared() > PerimeterRadiusSq ||
		    FMath::Abs(Delta.Z) > PerimeterHalfHeight)
		{
			continue;
		}
		if (RejectOffender(Proj))
		{
			continue;
		}
		// Seeker gate (AngrealUser.uc:248): a seeker only offends when it targets us
		// (Destination == self) or has no destination; one aimed past us is ignored.
		if (const AWoTSeekingProjectile* Seeker = Cast<AWoTSeekingProjectile>(Proj))
		{
			if (Seeker->Destination && Seeker->Destination != OwnerPawn)
			{
				continue;
			}
		}
		const float DistSq = Delta.SizeSquared();
		if (DistSq < BestDistSq)
		{
			Best = Proj;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

bool UWoTCasterComponent::RejectOffender(AActor* Offender) const
{
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!Offender || !OwnerPawn)
	{
		return true;
	}
	const FVector Origin = OwnerPawn->GetActorLocation();
	const FVector OffLoc = Offender->GetActorLocation();
	// offender.Velocity (native DefensiveDetector): the projectile's own velocity. Read
	// it from the movement component — it is authoritative and current the frame it is
	// set, where AActor::GetVelocity() (root ComponentVelocity) only reflects the prior
	// integrated tick.
	FVector Velocity = Offender->GetVelocity();
	if (const AWoTProjectile* Proj = Cast<AWoTProjectile>(Offender))
	{
		if (Proj->ProjectileMovement)
		{
			Velocity = Proj->ProjectileMovement->Velocity;
		}
	}

	// Rule 4 — not moving toward us (native DefensiveDetector): the offender steps
	// one unit along its velocity; if that does not bring it closer, it is not incoming.
	// A stationary offender (zero velocity) fails this too, matching the shipped test.
	const float DistNow = FVector::Dist(OffLoc, Origin);
	if (FVector::Dist(OffLoc + Velocity.GetSafeNormal(), Origin) >= DistNow)
	{
		return true;
	}
	// Rule 5 — arrival time (GetArrivalTime = distance / speed) outside the response
	// window, or uncomputable (not moving).
	const float Speed = Velocity.Size();
	if (Speed <= 0.0f)
	{
		return true;
	}
	const float Arrival = DistNow / Speed;
	return Arrival < DR_MinResponseTime || Arrival > DR_MaxResponseTime;
}

AWoTAngrealInventory* UWoTCasterComponent::SelectDefensiveWeave(AActor* Offender)
{
	// SelectDefensiveAngreal (AngrealWeaponAdapter.uc:282): interval gate, then the
	// highest GetEffectivePriority among bDefensive items with the incoming projectile
	// set as victim (which routes GetEffectivePriority -> GetDefensivePriority).
	UWoTInventoryComponent* Inv = GetInventory();
	const UWorld* World = GetWorld();
	const AWoTCharacter* OwnerCharacter = Cast<AWoTCharacter>(GetOwner());
	if (!Inv || !World || !Offender ||
		(OwnerCharacter && OwnerCharacter->IsCastingBlocked()) ||
	    World->GetTimeSeconds() <= LastDefensiveUsageTime + MinDefensiveUsageInterval)
	{
		return nullptr;
	}
	AWoTAngrealInventory* Best = nullptr;
	float BestPriority = 0.0f;
	for (AWoTInventory* Item : Inv->Items)
	{
		AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item);
		if (!Weave || Weave == AdaptedWeave || !Weave->bDefensive)
		{
			continue;
		}
		Weave->SetVictim(Offender);
		float Priority = 0.0f;
		if (Weave->GetEffectivePriority(Priority) && (!Best || Priority > BestPriority))
		{
			Best = Weave;
			BestPriority = Priority;
		}
	}
	if (Best)
	{
		// DetermineDefensiveUsage (AngrealWeaponAdapter.uc:267).
		Best->InitChargeGroup();
	}
	return Best;
}

void UWoTCasterComponent::BeginCast(AWoTAngrealInventory* Weave)
{
	// UseSpecificAngreal (AngrealUser.uc:214): release, select, press.
	UWoTInventoryComponent* Inv = GetInventory();
	if (!Inv || !Weave)
	{
		return;
	}
	Inv->CeaseUsingAngreal();
	Inv->SelectedItem = Weave;
	ActiveWeave = Weave;
	AdaptedWeave = Weave;
	LastSeenCharges = Weave->CurCharges;
	LastSeenUseCount = Weave->CastUseCount;
	// Adapter.Tick must run before the adapted inventory's actor tick, otherwise an
	// automatic launcher can consume the next charge using the previous frame's aim.
	// It also runs after the controller has completed its ordinary focus update, so
	// that update cannot replace the led rotation between adapter and launch ticks.
	if (APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		if (AController* Controller = OwnerPawn->GetController())
		{
			AddTickPrerequisiteActor(Controller);
		}
	}
	Weave->AddTickPrerequisiteComponent(this);
	UpdateNPCProjectileAim();
	// Keep ActiveWeave installed across the synchronous call so ChargeUsed can
	// address this adapter, but publish casting presentation only after the real
	// DefaultCastReflector/NoCast gate accepts the activation.
	if (!Inv->UseAngreal())
	{
		Release();
		return;
	}
	BeginContinuousCastPresentation();
}

void UWoTCasterComponent::Release()
{
	AWoTAngrealInventory* Released = ActiveWeave;
	if (UWoTInventoryComponent* Inv = GetInventory(); Inv && ActiveWeave)
	{
		Inv->CeaseUsingAngreal();
	}
	if (Released)
	{
		Released->RemoveTickPrerequisiteComponent(this);
	}
	if (APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		if (AController* Controller = OwnerPawn->GetController())
		{
			RemoveTickPrerequisiteActor(Controller);
		}
	}
	ActiveWeave = nullptr;
	EndCastPresentation();
}

void UWoTCasterComponent::NotifyTargetKilled(AActor* Victim)
{
	if (ActiveWeave && ActiveWeave->GetVictim() == Victim)
	{
		Release();
	}
}

bool UWoTCasterComponent::IsContinuousCasting() const
{
	return ActiveWeave && ActiveWeave->MaxChargeUsedInterval == 0.0f;
}

void UWoTCasterComponent::UpdateNPCProjectileAim()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	AWoTProjectileLauncher* Launcher = Cast<AWoTProjectileLauncher>(ActiveWeave);
	if (!OwnerPawn || OwnerPawn->IsPlayerControlled() || !Launcher
		|| !Launcher->IsLeadable() || !IsValid(Launcher->GetVictim()))
	{
		return;
	}
	if (AWoTNPCController* Controller = Cast<AWoTNPCController>(OwnerPawn->GetController()))
	{
		// AAIController's ordinary focal-point update aims at the target's current
		// location. Once a leadable adapter owns the cast, remove that competing
		// focus and publish the calculated firing view directly.
		Controller->ClearFocus(EAIFocusPriority::Gameplay);
		Controller->SetControlRotation(Launcher->GetBestTrajectory());
	}
}

void UWoTCasterComponent::BeginContinuousCastPresentation()
{
	if (!IsContinuousCasting() || bOwnsAttackRunPresentation)
	{
		return;
	}
	if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
	{
		if (!Character->LoopSlotAnim(TEXT("AttackRun")))
		{
			// Table-less test/runtime fallbacks retain AngrealUser's literal
			// AnimSequence='ATTACKRUN' observable.
			Character->SetAnimIntent(TEXT("ATTACKRUN"), 1.0f, true);
		}
		bOwnsAttackRunPresentation = true;
	}
}

void UWoTCasterComponent::StampChargeUsedPresentation()
{
	AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner());
	if (!Character)
	{
		return;
	}
	if (IsContinuousCasting())
	{
		BeginContinuousCastPresentation();
		return;
	}
	if (!Character->StampSlotAnim(TEXT("AttackRun")))
	{
		Character->SetAnimIntent(TEXT("ATTACKRUN"), 1.0f, false);
	}
	bOwnsAttackRunPresentation = true;
}

void UWoTCasterComponent::EndCastPresentation()
{
	if (!bOwnsAttackRunPresentation)
	{
		return;
	}
	bOwnsAttackRunPresentation = false;
	if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
	{
		Character->RefreshNPCLocomotionAnimation();
	}
}

void UWoTCasterComponent::NotifyChargeUsed(AWoTAngrealInventory* Weave)
{
	if (!Weave || Weave != ActiveWeave || !GetWorld())
	{
		return;
	}
	if (Weave->bDefensive)
	{
		LastDefensiveUsageTime = GetWorld()->GetTimeSeconds();
	}
	else
	{
		LastOffensiveUsageTime = GetWorld()->GetTimeSeconds();
	}
	StampChargeUsedPresentation();
}

void UWoTCasterComponent::InterruptForIceContact()
{
	Release();
}

void UWoTCasterComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                        FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!ActiveWeave)
	{
		return;
	}
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	// AngrealWeaponAdapter.Tick re-applies GetBestTrajectory every active tick,
	// before the adapted launcher's prerequisite-ordered actor tick.
	UpdateNPCProjectileAim();

	// The usage cycle (AngrealWeaponAdapter.uc:81 ChargeUsed + :150
	// OnItemUsageNotification), driven by observing the use counter: on each shot
	// (charge spent OR a 0-cost launch), stamp the usage time, release if the weave
	// says so, and stop the volley if the victim is gone or no longer visible. A
	// ChargeCost=0 launcher (the Legion seeker) never drops CurCharges, so the
	// CastUseCount tick is what ends its single-shot cast (else it would hold forever).
	if (ActiveWeave->CurCharges < LastSeenCharges ||
	    ActiveWeave->CastUseCount != LastSeenUseCount)
	{
		LastSeenCharges = ActiveWeave->CurCharges;
		LastSeenUseCount = ActiveWeave->CastUseCount;
		// ChargeUsed (AngrealWeaponAdapter.uc:87): stamp the usage time of the hand
		// the weave belongs to, so each interval gate paces its own selections.
		// The synchronous inventory ChargeUsed callback stamps hand timing and
		// presentation. The counter observation retains group-release policy.
		if (!ActiveWeave->ShouldCast())
		{
			// ChargeUsed: only an item that can no longer be used clears the
			// adapted-item exclusion. A normal button release retains it.
			AdaptedWeave = nullptr;
			Release();
			return;
		}
		if (ActiveWeave->ShouldUncast())
		{
			Release();
			return;
		}
	}
	AActor* Victim = ActiveWeave->GetVictim();
	if (!Victim || !OwnerPawn || !CanContinueVolley(Victim))
	{
		// Target lost mid-volley: release (OnItemUsageNotification's
		// PawnCanSeeActor gate).
		Release();
	}
}

bool UWoTCasterComponent::CanContinueVolley(AActor* Victim) const
{
	return WoTSight::PawnCanSeeActor(Cast<APawn>(GetOwner()), Victim);
}
