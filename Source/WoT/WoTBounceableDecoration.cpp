#include "WoTBounceableDecoration.h"
#include "WoTRandom.h"

#include "Projectiles/WoTProjectile.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"

#include "Engine/World.h"

AWoTBounceableDecoration::AWoTBounceableDecoration()
{
	bProjTarget = true;
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

void AWoTBounceableDecoration::ApplyWorldCollision()
{
	if (!Collision) return;
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	if (bCollideActors && !bBlockActors && !bBlockPlayers)
		Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

void AWoTBounceableDecoration::SetupBounce(const FVector& InVelocity)
{
	if (!HasAuthority()) return;
	BounceVelocity = InVelocity;
	SpinRate = FRotator(WoTRandom::FRandRange(-686.6f, 686.6f),
		WoTRandom::FRandRange(-686.6f, 686.6f), WoTRandom::FRandRange(-686.6f, 686.6f));
	bAnimating = true;
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
	BounceVelocity.Z += GetWorld()->GetGravityZ() * DeltaSeconds;
	AddActorLocalRotation(SpinRate * DeltaSeconds);
	FHitResult Hit;
	AddActorWorldOffset(BounceVelocity * DeltaSeconds, true, &Hit);
	if (Hit.IsValidBlockingHit()) HitWorld(Hit.ImpactNormal, BounceVelocity.Size());
}

void AWoTBounceableDecoration::HitWorld(const FVector& HitNormal, float Speed)
{
	if ((bFirstHit || Speed > MinBounceSoundSpeed) && !LandSound1.IsNone())
	{
		WoTSound::PlayReplicated(*GetWorld(), LandSound1, GetActorLocation(),
			WoTSound::DefaultPlaySoundRadius,
			BaseBounceVolume * WoTRandom::FRandRange(1.0f - VolumePerturbPercent * .01f,
				1.0f + VolumePerturbPercent * .01f),
			BaseBouncePitch * WoTRandom::FRandRange(1.0f - PitchPerturbPercent * .01f,
				1.0f + PitchPerturbPercent * .01f));
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
	if (BounceVelocity.Size() < MinBounceSpeed) Settle(HitNormal);
}

void AWoTBounceableDecoration::Settle(const FVector& HitNormal)
{
	bAnimating = false;
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
