#include "WoTWeaponProjectile.h"
#include "../WoTRandom.h"

#include "../WoTSound.h"
#include "../WoTDecal.h"
#include "../WoTParticleSprayer.h"
#include "../WoTWeaveFX.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTMover.h"
#include "../WoTVertexAnimComponent.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "Components/SphereComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

namespace
{
	void PlayWeaponProjectileSound(AWoTWeaponProjectile* Projectile,
		const TSoftObjectPtr<USoundBase>& Sound, const FVector& Location)
	{
		// Asset policy: config keeps soft refs and gameplay never forces a sync load.
		// A cooked/streamed sound plays when resident; a missing asset is Accessed-
		// None-tolerant, matching the shipped guards.
		if (Projectile)
		{
			WoTSound::PlayOneShot(Projectile,
				WoTSound::ResolveResidentOrRuntime(Sound), Location,
				WoTSound::WorldSoundRadius(Projectile->SoundRadius), Projectile->SoundPitch / 64.0f);
		}
	}
}

AWoTWeaponProjectile::AWoTWeaponProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	// Engine.Projectile is temporary: every machine simulates this initial-bunch
	// copy and resolves its own presentation while authority alone changes play.
	bNetTemporary = true;
	Speed = 5.0f;                 // WotWeaponProjectile.uc:266
	MaxSpeed = 5.0f;
	Damage = 10.0f;               // uc:268
	bAlwaysRelevant = true;       // uc:271
	SoundVolume = 255;            // uc:273
	InitialLifeSpan = 140.0f;     // Engine.Projectile CDO

	ProjectileMovement->bRotationFollowsVelocity = false;
	Collision->OnComponentBeginOverlap.AddDynamic(this,
		&AWoTWeaponProjectile::OnWeaponProjectileOverlap);
	Collision->OnComponentHit.AddDynamic(this, &AWoTWeaponProjectile::OnWeaponProjectileHit);
	Collision->SetNotifyRigidBodyCollision(true);
	CylinderCollision->OnComponentBeginOverlap.AddDynamic(this,
		&AWoTWeaponProjectile::OnWeaponProjectileOverlap);
	CylinderCollision->OnComponentHit.AddDynamic(this,
		&AWoTWeaponProjectile::OnWeaponProjectileHit);
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(Collision);
	DisplayMesh->SetCollisionProfileName(TEXT("NoCollision"));
}

void AWoTWeaponProjectile::BeginPlay()
{
	Super::BeginPlay();

	// Flying.BeginState (WotWeaponProjectile.uc:204-216): establish velocity from
	// the aimed rotation before randomizing only the axes that will spin.
	if (HasAuthority() && ProjectileMovement)
	{
		ProjectileMovement->Velocity = GetActorRotation().Vector() * Speed;
		ProjectileMovement->InitialSpeed = Speed;
		ProjectileMovement->MaxSpeed = MaxSpeed;
	}
	else if (bStuck)
	{
		bResolved = true;
		StopAmbientHum();
		if (ProjectileMovement)
		{
			ProjectileMovement->StopMovementImmediately();
			ProjectileMovement->SetActive(false);
		}
		SetActorEnableCollision(false);
	}
	else if (bNetTemporary)
	{
		ApplyReplicatedInitialVelocity();
	}
	FRotator Rotation = GetActorRotation();
	if (YawSpinRate != 0)   { Rotation.Yaw = WoTRandom::FRandRange(0.0f, 360.0f); }
	if (RollSpinRate != 0)  { Rotation.Roll = WoTRandom::FRandRange(0.0f, 360.0f); }
	if (PitchSpinRate != 0) { Rotation.Pitch = WoTRandom::FRandRange(0.0f, 360.0f); }
	SetActorRotation(Rotation);
#if WOT_GUARDED_BOOTSTRAP
	if (DisplayMesh->SetVertexMeshPath(ProjectileMeshAsset.ToSoftObjectPath()))
	{
		ApplyProjectileSkin();
	}
#else
	if (ProjectileMeshAsset.IsValid())
	{
		DisplayMesh->SetSkeletalMeshAsset(ProjectileMeshAsset.Get());
		ApplyProjectileSkin();
	}
	else if (!ProjectileMeshAsset.IsNull())
	{
		const TWeakObjectPtr<AWoTWeaponProjectile> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			ProjectileMeshAsset.ToSoftObjectPath(), FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTWeaponProjectile* Projectile = WeakThis.Get(); Projectile &&
					Projectile->DisplayMesh && Projectile->ProjectileMeshAsset.IsValid())
				{
					Projectile->DisplayMesh->SetSkeletalMeshAsset(Projectile->ProjectileMeshAsset.Get());
					Projectile->ApplyProjectileSkin();
				}
			}));
	}
