#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "../Testing/WoTTrailerDirector.h"
#include "../WoTSound.h"
#include "WoTTestWorld.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
	constexpr EAutomationTestFlags TrailerFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FJsonValue> TimelineEntry(double Time, const TCHAR* Command)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("t"), Time);
		Entry->SetStringField(TEXT("cmd"), Command);
		return MakeShared<FJsonValueObject>(Entry);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerEasingSpec,
	"WoT.Testing.Trailer.Easing", TrailerFlags)
bool FWoTTrailerEasingSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	TestEqual(TEXT("linear midpoint"), Ease(0.5f, EEase::Linear), 0.5f);
	TestEqual(TEXT("cubic in midpoint"), Ease(0.5f, EEase::In), 0.125f);
	TestEqual(TEXT("cubic out midpoint"), Ease(0.5f, EEase::Out), 0.875f);
	TestEqual(TEXT("cubic inout midpoint"), Ease(0.5f, EEase::InOut), 0.5f);
	TestEqual(TEXT("smoothstep midpoint"), Ease(0.5f, EEase::SmoothStep), 0.5f);
	TestEqual(TEXT("smootherstep midpoint"), Ease(0.5f, EEase::SmootherStep), 0.5f);
	TestTrue(TEXT("outback has a small hero overshoot"),
		Ease(0.75f, EEase::OutBack) > 1.0f);
	TestEqual(TEXT("input clamps below zero"), Ease(-1.0f, EEase::Linear), 0.0f);
	TestEqual(TEXT("input clamps above one"), Ease(2.0f, EEase::Linear), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerCatmullRomArcLengthSpec,
	"WoT.Testing.Trailer.CatmullRomArcLength", TrailerFlags)
bool FWoTTrailerCatmullRomArcLengthSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	const TArray<FVector> Controls = {
		FVector(0, 0, 0), FVector(100, 0, 0),
		FVector(100, 100, 0), FVector(100, 200, 0),
	};
	const FArcLengthPath Curve = BuildCentripetalPath(Controls, 24);
	TestTrue(TEXT("curve begins at the first endpoint"),
		Curve.Sample(0.0f).Equals(Controls[0], KINDA_SMALL_NUMBER));
	TestTrue(TEXT("curve ends at the last endpoint"),
		Curve.Sample(Curve.Length()).Equals(Controls.Last(), KINDA_SMALL_NUMBER));
	for (int32 Index = 1; Index < Curve.Distances.Num(); ++Index)
	{
		TestTrue(TEXT("arc-length progress is strictly monotonic"),
			Curve.Distances[Index] > Curve.Distances[Index - 1]);
	}
	for (const FVector& Point : Curve.Points)
	{
		TestTrue(TEXT("centripetal sharp corner does not overshoot X"),
			Point.X >= -KINDA_SMALL_NUMBER && Point.X <= 100.0f + KINDA_SMALL_NUMBER);
		TestTrue(TEXT("centripetal sharp corner does not overshoot Y"),
			Point.Y >= -KINDA_SMALL_NUMBER && Point.Y <= 200.0f + KINDA_SMALL_NUMBER);
	}
	const float Quarter = Curve.Length() * 0.25f;
	TestTrue(TEXT("equal distance samples advance along the curve"),
		FVector::Distance(Curve.Sample(Quarter), Curve.Sample(Quarter * 2.0f)) > 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerArcPathLocalSearchSpec,
	"WoT.Testing.Trailer.ArcPathLocalSearch", TrailerFlags)
bool FWoTTrailerArcPathLocalSearchSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	auto Polyline = [](std::initializer_list<FVector> Controls)
	{
		FArcLengthPath Path;
		for (const FVector& Point : Controls)
		{
			Path.Distances.Add(Path.Points.IsEmpty() ? 0.0f
				: Path.Distances.Last() + FVector::Dist(Path.Points.Last(), Point));
			Path.Points.Add(Point);
		}
		return Path;
	};
	// An out-and-back route whose return pass runs 8 uu beside the outbound
	// one, as a sweep route doubles back through a corridor.
	const FArcLengthPath Route = Polyline({FVector(0, 0, 0), FVector(1000, 0, 0),
		FVector(1000, 8, 0), FVector(0, 8, 0)});
	const FVector Walker(100, 6, 0);
	TestTrue(TEXT("an unbounded search jumps to the closer return pass"),
		Route.FindNearestDistance(Walker, 90.0f) > 1000.0f);
	TestTrue(TEXT("a window just ahead keeps the outbound pass"),
		FMath::IsNearlyEqual(Route.FindNearestDistance(Walker, 90.0f, 150.0f), 100.0f, 0.01f));
	TestTrue(TEXT("a point beyond the window clamps to its far edge"),
		FMath::IsNearlyEqual(Route.FindNearestDistance(FVector(800, 0, 0), 90.0f, 150.0f),
			150.0f, 0.01f));
	TestTrue(TEXT("a point behind the window clamps to its near edge"),
		FMath::IsNearlyEqual(Route.FindNearestDistance(FVector(20, 0, 0), 90.0f, 150.0f),
			90.0f, 0.01f));

	// A dead end the route turns straight back from: both passes are the same
	// line, and progress near the tip moves onto the return pass.
	const FArcLengthPath Spur = Polyline({FVector(0, 0, 0), FVector(100, 0, 0),
		FVector(0, 0, 0)});
	TestTrue(TEXT("near a retraced tip the tie goes to the return pass"),
		FMath::IsNearlyEqual(Spur.FindNearestDistance(FVector(80, 2, 0), 70.0f, 140.0f),
			120.0f, 0.01f));
	TestTrue(TEXT("a clearly nearer earlier point still wins"),
		FMath::IsNearlyEqual(Route.FindNearestDistance(FVector(500, 1, 0), 450.0f, 1600.0f),
			500.0f, 0.01f));
	TestTrue(TEXT("a tie past the window is out of reach by default"),
		FMath::IsNearlyEqual(Spur.FindNearestDistance(FVector(75, 0, 0), 70.0f, 110.0f),
			75.0f, 0.01f));
	TestTrue(TEXT("a tie past the window is taken within the tie reach"),
		FMath::IsNearlyEqual(Spur.FindNearestDistance(FVector(75, 0, 0), 70.0f, 110.0f,
			150.0f), 125.0f, 0.01f));
	TestTrue(TEXT("a closer pass beyond the tie reach stays out of reach"),
		FMath::IsNearlyEqual(Route.FindNearestDistance(FVector(100, 6, 0), 90.0f, 150.0f,
			300.0f), 100.0f, 0.01f));
	// Arena_04's spur: the outbound pass climbs to the tip, the return runs
	// level 8 uu aside; the walker stands on the return line, 35 uu below.
	const FArcLengthPath Rising = Polyline({FVector(0, 0, 0), FVector(100, 0, 26),
		FVector(0, 8, 26)});
	const FVector Below(80, 7.5, -12);
	TestTrue(TEXT("in 3D the height gap keeps the outbound pass nearer"),
		Rising.FindNearestDistance(Below, 70.0f, 110.0f, 160.0f) < 100.0f);
	TestTrue(TEXT("over the floor the walker is on the return pass"),
		Rising.FindNearestDistance(Below, 70.0f, 110.0f, 160.0f, true) > 110.0f);

	// Arena_12's stair: 40 uu of rise over 5 uu, then 29 uu per 18 uu step.
	const FArcLengthPath Stair = Polyline({FVector(0, 0, 0), FVector(5, 0, 40),
		FVector(23, 0, 69), FVector(41, 0, 98), FVector(59, 0, 127)});
	TestTrue(TEXT("a 40 uu lead along the curve stays over the first riser"),
		Stair.Sample(40.0f).X < 6.0f);
	TestTrue(TEXT("a 40 uu lead over the floor runs 40 uu ahead"),
		FMath::IsNearlyEqual(Stair.Sample(Stair.AdvanceHorizontal(0.0f, 40.0f)).X,
			40.0f, 0.01f));
	TestTrue(TEXT("a floor lead starts from the given distance"),
		FMath::IsNearlyEqual(Stair.Sample(Stair.AdvanceHorizontal(
			Stair.Distances[2], 9.0f)).X, 32.0f, 0.01f));
	TestEqual(TEXT("a floor lead past the end stops at the end"),
		Stair.AdvanceHorizontal(0.0f, 1000.0f), Stair.Length());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerCriticalSpringSpec,
	"WoT.Testing.Trailer.CriticalSpring", TrailerFlags)
bool FWoTTrailerCriticalSpringSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	FScalarSpring Fixed;
	float FirstSettled = -1.0f;
	for (int32 Step = 1; Step <= 120; ++Step)
	{
		StepCriticalSpring(Fixed, 100.0f, 1.0f, 1.0f / 60.0f);
		TestTrue(TEXT("critical spring never overshoots"),
			Fixed.Value >= 0.0f && Fixed.Value <= 100.0f + KINDA_SMALL_NUMBER);
		if (FirstSettled < 0.0f && FMath::Abs(Fixed.Value - 100.0f) <= 5.0f)
		{
			FirstSettled = Step / 60.0f;
		}
	}
	TestTrue(TEXT("spring settles within ten percent of its stated time"),
		FirstSettled >= 0.9f && FirstSettled <= 1.1f);

	FScalarSpring Variable;
	const float Steps[] = {0.017f, 0.041f, 0.009f, 0.083f, 0.15f, 0.2f, 0.5f};
	for (float DeltaSeconds : Steps)
	{
		StepCriticalSpring(Variable, 100.0f, 1.0f, DeltaSeconds);
	}
	TestTrue(TEXT("exact spring integration is stable under variable dt"),
		FMath::IsNearlyEqual(Variable.Value,
			100.0f * (1.0f - FMath::Exp(-4.744f) * (1.0f + 4.744f)), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerYawWrapSpringSpec,
	"WoT.Testing.Trailer.YawWrapSpring", TrailerFlags)
bool FWoTTrailerYawWrapSpringSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	FScalarSpring Yaw;
	Yaw.Value = 179.0f;
	StepCriticalAngleSpring(Yaw, -179.0f, 0.5f, 1.0f / 60.0f);
	TestTrue(TEXT("yaw begins along the positive two-degree short arc"),
		FMath::FindDeltaAngleDegrees(179.0f, Yaw.Value) > 0.0f);
	for (int32 Step = 0; Step < 120; ++Step)
	{
		StepCriticalAngleSpring(Yaw, -179.0f, 0.5f, 1.0f / 60.0f);
	}
	TestTrue(TEXT("yaw spring settles across the seam"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw.Value, -179.0f)) < 0.05f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerFollowPullInSpec,
	"WoT.Testing.Trailer.FollowPullIn", TrailerFlags)
bool FWoTTrailerFollowPullInSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	TestEqual(TEXT("hit pulls camera to clearance before wall"),
		PullInCameraDistance(400.0f, 250.0f, 10.0f), 240.0f);
	TestEqual(TEXT("pull-in cannot exceed desired arm"),
		PullInCameraDistance(400.0f, 900.0f, 10.0f), 400.0f);
	TestEqual(TEXT("near hit cannot produce a negative arm"),
		PullInCameraDistance(400.0f, 5.0f, 10.0f), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerCameraVelocityContinuitySpec,
	"WoT.Testing.Trailer.CameraVelocityContinuity", TrailerFlags)
