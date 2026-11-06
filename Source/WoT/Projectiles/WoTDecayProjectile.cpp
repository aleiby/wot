#include "WoTDecayProjectile.h"
#include "../WoTMeshFX.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTDecal.h"
#include "Components/PointLightComponent.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"

#include "../Inventory/WoTDecreaseChargesLeech.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"

namespace
{
int32 DecayCosmeticSeed(const UObject* Object)
{
	const uint64 Entropy = FPlatformTime::Cycles64()
		^ static_cast<uint64>(reinterpret_cast<UPTRINT>(Object));
	return static_cast<int32>(Entropy ^ (Entropy >> 32));
}
}

AWoTDecayProjectile::AWoTDecayProjectile()
{
	ExplosionEffectName = FName(TEXT("DecayExplosion"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Sprite->DrawScale = 0.8f;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// AngrealDecayProjectile defaultproperties (simulation subset).
	Duration = 15.0f;
	LeechClasses.Add(UWoTDecreaseCommonChargesLeech::StaticClass());   // (0)
	LeechClasses.Add(UWoTDecreaseUncommonChargesLeech::StaticClass()); // (1)
	LeechClasses.Add(UWoTDecreaseRareChargesLeech::StaticClass());     // (2)
	LeechClasses.Add(UWoTDecreaseHealthLeech::StaticClass());          // (3) HP drain
	// (The original sets the health leech's AffectResolution per target —
	// NPCDamageRate 0.1 / PlayerDamageRate 0.2; its default 0.2 = the player rate.
	// The NPC rate lands with the pawn-AI port.)

	Speed = 75.0f;
	MaxSpeed = 800.0f;
	InitialLifeSpan = 0.0f;         // LifeSpan=0
	Collision->InitSphereRadius(6.0f);   // CollisionRadius=6

	// Sound'Angreal.Decay.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LaunchDC.LaunchDC"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/HitDC.HitDC"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/LoopDC.LoopDC"));
	SoundRadius = 160;             // AmbientSound (LoopDC)
	SoundVolume = 255;
}

void AWoTDecayProjectile::ProcessDamage(AActor* Other, float /*GivenDamage*/,
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
		return;
	}

	// AttachLeechEffect.Invoke inlined (cf. Ice/Taint): create each leech on the
	// victim, seed it from us, share Duration, and attach.
	for (const TSubclassOf<UWoTLeech>& LeechClass : LeechClasses)
	{
		if (!LeechClass)
		{
			continue;
		}
		UWoTLeech* Leech = NewObject<UWoTLeech>(Victim, LeechClass);
		Leech->InitializeWithProjectile(this);
		Leech->Lifespan = Duration;
		Leech->AttachTo(Victim);
	}

	// No Super::ProcessDamage — Decay's damage is the (deferred) health leech's,
	// not a direct impact hit.
}

void AWoTDecayProjectile::BeginPlay()
{
	Super::BeginPlay();
	WoTUE1LightRegistry::Configure(Light, TEXT("AngrealDecayProjectile"));
	if (GetNetMode() != NM_DedicatedServer)
	{
		CosmeticRandom.Initialize(DecayCosmeticSeed(this));
		// AngrealDecayProjectile.uc:83-90: the first drip uses the same 1-2
		// second random delay as every subsequent drip.
		NextDripTime = CosmeticRandom.FRandRange(1.0f, 2.0f);
		WoTMeshFX::ConfigureClassSprite(*Sprite,
			TEXT("AngrealDecayProjectile"), 30.0f);
	}
}

void AWoTDecayProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// The drip trail (Decay Tick): every 1-2s, drop a DecaySplatter at Location.
	if (GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	NextDripTime -= DeltaTime;
	if (NextDripTime <= 0.0f)
	{
		NextDripTime = CosmeticRandom.FRandRange(1.0f, 2.0f); // Min/MaxDripTime
		WoTDecalFX::SpawnFlying(*GetWorld(), TEXT("DecaySplatter"), GetActorLocation(),
		                        FVector::ZeroVector, &CosmeticRandom);
		LastDripConfig = TEXT("DecaySplatter");
	}
}

void AWoTDecayProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	AActor* Victim = IsValid(Destination.Get()) ? Destination.Get() : HitActor.Get();
	if (GetWorld())
	{
		WoTWeaveFX::SpawnExplosion(*GetWorld(), TEXT("DecayExplosion"),
			Victim ? Victim->GetActorLocation() : HitLocation, Victim);
	}
	const FName Saved = ExplosionEffectName;
	ExplosionEffectName = NAME_None;
	Super::Explode(HitLocation, HitNormal);
	ExplosionEffectName = Saved;
}