#endif

	// Preserve the pre-extraction Archer behavior: the launch cue is needed now,
	// so synchronously resolve it at spawn (WotWeaponProjectile.uc:69).
	if (!bStuck)
	{
		if (USoundBase* LoadedSpawnSound = WoTSound::Resolve(SpawnSound))
		{
			WoTSound::PlayOneShot(this, LoadedSpawnSound, GetActorLocation(),
				WoTSound::WorldSoundRadius(SoundRadius), SoundPitch / 64.0f);
		}
		// Impact is later in the flight, so begin streaming both possible hit cues now.
		TArray<FSoftObjectPath> ImpactSounds;
		if (!SoundHitPawn.IsNull()) { ImpactSounds.Add(SoundHitPawn.ToSoftObjectPath()); }
		if (!SoundHitWall.IsNull()) { ImpactSounds.Add(SoundHitWall.ToSoftObjectPath()); }
		if (ImpactSounds.Num() > 0)
		{
			UAssetManager::GetStreamableManager().RequestAsyncLoad(ImpactSounds);
		}
	}
}

void AWoTWeaponProjectile::ApplyReplicatedInitialVelocity()
{
	if (!ProjectileMovement) return;
	const FVector Velocity = GetReplicatedMovement().LinearVelocity;
	ProjectileMovement->Velocity = Velocity;
	ProjectileMovement->InitialSpeed = Velocity.Size();
	ProjectileMovement->MaxSpeed = MaxSpeed;
}

void AWoTWeaponProjectile::ApplyProjectileSkin()
{
	if (!DisplayMesh || SkinOverride.IsNull())
	{
		return;
	}
#if !WOT_GUARDED_BOOTSTRAP
	if (!SkinOverride.IsValid())
	{
		const TWeakObjectPtr<AWoTWeaponProjectile> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			SkinOverride.ToSoftObjectPath(), FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTWeaponProjectile* Projectile = WeakThis.Get())
				{
					Projectile->ApplyProjectileSkin();
				}
			}));
		return;
	}
#endif
	DisplayMesh->SetAllSkinOverrides(SkinOverride.ToSoftObjectPath());
}

void AWoTWeaponProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bMotionBlur && !bResolved && !bStuck)
	{
		UpdateMotionBlur();
	}
	else if (!MotionBlurGhosts.IsEmpty())
	{
		ClearMotionBlur();
	}
	if (bStuck)
	{
		StuckElapsed += DeltaSeconds;
		if (GetNetMode() != NM_Standalone)
		{
			ScaleGlow = FadeStartGlow;
			if (HasAuthority()
				&& StuckElapsed > StayStuckTime + FadeAwayTime)
			{
				Destroy();
			}
			return;
		}
		if (StuckElapsed > StayStuckTime)
		{
			const float FadeAlpha = FMath::Clamp(
				(StuckElapsed - StayStuckTime) / FMath::Max(FadeAwayTime, KINDA_SMALL_NUMBER),
				0.0f, 1.0f);
			ScaleGlow = FMath::Lerp(FadeStartGlow, FadeEndGlow, FadeAlpha); // Util.Fade, uc:237
			if (FadeAlpha >= 1.0f)
			{
				Destroy();
			}
		}
		return;
	}
	if (bResolved)
	{
		return;
	}

	// UE1 rotator rates use 65536 units/revolution (uc:96-127).
	FRotator Rotation = GetActorRotation();
	constexpr float UE1ToDegrees = 360.0f / 65536.0f;
	Rotation.Yaw += YawSpinRate * DeltaSeconds * UE1ToDegrees;
	Rotation.Roll += RollSpinRate * DeltaSeconds * UE1ToDegrees;
	Rotation.Pitch += PitchSpinRate * DeltaSeconds * UE1ToDegrees;
	SetActorRotation(Rotation);

	if (PreviousOther.IsValid() && GetWorld() && GetWorld()->GetTimeSeconds() >= ClearOtherTime)
	{
		PreviousOther.Reset();
	}
}

