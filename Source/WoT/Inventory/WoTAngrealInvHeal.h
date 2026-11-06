#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvHeal.generated.h"

// Port of Angreal.AngrealInvHeal — the Heal ter'angreal. Spec:
// exports/script/Angreal/AngrealInvHeal.uc. A non-launcher artifact: each cast
// raises the caster's health by HealingAmount (IncreaseHealthEffect inlined as
// AWoTCharacter::IncreaseHealth), unless already at full — then it does nothing
// and spends no charge.
UCLASS()
class AWoTAngrealInvHeal : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvHeal();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Heal") int32 HealingAmount = 10;

	virtual void CastArtifact() override;   // Cast()
};
