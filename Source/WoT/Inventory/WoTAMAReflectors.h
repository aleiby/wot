#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTAMAReflectors.generated.h"

// Ports of Angreal.NoCastReflector + Angreal.NoBadEffectsReflector — the pair the
// anti-magic dome (AMAPearl) installs on everyone inside it. Specs:
// exports/script/Angreal/NoCastReflector.uc, NoBadEffectsReflector.uc. Both run at
// Priority 255 (they win over any other reflector).

// NoCastReflector: aborts the host's UseAngreal/CeaseUsingAngreal — while it's
// installed the pawn simply can't cast (folded into InventoryComponent's cast
// path via AWoTCharacter::IsCastingBlocked).
UCLASS()
class UWoTNoCastReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTNoCastReflector() { Priority = 255; }
	virtual bool BlocksCasting() const override { return true; }
};

// NoBadEffectsReflector: blocks every deleterious incoming effect (ProcessEffect
// throws away bad Invokables). Non-removable (a cleanse can't strip the dome's
// protection). Folds into AWoTCharacter::IsEffectBlocked via the bDeleterious flag.
UCLASS()
class UWoTNoBadEffectsReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTNoBadEffectsReflector()
	{
		Priority = 255;
		bRemovable = false;   // NoBadEffectsReflector.bRemovable=False
		bDisplayIcon = false; // NoBadEffectsReflector.uc:32
	}
	virtual bool IgnoresEffect(const AWoTAngrealInventory* /*Source*/,
	                           bool bEffectDeleterious) const override
	{
		return bEffectDeleterious;
	}
	// NoBadEffectsReflector overrides ONLY ProcessEffect — it has no TakeDamage
	// interception. Weave damage is blocked at the delivery layer (a DamageEffect
	// routed through ProcessEffect consults IgnoresEffect above before TakeDamage);
	// direct TakeDamage sources — pain zones, falling — still hurt inside a dome.
};
