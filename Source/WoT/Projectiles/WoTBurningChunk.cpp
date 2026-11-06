#include "WoTBurningChunk.h"
#include "../WoTRandom.h"

#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "Components/PointLightComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "TimerManager.h"

AWoTBurningChunk::AWoTBurningChunk()
{
	PrimaryActorTick.bCanEverTick = true;
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->DrawScale = 0.1f;
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("LavaRock"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetVisibility(false);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("BurningChunkLight"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);

	SetUE1CollisionSize(0.0f, 0.0f);
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	if (UWoTProjectileMovementComponent* Move = Cast<UWoTProjectileMovementComponent>(ProjectileMovement))
	{
		Move->bUseRetailFallingGravity = true;
		Move->bOwnerHandlesBlockingHits = true;
	}
	ProjectileMovement->bShouldBounce = true;
	ProjectileMovement->Bounciness = 1.0f;
	ProjectileMovement->Friction = 0.0f;
	ProjectileMovement->BounceVelocityStopSimulatingThreshold = 0.0f;

	Damage = 1.0f;
	DamageRadius = 0.0f;
	DamageType = TEXT("hurt");
	bDestroyOnExplode = true;
	bNetTemporary = false;
	bReplicates = false;
	SetReplicatingMovement(false);
	InitialLifeSpan = 3.0f;
	HitWaterEffectName = TEXT("FireballFizzle");
	HitWaterEffectOffset = FVector(0, 0, 39);
}

void AWoTBurningChunk::BeginPlay()
{
	Duration = RandomRange(MinChunkLife, MaxChunkLife);
	Speed = RandomRange(MinChunkSpeed, MaxChunkSpeed);
	// UE1 PHYS_Falling is bounded by ZoneTerminalVelocity (2500 by default),
	// not Projectile.MaxSpeed (UE1's physFalling).
	MaxSpeed = 2500.0f;
	Super::BeginPlay();
	SetLifeSpan(Duration);
	FullLightIntensity = WoTUE1LightRegistry::Configure(Light, TEXT("BurningChunk"))
		? Light->Intensity : 0.0f;
	Light->SetVisibility(true);
	if (bLavaRockVisual)
	{
		Sprite->SetVisibility(false);
		Mesh->SetVisibility(true);
		if (Mesh->SetVertexMesh(TEXT("LavaRock"), TEXT("Angreal")))
		{
			Mesh->SetRelativeScale3D(FVector(0.7f));
		}
	}
	else
	{
		WoTMeshFX::ConfigureClassSprite(*Sprite, TEXT("BurningChunk"));
	}
	LastFlightVelocity = ProjectileMovement->Velocity;
	GetWorldTimerManager().SetTimer(SprayerTimer, this,
		&AWoTBurningChunk::StartSprayer, RandomRange(0.0f, 0.5f), false);
}

void AWoTBurningChunk::UseLocalRandom(int32 Seed)
{
	bUseLocalRandom = true;
	LocalRandom.Initialize(Seed);
}

float AWoTBurningChunk::RandomRange(float Min, float Max)
{
	return bUseLocalRandom ? LocalRandom.FRandRange(Min, Max)
		: WoTRandom::FRandRange(Min, Max);
}

void AWoTBurningChunk::StartSprayer()
{
	if (!IsValid(this) || bWaterFizzled || SprayerConfig.IsNone() || !GetWorld()) { return; }
	Sprayer = WoTWeaveFX::SpawnLocal(*GetWorld(), SprayerConfig,
		GetActorLocation(), FVector::UpVector.Rotation(), this);
	if (Sprayer)
	{
		SprayerVolume = Sprayer->Volume;
	}
}

void AWoTBurningChunk::StopSprayer()
{
	WoTWeaveFX::FadeOut(Sprayer, 5.0f);
	Sprayer = nullptr;
}

void AWoTBurningChunk::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bSettled && ProjectileMovement && !ProjectileMovement->Velocity.IsNearlyZero())
	{
		LastFlightVelocity = ProjectileMovement->Velocity;
	}
	const float Remaining = GetLifeSpan();
	const float Fade = FMath::Clamp(Remaining / FadeTime, 0.0f, 1.0f);
	ScaleGlow = Fade;
	Sprite->ScaleGlow = Fade;
	if (Light) { Light->SetIntensity(FullLightIntensity * Fade); }
	if (Sprayer)
	{
		Sprayer->Volume = Duration > 0.0f ? SprayerVolume * Remaining / Duration : 0.0f;
	}
}

void AWoTBurningChunk::NotifyHit(UPrimitiveComponent*, AActor*,
	UPrimitiveComponent*, bool, FVector HitLocation, FVector HitNormal,
	FVector, const FHitResult&)
{
	UpdateWaterZone();
	if (!IsValid(this) || IsActorBeingDestroyed()) { return; }
	if (HitNormal.Z > 0.7f)
	{
		// BurningChunk.uc Landed: place BurnDecal, then engine Falling settles.
		WoTDecalFX::SpawnAligned(*GetWorld(), TEXT("BurnDecal"),
			HitLocation, HitNormal, false);
		bSettled = true;
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->Deactivate();
		return;
	}
	// BurningChunk.uc Bounce: UE1 vector*vector is component-wise.
	const FVector V = LastFlightVelocity;
	ProjectileMovement->Velocity = FVector::DotProduct(V, HitNormal)
		* HitNormal * V;
}

