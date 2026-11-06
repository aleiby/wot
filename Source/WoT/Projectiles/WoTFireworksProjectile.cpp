#include "WoTFireworksProjectile.h"
#include "../WoTRandom.h"
#include "../WoTWeaveFX.h"
#include "../WoTSound.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "TimerManager.h"

AWoTFireworksProjectile::AWoTFireworksProjectile()
{
	// AngrealFireworksProjectile defaultproperties (simulation subset).
	Speed = 2000.0f;
	InitialLifeSpan = 140.0f;   // engine Projectile default (fuse usually fires first)
	Damage = 50.0f;
	DamageRadius = 320.0f;             // wide splash
	MomentumTransfer = 1500;
	DamageType = "xxFxx";             // Fire
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6

	// PHYS_Falling + bBounce: gravity plus a damped (0.6) wall bounce.
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	CastChecked<UWoTProjectileMovementComponent>(ProjectileMovement)
		->bUseRetailFallingGravity = true;
	ProjectileMovement->bShouldBounce = true;
	ProjectileMovement->Bounciness = 0.6f;

	// Sound'Angreal.Fireworks.*': the per-set Launch1FW/Explode*FW sounds are live.
	// SpawnSound/ImpactSound deliberately stay empty because those set-specific
	// one-shots supersede the generic base-projectile sounds.
	SpawnSound = FSoftObjectPath();
	ImpactSound = FSoftObjectPath();
	// HitWaterSound borrows the Fireball's splash (uc:121: AngrealInventoryFireball.HitWaterFB).
	HitWaterSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitWaterFB.HitWaterFB"));
	ImpactSoundPitch = 0.5f;       // deep boom
	HitWaterSoundPitch = 1.2f;
	HitWaterEffectName = TEXT("FireballFizzle");
	HitWaterEffectOffset = FVector(0, 0, 56);
}

const TArray<FWoTFireworkSet>& AWoTFireworksProjectile::GetFireworkSets()
{
	static const TArray<FWoTFireworkSet> Sets = {
		{TEXT("Firework01"),TEXT("Firework05"),TEXT("Angreal/Launch1FW"),TEXT("Angreal/Explode1FW")},
		{TEXT("Firework06"),TEXT("Firework05"),TEXT("Angreal/Launch1FW"),TEXT("Angreal/Explode1FW")},
		{TEXT("Firework03"),TEXT("Firework09"),TEXT("Angreal/Launch1FW"),TEXT("Angreal/Explode3FW")},
		{TEXT("Firework04"),TEXT("Firework08"),TEXT("Angreal/Launch1FW"),TEXT("Angreal/Explode2FW")},
		{TEXT("Firework02"),TEXT("Firework07"),TEXT("Angreal/Launch1FW"),TEXT("Angreal/Explode2FW")}};
	return Sets;
}

void AWoTFireworksProjectile::BeginPlay()
{
	Super::BeginPlay();
	SelectedFirework = WoTRandom::RandHelper(GetFireworkSets().Num());
	const FWoTFireworkSet& Set = GetFireworkSets()[SelectedFirework];
	const FWoTSprayerBundleRow* Row = WoTWeaveFX::Find(Set.ProjectileConfig);
	Trail = WoTWeaveFX::SpawnLocal(*GetWorld(), Set.ProjectileConfig,
		GetActorLocation(), Row ? Row->DefaultRotation : GetActorRotation(), this, true);
	LastLaunchSound = Set.LaunchSoundPath;
	WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(Set.LaunchSoundPath),
		GetActorLocation(), 2500.0f, 1.0f);

	// PreBeginPlay: pick a random fuse and (deferred) a random FireworkSet. The
	// fuse runs on BOTH roles (N2): UE1's simulated PreBeginPlay set the Timer on the
	// ROLE_SimulatedProxy too, so the client detonated its own bNetTemporary copy
	// locally. Fireworks bounces off walls (its NotifyHit never explodes), so the
	// client copy has NO impact-teardown path — without a client fuse it would bounce
	// until its 140s LifeSpan (a client-side leak, the N2 target). Detonate()->Explode()
	// is teardown + Destroy on the client, damage-free (splash is authority-gated in
	// Explode). Each role picks its own FRand fuse (as in the original), so the burst
	// TIME differs slightly client-vs-server — a cosmetic desync within the
	// "cosmetics locally simulated" rule (spec 3.4); damage stays server-authoritative.
	const float Fuse = WoTRandom::FRandRange(MinExpTime, MaxExpTime);
	GetWorldTimerManager().SetTimer(FuseTimer, this,
		&AWoTFireworksProjectile::Detonate, FMath::Max(Fuse, 0.01f), false);
}

void AWoTFireworksProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	if (GetFireworkSets().IsValidIndex(SelectedFirework))
	{
		const FWoTFireworkSet& Set = GetFireworkSets()[SelectedFirework];
		WoTWeaveFX::FadeOut(Trail, 5.0f);
		const FWoTSprayerBundleRow* Row = WoTWeaveFX::Find(Set.ExplosionConfig);
		WoTWeaveFX::SpawnLocal(*GetWorld(), Set.ExplosionConfig, HitLocation,
			Row ? Row->DefaultRotation : FRotator::ZeroRotator);
		LastExplosionSound = Set.ExplosionSoundPath;
		WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(Set.ExplosionSoundPath),
			HitLocation, 2500.0f, .5f);
	}
	Super::Explode(HitLocation, HitNormal);
}

void AWoTFireworksProjectile::Destroyed()
{
	WoTWeaveFX::FadeOut(Trail, 5.0f);
	Super::Destroyed();
}

void AWoTFireworksProjectile::Detonate()
{
	// Timer(): Explode(Location, Normal(Velocity)). Explode = base splash + destroy.
	Explode(GetActorLocation(), GetVelocity().GetSafeNormal());
}

void AWoTFireworksProjectile::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
                                        UPrimitiveComponent* OtherComp, bool bSelfMoved,
                                        FVector HitLocation, FVector HitNormal,
                                        FVector NormalImpulse, const FHitResult& Hit)
{
	// HitWall bounces (handled by the ProjectileMovementComponent) instead of
	// detonating — deliberately do NOT call Super (which would Explode).
}
