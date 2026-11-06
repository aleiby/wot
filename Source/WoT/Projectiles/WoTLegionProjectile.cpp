#include "WoTLegionProjectile.h"
#include "../WoTRandom.h"

#include "Components/SphereComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Components/PointLightComponent.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTSpriteBillboard.h"

AWoTLegionProjectile::AWoTLegionProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(GetProjectilePresentationRoot());
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SpiritLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("SpiritLight"));
	SpiritLight->SetupAttachment(GetProjectilePresentationRoot());
	SpiritLight->SetCastShadows(false);
	SpiritLight->SetUseInverseSquaredFalloff(false);
	SpiritLight->SetIntensity(5.0f);
	SpiritLight->SetAttenuationRadius(150.0f);
	TailSpriteA = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("TailSpriteA"));
	TailSpriteA->SetupAttachment(GetProjectilePresentationRoot());
	TailSpriteA->DrawScale = 0.4f;
	TailSpriteA->ScaleGlow = 0.09f;
	TailSpriteB = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("TailSpriteB"));
	TailSpriteB->SetupAttachment(GetProjectilePresentationRoot());
	TailSpriteB->DrawScale = 0.65f;
	TailSpriteB->ScaleGlow = 0.09f;
	// LegionProjectile.uc:357-358.
	SetUE1CollisionSize(6.0f, 12.0f);
	// LegionProjectile.uc defaultproperties (:322-361). Only the deltas over the
	// SeekingProjectile base; homing/replication are inherited.
	Speed = 250.0f;                 // uc:340
	MaxSpeed = 450.0f;              // uc:341
	Acceleration = 20.0f;           // uc:336 (overrides the base 16)
	Damage = 25.0f;                 // uc:342
	MomentumTransfer = 200;         // uc:343
	InitialLifeSpan = 0.0f;         // uc:346 LifeSpan=0 => infinite (GetMaxRange=MaxInt)

	// LaunchOffset (170,0,-110) — the muzzle offset the launcher spawns us at (uc:338).
	LaunchOffset = FVector(170.0f, 0.0f, -110.0f);

	// uc:337 — does NOT touch the player's seeker-count HUD; uc:339 — launches straight
	// with no target. (Both override the SeekingProjectile base True.)
	bNotifiesDestination = false;
	bRequiresDestination = false;

	// uc:355 bProjTarget=True — a valid shoot-down target; uc:347 Style=STY_Translucent.
	bProjTarget = true;
	bStyleTranslucent = true;
	ScaleGlow = 2.0f;
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Spirit_Spawn1.Spirit_Spawn1"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Spirit_HitPawn1.Spirit_HitPawn1"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Spirit_Ambient1.Spirit_Ambient1"));
	SoundRadius = 160;
	SoundVolume = 100;

	// uc:769 bAlwaysRelevant is inherited from the SeekingProjectile base.
}

void AWoTLegionProjectile::BeginPlay()
{
	Super::BeginPlay();
	DisplayMesh->SetVertexMesh(TEXT("LegionSpiritHead"), TEXT("WoTPawns"));
	DisplayMesh->SetRelativeScale3D(FVector(1.0f));
	if (GetNetMode() == NM_DedicatedServer) SpiritLight->SetVisibility(false);
	else
	{
		TailSpriteA->SetSpriteSeries(TEXT("WoTPawns"), {TEXT("wet3")}, 64.0f, 64.0f);
		TailSpriteB->SetSpriteSeries(TEXT("WoTPawns"), {TEXT("wet4")}, 64.0f, 64.0f);
	}
}

void AWoTLegionProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	TailClock += DeltaSeconds;
	const float AngleA = TailClock * 125000.0f * 360.0f / 65536.0f;
	const float AngleB = TailClock * 55000.0f * 360.0f / 65536.0f;
	TailSpriteA->SetRelativeLocation(FRotator(0, AngleA, 0).Vector()
		* (10.0f * FMath::Sin(TailClock * 5.0f)));
	TailSpriteB->SetRelativeLocation(FRotator(0, AngleB, 0).Vector()
		* (5.0f * FMath::Sin(TailClock * 16.0f)));
	if (DisplayMesh)
	{
		const float DeltaUnits = SpinDirection * DeltaSeconds * CurrentSpinUnits;
		DisplayMesh->AddLocalRotation(FRotator(0,0,DeltaUnits * 360.0f / 65536.0f));
		TotalRollUnits += DeltaUnits;
		if (FMath::Abs(TotalRollUnits) >= RotationRangeUnits)
		{
			SpinDirection *= -1; TotalRollUnits = 0;
			const float SpeedRatio = ProjectileMovement ? ProjectileMovement->Velocity.Size()/250.0f : 1.0f;
			CurrentSpinUnits = 2048.0f * SpeedRatio * WoTRandom::FRandRange(0.5f,1.5f);
			RotationRangeUnits = WoTRandom::FRandRange(9216.0f,15360.0f);
		}
	}
	const bool bClose = IsValid(Destination)
		&& FVector::DistSquared(GetActorLocation(),Destination->GetActorLocation()) <= FMath::Square(256.0f);
	if (bClose && !bClosingEffect && Health > 0)
	{
		bClosingEffect=true; ClosingUntil=GetWorld()->GetTimeSeconds()+1.0f;
		AmbientGlow=255.0f;
		DisplayMesh->SetSkinOverride(1, FSoftObjectPath(
			TEXT("/Game/WoT/Textures/WoTPawns/LegionSpiritHead2.LegionSpiritHead2")));
		if(USoundBase* S=WoTSound::LoadBySubPath(TEXT("WoTPawns/Spirit_Closing1")))
			WoTSound::PlayOneShot(this,S,GetActorLocation(),1000.0f);
	}
	else if(!bClose && bClosingEffect && GetWorld()->GetTimeSeconds()>=ClosingUntil)
	{
		bClosingEffect=false;AmbientGlow=0.0f;DisplayMesh->ClearSkinOverrides();
	}
}

