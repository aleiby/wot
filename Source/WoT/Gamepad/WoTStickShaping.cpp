#include "WoTStickShaping.h"

FVector2D FWoTStickShaping::RadialDeadZone(FVector2D Raw, float DeadZone, float OuterZone)
{
	const double Magnitude = Raw.Size();
	if (OuterZone <= DeadZone || Magnitude <= DeadZone || Magnitude <= UE_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	const double Shaped = FMath::Min(1.0, (Magnitude - DeadZone) / (OuterZone - DeadZone));
	return Raw / Magnitude * Shaped;
}

FVector2D FWoTStickShaping::Move(FVector2D Raw, float DeadZone, float OuterZone, float Gain)
{
	const FVector2D Shaped = RadialDeadZone(Raw, DeadZone, OuterZone);
	const double Magnitude = Shaped.Size();
	if (Magnitude <= UE_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	return Shaped / Magnitude * FMath::Min(1.0, Magnitude * Gain);
}

FVector2D FWoTStickShaping::LookDegrees(FVector2D Raw, float DeadZone, float OuterZone,
	float Exponent, float YawRate, float PitchRate, float Sensitivity, bool bInvertY,
	float FOVScale, float DeltaSeconds)
{
	const FVector2D Shaped = RadialDeadZone(Raw, DeadZone, OuterZone);
	const double Magnitude = Shaped.Size();
	if (Magnitude <= UE_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	const FVector2D Curved = Shaped / Magnitude * FMath::Pow(Magnitude, double(Exponent));
	const double Scale = double(Sensitivity) * FOVScale * DeltaSeconds;
	return FVector2D(Curved.X * YawRate * Scale,
		(bInvertY ? -Curved.Y : Curved.Y) * PitchRate * Scale);
}