void AWoTWeaponProjectile::UpdateMotionBlur()
{
	if (GetNetMode() == NM_DedicatedServer || !DisplayMesh)
	{
		return;
	}
	// Native MotionBlurRI's ten nodes include the current display copy, leaving
	// at most nine historical copies behind it.
	constexpr int32 MaxGhosts = 9;
	const int32 VisibleItems = FMath::Min(MotionBlurHistory.Num(), MaxGhosts);
	while (MotionBlurGhosts.Num() < VisibleItems)
	{
		const int32 Index = MotionBlurGhosts.Num();
		UWoTVertexAnimComponent* Ghost = NewObject<UWoTVertexAnimComponent>(this,
			*FString::Printf(TEXT("MotionBlurGhost%d"), Index));
		if (!Ghost)
		{
			break;
		}
		Ghost->SetupAttachment(GetRootComponent());
		// Actor.Style participates in material selection, so establish it before
		// CopyDisplayMeshFrom binds the runtime mesh/materials.
		Ghost->Style = 3;
		Ghost->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ghost->SetCastShadow(false);
		Ghost->SetUsingAbsoluteLocation(true);
		Ghost->SetUsingAbsoluteRotation(true);
		Ghost->SetUsingAbsoluteScale(true);
		Ghost->RegisterComponent();
		Ghost->CopyDisplayMeshFrom(DisplayMesh);
		MotionBlurGhosts.Add(Ghost);
	}

	UMaterialInterface* FallbackMaterial = nullptr;
	for (int32 Index = 0; Index < MotionBlurGhosts.Num(); ++Index)
	{
		UWoTVertexAnimComponent* Ghost = MotionBlurGhosts[Index];
		if (!Ghost) continue;
		if (DisplayMesh->HasDisplayMesh()
			&& Ghost->GetDisplayMeshName() != DisplayMesh->GetDisplayMeshName())
		{
			Ghost->CopyDisplayMeshFrom(DisplayMesh);
		}
		Ghost->SetWorldTransform(MotionBlurHistory[Index]);
		Ghost->SetVisibility(true);
		const int32 TotalNodeCount = VisibleItems + 1;
		const float Glow = ScaleGlow * (1.0f
			- static_cast<float>(Index + 1) / static_cast<float>(TotalNodeCount));
		const int32 SlotCount = FMath::Max(1, Ghost->GetNumMaterials());
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			UMaterialInstanceDynamic* MID =
				Cast<UMaterialInstanceDynamic>(Ghost->GetMaterial(Slot));
			if (!MID)
			{
				UMaterialInterface* Parent = Ghost->GetMaterial(Slot);
				if (!Parent)
				{
					if (!FallbackMaterial)
					{
						FallbackMaterial = UWoTRuntimeAssets::ResolveMaterialPath(
							FSoftObjectPath(TEXT("/Game/WoT/Materials/M_WoT_MeshLitT."
								"M_WoT_MeshLitT")));
					}
					Parent = FallbackMaterial;
				}
				if (Parent)
				{
					MID = UMaterialInstanceDynamic::Create(Parent, Ghost);
					Ghost->SetMaterial(Slot, MID);
				}
			}
			if (MID)
			{
				MID->SetScalarParameterValue(TEXT("MLScaleGlow"), Glow);
				MID->SetScalarParameterValue(TEXT("Glow"), Glow);
				MID->SetVectorParameterValue(TEXT("MLAmbient"),
					FLinearColor(Glow, Glow, Glow, 1.0f));
			}
		}
	}

	MotionBlurHistory.Insert(GetActorTransform(), 0);
	if (MotionBlurHistory.Num() > MaxGhosts)
	{
		MotionBlurHistory.SetNum(MaxGhosts, EAllowShrinking::No);
	}
}

