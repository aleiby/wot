#pragma once

#include "CoreMinimal.h"
#include "WoTVRSession.h"

namespace WoTVR
{
	/** World units per metre (design section 6, confirmed in the headset): the
	 * only conversion from tracked metres, applied once. 0.10 m is 5.25 units. */
	inline constexpr double UnitsPerMeter = 52.5;
	/** Head pitch, in degrees, beyond which the forward direction's yaw is
	 * unstable: a head looking nearly straight up or down keeps the last valid
	 * yaw for aim and movement. */
	inline constexpr double NearVerticalPitchDegrees = 85.0;
}

/**
 * The rig's pose arithmetic (design sections 6 and 9), free of worlds and XR so
 * specs can check it directly.
 *
 * Tracking space holds the raw head pose in metres. Calibration confirms a
 * neutral pose there (session-local): its position becomes the anchor's eye,
 * its yaw becomes forward. The artificial heading rotates the calibrated frame
 * into the world; turning changes only the heading (and, to turn about the
 * current head, the neutral position). The rendered centre eye is
 *
 *     Anchor + R(Heading) * R(-NeutralYaw) * (Head - Neutral) * UnitsPerMeter
 *
 * and its orientation R(Heading) * R(-NeutralYaw) * HeadOrientation. At the
 * neutral pose the eye is exactly the anchor (the simulation eye in play): zero
 * model offset, decision G deferred.
 */
struct WOTVR_API FWoTVRRig
{
	/** The artificial heading in degrees, (-180, 180]. */
	double HeadingYaw = 0.0;
	/** A neutral pose has been confirmed for the current tracking space. */
	bool bCalibrated = false;
	FVector NeutralPositionMeters = FVector::ZeroVector;
	double NeutralYaw = 0.0;
	/** The head's last yaw relative to neutral away from vertical. */
	double LastValidRelativeYaw = 0.0;

	static FVector ToUnits(const FVector& Meters) { return Meters * WoTVR::UnitsPerMeter; }

	/** The yaw of Orientation's forward direction in degrees, or Fallback when
	 * it points nearly straight up or down (bOutValid false). */
	static double ForwardYaw(const FQuat& Orientation, double Fallback, bool* bOutValid = nullptr);

	/** The head's yaw relative to neutral, falling back to the last valid one. */
	double RelativeYaw(const FWoTVRHeadPose& Pose) const;
	/** Tracked translation from neutral, in world units, in the calibrated frame. */
	FVector RelativeOffset(const FWoTVRHeadPose& Pose) const;

	/** The head's world orientation: heading composed with the head. */
	FQuat WorldOrientation(const FWoTVRHeadPose& Pose) const;
	/** The head's orientation under another heading (a special view's): only
	 * the heading's yaw; pitch and roll stay the head's. */
	FQuat OrientationUnderHeading(const FWoTVRHeadPose& Pose, double Heading) const;
	/** The rendered centre eye's offset from the anchor, in world units. */
	FVector WorldOffset(const FWoTVRHeadPose& Pose) const;
	/** The head's world yaw: heading plus relative yaw. */
	double WorldYaw(const FWoTVRHeadPose& Pose) const;
	/** The head aim: world yaw and absolute pitch, no roll. */
	FRotator Aim(const FWoTVRHeadPose& Pose) const;

	/** Remembers the head's relative yaw while it is away from vertical. */
	void NoteValidPose(const FWoTVRHeadPose& Pose);
	/** Confirms Pose as neutral: its position becomes the anchor's eye and its
	 * yaw forward. The heading is unchanged. */
	void Calibrate(const FWoTVRHeadPose& Pose);
	/** Puts the head back over the anchor horizontally, keeping the vertical
	 * calibration (the user's posture). Calibrates if nothing was confirmed. */
	void ReanchorHorizontal(const FWoTVRHeadPose& Pose);
	/** Sets the heading so the head's current world yaw is AimYaw. */
	void AlignHeading(const FWoTVRHeadPose& Pose, double AimYaw);
	/** Turns the heading by DeltaYaw about the current head: the head's world
	 * position stays put (the eyes do not orbit) and its world yaw changes by
	 * DeltaYaw once. No pitch or roll. */
	void Turn(const FWoTVRHeadPose& Pose, double DeltaYaw);
	/** Confirms Pose as neutral, keeping the world aim yaw: the head returns over
	 * the anchor and looks where it looked. */
	void Recentre(const FWoTVRHeadPose& Pose);
};
