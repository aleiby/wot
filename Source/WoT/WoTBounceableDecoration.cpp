#include "WoTBounceableDecoration.h"
#include "WoTFallingPhysics.h"
#include "WoTRandom.h"

#include "Projectiles/WoTProjectile.h"
#include "WoTMover.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTZoneVolume.h"

#include "Engine/World.h"
#include "EngineUtils.h"

AWoTBounceableDecoration::AWoTBounceableDecoration()
{
	bProjTarget = true;
	bStatic = false;   // BounceableDecoration.uc:184
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicatingMovement(true);
	ApplyWorldCollision();
}

void AWoTBounceableDecoration::ConfigureRuntimeCollision(bool bInCollideActors,
	bool bInBlockActors, bool bInBlockPlayers, float Radius, float HalfHeight)
{
	Super::ConfigureRuntimeCollision(bInCollideActors, bInBlockActors,
		bInBlockPlayers, Radius, HalfHeight);
	ApplyWorldCollision();
}

void AWoTBounceableDecoration::PostInitializeComponents()
{
	// The base re-applies its actor-only collision here, after the projection
	// configured this decoration, and that clears the world responses. Without
	// them a placed decoration fell through the level when knocked. Re-assert
	// them, as the breakable decoration does.
	Super::PostInitializeComponents();
	ApplyWorldCollision();
}

void AWoTBounceableDecoration::ApplyWorldCollision()
{
	if (!Collision) return;
	// BounceableDecoration.uc:187 (bCollideWorld): it bounces off and lands on
	// the level and movers. Movers are WorldDynamic, like every other dynamic
	// body; BeginPlay keeps the rest from blocking it.
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	if (bCollideActors && !bBlockActors && !bBlockPlayers)
		Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

void AWoTBounceableDecoration::BeginPlay()
{
	Super::BeginPlay();
	// BounceableDecoration.uc:188-189 (bBlockActors and bBlockPlayers false): the
	// decoration collides with the world and movers only. Carcasses, gore,
	// debris, dropped items and other decorations neither stop it nor land on
	// it, in either direction, as for the tossed weapon (AWoTTossedWeapon).
	UWorld* World = GetWorld();
	if (!World) return;
	for (TActorIterator<AActor> It(World); It; ++It) IgnoreNonWorldActor(*It);
	WorldOnlySpawnHandle = World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(
			this, &AWoTBounceableDecoration::IgnoreNonWorldActor));
}

void AWoTBounceableDecoration::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (WorldOnlySpawnHandle.IsValid() && GetWorld())
	{
		GetWorld()->RemoveOnActorSpawnedHandler(WorldOnlySpawnHandle);
		WorldOnlySpawnHandle.Reset();
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& Component : NonBlockingComponents)
	{
		if (Component.IsValid() && Collision)
			Component->IgnoreComponentWhenMoving(Collision, false);
	}
	NonBlockingComponents.Reset();
	Super::EndPlay(EndPlayReason);
}

void AWoTBounceableDecoration::IgnoreNonWorldActor(AActor* Actor)
{
	if (!Collision || !IsValid(Actor) || Actor == this || Actor->IsA<AWoTMover>()) return;
	// The level is WorldStatic; every other body that could block this one is a
	// WorldDynamic component.
	TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
	for (UPrimitiveComponent* Component : Components)
	{
		if (!Component || Component->GetCollisionObjectType() != ECC_WorldDynamic) continue;
		Collision->IgnoreComponentWhenMoving(Component, true);
		Component->IgnoreComponentWhenMoving(Collision, true);
		NonBlockingComponents.Add(Component);
	}
}

void AWoTBounceableDecoration::SetupBounce(const FVector& InVelocity)
{
	if (!HasAuthority()) return;
	BounceVelocity = InVelocity;
	SpinRate = FRotator(WoTRandom::FRandRange(-686.6f, 686.6f),
		WoTRandom::FRandRange(-686.6f, 686.6f), WoTRandom::FRandRange(-686.6f, 686.6f));
	bAnimating = true;
	bBounce = true;
	bFirstHit = true;
	SetActorTickEnabled(true);
}

void AWoTBounceableDecoration::AddBounceVelocity(const FVector& AddedVelocity)
{
	SetupBounce(BounceVelocity + AddedVelocity);
}

float AWoTBounceableDecoration::TakeDamage(float DamageAmount,
	const FDamageEvent& DamageEvent, AController* EventInstigator,
	AActor* DamageCauser)
{
	Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (!HasAuthority()) return 0.0f;
	FVector Momentum = FVector::ZeroVector;
	if (DamageEvent.IsOfType(FWoTDamageEvent::ClassID))
	{
		Momentum = static_cast<const FWoTDamageEvent&>(DamageEvent).UE1Momentum;
	}
	Momentum.Z = FMath::Abs(Momentum.Z * 4.0f + 3000.0f);
	SetupBounce(Momentum * 0.02f);
	return DamageAmount;
}

