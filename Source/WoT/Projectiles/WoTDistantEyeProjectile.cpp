#include "WoTDistantEyeProjectile.h"

#include "../Inventory/WoTAngrealInvDistantEye.h"
#include "../Inventory/WoTDistantEyeCastReflector.h"
#include "../Inventory/WoTDistantEyeLeech.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../WoTCharacter.h"
#include "../WoTSoundTable.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"
#include "WoTDartProjectile.h"

#include "Camera/CameraTypes.h"
#include "Components/SphereComponent.h"
#include "Engine/Texture2D.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Net/UnrealNetwork.h"

AWoTDistantEyeProjectile::AWoTDistantEyeProjectile()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PrimaryActorTick.bCanEverTick = true;
	bNetTemporary = false;
	bAlwaysRelevant = true;
	InitialLifeSpan = 0.0f;
	Speed = 0.0f;
	MaxSpeed = 0.0f;
	Health = 50;
	bProjTarget = true;
	FrameTime = 0.2f;
	EyeRoundsPerMinute = 320.0f;

	ProjectileMovement->StopMovementImmediately();
	ProjectileMovement->Deactivate();
	Collision->InitSphereRadius(8.0f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);

	ImpactSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/DropDE.DropDE"));
	for (int32 Frame = 1; Frame <= 7; ++Frame)
	{
		const FString Name = FString::Printf(TEXT("DE_%02d"), Frame);
		AnimSkins.Add(TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/Angreal/%s.%s"), *Name, *Name))));
	}
}

void AWoTDistantEyeProjectile::BeginPlay()
{
	Super::BeginPlay();
	BindWeaveMesh(DisplayMesh, FName(TEXT("FlatEye")), 1.0f);
	ApplySkinFrame(CurrentSkinIndex);
}

void AWoTDistantEyeProjectile::Open()
{
	if (!HasAuthority() || bOpen)
	{
		return;
	}
	bOpen = true;
	OnRep_Open();
	InstallViewingEffects();
	ForceNetUpdate();
}

void AWoTDistantEyeProjectile::Close()
{
	if (!HasAuthority())
	{
		return;
	}
	UnFire();
	RemoveViewingEffects();
	if (bOpen)
	{
		bOpen = false;
		OnRep_Open();
		ForceNetUpdate();
	}
}

void AWoTDistantEyeProjectile::OnRep_Open()
{
	TargetSkinIndex = bOpen ? AnimSkins.Num() - 1 : 0;
	FrameAccumulator = 0.0f;
}

void AWoTDistantEyeProjectile::InstallViewingEffects()
{
	APawn* Host = GetInstigator();
	if (!IsValid(Host))
	{
		return;
	}

	if (!IsValid(CastReflector))
	{
		CastReflector = NewObject<UWoTDistantEyeCastReflector>(Host);
		CastReflector->Eye = this;
		CastReflector->SetSourceAngreal(SourceAngreal);
		CastReflector->bFromProjectile = true;
		CastReflector->Install(Host);
	}
	if (!IsValid(ViewLeech))
	{
		ViewLeech = NewObject<UWoTDistantEyeLeech>(Host);
		ViewLeech->InitializeWithProjectile(this);
		ViewLeech->AttachTo(Host);
	}
}

void AWoTDistantEyeProjectile::RemoveViewingEffects()
{
	if (IsValid(CastReflector))
	{
		CastReflector->Uninstall();
	}
	CastReflector = nullptr;
	if (IsValid(ViewLeech))
	{
		ViewLeech->Unattach();
	}
	ViewLeech = nullptr;
}

void AWoTDistantEyeProjectile::Fire()
{
	if (!HasAuthority())
	{
		return;
	}
	bFiring = true;
	LaunchDart();
}

void AWoTDistantEyeProjectile::UnFire()
{
	bFiring = false;
}

