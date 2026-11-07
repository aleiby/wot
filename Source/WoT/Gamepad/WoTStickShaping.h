// Pure stick math for the controller layer. No engine state: every input is a
// parameter, so the specs pin the exact numbers.
#pragma once

#include "CoreMinimal.h"

struct FWoTStickShaping
{
	/** 0 inside DeadZone; rescaled to reach 1 at OuterZone; clamped to 1; direction kept. */
	static FVector2D RadialDeadZone(FVector2D Raw, float DeadZone, float OuterZone);
	/** Movement input (X strafe, Y forward): shaped magnitude times Gain, clamped to 1. */
	static FVector2D Move(FVector2D Raw, float DeadZone, float OuterZone, float Gain);
	/** This frame's look in degrees: X yaw (right positive), Y pitch (up positive). */
	static FVector2D LookDegrees(FVector2D Raw, float DeadZone, float OuterZone,
		float Exponent, float YawRate, float PitchRate, float Sensitivity, bool bInvertY,
		float FOVScale, float DeltaSeconds);
};
