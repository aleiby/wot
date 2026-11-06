#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "../WoTRetailProjectileTarget.h"
#include "Engine/DamageEvents.h"
#include "WoTProjectile.generated.h"

class UPrimitiveComponent;
class USceneComponent;

// Damage event preserving the original UnrealScript damage-type name
// ('hurt', 'xxFxx', 'burned'...) through the UE5 damage plumbing.
USTRUCT()
struct FWoTDamageEvent : public FPointDamageEvent
{
	GENERATED_BODY()

	FName WoTDamageType;
	// Pawn.TakeDamage's incoming UnrealScript momentum, before its walking-floor,
	// self-hit, and Mass folds (Engine.Pawn.TakeDamage; checked against the
	// retail bytecode).
	FVector UE1Momentum = FVector::ZeroVector;

	static const int32 ClassID = 0x57F0;
	virtual int32 GetTypeID() const override { return ClassID; }
	virtual bool IsOfType(int32 InID) const override
	{
		return InID == ClassID || FPointDamageEvent::IsOfType(InID);
	}
};

UCLASS()
class WOT_API UWoTProjectileMovementComponent : public UProjectileMovementComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|Projectile")
	bool bUseRetailFallingGravity = false;
	// PHYS_Falling dispatches HitWall/Landed before its native continuation.
	// BurningChunk uses this to keep the handler-authored velocity instead of
	// applying UProjectileMovementComponent's second, unrelated bounce fold.
	UPROPERTY(EditAnywhere, Category = "WoT|Projectile")
	bool bOwnerHandlesBlockingHits = false;
	virtual FVector ComputeAcceleration(const FVector& InVelocity,
		float DeltaTime) const override;

protected:
	virtual EHandleBlockingHitResult HandleBlockingHit(const FHitResult& Hit,
		float TimeTick, const FVector& MoveDelta,
		float& SubTickTimeRemaining) override;
};

// Port of Engine.Projectile + WOT.WotProjectile (thin abstract, collapsed —
// PORT_NOTES). Spec: exports/script/WOT/WotProjectile.uc + the
// original engine's Projectile class. PHYS_Projectile → ProjectileMovement with no
// gravity; bNetTemporary + replicated initial velocity.
UCLASS(Abstract)
class AWoTProjectile : public AActor, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float Speed = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float MaxSpeed = 2000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") float Damage = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") int32 MomentumTransfer = 0;

	// UE1 Actor.bProjTarget ("can be shot"): projectile touches accept it
	// (AngrealProjectile.uc:84) and splash hurts it (GenericProjectile.uc:310)
	// even when it blocks nothing — e.g. an armed ExpWard. False per
	// Actor/Projectile defaults.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") bool bProjTarget = false;
	virtual bool IsRetailProjectileTarget() const override { return bProjTarget; }

	// UE1 Actor display lighting knobs, consumed by the runtime mesh-light
	// bake (UWoTMeshLight reads them by reflection). Defaults per Engine.Actor;
	// weave classes that author them set CDO values in their constructors.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float ScaleGlow = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float AmbientGlow = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bUnlit = false;
	// UE1 Actor.Style=STY_Translucent. Runtime mesh-light binding reads this by
	// reflection beside bUnlit so actor Style, not an Interchange fallback's
	// authored blend mode, selects the translucent mesh master.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bStyleTranslucent = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	TObjectPtr<class USphereComponent> Collision;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	TObjectPtr<class UWoTFlatCylinderComponent> CylinderCollision;

	// UE1's projectile collision is an independently-authored cylinder. The
	// zero/zero inherited default remains a point sweep; authored nonzero pairs
	// use the project's flat-ended cylinder so radius and height stay independent.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	float UE1CollisionRadius = 32.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	float UE1CollisionHeight = 32.0f;
	void SetUE1CollisionSize(float Radius, float Height);
	UPrimitiveComponent* GetActiveSweptCollision() const;
	void AlignPresentationToVelocity(const FVector& Velocity);
	USphereComponent* GetProjectilePresentationRoot() const { return Collision.Get(); }

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	TObjectPtr<class UProjectileMovementComponent> ProjectileMovement;

	// asset refs come from generated leaf BPs / data assets (ground rule 2)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> SpawnSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> ImpactSound;
	// UE1 AmbientSound: looping positional loop while the projectile lives
	// (the wave imports as looping — import_sounds.py loop set)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> AmbientLoopSound;

	// UE1 Actor byte sound params driving the AmbientSound loop's attenuation
	// (WorldSoundRadius = 25*(SoundRadius+1)), volume (/255), pitch (/64).
	// Engine.Actor defaults 32/128/64.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") uint8 SoundRadius = 32;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") uint8 SoundVolume = 128;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") uint8 SoundPitch = 64;

	// The live flight-hum loop (created at BeginPlay on machines with listeners
	// when AmbientLoopSound is set). Public for specs.
	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> AmbientHum;

	// UE1 AmbientSound=None (a stuck weapon projectile goes quiet —
	// WotWeaponProjectile.uc:150).
	void StopAmbientHum();

	virtual void BeginPlay() override;
	virtual void OnRep_ReplicatedMovement() override;
};
