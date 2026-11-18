#include "WoTDartProjectile.h"
#include "Components/PointLightComponent.h"
#include "../WoTMeshFX.h"
#include "../WoTMeshLight.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTVertexAnimComponent.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Net/UnrealNetwork.h"

namespace
{
	float DartHue(FName Color)
	{
		if (Color == TEXT("Red")) { return 0.0f; }
		if (Color == TEXT("Green")) { return 110.0f; }
		if (Color == TEXT("Yellow")) { return 30.0f; }
		if (Color == TEXT("Purple")) { return 200.0f; }
		return 154.0f;
	}

	bool ConfigureDartLight(UPointLightComponent* Light,
		FName RegistryKey, uint8 Hue)
	{
		if (Light == nullptr || (Light->GetWorld() != nullptr
			&& Light->GetWorld()->GetNetMode() == NM_DedicatedServer))
		{
			if (Light != nullptr) Light->SetVisibility(false);
			return false;
		}
		const FWoTUE1LightRow* Base = WoTUE1LightRegistry::Find(RegistryKey);
		if (Base == nullptr)
		{
			if (Light != nullptr) Light->SetVisibility(false);
			return false;
		}
		FWoTUE1LightRow Row = *Base;
		Row.Hue = Hue;
		return WoTUE1LightRegistry::Configure(Light, Row);
	}
}

AWoTDartProjectile::AWoTDartProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// AngrealDartProjectile.uc:121-127 display defaults.
	ScaleGlow = 4.0f;
	AmbientGlow = 211.0f;
	bUnlit = true;
	bStyleTranslucent = true;
	Speed = 2000.0f;
	InitialLifeSpan = 140.0f;   // engine Projectile default (a miss dies at 140s)
	Damage = 10.0f;
	DamageType = "AxFxx";                 // Air + Fire
	Collision->InitSphereRadius(6.0f);    // CollisionRadius=6
	// Retail Tick owns rotator(Velocity)+Roll. Letting the movement component
	// also face velocity later in the frame races and erases the accumulated roll.
	ProjectileMovement->bRotationFollowsVelocity = false;

	// Sound'Angreal.AngrealInventoryDart.*' — bound by the sound pass.
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchDT.LaunchDT"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitLevelDT.HitLevelDT"));
	HitPawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitPawnDT.HitPawnDT"));
	HitWaterSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitWaterDT.HitWaterDT"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopDT.LoopDT"));
	SoundRadius = 160;             // AmbientSound (LoopDT)
	SoundVolume = 100;
	HitWaterSoundPitch = 1.2f;     // HitWaterDT
	HitWaterEffectName = TEXT("WaterSplash");
}

void AWoTDartProjectile::BeginPlay()
{
	Super::BeginPlay();
	BindWeaveMesh(DisplayMesh, FName(TEXT("xboxdart")), 3.0f);
	if (HasAuthority())
	{
		// Stamp the owner's current player-color/faction appearance at fire time;
		// VariantColor then rides the projectile's initial replication bunch.
		VariantColor = WoTPlayerColorFX::ResolveDart(GetInstigator());
	}
	ApplyVariantPresentation();
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		// The mesh binds after component BeginPlay; route Style immediately rather
		// than waiting for the post-spawn movement-bake timer.
		MeshLight->ForceRuntimeBake();
	}
}

void AWoTDartProjectile::OnRep_VariantColor()
{
	ApplyVariantPresentation();
}

void AWoTDartProjectile::ApplyVariantPresentation()
{
	const FString Lower = VariantColor.ToString().ToLower();
	// Green/Purple/Red/Yellow Dart.uc:23 override only LightHue; the base
	// AngrealDartProjectile.uc:133-138 supplies every other light byte.
	ConfigureDartLight(Light, TEXT("AngrealDartProjectile"),
		static_cast<uint8>(DartHue(VariantColor)));
	// Retail Skin= swaps the texture across the mesh while preserving each
	// slot's routed parent. The B3 seam does that in both development and the
	// authenticated runtime, including all five faction/player colors.
	DisplayMesh->SetAllSkinOverrides(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/Angreal/Comet_%s.Comet_%s"),
		*Lower, *Lower)));
	if (IsValid(CoronaLight))
	{
		CoronaLight->Destroy();
	}
	CoronaLight = GetWorld() ? WoTMeshFX::SpawnSpriteLocal(*GetWorld(),
		FName(*FString::Printf(TEXT("DartCorona%s"), *Lower)),
		GetActorLocation(), this) : nullptr;
}

void AWoTDartProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!GetVelocity().IsNearlyZero())
	{
		AccumRollUnits = FMath::Fmod(AccumRollUnits + SpinRate * DeltaTime, 65536.0f);
		FRotator Rot = GetVelocity().Rotation();
		Rot.Roll = AccumRollUnits * (360.0f / 65536.0f);
		SetActorRotation(Rot);
	}
}

void AWoTDartProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	if (GetWorld() && GetNetMode() != NM_DedicatedServer)
	{
		const FTransform At(HitLocation);
		if (AWoTDartExplosionEffect* Effect =
			GetWorld()->SpawnActorDeferred<AWoTDartExplosionEffect>(
				AWoTDartExplosionEffect::StaticClass(), At, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			Effect->Configure(VariantColor, DartHue(VariantColor));
			Effect->FinishSpawning(At);
		}
	}
	Super::Explode(HitLocation, HitNormal);
}

void AWoTDartProjectile::Destroyed()
{
	if (IsValid(CoronaLight)) { CoronaLight->Destroy(); }
	Super::Destroyed();
}

void AWoTDartProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDartProjectile, VariantColor);
}

AWoTDartExplosionEffect::AWoTDartExplosionEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	InitialLifeSpan = 0.4f;
}

void AWoTDartExplosionEffect::Configure(FName Color, float Hue)
{
	const FString Lower = Color.ToString().ToLower();
	TArray<FString> Frames{FString::Printf(TEXT("Expl_%s"), *Lower)};
	Sprite->SetSpriteSeries(TEXT("Angreal"), Frames, 64.0f, 64.0f);
	Sprite->DrawScale = 0.0f;
	// DartExplosion.uc:66-71; leaf Dart colors override only the hue.
	FullLightIntensity = ConfigureDartLight(
		Light, TEXT("DartExplosion"), static_cast<uint8>(Hue))
		? Light->Intensity : 0.0f;
}

void AWoTDartExplosionEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FadeTimer = FMath::Max(0.0f, FadeTimer - DeltaSeconds);
	LightFadeTimer = FMath::Max(0.0f, LightFadeTimer - DeltaSeconds);
	ScaleTimer = FMath::Min(0.25f, ScaleTimer + DeltaSeconds);
	Sprite->ScaleGlow = FadeTimer / 0.25f;
	Sprite->DrawScale = 0.5f * FMath::Sqrt(ScaleTimer / 0.25f);
	Light->SetIntensity(FullLightIntensity * (LightFadeTimer / 0.4f));
}
