#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTGenericProjectile.generated.h"

// Port of Angreal.GenericProjectile: straight-flying weaves use this base for
// direct-hit and radial damage, while subclasses add specialized effects
// (GenericProjectile.uc:7-8).
// Divergences (PORT_NOTES): DamageEffect/Invokable pipeline replaced by
// direct TakeDamage carrying the original type name until the effects
// framework is ported.
UCLASS(Abstract)
class AWoTGenericProjectile : public AWoTAngrealProjectile
{
	GENERATED_BODY()

public:
	AWoTGenericProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Damage") FName DamageType = "hurt";
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Damage") float DamageRadius = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Damage") bool bExplode = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Damage") bool bHurtsOwner = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Damage") bool bDestroyOnExplode = true;

	// pawn that spawned us — immune until reflected/forked
	UPROPERTY(Replicated) TObjectPtr<APawn> IgnoredPawn;

	// true if we are a Fork duplicate — don't fork again (loop guard)
	UPROPERTY() bool bIsForkCopy = false;

	// launcher support (spawn offset relative to Source Location/Rotation +
	// BaseEyeHeight for pawns; LauncherClass itself is folded into
	// AWoTProjectileLauncher — PORT_NOTES)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launch") FVector LaunchOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launch") bool bUsesDestination = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launch") bool bRequiresDestination = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launch") bool bRequiresLeading = true;
	// max targeting angle for destination-using projectiles (degrees)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Launch") float MaxTargetAngle = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> HitPawnSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> HitWaterSound;

	// UE1 GenericProjectile per-shot PlaySound params: world-unit radii + float
	// pitch (1.0 = normal). GenericProjectile.uc defaults: 2500/2500/2500/1000
	// radii, all pitches 1.0.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float SpawnSoundRadius = 2500.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float SpawnSoundPitch = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float ImpactSoundRadius = 2500.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float ImpactSoundPitch = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float HitPawnSoundRadius = 2500.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float HitPawnSoundPitch = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float HitWaterSoundRadius = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") float HitWaterSoundPitch = 1.0f;

	virtual void BeginPlay() override;               // ServerInit: velocity from rotation
	// Seed the client's PMC from the initial replicated velocity so a
	// bNetTemporary sim proxy flies the trajectory the server launched — UE1's
	// ROLE_SimulatedProxy + native Velocity replication (GenericProjectile.uc:449).
	// UE5 delivers the velocity in the bNetTemporary burst as
	// ReplicatedMovement.LinearVelocity but never feeds the PMC; this bridges it.
	// The server/authority path (velocity-set in BeginPlay) is unchanged.
	void ApplyReplicatedInitialVelocity();
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;

	// Read-only observation seam for the packaged BSP proof harness: NotifyHit
	// explodes and tears the actor down before the engine's OnActorHit
	// broadcast can reach an external listener, so the harness binds here.
	// Broadcast with the exact FHitResult gameplay consumed; never gates or
	// reorders any gameplay effect. Unbound outside proof runs.
	DECLARE_MULTICAST_DELEGATE_TwoParams(
		FWoTProjectileImpactProbe, AActor*, const FHitResult&);
	static FWoTProjectileImpactProbe OnImpactProbe;

	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	virtual void ProcessDamage(AActor* Other, float GivenDamage,
	                           APawn* GivenInstigator, FVector HitLocation,
	                           FVector GivenMomentum);
	virtual void Explode(FVector HitLocation, FVector HitNormal);

	// The impact decal class (gore 4e; NAME_None = none). Spawned LOCALLY inside
	// Explode on every machine — the client-simulated detonation is exactly where
	// UE1's RemoteRole=None decals came from (Fireball/ExpWard ScorchMark).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName ImpactDecalType;
	// The caller-set decal DrawScale (> 0 overrides the decal class default):
	// BurnMark.DrawScale = 2.0 (AngrealFireballProjectile.uc:152) and
	// ScorchSize 2.5 (AngrealExpWardProjectile.uc:140, :269).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float ImpactDecalDrawScale = -1.0f;
	// The impact burst (6a-iii; NAME_None = none): a WoTWeaveFX explosion row
	// (FireballExplode3/GreenExplode/DecayExplosion), spawned locally in Explode
	// beside the decal — every machine runs its own.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile")
	FName ExplosionEffectName;
	// Retail HitWaterClass, expressed as a registry key. Explosion rows take
	// precedence; otherwise this resolves as a sprayer bundle.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") FName HitWaterEffectName;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Projectile") FVector HitWaterEffectOffset = FVector::ZeroVector;
	void ProcessSplashDamage();
	// Persistent-channel client teardown: mirror the server detonation cosmetics
	// when the server Destroy() propagates over an open (non-bNetTemporary) channel.
	virtual void Destroyed() override;
	// GenericProjectile.HitWater: fired when the weave enters a water volume —
	// spawns the HitWaterClass fizzle and plays HitWaterSound locally. Called by
	// AWoTZoneVolume on every machine (the original's simulated ZoneChange path).
	virtual void HitWater();

	void SetIgnoredPawn(APawn* GivenPawn) { IgnoredPawn = GivenPawn; }
	// The base straight-flight projectile ignores destination assignment
	// (GenericProjectile.uc:149-153).
	virtual void SetDestination(AActor* Dest) {}
	// override for special cases (explosive wards fail off the ground)
	virtual bool CreationSucceeded() { return true; }

	// AI support keeps the source's approximate trigonometric lead calculation
	// (GenericProjectile.uc:360-386).
	virtual FRotator CalculateTrajectory(const AActor* Source,
		const AActor* Destination, float ProjectileSpeed) const;
	// Class defaults supply maximum travel and minimum safe launch distances
	// (GenericProjectile.uc:395).
	static float GetMaxRange(TSubclassOf<AWoTGenericProjectile> Class);
	static float GetMinRange(TSubclassOf<AWoTGenericProjectile> Class);

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Set once the weave has hit water (HitWater ran at least once).
	UPROPERTY() bool bHitWater = false;

	// UE1 ULevel::SetActorZone for a weave: the zone is
	// the one containing the actor's Location POINT, re-evaluated after every move
	// and SetLocation and once after spawn, whatever the collision settings. A
	// change INTO a water zone fires ZoneChange -> HitWater
	// (GenericProjectile.uc:347-353), so crossing water/air/water splashes each
	// time. Called once per frame for every weave by UWoTZoneIndex (after
	// movement), before a wall hit is processed (UE1 sets the zone at the end of
	// MoveActor, before the caller's HitWall), and by subclasses after a
	// placement SetLocation (Explosive Ward).
	void UpdateWaterZone();

protected:
	// Preserve the actor/component classification before ProcessDamage can destroy
	// the impact target. Fireball uses this to run the retail non-pawn aftermath
	// even when a fatal direct hit retires a breakable during ProcessDamage.
	virtual void CaptureImpactContext(AActor* Other,
		const UPrimitiveComponent* OtherComponent) {}
	UPROPERTY() TObjectPtr<AActor> HitActor;   // don't splash the direct hit twice
	// Exact primitive supplied by NotifyHit. Materialized BSP actors deliberately
	// own a scene root and colliding child meshes, so actor-root inspection loses
	// the retail "Level geometry" decision.
	UPROPERTY() TObjectPtr<UPrimitiveComponent> HitPrimitive;

private:
	// The water zone containing the weave's Location at the last UpdateWaterZone.
	TWeakObjectPtr<class AWoTZoneVolume> CurrentWaterZone;
};
