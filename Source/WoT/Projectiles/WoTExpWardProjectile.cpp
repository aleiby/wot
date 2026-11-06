#include "WoTExpWardProjectile.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSound.h"
#include "Components/PointLightComponent.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "../WoTMover.h"
#include "../WoTMeshLight.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AWoTExpWardProjectile::AWoTExpWardProjectile()
{
	ExplosionEffectName = FName(TEXT("FireballExplode3"));   // ExpWard.uc:145
	PrimaryActorTick.bCanEverTick = true;
	// AngrealExpWardProjectile.uc:264,281-285 display defaults.
	ScaleGlow = 1.0f;
	ScaleGlowRate = 1.0f;
	bUnlit = true;
	bStyleTranslucent = true;
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// Explode scorch (AngrealExpWardProjectile.uc:137-141: ScorchType
	// ScorchMark at DrawScale = ScorchSize 2.5, :268-269).
	ImpactDecalType = FName(TEXT("ScorchMark"));
	ImpactDecalDrawScale = 2.5f;
	// AngrealExpWardProjectile defaultproperties (simulation subset).
	Damage = 50.0f;
	DamageRadius = 120.0f;              // splash on detonation
	MomentumTransfer = 6000;
	DamageType = "xEFxx";              // Earth + Fire
	bHurtsOwner = true;               // the ward can catch its placer
	TraceDist = 200.0f;
	ActivationTime = 1.0f;
	InitialLifeSpan = 60.0f;          // MP auto-detonate; standalone -> permanent

	// N2 divergence from the raw bNetTemporary default (PORT_NOTES §8): a stationary,
	// long-lived, ≤16-on-the-field ward rides a PERSISTENT channel so a server-only
	// early detonation (proximity/shot/Trigger before its 60s) propagates its Destroy
	// to clients instead of leaving a ≤60s lingering ghost. It then inherits the base
	// AWoTGenericProjectile::Destroyed() client explode-on-destroy cosmetics.
	bNetTemporary = false;

	// AngrealExpWardProjectile.uc: CollisionRadius=24, CollisionHeight=8.
	SetUE1CollisionSize(24.0f, 8.0f);
	// bProjTarget=True (uc:290): shootable — a dart hit or splash detonates the
	// ward. The Waiting state's TakeDamage/Touch ignore (uc:181) maps to disabled
	// collision until armed + the bArmed gates below.
	bProjTarget = true;

	// Stationary (UE1 PHYS_None): no travel, no gravity.
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->InitialSpeed = 0.0f;
	ProjectileMovement->MaxSpeed = 0.0f;

	// Sound'Angreal.LandMine.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/PlaceLM.PlaceLM"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ExplodeLM.ExplodeLM"));
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateLM.ActivateLM"));
}

void AWoTExpWardProjectile::BeginPlay()
{
	Super::BeginPlay();
	FullLightIntensity = WoTUE1LightRegistry::Configure(
		Light, TEXT("AngrealExpWardProjectile")) ? Light->Intensity : 0.0f;
	BindWeaveMesh(DisplayMesh, FName(TEXT("ExpWard")), 0.3f);
	ApplyVisualPresentation();

	if (HasAuthority())
	{
		// Trace forward to the nearest surface and stick to it (PreBeginPlay).
		// Retail traces ALL world geometry, not just static BSP
		// (AngrealExpWardProjectile.uc:68), so include movable world actors.
		const FVector Start = GetActorLocation();
		const FVector End = Start + GetActorForwardVector() * TraceDist;
		FHitResult Hit;
		// AngrealExpWardProjectile.uc:68 Trace(..., false): only level/movers
		// place the ward. The accepted hit supplies retail location and normal.
		if (WoTVisBlockingTrace(GetWorld(), Start, End, this, Hit))
		{
			RelativeUp = Hit.ImpactNormal;
			AlignPresentationToVelocity(RelativeUp);
			bCorrectlyPlaced = SetActorLocation(Hit.ImpactPoint + RelativeUp * 0.5f);
			// "Stick to movers": SetBase(HitActor) (uc:83-85) so the ward rides
			// a moving platform/door instead of hanging in the air.
			if (AWoTMover* M = Cast<AWoTMover>(Hit.GetActor()))
			{
				AttachToActor(M, FAttachmentTransformRules::KeepWorldTransform);
			}
			// Retail SetLocation -> SetActorZone -> ZoneChange (UE1's FarMoveActor
			// and SetActorZone): a ward placed on a surface inside a water zone hits water
			// and is destroyed (AngrealExpWardProjectile.uc:165-169) during spawn,
			// so SpawnActor returns None and the cast fails without using a charge
			// (LaunchProjectileEffect.uc:133-141, ProjectileLauncher.uc:216-226).
			// No sound or fizzle: the ward authors no HitWater class or sound.
			UpdateWaterZone();
			if (!IsValid(this) || IsActorBeingDestroyed())
			{
				return;
			}
		}
	}

	// Inert until armed (UE1 SetCollision(False) in PreBeginPlay + Waiting state).
	SetActorEnableCollision(false);

	// Explosive wards are permanent in SP; timed in MP (LifeSpan default 60).
	if (GetNetMode() == NM_Standalone)
	{
		SetLifeSpan(0.0f);
	}

	// Register our Tag so a matching Trigger/Dispatcher can detonate us (retail
	// Trigger). Player-thrown wards have no Tag, so this is a no-op for them.
	if (HasAuthority() && !TagName.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->RegisterTag(TagName, this);
		}
	}

	// Waiting: arm after ActivationTime (Sleep(ActivationTime) -> active).
	GetWorldTimerManager().SetTimer(ArmTimer, this,
		&AWoTExpWardProjectile::Activate, ActivationTime, false);
}

