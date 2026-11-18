#include "WoTVRTuning.h"

#include "HAL/IConsoleManager.h"

namespace
{
TAutoConsoleVariable<float> CVarWoTVRNearClip(TEXT("wot.vr.NearClip"), 2.5f,
	TEXT("VR stereo near plane in world units (2.5 = about 4.8 cm at 52.5 units per metre); keep it under wot.vr.ProbeRadius / 2.3."));
TAutoConsoleVariable<float> CVarWoTVRFollowGap(TEXT("wot.vr.FollowGap"), 8.0f,
	TEXT("VR: horizontal distance the rendered eyes may lead the capsule before the view fades."));
TAutoConsoleVariable<float> CVarWoTVRFadeRamp(TEXT("wot.vr.FadeRamp"), 2.0f,
	TEXT("VR: width of the fade ramp before the follow gap and the rise margin."));
TAutoConsoleVariable<float> CVarWoTVRRiseMargin(TEXT("wot.vr.RiseMargin"), 4.0f,
	TEXT("VR: how far the head may rise above the standing simulation eye before the view fades."));
TAutoConsoleVariable<float> CVarWoTVRFloorClearance(TEXT("wot.vr.FloorClearance"), 6.0f,
	TEXT("VR: the rendered eye must stay this far above the capsule's bottom."));
TAutoConsoleVariable<float> CVarWoTVRProbeRadius(TEXT("wot.vr.ProbeRadius"), 6.0f,
	TEXT("VR: radius of the probe swept from the simulation eye to each rendered eye."));
TAutoConsoleVariable<float> CVarWoTVRFadeInSeconds(TEXT("wot.vr.FadeInSeconds"), 0.25f,
	TEXT("VR: seconds the view takes to clear once a view-safety hazard ends."));
TAutoConsoleVariable<float> CVarWoTVRTransitionFadeSeconds(TEXT("wot.vr.TransitionFadeSeconds"), 0.25f,
	TEXT("VR: seconds of the short fade at a teleport, respawn, recentre or mode switch."));
TAutoConsoleVariable<float> CVarWoTVRSnapTrigger(TEXT("wot.vr.SnapTriggerThreshold"), 0.7f,
	TEXT("VR snap turn: deflection that triggers a snap."));
TAutoConsoleVariable<float> CVarWoTVRSnapRearm(TEXT("wot.vr.SnapRearmThreshold"), 0.3f,
	TEXT("VR snap turn: deflection under which the next snap re-arms."));
TAutoConsoleVariable<float> CVarWoTVRSmoothDeadZone(TEXT("wot.vr.SmoothDeadZone"), 0.25f,
	TEXT("VR smooth turn: dead zone on the deflection."));
TAutoConsoleVariable<float> CVarWoTVRStartTimeout(TEXT("wot.vr.StartTimeoutSeconds"), 10.0f,
	TEXT("VR: seconds a requested VR start may take before play continues flat."));
TAutoConsoleVariable<float> CVarWoTVRSessionRetry(TEXT("wot.vr.SessionRetrySeconds"), 1.0f,
	TEXT("VR: seconds between attempts to start a new session after the runtime ended one, while it refuses."));
TAutoConsoleVariable<float> CVarWoTVRGameplayYawSettle(TEXT("wot.vr.GameplayYawSettleSeconds"), 0.5f,
	TEXT("VR: seconds a gameplay yaw change must hold before the view takes it (rotating bases, place swaps)."));
TAutoConsoleVariable<float> CVarWoTVRHUDDistance(TEXT("wot.vr.HUDDistance"), 1.2f,
	TEXT("VR HUD panel: distance from the head in metres."));
TAutoConsoleVariable<float> CVarWoTVRHUDWidthDegrees(TEXT("wot.vr.HUDWidthDegrees"), 35.0f,
	TEXT("VR HUD panel: width as an angle at its distance, in degrees."));
TAutoConsoleVariable<float> CVarWoTVRHUDDropDegrees(TEXT("wot.vr.HUDDropDegrees"), 30.0f,
	TEXT("VR HUD panel: how far below the level forward view its centre sits, in degrees."));
TAutoConsoleVariable<float> CVarWoTVRHUDFollowDeadZone(TEXT("wot.vr.HUDFollowDeadZone"), 20.0f,
	TEXT("VR HUD panel: the head may turn this many degrees from the panel before it follows."));
TAutoConsoleVariable<float> CVarWoTVRHUDFollowRate(TEXT("wot.vr.HUDFollowRate"), 3.0f,
	TEXT("VR HUD panel: how quickly a following panel closes on the head's yaw, per second."));
TAutoConsoleVariable<float> CVarWoTVRMenuDistance(TEXT("wot.vr.MenuDistance"), 1.5f,
	TEXT("VR menus and windows: distance in front of the view when one opens, in metres."));
TAutoConsoleVariable<float> CVarWoTVRMenuWidth(TEXT("wot.vr.MenuWidth"), 1.3f,
	TEXT("VR menus and windows: panel width in metres."));
TAutoConsoleVariable<float> CVarWoTVRReadingDistance(TEXT("wot.vr.ReadingDistance"), 1.5f,
	TEXT("VR scoreboard panel: distance in front of the view when the board shows, in metres."));
TAutoConsoleVariable<float> CVarWoTVRReadingWidth(TEXT("wot.vr.ReadingWidth"), 1.2f,
	TEXT("VR scoreboard panel: width in metres."));
TAutoConsoleVariable<float> CVarWoTVRCardWidth(TEXT("wot.vr.CardWidth"), 0.45f,
	TEXT("VR passive cards (beside the HUD panel): width in metres."));
TAutoConsoleVariable<float> CVarWoTVRCardGap(TEXT("wot.vr.CardGap"), 0.04f,
	TEXT("VR passive cards: gap between the HUD panel and a card, in metres."));
TAutoConsoleVariable<float> CVarWoTVRGazeMoveDegrees(TEXT("wot.vr.GazeMoveDegrees"), 1.0f,
	TEXT("VR menus: after D-pad or stick navigation, confirm presses the focused control until the gaze turns this many degrees onto another control."));

float WoTVRTuningRead(const TAutoConsoleVariable<float>& Variable, float Min, float Max)
{
	const float Value = Variable.GetValueOnGameThread();
	return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Min;
}
}

