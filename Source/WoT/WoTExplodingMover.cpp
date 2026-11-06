#include "WoTExplodingMover.h"
#include "WoTRandom.h"

#include "Inventory/WoTAngrealInventory.h"
#include "Projectiles/WoTProjectile.h"
#include "WoTEvents.h"
#include "WoTSound.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"

AWoTExplodingMover::AWoTExplodingMover()
{
	InitialState = EWoTMoverState::None;
	EncroachType = EWoTMoverEncroachType::IgnoreWhenEncroach;
	MoveTime = 0.0f;
	bTriggerOnceOnly = true;
	ActivatedBy.Init(NAME_None, 5);
	ActivatedBy[0] = FName(TEXT("All"));
}

void AWoTExplodingMover::BeginPlay()
{
	Super::BeginPlay();
	// An initial replication bunch can invoke OnRep before AWoTMover has cached
	// its base pose. Late joiners apply the spent pose here without replaying a
	// break sound they were not present to hear.
	if (!HasAuthority() && bExploded)
	{
		ApplyExplodedPresentation();
	}
}

bool AWoTExplodingMover::AcceptsDamageType(FName DamageType) const
{
	if (DamageType == FName(TEXT("All")))
	{
		return true;
	}
	for (const FName Allowed : ActivatedBy)
	{
		if (Allowed == FName(TEXT("All")) || Allowed == DamageType
			|| AWoTAngrealInventory::DamageTypeContains(DamageType, Allowed))
		{
			return true;
		}
	}
	return false;
}

float AWoTExplodingMover::TakeDamage(float DamageAmount,
	const FDamageEvent& DamageEvent, AController* EventInstigator, AActor*)
{
	if (!HasAuthority() || bExploded || bOnlyTriggerable)
	{
		return 0.0f;
	}

	FName DamageType(TEXT("All"));
	FVector Momentum = FVector::ZeroVector;
	if (DamageEvent.IsOfType(FWoTDamageEvent::ClassID))
	{
		const FWoTDamageEvent& Ev = static_cast<const FWoTDamageEvent&>(DamageEvent);
		if (!Ev.WoTDamageType.IsNone())
		{
			DamageType = Ev.WoTDamageType;
		}
		// ShotDirection remains the trace/impact direction. Retail impulse is
		// carried independently so non-pawn receivers share the character path.
		Momentum = Ev.UE1Momentum;
	}
	if (!AcceptsDamageType(DamageType))
	{
		return 0.0f;
	}

	const int32 AppliedDamage = FMath::TruncToInt(DamageAmount);
	Health -= AppliedDamage;
	if (Health <= 0)
	{
		Explode(EventInstigator ? EventInstigator->GetPawn() : nullptr, Momentum);
	}
	return static_cast<float>(AppliedDamage);
}

void AWoTExplodingMover::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (InitialState != EWoTMoverState::None)
	{
		// Authored base state: the STATE's Trigger shadows the class's plain
		// Trigger-to-Explode (UE1 semantics) — run the base FSM instead. Note
		// TakeDamage stays live regardless: ExplodingMover.uc:33's plain-function
		// override is not shadowed by any base state.
		Super::WoTTrigger(Other, EventInstigator);
		return;
	}
	Explode(EventInstigator, GetActorForwardVector());
}

void AWoTExplodingMover::Explode(APawn* EventInstigator, const FVector& Momentum)
{
	if (!HasAuthority() || bExploded)
	{
		return;
	}
	bExploded = true;
	bSpent = bTriggerOnceOnly;
	ApplyExplodedPresentation();

	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, EventInstigator);
	if (!BreakingSound.IsNone())
	{
		WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(BreakingSound),
			GetActorLocation(), 1600.0f, 1.0f, 2.0f);
	}

	SpawnFragments(EWoTFragmentType::Wall, NumWallChunks, WallParticleSize,
		FVector::ZeroVector, WallTextureName);
	SpawnFragments(EWoTFragmentType::Wood, NumWoodChunks, WoodParticleSize,
		FVector::ZeroVector, WoodTextureName);
	SpawnFragments(EWoTFragmentType::Glass, NumGlassChunks, GlassParticleSize,
		Momentum, GlassTextureName);
	ForceNetUpdate();
}

void AWoTExplodingMover::SpawnFragments(EWoTFragmentType Type, int32 Count,
	float Scale, const FVector& Momentum, FName SkinName)
{
	FActorSpawnParameters Params;
	Params.Owner = Type == EWoTFragmentType::Glass ? GetOwner() : nullptr;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector Location = GetActorLocation() + ExplosionDimensions * WoTRandom::VRand();
		if (AWoTFragment* Fragment = GetWorld()->SpawnActor<AWoTFragment>(
			AWoTFragment::StaticClass(), Location, FRotator::ZeroRotator, Params))
		{
			Fragment->Init(Type, ExplosionSize, Momentum, Scale, SkinName);
		}
	}
}

void AWoTExplodingMover::ApplyExplodedPresentation()
{
	ApplyAlpha(1.0f, HasAuthority());
	if (Mesh)
	{
		Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}
}

void AWoTExplodingMover::OnRep_Exploded()
{
	if (bExploded && HasActorBegunPlay())
	{
		ApplyExplodedPresentation();
		if (!BreakingSound.IsNone())
		{
			WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(BreakingSound),
				GetActorLocation(), 1600.0f, 1.0f, 2.0f);
		}
	}
}

void AWoTExplodingMover::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTExplodingMover, bExploded);
}