void AWoTLegionProjectile::Destroyed()
{
	Super::Destroyed();
}

float AWoTLegionProjectile::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                       AController* EventInstigator, AActor* DamageCauser)
{
	// VeryDead ignores ProcessDamage/TakeDamage (LegionProjectile.uc:249).
	if (bVeryDead || !HasAuthority())
	{
		return 0.0f;
	}
	const float Actual = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	// LegionProjectile.uc:216-222: Health -= Damage; VeryDead at <=0.
	Health -= FMath::RoundToInt(DamageAmount);
	if (Health > 0)
	{
		const FName Sound = WoTRandom::FRand()<0.5f ? TEXT("WoTPawns/Spirit_TakeDamage1") : TEXT("WoTPawns/Spirit_TakeDamage2");
		WoTSound::PlayReplicated(*GetWorld(), Sound, GetActorLocation(), 1000.0f);
	}
	if (Health <= 0)
	{
		GotoVeryDead();
	}
	return Actual;
}

void AWoTLegionProjectile::TargetDestroyed()
{
	// LegionProjectile.uc:313-317: target died -> put ourselves down (GotoState VeryDead)
	// so we don't hunt the respawn. Server-authoritative.
	if (HasAuthority() && !bVeryDead)
	{
		GotoVeryDead();
	}
}

void AWoTLegionProjectile::PutDownSeekersTargeting(const UWorld* World, const AActor* Victim)
{
	// Legion.Killed (uc:475-481): foreach LegionProjectile with Destination==Other ->
	// TargetDestroyed. The base-class seeker's own destination-death path turns a lost
	// seeker into a straight projectile (DestinationLost); this explicit put-down instead
	// dives+destroys it, so a killed victim's spirits die rather than fly on.
	if (!World || !IsValid(Victim))
	{
		return;
	}
	for (TActorIterator<AWoTLegionProjectile> It(World); It; ++It)
	{
		AWoTLegionProjectile* Spirit = *It;
		if (IsValid(Spirit) && Spirit->Destination == Victim)
		{
			Spirit->TargetDestroyed();
		}
	}
}

void AWoTLegionProjectile::GotoVeryDead()
{
	// state VeryDead BeginState (LegionProjectile.uc:291-308): stop seeking, dive, fade,
	// and destroy. The spin crank-up, killed sound, and Fade are presentation — the
	// load-bearing shape is: no more homing, no explosion, a downward dive that ends in a
	// short-lifespan Destroy (the fade window). Landing (NotifyHit while !bSeeking) also
	// tears us down (TouchDown/Landed, uc:251-269).
	bVeryDead = true;
	CurrentSpinUnits = 204800.0f;
	WoTSound::PlayReplicated(*GetWorld(), TEXT("WoTPawns/Spirit_TakeDamage1"),
		GetActorLocation(), 1000.0f);

	// Drop the lock (ExitSeeking re-blocks world collision so the dive lands).
	SetDestination(nullptr);

	// Velocity = vect(100,0,-10) * Normal(Velocity) (uc:307): a forward-and-down dive
	// along the current heading.
	if (ProjectileMovement)
	{
		const FVector Heading = ProjectileMovement->Velocity.GetSafeNormal();
		ProjectileMovement->Velocity = FVector(DiveForward, 0.0f, -DiveDown) * Heading;
		ProjectileMovement->ProjectileGravityScale = 1.0f;   // SetPhysics(PHYS_FALLING) (uc:302)
	}

	// class'util'.static.Fade(Self, 0.5) then Destroy — the fade window as a lifespan.
	SetLifeSpan(FMath::Max(VeryDeadFadeTime, 0.01f));
}

void AWoTLegionProjectile::ProcessTouch(AActor* Other, FVector HitLocation)
{
	// A downed spirit no longer damages anyone (VeryDead ignores; it only lands and dies).
	if (bVeryDead)
	{
		return;
	}
	// Base semantics: while seeking, only the Destination triggers the explosion
	// (SeekingProjectile.uc:609-621); otherwise the generic pawn passthrough.
	Super::ProcessTouch(Other, HitLocation);
	// LegionProjectile.uc:199-212: a live spirit that ran into something non-alive
	// (non-pawn / world) just dies — it does not explode on walls.
	if (!bVeryDead && IsValid(Other) && !Cast<APawn>(Other))
	{
		Destroy();
	}
}

void AWoTLegionProjectile::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
                                     UPrimitiveComponent* OtherComp, bool bSelfMoved,
                                     FVector HitLocation, FVector HitNormal,
                                     FVector NormalImpulse, const FHitResult& Hit)
{
	// LegionProjectile.uc HitWall (uc:187-195) / VeryDead.Landed (uc:266-269): a spirit
	// never explodes on the world — while seeking the base ignores walls; otherwise (a
	// straight/dropped or diving spirit) it is simply destroyed on contact.
	if (!bSeeking)
	{
		if (bVeryDead)
		{
			WoTSound::PlayReplicated(*GetWorld(), TEXT("WoTPawns/Spirit_Spawn1"),
				HitLocation, 1000.0f);
		}
		Destroy();
		return;
	}
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal,
	                 NormalImpulse, Hit);
}
