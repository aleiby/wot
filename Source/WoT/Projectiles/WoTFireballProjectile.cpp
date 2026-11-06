#include "WoTFireballProjectile.h"
#include "../WoTRandom.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSound.h"
#include "../WoTWeaveFX.h"
#include "../WoTParticleSprayer.h"
#include "../World/WoTRuntimeBSPRenderer.h"

#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ModelComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTBurningChunk.h"

namespace
{
// UE1 rotator units → degrees
constexpr float RotUnitsToDeg = 360.0f / 65536.0f;
}

AWoTFireballFire::AWoTFireballFire()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AWoTFireballFire::Initialize(AActor* InFollowActor)
{
	FollowActor = InFollowActor;
	if (IsValid(FollowActor))
	{
		RelativeOffset = FollowActor->GetActorRotation().UnrotateVector(
			GetActorLocation() - FollowActor->GetActorLocation());
		RelativeRotation = GetActorRotation() - FollowActor->GetActorRotation();
	}
}

void AWoTFireballFire::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(Duration);
	Flame = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("Flame01"),
		GetActorLocation(), GetActorRotation(), this, true);
	if (Flame)
	{
		InitialVolume = Flame->Volume;
	}
}

void AWoTFireballFire::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const bool bFollowAlive = IsValid(FollowActor);
	if (bFollowAlive)
	{
		const FRotator FollowRotation = FollowActor->GetActorRotation();
		SetActorLocation(FollowActor->GetActorLocation()
			+ FollowRotation.RotateVector(RelativeOffset));
		SetActorRotation(FollowRotation + RelativeRotation);
	}
	if (Flame)
	{
		Flame->bOn = bFollowAlive;
		Flame->Volume = InitialVolume * FMath::Clamp(
			GetLifeSpan() / FMath::Max(Duration, UE_SMALL_NUMBER), 0.0f, 1.0f);
	}
}

void AWoTFireballFire::Destroyed()
{
	if (IsValid(Flame))
	{
		Flame->Destroy();
	}
	Flame = nullptr;
	Super::Destroyed();
}

AWoTFireballProjectile::AWoTFireballProjectile()
{
	ExplosionEffectName = FName(TEXT("FireballExplode3"));
	HitWaterEffectName = TEXT("FireballFizzle");
	HitWaterEffectOffset = FVector(0, 0, 56);
	// HitWall scorch (AngrealFireballProjectile.uc:151-153: ScorchMark with
	// BurnMark.DrawScale = 2.0 before Align).
	ImpactDecalType = FName(TEXT("ScorchMark"));
	ImpactDecalDrawScale = 2.0f;
	PrimaryActorTick.bCanEverTick = true;

	// defaults from AngrealFireballProjectile.uc
	ScaleGlow = 5.0f;
	AmbientGlow = 60.0f;
	Speed = 1000.0f;
	InitialLifeSpan = 140.0f;   // engine Projectile default (a miss dies at 140s)
	Damage = 40.0f;
	DamageRadius = 220.0f;
	MomentumTransfer = 3500;
	DamageType = "xxFxx";
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6 (height 12 cyl)

	RockMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("RockMesh"));
	RockMesh->SetupAttachment(RootComponent);
	RockMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RockMesh->SetRelativeScale3D(FVector(0.9f));   // DrawScale=0.9

	// AngrealFireballProjectile.uc:225-230 — the authored Radius=8 resolves
	// through WorldLightRadius to 25*(8+1)=225uu.
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);   // configured after payload activation

	// Sound'Angreal.AngrealInventoryFireball.*' → /Game/WoT/Sounds/Angreal/*
	// (deterministic import paths — Appendix A soft-path CDO defaults)
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/Launch2FB.Launch2FB"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitLevelFB.HitLevelFB"));
	BurstSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchFB.LaunchFB"));
	HitPawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitPawnFB.HitPawnFB"));
	HitWaterSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitWaterFB.HitWaterFB"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopFB.LoopFB"));
	SoundRadius = 160;             // AmbientSound (LoopFB)
	SoundVolume = 255;
	SoundPitch = 96;
	ImpactSoundPitch = 0.5f;       // deep boom (HitLevelFB)
	HitWaterSoundPitch = 1.2f;     // HitWaterFB
}

void AWoTFireballProjectile::BeginPlay()
{
	Super::BeginPlay();
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealFireballProjectile"));
	Light->SetVisibility(false);   // LightType=LT_None until burst
	BindWeaveMesh(RockMesh, FName(TEXT("LavaRock")), 0.9f);
	SpawnTime = GetWorld()->GetTimeSeconds();
	// PreBeginPlay: random initial roll
	FRotator R = GetActorRotation();
	R.Roll = WoTRandom::FRand() * 360.0f;
	SetActorRotation(R);
}

void AWoTFireballProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const float T = GetWorld()->GetTimeSeconds() - SpawnTime;

	if (T <= RockDuration)
	{
		// tumbling LavaRock
		RockMesh->AddLocalRotation(
			FRotator(0, 0, RockRollRate * RotUnitsToDeg * DeltaTime));
	}
	else
	{
		RockMesh->SetVisibility(false);   // DrawType = DT_None
	}

	if (T > SprayerDelay && !GetVelocity().IsNearlyZero())
	{
		// The trail bundles (Tick lazy-spawn as shipped; the head's 0.1s LifeSpan
		// makes it a self-refreshing burst).
		const FRotator VeloDir = GetVelocity().Rotation();
		if (!IsValid(Head))
		{
			Head = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("FireballSprayer2"),
			                              GetActorLocation(), VeloDir, this,
			                              /*bRot*/true, /*bShift*/true);
		}
		if (!IsValid(Cone))
		{
			Cone = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("FireballFlame"),
			                              GetActorLocation(), VeloDir, this, true);
		}
		if (!IsValid(Smoke))
		{
			Smoke = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("FireballSmoke"),
			                               GetActorLocation(), VeloDir, this, true);
		}
		Light->SetVisibility(true);       // LightType = default (steady)
		if (!bBurstSoundPlayed)
		{
			bBurstSoundPlayed = true;
			// AngrealFireballProjectile.uc:74: simulated Tick — each machine's
			// own play, volume 1, unset radius 1600.
			if (USoundBase* S = WoTSound::Resolve(BurstSound))
			{
				WoTSound::PlayOneShot(this, S, GetActorLocation(),
					WoTSound::DefaultPlaySoundRadius);
			}
		}
		// FireballSprayer2/FireballFlame/FireballSmoke spawns: Niagara pass
	}
}

void AWoTFireballProjectile::Destroyed()
{
	// Fireball.Destroyed: the trail fades out over 5s.
	WoTWeaveFX::FadeOut(Head, 5.0f);
	WoTWeaveFX::FadeOut(Cone, 5.0f);
	WoTWeaveFX::FadeOut(Smoke, 5.0f);
	Super::Destroyed();
}

bool AWoTFireballProjectile::ShouldSpawnScorchForImpact(
	const AActor* ImpactActor, const UPrimitiveComponent* ImpactComponent)
{
	if (!IsValid(ImpactActor) || ImpactActor->IsA<APawn>()) return false;
	// Retail tests HitActor.IsA('LevelInfo'), not Actor.bWorldGeometry or a
	// collision channel. Runtime-projected BSP is owned by AWoTRuntimeBSPWorld;
	// editor/imported UE BSP exposes UModelComponent surfaces.
	return ImpactActor->IsA<AWoTRuntimeBSPWorld>()
		|| (IsValid(ImpactComponent) && ImpactComponent->IsA<UModelComponent>());
}

void AWoTFireballProjectile::CaptureImpactContext(AActor* Other,
	const UPrimitiveComponent* OtherComponent)
{
	bHasImpactSnapshot = true;
	bImpactWasNonPawn = Other && !Other->IsA<APawn>();
	ImpactFollowActor = bImpactWasNonPawn ? Other : nullptr;
	const UPrimitiveComponent* Primitive = OtherComponent;
	if (!Primitive && Other)
	{
		Primitive = Cast<UPrimitiveComponent>(Other->GetRootComponent());
	}
	bImpactWasLevelGeometry = bImpactWasNonPawn
		&& ShouldSpawnScorchForImpact(Other, Primitive);
}

void AWoTFireballProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	if (!bHasImpactSnapshot)
	{
		CaptureImpactContext(HitActor, HitPrimitive);
	}
	// AngrealFireballProjectile.uc:142: BlastBits is aimed along the impact normal.
	if (GetWorld()) { WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("BlastBits"), HitLocation, HitNormal.Rotation()); }
	// HitWall creates ScorchMark only for Level geometry (uc:146-154). BlockAll
	// and Ice are actors even when their collision blocks the projectile.
	const FName SavedDecal = ImpactDecalType;
	if (GetWorld() && bImpactWasNonPawn)
	{
		AWoTBurningChunk::SpawnFan(*GetWorld(), AWoTBurningChunk::StaticClass(),
			HitLocation, HitNormal, this);
	}
	if (GetWorld() && bImpactWasNonPawn && !bImpactWasLevelGeometry)
	{
		const FTransform At(FVector::UpVector.Rotation(), HitLocation);
		if (AWoTFireballFire* Fire =
			GetWorld()->SpawnActorDeferred<AWoTFireballFire>(
				AWoTFireballFire::StaticClass(), At, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			Fire->Initialize(ImpactFollowActor);
			Fire->FinishSpawning(At);
		}
	}
	if (!bImpactWasLevelGeometry)
	{
		ImpactDecalType = NAME_None;
	}
	Super::Explode(HitLocation, HitNormal);
	ImpactDecalType = SavedDecal;
}
