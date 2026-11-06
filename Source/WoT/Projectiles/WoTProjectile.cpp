#include "WoTProjectile.h"

#include "../WoTAmbientSound.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTSound.h"
#include "../WoTZoneVolume.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"

FVector UWoTProjectileMovementComponent::ComputeAcceleration(
	const FVector& InVelocity, float DeltaTime) const
{
	FVector Acceleration = Super::ComputeAcceleration(InVelocity, DeltaTime);
	if (bUseRetailFallingGravity)
	{
		const AActor* OwnerActor = GetOwner();
		Acceleration += AWoTZoneVolume::GetGravityAt(
			OwnerActor ? OwnerActor->GetWorld() : nullptr,
			OwnerActor ? OwnerActor->GetActorLocation() : FVector::ZeroVector);
	}
	return Acceleration;
}

UProjectileMovementComponent::EHandleBlockingHitResult
UWoTProjectileMovementComponent::HandleBlockingHit(const FHitResult& Hit,
	float TimeTick, const FVector& MoveDelta, float& SubTickTimeRemaining)
{
	if (!bOwnerHandlesBlockingHits)
	{
		return Super::HandleBlockingHit(Hit, TimeTick, MoveDelta,
			SubTickTimeRemaining);
	}
	AActor* OwnerActor = UpdatedComponent ? UpdatedComponent->GetOwner() : nullptr;
	if (!IsValid(OwnerActor) || !IsActive() || HasStoppedSimulation())
	{
		return EHandleBlockingHitResult::Abort;
	}
	// MoveComponent has already dispatched NotifyHit, where the owner applied the
	// retail HitWall/Landed result. Continue without UE5 reflection/friction.
	if (Hit.bStartPenetrating)
	{
		return EHandleBlockingHitResult::Abort;
	}
	SubTickTimeRemaining = TimeTick * (1.0f - Hit.Time);
	return EHandleBlockingHitResult::AdvanceNextSubstep;
}

AWoTProjectile::AWoTProjectile()
{
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->SetCollisionProfileName(TEXT("Projectile"));
	Collision->SetGenerateOverlapEvents(true);
	RootComponent = Collision;
	CylinderCollision = CreateDefaultSubobject<UWoTFlatCylinderComponent>(
		TEXT("CylinderCollision"));
	CylinderCollision->SetupAttachment(Collision);
	CylinderCollision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CylinderCollision->SetGenerateOverlapEvents(false);

	ProjectileMovement = CreateDefaultSubobject<UWoTProjectileMovementComponent>(
		TEXT("ProjectileMovement"));
	ProjectileMovement->ProjectileGravityScale = 0.0f;   // PHYS_Projectile
	ProjectileMovement->bRotationFollowsVelocity = true; // bDirectional

	// Projectile defaults: bNetTemporary, bReplicateInstigator, sim proxy
	bNetTemporary = true;
	bReplicates = true;
	SetReplicatingMovement(true);
}

void AWoTProjectile::SetUE1CollisionSize(float Radius, float Height)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHeight = FMath::Max(0.0f, Height);
	if (UE1CollisionRadius <= 0.0f && UE1CollisionHeight <= 0.0f)
	{
		Collision->SetSphereRadius(0.0f);
		return;
	}

	// Every shipped non-point leaf authors both dimensions. Keep the fallback
	// non-degenerate for malformed data without collapsing one axis into the other.
	CylinderCollision->SetCylinderSize(
		FMath::Max(UE1CollisionRadius, KINDA_SMALL_NUMBER),
		FMath::Max(UE1CollisionHeight, KINDA_SMALL_NUMBER));
	CylinderCollision->SetCollisionProfileName(TEXT("Projectile"));
	CylinderCollision->SetGenerateOverlapEvents(true);
	CylinderCollision->SetNotifyRigidBodyCollision(true);
	CylinderCollision->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
	SetRootComponent(CylinderCollision);
	Collision->SetupAttachment(CylinderCollision);
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetGenerateOverlapEvents(false);
	if (ProjectileMovement)
	{
		ProjectileMovement->SetUpdatedComponent(CylinderCollision);
		// The flat UE1 cylinder is a world-up sweep. Directional presentation is
		// carried by the disabled sphere child instead of tilting the swept root.
		ProjectileMovement->bRotationFollowsVelocity = false;
	}
}

UPrimitiveComponent* AWoTProjectile::GetActiveSweptCollision() const
{
	if (ProjectileMovement)
	{
		if (UPrimitiveComponent* Updated = Cast<UPrimitiveComponent>(
			ProjectileMovement->UpdatedComponent))
		{
			return Updated;
		}
	}
	return Cast<UPrimitiveComponent>(RootComponent);
}

void AWoTProjectile::AlignPresentationToVelocity(const FVector& Velocity)
{
	if (Velocity.IsNearlyZero())
	{
		return;
	}
	FRotator PresentationRotation = Velocity.Rotation();
	PresentationRotation.Roll = GetActorRotation().Roll;
	if (GetActiveSweptCollision() == CylinderCollision)
	{
		FRotator WorldUpRoot = PresentationRotation;
		WorldUpRoot.Pitch = 0.0f;
		WorldUpRoot.Roll = 0.0f;
		SetActorRotation(WorldUpRoot);
		Collision->SetWorldRotation(PresentationRotation);
		return;
	}
	SetActorRotation(PresentationRotation);
}

void AWoTProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	if (USoundBase* S = WoTSound::Resolve(AmbientLoopSound, true))
	{
		// UE1 AmbientSound: the shared ambient component applies the byte params +
		// the steady-state 2.0 x retail AmbientFactor (was a one-off
		// SpawnSoundAttached at the 1x start volume — Slice 8c unified the paths).
		AmbientHum = NewObject<UWoTAmbientSoundComponent>(this, TEXT("AmbientHum"));
		AmbientHum->SetupAttachment(RootComponent);
		AmbientHum->RegisterComponent();
		AmbientHum->ConfigureUE1Ambient(S, SoundRadius, SoundVolume, SoundPitch);
		AmbientHum->Play();
	}
}

void AWoTProjectile::OnRep_ReplicatedMovement()
{
	Super::OnRep_ReplicatedMovement();
	AlignPresentationToVelocity(GetReplicatedMovement().LinearVelocity);
}

void AWoTProjectile::StopAmbientHum()
{
	if (AmbientHum)
	{
		AmbientHum->Stop();
	}
}