bool FWoTTrailerCameraVelocityContinuitySpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	TArray<FCameraKey> Keys = {
		{0.0, FVector(0, 0, 0), FRotator::ZeroRotator, 80.0f},
		{1.0, FVector(100, 50, 0), FRotator(0, 30, 0), 90.0f},
		{2.0, FVector(220, 0, 20), FRotator(0, 60, 0), 100.0f},
	};
	constexpr double Probe = 0.0005;
	const FVector AtKey = InterpolateStyledCamera(Keys, 1.0, EEase::Linear).Location;
	const FVector Before = InterpolateStyledCamera(Keys, 1.0 - Probe,
		EEase::Linear).Location;
	const FVector After = InterpolateStyledCamera(Keys, 1.0 + Probe,
		EEase::Linear).Location;
	const FVector IncomingVelocity = (AtKey - Before) / Probe;
	const FVector OutgoingVelocity = (After - AtKey) / Probe;
	TestTrue(TEXT("interior camera velocity is continuous"),
		IncomingVelocity.Equals(OutgoingVelocity, 0.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerCameraInterpolationSpec,
	"WoT.Testing.Trailer.CameraInterpolation", TrailerFlags)
bool FWoTTrailerCameraInterpolationSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	TArray<FCameraKey> Keys = {
		{0.0, FVector(0, 0, 0), FRotator(0, 170, 0), 80.0f},
		{1.0, FVector(100, 50, 20), FRotator(10, -170, 0), 100.0f},
	};
	const FCameraPose Start = InterpolateCamera(Keys, 0.0, EEase::Linear);
	const FCameraPose End = InterpolateCamera(Keys, 1.0, EEase::Linear);
	const FCameraPose Mid = InterpolateCamera(Keys, 0.5, EEase::Linear);
	TestTrue(TEXT("Catmull-Rom clamps exactly to first endpoint"),
		Start.Location.Equals(Keys[0].Location, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("Catmull-Rom clamps exactly to last endpoint"),
		End.Location.Equals(Keys[1].Location, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("yaw crosses the -180/180 seam by the short path"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(180.0f, Mid.Rotation.Yaw)) < 0.01f);
	TestEqual(TEXT("FOV interpolates"), Mid.FOV, 90.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerMoveToSpec,
	"WoT.Testing.Trailer.MoveTo", TrailerFlags)
bool FWoTTrailerMoveToSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	const TArray<FVector> Path = {FVector(0, 0, 0), FVector(100, 0, 0)};
	int32 Index = 0;
	TestFalse(TEXT("first waypoint advances but route is not done"),
		AdvanceWaypoints(FVector(5, 0, 0), Path, 10.0f, Index));
	TestEqual(TEXT("next waypoint index"), Index, 1);
	TestFalse(TEXT("outside final tolerance is not done"),
		AdvanceWaypoints(FVector(80, 0, 0), Path, 10.0f, Index));
	TestTrue(TEXT("inside final tolerance completes"),
		AdvanceWaypoints(FVector(95, 0, 0), Path, 10.0f, Index));
	TestEqual(TEXT("done index is one past path"), Index, Path.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerTimelineSpec,
	"WoT.Testing.Trailer.Timeline", TrailerFlags)
bool FWoTTrailerTimelineSpec::RunTest(const FString&)
{
	using namespace WoTTrailer;
	TArray<TSharedPtr<FJsonValue>> Json = {
		TimelineEntry(2.0, TEXT("late")),
		TimelineEntry(1.0, TEXT("first-tie")),
		TimelineEntry(1.0, TEXT("second-tie")),
	};
	TArray<FTimelineEntry> Parsed;
	TArray<FString> Errors;
	TestTrue(TEXT("valid entries parse"), ParseTimeline(Json, Parsed, Errors));
	TestEqual(TEXT("three entries parsed"), Parsed.Num(), 3);
	TestEqual(TEXT("earlier time sorts first"), Parsed[0].Command, FString(TEXT("first-tie")));
	TestEqual(TEXT("equal timestamps retain file order"), Parsed[1].Command,
		FString(TEXT("second-tie")));
	TestEqual(TEXT("later time sorts last"), Parsed[2].Command, FString(TEXT("late")));
	TestEqual(TEXT("original index retained"), Parsed[1].SourceIndex, 2);

	TSharedRef<FJsonObject> MissingCommand = MakeShared<FJsonObject>();
	MissingCommand->SetNumberField(TEXT("t"), 3.0);
	Json.Add(MakeShared<FJsonValueObject>(MissingCommand));
	TestFalse(TEXT("malformed entry rejects the timeline"), ParseTimeline(Json, Parsed, Errors));
	TestTrue(TEXT("malformed entry reports its source index"),
		Errors.Num() == 1 && Errors[0].Contains(TEXT("3")));
	return true;
}

// The rig's `listener` pin moves the retail hearing gate with the UE listener:
// a trailer shot films from a balcony ~2480 uu from a Seeker impact (HitSK,
// radius 2500, gate 2500/sqrt(1.3) ~ 2193), and the gate measured from the camera
// culled the impact the pinned listener sat 300 uu from.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrailerListenerPinsHearingGateSpec,
	"WoT.Testing.Trailer.ListenerPinsHearingGate", TrailerFlags)
bool FWoTTrailerListenerPinsHearingGateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* Controller = World->SpawnActor<APlayerController>(
		APlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, SpawnParams);
	APawn* Viewer = World->SpawnActor<APawn>(
		APawn::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, SpawnParams);
	USoundBase* Wave = WoTSound::LoadBySubPath(FName(TEXT("FootStubA/STON")));
	if (!TestNotNull(TEXT("listener-pin controller"), Controller)
		|| !TestNotNull(TEXT("listener-pin viewer pawn"), Viewer)
		|| !TestNotNull(TEXT("listener-pin fixture wave loads"), Wave))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Viewer);

	const float Radius = 2500.0f;
	const FVector Impact(2480.0f, 0.0f, 0.0f);
	TestEqual(TEXT("unpinned: the gate measures from the view target and culls"),
		WoTSound::PrepareEffectForTesting(Controller, Wave, Impact, Radius),
		WoTSound::EEffectAdmissionResult::OutsideHearingRadius);

	WoTSound::SetHearingOriginOverride(*World, FVector(2180.0f, 0.0f, 0.0f));
	TestEqual(TEXT("pinned 300 uu from the impact: admitted"),
		WoTSound::PrepareEffectForTesting(Controller, Wave, Impact, Radius),
		WoTSound::EEffectAdmissionResult::Admitted);
	TestEqual(TEXT("pinned: a sound beside the camera but far from the pin is culled"),
		WoTSound::PrepareEffectForTesting(Controller, Wave,
			FVector(-200.0f, 0.0f, 0.0f), Radius),
		WoTSound::EEffectAdmissionResult::OutsideHearingRadius);

	WoTSound::ClearHearingOriginOverride(*World);
	TestEqual(TEXT("cleared: the retail view-target gate returns"),
		WoTSound::PrepareEffectForTesting(Controller, Wave, Impact, Radius),
		WoTSound::EEffectAdmissionResult::OutsideHearingRadius);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
