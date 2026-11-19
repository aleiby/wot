#pragma once

#include "CoreMinimal.h"

// Exact critically damped integration for each constant target. A fixed small
// sampling step makes time-varying targets independent of camera frame rate.
struct FWoTFirstPersonSpring
{
	double Value = 0;
	double Velocity = 0;
	void Step(double Target, double DeltaTime);
};

class FWoTFirstPersonMotion
{
public:
	void Step(double DeltaTime, const FRotator& View, double GroundSpeed, double Amount);
	void FirePressed();
	void FireReleased();
	FVector Offset() const;
	FRotator Rotation() const;

private:
	FWoTFirstPersonSpring Float, Bob, SwayPitch, SwayYaw, Kick, Tremble;
	FRotator PreviousView = FRotator::ZeroRotator;
	bool bHasView = false;
	bool bHeld = false;
	double Time = 0;
	double Distance = 0;
	double Remainder = 0;
	double Strength = 1;
};
