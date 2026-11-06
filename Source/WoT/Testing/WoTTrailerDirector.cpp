#include "WoTTrailerDirector.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "Algo/StableSort.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace WoTTrailer
{
	EEase ParseEase(const FString& Name)
	{
		if (Name.Equals(TEXT("in"), ESearchCase::IgnoreCase)) return EEase::In;
		if (Name.Equals(TEXT("out"), ESearchCase::IgnoreCase)) return EEase::Out;
		if (Name.Equals(TEXT("inout"), ESearchCase::IgnoreCase)) return EEase::InOut;
		if (Name.Equals(TEXT("smoothstep"), ESearchCase::IgnoreCase)) return EEase::SmoothStep;
		if (Name.Equals(TEXT("smootherstep"), ESearchCase::IgnoreCase)) return EEase::SmootherStep;
		if (Name.Equals(TEXT("outback"), ESearchCase::IgnoreCase)) return EEase::OutBack;
		if (Name.Equals(TEXT("cut"), ESearchCase::IgnoreCase)) return EEase::Cut;
		return EEase::Linear;
	}

	float Ease(float Alpha, EEase Curve)
	{
		const float T = FMath::Clamp(Alpha, 0.0f, 1.0f);
		switch (Curve)
		{
		case EEase::In:
			return T * T * T;
		case EEase::Out:
			return 1.0f - FMath::Pow(1.0f - T, 3.0f);
		case EEase::InOut:
			return T < 0.5f
				? 4.0f * T * T * T
				: 1.0f - FMath::Pow(-2.0f * T + 2.0f, 3.0f) * 0.5f;
		case EEase::SmoothStep:
			return T * T * (3.0f - 2.0f * T);
		case EEase::SmootherStep:
			return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
		case EEase::OutBack:
		{
			constexpr float Overshoot = 0.8f;
			const float U = T - 1.0f;
			return 1.0f + (Overshoot + 1.0f) * U * U * U
				+ Overshoot * U * U;
		}
		case EEase::Cut:
			return T < 1.0f ? 0.0f : 1.0f;
		default:
			return T;
		}
	}

	namespace
	{
		float EaseStartDerivative(EEase Curve)
		{
			switch (Curve)
			{
			case EEase::Linear: return 1.0f;
			case EEase::In: return 0.0f;
			case EEase::Out: return 3.0f;
			case EEase::InOut: return 0.0f;
			case EEase::OutBack: return 3.8f;
			default: return 0.0f;
			}
		}

		float EaseEndDerivative(EEase Curve)
		{
			switch (Curve)
			{
			case EEase::Linear: return 1.0f;
			case EEase::In: return 3.0f;
			case EEase::Out: return 0.0f;
			case EEase::InOut: return 0.0f;
			default: return 0.0f;
			}
		}

		EEase SegmentEase(const TArray<FCameraKey>& Keys, int32 EndIndex,
			EEase DefaultCurve)
		{
			return Keys[EndIndex].bHasEase ? Keys[EndIndex].Ease : DefaultCurve;
		}

		FArcLengthPath MakeArcPath(const TArray<FVector>& Samples)
		{
			FArcLengthPath Result;
			for (const FVector& Sample : Samples)
			{
				if (!Result.Points.IsEmpty()
					&& Sample.Equals(Result.Points.Last(), UE_KINDA_SMALL_NUMBER))
				{
					continue;
				}
				const float Distance = Result.Points.IsEmpty() ? 0.0f
					: Result.Distances.Last() + FVector::Distance(Result.Points.Last(), Sample);
				Result.Points.Add(Sample);
				Result.Distances.Add(Distance);
			}
			return Result;
		}

		FVector CentripetalPoint(const FVector& P0, const FVector& P1,
			const FVector& P2, const FVector& P3, float Alpha)
		{
			auto Knot = [](float Previous, const FVector& A, const FVector& B)
			{
				return Previous + FMath::Sqrt(FMath::Max(
					static_cast<float>(FVector::Distance(A, B)), UE_KINDA_SMALL_NUMBER));
			};
			const float T0 = 0.0f;
			const float T1 = Knot(T0, P0, P1);
			const float T2 = Knot(T1, P1, P2);
			const float T3 = Knot(T2, P2, P3);
			const float T = FMath::Lerp(T1, T2, Alpha);
			auto Blend = [](const FVector& A, const FVector& B,
				float ATime, float BTime, float At)
			{
				return FMath::Lerp(A, B,
					(At - ATime) / FMath::Max(BTime - ATime, UE_KINDA_SMALL_NUMBER));
			};
			const FVector A1 = Blend(P0, P1, T0, T1, T);
			const FVector A2 = Blend(P1, P2, T1, T2, T);
			const FVector A3 = Blend(P2, P3, T2, T3, T);
			const FVector B1 = Blend(A1, A2, T0, T2, T);
			const FVector B2 = Blend(A2, A3, T1, T3, T);
			return Blend(B1, B2, T1, T2, T);
		}
	}

	FCameraPose InterpolateCamera(
		const TArray<FCameraKey>& Keys, double Time, EEase Curve)
	{
		FCameraPose Result;
		if (Keys.IsEmpty()) return Result;
		if (Keys.Num() == 1 || Time <= Keys[0].Time)
		{
			return {Keys[0].Location, Keys[0].Rotation, Keys[0].FOV};
		}
		if (Time >= Keys.Last().Time)
		{
			return {Keys.Last().Location, Keys.Last().Rotation, Keys.Last().FOV};
		}

		int32 Segment = 0;
		while (Segment + 1 < Keys.Num() && Time > Keys[Segment + 1].Time)
		{
			++Segment;
		}
		const FCameraKey& A = Keys[Segment];
		const FCameraKey& B = Keys[Segment + 1];
		const float RawAlpha = static_cast<float>(
			(Time - A.Time) / FMath::Max(B.Time - A.Time, UE_DOUBLE_SMALL_NUMBER));
		const float Alpha = Ease(RawAlpha, Curve);
		const FVector P0 = Keys[FMath::Max(0, Segment - 1)].Location;
		const FVector P3 = Keys[FMath::Min(Keys.Num() - 1, Segment + 2)].Location;
		Result.Location = FMath::CubicInterp(
			A.Location, 0.5 * (B.Location - P0),
			B.Location, 0.5 * (P3 - A.Location), Alpha);
		Result.Rotation.Pitch = A.Rotation.Pitch
			+ FMath::FindDeltaAngleDegrees(A.Rotation.Pitch, B.Rotation.Pitch) * Alpha;
		Result.Rotation.Yaw = A.Rotation.Yaw
			+ FMath::FindDeltaAngleDegrees(A.Rotation.Yaw, B.Rotation.Yaw) * Alpha;
		Result.Rotation.Roll = A.Rotation.Roll
			+ FMath::FindDeltaAngleDegrees(A.Rotation.Roll, B.Rotation.Roll) * Alpha;
		Result.Rotation.Normalize();
		Result.FOV = FMath::Lerp(A.FOV, B.FOV, Alpha);
		return Result;
	}

	FVector CameraKeyVelocity(const TArray<FCameraKey>& Keys, int32 KeyIndex,
		EEase DefaultCurve)
	{
		if (!Keys.IsValidIndex(KeyIndex) || Keys.Num() < 2) return FVector::ZeroVector;
		const bool bIncomingCut = KeyIndex > 0
			&& SegmentEase(Keys, KeyIndex, DefaultCurve) == EEase::Cut;
		const bool bOutgoingCut = KeyIndex + 1 < Keys.Num()
			&& SegmentEase(Keys, KeyIndex + 1, DefaultCurve) == EEase::Cut;
		if (Keys[KeyIndex].Hold > 0.0f || bIncomingCut || bOutgoingCut)
		{
			return FVector::ZeroVector;
		}
		if (KeyIndex > 0 && KeyIndex + 1 < Keys.Num())
		{
			const EEase Incoming = SegmentEase(Keys, KeyIndex, DefaultCurve);
			const EEase Outgoing = SegmentEase(Keys, KeyIndex + 1, DefaultCurve);
			if (EaseEndDerivative(Incoming) <= UE_KINDA_SMALL_NUMBER
				|| EaseStartDerivative(Outgoing) <= UE_KINDA_SMALL_NUMBER)
			{
				return FVector::ZeroVector;
			}
			const double Span = FMath::Max(Keys[KeyIndex + 1].Time
				- Keys[KeyIndex - 1].Time, UE_DOUBLE_SMALL_NUMBER);
			return (Keys[KeyIndex + 1].Location - Keys[KeyIndex - 1].Location) / Span;
		}
		const int32 Other = KeyIndex == 0 ? 1 : KeyIndex - 1;
		const double Span = FMath::Max(FMath::Abs(Keys[Other].Time
			- Keys[KeyIndex].Time), UE_DOUBLE_SMALL_NUMBER);
		return (Keys[Other].Location - Keys[KeyIndex].Location)
			/ (Keys[Other].Time >= Keys[KeyIndex].Time ? Span : -Span);
	}

	FCameraPose InterpolateStyledCamera(const TArray<FCameraKey>& Keys,
		double Time, EEase DefaultCurve)
	{
		if (Keys.IsEmpty()) return FCameraPose();
		if (Keys.Num() == 1 || Time <= Keys[0].Time)
		{
			return {Keys[0].Location, Keys[0].Rotation, Keys[0].FOV};
		}
		if (Time >= Keys.Last().Time)
		{
			return {Keys.Last().Location, Keys.Last().Rotation, Keys.Last().FOV};
		}
		int32 Segment = 0;
		while (Segment + 1 < Keys.Num() && Time > Keys[Segment + 1].Time) ++Segment;
		const FCameraKey& A = Keys[Segment];
		const FCameraKey& B = Keys[Segment + 1];
		const double MoveStart = A.Time + FMath::Max(0.0f, A.Hold);
		if (Time <= MoveStart)
		{
			return {A.Location, A.Rotation, A.FOV};
		}
		const EEase Curve = SegmentEase(Keys, Segment + 1, DefaultCurve);
		if (Curve == EEase::Cut)
		{
			return Time >= B.Time
				? FCameraPose{B.Location, B.Rotation, B.FOV}
				: FCameraPose{A.Location, A.Rotation, A.FOV};
		}
		const double Duration = FMath::Max(B.Time - MoveStart, UE_DOUBLE_SMALL_NUMBER);
		const float RawAlpha = static_cast<float>((Time - MoveStart) / Duration);
		const float Alpha = Ease(RawAlpha, Curve);
		const float StartDerivative = EaseStartDerivative(Curve);
		const float EndDerivative = EaseEndDerivative(Curve);
		const FVector StartVelocity = CameraKeyVelocity(Keys, Segment, DefaultCurve);
		const FVector EndVelocity = CameraKeyVelocity(Keys, Segment + 1, DefaultCurve);
		const FVector StartTangent = StartDerivative > UE_KINDA_SMALL_NUMBER
			? StartVelocity * Duration / StartDerivative : FVector::ZeroVector;
		const FVector EndTangent = EndDerivative > UE_KINDA_SMALL_NUMBER
			? EndVelocity * Duration / EndDerivative : FVector::ZeroVector;
		FCameraPose Result;
		Result.Location = FMath::CubicInterp(
			A.Location, StartTangent, B.Location, EndTangent, Alpha);
		Result.Rotation.Pitch = A.Rotation.Pitch + FMath::FindDeltaAngleDegrees(
			A.Rotation.Pitch, B.Rotation.Pitch) * Alpha;
		Result.Rotation.Yaw = A.Rotation.Yaw + FMath::FindDeltaAngleDegrees(
			A.Rotation.Yaw, B.Rotation.Yaw) * Alpha;
		Result.Rotation.Roll = A.Rotation.Roll + FMath::FindDeltaAngleDegrees(
			A.Rotation.Roll, B.Rotation.Roll) * Alpha;
		Result.Rotation.Normalize();
		Result.FOV = FMath::Lerp(A.FOV, B.FOV, Alpha);
		return Result;
	}

	float FArcLengthPath::Length() const
	{
		return Distances.IsEmpty() ? 0.0f : Distances.Last();
	}

	FVector FArcLengthPath::Sample(float Distance) const
	{
		if (Points.IsEmpty()) return FVector::ZeroVector;
		if (Points.Num() == 1 || Distance <= 0.0f) return Points[0];
		if (Distance >= Length()) return Points.Last();
		int32 Upper = 1;
		while (Upper < Distances.Num() && Distances[Upper] < Distance) ++Upper;
		const float Span = FMath::Max(Distances[Upper] - Distances[Upper - 1],
			UE_KINDA_SMALL_NUMBER);
		return FMath::Lerp(Points[Upper - 1], Points[Upper],
			(Distance - Distances[Upper - 1]) / Span);
	}

	FVector FArcLengthPath::Tangent(float Distance) const
	{
		if (Points.Num() < 2) return FVector::ForwardVector;
		const float Probe = FMath::Max(1.0f, Length() * 0.001f);
		return (Sample(Distance + Probe) - Sample(Distance - Probe)).GetSafeNormal();
	}

	float FArcLengthPath::FindNearestDistance(const FVector& Position,
		float MinimumDistance, float MaximumDistance, float TieMaximumDistance,
		bool bOverFloor, bool bLaterPassTies) const
	{
		const float Low = FMath::Clamp(MinimumDistance, 0.0f, Length());
		const float High = FMath::Clamp(MaximumDistance, Low, Length());
		const float TieHigh = FMath::Clamp(TieMaximumDistance, High, Length());
		// Over the floor, heights drop out of both the projection and the
		// distance: a route's PathNodes stand at their own heights, the walker
		// at its floor's.
		const FVector Flatten = bOverFloor ? FVector(1.0, 1.0, 0.0) : FVector::OneVector;
		auto Distance2 = [&](const FVector& Point)
		{
			return static_cast<float>(((Position - Point) * Flatten).SizeSquared());
		};
		float BestDistance = Low;
		float BestSquared = Distance2(Sample(BestDistance));
		// The candidate on one segment, kept inside [From, To].
		auto Consider = [&](int32 Index, float From, float To, bool bTieOnly)
		{
			const FVector Segment = Points[Index] - Points[Index - 1];
			const FVector Projected = Segment * Flatten;
			const float SegmentSquared = Projected.SizeSquared();
			float Alpha = SegmentSquared <= UE_KINDA_SMALL_NUMBER ? 0.0f
				: FMath::Clamp(static_cast<float>(FVector::DotProduct(
					(Position - Points[Index - 1]) * Flatten, Projected) / SegmentSquared),
					0.0f, 1.0f);
			const float Span = Distances[Index] - Distances[Index - 1];
			if (Span > UE_KINDA_SMALL_NUMBER)
			{
				Alpha = FMath::Clamp(Alpha,
					FMath::Max(0.0f, (From - Distances[Index - 1]) / Span),
					FMath::Min(1.0f, (To - Distances[Index - 1]) / Span));
			}
			const FVector Candidate = Points[Index - 1] + Segment * Alpha;
			const float Squared = Distance2(Candidate);
			const float Along = FMath::Lerp(Distances[Index - 1], Distances[Index], Alpha);
			// Arena_03's walker stuck at dead-end tips: the outbound and
			// return passes are the same line, the earlier one always won, and
			// progress never crossed the tip.
			const bool bLaterPassTie = bLaterPassTies && Along > BestDistance + 16.0f
				&& FMath::Sqrt(Squared) <= FMath::Sqrt(BestSquared) + 0.5f;
			if ((!bTieOnly && Squared < BestSquared) || bLaterPassTie)
			{
				BestSquared = Squared;
				BestDistance = Along;
			}
		};
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			if (Distances[Index] < Low) continue;
			if (Distances[Index - 1] > TieHigh) break;
			if (Distances[Index - 1] <= High)
			{
				Consider(Index, Low, High, false);
			}
			if (TieHigh > High && Distances[Index] > High)
			{
				Consider(Index, FMath::Max(Low, High), TieHigh, true);
			}
		}
		return BestDistance;
	}

	float FArcLengthPath::AdvanceHorizontal(float Distance, float Horizontal) const
	{
		const float Start = FMath::Clamp(Distance, 0.0f, Length());
		if (Points.Num() < 2 || Horizontal <= 0.0f) return Start;
		float Remaining = Horizontal;
		FVector Previous = Sample(Start);
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			if (Distances[Index] <= Start) continue;
			const float From = FMath::Max(Start, Distances[Index - 1]);
			const float Step = FVector::Dist2D(Previous, Points[Index]);
			if (Step >= Remaining)
			{
				return FMath::Lerp(From, Distances[Index], Remaining / Step);
			}
			Remaining -= Step;
			Previous = Points[Index];
		}
		return Length();
	}

	FArcLengthPath BuildCentripetalPath(const TArray<FVector>& ControlPoints,
		int32 SamplesPerSegment)
	{
		if (ControlPoints.Num() < 2) return MakeArcPath(ControlPoints);
		TArray<FVector> Samples;
		FVector BoundsMin = ControlPoints[0];
		FVector BoundsMax = ControlPoints[0];
		for (const FVector& Control : ControlPoints)
		{
			BoundsMin.X = FMath::Min(BoundsMin.X, Control.X);
			BoundsMin.Y = FMath::Min(BoundsMin.Y, Control.Y);
			BoundsMin.Z = FMath::Min(BoundsMin.Z, Control.Z);
			BoundsMax.X = FMath::Max(BoundsMax.X, Control.X);
			BoundsMax.Y = FMath::Max(BoundsMax.Y, Control.Y);
			BoundsMax.Z = FMath::Max(BoundsMax.Z, Control.Z);
		}
		const int32 Steps = FMath::Max(2, SamplesPerSegment);
		for (int32 Segment = 0; Segment + 1 < ControlPoints.Num(); ++Segment)
		{
			const FVector& P1 = ControlPoints[Segment];
			const FVector& P2 = ControlPoints[Segment + 1];
			const FVector P0 = Segment > 0 ? ControlPoints[Segment - 1] : 2.0f * P1 - P2;
			const FVector P3 = Segment + 2 < ControlPoints.Num()
				? ControlPoints[Segment + 2] : 2.0f * P2 - P1;
			for (int32 Step = Segment == 0 ? 0 : 1; Step <= Steps; ++Step)
			{
				FVector Point = CentripetalPoint(P0, P1, P2, P3,
					static_cast<float>(Step) / Steps);
				Point.X = FMath::Clamp(Point.X, BoundsMin.X, BoundsMax.X);
				Point.Y = FMath::Clamp(Point.Y, BoundsMin.Y, BoundsMax.Y);
				Point.Z = FMath::Clamp(Point.Z, BoundsMin.Z, BoundsMax.Z);
				Samples.Add(Point);
			}
		}
		return MakeArcPath(Samples);
	}

	FArcLengthPath BuildRoundedPath(const TArray<FVector>& ControlPoints,
		float TurnRadius, int32 SamplesPerCorner)
	{
		if (ControlPoints.Num() < 3 || TurnRadius <= 0.0f)
		{
			return MakeArcPath(ControlPoints);
		}
		TArray<FVector> Samples;
		Samples.Add(ControlPoints[0]);
		const int32 Steps = FMath::Max(2, SamplesPerCorner);
		for (int32 Index = 1; Index + 1 < ControlPoints.Num(); ++Index)
		{
			const FVector& Corner = ControlPoints[Index];
			const FVector Incoming = ControlPoints[Index - 1] - Corner;
			const FVector Outgoing = ControlPoints[Index + 1] - Corner;
			const float Trim = FMath::Min3(TurnRadius,
				static_cast<float>(Incoming.Size()) * 0.5f,
				static_cast<float>(Outgoing.Size()) * 0.5f);
			const FVector Entry = Corner + Incoming.GetSafeNormal() * Trim;
			const FVector Exit = Corner + Outgoing.GetSafeNormal() * Trim;
			Samples.Add(Entry);
			for (int32 Step = 1; Step <= Steps; ++Step)
			{
				const float T = static_cast<float>(Step) / Steps;
				const float U = 1.0f - T;
				Samples.Add(U * U * Entry + 2.0f * U * T * Corner + T * T * Exit);
			}
		}
		Samples.Add(ControlPoints.Last());
		return MakeArcPath(Samples);
	}

	void StepCriticalSpring(FScalarSpring& State, float Target,
		float SettleSeconds, float DeltaSeconds)
	{
		if (SettleSeconds <= 0.0f)
		{
			State.Value = Target;
			State.Velocity = 0.0f;
			return;
		}
		if (DeltaSeconds <= 0.0f) return;
		constexpr float SettleToOmega = 4.744f; // 5% critical response at t=settle.
		const float Omega = SettleToOmega / SettleSeconds;
		const float Error = State.Value - Target;
		const float Combined = State.Velocity + Omega * Error;
		const float Decay = FMath::Exp(-Omega * DeltaSeconds);
		State.Value = Target + (Error + Combined * DeltaSeconds) * Decay;
		State.Velocity = (State.Velocity - Omega * Combined * DeltaSeconds) * Decay;
	}

	void StepCriticalAngleSpring(FScalarSpring& State, float TargetDegrees,
		float SettleSeconds, float DeltaSeconds)
	{
		const float UnwrappedTarget = State.Value
			+ FMath::FindDeltaAngleDegrees(State.Value, TargetDegrees);
		StepCriticalSpring(State, UnwrappedTarget, SettleSeconds, DeltaSeconds);
		State.Value = FRotator::NormalizeAxis(State.Value);
	}

	float PullInCameraDistance(float DesiredDistance, float HitDistance,
		float Clearance)
	{
		return FMath::Clamp(HitDistance - FMath::Max(0.0f, Clearance),
			0.0f, FMath::Max(0.0f, DesiredDistance));
	}

	bool AdvanceWaypoints(const FVector& Position, const TArray<FVector>& Path,
		float Tolerance, int32& InOutIndex)
	{
		const float RadiusSquared = FMath::Square(FMath::Max(0.0f, Tolerance));
		while (Path.IsValidIndex(InOutIndex)
			&& FVector::DistSquared(Position, Path[InOutIndex]) <= RadiusSquared)
		{
			++InOutIndex;
		}
		return InOutIndex >= Path.Num();
	}

	bool ParseTimeline(const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FTimelineEntry>& OutEntries, TArray<FString>& OutErrors)
	{
		OutEntries.Reset();
		OutErrors.Reset();
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Object = Values[Index].IsValid()
				? Values[Index]->AsObject() : nullptr;
			double Time = -1.0;
			FString Command;
			if (!Object.IsValid() || !Object->TryGetNumberField(TEXT("t"), Time)
				|| Time < 0.0 || !Object->TryGetStringField(TEXT("cmd"), Command)
				|| Command.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("timeline entry %d needs nonnegative t and cmd"), Index));
				continue;
			}
			bool bFatal = false;
			Object->TryGetBoolField(TEXT("fatal"), bFatal);
			OutEntries.Add({Time, Index, Command, Object, bFatal});
		}
		if (!OutErrors.IsEmpty())
		{
			OutEntries.Reset();
			return false;
		}
		Algo::StableSort(OutEntries,
			[](const FTimelineEntry& A, const FTimelineEntry& B)
			{
				return A.Time < B.Time;
			});
		return true;
	}
}

#endif