void AWoTWeaponProjectile::ClearMotionBlur()
{
	for (UWoTVertexAnimComponent* Ghost : MotionBlurGhosts)
	{
		if (Ghost)
		{
			Ghost->DestroyComponent();
		}
	}
	MotionBlurGhosts.Empty();
	MotionBlurHistory.Empty();
}

void AWoTWeaponProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTWeaponProjectile, SkinOverride);
	DOREPLIFETIME(AWoTWeaponProjectile, bStyleMasked);
	DOREPLIFETIME_CONDITION(AWoTWeaponProjectile, bStuck, COND_InitialOnly);
}

void AWoTWeaponProjectile::OnWeaponProjectileOverlap(UPrimitiveComponent*, AActor* Other,
	UPrimitiveComponent*, int32, bool, const FHitResult& Hit)
{
	if (bResolved || !Other || Other == this || Other == GetOwner() ||
		Other == GetInstigator() || Other->IsA<AWoTProjectile>())
	{
		return;
	}
	ProcessTouch(Other, Hit.ImpactPoint.IsZero() ? GetActorLocation() : FVector(Hit.ImpactPoint));
}

void AWoTWeaponProjectile::OnWeaponProjectileHit(UPrimitiveComponent*, AActor* Other,
	UPrimitiveComponent*, FVector, const FHitResult& Hit)
{
	if (bResolved
		|| (Other && (Other == GetOwner() || Other == GetInstigator())))
	{
		return;
	}
	// A blocking pawn/actor is still ProcessTouch, not Flying.HitWall (uc:168).
	if (Other && !Other->IsA<AWoTProjectile>() && !Other->IsA<AWoTMover>() &&
		!Other->IsRootComponentStatic())
	{
		ProcessTouch(Other, Hit.ImpactPoint.IsZero() ? GetActorLocation() : FVector(Hit.ImpactPoint));
		return;
	}
	if (AWoTMover* Mover = Cast<AWoTMover>(Other);
		HasAuthority() && Mover && Mover->bDamageTriggered)
	{
		FWoTDamageEvent Event;
		Event.UE1Momentum = MomentumTransfer * GetVelocity().GetSafeNormal();
		Event.HitInfo.ImpactPoint = GetActorLocation();
		AController* EventInstigator = GetInstigator() ? GetInstigator()->GetController() : nullptr;
		Mover->TakeDamage(Damage, Event, EventInstigator, this); // uc:143-147
	}
	const FVector HitLocation = Hit.ImpactPoint.IsZero()
		? GetActorLocation() : FVector(Hit.ImpactPoint);
	const FVector HitNormal = Hit.ImpactNormal.IsNearlyZero()
		? -GetVelocity().GetSafeNormal() : FVector(Hit.ImpactNormal);
	PlayWallCosmetics(HitLocation, HitNormal);
	if (bStickInWalls)
	{
		StickInSomething();
	}
	else
	{
		Explode(HitLocation, HitNormal);
	}
}

void AWoTWeaponProjectile::ProcessTouch(AActor* Other, const FVector& HitLocation)
{
	// Flying.ProcessTouch (uc:170): the entire touch body — damage AND the
	// explode — is gated on Projectile(Other) == None. A weapon projectile
	// never damages or detonates against another projectile.
	if (!Other || Other->IsA<AWoTProjectile>())
	{
		return;
	}
	const bool bDuplicate = PreviousOther.Get() == Other && GetWorld() &&
		GetWorld()->GetTimeSeconds() < ClearOtherTime;
	if (HasAuthority() && !bDuplicate)
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = DamageTypeName;
		Event.UE1Momentum = FVector::ZeroVector;
		Event.HitInfo.ImpactPoint = HitLocation;
		AController* EventInstigator = GetInstigator() ? GetInstigator()->GetController() : nullptr;
		Other->TakeDamage(Damage, Event, EventInstigator, this); // uc:178: plain 'shot'
		PreviousOther = Other;
		ClearOtherTime = GetWorld() ? GetWorld()->GetTimeSeconds() + ClearOtherDelaySecs : 0.0f;
	}

	const FVector HitNormal = (HitLocation - Other->GetActorLocation()).GetSafeNormal();
	PlayTouchCosmetics(Other, HitLocation, HitNormal);
	if (!bPassThroughActors)
	{
		Explode(HitLocation, HitNormal);
	}
}

