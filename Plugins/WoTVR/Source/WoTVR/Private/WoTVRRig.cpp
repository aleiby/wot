#include "WoTVRRig.h"

namespace
{
FQuat WoTVRRigYaw(double Yaw)
{
	return FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
}
}

double FWoTVRRig::ForwardYaw(const FQuat& Orientation, double Fallback, bool* bOutValid)
{
	const FVector Forward = Orientation.GetForwardVector();
	const double Horizontal = FVector2D(Forward.X, Forward.Y).Size();
	const bool bValid = Horizontal
		> FMath::Cos(FMath::DegreesToRadians(WoTVR::NearVerticalPitchDegrees));
	if (bOutValid)
	{
		*bOutValid = bValid;
	}
	return bValid
		? FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X))
		: Fallback;
}

double FWoTVRRig::RelativeYaw(const FWoTVRHeadPose& Pose) const
{
	const FQuat Relative = WoTVRRigYaw(-NeutralYaw) * Pose.Orientation;
	return FMath::UnwindDegrees(ForwardYaw(Relative, LastValidRelativeYaw));
}

FVector FWoTVRRig::RelativeOffset(const FWoTVRHeadPose& Pose) const
{
	return ToUnits(WoTVRRigYaw(-NeutralYaw).RotateVector(Pose.PositionMeters - NeutralPositionMeters));
}

FQuat FWoTVRRig::WorldOrientation(const FWoTVRHeadPose& Pose) const
{
	return OrientationUnderHeading(Pose, HeadingYaw);
}

FQuat FWoTVRRig::OrientationUnderHeading(const FWoTVRHeadPose& Pose, double Heading) const
{
	return (WoTVRRigYaw(Heading) * WoTVRRigYaw(-NeutralYaw) * Pose.Orientation).GetNormalized();
}

FVector FWoTVRRig::WorldOffset(const FWoTVRHeadPose& Pose) const
{
	return WoTVRRigYaw(HeadingYaw).RotateVector(RelativeOffset(Pose));
}

double FWoTVRRig::WorldYaw(const FWoTVRHeadPose& Pose) const
{
	return FMath::UnwindDegrees(HeadingYaw + RelativeYaw(Pose));
}

FRotator FWoTVRRig::Aim(const FWoTVRHeadPose& Pose) const
{
	const FVector Forward = WorldOrientation(Pose).GetForwardVector();
	const double Pitch = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Forward.Z, -1.0, 1.0)));
	return FRotator(Pitch, WorldYaw(Pose), 0.0);
}

void FWoTVRRig::NoteValidPose(const FWoTVRHeadPose& Pose)
{
	bool bValid = false;
	const double Yaw = ForwardYaw(WoTVRRigYaw(-NeutralYaw) * Pose.Orientation, LastValidRelativeYaw, &bValid);
	if (bValid)
	{
		LastValidRelativeYaw = FMath::UnwindDegrees(Yaw);
	}
}

void FWoTVRRig::Calibrate(const FWoTVRHeadPose& Pose)
{
	NeutralPositionMeters = Pose.PositionMeters;
	NeutralYaw = FMath::UnwindDegrees(ForwardYaw(Pose.Orientation, NeutralYaw));
	LastValidRelativeYaw = 0.0;
	bCalibrated = true;
}

void FWoTVRRig::ReanchorHorizontal(const FWoTVRHeadPose& Pose)
{
	if (!bCalibrated)
	{
		Calibrate(Pose);
		return;
	}
	NeutralPositionMeters.X = Pose.PositionMeters.X;
	NeutralPositionMeters.Y = Pose.PositionMeters.Y;
}

void FWoTVRRig::AlignHeading(const FWoTVRHeadPose& Pose, double AimYaw)
{
	HeadingYaw = FMath::UnwindDegrees(AimYaw - RelativeYaw(Pose));
}

void FWoTVRRig::Turn(const FWoTVRHeadPose& Pose, double DeltaYaw)
{
	// Turning about the head: the neutral moves so the head's offset, seen
	// through the new heading, lands where it was. Yaw rotations commute, so the
	// calibrated frame and the heading need no other change.
	const FVector FromNeutral = Pose.PositionMeters - NeutralPositionMeters;
	NeutralPositionMeters = Pose.PositionMeters - WoTVRRigYaw(-DeltaYaw).RotateVector(FromNeutral);
	HeadingYaw = FMath::UnwindDegrees(HeadingYaw + DeltaYaw);
}

void FWoTVRRig::Recentre(const FWoTVRHeadPose& Pose)
{
	const double Yaw = WorldYaw(Pose);
	Calibrate(Pose);
	HeadingYaw = Yaw;
}
