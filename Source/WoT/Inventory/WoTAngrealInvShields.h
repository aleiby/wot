#pragma once

#include "CoreMinimal.h"
#include "WoTReflectorInstaller.h"
#include "WoTAngrealInvShields.generated.h"

// Ports of the five elemental shield ter'angreal (Angreal.AngrealInv*Shield) —
// thin ReflectorInstallers that install the matching Ignore*ElementReflector on
// the caster for 20s. Specs: exports/script/Angreal/AngrealInv*Shield.uc. Pickup
// mesh (AngrealElementalShield) + icons bind via the leaf-BP / soft-ref pass.

UCLASS()
class AWoTAngrealInvFireShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvFireShield();
};

UCLASS()
class AWoTAngrealInvWaterShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvWaterShield();
};

UCLASS()
class AWoTAngrealInvAirShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvAirShield();
};

UCLASS()
class AWoTAngrealInvEarthShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvEarthShield();
};

UCLASS()
class AWoTAngrealInvSpiritShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvSpiritShield();
};

// The Personal Shield — a hit-point shield (ShieldReflector), permanent until
// worn away by damage. Spec: exports/script/Angreal/AngrealInvShield.uc.
UCLASS()
class AWoTAngrealInvShield : public AWoTReflectorInstaller
{
	GENERATED_BODY()
public:
	AWoTAngrealInvShield();

	// AngrealInvShield.uc:33-43 — Cast() fails (no charge spent) while the
	// installed shield's pool is already full; recasting only replenishes.
	virtual void CastArtifact() override;
	virtual float GetPriority() const override;
};
