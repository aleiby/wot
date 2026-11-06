#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvIllusion.generated.h"

// Port of Angreal.AngrealInvIllusion: spawn a targetable image of the caster,
// replacing that caster's previous image. Spec:
// exports/script/Angreal/AngrealInvIllusion.uc.
UCLASS()
class AWoTAngrealInvIllusion : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvIllusion();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Illusion")
	FVector SpawnOffset = FVector(48.0f, 0.0f, 0.0f);
	UPROPERTY()
	TObjectPtr<class AWoTAngrealIllusionProjectile> Illusion;

	virtual void CastArtifact() override;
	bool HaveIllusion() const;
	AWoTAngrealIllusionProjectile* GetIllusion() const;
	virtual float GetPriority() const override;
};

