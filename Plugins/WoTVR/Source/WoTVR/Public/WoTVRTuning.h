#pragma once

#include "CoreMinimal.h"

// Values the headset sessions tune (design section 16). Each is a console
// variable (wot.vr.*) holding the design's proposal as its default; the
// accessors clamp to a safe range.
namespace WoTVRTuning
{
	/** The stereo near plane, in world units: 2.5 (about 4.8 cm at 52.5 per
	 * metre). A near rectangle's farthest corner lies about 2.25 times the near
	 * distance from the eye at a headset's asymmetric fields of view (up to
	 * about 55 degrees out from the axis each way), so 2.5 keeps every corner
	 * (about 5.6 units) inside the view-safety probe (ProbeRadius, 6): the view
	 * is black before any part of the near plane can reach a surface. Close
	 * geometry stays visible down to a hand's breadth, and the engine's
	 * reversed-Z infinite projection keeps depth precision at that distance.
	 * It does not depend on hands, the selected artifact or tracking, and
	 * replaces the flat view's plane only while VR owns the view. */
	WOTVR_API float NearClip();
	/** How far the rendered eyes may lead the capsule's axis horizontally before
	 * the view fades (design: proposed 8 units, about 15 cm). */
	WOTVR_API float FollowGap();
	/** Width of the fade ramp before the follow gap and the rise margin. */
	WOTVR_API float FadeRamp();
	/** How far the head may rise above the standing simulation eye before the
	 * view fades: 4 units (7.6 cm), the capsule's top. */
	WOTVR_API float RiseMargin();
	/** The rendered eye must stay this far above the capsule's bottom. */
	WOTVR_API float FloorClearance();
	/** Radius of the probe swept from the simulation eye to each rendered eye:
	 * the view fades before any surface comes this close to the eye path. */
	WOTVR_API float ProbeRadius();
	/** Seconds the view takes to clear after a hazard ends (the fade to black is
	 * immediate). */
	WOTVR_API float FadeInSeconds();
	/** Seconds of the short fade at a teleport, respawn, recentre or switch. */
	WOTVR_API float TransitionFadeSeconds();
	/** Snap turn: deflection that triggers, and the level it must fall under to
	 * re-arm (design section 9: 0.7 and 0.3). */
	WOTVR_API float SnapTriggerThreshold();
	WOTVR_API float SnapRearmThreshold();
	/** Smooth turn dead zone on the deflection. */
	WOTVR_API float SmoothDeadZone();
	/** Seconds a requested VR start may take before it falls back to flat. */
	WOTVR_API float StartTimeoutSeconds();
	/** Seconds between attempts to start a new session after the runtime ended
	 * one, while the runtime refuses (design section 14: VR recovers by itself
	 * once the runtime offers a session again). */
	WOTVR_API float SessionRetrySeconds();
	/** Seconds a gameplay yaw change outside a teleport, respawn or forced turn
	 * (a rotating base, a place swap) must hold still before the view takes it. */
	WOTVR_API float GameplayYawSettleSeconds();

	// The headset HUD and menus (design section 10). Panels are placed in the
	// runtime's tracking space, in metres; every value needs headset tuning.

	/** The HUD panel's distance from the head (design: 1.2 m). */
	WOTVR_API float HUDDistanceMeters();
	/** The HUD panel's width as an angle at that distance (design: 35 degrees). */
	WOTVR_API float HUDWidthDegrees();
	/** How far below the level forward view the HUD panel's centre sits. */
	WOTVR_API float HUDDropDegrees();
	/** The HUD panel's soft attachment: it stays put while the head's yaw is
	 * within this many degrees of the panel's, then follows. */
	WOTVR_API float HUDFollowDeadZoneDegrees();
	/** How quickly a following HUD panel closes on the head's yaw (per second:
	 * 3 closes 95% of the gap in a second). */
	WOTVR_API float HUDFollowRate();
	/** Menus and windows: world-locked this far in front of the view when one
	 * opens (design: about 1.5 m), and this wide. */
	WOTVR_API float MenuDistanceMeters();
	WOTVR_API float MenuWidthMeters();
	/** The scoreboard's reading panel: world-locked this far in front of the
	 * view when the board shows (design: about 1.5 m), and this wide. */
	WOTVR_API float ReadingDistanceMeters();
	WOTVR_API float ReadingWidthMeters();
	/** Passive cards beside the HUD panel: their width, and the gap between
	 * them and the HUD panel. */
	WOTVR_API float CardWidthMeters();
	WOTVR_API float CardGapMeters();
	/** After D-pad or stick navigation, confirm presses the focused control
	 * until the gaze has turned at least this far (degrees) and rests on a
	 * different control: a screen changing under a still gaze, or the gaze
	 * drifting while the player reads, keeps the focused control. */
	WOTVR_API float GazeMoveDegrees();
}
