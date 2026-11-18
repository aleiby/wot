#pragma once

#include "CoreMinimal.h"

namespace WoTFirstPerson
{
struct FPose
{
	const TCHAR* Class;
	FVector Offset;
	FRotator Rotation;
	float ScaleMultiplier;
};

const FPose& FindPose(FName Class);

struct FFit
{
	bool bVisible = false;
	double ModelScale = 0;
	double Shrink = 0;
	// Applied to the actor's real camera-space translation and scale; the
	// camera modifier leaves the engine's FirstPersonScale at one.
	double Pull = 0;
	double MinDepth = 0;
	double MaxDepth = 0;
};

// Bounds are in actor space, including each primitive's relative transform.
// The sphere about their center conservatively covers every pose rotation and
// all angular spring extremes. Translation extremes shrink with the model.
FFit Fit(const FBox& Bounds, const FPose& Pose, double CapsuleRadius,
	double NearPlane, double MotionAmount);

// The near plane the flat mode applies while a model shows: the engine's
// plane lowered to Tuning::ShownNearPlane, never raised.
double ShownNearPlane();
}