void AWoTWeaponProjectile::Explode(const FVector&, const FVector&)
{
	ClearMotionBlur();
	bResolved = true;
	Destroy(); // Explosion.BeginState (WotWeaponProjectile.uc:198,256)
}

void AWoTWeaponProjectile::PlayExplodeCosmetics(const FVector&, const FVector&)
{
}

void AWoTWeaponProjectile::PlayWallImpactCosmetics(const FVector&, const FVector&)
{
}

void AWoTWeaponProjectile::PlayTouchCosmetics(AActor* Other,
	const FVector& HitLocation, const FVector& HitNormal)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	PlayWeaponProjectileSound(this, Other && Other->IsA<APawn>()
		? SoundHitPawn : SoundHitWall, HitLocation);
	if (!bPassThroughActors)
	{
		StopAmbientHum();
		PlayExplodeCosmetics(HitLocation, HitNormal);
	}
}

void AWoTWeaponProjectile::PlayWallCosmetics(const FVector& HitLocation,
	const FVector& HitNormal)

{
	if (GetNetMode() == NM_DedicatedServer) return;
	PlayWeaponProjectileSound(this, SoundHitWall, HitLocation);
	StopAmbientHum();
	PlayWallImpactCosmetics(HitLocation, HitNormal);
	if (!bStickInWalls)
	{
		PlayExplodeCosmetics(HitLocation, HitNormal);
	}
}

void AWoTWeaponProjectile::StickInSomething()
{
	ClearMotionBlur();
	bResolved = true;
	bStuck = true;
	StuckElapsed = 0.0f;
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->SetActive(false);
	}
	SetActorEnableCollision(false);
	if (FadeAwayTime <= KINDA_SMALL_NUMBER)
	{
		Destroy(); // StuckInSomething.BeginState (uc:230-234)
	}
	else
	{
		// Tick reproduces Util.Fade's StayStuckTime delay and ScaleGlow ramp.
	}
}

AWoTMyrddraalBolt::AWoTMyrddraalBolt()
{
	// MyrddraalBolt.uc inherits Engine.Projectile's point-sized cylinder.
	SetUE1CollisionSize(0.0f, 0.0f);
	Speed = MaxSpeed = 2000.0f;
	RollSpinRate = 200000;
	StayStuckTime = 30.0f;
	FadeAwayTime = 2.0f;
	Damage = 10.0f;
	ProjectileMeshAsset = FSoftObjectPath(TEXT("/Game/WoT/Meshes/MMyrddraalBolt/MMyrddraalBolt/SkeletalMeshes/MMyrddraalBolt.MMyrddraalBolt"));
	SoundHitPawn = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Crossbow_HitPawn1.Crossbow_HitPawn1"));
	SoundHitWall = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Myrddraal_HitWall1.Myrddraal_HitWall1"));
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Crossbow_Shoot1.Crossbow_Shoot1"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Myrddraal_Flight1.Myrddraal_Flight1"));
}

void AWoTMyrddraalBolt::BeginPlay()
{
	Super::BeginPlay();
	if (!GetWorld() || IsStuck() || GetNetMode() == NM_DedicatedServer) return;
	for (const FName Config : {FName(TEXT("MyrdSprayerA")),
		FName(TEXT("MyrdSprayerB"))})
	{
		if (AWoTParticleSprayer* Sprayer = WoTWeaveFX::SpawnLocal(*GetWorld(),
			Config, GetActorLocation(), FVector::UpVector.Rotation(), this))
		{
			Sprayers.Add(Sprayer);
		}
	}
}

void AWoTMyrddraalBolt::StopSprayers()
{
	if (bStoppedSprayers) return;
	for (AWoTParticleSprayer* Sprayer : Sprayers)
	{
		if (IsValid(Sprayer)) Sprayer->bOn = false;
	}
	bStoppedSprayers = true;
}

void AWoTMyrddraalBolt::SpawnImpactBundle(const FVector& HitLocation)
{
	if (bSpawnedImpactBundle || !GetWorld()) return;
	WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("MyrdSprayerExp"), HitLocation,
		(-GetVelocity()).Rotation());
	WoTWeaveFX::SpawnExplosion(*GetWorld(), TEXT("MyrddraalElectric"), HitLocation);
	bSpawnedImpactBundle = true;
}

