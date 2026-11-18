#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTAngrealIllusionProjectile.generated.h"

// Port of WOT.AngrealIllusionProjectile: a stationary, targetable image of its
// caster that advertises itself to seeker projectiles. Spec:
// exports/script/WOT/AngrealIllusionProjectile.uc.
UCLASS()
class AWoTAngrealIllusionProjectile : public AWoTAngrealProjectile
{
	GENERATED_BODY()

public:
	AWoTAngrealIllusionProjectile();

	UPROPERTY(VisibleAnywhere, Category = "WoT|Illusion")
	TObjectPtr<class UCapsuleComponent> ImageCollision;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Illusion")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;

	UPROPERTY(Replicated, EditAnywhere, Category = "WoT|Illusion")
	uint8 Team = 255;
	UPROPERTY(ReplicatedUsing = OnRep_ReplicatedOwner)
	TObjectPtr<AActor> ReplicatedOwner;
	// IllusionTimerLeech.uc: the caster-owned countdown producer. Its live
	// duration is initialized from this projectile's remaining lifespan.
	UPROPERTY() TObjectPtr<class UWoTIllusionTimerLeech> Timer;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Illusion")
	float FadePercent = 0.1f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Illusion")
	bool bAnimate = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Illusion")
	float ShowSelfResolution = 0.0f;

	void AcquireImage(APawn* Image);
	void MyShowSelf();
	// AngrealIllusionProjectile.Explode is intentionally empty (uc:200).
	void Explode(FVector /*HitLocation*/, FVector /*HitNormal*/) {}

	virtual void SetSourceAngreal(class AWoTAngrealInventory* Source) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UFUNCTION() void OnRep_ReplicatedOwner();

private:
	void RemoveTimerIcon();
	float NextShowSelfTime = 0.0f;
};
