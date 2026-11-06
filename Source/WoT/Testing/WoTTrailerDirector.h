#pragma once

#include "CoreMinimal.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

class FJsonObject;
class FJsonValue;

namespace WoTTrailer
{
	enum class EEase : uint8
	{
		Linear,
		In,
		Out,
		InOut,
		SmoothStep,
		SmootherStep,
		OutBack,
		Cut,
	};

	EEase ParseEase(const FString& Name);
	float Ease(float Alpha, EEase Curve);

	struct FCameraKey
	{
		double Time = 0.0;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float FOV = 90.0f;
		EEase Ease = EEase::Linear;
		float Hold = 0.0f;
		bool bHasEase = false;
	};

	struct FCameraPose
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float FOV = 90.0f;
	};

	FCameraPose InterpolateCamera(
		const TArray<FCameraKey>& Keys, double Time, EEase Curve);
	FCameraPose InterpolateStyledCamera(
		const TArray<FCameraKey>& Keys, double Time, EEase DefaultCurve);
	FVector CameraKeyVelocity(const TArray<FCameraKey>& Keys, int32 KeyIndex,
		EEase DefaultCurve);

	struct FArcLengthPath
	{
		TArray<FVector> Points;
		TArray<float> Distances;

		float Length() const;
		FVector Sample(float Distance) const;
		FVector Tangent(float Distance) const;
		// The arc distance of the path point nearest Position, searched only
		// within [MinimumDistance, MaximumDistance]. A tie (within half a unit)
		// with a point more than 16 uu further along goes to that later point:
		// a route that turns straight back over itself ties everywhere near the
		// turn, and its progress must cross the turn. Such a later point may lie
		// beyond MaximumDistance, up to TieMaximumDistance; past the window only
		// a later point that ties (or is nearer) is taken. bOverFloor measures
		// distance over the floor (XY), for a walker whose height differs from
		// the route's. Without bLaterPassTies a tie keeps the earlier point (a
		// walk's start, which its route may pass again).
		float FindNearestDistance(const FVector& Position, float MinimumDistance = 0.0f,
			float MaximumDistance = TNumericLimits<float>::Max(),
			float TieMaximumDistance = 0.0f, bool bOverFloor = false,
			bool bLaterPassTies = true) const;
		// The arc distance at which the path has run Horizontal uu over the
		// floor (XY) past Distance.
		float AdvanceHorizontal(float Distance, float Horizontal) const;
	};

	FArcLengthPath BuildCentripetalPath(const TArray<FVector>& ControlPoints,
		int32 SamplesPerSegment = 16);
	FArcLengthPath BuildRoundedPath(const TArray<FVector>& ControlPoints,
		float TurnRadius, int32 SamplesPerCorner = 8);

	struct FScalarSpring
	{
		float Value = 0.0f;
		float Velocity = 0.0f;
	};

	void StepCriticalSpring(FScalarSpring& State, float Target,
		float SettleSeconds, float DeltaSeconds);
	void StepCriticalAngleSpring(FScalarSpring& State, float TargetDegrees,
		float SettleSeconds, float DeltaSeconds);
	float PullInCameraDistance(float DesiredDistance, float HitDistance,
		float Clearance);

	bool AdvanceWaypoints(const FVector& Position, const TArray<FVector>& Path,
		float Tolerance, int32& InOutIndex);

	struct FTimelineEntry
	{
		double Time = 0.0;
		int32 SourceIndex = INDEX_NONE;
		FString Command;
		TSharedPtr<FJsonObject> Request;
		bool bFatal = false;
	};

	bool ParseTimeline(const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FTimelineEntry>& OutEntries, TArray<FString>& OutErrors);
}

#endif
