#include "WoTTaintProjectile.h"
#include "../WoTMeshFX.h"

#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"

AWoTTaintProjectile::AWoTTaintProjectile()
{
	// defaults: speed=150, MomentumTransfer=2000, LifeSpan=0, collision 6x12
	Speed = 150.0f;
	MomentumTransfer = 2000;
	InitialLifeSpan = 0.0f;
	Collision->InitSphereRadius(6.0f);

	// Sound'Angreal.Taint.*'
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchTN.LaunchTN"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitTN.HitTN"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopTN.LoopTN"));
	SoundRadius = 160;             // AmbientSound (LoopTN)
	SoundVolume = 255;
}

void AWoTTaintProjectile::BeginPlay()
{
	Super::BeginPlay();
	Aura = WoTMeshFX::SpawnTaintVisual(*GetWorld(), this);
}

void AWoTTaintProjectile::Destroyed()
{
	if (IsValid(Aura)) { Aura->Destroy(); }
	Super::Destroyed();
}

void AWoTTaintProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	AActor* VictimActor = IsValid(Destination.Get()) ? Destination.Get() : HitActor.Get();
	if (GetWorld())
	{
		const FVector At = VictimActor ? VictimActor->GetActorLocation() : HitLocation;
		for (FName Config : {FName(TEXT("TaintExpDarts")), FName(TEXT("TaintExpTorus")),
			FName(TEXT("TaintExpGeoA")), FName(TEXT("TaintExpGeoB"))})
		{
			WoTMeshFX::SpawnLocal(*GetWorld(), Config, At, FRotator::ZeroRotator, VictimActor);
		}
	}
	// TaintAllAngrealEffect.Invoke, inlined: taint everything the victim holds
	if (HasAuthority())
	{
		if (APawn* Victim = Cast<APawn>(HitActor))
		{
			if (AWoTCharacter* Character = Cast<AWoTCharacter>(Victim);
				Character && Character->IsEffectBlocked(
					SourceAngreal, /*bEffectDeleterious*/ true,
					/*bEmitFeedback*/ true))
			{
				Super::Explode(HitLocation, HitNormal);
				return;
			}
			if (const UWoTInventoryComponent* Inv =
			        Victim->FindComponentByClass<UWoTInventoryComponent>())
			{
				for (AWoTInventory* Item : Inv->Items)
				{
					if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
					{
						Angreal->bTainted = true;
						Angreal->TaintInstigator = GetInstigator();
					}
				}
			}
		}
	}
	// TaintExpDarts/Torus/GeoA/GeoB burst — effects pass
	Super::Explode(HitLocation, HitNormal);
}
