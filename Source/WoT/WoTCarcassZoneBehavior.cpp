#include "WoTCarcassZoneBehavior.h"

#include "Projectiles/WoTProjectile.h"
#include "WoTSoundTable.h"
#include "WoTZoneVolume.h"

#include "Components/CapsuleComponent.h"

void WoTCarcassZoneBehavior::CheckZone(AActor& Actor, AWoTZoneVolume* Zone,
	bool bTakesDamage, float PainZoneDamageScale, float DeltaSeconds)
{
	if (!Actor.HasAuthority() || !bTakesDamage || !IsValid(Zone)) return;
	float Damage = 0.0f;
	FWoTDamageEvent Event;
	if (Zone->bDestructive)
	{
		Damage = 100.0f;
		Event.WoTDamageType = Zone->DamageType;
	}
	else if (Zone->bPainZone)
	{
		Damage = Zone->DamagePerSec * DeltaSeconds * PainZoneDamageScale;
		Event.WoTDamageType = TEXT("PainZone");
	}
	if (Damage > 0.0f)
	{
		Event.Damage = FMath::RoundToInt(Damage);
		Event.HitInfo.ImpactPoint = Actor.GetActorLocation();
		Actor.TakeDamage(Damage, Event, nullptr, Zone);
	}
}

void WoTCarcassZoneBehavior::HandleZoneChange(AActor& Actor,
	UCapsuleComponent* Capsule, FVector& Velocity, FRotator& RotationRate,
	float Mass, float Buoyancy, bool bTakesDamage, float PainZoneDamageScale,
	bool& bSplash, bool& bWaterCollisionFlattened, AWoTZoneVolume* NewZone,
	AWoTZoneVolume* OldZone)
{
	const bool bOldWater = IsValid(OldZone) && OldZone->bWaterZone;
	if (IsValid(NewZone) && NewZone->bWaterZone)
	{
		if (Mass <= Buoyancy && Capsule)
		{
			// UE5 capsules cannot express UE1 SetCollisionSize(0,0); disabling
			// collision has the same gameplay result for the floating body.
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			bWaterCollisionFlattened = true;
		}
		if (bSplash && !bOldWater && FMath::Abs(Velocity.Z) < 80.0f)
		{
			RotationRate *= 0.6f;
		}
		else if (!bOldWater && Velocity.Z < -200.0f)
		{
			const float SplashSize = FMath::Clamp(
				0.0001f * Mass
					* (250.0f - 0.5f * FMath::Max(-600.0f, Velocity.Z)),
				1.0f, 3.0f);
			if (Actor.HasAuthority() && !NewZone->EntrySoundSubPath.IsNone())
			{
				WoTSound::PlayReplicated(*Actor.GetWorld(),
					NewZone->EntrySoundSubPath, Actor.GetActorLocation(), 1600.0f,
					SplashSize);
			}
		}
		bSplash = true;
	}
	CheckZone(Actor, NewZone, bTakesDamage, PainZoneDamageScale, 1.0f);
}
