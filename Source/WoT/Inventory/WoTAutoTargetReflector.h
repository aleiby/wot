#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTAutoTargetReflector.generated.h"

// Port of Angreal.AutoTargetReflector (expands DefaultTargetingReflector) — the
// Find Target effect. Spec: exports/script/Angreal/AutoTargetReflector.uc. While
// installed it overrides the host's targeting so seeking weaves lock onto the
// nearest reachable retail target candidate, ignoring FOV and line of sight. A marker reflector:
// the actual nearest-reachable pick lives in AWoTAngrealInventory::FindBestTarget
// (guarded by AWoTCharacter::HasTargetingOverride). Reachability tries the
// retail world-geometry-only direct trace, then the live-selected retail path
// nodes (default) or synchronous UE5 NavMesh A/B arm.
UCLASS()
class UWoTAutoTargetReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTAutoTargetReflector()
	{
		Priority = 128;
		bRemoveExisting = true;
	}

	virtual bool OverridesTargeting() const override { return true; }
};
