#include "WoTShieldReturnToSenderReflector.h"

#include "../WoTUE1Names.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCWeapon.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"

#include "Engine/DamageEvents.h"
#include "GameFramework/Pawn.h"

UWoTShieldReturnToSenderReflector::UWoTShieldReturnToSenderReflector()
{
	// ShieldReturnToSenderReflector defaultproperties (uc:114-122): bLeadWhenReflecting
	// on; AimWeight/VeloWeight are the deflect-blend weights (index 2 AimWeight unset =
	// 0). Priority 160 + bRemoveExisting are inherited from ReturnToSenderReflector.
	bLeadWhenReflecting = true;
	bDeflect = false;
	AimWeight[0] = 1.0f; AimWeight[1] = 1.0f; AimWeight[2] = 0.0f;
	VeloWeight[0] = 3.0f; VeloWeight[1] = 7.0f; VeloWeight[2] = 1.0f;
}

void UWoTShieldReturnToSenderReflector::OnProjectileFeedback()
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (!Host || !GetWorld() || GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	WoTSound::PlayReplicated(*GetWorld(), ReflectSoundPath(),
		Host->GetActorLocation(), WoTSound::WorldSoundRadius(32));
}

bool UWoTShieldReturnToSenderReflector::BuildTouchReflectorDescription(
	FWoTTouchReflectorDescription& Out) const
{
	if (!Super::BuildTouchReflectorDescription(Out)) return false;
	if (!IrreleventProjectileType.IsNone())
		Out.UnaffectedProjectileTypes.AddUnique(IrreleventProjectileType);
	return true;
}

bool UWoTShieldReturnToSenderReflector::IsAffectable(const AActor* Other) const
{
	return Super::IsAffectable(Other)
		&& (IrreleventProjectileType.IsNone()
			|| !WoTUE1Names::ActorIsA(Other, IrreleventProjectileType));
}

FVector UWoTShieldReturnToSenderReflector::ComputeDeflectVelocity(
	const FVector& InLocation, const FVector& InVelocity,
	const FVector& OwnerLocation, int32 Difficulty) const
{
	// DoAdjustVelocity bDeflect branch (ShieldReturnToSenderReflector.uc:79-92).
	//   AimDir = Normal(Proj.Location - OwnerLocation);
	//   AimDir = AimDir << rotator(-Velocity); AimDir.X = Abs(AimDir.X); AimDir >>= ...;
	//   AimDir = (Aim*AimDir + Velo*Normal(-Velocity)) / (Aim+Velo);
	//   OutVelocity = VSize(Velocity) * AimDir;
	// UE1 `<<`/`>>` are the inverse/forward rotator transforms of a direction vector,
	// i.e. FRotator::UnrotateVector / RotateVector.
	FVector AimDir = (InLocation - OwnerLocation).GetSafeNormal();
	const FRotator RevRot = (-InVelocity).Rotation();
	AimDir = RevRot.UnrotateVector(AimDir);       // << : into the reverse-velocity frame
	AimDir.X = FMath::Abs(AimDir.X);              // always deflect back toward the source
	AimDir = RevRot.RotateVector(AimDir);         // >> : back to world space

	const int32 Idx = FMath::Clamp(Difficulty, 0, 2);
	const float Aim = AimWeight[Idx];
	const float Velo = VeloWeight[Idx];
	const FVector RevVel = (-InVelocity).GetSafeNormal();
	const float Sum = Aim + Velo;
	if (Sum > 0.0f)
	{
		AimDir = (Aim * AimDir + Velo * RevVel) / Sum;
	}
	// Note: the blend is NOT re-normalized (verbatim uc) — the output magnitude is the
	// incoming speed x |blend| (<= incoming speed), not exactly preserved.
	return InVelocity.Size() * AimDir;
}

bool UWoTShieldReturnToSenderReflector::RedirectImpactDamage(AWoTGenericProjectile* HitProjectile)
{
	// RedirectImpactDamage (uc:47-61): Pawn(Owner).SelectedItem (the shield proxy) takes
	// the projectile's damage, and 'Reflected' is triggered on the owner. The port's
	// SelectedItem is the held shield weapon (UseShield equips it; item 3). A missing
	// shield returns false — the shipped reflection gate (OnReflectedTouch, uc:29).
	AWoTNPCCharacter* OwnerPawn = Cast<AWoTNPCCharacter>(GetOwner());
	if (!IsValid(HitProjectile) || !OwnerPawn)
	{
		return false;
	}
	AWoTNPCWeapon* Shield = OwnerPawn->GetHeldWeapon();
	if (!IsValid(Shield))
	{
		return false;
	}
#if WITH_AUTOMATION_TESTS
	++RedirectImpactDamageCountForTest;
#endif
	// TriggerActor(Self, OwnerPawn, None, 'Reflected') (uc:56): the Questioner plays its
	// 'Reflected' voice slot (Questioner.uc:19-27); the Soldier's table has no such slot
	// so PlayVoiceSlot is a quiet no-op — the per-creature split falls out of the data.
	OwnerPawn->PlayVoiceSlot(FName(TEXT("Reflected")));
	// LAST: TakeDamage can destroy the shield, which fires OnWeaponDestroyed ->
	// DisableDeflect -> Uninstall(this). Nothing touches *this after this line.
	FDamageEvent DamageEvent;
	Shield->TakeDamage(static_cast<float>(HitProjectile->Damage), DamageEvent,
		nullptr, HitProjectile);
	return true;
}
