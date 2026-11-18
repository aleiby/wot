#include "WoTFirstPersonMotion.h"
#include "WoTFirstPersonTuning.h"

void FWoTFirstPersonSpring::Step(double Target, double DeltaTime)
{
	const double Omega = WoTFirstPerson::Tuning::SpringFrequency;
	const double Error = Value - Target;
	const double C = Velocity + Omega * Error;
	const double Decay = FMath::Exp(-Omega * DeltaTime);
	Value = Target + (Error + C * DeltaTime) * Decay;
	Velocity = (Velocity - Omega * C * DeltaTime) * Decay;
}

void FWoTFirstPersonMotion::Step(double DeltaTime, const FRotator& View,
	double GroundSpeed, double Amount)
{
	using namespace WoTFirstPerson::Tuning;
	Strength = FMath::Clamp(Amount, 0.0, 1.0);
	const FRotator Delta = bHasView ? (View - PreviousView).GetNormalized() : FRotator::ZeroRotator;
	PreviousView = View;
	bHasView = true;
	if (DeltaTime <= 0) return;
	const double PitchTarget = FMath::Clamp(-Delta.Pitch / DeltaTime * SwayLag, -MaxSway, MaxSway);
	const double YawTarget = FMath::Clamp(-Delta.Yaw / DeltaTime * SwayLag, -MaxSway, MaxSway);
	Remainder += DeltaTime;
	while (Remainder + 1.e-10 >= MotionStep)
	{
		Remainder -= MotionStep;
		Time += MotionStep;
		Distance += FMath::Max(0.0, GroundSpeed) * MotionStep;
		Float.Step(FMath::Sin(Time * IdleFrequency) * IdleAmplitude, MotionStep);
		Bob.Step(FMath::Sin(Distance * 2.0 * UE_PI / WalkCycleDistance) * WalkAmplitude
			* FMath::Clamp(GroundSpeed / WalkFullSpeed, 0.0, 1.0), MotionStep);
		SwayPitch.Step(PitchTarget, MotionStep);
		SwayYaw.Step(YawTarget, MotionStep);
		Kick.Step(0, MotionStep);
		Tremble.Step(bHeld ? FMath::Sin(Time * TrembleFrequency) * TrembleAmplitude : 0, MotionStep);
	}
}

void FWoTFirstPersonMotion::FirePressed()
{
	bHeld = true;
	Kick.Velocity = WoTFirstPerson::Tuning::FireKickVelocity;
}

void FWoTFirstPersonMotion::FireReleased()
{
	if (bHeld) Kick.Velocity = WoTFirstPerson::Tuning::ReleaseVelocity;
	bHeld = false;
}

FVector FWoTFirstPersonMotion::Offset() const
{
	using namespace WoTFirstPerson::Tuning;
	return FVector(FMath::Clamp(Kick.Value, -MotionDepthExtent, MotionDepthExtent),
		Tremble.Value, Float.Value + Bob.Value) * Strength;
}

FRotator FWoTFirstPersonMotion::Rotation() const
{
	return FRotator(SwayPitch.Value * Strength, SwayYaw.Value * Strength, 0);
}
