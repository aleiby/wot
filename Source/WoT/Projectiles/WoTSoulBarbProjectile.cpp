#include "WoTSoulBarbProjectile.h"
#include "../WoTUE1LightRegistry.h"
#include "Components/PointLightComponent.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"
#include "../WoTMeshFX.h"
#include "../Assets/WoTRuntimeAssets.h"

#include "../Inventory/WoTRarityBasedDamageReflector.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"

AWoTSoulBarbProjectile::AWoTSoulBarbProjectile()
{
	Flare = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Flare"));
	Flare->SetupAttachment(RootComponent);
	Flare->DrawScale = 0.8f;
	Hole = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Hole"));
	Hole->SetupAttachment(RootComponent);
	Hole->DrawScale = 0.6f;
	Hole->bModulateOnly = true;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// AngrealSoulBarbProjectile defaultproperties (simulation subset).
	Duration = 15.0f;
	Speed = 300.0f;
	MomentumTransfer = 2000;
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6

	// Sound'Angreal.SoulBarb.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchSB.LaunchSB"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitSB.HitSB"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopSB.LoopSB"));
	SoundRadius = 160;             // AmbientSound (LoopSB)
	SoundVolume = 100;
}

void AWoTSoulBarbProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	// AngrealSoulBarbProjectile.uc:107-110: the impact mesh exists only when the
	// seeking destination is still valid. A wall/invalid-target explosion has no
	// fallback impact at HitActor or HitLocation.
	AActor* VictimActor = DestinationExists() ? Destination.Get() : nullptr;
	if (GetWorld() && VictimActor)
	{
		WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("SoulBarbImpact"),
			VictimActor->GetActorLocation(),
			FRotator::ZeroRotator, VictimActor);
	}
	// SoulBarbImpact visual (spawned at the destination) — deferred presentation.

	// InstallReflectorEffect inlined: install a RarityBasedDamageReflector on the
	// victim for Duration — thereafter it takes rarity-scaled damage each time it
	// uses an artifact charge.
	if (HasAuthority())
	{
		if (APawn* Victim = Cast<APawn>(HitActor))
		{
			UWoTRarityBasedDamageReflector* Reflector =
				NewObject<UWoTRarityBasedDamageReflector>(Victim);
			Reflector->SetSourceAngreal(SourceAngreal);
			Reflector->Lifespan = Duration;
			Reflector->bFromProjectile = true;   // InstallReflectorEffect.uc:51-53
			Reflector->Install(Victim);
		}
	}

	Super::Explode(HitLocation, HitNormal);
}

void AWoTSoulBarbProjectile::BeginPlay()
{
	Super::BeginPlay();
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealSoulBarbProjectile"));
	Flare->SetSpriteMaterials(
		UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(TEXT("/Game/WoT/Materials/Instances/MI_PSAddFB_Angreal__NimbusAnim.MI_PSAddFB_Angreal__NimbusAnim"))),
		UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(TEXT("/Game/WoT/Materials/Instances/MI_PSModFB_Angreal__NimbusAnim.MI_PSModFB_Angreal__NimbusAnim"))),
		64.0f, 64.0f);
	TArray<FString> Disk;
	Disk.Add(TEXT("MODdiskC"));
	Hole->SetSpriteSeries(TEXT("Angreal"), Disk, 64.0f, 64.0f);
	SmokeTrail = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("SoulBarbSmoke"),
		GetActorLocation(), (-GetVelocity()).Rotation(), this);
}

void AWoTSoulBarbProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (IsValid(SmokeTrail) && !GetVelocity().IsNearlyZero())
	{
		SmokeTrail->SetActorRotation((-GetVelocity()).Rotation());
	}
}

void AWoTSoulBarbProjectile::Destroyed()
{
	WoTWeaveFX::FadeOut(SmokeTrail, 5.0f);
	Super::Destroyed();
}
