#include "WoTEarthTremorProjectile.h"
#include "../WoTUE1LightRegistry.h"
#include "WoTEarthTremorFX.h"
#include "Components/PointLightComponent.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTMeshFX.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
int32 EarthTremorCosmeticSeed(const UObject* Object)
{
	const uint64 Entropy = FPlatformTime::Cycles64()
		^ static_cast<uint64>(reinterpret_cast<UPTRINT>(Object));
	return static_cast<int32>(Entropy ^ (Entropy >> 32));
}
}

AWoTEarthTremorProjectile::AWoTEarthTremorProjectile()
{
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->DrawScale = 0.8f;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// AngrealEarthTremorProjectile defaultproperties (simulation subset).
	AmbientGlow = 211.0f;  // AngrealEarthTremorProjectile.uc
	bUnlit = true;         // AngrealEarthTremorProjectile.uc
	Speed = 500.0f;
	InitialLifeSpan = 140.0f;   // engine Projectile default
	Damage = 30.0f;
	DamageRadius = 280.0f;
	MomentumTransfer = 10000;
	DamageType = "xExxx";                 // Earth (bElementEarth -> GetDamageType)
	Collision->InitSphereRadius(6.0f);    // CollisionRadius=6 (height 12 cyl)
	// HitWaterClass=Class'Angreal.FireballFizzle', HitWaterOffset=(Z=56)
	// (AngrealEarthTremorProjectile.uc:160-161): the lob fizzles at the surface,
	// keeps falling and still erupts on the bottom (no HitWater override).
	HitWaterEffectName = TEXT("FireballFizzle");
	HitWaterEffectOffset = FVector(0.0f, 0.0f, 56.0f);

	// PHYS_Falling: the lob arcs and falls under gravity. bBounce=True is inert
	// (see the header) — leave bShouldBounce false so it explodes on first
	// contact via the base NotifyHit->Explode, matching HitWall->Explode.
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	CastChecked<UWoTProjectileMovementComponent>(ProjectileMovement)
		->bUseRetailFallingGravity = true;

	// Sound'Angreal.EarthTremor.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchET.LaunchET"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitLevelET.HitLevelET"));
	HitPawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitLevelET.HitLevelET"));
	HitWaterSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitWaterET.HitWaterET"));
}

void AWoTEarthTremorProjectile::BeginPlay()
{
	Super::BeginPlay();   // ServerInit: spawn sound + straight-line velocity
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealEarthTremorProjectile"));
	// Recorded §2 compatibility decision: this retail MaxFrameRate=0 ring is
	// shared by texture identity but held to the port's established 30 Hz pace.
	if (GetNetMode() != NM_DedicatedServer)
	{
		CosmeticRandom.Initialize(EarthTremorCosmeticSeed(this));
		WoTMeshFX::ConfigureClassSprite(*Sprite,
			TEXT("AngrealEarthTremorProjectile"), 30.0f);
	}

	if (HasAuthority())
	{
		// PostBeginPlay override: Velocity = (Vector(Rotation) + (0,0,0.3)) * Speed
		// — aim up ~17deg so the weave lobs onto the ground ahead.
		ProjectileMovement->Velocity =
			(GetActorRotation().Vector() + FVector(0.0f, 0.0f, 0.3f)) * Speed;
	}
}

FRotator AWoTEarthTremorProjectile::CalculateTrajectory(const AActor* Source,
	const AActor* Destination, float ProjectileSpeed) const
{
	(void)ProjectileSpeed;
	if (!Source || !Destination)
	{
		return FRotator::ZeroRotator;
	}
	FVector Direction =
		(Destination->GetActorLocation() - Source->GetActorLocation()).GetSafeNormal();
	Direction.Z = 0.33f;
	return Direction.Rotation();
}

void AWoTEarthTremorProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	if (HasAuthority() && GetWorld())
	{
		const FVector Z = HitNormal.GetSafeNormal();
		const FVector Aim = GetActorRotation().Vector();
		FVector Y = FVector::CrossProduct(Z, Aim).GetSafeNormal();
		if (Y.IsNearlyZero()) { Y = FVector::RightVector; }
		const FVector X = FVector::CrossProduct(Z, -Y).GetSafeNormal();
		const FTransform At(FRotationMatrix::MakeFromXZ(X, Z).ToQuat(), HitLocation);
		AWoTEarthTremor* Tremor = GetWorld()->SpawnActorDeferred<AWoTEarthTremor>(
			AWoTEarthTremor::StaticClass(), At, nullptr, GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Tremor)
		{
			Tremor->SetSourceAngreal(SourceAngreal);
			Tremor->FinishSpawning(At);
		}
	}
	if (GetWorld())
	{
		AWoTBurningChunk::SpawnFan(*GetWorld(), AWoTEarthTremorChunk::StaticClass(),
			HitLocation, HitNormal, this);
	}
	if (GetNetMode() != NM_DedicatedServer)
	{
		ExplosionEffectName = CosmeticRandom.RandHelper(2) == 0
			? FName(TEXT("FireballExplode")) : FName(TEXT("FireballExplode2"));
	}
	Super::Explode(HitLocation, HitNormal);
}