void AWoTBounceableDecoration::NotifyActorBeginOverlap(AActor* Other)
{
	Super::NotifyActorBeginOverlap(Other);
	if (!HasAuthority() || Damage <= 0.0f || !IsValid(Other) || Other == this) return;
	FWoTDamageEvent Event;
	Event.Damage = Damage;
	Event.WoTDamageType = DamageType;
	Event.UE1Momentum = (Other->GetActorLocation() - GetActorLocation()).GetSafeNormal()
		* MomentumTransfer;
	Event.HitInfo.ImpactPoint = GetActorLocation();
	Other->TakeDamage(Damage, Event, nullptr, this);
}

void AWoTBounceableDecoration::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !bAnimating || !GetWorld()) return;
	// The original's falling step begins by checking the decoration is still
	// inside the level; outside it, it is removed.
	if (WoTFallingPhysics::DestroyOutsideWorld(*this)) return;
	// The zone's gravity. While the decoration bounces, the original's step adds
	// half of it and keeps that velocity, refining it to the full step only once
	// bouncing ends (as for the breakable decoration).
	BounceVelocity += AWoTZoneVolume::GetGravityAt(GetWorld(), GetActorLocation())
		* DeltaSeconds * (bBounce ? 0.5f : 1.0f);
	if (!bBounce)
	{
		BounceVelocity = BounceVelocity.GetClampedToMaxSize(WoTFallingPhysics::TerminalSpeed);
	}
	AddActorLocalRotation(SpinRate * DeltaSeconds);
	FHitResult Hit;
	AddActorWorldOffset(BounceVelocity * DeltaSeconds, true, &Hit);
	if (Hit.bBlockingHit)
	{
		if (!bBounce && Hit.ImpactNormal.Z > 0.7f)
		{
			Settle(Hit.ImpactNormal);
		}
		else
		{
			HitWorld(Hit.ImpactNormal, BounceVelocity.Size());
		}
	}
	if (!bBounce)
	{
		BounceVelocity = BounceVelocity.GetClampedToMaxSize(WoTFallingPhysics::TerminalSpeed);
	}
}

void AWoTBounceableDecoration::HitWorld(const FVector& HitNormal, float Speed)
{
	if (bBounce && (bFirstHit || Speed > MinBounceSoundSpeed) && !LandSound1.IsNone())
	{
		// BounceableDecoration.uc:61: the decoration's PlaySound in SLOT_Misc,
		// volume then pitch perturbed, the engine's default radius.
		const float Volume = BaseBounceVolume * WoTRandom::FRandRange(
			1.0f - VolumePerturbPercent * .01f, 1.0f + VolumePerturbPercent * .01f);
		const float Pitch = BaseBouncePitch * WoTRandom::FRandRange(
			1.0f - PitchPerturbPercent * .01f, 1.0f + PitchPerturbPercent * .01f);
		WoTSound::PlayReplicatedFrom(*this, LandSound1, WoTSound::ESlot::Misc, Volume,
			/*bNoOverride*/ false, 0.0f, Pitch);
	}
	if (bFirstHit && Speed < MinFirstHitSpeed)
	{
		SpinRate = FRotator::ZeroRotator;
		SetActorRotation(FRotator(0.0f, WoTRandom::FRandRange(0.0f, 360.0f), 0.0f));
	}
	bFirstHit = false;
	SpinRate *= RotationDampeningRatio;
	BounceVelocity = SpeedDampeningRatio
		* (BounceVelocity - 2.0f * HitNormal * (BounceVelocity | HitNormal));
	// BounceableDecoration.uc:81-85 disables only the next bounce. Keep falling
	// after a wall hit; Landed (:135-163) is dispatched on a later floor contact.
	if (BounceVelocity.Size() < MinBounceSpeed) bBounce = false;
}

void AWoTBounceableDecoration::Settle(const FVector& HitNormal)
{
	bAnimating = false;
	bBounce = false;
	BounceVelocity = FVector::ZeroVector;
	SpinRate = FRotator::ZeroRotator;
	FRotator R = GetActorRotation();
	if (LandedYaw >= 0.0f) R.Yaw = LandedYaw * 360.0f / 65536.0f;
	if (LandedPitch >= 0.0f) R.Pitch = LandedPitch * 360.0f / 65536.0f;
	if (LandedRoll >= 0.0f) R.Roll = LandedRoll * 360.0f / 65536.0f;
	const FVector SurfaceTilt = FVector::UpVector - HitNormal.GetSafeNormal();
	if (!SurfaceTilt.IsNearlyZero()) R += SurfaceTilt.Rotation();
	SetActorRotation(R);
	SetActorTickEnabled(false);
}
