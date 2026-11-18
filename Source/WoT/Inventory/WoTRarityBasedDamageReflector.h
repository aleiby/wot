#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTRarityBasedDamageReflector.generated.h"

// Port of Angreal.RarityBasedDamageReflector (expands Reflector) — Soul Barb's
// payload. Spec: exports/script/Angreal/RarityBasedDamageReflector.uc. While
// installed, the host takes self-inflicted damage every time it uses a charge,
// scaled by the used artifact's rarity (TAINT_{COMMON,UNCOMMON,RARE}_DAMAGE).
UCLASS()
class UWoTRarityBasedDamageReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTRarityBasedDamageReflector();

	virtual void OnChargeUsed(class AWoTAngrealInventory* Ang) override;
};
