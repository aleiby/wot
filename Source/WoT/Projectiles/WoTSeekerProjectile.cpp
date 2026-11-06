#include "WoTSeekerProjectile.h"
#include "../WoTWeaveFX.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTMeshFX.h"
#include "../WoTUE1LightRegistry.h"
#include "../Assets/WoTRuntimeAssets.h"

#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"

AWoTSeekerProjectile::AWoTSeekerProjectile()
{
	ExplosionEffectName = FName(TEXT("GreenExplode"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);

	// defaults: Speed=150, Damage=60, DamageRadius=240, MomentumTransfer=2000,
	// collision 6x12
	Speed = 150.0f;
	Damage = 60.0f;
	DamageRadius = 240.0f;
	MomentumTransfer = 2000;
	Collision->InitSphereRadius(6.0f);

	// AngrealSeekerProjectile.uc:112-115: LightEffect and LightRadius are
	// omitted, so both resolve to zero (LE_None and a 25uu world radius).
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);

	// Sound'Angreal.AngrealSeekerProjectile.*'
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchSK.LaunchSK"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitSK.HitSK"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopSK.LoopSK"));
	SoundRadius = 160;             // AmbientSound (LoopSK)
	SoundVolume = 255;
}

void AWoTSeekerProjectile::BeginPlay()
{
	Super::BeginPlay();
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealSeekerProjectile"));
	SmokeTrail = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("SeekerSmoke"),
	                                    GetActorLocation(), GetActorRotation(), this,
	                                    /*bRot*/true);
	Globe = WoTMeshFX::SpawnSpriteLocal(*GetWorld(), TEXT("SeekerGlobe"),
		GetActorLocation(), this);
	if (IsValid(Globe))
	{
		// AngrealSeekerGlobe.uc:18-22 bases the globe on its owner. Keep its
		// center with the projectile through movement and same-frame teleports.
		Globe->AttachToActor(this, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	}
	// FireTexture SLightning01 — the live procedural particle pair (6b-ii
	// masters); the fractal subsystem neutralizes the grid and binds its live
	// 256x256 field after spawn.
	Sprite->SetSpriteMaterials(
		UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(TEXT("/Game/WoT/Materials/Instances/MI_PSAddFB_Angreal__SLightning01.MI_PSAddFB_Angreal__SLightning01"))),
		UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(TEXT("/Game/WoT/Materials/Instances/MI_PSModFB_Angreal__SLightning01.MI_PSModFB_Angreal__SLightning01"))),
		256.0f, 256.0f, FName(TEXT("Angreal__SLightning01")));
}

void AWoTSeekerProjectile::Destroyed()
{
	// Seeker.Destroyed: the smoke fades over 2s.
	WoTWeaveFX::FadeOut(SmokeTrail, 2.0f);
	if (IsValid(Globe)) { Globe->Destroy(); }
	Super::Destroyed();
}
