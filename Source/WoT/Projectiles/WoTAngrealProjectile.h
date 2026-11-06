#pragma once

#include "CoreMinimal.h"
#include "../WoTDynamicLightSubsystem.h"
#include "WoTProjectile.h"
#include "WoTAngrealProjectile.generated.h"

// Port of WOT.AngrealProjectile (1:1 — the type identity matters: original
// code casts to AngrealProjectile for reflect/fork/NotifyHitByAngrealProjectile).
// Spec: exports/script/WOT/AngrealProjectile.uc.
// ProjTouch: the original's manual 256-slot proximity poll worked around
// UE1's unreliable fast-projectile Touch; reimplemented as a timer +
// sphere-overlap diff feeding the same ProjTouch/ProjUnTouch virtuals.
UCLASS(Abstract)
class AWoTAngrealProjectile : public AWoTProjectile
{
	GENERATED_BODY()

public:

	// 6a-ii display helpers, shared by every weave projectile.
	// UE1 dynamic-light recipe (the Fireball scaffold constants): attenuation =
	// (LightRadius+1) x 25, intensity = Brightness/64 x 5, falloff 2, no
	// shadows; color from the UE1 hue/saturation bytes (Sat 255 = white).
	static void ConfigureUE1Light(class UPointLightComponent* Light,
		uint8 Brightness, uint8 Hue, uint8 Saturation, float Radius,
		EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence,
		EWoTDynLightType Type = EWoTDynLightType::Steady,
		uint8 Period = 0, uint8 Phase = 0);
	// Resolve + bind an imported vertex-anim mesh; Scale is the defaultproperties
	// DrawScale ONLY (the #exec MESHMAP SCALE is baked into the geometry).
	static void BindWeaveMesh(class UWoTVertexAnimComponent* Visual, FName MeshName,
	                          float DrawScale);
	AWoTAngrealProjectile();

	UPROPERTY(Replicated) TObjectPtr<class AWoTAngrealInventory> SourceAngreal;
	UPROPERTY(Replicated) TObjectPtr<APawn> SourcePawn;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> ReflectedSound;

	// ProjTouch proximity system (replication per original: time + radius)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Collision") bool bGenProjTouch = false;
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Collision")
	float ProjTouchTime = 0.0f;
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Collision")
	float ProjCollisionRadius = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Collision") bool bTouchPawnsOnly = false;

	UFUNCTION(BlueprintCallable, Category = "WoT")
	virtual void SetSourceAngreal(class AWoTAngrealInventory* Source);

	// called when a Reflect artifact bounces this projectile
	virtual void Reflected();

	// AngrealProjectile.Touch filter: bProjTarget || (bBlockActors && bBlockPlayers)
	static bool IsTouchTarget(const AActor* Other);

	virtual void ProcessTouch(AActor* Other, FVector HitLocation) {}
	virtual void ProjTouch(AActor* Other) {}
	virtual void ProjUnTouch(AActor* Other) {}

	virtual void Tick(float DeltaTime) override;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UFUNCTION()
	void OnProjectileOverlap(UPrimitiveComponent* Comp, AActor* Other,
	                         UPrimitiveComponent* OtherComp, int32 BodyIndex,
	                         bool bFromSweep, const FHitResult& Hit);

private:
	float ProjTouchTimer = 0.0f;
	TSet<TWeakObjectPtr<AActor>> ProjTouching;
};
