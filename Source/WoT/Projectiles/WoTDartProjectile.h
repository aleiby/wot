#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "WoTDartProjectile.generated.h"

// Port of Angreal.AngrealDartProjectile (expands GenericProjectile, abstract) —
// the Dart ter'angreal's projectile. Spec: exports/script/Angreal/
// AngrealDartProjectile.uc. A fast, weak direct-damage dart (Damage 10, no
// splash) — the base GenericProjectile ProcessDamage/Explode path handles the
// hit, so this is just defaultproperties.
//
// The original's five per-player color variants (Blue/Red/Green/Purple/Yellow
// Dart, chosen in AngrealInvDart.GiveTo from PlayerColor/mesh) are cosmetic team
// tints of this same class — folded to one concrete here; the color selection is
// selected through the shared player-color seam. Bound: spin (Tick), the
// FakeCorona glow, and the DartExplosion impact visual.
UCLASS()
class AWoTDartProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:

	// The dart display (AngrealDartProjectile.uc:121-127): xboxdart at DrawScale
	// 3.0, translucent and fullbright through ScaleGlow/AmbientGlow/bUnlit.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Transient) TObjectPtr<class AWoTWeaveSpriteEffect> CoronaLight;
	UPROPERTY(ReplicatedUsing = OnRep_VariantColor, VisibleAnywhere, Category = "WoT|Dart")
	FName VariantColor = TEXT("Blue");
	UFUNCTION() void OnRep_VariantColor();
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Dart") int32 SpinRate = 200000;
	AWoTDartProjectile();

private:
	void ApplyVariantPresentation();
	float AccumRollUnits = 0.0f;
};

UCLASS()
class AWoTDartExplosionEffect : public AActor
{
	GENERATED_BODY()
public:
	AWoTDartExplosionEffect();
	void Configure(FName Color, float Hue);
	virtual void Tick(float DeltaSeconds) override;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UPointLightComponent> Light;
	float FadeTimer = 0.25f;
	float LightFadeTimer = 0.4f;
	float ScaleTimer = 0.0f;
	float FullLightIntensity = 0.0f;
};
