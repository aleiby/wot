#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvWhirlwind.generated.h"

// Port of Angreal.AngrealInvWhirlwind — the Whirlwind ter'angreal. Spec:
// exports/script/Angreal/AngrealInvWhirlwind.uc. A non-launcher artifact that,
// on cast, finds the best target in view and installs a UWoTWhirlwindLeech that
// lifts and spins it; while held, the caster steers the victim to a point ahead
// of its view (InitialDistance away). Releasing or running out of charges (or
// losing the leech) stops it.
UCLASS()
class AWoTAngrealInvWhirlwind : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvWhirlwind();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Whirlwind") float MaxAngle = 22.5f;

	virtual void CastArtifact() override;     // Cast()
	virtual void UnCastArtifact() override;   // UnCast()
	virtual void Tick(float DeltaTime) override;

	// True if the pawn currently carries a WhirlwindLeech (Levitate's gate).
	static bool PlayerIsBeingWhirlwinded(APawn* Other);

private:
	UPROPERTY() TObjectPtr<class UWoTWhirlwindLeech> WWLeech;
	bool bCasting = false;
	float InitialDistance = 0.0f;
	float ChargeTimer = 0.0f;
};