void AWoTBurningChunk::ProcessTouch(AActor* Other, FVector HitLocation)
{
	Super::ProcessTouch(Other, HitLocation);
	if (IsValid(this) && IsValid(Other) && ProjectileMovement)
	{
		const FVector N = (GetActorLocation() - Other->GetActorLocation()).GetSafeNormal();
		const FVector V = LastFlightVelocity;
		ProjectileMovement->Velocity = FVector::DotProduct(V, N) * N * V;
	}
}

void AWoTBurningChunk::ProcessDamage(AActor* Other, float GivenDamage,
	APawn* GivenInstigator, FVector HitLocation, FVector GivenMomentum)
{
	// RemoteRole=None chunks are spawned independently on clients. A client-local
	// actor has ROLE_Authority, so net mode—not Role—is the damage authority gate.
	if (GetNetMode() != NM_Client)
	{
		Super::ProcessDamage(Other, GivenDamage, GivenInstigator,
			HitLocation, GivenMomentum);
	}
}

void AWoTBurningChunk::HitWater()
{
	if (bWaterFizzled) { return; }
	bWaterFizzled = true;
	bHitWater = true;
	GetWorldTimerManager().ClearTimer(SprayerTimer);
	if (GetWorld())
	{
		const FVector At = GetActorLocation() + HitWaterEffectOffset;
		if (AWoTExplosionEffect* Fizzle = WoTWeaveFX::SpawnExplosion(
			*GetWorld(), HitWaterEffectName, At))
		{
			// BurningChunk.uc:146-150 scales the spawned fizzle, not the chunk.
			Fizzle->Sprite->DrawScale = 0.7f;
		}
		else
		{
			WoTWeaveFX::SpawnLocal(*GetWorld(), HitWaterEffectName, At,
				FVector::UpVector.Rotation());
		}
	}
	if (Light) { Light->SetVisibility(false); Light->SetIntensity(0.0f); }
	StopSprayer();
}

void AWoTBurningChunk::Destroyed()
{
	GetWorldTimerManager().ClearTimer(SprayerTimer);
	StopSprayer();
	Super::Destroyed();
}

void AWoTBurningChunk::SpawnFan(UWorld& World,
	TSubclassOf<AWoTBurningChunk> ChunkClass, const FVector& Location,
	const FVector& SurfaceNormal, const AWoTAngrealProjectile* Source)
{
	if (!ChunkClass) { return; }
	const bool bCosmeticSimulation = Source && !Source->HasAuthority();
	uint32 Seed = HashCombineFast(GetTypeHash(Source ? Source->GetFName() : NAME_None),
		GetTypeHash(Location.X));
	Seed = HashCombineFast(Seed, GetTypeHash(Location.Y));
	Seed = HashCombineFast(Seed, GetTypeHash(Location.Z));
	FRandomStream FanRandom(static_cast<int32>(Seed));
	auto Range = [&](float Min, float Max)
	{
		return bCosmeticSimulation ? FanRandom.FRandRange(Min, Max)
			: WoTRandom::FRandRange(Min, Max);
	};
	const FRotator SurfaceRotation = SurfaceNormal.GetSafeNormal().Rotation();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const float Pitch = Index == 0 ? Range(0.0f, 45.0f)
			: Range(45.0f, 90.0f);
		const float Yaw = Index == 0 ? Range(0.0f, 360.0f)
			: 90.0f * (Index - 1) + Range(0.0f, 90.0f);
		// UnrealScript rotator addition is component-wise, not quaternion
		// composition (AngrealProjectile.uc:305-308,315-348).
		const FVector Direction = (SurfaceRotation
			+ FRotator(Pitch, Yaw, 0.0f)).Vector().GetSafeNormal();
		const FTransform Transform(Direction.Rotation(), Location);
		AWoTBurningChunk* Chunk = World.SpawnActorDeferred<AWoTBurningChunk>(
			ChunkClass, Transform, Source ? Source->GetOwner() : nullptr,
			Source ? Source->GetInstigator() : nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Chunk) { continue; }
		if (Source)
		{
			Chunk->SetSourceAngreal(Source->SourceAngreal);
			// SpawnChunks restores the projectile's current instigator after
			// SetSourceAngreal, which may point at the artifact's original owner.
			Chunk->SetInstigator(Source->GetInstigator());
		}
		if (bCosmeticSimulation)
		{
			Chunk->UseLocalRandom(static_cast<int32>(Seed
				^ (0x9e3779b9u * static_cast<uint32>(Index + 1))));
		}
		Chunk->FinishSpawning(Transform);
	}
}

