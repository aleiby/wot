#include "WoTWallOfAirProjectile.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSound.h"
#include "Components/PointLightComponent.h"
#include "../WoTVertexAnimComponent.h"

#include "../Inventory/WoTAngrealInventory.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTUE1Names.h"
#include "../WoTWeaveFX.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

AWoTWallOfAirProjectile::AWoTWallOfAirProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// AngrealWallOfAirProjectile defaultproperties.
	PctDistFromSource = 0.333333f;
	UnAffectedTypes[0] = FName(TEXT("MashadarGuide"));
	UnAffectedTypes[1] = FName(TEXT("MachinShin"));

	// Physics=PHYS_None: the wall never integrates velocity — Tick re-plants it.
	ProjectileMovement->bAutoActivate = false;

	// bNetTemporary=False: a persistent wall whose plant point keeps replicating
	// (the base Projectile default is a fire-and-forget temporary).
	bNetTemporary = false;
	InitialLifeSpan = 0.0f;   // LifeSpan=0: lives until the caster/target dies

	// AngrealWallOfAirProjectile.uc: CollisionRadius=75, CollisionHeight=75.
	SetUE1CollisionSize(75.0f, 75.0f);
	// bCollideWorld=False: overlaps pawns/weaves (profile) but ignores world geo.
	CylinderCollision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
	CylinderCollision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	// Our own touch handler catches weaves the base Touch filter (pawns/blockers)
	// would reject — AngrealWallOfAirProjectile overrides Touch wholesale.
	CylinderCollision->OnComponentBeginOverlap.AddDynamic(
		this, &AWoTWallOfAirProjectile::OnWallTouch);

	// Sound'Angreal.WallOfAir.*' (import flattens GROUP=WallOfAir into .../Angreal/).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchWA.LaunchWA"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopWA.LoopWA"));
	DeflectSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/DeflectWA.DeflectWA"));
	SoundRadius = 255;   // AmbientSound (LoopWA)
	SoundVolume = 255;
}

void AWoTWallOfAirProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// replication { unreliable if(Role==ROLE_Authority) SourceLocation, DestinationLocation }
	// (reliability nuance folded, cf. WhirlwindLeech.DesiredLocation).
	DOREPLIFETIME(AWoTWallOfAirProjectile, SourceLocation);
	DOREPLIFETIME(AWoTWallOfAirProjectile, DestinationLocation);
}

void AWoTWallOfAirProjectile::BeginPlay()
{
	Super::BeginPlay();   // starts the ambient loop (LoopWA)
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealWallOfAirProjectile"));
	BindWeaveMesh(DisplayMesh, FName(TEXT("WallOfAir")), 1.0f);
	if (DisplayMesh->HasDisplayMesh())
	{
		DisplayMesh->LoopAnim(FName(TEXT("All")), 1.0f);
	}

	// PreBeginPlay: PlaySound(SpawnSound). LoopAnim('ALL',1.0) is presentation.
	// (AngrealWallOfAirProjectile.uc:53, simulated: each machine's own play,
	// volume 1, unset radius 1600.)
	if (USoundBase* S = WoTSound::Resolve(SpawnSound))
	{
		WoTSound::PlayOneShot(this, S, GetActorLocation(),
			WoTSound::DefaultPlaySoundRadius);
	}
}

void AWoTWallOfAirProjectile::Tick(float DeltaTime)
{
	// Authority removes the wall when either endpoint is missing or dead
	// (AngrealWallOfAirProjectile.uc:60-65).
	if (HasAuthority())
	{
		const AWoTCharacter* Src = Cast<AWoTCharacter>(Source);
		const AWoTCharacter* Dst = Cast<AWoTCharacter>(Destination);
		if (!Src || !Src->IsAlive() || !Dst || !Dst->IsAlive())
		{
			Destroy();
			return;
		}

		if (Source)
		{
			SourceLocation = Source->GetActorLocation();
		}
		if (Destination)
		{
			DestinationLocation = Destination->GetActorLocation();
		}
	}

	// Plant the wall a third of the way along, facing the target.
	SetActorLocation(SourceLocation
		+ (DestinationLocation - SourceLocation) * PctDistFromSource);
	AlignPresentationToVelocity(DestinationLocation - SourceLocation);

	Super::Tick(DeltaTime);
}

void AWoTWallOfAirProjectile::Destroyed()
{
	// Tell the ter'angreal to clean up (drop the leeches, end the cast).
	if (HasAuthority() && IsValid(SourceAngreal))
	{
		SourceAngreal->UnCastArtifact();
	}
	Super::Destroyed();
}

bool AWoTWallOfAirProjectile::IsAffectable(const AActor* Other) const
{
	if (!Other)
	{
		return false;
	}
	// UE1 IsA(name): MashadarGuide/MachinShin pass through untouched. The former
	// is a native port; the latter remains a harmless unported nonmatch.
	for (const FName& TypeName : UnAffectedTypes)
	{
		if (WoTUE1Names::ActorIsA(Other, TypeName))
		{
			return false;
		}
	}
	return true;
}

void AWoTWallOfAirProjectile::OnWallTouch(UPrimitiveComponent* /*OverlappedComp*/,
	AActor* Other, UPrimitiveComponent* /*OtherComp*/, int32 /*BodyIndex*/,
	bool /*bFromSweep*/, const FHitResult& /*Hit*/)
{
	TouchActor(Other);
}

void AWoTWallOfAirProjectile::TouchActor(AActor* Other)
{
	if (!IsValid(Other) || Other == this || !IsAffectable(Other))
	{
		return;
	}

	const FVector ImpactLocation = Other->GetActorLocation();
	const FRotator ImpactRotation = (ImpactLocation - GetActorLocation()).Rotation();
	bool bDeflected = false;
	if (AWoTAngrealProjectile* Weave = Cast<AWoTAngrealProjectile>(Other))
	{
		// Unravel any weave that strikes the wall.
		// AngrealWallOfAirProjectile.uc:108 Other.PlaySound(DeflectSound) in the
		// simulated Touch: a local play at the weave, volume 1, radius 1600.
		if (USoundBase* S = WoTSound::Resolve(DeflectSound))
		{
			WoTSound::PlayOneShot(this, S, Weave->GetActorLocation(),
				WoTSound::DefaultPlaySoundRadius);
		}
		Weave->Destroy();
		bDeflected = true;
	}
	else if (AWoTProjectile* Proj = Cast<AWoTProjectile>(Other))
	{
		// A non-angreal projectile is knocked to the ground. UE1: Velocity=(0,0,-1),
		// Acceleration=(0,0,-950) -> it falls. Our ProjectileMovement carries no
		// gravity by default, so give it the UE1 -950 (world -980 * 950/980, the
		// established movement-parity gravity) and a downward nudge. The UE1
		// SetLocation(Location) bJustTeleported trick isn't needed here.
		if (UProjectileMovementComponent* PM = Proj->ProjectileMovement)
		{
			PM->Velocity = FVector(0.0f, 0.0f, -1.0f);
			PM->ProjectileGravityScale = 950.0f / 980.0f;
			PM->Activate();
		}
		bDeflected = true;
	}

	if (bDeflected)
	{
		WoTWeaveFX::SpawnBurstMarker(
			*GetWorld(), FName(TEXT("WOTSparks")), ImpactLocation, ImpactRotation);
		++SparkBurstCount;
	}
}
