#include "WoTLightGlobe.h"

#include "WoTActorRotator.h"
#include "WoTAmbientSound.h"
#include "WoTMeshFX.h"
#include "WoTParticleSprayer.h"
#include "WoTSound.h"
#include "WoTWeaveFX.h"

#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

AWoTLightGlobe::AWoTLightGlobe()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	// LightGlobe.uc: bHidden (no visual of its own), bAlwaysRelevant,
	// RemoteRole SimulatedProxy; identity replicates, movement is locally
	// simulated on every machine from the followed actor.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetActorHiddenInGame(true);
}

void AWoTLightGlobe::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTLightGlobe, FollowActor);
	DOREPLIFETIME(AWoTLightGlobe, Color);
	DOREPLIFETIME(AWoTLightGlobe, LGLightRadius);
}

void AWoTLightGlobe::Follow(AActor* Other)
{
	FollowActor = Other;
	if (FollowActor)
	{
		SetActorLocation(FollowActor->GetActorLocation());
		SpawnJarChain();
	}
	else if (HasAuthority())
	{
		Destroy();
	}
}

void AWoTLightGlobe::OnRep_FollowActor()
{
	// LightGlobe.uc Tick's LastClientFollowActor watch: rebind on change.
	Follow(FollowActor);
}

void AWoTLightGlobe::SpawnJarChain()
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer || JarRotator)
	{
		return;
	}

	// PixieJarRotator.uc: the glass jar's wander orbit around the globe center.
	const FTransform Center(FRotator::ZeroRotator, GetActorLocation());
	JarRotator = World->SpawnActorDeferred<AWoTActorRotator>(
		AWoTActorRotator::StaticClass(), Center);
	if (JarRotator)
	{
		JarRotator->bUpdateActorRotation = false;
		JarRotator->MinRadius = -20.0f;
		JarRotator->MaxRadius = 20.0f;
		JarRotator->RadiusShiftRate = 5.0f;
		JarRotator->MinRotationRate = -40000.0f;
		JarRotator->MaxRotationRate = 40000.0f;
		JarRotator->RotationShiftRate = 5000.0f;
		JarRotator->MinHeight = -16.0f;
		JarRotator->MaxHeight = 16.0f;
		JarRotator->HeightShiftRate = 10.0f;
		JarRotator->PhysRotationRateUnits = FRotator(8000.0f, 6000.0f, 10000.0f);
	}

	Jar = WoTMeshFX::SpawnLocal(*World, TEXT("PixieJar"), GetActorLocation());
	if (JarRotator)
	{
		if (Jar)
		{
			JarRotator->DrivenActors.Add(TSoftObjectPtr<AActor>(Jar));
		}
		JarRotator->FinishSpawning(Center);
	}

	// Pixie.uc: the sparkle sprayer's tighter orbit around the jar.
	Pixie = World->SpawnActorDeferred<AWoTActorRotator>(
		AWoTActorRotator::StaticClass(), Center);
	if (Pixie)
	{
		Pixie->bUpdateActorRotation = false;
		Pixie->MinRadius = -8.0f;
		Pixie->MaxRadius = 8.0f;
		Pixie->RadiusShiftRate = 30.0f;
		Pixie->MinRotationRate = 20.0f;
		Pixie->MaxRotationRate = 100.0f;
		Pixie->RotationShiftRate = 60.0f;
		Pixie->MinHeight = -8.0f;
		Pixie->MaxHeight = 8.0f;
		Pixie->HeightShiftRate = 20.0f;
		Pixie->PhysRotationRateUnits = FRotator(15000.0f, 10000.0f, 20000.0f);
	}

	// PixieJar.SetColor: the five color leaves only; an unmapped color spawns
	// no sprayer, exactly the retail switch (default Blue arrives via the
	// artifact's SetColor fold before any globe exists).
	Sprayer = WoTWeaveFX::SpawnLocal(*World,
		FName(*FString::Printf(TEXT("PixieSprayer%s"), *Color.ToString())),
		GetActorLocation(), FRotator(90.0f, 0.0f, 0.0f));
	if (Sprayer)
	{
		if (LGLightRadius >= 0 && Sprayer->Light)
		{
			// Pixie.SetLightRadius: override only the radius byte (same
			// (r+1)*25 UU transform ConfigureUE1Light applied).
			Sprayer->Light->SetAttenuationRadius((LGLightRadius + 1.0f) * 25.0f);
		}
		// PixieSprayer AmbientSound=LoopLG SoundRadius=8 Volume=90 Pitch=99.
		if (USoundBase* LoopSound = WoTSound::LoadBySubPath(TEXT("Angreal/LoopLG"), true))
		{
			Loop = NewObject<UWoTAmbientSoundComponent>(Sprayer, TEXT("PixieLoop"));
			if (Loop)
			{
				Loop->SetupAttachment(Sprayer->GetRootComponent());
				Loop->RegisterComponent();
				Loop->ConfigureUE1Ambient(LoopSound, 8, 90, 99);
				Loop->Play();
			}
		}
	}
	if (Pixie)
	{
		if (Sprayer)
		{
			Pixie->DrivenActors.Add(TSoftObjectPtr<AActor>(Sprayer));
		}
		Pixie->FinishSpawning(Center);
	}
}

void AWoTLightGlobe::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (HasAuthority() && !IsValid(FollowActor))
	{
		Destroy();
		return;
	}

	if (IsValid(FollowActor))
	{
		const FVector Desired = FollowActor->GetActorLocation()
			+ FollowActor->GetActorRotation().RotateVector(FollowOffset);
		// LightGlobe.uc Tick: Loc += (Desired-Loc)/MagicNumber PER TICK at the
		// era's ~30Hz render rate — normalized frame-rate independent here
		// (PORT_NOTES divergence).
		const float Alpha = 1.0f
			- FMath::Pow(1.0f - 1.0f / FMath::Max(MagicNumber, 1.0f),
			             DeltaSeconds * 30.0f);
		SetActorLocation(GetActorLocation()
			+ (Desired - GetActorLocation()) * Alpha);
	}

	// Retail chain drag: LightGlobe.Tick re-centers PixieJarRotator on itself;
	// PixieJar.Tick re-centers the Pixie on the jar.
	if (JarRotator)
	{
		JarRotator->SetActorLocation(GetActorLocation());
	}
	if (Pixie && Jar)
	{
		Pixie->SetActorLocation(Jar->GetActorLocation());
	}
}

void AWoTLightGlobe::Destroyed()
{
	// LightGlobe.Destroyed + PixieJar.Destroyed + Pixie.Destroyed collapsed:
	// jar/rotators die outright; the sprayer retires gracefully (emitter off,
	// 2s particle linger, light and loop silenced) — the retail
	// ParticleSprayer branch of Pixie.Destroyed.
	if (Jar)
	{
		Jar->Destroy();
		Jar = nullptr;
	}
	if (JarRotator)
	{
		JarRotator->Destroy();
		JarRotator = nullptr;
	}
	if (Pixie)
	{
		Pixie->Destroy();
		Pixie = nullptr;
	}
	if (Sprayer)
	{
		if (Loop)
		{
			Loop->Stop();
			Loop = nullptr;
		}
		if (Sprayer->Light)
		{
			Sprayer->Light->SetVisibility(false);
		}
		WoTWeaveFX::FadeOut(Sprayer, 2.0f);
		Sprayer = nullptr;
	}
	Super::Destroyed();
}
