#include "WoTAngrealAMAProjectile.h"
#include "../WoTVertexAnimComponent.h"

#include "WoTAMAPearl.h"
#include "WoTProjectile.h"
#include "../WoTMeshLight.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"

#include "Components/SphereComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealAMAProjectile::AWoTAngrealAMAProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// AngrealAMAProjectile defaultproperties (simulation subset).
	Speed = 200.0f;
	Damage = 0.0f;
	DamageRadius = 0.0f;
	bExplode = false;          // never explodes — it places, then plants the dome
	TriggerRadius = 150.0f;
	// A placed mine is persistent and must carry its Placing/Waiting/Fading
	// presentation transitions after the initial spawn bunch.
	bNetTemporary = false;
	bAlwaysRelevant = true;
	SetUE1CollisionSize(12.0f, 10.0f);   // retail 12x10 cylinder

	// PHYS_Falling: lobbed onto the ground.
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	CastChecked<UWoTProjectileMovementComponent>(ProjectileMovement)
		->bUseRetailFallingGravity = true;

	// The retail global Touch rides the mine's own CollisionRadius=12 sphere.
	CylinderCollision->OnComponentBeginOverlap.AddDynamic(
		this, &AWoTAngrealAMAProjectile::OnMineBeginOverlap);
	CylinderCollision->OnComponentEndOverlap.AddDynamic(
		this, &AWoTAngrealAMAProjectile::OnMineEndOverlap);

	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchAM.LaunchAM"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/PlaceAM.PlaceAM"));
	HitWaterSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitWaterAM.HitWaterAM"));
}

void AWoTAngrealAMAProjectile::NotifyHit(UPrimitiveComponent*, AActor*,
	                                     UPrimitiveComponent*, bool, FVector,
	                                     FVector HitNormal, FVector, const FHitResult&)
{
	// HitWall/Landed -> Place (the base would Explode; we deliberately don't call it).
	if (HasAuthority() && !bPlaced)
	{
		Place(HitNormal);
	}
}

void AWoTAngrealAMAProjectile::Place(const FVector& HitNormal)
{
	if (bPlaced)
	{
		return;
	}
	bPlaced = true;

	// Freeze where we landed.
	ProjectileMovement->StopMovementImmediately();
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	CastChecked<UWoTProjectileMovementComponent>(ProjectileMovement)
		->bUseRetailFallingGravity = false;
	SetActorRotation(HitNormal.Rotation());
	if (UWorld* World = GetWorld())
	{
		WoTSound::PlayReplicated(*World, TEXT("Angreal/PlaceAM"),
			GetActorLocation(), WoTSound::WorldSoundRadius(32));
	}

	// Prime, then arm (the original's Priming Sleep -> Waiting).
	if (PrimeTime > 0.0f)
	{
		GetWorldTimerManager().SetTimer(PrimeTimer, this,
			&AWoTAngrealAMAProjectile::Arm, PrimeTime, false);
	}
	else
	{
		Arm();
	}
}

void AWoTAngrealAMAProjectile::Arm()
{
	// Priming -> Waiting. Waiting.BeginPlay's SetCollisionSize(TriggerRadius)
	// (AngrealAMAProjectile.uc:111-114) never runs — BeginPlay is only sent at
	// spawn — so arming changes nothing about what trips the mine.
	bArmed = true;
}

void AWoTAngrealAMAProjectile::Touch(AActor* Other)
{
	// Global Touch (AngrealAMAProjectile.uc:33-43): `function`, so authority only;
	// active in Placing, Priming and Waiting alike (Fading turns collision off).
	if (!HasAuthority() || bFading || !IsValid(Other) || Other == this)
	{
		return;
	}
	if (Cast<APawn>(Other) || Cast<AWoTProjectile>(Other))
	{
		PlantDome();
	}
}

void AWoTAngrealAMAProjectile::OnMineBeginOverlap(UPrimitiveComponent*,
	AActor* Other, UPrimitiveComponent*, int32, bool bFromSweep, const FHitResult&)
{
	if (!IsValid(Other))
	{
		return;
	}
	// UE1 sends no Touch at spawn, and leaving a cylinder you started inside is
	// not a hit (UE1's cylinder LineCheck, when already inside, only opposes further
	// inward movement): an actor overlapping at spawn touches only after it has left.
	if (SpawnOverlaps.Contains(Other))
	{
		return;
	}
	Touch(Other);
}

void AWoTAngrealAMAProjectile::OnMineEndOverlap(UPrimitiveComponent*,
	AActor* Other, UPrimitiveComponent*, int32)
{
	SpawnOverlaps.Remove(Other);
}

void AWoTAngrealAMAProjectile::PlantDome()
{
	// One dome per mine — a single proximity poll can report several fresh pawns,
	// but Touch spawns exactly one AMAPearl (AngrealAMAProjectile.uc:37-41).
	if (bTriggered || !HasAuthority())
	{
		return;
	}
	bTriggered = true;
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.Instigator = GetInstigator();
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AWoTAMAPearl* Pearl = GetWorld()->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), GetActorLocation(), FRotator::ZeroRotator, Params))
	{
		Pearl->SetSourceAngreal(SourceAngreal);
		Pearl->Go();
	}
	// The shipped Fading state is unreachable. The box stays present until the
	// pearl's first ProjectileList poll absorbs it with UnravelAM and WOTSparks.
}

void AWoTAngrealAMAProjectile::BeginPlay()
{
	// Who the mine overlaps at spawn, queried with its own shape and responses
	// before UpdateInitialOverlaps (which runs right after BeginPlay) reports them.
	if (UWorld* World = GetWorld(); World && CylinderCollision)
	{
		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AMASpawnOverlaps), false, this);
		FCollisionResponseParams Responses;
		Responses.CollisionResponse = CylinderCollision->GetCollisionResponseToChannels();
		World->OverlapMultiByChannel(Overlaps, CylinderCollision->GetComponentLocation(),
			CylinderCollision->GetComponentQuat(), CylinderCollision->GetCollisionObjectType(),
			CylinderCollision->GetCollisionShape(), Params, Responses);
		for (const FOverlapResult& Overlap : Overlaps)
		{
			if (AActor* Actor = Overlap.GetActor(); IsValid(Actor) && Actor != this)
			{
				SpawnOverlaps.Add(Actor);
			}
		}
	}
	Super::BeginPlay();
	BindWeaveMesh(DisplayMesh, FName(TEXT("AngrealAntiMagicAuraPickup")), 1.0f);
}

void AWoTAngrealAMAProjectile::OnRep_Fading()
{
	if (bFading) { BeginFadingPresentation(); }
}

void AWoTAngrealAMAProjectile::BeginFadingPresentation()
{
	ProjectileMovement->StopMovementImmediately();
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	CastChecked<UWoTProjectileMovementComponent>(ProjectileMovement)
		->bUseRetailFallingGravity = false;
	SetActorEnableCollision(false);
	bStyleTranslucent = true;
}

void AWoTAngrealAMAProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bFading) { return; }
	ScaleGlow = FMath::Max(0.0f, ScaleGlow - 1.5f * DeltaTime);
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		MeshLight->ForceRuntimeBake();
	}
	if (HasAuthority() && ScaleGlow <= 0.0f)
	{
		Destroy();
	}
}

void AWoTAngrealAMAProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAngrealAMAProjectile, bFading);
}
