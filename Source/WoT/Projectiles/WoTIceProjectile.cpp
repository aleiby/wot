#include "WoTIceProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTWeaveFX.h"

#include "../Inventory/WoTIceLeech.h"

#include "Components/SphereComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/Pawn.h"

AWoTIceProjectile::AWoTIceProjectile()
{
	// AngrealIceProjectile defaultproperties (simulation subset).
	Duration = 7.0f;
	LeechClasses.Add(UWoTIceLeech::StaticClass());   // LeechClasses(0)

	Speed = 300.0f;
	InitialLifeSpan = 0.0f;      // LifeSpan=0
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);

	// Sound'Angreal.Ice.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchIC.LaunchIC"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitPawnIC.HitPawnIC"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopIC.LoopIC"));
	SoundRadius = 64;              // AmbientSound (LoopIC)
	SoundVolume = 128;
}

void AWoTIceProjectile::BeginPlay()
{
	Super::BeginPlay();
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealIceProjectile"));
}

void AWoTIceProjectile::ProcessDamage(AActor* Other, float /*GivenDamage*/,
                                      APawn* /*GivenInstigator*/, FVector /*HitLocation*/,
                                      FVector /*GivenMomentum*/)
{
	// Original ProcessDamage is a non-simulated function → server only.
	if (!HasAuthority())
	{
		return;
	}

	APawn* Victim = Cast<APawn>(Other);
	if (!IsValid(Victim))
	{
		return;   // Pawn(Other).Health access is Accessed-None-tolerant -> no-op
	}

	// AngrealIceProjectile.uc:75: do not freeze dead/dying victims (Health > 0).
	if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Victim))
	{
		if (!Character->IsAlive()) { return; }
	}

	// AttachLeechEffect.Invoke inlined (cf. WoTTaintProjectile): create each
	// leech on the victim, seed it from us, share Duration, and attach. Duration
	// is common to all so they expire together.
	for (const TSubclassOf<UWoTLeech>& LeechClass : LeechClasses)
	{
		if (!LeechClass)
		{
			continue;
		}
		UWoTLeech* Leech = NewObject<UWoTLeech>(Victim, LeechClass);
		if (!Leech)
		{
			continue;
		}
		Leech->InitializeWithProjectile(this);
		Leech->Lifespan = Duration;
		Leech->AttachTo(Victim);
	}

	// No Super::ProcessDamage — Freeze deals no hit-point damage.
}

void AWoTIceProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// The shipped lazy spawn: once moving, keep the three snow sprayers alive.
	if (GetVelocity().IsNearlyZero())
	{
		return;
	}
	static const FName SnowNames[3] = {FName(TEXT("SnowSprayer01")),
	                                   FName(TEXT("SnowSprayer02")),
	                                   FName(TEXT("SnowSprayer03"))};
	const FRotator VeloDir = GetVelocity().Rotation();
	SAngle += SRate * DeltaTime;
	const FVector Loc = GetActorLocation()
		+ FRotationMatrix(VeloDir).GetScaledAxis(EAxis::Y)
			* (SRadius * FMath::Sin(SAngle));
	for (int32 i = 0; i < 3; ++i)
	{
		if (!IsValid(Snow[i]))
		{
			Snow[i] = WoTWeaveFX::SpawnLocal(*GetWorld(), SnowNames[i],
			                                 Loc, VeloDir);
		}
		else
		{
			Snow[i]->SetActorLocation(Loc);
			Snow[i]->SetActorRotation(VeloDir);
		}
	}
}

void AWoTIceProjectile::Destroyed()
{
	// Ice.Destroyed: bOn=false + LifeSpan 5.
	for (TObjectPtr<AWoTParticleSprayer>& S : Snow)
	{
		WoTWeaveFX::FadeOut(S, 5.0f);
	}
	Super::Destroyed();
}
