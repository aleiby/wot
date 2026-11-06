#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTEvents.h"
#include "WoTFragment.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTCollisionBlocker.h"
#include "WoTExplodingWall.generated.h"

// Port of WOTExplodingWall (WOTExplodingWall.uc — the Effects-subclass breakable
// wall, G35), the near-twin of AWoTExplodingMover that is a plain Actor rather
// than a Mover brush. Retail draws nothing (PreBeginPlay forces DrawType=DT_None)
// and blocks nothing (bBlockActors/bBlockPlayers stay false): its live MP
// footprint is PRESENCE — a 32x32 invisible flat cylinder with
// bProjTarget=True that weave projectile overlaps dispatch Touch against. The break path itself is
// ACTIVATION-DEAD in shipped MP (all six authors gate on DamageType 'break01',
// which nothing emits — PORT_NOTES G35), but SP/mission maps and mods reach it,
// so it ports faithfully.
UCLASS()
class AWoTExplodingWall : public AActor, public IWoTTriggerable,
	public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTExplodingWall();

	// The retail 32x32 collision cylinder: a pure projectile Touch target. It
	// overlaps the WoTProjectile object channel (bProjTarget=True); pawns pass
	// through (bBlockActors/bBlockPlayers false — Actor defaults, unset by the
	// .uc and by every MP author).
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTFlatCylinderComponent> Capsule;

	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") float ExplosionSize = 200.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") float ExplosionDimensions = 120.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") float WallParticleSize = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") float WoodParticleSize = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") float GlassParticleSize = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") int32 NumWallChunks = 10;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") int32 NumWoodChunks = 3;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") int32 NumGlassChunks = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") FName WallTextureName;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") FName WoodTextureName;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") FName GlassTextureName;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") int32 Health = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") TArray<FName> ActivatedBy;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") FName BreakingSound;
	// Presentation stubs (uc:107-108 s.bUnlit / STY_Translucent on the glass
	// debris): carried for author fidelity, not yet threaded into AWoTFragment
	// display — same standing gap as the mover port (PORT_NOTES G35).
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bTranslucentGlass = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bUnlitGlass = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bOnlyTriggerable = false;
	UPROPERTY(EditAnywhere, Category = "WoT") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT") FName EventName;

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }

	void Explode(APawn* EventInstigator, const FVector& Momentum);
	bool HasExploded() const { return bExploded; }

private:
	void ApplyOccluderCollision();
	void SpawnFragments(EWoTFragmentType Type, int32 Count, float Scale,
		const FVector& Momentum, FName SkinName);

	// One-shot latch: retail Destroy()s at the end of Explode (uc:112); the latch
	// guards re-entry until the pending kill lands.
	bool bExploded = false;
};
