#pragma once

#include "CoreMinimal.h"
#include "WoTReturnToSenderReflector.h"
#include "WoTShieldReturnToSenderReflector.generated.h"

// Port of WoTPawns.ShieldReturnToSenderReflector (expands ReturnToSenderReflector)
// — the reflector a ShieldUser (Soldier/Questioner) installs while its shield is up.
// Spec: exports/script/WoTPawns/ShieldReturnToSenderReflector.uc.
//
// It is a plain ReturnToSender in the port: while installed, ReflectsProjectiles()
// is true, so AWoTGenericProjectile::ProcessTouch bounces an incoming weave back at
// its originator and the host takes no hit (the deflect). The three config fields
// (ShieldReturnToSenderReflector.uc:14-16) are carried verbatim; ProcessTouch asks
// the reflector about the concrete projectile before dispatch:
//   - bDeflect            Soldier true: knock the weave away from the direction it
//                         came (DoAdjustVelocity, uc:79-93) vs Questioner's reflect.
//   - bLeadWhenReflecting Questioner true: reflect with leading (uc:98-101) — this is
//                         the path ProcessTouch already reproduces (re-aim at the
//                         attacker with leading).
//   - IrreleventProjectileType  Soldier 'SeekingProjectile': these pass through
//                         un-deflected (uc:37-40).
UCLASS()
class UWoTShieldReturnToSenderReflector : public UWoTReturnToSenderReflector
{
	GENERATED_BODY()

public:
	UWoTShieldReturnToSenderReflector();

	// ShieldReturnToSenderReflector.uc:14-16 (carried verbatim; see the class note).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	FName IrreleventProjectileType;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	bool bDeflect = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	bool bLeadWhenReflecting = true;

	// DoAdjustVelocity's difficulty-indexed deflect-blend weights (uc:21-22, CDO
	// :117-121). Larger Aim/Velo ratios deflect more; smaller reflect more.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float AimWeight[3];
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	float VeloWeight[3];

	// ShieldUser.ShieldReflectorSoundName override onto the reflect sound path
	// (Soldier "WOTPawns.Shield_Deflect1", Soldier.uc:48; the Questioner keeps the
	// base). NAME_None = the ReturnToSender default (WoTReturnToSenderReflector.h:66).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Shield")
	FName ReflectSoundOverride;

	bool IsAffectable(const AActor* Other) const;
	virtual bool IsProjectileAffectable(const AActor* Proj) const override
	{
		return IsAffectable(Proj);
	}
	virtual bool BuildTouchReflectorDescription(
		FWoTTouchReflectorDescription& Out) const override;
	virtual FName ReflectSoundPath() const override
	{
		return ReflectSoundOverride.IsNone() ? Super::ReflectSoundPath() : ReflectSoundOverride;
	}
	// ShieldReturnToSenderReflector's shipped impact feedback is sound-only;
	// unlike the player Reflect artifact it does not spawn ReflectSkinEffect.
	virtual void OnProjectileFeedback() override;

	// MP has no difficulty setting; GameInfo default Difficulty=1 (Engine
	// GameInfo; no MP game type overrides it) — index 1 (AimWeight 1,
	// VeloWeight 7). Carried as the fixed MP index (PORT_NOTES).
	static int32 MPDifficulty() { return 1; }

	// DoAdjustVelocity bDeflect branch (uc:79-92): the deflect-away velocity — knock
	// the weave back toward the side it came from, blended toward -Velocity by the
	// difficulty weights, at the incoming speed x the blend magnitude.
	FVector ComputeDeflectVelocity(const FVector& InLocation, const FVector& InVelocity,
		const FVector& OwnerLocation, int32 Difficulty) const;

	// RedirectImpactDamage (uc:47-61): route the deflected weave's damage into the
	// host's held shield weapon and fire the 'Reflected' trigger (Questioner voice;
	// the Soldier's table has no such slot → silent). Returns true when a live shield
	// took the damage (the shipped reflection gate). May destroy the shield.
	bool RedirectImpactDamage(class AWoTGenericProjectile* HitProjectile);
#if WITH_AUTOMATION_TESTS
	int32 RedirectImpactDamageCountForTest = 0;
#endif
};