void AWoTMyrddraalBolt::PlayExplodeCosmetics(const FVector& HitLocation,
	const FVector&)
{
	StopSprayers();
	SpawnImpactBundle(HitLocation);
}

void AWoTMyrddraalBolt::PlayWallImpactCosmetics(const FVector& HitLocation,
	const FVector& HitNormal)
{
	StopSprayers();
	SpawnImpactBundle(HitLocation);
	if (!bSpawnedCrack && GetWorld())
	{
		WoTDecalFX::SpawnAligned(*GetWorld(), TEXT("CrackDecal"),
			HitLocation + HitNormal, HitNormal);
		bSpawnedCrack = true;
	}
}

void AWoTMyrddraalBolt::Destroyed()
{
	// Destruction without an impact (lifespan, teardown, relevancy) is cleanup
	// only. Impact bundles are driven exactly once by the replicated event.
	StopSprayers();
	Super::Destroyed();
}

AWoTTrollocAxeProjectile::AWoTTrollocAxeProjectile()
{
	// TrollocAxeProjectileBase.uc likewise authors no size over the 0/0 base.
	SetUE1CollisionSize(0.0f, 0.0f);
	Speed = MaxSpeed = 800.0f;
	YawSpinRate = 300000;
	Damage = 10.0f;
	// TrollocAxeSProjectile -> TrollocAxeProjectileBase authors these over the
	// WotWeaponProjectile defaults (TrollocAxeProjectileBase.uc:14-18).
	StayStuckTime = 2.0f;
	FadeAwayTime = 3.0f;
	ProjectileMeshAsset = FSoftObjectPath(TEXT("/Game/WoT/Meshes/MTrollocAxeSProjectile/MTrollocAxeSProjectile/SkeletalMeshes/MTrollocAxeSProjectile.MTrollocAxeSProjectile"));
	SoundHitPawn = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Axe_HitPawn1.Axe_HitPawn1"));
	SoundHitWall = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Halberd_HitWall1.Halberd_HitWall1"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Axe_Flight1.Axe_Flight1"));
}

AWoTBATHalberdProjectile::AWoTBATHalberdProjectile()
{
	Speed = MaxSpeed = 2400.0f;
	RollSpinRate = 400000;
	bPassThroughActors = true;
	StayStuckTime = 2.0f;
	FadeAwayTime = 3.0f;
	// The sphere root can reproduce BATHalberdProjectile.uc:36's radius, but not
	// its separately-authored 15uu cylinder half-height (tracked in PORT_NOTES).
	Collision->SetSphereRadius(5.0f);
	ProjectileMeshAsset = FSoftObjectPath(TEXT("/Game/WoT/Meshes/MBATHalberdProjectile/MBATHalberdProjectile/SkeletalMeshes/MBATHalberdProjectile.MBATHalberdProjectile"));
	SoundHitPawn = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Sword_HitPawn1.Sword_HitPawn1"));
	SoundHitWall = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Halberd_HitWall1.Halberd_HitWall1"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Halberd_Flight1.Halberd_Flight1"));
	SoundRadius = 64;
	SoundPitch = 128;
}

AWoTQuestShieldProjectile::AWoTQuestShieldProjectile()
{
	// QuestionerShield.uc:93-94.
	SetUE1CollisionSize(18.0f, 2.0f);
	Speed = MaxSpeed = 2400.0f;
	YawSpinRate = 400000;
	bPassThroughActors = true;
	Damage = 25.0f;
	StayStuckTime = 2.5f;
	FadeAwayTime = 1.0f;
	bStyleMasked = true;
	bMotionBlur = true;
	ProjectileMeshAsset = FSoftObjectPath(TEXT("/Game/WoT/Meshes/MQuestionerShield/MQuestionerShield/SkeletalMeshes/MQuestionerShield.MQuestionerShield"));
	SoundHitPawn = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Sword_HitPawn1.Sword_HitPawn1"));
	SoundHitWall = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Shield_HitWall1.Shield_HitWall1"));
	AmbientLoopSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Arrow_Flight1.Arrow_Flight1"));
	SoundRadius = 64;
	SoundPitch = 192;
}
