#include "WoTProjectileLauncher.h"

#include "../WoTCharacter.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"

#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

AWoTProjectileLauncher::AWoTProjectileLauncher()
{
	// ProjectileLauncher defaultproperties
	bRestrictsUsage = true;
	Priority = 1.0f;
	MaxChargeUsedInterval = 1.0f;
}

void AWoTProjectileLauncher::CastArtifact()
{
	if (!bCasting)
	{
		Super::CastArtifact();
		bCasting = true;
		if (bAutoFire)
		{
			SetActorTickEnabled(true);   // UE1 ticked always; we tick on demand
		}
		AttemptCast();
	}
}

void AWoTProjectileLauncher::UnCastArtifact()
{
	if (bCasting)
	{
		Super::UnCastArtifact();
		bCasting = false;
		if (GetItemState() == EWoTInventoryState::Held)
		{
			SetActorTickEnabled(false);
		}
	}
}

void AWoTProjectileLauncher::Failed()
{
	// A failure removes the cooldown, permitting another attempt next tick
	// (ProjectileLauncher.uc:122).
	NextCastTime = GetWorld()->GetTimeSeconds();
	Super::Failed();
}

void AWoTProjectileLauncher::Reset()
{
	Super::Reset();
	NextCastTime = 0.0f;
}

void AWoTProjectileLauncher::AttemptCast()
{
	AActor* Source = LastOwner ? LastOwner.Get() : GetOwner();
	if (!IsValid(Source) || Source->IsActorBeingDestroyed())
	{
		// Actor ownership does not imply lifetime ownership in UE5. A launcher
		// that survives even one frame past its pawn must not cast from a stale
		// LastOwner while the pawn's Destroyed cleanup propagates.
		UnCastArtifact();
		return;
	}
	// Start one shot when its charge cost can be paid (ProjectileLauncher.uc:152).
	if (GetWorld()->GetTimeSeconds() > NextCastTime && HaveEnoughCharges())
	{
		LaunchProjectile();
	}
}

void AWoTProjectileLauncher::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Automatic mode retries while the caster keeps input active
	// (ProjectileLauncher.uc:162).
	if (bAutoFire && bCasting)
	{
		AttemptCast();
	}
}

void AWoTProjectileLauncher::LaunchProjectile()
{
	if (!ProjectileClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: ProjectileClass not properly set."), *GetName());
		Failed();
		return;
	}

	// LaunchProjectileEffect.Invoke, folded (PORT_NOTES):
	const AWoTGenericProjectile* CDO = ProjectileClass.GetDefaultObject();
	AActor* Source = LastOwner ? LastOwner.Get() : GetOwner();
	if (!IsValid(Source) || Source->IsActorBeingDestroyed())
	{
		UnCastArtifact();
		return;
	}
	APawn* SourcePawn = Cast<APawn>(Source);

	// Resolve a destination only for projectile classes that use one
	// (ProjectileLauncher.uc:192).
	BestTarget = nullptr;
	if (CDO->bUsesDestination)
	{
		BestTarget = GetBestTarget();
	}

	bool bSuccess = false;
	if (Source && !(CDO->bRequiresDestination && !BestTarget))
	{
		// rotation: Pawn ViewRotation, else actor rotation
		const FRotator Rot =
			SourcePawn ? SourcePawn->GetViewRotation() : Source->GetActorRotation();

		// location: Source + (LaunchOffset >> Source.Rotation) + eye height
		FVector Loc = Source->GetActorLocation();
		if (!CDO->LaunchOffset.IsZero())
		{
			Loc += Source->GetActorRotation().RotateVector(CDO->LaunchOffset);
		}
		if (SourcePawn)
		{
			Loc.Z += SourcePawn->BaseEyeHeight;
		}

		FActorSpawnParameters Params;
		Params.Owner = Source;
		Params.Instigator = SourcePawn;
		// UE1 Spawn adjusted into free space rather than failing on the castor
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		AWoTGenericProjectile* Proj = GetWorld()->SpawnActor<AWoTGenericProjectile>(
			ProjectileClass, Loc, Rot, Params);

		if (Proj)
		{
			Proj->SetSourceAngreal(this);
			if (!Proj->bHurtsOwner)
			{
				Proj->SetIgnoredPawn(SourcePawn);
			}
			if (AWoTSeekingProjectile* Seeking = Cast<AWoTSeekingProjectile>(Proj))
			{
				Seeking->NotifySeePawn(BestTarget);
			}
			else
			{
				Proj->SetDestination(BestTarget);
			}
			bSuccess = Proj->CreationSucceeded();
			if (!bSuccess)
			{
				Proj->Destroy();
			}
		}
	}

	if (bSuccess)
	{
		UseCharge();
		// Weapon fire is the loudest tactical cue: route through the G18 hearing
		// seam so tracking NPCs acquire the shooter by ear (retail CheckNoiseHearing
		// fans instigated noise to every pawn). Falls back to the bare engine noise
		// for non-WoT sources.
		if (AWoTCharacter* NoisyCaster = Cast<AWoTCharacter>(Source))
		{
			NoisyCaster->EmitNoise(1.0f);
		}
		else
		{
			Source->MakeNoise(1.0f, SourcePawn, Source->GetActorLocation());
		}
		// Advance the cooldown to the next permitted launch time
		// (ProjectileLauncher.uc:221).
		NextCastTime = GetWorld()->GetTimeSeconds() + (60.0f / RoundsPerMinute);
	}
	else
	{
		Failed();
	}
}

AActor* AWoTProjectileLauncher::GetBestTarget()
{
	// AI support: an explicitly assigned victim wins
	if (Target)
	{
		return Target;
	}
	const APawn* P = Cast<APawn>(LastOwner ? LastOwner.Get() : GetOwner());
	if (P && ProjectileClass)
	{
		return FindBestTarget(GetTrajectorySource(), P->GetViewRotation(),
			ProjectileClass.GetDefaultObject()->MaxTargetAngle);
	}
	return nullptr;
}

// FindBestTarget / IsTargetable moved to AWoTAngrealInventory (the base) so
// non-launcher artifacts (Whirlwind) can target; inherited here unchanged.

// ---- AI support ----

bool AWoTProjectileLauncher::IsLeadable()
{
	return ProjectileClass
	       && ProjectileClass.GetDefaultObject()->bRequiresLeading;
}

float AWoTProjectileLauncher::GetLeadSpeed()
{
	return IsLeadable() ? ProjectileClass.GetDefaultObject()->Speed : 0.0f;
}

FRotator AWoTProjectileLauncher::GetBestTrajectory()
{
	AActor* Source = LastOwner ? LastOwner.Get() : GetOwner();
	if (IsLeadable())
	{
		return ProjectileClass.GetDefaultObject()->CalculateTrajectory(Source, Target,
			ProjectileClass.GetDefaultObject()->Speed);
	}
	if (Source && Target)
	{
		return (Target->GetActorLocation() - Source->GetActorLocation()).Rotation();
	}
	return FRotator::ZeroRotator;
}

float AWoTProjectileLauncher::GetMaxRange()
{
	return AWoTGenericProjectile::GetMaxRange(ProjectileClass);
}

float AWoTProjectileLauncher::GetMinRange()
{
	return AWoTGenericProjectile::GetMinRange(ProjectileClass);
}
