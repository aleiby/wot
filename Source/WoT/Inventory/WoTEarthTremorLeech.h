#pragma once

#include "CoreMinimal.h"
#include "WoTDecreaseHealthLeech.h"
#include "WoTEarthTremorLeech.generated.h"

// Port of Angreal.EarthTremorLeech (expands DecreaseHealthLeech) — the health
// drain attached to a pawn caught by an Earth Tremor eruption. Spec:
// exports/script/Angreal/EarthTremorLeech.uc.
//
// It hides its HUD icon, inherits the 0.2-second health drain, and asks the
// victim's owning controller for the retail roll-only view shake each affect.
UCLASS()
class UWoTEarthTremorLeech : public UWoTDecreaseHealthLeech
{
	GENERATED_BODY()

public:
	UWoTEarthTremorLeech();
	virtual void AffectHost(int32 Iterations = 0) override;
};
