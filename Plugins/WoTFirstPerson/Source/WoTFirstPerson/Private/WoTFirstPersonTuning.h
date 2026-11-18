#pragma once

// Presentation units are centimeters, angles degrees, times seconds.
namespace WoTFirstPerson::Tuning
{
inline constexpr double TargetExtent = 9.0;
// While a model shows, the flat mode lowers the view's near plane from the
// engine's 10 to this, so the pulled model has depth room between the near
// plane and the nearest wall. The world only gains geometry 2-10 units from
// the eye, which the larger plane used to cut away.
inline constexpr double ShownNearPlane = 2.0;
inline constexpr double NearMargin = 1.0;
inline constexpr double WallMargin = 2.0;
inline constexpr double SweepMargin = 1.0;
inline constexpr double ClearanceEpsilon = 0.01;
inline constexpr double MotionDepthExtent = 1.5;
inline constexpr double SwitchSeconds = 0.12;
inline constexpr double SwitchDrop = 40.0;
inline constexpr double SpringFrequency = 24.0;
inline constexpr double MotionStep = 1.0 / 720.0;
inline constexpr double IdleFrequency = 1.5;
inline constexpr double IdleAmplitude = 0.18;
inline constexpr double SwayLag = 0.025;
inline constexpr double MaxSway = 4.0;
inline constexpr double WalkCycleDistance = 120.0;
inline constexpr double WalkFullSpeed = 150.0;
inline constexpr double WalkAmplitude = 0.35;
inline constexpr double TrembleFrequency = 48.0;
inline constexpr double TrembleAmplitude = 0.06;
inline constexpr double FireKickVelocity = -90.0;
inline constexpr double ReleaseVelocity = 12.0;
// A key light on the hand, fixed to the view (over the left shoulder): the
// world's lights rarely reach a first-person hand, which otherwise shades
// flat. Camera space, cm; a cool display-space colour, as the mesh-lit light
// slots take, scaled by the hand's ScaleGlow like the world's lights.
inline constexpr double HandKeyX = -40.0;
inline constexpr double HandKeyY = -100.0;
inline constexpr double HandKeyZ = 100.0;
inline constexpr double HandKeyRadius = 1000.0;
inline constexpr double HandKeyIntensity = 0.20;
inline constexpr float HandKeyTintR = 0.85f;
inline constexpr float HandKeyTintG = 0.93f;
inline constexpr float HandKeyTintB = 1.06f;
}
