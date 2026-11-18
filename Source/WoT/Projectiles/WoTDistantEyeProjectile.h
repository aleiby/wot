#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTDistantEyeProjectile.generated.h"

// Port of Angreal.AngrealDistantEyeProjectile: a permanent, stationary,
// shootable remote camera. Open/Close own the cast reflector and owner-view
// leech; destroying the eye is the sole charge-spending event.
UCLASS()
class AWoTDistantEyeProjectile : public AWoTAngrealProjectile
{
	GENERATED_BODY()

public:
	AWoTDistantEyeProjectile();

	UPROPERTY(VisibleAnywhere, Category = "WoT|DistantEye")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(ReplicatedUsing = OnRep_Open, VisibleAnywhere,
		Category = "WoT|DistantEye")
	bool bOpen = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	int32 Health = 50;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	float FrameTime = 0.2f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	float EyeRoundsPerMinute = 320.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	TArray<TSoftObjectPtr<class UTexture2D>> AnimSkins;

	void Open();
	void Close();
	void Fire();
	void UnFire();
	bool IsFiring() const { return bFiring; }
	int32 GetCurrentSkinIndexForTest() const { return CurrentSkinIndex; }

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION() void OnRep_Open();
	void InstallViewingEffects();
	void RemoveViewingEffects();
	void LaunchDart();
	void ApplySkinFrame(int32 Index);

	UPROPERTY() TObjectPtr<class UWoTDistantEyeCastReflector> CastReflector;
	UPROPERTY() TObjectPtr<class UWoTDistantEyeLeech> ViewLeech;
	bool bFiring = false;
	bool bDestroyedHandled = false;
	int32 CurrentSkinIndex = 0;
	int32 TargetSkinIndex = 0;
	float FrameAccumulator = 0.0f;
	float NextFireTime = 0.0f;
};
