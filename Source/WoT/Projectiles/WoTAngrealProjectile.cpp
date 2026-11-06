#include "WoTAngrealProjectile.h"

#include "../WoTCarcass.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/SkeletalMesh.h"

#include "../Inventory/WoTAngrealInventory.h"

#include "Components/SphereComponent.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealProjectile::AWoTAngrealProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision->OnComponentBeginOverlap.AddDynamic(
		this, &AWoTAngrealProjectile::OnProjectileOverlap);
	Collision->SetNotifyRigidBodyCollision(true);
	CylinderCollision->OnComponentBeginOverlap.AddDynamic(
		this, &AWoTAngrealProjectile::OnProjectileOverlap);
}

void AWoTAngrealProjectile::BeginPlay()
{
	Super::BeginPlay();
	// Constructor-created components do not have a world in every creation
	// path. Replay the component-tag contract once the world is authoritative.
	UWoTDynamicLightSubsystem::RegisterTaggedComponents(this);
}

void AWoTAngrealProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// replication { reliable if(Role==ROLE_Authority)
	//   SourceAngreal, SourcePawn, ProjTouchTime, ProjCollisionRadius }
	DOREPLIFETIME(AWoTAngrealProjectile, SourceAngreal);
	DOREPLIFETIME(AWoTAngrealProjectile, SourcePawn);
	DOREPLIFETIME(AWoTAngrealProjectile, ProjTouchTime);
	DOREPLIFETIME(AWoTAngrealProjectile, ProjCollisionRadius);
}

void AWoTAngrealProjectile::SetSourceAngreal(AWoTAngrealInventory* Source)
{
	SourceAngreal = Source;
	if (SourceAngreal)
	{
		APawn* P = Cast<APawn>(SourceAngreal->GetOwner());
		SetInstigator(P);
		SourcePawn = P;
	}
}

void AWoTAngrealProjectile::Reflected()
{
	// AngrealProjectile.uc:120-126: a non-simulated PlaySound — the server's
	// play, replicated to players in range (UE1's native PlaySound), radius 1600.
	if (UWorld* World = GetWorld())
	{
		WoTSound::PlayReplicatedAsset(*World, ReflectedSound, GetActorLocation(),
			WoTSound::DefaultPlaySoundRadius);
	}
}

bool AWoTAngrealProjectile::IsTouchTarget(const AActor* Other)
{
	// bProjTarget || (bBlockActors && bBlockPlayers): pawns and anything
	// that blocks pawn movement qualify under UE5 collision semantics.
	if (!Other)
	{
		return false;
	}
	if (Other->IsA<APawn>())
	{
		return true;
	}
	// bProjTarget=true on corpses (WOTCarcass.uc:150): shootable, non-blocking.
	if (const IWoTRetailProjectileTarget* Target =
		Cast<IWoTRetailProjectileTarget>(Other))
	{
		return Target->IsRetailProjectileTouchTarget();
	}
	// bProjTarget: shootable non-blockers — e.g. an armed ExpWard
	// (AngrealExpWardProjectile.uc:288-290: bProjTarget=True, bCollideActors=False).
	const UPrimitiveComponent* Root =
		Cast<UPrimitiveComponent>(Other->GetRootComponent());
	return Root && Root->GetCollisionEnabled() != ECollisionEnabled::NoCollision
	       && Root->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block
	       && Root->GetCollisionResponseToChannel(ECC_GameTraceChannel1) == ECR_Block;
}

void AWoTAngrealProjectile::OnProjectileOverlap(
	UPrimitiveComponent*, AActor* Other, UPrimitiveComponent*, int32, bool,
	const FHitResult& Hit)
{
	// AngrealProjectile.Touch: filter, then hand to ProcessTouch. The trace
	// refinement (Trace(Location, OldLocation)) is covered by the sweep hit.
	if (Other && Other != this && Other != GetOwner() && IsTouchTarget(Other))
	{
		ProcessTouch(Other, Hit.ImpactPoint.IsZero()
		                        ? GetActorLocation() : FVector(Hit.ImpactPoint));
	}
}

void AWoTAngrealProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bGenProjTouch || ProjCollisionRadius <= 0.0f)
	{
		return;
	}
	ProjTouchTimer -= DeltaTime;
	if (ProjTouchTimer >= 0.0f)
	{
		return;
	}
	ProjTouchTimer = ProjTouchTime;

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByChannel(
		Overlaps, GetActorLocation(), FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeSphere(ProjCollisionRadius));
	TSet<TWeakObjectPtr<AActor>> Now;
	auto AddTouching = [this, &Now](AActor* Actor)
	{
		if (!IsValid(Actor) || Actor == this || Now.Contains(Actor))
		{
			return;
		}
		Now.Add(Actor);
		if (!ProjTouching.Contains(Actor))
		{
			ProjTouch(Actor);
		}
	};
	for (const FOverlapResult& O : Overlaps)
	{
		AActor* A = O.GetActor();
		if (!A || A == this || (bTouchPawnsOnly && !A->IsA<APawn>()))
		{
			continue;
		}
		AddTouching(A);
	}
	if (!bTouchPawnsOnly)
	{
		// Retail walks Level.ProjectileList independently of collision. This is
		// how dormant eyes and not-yet-armed wards are still found by an AMA dome.
		const FVector Center = GetActorLocation();
		const float RadiusSq = FMath::Square(ProjCollisionRadius);
		for (TActorIterator<AWoTAngrealProjectile> It(GetWorld()); It; ++It)
		{
			AWoTAngrealProjectile* Projectile = *It;
			if (Projectile != this
				&& FVector::DistSquared(Center, Projectile->GetActorLocation())
					<= RadiusSq)
			{
				AddTouching(Projectile);
			}
		}
	}
	for (const TWeakObjectPtr<AActor>& Old : ProjTouching)
	{
		if (Old.IsValid() && !Now.Contains(Old))
		{
			ProjUnTouch(Old.Get());
		}
	}
	ProjTouching = MoveTemp(Now);
}

void AWoTAngrealProjectile::ConfigureUE1Light(UPointLightComponent* Light,
	uint8 Brightness, uint8 Hue, uint8 Saturation, float Radius,
	EWoTDynLightShape Shape, EWoTDynLightType Type, uint8 Period, uint8 Phase)
{
	if (!Light)
	{
		return;
	}
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetAttenuationRadius((Radius + 1.0f) * 25.0f);   // WorldLightRadius: 25 x (r+1)
	Light->bUseInverseSquaredFalloff = false;
	Light->LightFalloffExponent = 2.0f;
	Light->SetIntensity(Brightness / 64.0f * 5.0f);
	// UE1 hue 0-255 over the wheel; saturation 0 = pure hue, 255 = white.
	const FLinearColor Color = FLinearColor::MakeFromHSV8(
		Hue, static_cast<uint8>(255 - Saturation), 255);
	Light->SetLightColor(Color);
	Light->SetCastShadows(false);
	UWoTDynamicLightSubsystem::RegisterOrDefer(
		Light, Brightness, Hue, Saturation, Shape, Type, Period, Phase);
}

void AWoTAngrealProjectile::BindWeaveMesh(UWoTVertexAnimComponent* Visual,
                                          FName MeshName, float DrawScale)
{
	if (!Visual)
	{
		return;
	}
	if (Visual->SetVertexMesh(MeshName, TEXT("Angreal")))
	{
		Visual->SetRelativeScale3D(FVector(DrawScale));
	}
}
