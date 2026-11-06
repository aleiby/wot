#pragma once

#include "CoreMinimal.h"
#include "WoTDecoration.h"
#include "WoTBounceableDecoration.generated.h"

// WOT.BounceableDecoration: skull/chunk/book props that enter Animate when
// damaged, tumble under gravity, and settle after damped world bounces.
UCLASS()
class AWoTBounceableDecoration : public AWoTDecoration
{
	GENERATED_BODY()

public:
	AWoTBounceableDecoration();
	virtual void ConfigureRuntimeCollision(bool bInCollideActors,
		bool bInBlockActors, bool bInBlockPlayers, float Radius,
		float HalfHeight) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void NotifyActorBeginOverlap(AActor* Other) override;

	void SetupBounce(const FVector& InVelocity);
	void AddBounceVelocity(const FVector& AddedVelocity);
	bool IsBouncing() const { return bAnimating; }
	FVector GetBounceVelocity() const { return BounceVelocity; }

	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float Damage = 0.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float MomentumTransfer = 0.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") FName DamageType = TEXT("hurt");
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float Mass = 1.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") FName LandSound1;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float MinBounceSoundSpeed = 10.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float MinFirstHitSpeed = 400.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float MinBounceSpeed = 10.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float SpeedDampeningRatio = 0.33f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float RotationDampeningRatio = 0.75f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float VolumePerturbPercent = 30.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float PitchPerturbPercent = 30.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float BaseBounceVolume = 1.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float BaseBouncePitch = 1.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float LandedYaw = -1.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float LandedRoll = -1.0f;
	UPROPERTY(EditAnywhere, Category="WoT|Bounce") float LandedPitch = -1.0f;

private:
	void ApplyWorldCollision();
	void HitWorld(const FVector& HitNormal, float Speed);
	void Settle(const FVector& HitNormal);
	FVector BounceVelocity = FVector::ZeroVector;
	FRotator SpinRate = FRotator::ZeroRotator;
	bool bAnimating = false;
	bool bFirstHit = false;
};
