#include "WoTTracerProjectile.h"
#include "../WoTWeaveFX.h"
#include "../WoTVertexAnimComponent.h"

#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../AI/WoTCreatureData.h"
#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"

AWoTTracerProjectile::AWoTTracerProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// AngrealTracerProjectile defaultproperties
	Speed = 1.0f;
	MaxSpeed = 500.0f;
	Acceleration = 150.0f;
	bDesperateSeeker = true;
	bTakeShortcuts = false;
	ReachTolerance = 20.0f;
	bNotifiesDestination = false;
	InitialLifeSpan = 0.0f;             // LifeSpan=0: no auto-destroy
	Collision->InitSphereRadius(16.0f); // CollisionRadius=16, CollisionHeight=16

	// Sound'Angreal.Tracer.*' (import flattens GROUP=Tracer into .../Angreal/)
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateTR.ActivateTR"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ImpactTR.ImpactTR"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopTR.LoopTR"));
	SoundRadius = 64;              // AmbientSound (LoopTR) attenuation
	ImpactSoundRadius = 700.0f;    // ImpactTR one-shot radius
}

// AngrealTracerProjectile.SetDestination: remember which seal the destination
// carries (a pawn) or is (a world seal), so Timer()/RetargetSeal can re-follow.
void AWoTTracerProjectile::SetDestination(AActor* Dest)
{
	if (AWoTSeal* Carried = FindCarriedSeal(Dest))
	{
		TrackingSeal = Carried;
	}
	else if (AWoTSeal* Seal = Cast<AWoTSeal>(Dest))
	{
		TrackingSeal = Seal;
	}
	Super::SetDestination(Dest);
}

void AWoTTracerProjectile::NotifySeekerBounced(AWoTPathNode*)
{
	// AngrealTracerProjectile.uc:119-123: unlike an ordinary seeker, a
	// bouncing tracer commits to the seal's last known position and never
	// asks the navigation backend for another direction.
	ProjectileMovement->Velocity =
		(DestLoc - GetActorLocation()).GetSafeNormal()
		* ProjectileMovement->Velocity.Size();
	bDisableCalcNextDirection = true;
}

// AngrealTracerProjectile.Timer (1Hz): follow the seal across pickup/drop.
void AWoTTracerProjectile::RetargetSeal()
{
	if (Cast<APawn>(Destination))
	{
		// Following a carrier — did they drop it?
		if (AWoTSeal* Carried = FindCarriedSeal(Destination))
		{
			TrackingSeal = Carried;
		}
		else
		{
			SetDestination(TrackingSeal);   // dropped -> chase the seal itself
		}
	}
	else if (AWoTSeal* DestSeal = Cast<AWoTSeal>(Destination))
	{
		// Following a world seal — did someone pick it up?
		if (DestSeal->GetItemState() != EWoTInventoryState::Pickup)
		{
			SetDestination(DestSeal->GetOwner());   // picked up -> chase the carrier
		}
	}
}

void AWoTTracerProjectile::Tick(float DeltaTime)
{
	if (HasAuthority())
	{
		RetargetTimer -= DeltaTime;
		if (RetargetTimer <= 0.0f)
		{
			RetargetTimer = 1.0f;
			RetargetSeal();
		}
	}

	Super::Tick(DeltaTime);

	// SpinRate=(Roll=65000): a spinning weave. The base seeking Tick aligns
	// pitch/yaw to velocity and preserves Roll, so a roll spin survives. (The
	// original accumulates Spin every tick, producing an accelerating roll.)
	if (SpinAccelerationRollDegPerSec2 != 0.0f)
	{
		SpinRollDegPerSec += SpinAccelerationRollDegPerSec2 * DeltaTime;
		AddActorLocalRotation(FRotator(0.0f, 0.0f, SpinRollDegPerSec * DeltaTime));
	}
}

// Pawn.FindInventoryType(class'Seal'): the seal a pawn is carrying, or null.
AWoTSeal* AWoTTracerProjectile::FindCarriedSeal(AActor* PawnActor)
{
	AWoTCharacter* Char = Cast<AWoTCharacter>(PawnActor);
	if (!Char || !Char->InventoryComponent)
	{
		return nullptr;
	}
	return Cast<AWoTSeal>(
		Char->InventoryComponent->FindInventoryType(AWoTSeal::StaticClass()));
}

void AWoTTracerProjectile::BeginPlay()
{
	SpinAccelerationRollDegPerSec2 =
		WoTCreatureData::TracerSpinAccelerationRollDegPerSec2();
	Super::BeginPlay();
	// TracerSprayer, aimed up (rotator(vect(0,0,1))), FollowActor model. The
	// shipped VisibilityRadius/Height=0 overrides kill distance scaling;
	// MinVolume=Volume is the ParticleDensity floor — a no-op at max density.
	Trail = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("TracerSprayer"),
	                               GetActorLocation(), FRotator(90, 0, 0), this);
	if (Trail)
	{
		Trail->VisibilityRadius = 0.0f;
		Trail->VisibilityHeight = 0.0f;
	}
	BindWeaveMesh(DisplayMesh, FName(TEXT("Tracer")), 1.0f);
	if (DisplayMesh->HasDisplayMesh())
	{
		DisplayMesh->LoopAnim(FName(TEXT("Tracer")), 0.1f);
	}
}

void AWoTTracerProjectile::Destroyed()
{
	// Tracer expiry: Volume=0 + LifeSpan 15 — existing motes drift out their
	// authored 15s while emission stops.
	if (IsValid(Trail))
	{
		Trail->Volume = 0.0f;
		Trail->bOn = false;
		Trail->FollowActor = nullptr;
		Trail->SetLifeSpan(15.0f);
	}
	Super::Destroyed();
}
