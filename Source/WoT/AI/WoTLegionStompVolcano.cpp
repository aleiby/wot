#include "WoTLegionStompVolcano.h"

#include "../Projectiles/WoTProjectile.h"
#include "../WoTAnimData.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

namespace
{
	const FName StageOne(TEXT("VolcanoStageOne"));
	const FName StageThree(TEXT("VolcanoStageThree"));

	float StageDuration(const TArray<FWoTAnimSequence>* Sequences, FName Name)
	{
		if (Sequences)
		{
			for (const FWoTAnimSequence& Sequence : *Sequences)
			{
				if (Sequence.Name == Name && Sequence.NumFrames > 1
					&& Sequence.Rate > KINDA_SMALL_NUMBER)
				{
					return (Sequence.NumFrames - 1)
						/ (Sequence.Rate * AWoTLegionStompVolcano::VolcanoAnimRate);
				}
			}
		}
		// LegionStompVolcano.uc:11,13 plus the converter-authored default rate:
		// (10 frames - the clamped terminal frame) / (30 fps * PlayAnim rate 0.5).
		return 9.0f / (30.0f * AWoTLegionStompVolcano::VolcanoAnimRate);
	}
}

AWoTLegionStompVolcano::AWoTLegionStompVolcano()
{
	PrimaryActorTick.bCanEverTick = true;

	Collision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("VolcanoCollision"));
	SetRootComponent(Collision);
	Collision->InitCapsuleSize(VolcanoRadius, VolcanoHalfHeight);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(true);
	Collision->SetCanEverAffectNavigation(false);
	Collision->OnComponentBeginOverlap.AddDynamic(
		this, &AWoTLegionStompVolcano::OnVolcanoBeginOverlap);
	Collision->OnComponentEndOverlap.AddDynamic(
		this, &AWoTLegionStompVolcano::OnVolcanoEndOverlap);

	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("VolcanoMesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetRelativeScale3D(FVector(1.75f)); // LegionStompVolcano.uc:68
	Mesh->OnAnimEnd.AddDynamic(this, &AWoTLegionStompVolcano::OnVolcanoAnimEnd);

	// Effects.RemoteRole=ROLE_SimulatedProxy (LegionStompVolcano.uc:64): the
	// authority owns gameplay and every receiving machine derives presentation.
	bReplicates = true;
	SetReplicatingMovement(false);
	NetPriority = 6.0f;
}

float AWoTLegionStompVolcano::ResolveEruptionLifetimeSeconds()
{
	const TArray<FWoTAnimSequence>* Sequences =
		WoTAnimData::FindSequences(TEXT("LegionStompVolcano"));
	return StageDuration(Sequences, StageOne) + StageDuration(Sequences, StageThree);
}

void AWoTLegionStompVolcano::BeginPlay()
{
	Super::BeginPlay();

	EruptionLifetimeSeconds = ResolveEruptionLifetimeSeconds();
	SetLifeSpan(EruptionLifetimeSeconds);

	if (GetNetMode() != NM_DedicatedServer)
	{
		BindAndPlayVisual();
		SpawnSprayers();
	}

	// UE1 spawn encroachment produces the first Touch.  Refresh after BeginPlay so
	// a pawn already inside the newly registered broadphase is evaluated once;
	// CylinderMembers prevents a component callback plus this census from applying
	// the same enter edge twice.
	Collision->UpdateOverlaps();
	TArray<AActor*> InitialOverlaps;
	Collision->GetOverlappingActors(InitialOverlaps, AWoTCharacter::StaticClass());
	for (AActor* Other : InitialOverlaps)
	{
		if (AWoTCharacter* Victim = Cast<AWoTCharacter>(Other))
		{
			BroadphasePawns.Add(Victim);
		}
		UpdateCylinderMembership(Other);
	}
}

void AWoTLegionStompVolcano::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SweepCylinderMembership();
}

void AWoTLegionStompVolcano::BindAndPlayVisual()
{
	if (Mesh->SetVertexMesh(TEXT("LegionStompVolcano")))
	{
		Mesh->PlayAnim(StageOne, VolcanoAnimRate);
	}
}

void AWoTLegionStompVolcano::SpawnSprayers()
{
	if (!GetWorld())
	{
		return;
	}
	const FVector At = GetActorLocation() + FVector(0.0f, 0.0f, 32.0f);
	const FRotator Up = FVector::UpVector.Rotation();
	for (const FName Config : {FName(TEXT("LegionStompSprayerA")),
		FName(TEXT("LegionStompSprayerB")), FName(TEXT("LegionStompSprayerC")),
		FName(TEXT("LegionStompSprayerD"))})
	{
		WoTWeaveFX::SpawnLocal(*GetWorld(), Config, At, Up);
	}
}

