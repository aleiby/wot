#include "WoTFirstPersonFraming.h"
#include "WoTFirstPersonTuning.h"

namespace WoTFirstPerson
{
namespace
{
const FPose Poses[] = {
#include "Generated/WoTHeldArtifactPoses.inl"
};
}

double ShownNearPlane()
{
	return FMath::Min<double>(GNearClippingPlane, Tuning::ShownNearPlane);
}

const FPose& FindPose(FName Class)
{
	for (const FPose& Pose : Poses)
		if (Class == FName(Pose.Class)) return Pose;
	// The generator requires one default and sorts it first.
	return Poses[0];
}

FFit Fit(const FBox& Bounds, const FPose& Pose, double CapsuleRadius,
	double NearPlane, double MotionAmount)
{
	FFit Result;
	const double NearLimit = NearPlane + Tuning::NearMargin;
	const double FarLimit = CapsuleRadius - Tuning::WallMargin;
	if (!Bounds.IsValid || Bounds.GetExtent().GetMax() <= UE_SMALL_NUMBER
		|| !FMath::IsFinite(FarLimit) || FarLimit <= NearLimit || Pose.Offset.X <= 0 || Pose.ScaleMultiplier <= 0)
		return Result;
	// Keep strict inequalities without consuming a narrow, but valid corridor.
	const double Epsilon = FMath::Min(Tuning::ClearanceEpsilon, (FarLimit - NearLimit) * 0.1);
	const double Near = NearLimit + Epsilon;
	const double Far = FarLimit - Epsilon;
	const double BaseScale = Tuning::TargetExtent / Bounds.GetExtent().GetMax() * Pose.ScaleMultiplier;
	const double DepthExtent = Bounds.GetExtent().Size() * BaseScale
		+ Tuning::MotionDepthExtent * FMath::Clamp(MotionAmount, 0.0, 1.0);
	Result.Pull = (Near + Far) / (2.0 * Pose.Offset.X);
	const double AvailableExtent = (Far - Near) / (2.0 * Result.Pull);
	Result.Shrink = FMath::Min(1.0, AvailableExtent / DepthExtent);
	Result.ModelScale = BaseScale * Result.Shrink;
	Result.MinDepth = Pose.Offset.X - DepthExtent * Result.Shrink;
	Result.MaxDepth = Pose.Offset.X + DepthExtent * Result.Shrink;
	Result.bVisible = true;
	return Result;
}
}