void AWoTExpWardProjectile::WoTTrigger(AActor* /*Other*/, APawn* /*EventInstigator*/)
{
	// AngrealExpWardProjectile.uc:125 (singular Trigger): detonate in place.
	if (!HasAuthority())
	{
		return;
	}
	Explode(GetActorLocation(), RelativeUp);
}

bool AWoTExpWardProjectile::CreationSucceeded()
{
	return bCorrectlyPlaced;
}

void AWoTExpWardProjectile::Activate()
{
	if (bArmed && VisualPhase != EWoTExpWardVisualPhase::Waiting)
	{
		return;
	}
	bArmed = true;
	ArmingOccupants.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Other = *It;
		if (Other != this && IsTouchTarget(Other)
			&& WoTUE1Collision::CylindersOverlap(GetActorLocation(),
				UE1CollisionRadius, UE1CollisionHeight, Other))
		{
			// Touch is generated by relative movement. Remember the separation,
			// not only the occupant's world position, so a mover carrying the ward
			// into a stationary pawn triggers exactly like retail.
			ArmingOccupants.Add(Other,
				Other->GetActorLocation() - GetActorLocation());
		}
	}
	SetActorEnableCollision(true);   // proximity triggering goes live
	SetVisualPhase(EWoTExpWardVisualPhase::FadeOut);
	PlayActivateSoundOnce();
}

void AWoTExpWardProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bArmed && !ArmingOccupants.IsEmpty())
	{
		TArray<TWeakObjectPtr<AActor>> Remove;
		for (const TPair<TWeakObjectPtr<AActor>, FVector>& Entry : ArmingOccupants)
		{
			AActor* Other = Entry.Key.Get();
			if (!IsValid(Other) || !WoTUE1Collision::CylindersOverlap(
				GetActorLocation(), UE1CollisionRadius, UE1CollisionHeight, Other))
			{
				Remove.Add(Entry.Key);
			}
			else if (!(Other->GetActorLocation() - GetActorLocation()).Equals(
				Entry.Value, KINDA_SMALL_NUMBER))
			{
				Remove.Add(Entry.Key);
				Super::ProcessTouch(Other, Other->GetActorLocation());
				if (!IsValid(this) || IsActorBeingDestroyed()) { return; }
			}
		}
		for (const TWeakObjectPtr<AActor>& Other : Remove) { ArmingOccupants.Remove(Other); }
	}
	if (!bArmed || VisualPhase == EWoTExpWardVisualPhase::Waiting)
	{
		return;
	}

	if (VisualPhase == EWoTExpWardVisualPhase::FadeOut)
	{
		ScaleGlow = FMath::Max(0.0f, ScaleGlow - ScaleGlowRate * DeltaSeconds);
		if (ScaleGlow <= 0.0f)
		{
			SetVisualPhase(EWoTExpWardVisualPhase::FadeIn);
		}
	}
	else
	{
		ScaleGlow = FMath::Min(1.0f, ScaleGlow + ScaleGlowRate * DeltaSeconds);
		if (ScaleGlow >= 1.0f)
		{
			SetVisualPhase(EWoTExpWardVisualPhase::FadeOut);
		}
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::SetVisualPhase(EWoTExpWardVisualPhase NewPhase)
{
	if (VisualPhase == NewPhase)
	{
		return;
	}
	VisualPhase = NewPhase;
	if (HasAuthority())
	{
		ForceNetUpdate();
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::OnRep_VisualPhase()
{
	if (VisualPhase != EWoTExpWardVisualPhase::Waiting)
	{
		bArmed = true;
		PlayActivateSoundOnce();
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::PlayActivateSoundOnce()
{
	if (bActivateSoundPlayed || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	bActivateSoundPlayed = true;
	// AngrealExpWardProjectile.uc:185: PlaySound in the simulated Waiting state
	// code — each machine's own play, volume 1, unset radius 1600.
	if (USoundBase* Sound = WoTSound::Resolve(ActivateSound))
	{
		WoTSound::PlayOneShot(this, Sound, GetActorLocation(),
			WoTSound::DefaultPlaySoundRadius);
	}
}

void AWoTExpWardProjectile::ApplyVisualPresentation()
{
	// At the FadeOut trough the translucent source is exactly black; hiding the
	// actor avoids leaving an opaque/depth-writing shell for that armed instant.
	// FadeIn unhides it on the next positive ScaleGlow step.
	SetActorHiddenInGame(bArmed && ScaleGlow <= KINDA_SMALL_NUMBER);
	if (IsValid(Light))
	{
		const float Glow = VisualPhase == EWoTExpWardVisualPhase::Waiting
			? 0.0f : FMath::Clamp(ScaleGlow, 0.0f, 1.0f);
		Light->SetIntensity(FullLightIntensity * Glow);
	}
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		// bUnlit makes this a cheap flat-color refresh: MLAmbient is
		// clamp(ScaleGlow*0.5), exactly the translucent-master source multiplier.
		MeshLight->ForceRuntimeBake();
	}
}

void AWoTExpWardProjectile::ProcessTouch(AActor* Other, FVector HitLocation)
{
	// Waiting state ignores Touch; only a live ward detonates on proximity.
	if (!bArmed)
	{
		return;
	}
	if (!WoTUE1Collision::CylindersOverlap(GetActorLocation(),
		UE1CollisionRadius, UE1CollisionHeight, Other))
	{
		return;
	}
	if (ArmingOccupants.Contains(Other))
	{
		return;
	}
	Super::ProcessTouch(Other, HitLocation);
}

void AWoTExpWardProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	// One detonation per ward: retail's TakeDamage/Trigger are `singular`
	// (uc:119,125), which is what stops two armed wards in mutual splash radius
	// from re-detonating each other in unbounded recursion (A -> B -> A -> ...).
	if (bDetonated)
	{
		return;
	}
	bDetonated = true;
	if (GetWorld())
	{
		// AngrealExpWardProjectile.uc:151: the placed ward's BlastBits follows
		// its stored surface RelativeUp, rather than the detonation caller's normal.
		WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("BlastBits"), HitLocation,
			RelativeUp.Rotation());
	}
	// FireballExplode3 + BlastBits bursts; the scorch
	// decal rides the base Explode's ImpactDecalType.
	// TriggerOthers (AngrealExpWardProjectile.uc:154): fire our Event so any actor
	// tagged with it (e.g. a chained ward) is triggered. Empty for player-thrown wards.
	if (HasAuthority() && !EventName.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(EventName, this, nullptr);
		}
	}
	Super::Explode(HitLocation, HitNormal);   // splash (DamageRadius) + destroy
}

void AWoTExpWardProjectile::LifeSpanExpired()
{
	// Expired() -> Explode() (MP auto-detonate) instead of a silent destroy.
	Explode(GetActorLocation(), RelativeUp);
}

float AWoTExpWardProjectile::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                        AController* EventInstigator, AActor* DamageCauser)
{
	const float Actual = Super::TakeDamage(DamageAmount, DamageEvent,
		EventInstigator, DamageCauser);
	// A live ward detonates when shot (Waiting state ignores TakeDamage).
	if (bArmed)
	{
		Explode(GetActorLocation(), RelativeUp);
	}
	return Actual;
}

void AWoTExpWardProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Original: reliable if Role==Authority RelativeUp.
	DOREPLIFETIME(AWoTExpWardProjectile, RelativeUp);
	DOREPLIFETIME(AWoTExpWardProjectile, bArmed);
	DOREPLIFETIME(AWoTExpWardProjectile, VisualPhase);
}