void AWoTDistantEyeProjectile::LaunchDart()
{
	if (!HasAuthority() || !GetWorld() || !IsValid(SourceAngreal)
		|| GetWorld()->GetTimeSeconds() < NextFireTime)
	{
		return;
	}
	NextFireTime = GetWorld()->GetTimeSeconds() + 60.0f / EyeRoundsPerMinute;

	FActorSpawnParameters Params;
	// Retail Spawn(ProjectileType) makes the EYE the dart's Owner (UE1 defaults
	// Owner to the spawner) — the projectile overlap filter's Other != GetOwner()
	// is what keeps the dart from detonating on its own eye at spawn (UE1 never
	// touched start-penetrating actors). Instigator stays the caster for damage
	// credit. Deliberate retail consequence: an eye dart CAN hit the caster.
	Params.Owner = this;
	Params.Instigator = GetInstigator();
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AWoTDartProjectile* Dart = GetWorld()->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), GetActorLocation(),
		GetActorRotation(), Params))
	{
		Dart->SetSourceAngreal(SourceAngreal);
		Dart->VariantColor = FName(TEXT("Green"));
		Dart->OnRep_VariantColor();
		Dart->ForceNetUpdate();
	}
}

void AWoTDistantEyeProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (CurrentSkinIndex != TargetSkinIndex && AnimSkins.Num() > 0)
	{
		FrameAccumulator += DeltaTime;
		while (CurrentSkinIndex != TargetSkinIndex
			&& FrameAccumulator >= FrameTime)
		{
			FrameAccumulator -= FrameTime;
			CurrentSkinIndex += CurrentSkinIndex < TargetSkinIndex ? 1 : -1;
			ApplySkinFrame(CurrentSkinIndex);
		}
	}

	if (!HasAuthority())
	{
		return;
	}
	AWoTCharacter* Host = Cast<AWoTCharacter>(GetInstigator());
	if (!IsValid(Host) || !Host->IsAlive())
	{
		Destroy();
		return;
	}
	if (!bOpen)
	{
		return;
	}
	SetActorRotation(Host->GetViewRotation());
	if (bFiring && GetWorld()->GetTimeSeconds() >= NextFireTime)
	{
		LaunchDart();
	}
}

void AWoTDistantEyeProjectile::ApplySkinFrame(int32 Index)
{
	if (!AnimSkins.IsValidIndex(Index) || !DisplayMesh)
	{
		return;
	}
	DisplayMesh->SetAllSkinOverrides(AnimSkins[Index].ToSoftObjectPath());
}

float AWoTDistantEyeProjectile::TakeDamage(float DamageAmount,
	const FDamageEvent&, AController*, AActor*)
{
	// Retail Health -= Damage is UNCONDITIONAL (uc:65-74): negative damage
	// heals the eye — keep only the authority + already-dead guards.
	if (!HasAuthority() || Health <= 0)
	{
		return 0.0f;
	}
	Health -= FMath::TruncToInt(DamageAmount);
	if (Health <= 0)
	{
		Destroy();
	}
	return DamageAmount;
}

void AWoTDistantEyeProjectile::CalcCamera(float DeltaTime,
	FMinimalViewInfo& OutResult)
{
	Super::CalcCamera(DeltaTime, OutResult);
	OutResult.Location = GetActorLocation();
	if (const APawn* Host = GetInstigator())
	{
		OutResult.Rotation = Host->GetViewRotation();
	}
	else
	{
		OutResult.Rotation = GetActorRotation();
	}
	// AWoTPlayerController::CalcCamera's G32 match-end branch returns before
	// delegating to its external target, so that higher state deliberately wins.
}

void AWoTDistantEyeProjectile::Destroyed()
{
	if (!bDestroyedHandled)
	{
		bDestroyedHandled = true;
		if (GetWorld() && GetNetMode() != NM_DedicatedServer)
		{
			if (AWoTExplosionEffect* Effect = WoTWeaveFX::SpawnExplosion(
				*GetWorld(), TEXT("GreenExplode"), GetActorLocation()))
			{
				Effect->Sprite->DrawScale = 0.3f;
			}
		}
		if (HasAuthority())
		{
			AWoTAngrealInventory* ChargeSource = SourceAngreal;
			if (GetWorld())
			{
				WoTSound::PlayReplicated(*GetWorld(), TEXT("Angreal/DropDE"),
					GetActorLocation(), 1600.0f);
			}
			Close();
			if (AWoTAngrealInvDistantEye* Item =
				Cast<AWoTAngrealInvDistantEye>(ChargeSource))
			{
				Item->NotifyEyeDestroyed(this);
			}
			if (IsValid(ChargeSource))
			{
				ChargeSource->UseCharge();
			}
		}
	}
	Super::Destroyed();
}

void AWoTDistantEyeProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDistantEyeProjectile, bOpen);
}