namespace WoTVRTuning
{
float NearClip() { return WoTVRTuningRead(CVarWoTVRNearClip, 0.5f, 20.0f); }
float FollowGap() { return WoTVRTuningRead(CVarWoTVRFollowGap, 1.0f, 64.0f); }
float FadeRamp() { return WoTVRTuningRead(CVarWoTVRFadeRamp, 0.01f, 32.0f); }
float RiseMargin() { return WoTVRTuningRead(CVarWoTVRRiseMargin, 0.0f, 64.0f); }
float FloorClearance() { return WoTVRTuningRead(CVarWoTVRFloorClearance, 0.0f, 64.0f); }
float ProbeRadius() { return WoTVRTuningRead(CVarWoTVRProbeRadius, 0.5f, 32.0f); }
float FadeInSeconds() { return WoTVRTuningRead(CVarWoTVRFadeInSeconds, 0.0f, 5.0f); }
float TransitionFadeSeconds() { return WoTVRTuningRead(CVarWoTVRTransitionFadeSeconds, 0.0f, 5.0f); }
float SnapTriggerThreshold() { return WoTVRTuningRead(CVarWoTVRSnapTrigger, 0.05f, 1.0f); }
float SnapRearmThreshold() { return WoTVRTuningRead(CVarWoTVRSnapRearm, 0.0f, 0.95f); }
float SmoothDeadZone() { return WoTVRTuningRead(CVarWoTVRSmoothDeadZone, 0.0f, 0.95f); }
float StartTimeoutSeconds() { return WoTVRTuningRead(CVarWoTVRStartTimeout, 0.5f, 120.0f); }
float SessionRetrySeconds() { return WoTVRTuningRead(CVarWoTVRSessionRetry, 0.1f, 60.0f); }
float GameplayYawSettleSeconds() { return WoTVRTuningRead(CVarWoTVRGameplayYawSettle, 0.0f, 10.0f); }
float HUDDistanceMeters() { return WoTVRTuningRead(CVarWoTVRHUDDistance, 0.3f, 5.0f); }
float HUDWidthDegrees() { return WoTVRTuningRead(CVarWoTVRHUDWidthDegrees, 5.0f, 120.0f); }
float HUDDropDegrees() { return WoTVRTuningRead(CVarWoTVRHUDDropDegrees, -60.0f, 80.0f); }
float HUDFollowDeadZoneDegrees() { return WoTVRTuningRead(CVarWoTVRHUDFollowDeadZone, 0.0f, 90.0f); }
float HUDFollowRate() { return WoTVRTuningRead(CVarWoTVRHUDFollowRate, 0.1f, 30.0f); }
float MenuDistanceMeters() { return WoTVRTuningRead(CVarWoTVRMenuDistance, 0.3f, 5.0f); }
float MenuWidthMeters() { return WoTVRTuningRead(CVarWoTVRMenuWidth, 0.1f, 5.0f); }
float ReadingDistanceMeters() { return WoTVRTuningRead(CVarWoTVRReadingDistance, 0.3f, 5.0f); }
float ReadingWidthMeters() { return WoTVRTuningRead(CVarWoTVRReadingWidth, 0.1f, 5.0f); }
float CardWidthMeters() { return WoTVRTuningRead(CVarWoTVRCardWidth, 0.05f, 3.0f); }
float CardGapMeters() { return WoTVRTuningRead(CVarWoTVRCardGap, 0.0f, 1.0f); }
float GazeMoveDegrees() { return WoTVRTuningRead(CVarWoTVRGazeMoveDegrees, 0.0f, 30.0f); }
}
