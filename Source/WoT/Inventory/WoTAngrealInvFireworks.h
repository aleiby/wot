#pragma once

#include "CoreMinimal.h"
#include "WoTProjectileLauncher.h"
#include "WoTAngrealInvFireworks.generated.h"

// Port of Angreal.AngrealInvFireworks — the Fireworks ter'angreal. Spec:
// exports/script/Angreal/AngrealInvFireworks.uc. An unfinished dev artifact: its
// shipped defaultproperties are a Fireball copy with placeholder Title/Description
// ("Fireball"/"None") and the Fireball pickup assets, only the projectile swapped.
// Ported verbatim for checklist completeness.
UCLASS()
class AWoTAngrealInvFireworks : public AWoTProjectileLauncher
{
	GENERATED_BODY()

public:
	AWoTAngrealInvFireworks();
};
