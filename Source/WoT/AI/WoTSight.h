#pragma once

#include "CoreMinimal.h"

class APawn;
class AActor;

struct FWoTLineOfSightParams
{
	bool bShowSelf = false;
	bool bViewerIsPlayer = false;
	float SightRadius = 2500.0f;
	float PeripheralVision = 0.0f;
	float Skill = 0.0f;
	uint8 TargetVisibility = 128;
	const AActor* Enemy = nullptr;
};

namespace WoTSight
{
	bool LineOfSightTo(const APawn* Observer, const AActor* Target,
		const FWoTLineOfSightParams& Params);
	bool ConeSight(const APawn* Observer, float PeripheralVision,
		float SightRadius, const AActor* Target, uint8 TargetVisibility);
	bool OmniSight(const APawn* Observer, const AActor* Target,
		uint8 TargetVisibility);
	bool PawnCanSeeActor(const APawn* Observer, const AActor* Target,
		float FOVCos = 0.0f, bool bTraceActors = false);
	bool TraceToTarget(const APawn* Observer, const AActor* Target);
	uint8 VisibilityOf(const AActor* Target);

#if WITH_AUTOMATION_TESTS
	void SetTraceOverrideForTesting(
		TFunction<bool(const FVector&, const FVector&)> Override);
	void SetRandomOverrideForTesting(TFunction<float()> Override);
	void ResetStateForTesting(const APawn* Observer);
#endif
}
