#pragma once

#include "CoreMinimal.h"
#include "WoTFragment.h"
#include "WoTMover.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTExplodingMover.generated.h"

UCLASS()
class AWoTExplodingMover : public AWoTMover, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTExplodingMover();

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
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bTranslucentGlass = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bUnlitGlass = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Explosion") bool bOnlyTriggerable = false;

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void BeginPlay() override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void Explode(APawn* EventInstigator, const FVector& Momentum);
	bool HasExploded() const { return bExploded; }

protected:
	UPROPERTY(ReplicatedUsing = OnRep_Exploded) bool bExploded = false;
	UFUNCTION() void OnRep_Exploded();

private:
	bool AcceptsDamageType(FName DamageType) const;
	void ApplyExplodedPresentation();
	void SpawnFragments(EWoTFragmentType Type, int32 Count, float Scale,
		const FVector& Momentum, FName SkinName);
};
