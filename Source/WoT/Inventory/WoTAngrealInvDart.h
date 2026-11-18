#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvDart.generated.h"

// Port of Angreal.AngrealInvDart — the Dart ter'angreal (the default artifact).
// Spec: exports/script/Angreal/AngrealInvDart.uc. A thin auto-firing
// ProjectileLauncher: bAutoFire + RoundsPerMinute 320 spray the weak darts while
// the fire key is held (the auto-fire loop is already in AWoTProjectileLauncher).
//
// GiveTo/SetColor's per-player dart variant is folded through WoTPlayerColorFX
// — a cosmetic team tint, folded into the single AWoTDartProjectile. Pickup mesh
// (Angreal.AngrealInventoryDart) + icons bind via the leaf-BP / soft-ref pass.
UCLASS()
class AWoTAngrealInvDart : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvDart();
	void SetColor(FName Color);
	// Retained ProjectileClassName variant. Projectile instances still resolve
	// the owner's current color at fire time, matching GiveTo's later recolor.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Dart")
	FName ConfiguredDartColor = TEXT("Blue");
};
