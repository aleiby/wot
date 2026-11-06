#include "WoTWeaponComponent.h"
#include "../WoTRandom.h"
#include "WoTPathNodeIterator.h"

#include "../Projectiles/WoTProjectile.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

UWoTWeaponComponent::UWoTWeaponComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

// A world-geometry line trace mirroring Invoker.Trace(...,false) (bTraceActors
// false — the occlusion checks in AdjustProjectileWeapondAim see only the level,
// never pawns). Ignores the shooter and the target; hits static world only.
static bool AimTraceBlocked(const APawn* Invoker, const AActor* Target,
                            const FVector& Start, const FVector& End)
{
	const UWorld* World = Invoker ? Invoker->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}
	// WotWeapon.uc:595/628/635 Trace(..., false): only level and movers alter
	// lead/head aim; allies, projectiles and carcasses are transparent.
	return WoTVisBlockingTraceHits(
		const_cast<UWorld*>(World), Start, End, Invoker);
}

// -------------------------------------------------------------- usage model

bool UWoTWeaponComponent::CanUseAsMeleeWeapon(float GoalDistance) const
{
	// WotWeapon.uc:348.
	return GoalDistance >= MinMeleeRange && GoalDistance <= MaxMeleeRange;
}

bool UWoTWeaponComponent::CanUseAsProjectileWeapon(float GoalDistance) const
{
	// WotWeapon.uc:358 — includes the "has a projectile type" clause (uc:365).
	return GoalDistance >= MinProjectileRange && GoalDistance <= MaxProjectileRange &&
	       ProjectileClass != nullptr;
}

void UWoTWeaponComponent::DetermineWeaponUsage(float GoalDistance, bool bGoalVisible)
{
	DeterminedWeaponUsage = ClassifyWeaponUsage(
		GoalDistance, bGoalVisible, MinMeleeRange, MaxMeleeRange, MeleeEffectiveness,
		MinProjectileRange, MaxProjectileRange, ProjectileEffectiveness,
		ProjectileClass != nullptr, bRequireTargetVisibility);
}

EWoTWeaponUsage UWoTWeaponComponent::ClassifyWeaponUsage(
	float GoalDistance, bool bGoalVisible, float MinMelee, float MaxMelee,
	float InMeleeEffectiveness, float MinProjectile, float MaxProjectile,
	float InProjectileEffectiveness, bool bHasProjectileClass, bool bRequireVisibility)
{
	// WotWeapon.uc:285-326 — verbatim ladder. Inclusive endpoints are important:
	// the retail weapons can fire exactly at Min/MaxProjectileRange.
	if (bRequireVisibility && !bGoalVisible)
	{
		return EWoTWeaponUsage::None;
	}
	const bool bMelee = GoalDistance >= MinMelee && GoalDistance <= MaxMelee;
	const bool bProjectile = GoalDistance >= MinProjectile && GoalDistance <= MaxProjectile &&
		bHasProjectileClass;
	if (bMelee && bProjectile)
	{
		// The more-effective mode wins the tie (uc:300).
		return (InMeleeEffectiveness > InProjectileEffectiveness)
			? EWoTWeaponUsage::Melee : EWoTWeaponUsage::Projectile;
	}
	if (bMelee)
	{
		return EWoTWeaponUsage::Melee;
	}
	if (bProjectile)
	{
		return EWoTWeaponUsage::Projectile;
	}
	return EWoTWeaponUsage::None;
}

// -------------------------------------------------------------- aim support

FVector UWoTWeaponComponent::GetAimSource(const APawn* Invoker, const FVector& TargetLocation)
{
	// WotWeapon.uc:506: muzzle just outside the shooter's cylinder toward the
	// target, raised half the shooter's half-height. UE5 GetSimpleCollisionHalf-
	// Height() == UE1 CollisionHeight, so the .uc's CollisionHeight/2 is HalfHeight*0.5.
	if (!Invoker)
	{
		return TargetLocation;
	}
	const FVector InvLoc = Invoker->GetActorLocation();
	const FVector Forward = (TargetLocation - InvLoc).Rotation().Vector();
	const float Radius = Invoker->GetSimpleCollisionRadius();
	const float HalfHeight = Invoker->GetSimpleCollisionHalfHeight();
	return InvLoc + (Radius + 4.0f) * Forward + FVector(0.0f, 0.0f, HalfHeight * 0.5f);
}

float UWoTWeaponComponent::GetLeadSpeed() const
{
	// WotWeapon.uc:550: WotWeaponProjectileType.default.Speed.
	if (const AWoTProjectile* CDO = ProjectileClass ? ProjectileClass->GetDefaultObject<AWoTProjectile>() : nullptr)
	{
		return CDO->Speed;
	}
	return 0.0f;
}

FVector UWoTWeaponComponent::ComputeLeadDestination(const FVector& TargetLoc, const FVector& TargetVel,
                                                    const FVector& ProjectileStart, float ProjectileSpeed,
                                                    float LeadRandom01)
{
	// WotWeapon.uc:592-594, verbatim: shift the aim point along the target's
	// velocity by (time-of-flight) scaled by FMin(1, 0.7 + 0.6*FRand()).
	FVector Dest = TargetLoc;
	if (ProjectileSpeed > 0.0f)
	{
		const float Frac = FMath::Min(1.0f, 0.7f + 0.6f * LeadRandom01);
		Dest += Frac * (TargetVel * (TargetLoc - ProjectileStart).Size() / ProjectileSpeed);
	}
	return Dest;
}