void AWoTLegionStompVolcano::OnVolcanoBeginOverlap(
	UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32,
	bool, const FHitResult&)
{
	if (AWoTCharacter* Victim = Cast<AWoTCharacter>(OtherActor))
	{
		BroadphasePawns.Add(Victim);
	}
	UpdateCylinderMembership(OtherActor);
}

void AWoTLegionStompVolcano::OnVolcanoEndOverlap(
	UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32)
{
	BroadphasePawns.Remove(Cast<AWoTCharacter>(OtherActor));
	// A broadphase end normally means an exact-cylinder exit too, but evaluate the
	// retail predicate rather than letting the clamped capsule define that edge.
	UpdateCylinderMembership(OtherActor);
}

bool AWoTLegionStompVolcano::IsInsideRetailCylinder(
	const AWoTCharacter* Victim) const
{
	if (!IsValid(Victim))
	{
		return false;
	}
	const FVector Delta = Victim->GetActorLocation() - GetActorLocation();
	const float ContactRadius = VolcanoRadius + Victim->GetSimpleCollisionRadius();
	const float ContactHalfHeight =
		VolcanoHalfHeight + Victim->GetSimpleCollisionHalfHeight();
	return FVector::DistSquared2D(Victim->GetActorLocation(), GetActorLocation())
			<= FMath::Square(ContactRadius)
		&& FMath::Abs(Delta.Z) <= ContactHalfHeight;
}

void AWoTLegionStompVolcano::UpdateCylinderMembership(AActor* OtherActor)
{
	AWoTCharacter* Victim = Cast<AWoTCharacter>(OtherActor);
	if (!IsValid(Victim))
	{
		BroadphasePawns.Remove(Victim);
		CylinderMembers.Remove(Victim);
		return;
	}

	const bool bWasInside = CylinderMembers.Contains(Victim);
	const bool bIsInside = IsInsideRetailCylinder(Victim);
	if (!bIsInside)
	{
		CylinderMembers.Remove(Victim);
		return;
	}
	if (!bWasInside)
	{
		CylinderMembers.Add(Victim);
		ApplyTouch(Victim);
	}
}

void AWoTLegionStompVolcano::SweepCylinderMembership()
{
	// In particular, +Z can leave the 16-unit retail cylinder while the pawn still
	// overlaps the component's clamped 48-unit broadphase.  Sweep both populations
	// for the actor's lifetime so that exit clears and gravity re-entry touches.
	for (auto It = BroadphasePawns.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = CylinderMembers.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TSet<TWeakObjectPtr<AWoTCharacter>> Candidates = BroadphasePawns;
	Candidates.Append(CylinderMembers);
	for (const TWeakObjectPtr<AWoTCharacter>& Candidate : Candidates)
	{
		UpdateCylinderMembership(Candidate.Get());
	}
}

void AWoTLegionStompVolcano::ApplyTouch(AActor* OtherActor)
{
	AWoTCharacter* Victim = Cast<AWoTCharacter>(OtherActor);
	if (!HasAuthority() || !IsValid(Victim) || Victim->Health <= 0)
	{
		return;
	}

	FWoTDamageEvent DamageEvent;
	DamageEvent.WoTDamageType = TEXT("xxFxx");
	DamageEvent.UE1Momentum = FVector::ZeroVector;
	DamageEvent.HitInfo.ImpactPoint = Victim->GetActorLocation();
	APawn* DamageOwner = Cast<APawn>(GetOwner());
	Victim->TakeDamage(VolcanoDamage, DamageEvent,
		DamageOwner ? DamageOwner->GetController() : nullptr, GetOwner());
	if (UCharacterMovementComponent* Movement = Victim->GetCharacterMovement())
	{
		// LegionStompVolcano.uc:41-45 applies the launch only to PHYS_Walking.
		// A pawn already tossed by Legion.ShakeRadius is falling when the newly
		// spawned volcano touches it, so that touch damages and shakes without
		// adding a second vertical impulse.
		if (Movement->IsMovingOnGround())
		{
			Movement->SetMovementMode(MOVE_Falling);
			Movement->Velocity.Z += VolcanoLaunchZ;
		}
	}

	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Victim->GetController()))
	{
		PC->ClientShakeView(0.6f, 3500.0f, -50.0f);
	}
}

void AWoTLegionStompVolcano::OnVolcanoAnimEnd(FName Sequence)
{
	if (Sequence == StageOne)
	{
		Mesh->PlayAnim(StageThree, VolcanoAnimRate);
	}
}