FRotator UWoTWeaponComponent::AdjustProjectileWeaponAim(APawn* Invoker, AActor* Target,
                                                        float ProjectileSpeed, const FVector& ProjectileStart,
                                                        float LeadRandom01)
{
	// WotWeapon.uc:568 for an NPC bow. AimError = 0 (MissOdds truncates to int 0),
	// so the skill/AimError scaling (uc:584-588) is inert and the yaw perturb
	// (uc:660) is +0.5 unit ≈ nothing — both dropped. The bIsPlayer feet-aim branch
	// (uc:603-622) and the ContextSensitiveGoal last-visible branch (uc:638-655)
	// never apply to an Archer shooting an actor goal — ported dead (omitted).
	if (!Target)
	{
		return Invoker ? Invoker->GetActorRotation() : FRotator::ZeroRotator;
	}
	const float Roll = (LeadRandom01 >= 0.0f) ? LeadRandom01 : WoTRandom::FRand();
	const FVector TargetLoc = Target->GetActorLocation();

	FVector Dest = TargetLoc;
	if (ProjectileSpeed > 0.0f)
	{
		Dest = ComputeLeadDestination(TargetLoc, Target->GetVelocity(), ProjectileStart, ProjectileSpeed, Roll);
		// If the led point is occluded, pull the aim halfway back to the target
		// (uc:595-599).
		if (AimTraceBlocked(Invoker, Target, ProjectileStart, Dest))
		{
			Dest = 0.5f * (Dest + TargetLoc);
		}
	}

	// Abdomen then head trace (uc:624-636): reset the aim height to the target's
	// origin (dropping the lead's vertical component), and if that line is blocked
	// raise to 0.9 of the target's half-height. TraceHitActor starts non-None (the
	// Invoker sentinel, uc:602), so the abdomen trace always runs.
	float GoalRadius = 0.0f, GoalHalfHeight = 0.0f;
	Target->GetSimpleCollisionCylinder(GoalRadius, GoalHalfHeight);
	Dest.Z = TargetLoc.Z;
	bool bBlocked = AimTraceBlocked(Invoker, Target, ProjectileStart, Dest);
	if (bBlocked)
	{
		Dest.Z = TargetLoc.Z + 0.9f * GoalHalfHeight;
	}

	FRotator AimDir = (Dest - ProjectileStart).Rotation();

	// Yaw clamp (uc:663-676): keep the shot within ~45° (8192 UE1 units) of the
	// shooter's current facing, so it never whips around.
	if (Invoker)
	{
		const float InvYaw = Invoker->GetActorRotation().Yaw;
		const float Delta = FMath::FindDeltaAngleDegrees(InvYaw, AimDir.Yaw);
		if (FMath::Abs(Delta) > 45.0f)
		{
			AimDir.Yaw = InvYaw + FMath::Sign(Delta) * 45.0f;
		}
	}
	return AimDir;
}

bool UWoTWeaponComponent::ShouldLeadTarget(const APawn* Invoker, const AActor* Target)
{
	// WotWeapon.uc:526: a pawn shooting a moving actor.
	return Invoker != nullptr && Target != nullptr &&
	       !FMath::IsNearlyZero(Target->GetVelocity().Size());
}

void UWoTWeaponComponent::GetAimParams(APawn* Invoker, AActor* Target, FVector& OutSource,
                                       FRotator& OutDirection, float LeadRandom01) const
{
	// WotWeapon.uc:449.
	const FVector TargetLocation = Target->GetActorLocation();
	OutSource = GetAimSource(Invoker, TargetLocation);
	if (ShouldLeadTarget(Invoker, Target))   // IsLeadable() is always true (uc:519)
	{
		OutDirection = AdjustProjectileWeaponAim(Invoker, Target, GetLeadSpeed(), OutSource, LeadRandom01);
	}
	else
	{
		OutDirection = (TargetLocation - Invoker->GetActorLocation()).Rotation();
	}
}

void UWoTWeaponComponent::UseProjectileWeapon(APawn* Invoker, AActor* Target)
{
	// WotWeapon.uc:376. RandomizeAimParams (uc:471) is a no-op — its body is
	// commented out in shipped. ViewRotation is left to the controller focus.
	if (!Invoker || !Target || !ProjectileClass ||
	    !GetWorld() || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	FVector Source;
	FRotator Direction;
	GetAimParams(Invoker, Target, Source, Direction, /*live FRand*/ -1.0f);
	FireProjectile(Invoker, Source, Direction);
}

void UWoTWeaponComponent::FireProjectile(APawn* Invoker, const FVector& Source, const FRotator& Direction)
{
	// WotWeapon.uc:396: Spawn(ProjectileType, Self, , Source, Direction).
	FActorSpawnParameters Params;
	Params.Owner = Invoker;
	Params.Instigator = Invoker;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	GetWorld()->SpawnActor<AWoTProjectile>(ProjectileClass, Source, Direction, Params);
}
